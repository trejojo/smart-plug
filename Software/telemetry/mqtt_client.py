"""
SmartPlug MQTT Client
=====================

PC-side MQTT layer for the SmartPlug GUI/console.

This version is aligned with the current ESP32 firmware implementation in
`module_mqtt.c`:

ESP32 -> PC topics:
    smartplug/status       Combined live telemetry package
    smartplug/events       Critical protection events
    smartplug/relay        Relay state updates
    smartplug/led          RGB LED state updates
    smartplug/temperature  Temperature-only updates
    smartplug/energy       Energy-only updates
    aice/status            Safety-limit update acknowledgments

PC -> ESP32 topics:
    smartplug/cmd          Plain-text relay commands: RELAY_ON / RELAY_OFF
    aice/cmd               JSON safety limits: {"max_vrms": ..., "max_iarms": ...}

The module can be used in two ways:
    1. Imported by a future GUI as a reusable client class.
    2. Run directly as a console tool for testing.

Important unit convention:
    - vrms / max_vrms are V RMS.
    - irms / max_iarms / current_a_arms / current_b_arms are A RMS.
"""

from __future__ import annotations

import argparse
import json
import queue
import sys
import time
from dataclasses import dataclass, field, asdict
from datetime import datetime
from typing import Any, Callable, Dict, Optional

try:
    import paho.mqtt.client as mqtt
except ModuleNotFoundError:  # Allows parser/data models to be imported without paho installed.
    mqtt = None



# -----------------------------------------------------------------------------
# MQTT topic contract currently implemented by module_mqtt.c
# -----------------------------------------------------------------------------

SMARTPLUG_STATUS_TOPIC = "smartplug/status"
SMARTPLUG_EVENTS_TOPIC = "smartplug/events"
SMARTPLUG_RELAY_TOPIC = "smartplug/relay"
SMARTPLUG_LED_TOPIC = "smartplug/led"
SMARTPLUG_TEMPERATURE_TOPIC = "smartplug/temperature"
SMARTPLUG_ENERGY_TOPIC = "smartplug/energy"
SMARTPLUG_CMD_TOPIC = "smartplug/cmd"

AICE_CMD_TOPIC = "aice/cmd"
AICE_STATUS_TOPIC = "aice/status"

DEFAULT_SUBSCRIPTIONS = (
    "smartplug/#",
    "aice/#",
)

DEFAULT_BROKER = "192.168.137.1"
DEFAULT_PORT = 1883
DEFAULT_KEEPALIVE = 60


# -----------------------------------------------------------------------------
# PC-side normalized data models for GUI usage
# -----------------------------------------------------------------------------


def now_iso_ms() -> str:
    """Return a local PC timestamp with millisecond resolution."""
    return datetime.now().isoformat(timespec="milliseconds")


@dataclass
class TelemetrySample:
    """Normalized current-firmware smartplug/status payload."""

    timestamp_received_pc: str
    vrms: Optional[float] = None
    irms: Optional[float] = None
    pf: Optional[float] = None
    active_power: Optional[float] = None
    reactive_power: Optional[float] = None
    frequency: Optional[float] = None
    no_load: Optional[bool] = None
    energy_wh: Optional[int] = None
    relay: Optional[bool] = None
    tmp_c: Optional[float] = None
    raw: Dict[str, Any] = field(default_factory=dict)

    @staticmethod
    def from_payload(payload: Dict[str, Any]) -> "TelemetrySample":
        return TelemetrySample(
            timestamp_received_pc=now_iso_ms(),
            vrms=_as_optional_float(payload.get("vrms")),
            irms=_as_optional_float(payload.get("irms")),
            pf=_as_optional_float(payload.get("pf")),
            active_power=_as_optional_float(payload.get("active_power")),
            reactive_power=_as_optional_float(payload.get("reactive_power")),
            frequency=_as_optional_float(payload.get("frequency")),
            no_load=_as_optional_bool(payload.get("no_load")),
            energy_wh=_as_optional_int(payload.get("energy_wh")),
            relay=_as_optional_bool(payload.get("relay")),
            tmp_c=_as_optional_float(payload.get("tmp_c")),
            raw=payload,
        )


@dataclass
class CriticalEvent:
    """Normalized current-firmware smartplug/events payload."""

    timestamp_received_pc: str
    event_type: Optional[str] = None
    cause: Optional[str] = None
    timestamp_device_ms: Optional[int] = None
    voltage_vrms: Optional[float] = None
    current_a_arms: Optional[float] = None
    current_b_arms: Optional[float] = None
    duration_cycles: Optional[int] = None
    action_taken: Optional[str] = None
    system_status: Optional[str] = None
    raw: Dict[str, Any] = field(default_factory=dict)

    @staticmethod
    def from_payload(payload: Dict[str, Any]) -> "CriticalEvent":
        data = payload.get("data") if isinstance(payload.get("data"), dict) else {}
        return CriticalEvent(
            timestamp_received_pc=now_iso_ms(),
            event_type=_as_optional_str(payload.get("event_type")),
            cause=_as_optional_str(payload.get("cause")),
            timestamp_device_ms=_as_optional_int(payload.get("timestamp")),
            voltage_vrms=_as_optional_float(data.get("voltage_vrms")),
            current_a_arms=_as_optional_float(data.get("current_a_arms")),
            current_b_arms=_as_optional_float(data.get("current_b_arms")),
            duration_cycles=_as_optional_int(data.get("duration_cycles")),
            action_taken=_as_optional_str(payload.get("action_taken")),
            system_status=_as_optional_str(payload.get("system_status")),
            raw=payload,
        )


@dataclass
class SafetyLimitAck:
    """Normalized current-firmware aice/status payload."""

    timestamp_received_pc: str
    event_type: Optional[str] = None
    accepted: Optional[bool] = None
    max_vrms: Optional[float] = None
    max_iarms: Optional[float] = None
    reason: Optional[str] = None
    raw: Dict[str, Any] = field(default_factory=dict)

    @staticmethod
    def from_payload(payload: Dict[str, Any]) -> "SafetyLimitAck":
        return SafetyLimitAck(
            timestamp_received_pc=now_iso_ms(),
            event_type=_as_optional_str(payload.get("event_type")),
            accepted=_as_optional_bool(payload.get("accepted")),
            max_vrms=_as_optional_float(payload.get("max_vrms")),
            max_iarms=_as_optional_float(payload.get("max_iarms")),
            reason=_as_optional_str(payload.get("reason")),
            raw=payload,
        )


@dataclass
class RelayState:
    timestamp_received_pc: str
    event_type: Optional[str] = None
    relay: Optional[bool] = None
    raw: Dict[str, Any] = field(default_factory=dict)

    @staticmethod
    def from_payload(payload: Dict[str, Any]) -> "RelayState":
        return RelayState(
            timestamp_received_pc=now_iso_ms(),
            event_type=_as_optional_str(payload.get("event_type")),
            relay=_as_optional_bool(payload.get("relay")),
            raw=payload,
        )


@dataclass
class LedState:
    timestamp_received_pc: str
    r: Optional[int] = None
    g: Optional[int] = None
    b: Optional[int] = None
    raw: Dict[str, Any] = field(default_factory=dict)

    @staticmethod
    def from_payload(payload: Dict[str, Any]) -> "LedState":
        return LedState(
            timestamp_received_pc=now_iso_ms(),
            r=_as_optional_int(payload.get("r")),
            g=_as_optional_int(payload.get("g")),
            b=_as_optional_int(payload.get("b")),
            raw=payload,
        )


@dataclass
class TemperatureSample:
    timestamp_received_pc: str
    event_type: Optional[str] = None
    temperature_c: Optional[float] = None
    raw: Dict[str, Any] = field(default_factory=dict)

    @staticmethod
    def from_payload(payload: Dict[str, Any]) -> "TemperatureSample":
        return TemperatureSample(
            timestamp_received_pc=now_iso_ms(),
            event_type=_as_optional_str(payload.get("event_type")),
            temperature_c=_as_optional_float(payload.get("temperature_c")),
            raw=payload,
        )


@dataclass
class EnergySample:
    """Current-firmware smartplug/energy payload.

    Note: current firmware uses generic keys: voltage, current, power, energy.
    The combined smartplug/status package is preferred for the GUI because it
    uses clearer names such as vrms, irms and active_power.
    """

    timestamp_received_pc: str
    voltage: Optional[float] = None
    current: Optional[float] = None
    power: Optional[float] = None
    energy: Optional[int] = None
    raw: Dict[str, Any] = field(default_factory=dict)

    @staticmethod
    def from_payload(payload: Dict[str, Any]) -> "EnergySample":
        return EnergySample(
            timestamp_received_pc=now_iso_ms(),
            voltage=_as_optional_float(payload.get("voltage")),
            current=_as_optional_float(payload.get("current")),
            power=_as_optional_float(payload.get("power")),
            energy=_as_optional_int(payload.get("energy")),
            raw=payload,
        )


@dataclass
class CommandEcho:
    """Observed command on a subscribed command topic.

    MQTT clients subscribed to smartplug/# or aice/# may receive their own
    published commands back from the broker. The GUI may ignore or log these.
    """

    timestamp_received_pc: str
    topic: str
    command_text: Optional[str] = None
    command_json: Optional[Dict[str, Any]] = None
    raw_text: str = ""


@dataclass
class ParsedMqttMessage:
    topic: str
    kind: str
    timestamp_received_pc: str
    payload_text: str
    json_payload: Optional[Dict[str, Any]] = None
    data: Any = None
    error: Optional[str] = None

    def to_dict(self) -> Dict[str, Any]:
        output = asdict(self)
        if hasattr(self.data, "__dataclass_fields__"):
            output["data"] = asdict(self.data)
        return output


# -----------------------------------------------------------------------------
# Conversion helpers
# -----------------------------------------------------------------------------


def _as_optional_float(value: Any) -> Optional[float]:
    if value is None or isinstance(value, bool):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None



def _as_optional_int(value: Any) -> Optional[int]:
    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None



def _as_optional_bool(value: Any) -> Optional[bool]:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)) and value in (0, 1):
        return bool(value)
    if isinstance(value, str):
        normalized = value.strip().lower()
        if normalized in ("true", "1", "on", "yes"):
            return True
        if normalized in ("false", "0", "off", "no"):
            return False
    return None



def _as_optional_str(value: Any) -> Optional[str]:
    if value is None:
        return None
    return str(value)


# -----------------------------------------------------------------------------
# Parser layer
# -----------------------------------------------------------------------------


def parse_mqtt_message(topic: str, payload_bytes: bytes) -> ParsedMqttMessage:
    """Parse one MQTT message into a GUI-friendly normalized object.

    This function intentionally routes by topic first because the current
    firmware does not include event_type in every payload, especially
    smartplug/status, smartplug/led and smartplug/energy.
    """
    timestamp_pc = now_iso_ms()

    try:
        payload_text = payload_bytes.decode("utf-8")
    except UnicodeDecodeError as exc:
        return ParsedMqttMessage(
            topic=topic,
            kind="binary_payload",
            timestamp_received_pc=timestamp_pc,
            payload_text="",
            error=f"Payload is not valid UTF-8: {exc}",
        )

    json_payload: Optional[Dict[str, Any]] = None
    parsed_json_error: Optional[str] = None
    try:
        candidate = json.loads(payload_text)
        if isinstance(candidate, dict):
            json_payload = candidate
        else:
            parsed_json_error = "JSON payload is valid but is not an object"
    except json.JSONDecodeError as exc:
        parsed_json_error = str(exc)

    # Current firmware expects/uses plain text on smartplug/cmd.
    if topic == SMARTPLUG_CMD_TOPIC:
        return ParsedMqttMessage(
            topic=topic,
            kind="command_echo",
            timestamp_received_pc=timestamp_pc,
            payload_text=payload_text,
            json_payload=json_payload,
            data=CommandEcho(
                timestamp_received_pc=timestamp_pc,
                topic=topic,
                command_text=payload_text.strip() if json_payload is None else None,
                command_json=json_payload,
                raw_text=payload_text,
            ),
        )

    if topic == AICE_CMD_TOPIC:
        return ParsedMqttMessage(
            topic=topic,
            kind="safety_command_echo",
            timestamp_received_pc=timestamp_pc,
            payload_text=payload_text,
            json_payload=json_payload,
            data=CommandEcho(
                timestamp_received_pc=timestamp_pc,
                topic=topic,
                command_text=None if json_payload else payload_text.strip(),
                command_json=json_payload,
                raw_text=payload_text,
            ),
            error=parsed_json_error if json_payload is None else None,
        )

    # All ESP32-to-PC topics below are expected to be JSON objects.
    if json_payload is None:
        return ParsedMqttMessage(
            topic=topic,
            kind="invalid_json",
            timestamp_received_pc=timestamp_pc,
            payload_text=payload_text,
            error=parsed_json_error or "Payload is not a JSON object",
        )

    if topic == SMARTPLUG_STATUS_TOPIC:
        return ParsedMqttMessage(
            topic=topic,
            kind="status",
            timestamp_received_pc=timestamp_pc,
            payload_text=payload_text,
            json_payload=json_payload,
            data=TelemetrySample.from_payload(json_payload),
        )

    if topic == SMARTPLUG_EVENTS_TOPIC:
        event_type = json_payload.get("event_type")
        kind = "critical_event" if event_type == "CRITICAL_PROTECTION" else "event"
        return ParsedMqttMessage(
            topic=topic,
            kind=kind,
            timestamp_received_pc=timestamp_pc,
            payload_text=payload_text,
            json_payload=json_payload,
            data=CriticalEvent.from_payload(json_payload),
        )

    if topic == AICE_STATUS_TOPIC:
        return ParsedMqttMessage(
            topic=topic,
            kind="safety_ack",
            timestamp_received_pc=timestamp_pc,
            payload_text=payload_text,
            json_payload=json_payload,
            data=SafetyLimitAck.from_payload(json_payload),
        )

    if topic == SMARTPLUG_RELAY_TOPIC:
        return ParsedMqttMessage(
            topic=topic,
            kind="relay_state",
            timestamp_received_pc=timestamp_pc,
            payload_text=payload_text,
            json_payload=json_payload,
            data=RelayState.from_payload(json_payload),
        )

    if topic == SMARTPLUG_LED_TOPIC:
        return ParsedMqttMessage(
            topic=topic,
            kind="led_state",
            timestamp_received_pc=timestamp_pc,
            payload_text=payload_text,
            json_payload=json_payload,
            data=LedState.from_payload(json_payload),
        )

    if topic == SMARTPLUG_TEMPERATURE_TOPIC:
        return ParsedMqttMessage(
            topic=topic,
            kind="temperature",
            timestamp_received_pc=timestamp_pc,
            payload_text=payload_text,
            json_payload=json_payload,
            data=TemperatureSample.from_payload(json_payload),
        )

    if topic == SMARTPLUG_ENERGY_TOPIC:
        return ParsedMqttMessage(
            topic=topic,
            kind="energy",
            timestamp_received_pc=timestamp_pc,
            payload_text=payload_text,
            json_payload=json_payload,
            data=EnergySample.from_payload(json_payload),
        )

    return ParsedMqttMessage(
        topic=topic,
        kind="unknown_json",
        timestamp_received_pc=timestamp_pc,
        payload_text=payload_text,
        json_payload=json_payload,
        data=json_payload,
    )


# -----------------------------------------------------------------------------
# Reusable MQTT client for GUI or console
# -----------------------------------------------------------------------------


def _require_paho_mqtt() -> None:
    if mqtt is None:
        raise RuntimeError(
            "paho-mqtt is required to use SmartPlugMqttClient. "
            "Install it with: pip install paho-mqtt"
        )


class SmartPlugMqttClient:
    """Reusable MQTT client layer for the SmartPlug GUI.

    For a GUI, avoid updating GUI widgets directly from MQTT callbacks because
    they run in the MQTT network thread. Instead, poll `message_queue` from the
    GUI main thread, or attach callbacks that forward events safely to the GUI.
    """

    def __init__(
        self,
        broker: str = DEFAULT_BROKER,
        port: int = DEFAULT_PORT,
        keepalive: int = DEFAULT_KEEPALIVE,
        client_id: str = "SmartPlug_PC_Client",
        subscriptions: tuple[str, ...] = DEFAULT_SUBSCRIPTIONS,
        console_print: bool = False,
    ) -> None:
        self.broker = broker
        self.port = port
        self.keepalive = keepalive
        self.client_id = client_id
        self.subscriptions = subscriptions
        self.console_print = console_print
        self.is_connected = False
        self.message_queue: "queue.Queue[ParsedMqttMessage]" = queue.Queue()

        self.on_any_message: Optional[Callable[[ParsedMqttMessage], None]] = None
        self.on_status: Optional[Callable[[TelemetrySample], None]] = None
        self.on_critical_event: Optional[Callable[[CriticalEvent], None]] = None
        self.on_safety_ack: Optional[Callable[[SafetyLimitAck], None]] = None
        self.on_relay_state: Optional[Callable[[RelayState], None]] = None

        _require_paho_mqtt()
        self.client = self._create_mqtt_client()
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message

    def _create_mqtt_client(self) -> mqtt.Client:
        """Create a Paho client compatible with paho-mqtt 1.x and 2.x."""
        try:
            # Paho MQTT 2.x supports this enum. VERSION1 preserves the older
            # callback signatures used below and avoids breaking existing code.
            return mqtt.Client(
                callback_api_version=mqtt.CallbackAPIVersion.VERSION1,
                client_id=self.client_id,
                clean_session=True,
            )
        except AttributeError:
            # Paho MQTT 1.x
            return mqtt.Client(client_id=self.client_id, clean_session=True)
        except TypeError:
            # Some 1.x/2.x installations differ slightly in constructor support.
            return mqtt.Client(client_id=self.client_id, clean_session=True)

    def _on_connect(self, client: mqtt.Client, userdata: Any, flags: Any, rc: int) -> None:
        self.is_connected = rc == 0
        timestamp = now_iso_ms()

        if rc != 0:
            print(f"[{timestamp}] MQTT connection failed with code {rc}")
            return

        if self.console_print:
            print(f"\n[{timestamp}] Connected to MQTT broker at {self.broker}:{self.port}")
            print(f"[{timestamp}] Subscribing to topics...")

        for topic in self.subscriptions:
            result, mid = client.subscribe(topic)
            if self.console_print:
                if result == mqtt.MQTT_ERR_SUCCESS:
                    print(f"[{timestamp}] OK subscribed to: {topic}")
                else:
                    print(f"[{timestamp}] ERROR subscribing to {topic}: result={result}, mid={mid}")

    def _on_disconnect(self, client: mqtt.Client, userdata: Any, rc: int) -> None:
        self.is_connected = False
        if self.console_print:
            timestamp = now_iso_ms()
            if rc == 0:
                print(f"[{timestamp}] Disconnected from MQTT broker cleanly")
            else:
                print(f"[{timestamp}] Unexpected MQTT disconnection. rc={rc}")

    def _on_message(self, client: mqtt.Client, userdata: Any, msg: mqtt.MQTTMessage) -> None:
        parsed = parse_mqtt_message(msg.topic, msg.payload)
        self.message_queue.put(parsed)
        self._dispatch_callbacks(parsed)

        if self.console_print:
            print(format_console_message(parsed))
            print(" > ", end="", flush=True)

    def _dispatch_callbacks(self, parsed: ParsedMqttMessage) -> None:
        if self.on_any_message is not None:
            self.on_any_message(parsed)

        if parsed.kind == "status" and self.on_status is not None:
            self.on_status(parsed.data)
        elif parsed.kind == "critical_event" and self.on_critical_event is not None:
            self.on_critical_event(parsed.data)
        elif parsed.kind == "safety_ack" and self.on_safety_ack is not None:
            self.on_safety_ack(parsed.data)
        elif parsed.kind == "relay_state" and self.on_relay_state is not None:
            self.on_relay_state(parsed.data)

    def connect(self) -> None:
        self.client.connect(self.broker, self.port, self.keepalive)

    def start(self) -> None:
        """Connect and start the MQTT network loop in a background thread."""
        self.connect()
        self.client.loop_start()

    def stop(self) -> None:
        """Disconnect and stop the MQTT network loop."""
        try:
            if self.is_connected:
                self.client.disconnect()
        finally:
            self.client.loop_stop()

    def publish_relay(self, relay_on: bool, qos: int = 1) -> mqtt.MQTTMessageInfo:
        """Publish the relay command expected by current firmware.

        Current module_mqtt.c expects plain text on smartplug/cmd, not JSON.
        """
        payload = "RELAY_ON" if relay_on else "RELAY_OFF"
        return self.client.publish(SMARTPLUG_CMD_TOPIC, payload, qos=qos)

    def publish_safety_limits(
        self,
        max_vrms: float,
        max_iarms: float,
        qos: int = 1,
    ) -> mqtt.MQTTMessageInfo:
        """Publish voltage/current RMS safety limits expected by current firmware."""
        payload = json.dumps(
            {
                "max_vrms": float(max_vrms),
                "max_iarms": float(max_iarms),
            },
            separators=(",", ":"),
        )
        return self.client.publish(AICE_CMD_TOPIC, payload, qos=qos)

    def publish_raw(self, topic: str, payload: str, qos: int = 1) -> mqtt.MQTTMessageInfo:
        """Development helper for sending arbitrary messages."""
        return self.client.publish(topic, payload, qos=qos)


# -----------------------------------------------------------------------------
# Console formatting and CLI
# -----------------------------------------------------------------------------


def _fmt(value: Any, suffix: str = "", ndigits: int = 2) -> str:
    if value is None:
        return "--"
    if isinstance(value, float):
        return f"{value:.{ndigits}f}{suffix}"
    return f"{value}{suffix}"



def _on_off(value: Optional[bool]) -> str:
    if value is True:
        return "ON"
    if value is False:
        return "OFF"
    return "--"



def format_console_message(parsed: ParsedMqttMessage) -> str:
    ts = parsed.timestamp_received_pc

    if parsed.kind == "status" and isinstance(parsed.data, TelemetrySample):
        d = parsed.data
        no_load_text = "YES" if d.no_load else "NO" if d.no_load is False else "--"
        return (
            f"\n[{ts}] STATUS {parsed.topic} | "
            f"V={_fmt(d.vrms, ' Vrms')} | "
            f"I={_fmt(d.irms, ' Arms', 3)} | "
            f"P={_fmt(d.active_power, ' W')} | "
            f"Q={_fmt(d.reactive_power, ' var')} | "
            f"PF={_fmt(d.pf, '', 3)} | "
            f"f={_fmt(d.frequency, ' Hz')} | "
            f"E={_fmt(d.energy_wh, ' Wh')} | "
            f"relay={_on_off(d.relay)} | "
            f"no_load={no_load_text} | "
            f"T={_fmt(d.tmp_c, ' °C')}"
        )

    if parsed.kind == "critical_event" and isinstance(parsed.data, CriticalEvent):
        d = parsed.data
        return (
            f"\n[{ts}] CRITICAL EVENT {parsed.topic} | "
            f"cause={d.cause or '--'} | "
            f"V={_fmt(d.voltage_vrms, ' Vrms')} | "
            f"IA={_fmt(d.current_a_arms, ' Arms', 3)} | "
            f"IB={_fmt(d.current_b_arms, ' Arms', 3)} | "
            f"cycles={_fmt(d.duration_cycles)} | "
            f"action={d.action_taken or '--'} | "
            f"status={d.system_status or '--'}"
        )

    if parsed.kind == "safety_ack" and isinstance(parsed.data, SafetyLimitAck):
        d = parsed.data
        result = "ACCEPTED" if d.accepted else "REJECTED" if d.accepted is False else "UNKNOWN"
        return (
            f"\n[{ts}] SAFETY ACK {parsed.topic} | "
            f"{result} | "
            f"max_vrms={_fmt(d.max_vrms, ' Vrms')} | "
            f"max_iarms={_fmt(d.max_iarms, ' Arms', 3)} | "
            f"reason={d.reason or '--'}"
        )

    if parsed.kind == "relay_state" and isinstance(parsed.data, RelayState):
        return f"\n[{ts}] RELAY {parsed.topic} | relay={_on_off(parsed.data.relay)}"

    if parsed.kind == "led_state" and isinstance(parsed.data, LedState):
        d = parsed.data
        return f"\n[{ts}] LED {parsed.topic} | r={d.r} g={d.g} b={d.b}"

    if parsed.kind == "temperature" and isinstance(parsed.data, TemperatureSample):
        d = parsed.data
        return f"\n[{ts}] TEMPERATURE {parsed.topic} | T={_fmt(d.temperature_c, ' °C')}"

    if parsed.kind == "energy" and isinstance(parsed.data, EnergySample):
        d = parsed.data
        return (
            f"\n[{ts}] ENERGY {parsed.topic} | "
            f"voltage={_fmt(d.voltage)} | current={_fmt(d.current)} | "
            f"power={_fmt(d.power)} | energy={_fmt(d.energy)}"
        )

    if parsed.kind in ("command_echo", "safety_command_echo"):
        return f"\n[{ts}] COMMAND ECHO {parsed.topic} | {parsed.payload_text}"

    if parsed.error:
        return f"\n[{ts}] {parsed.kind.upper()} {parsed.topic} | error={parsed.error} | payload={parsed.payload_text}"

    if parsed.json_payload is not None:
        return f"\n[{ts}] {parsed.kind.upper()} {parsed.topic}: {json.dumps(parsed.json_payload, ensure_ascii=False)}"

    return f"\n[{ts}] {parsed.kind.upper()} {parsed.topic}: {parsed.payload_text}"



def print_help() -> None:
    print(
        """
Commands:
  relay on                 Publish RELAY_ON to smartplug/cmd
  relay off                Publish RELAY_OFF to smartplug/cmd
  on                       Alias for: relay on
  off                      Alias for: relay off
  set <vrms> <iarms>       Publish RMS safety limits to aice/cmd
                           Example: set 135.0 5.0
  raw <topic> <payload>    Development helper to publish any payload
  help                     Show this help
  quit                     Exit

Current firmware notes:
  - Relay command payload is plain text: RELAY_ON / RELAY_OFF.
  - Safety-limit command payload is JSON: {"max_vrms":135.0,"max_iarms":5.0}.
  - Main dashboard topic is smartplug/status.
""".strip()
    )



def run_console(args: argparse.Namespace) -> int:
    print("=" * 78)
    print("SmartPlug MQTT Telemetry & Command Console")
    print("=" * 78)
    print(f"Broker: {args.broker}:{args.port}")
    print("Subscriptions: " + ", ".join(DEFAULT_SUBSCRIPTIONS))
    print_help()
    print("=" * 78)

    smartplug = SmartPlugMqttClient(
        broker=args.broker,
        port=args.port,
        keepalive=args.keepalive,
        client_id=args.client_id,
        console_print=True,
    )

    try:
        print(f"Connecting to MQTT broker at {args.broker}:{args.port}...")
        smartplug.start()
        time.sleep(0.5)

        while True:
            try:
                cmd = input(" > ").strip()
            except EOFError:
                break

            if not cmd:
                continue

            normalized = cmd.lower()
            parts = cmd.split()

            if normalized in ("quit", "exit"):
                break

            if normalized == "help":
                print_help()
                continue

            if normalized in ("on", "relay on"):
                smartplug.publish_relay(True)
                print(f"Published to {SMARTPLUG_CMD_TOPIC}: RELAY_ON")
                continue

            if normalized in ("off", "relay off"):
                smartplug.publish_relay(False)
                print(f"Published to {SMARTPLUG_CMD_TOPIC}: RELAY_OFF")
                continue

            if parts and parts[0].lower() == "set":
                if len(parts) != 3:
                    print("Error: use set <vrms> <iarms>, for example: set 135.0 5.0")
                    continue
                try:
                    max_vrms = float(parts[1])
                    max_iarms = float(parts[2])
                except ValueError:
                    print("Error: vrms and iarms must be valid numbers")
                    continue
                smartplug.publish_safety_limits(max_vrms, max_iarms)
                print(
                    f"Published to {AICE_CMD_TOPIC}: "
                    f"{{\"max_vrms\":{max_vrms},\"max_iarms\":{max_iarms}}}"
                )
                continue

            if parts and parts[0].lower() == "raw":
                if len(parts) < 3:
                    print("Error: use raw <topic> <payload>")
                    continue
                topic = parts[1]
                payload = cmd.split(maxsplit=2)[2]
                smartplug.publish_raw(topic, payload)
                print(f"Published raw message to {topic}: {payload}")
                continue

            print("Unknown command. Type 'help' for options.")

    except ConnectionRefusedError:
        print(f"Connection refused. Check that Mosquitto is running at {args.broker}:{args.port}.")
        return 2
    except KeyboardInterrupt:
        print("\nStopped by user")
        return 130
    finally:
        smartplug.stop()
        print("Exited safely.")

    return 0



def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="SmartPlug MQTT console/client")
    parser.add_argument("--broker", default=DEFAULT_BROKER, help="MQTT broker IP/hostname")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="MQTT broker port")
    parser.add_argument("--keepalive", type=int, default=DEFAULT_KEEPALIVE, help="MQTT keepalive seconds")
    parser.add_argument("--client-id", default="SmartPlug_PC_Console", help="MQTT client ID")
    return parser



def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()
    return run_console(args)


if __name__ == "__main__":
    sys.exit(main())
