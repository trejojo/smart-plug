"""
SmartPlug MQTT Client
=====================

Reusable PC-side MQTT layer for the AYCE Smart Plug GUI and console tools.

Standard topic contract
-----------------------

ESP32 -> PC:
    smartplug/telemetry/status        Main combined telemetry JSON
    smartplug/telemetry/temperature   Temperature-only JSON
    smartplug/telemetry/energy        Energy-only/basic electrical JSON
    smartplug/events/protection       Critical protection events JSON
    smartplug/state/relay             Relay state JSON
    smartplug/state/led               RGB LED state JSON
    smartplug/commands/ack            ACKs for relay/config/waveform commands JSON
    smartplug/waveform/data           512-sample waveform capture JSON

PC -> ESP32:
    smartplug/commands/relay          Relay command JSON
    smartplug/commands/config         Safety/config command JSON
    smartplug/waveform/request        512-sample waveform request JSON

The parser also accepts the older development topics used before the topic
standardization so the PC tools remain useful during firmware transition.
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
except ModuleNotFoundError:  # Allows models/parsers to be imported without paho installed.
    mqtt = None


# -----------------------------------------------------------------------------
# MQTT topic contract
# -----------------------------------------------------------------------------

SMARTPLUG_BASE = "smartplug"

TOPIC_TELEMETRY_STATUS = f"{SMARTPLUG_BASE}/telemetry/status"
TOPIC_TELEMETRY_TEMPERATURE = f"{SMARTPLUG_BASE}/telemetry/temperature"
TOPIC_TELEMETRY_ENERGY = f"{SMARTPLUG_BASE}/telemetry/energy"
TOPIC_EVENTS_PROTECTION = f"{SMARTPLUG_BASE}/events/protection"
TOPIC_STATE_RELAY = f"{SMARTPLUG_BASE}/state/relay"
TOPIC_STATE_LED = f"{SMARTPLUG_BASE}/state/led"
TOPIC_COMMAND_ACK = f"{SMARTPLUG_BASE}/commands/ack"
TOPIC_COMMAND_RELAY = f"{SMARTPLUG_BASE}/commands/relay"
TOPIC_COMMAND_CONFIG = f"{SMARTPLUG_BASE}/commands/config"
TOPIC_WAVEFORM_REQUEST = f"{SMARTPLUG_BASE}/waveform/request"
TOPIC_WAVEFORM_DATA = f"{SMARTPLUG_BASE}/waveform/data"

# Legacy development aliases kept for transition/testing only.
LEGACY_TOPIC_STATUS = "smartplug/status"
LEGACY_TOPIC_EVENTS = "smartplug/events"
LEGACY_TOPIC_RELAY = "smartplug/relay"
LEGACY_TOPIC_LED = "smartplug/led"
LEGACY_TOPIC_TEMPERATURE = "smartplug/temperature"
LEGACY_TOPIC_ENERGY = "smartplug/energy"
LEGACY_TOPIC_RELAY_CMD = "smartplug/cmd"
LEGACY_TOPIC_AYCE_CMD = "ayce/cmd"
LEGACY_TOPIC_AYCE_STATUS = "ayce/status"
LEGACY_TOPIC_WAVEFORM_CHUNK = "smartplug/waveform/chunk"
LEGACY_TOPIC_WAVEFORM_DATA = "smartplug/waveform"
LEGACY_TOPIC_WAVEFORM_CMD = "smartplug/waveform/cmd"

STATUS_TOPICS = {TOPIC_TELEMETRY_STATUS, LEGACY_TOPIC_STATUS}
EVENT_TOPICS = {TOPIC_EVENTS_PROTECTION, LEGACY_TOPIC_EVENTS}
RELAY_STATE_TOPICS = {TOPIC_STATE_RELAY, LEGACY_TOPIC_RELAY}
LED_STATE_TOPICS = {TOPIC_STATE_LED, LEGACY_TOPIC_LED}
TEMPERATURE_TOPICS = {TOPIC_TELEMETRY_TEMPERATURE, LEGACY_TOPIC_TEMPERATURE}
ENERGY_TOPICS = {TOPIC_TELEMETRY_ENERGY, LEGACY_TOPIC_ENERGY}
ACK_TOPICS = {TOPIC_COMMAND_ACK, LEGACY_TOPIC_AYCE_STATUS}
WAVEFORM_DATA_TOPICS = {TOPIC_WAVEFORM_DATA, LEGACY_TOPIC_WAVEFORM_CHUNK, LEGACY_TOPIC_WAVEFORM_DATA}
COMMAND_TOPICS = {TOPIC_COMMAND_RELAY, TOPIC_COMMAND_CONFIG, TOPIC_WAVEFORM_REQUEST,
                  LEGACY_TOPIC_RELAY_CMD, LEGACY_TOPIC_AYCE_CMD, LEGACY_TOPIC_WAVEFORM_CMD}

DEFAULT_SUBSCRIPTIONS = (
    "smartplug/#",
    "ayce/#",  # transition only; harmless once firmware stops using ayce/*
)

DEFAULT_BROKER = "192.168.137.1"
DEFAULT_PORT = 1883
DEFAULT_KEEPALIVE = 60
DEFAULT_WAVEFORM_SAMPLE_COUNT = 512
DEFAULT_WAVEFORM_SAMPLE_RATE_HZ = 2400


# Backward-compatible constant aliases used by older scripts.
SMARTPLUG_STATUS_TOPIC = TOPIC_TELEMETRY_STATUS
SMARTPLUG_EVENTS_TOPIC = TOPIC_EVENTS_PROTECTION
SMARTPLUG_RELAY_TOPIC = TOPIC_STATE_RELAY
SMARTPLUG_LED_TOPIC = TOPIC_STATE_LED
SMARTPLUG_TEMPERATURE_TOPIC = TOPIC_TELEMETRY_TEMPERATURE
SMARTPLUG_ENERGY_TOPIC = TOPIC_TELEMETRY_ENERGY
SMARTPLUG_CMD_TOPIC = TOPIC_COMMAND_RELAY
AYCE_CMD_TOPIC = TOPIC_COMMAND_CONFIG
AYCE_STATUS_TOPIC = TOPIC_COMMAND_ACK


# -----------------------------------------------------------------------------
# PC-side normalized data models
# -----------------------------------------------------------------------------


def now_iso_ms() -> str:
    return datetime.now().isoformat(timespec="milliseconds")


@dataclass
class TelemetrySample:
    timestamp_received_pc: str
    vrms: Optional[float] = None
    irms: Optional[float] = None
    pf: Optional[float] = None
    active_power: Optional[float] = None
    reactive_power: Optional[float] = None
    apparent_power: Optional[float] = None
    frequency: Optional[float] = None
    no_load: Optional[bool] = None
    energy_wh: Optional[float] = None
    relay: Optional[bool] = None
    tmp_c: Optional[float] = None
    raw: Dict[str, Any] = field(default_factory=dict)

    @staticmethod
    def from_payload(payload: Dict[str, Any]) -> "TelemetrySample":
        return TelemetrySample(
            timestamp_received_pc=now_iso_ms(),
            vrms=_as_optional_float(_first_present(payload, "vrms", "voltage_vrms", "voltage")),
            irms=_as_optional_float(_first_present(payload, "irms", "current_arms", "current_a", "current")),
            pf=_as_optional_float(_first_present(payload, "pf", "power_factor")),
            active_power=_as_optional_float(_first_present(payload, "active_power", "power_w", "power")),
            reactive_power=_as_optional_float(_first_present(payload, "reactive_power", "reactive_power_var")),
            apparent_power=_as_optional_float(payload.get("apparent_power")),
            frequency=_as_optional_float(_first_present(payload, "frequency", "frequency_hz")),
            no_load=_as_optional_bool(payload.get("no_load")),
            energy_wh=_as_optional_float(_first_present(payload, "energy_wh", "energy")),
            relay=_as_optional_bool(payload.get("relay")),
            tmp_c=_as_optional_float(_first_present(payload, "tmp_c", "temperature_c")),
            raw=payload,
        )


@dataclass
class CriticalEvent:
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
            voltage_vrms=_as_optional_float(_first_present(data, "voltage_vrms", "vrms")),
            current_a_arms=_as_optional_float(_first_present(data, "current_a_arms", "irms")),
            current_b_arms=_as_optional_float(data.get("current_b_arms")),
            duration_cycles=_as_optional_int(data.get("duration_cycles")),
            action_taken=_as_optional_str(payload.get("action_taken")),
            system_status=_as_optional_str(payload.get("system_status")),
            raw=payload,
        )


@dataclass
class CommandAck:
    timestamp_received_pc: str
    event_type: Optional[str] = None
    accepted: Optional[bool] = None
    command: Optional[str] = None
    action: Optional[str] = None
    max_vrms: Optional[float] = None
    max_iarms: Optional[float] = None
    reason: Optional[str] = None
    raw: Dict[str, Any] = field(default_factory=dict)

    @staticmethod
    def from_payload(payload: Dict[str, Any]) -> "CommandAck":
        return CommandAck(
            timestamp_received_pc=now_iso_ms(),
            event_type=_as_optional_str(payload.get("event_type")),
            accepted=_as_optional_bool(payload.get("accepted")),
            command=_as_optional_str(payload.get("command")),
            action=_as_optional_str(payload.get("action")),
            max_vrms=_as_optional_float(payload.get("max_vrms")),
            max_iarms=_as_optional_float(payload.get("max_iarms")),
            reason=_as_optional_str(payload.get("reason")),
            raw=payload,
        )


# Backward-compatible name used by earlier GUI code.
SafetyLimitAck = CommandAck


@dataclass
class RelayState:
    timestamp_received_pc: str
    event_type: Optional[str] = None
    relay: Optional[bool] = None
    accepted: Optional[bool] = None
    reason: Optional[str] = None
    raw: Dict[str, Any] = field(default_factory=dict)

    @staticmethod
    def from_payload(payload: Dict[str, Any]) -> "RelayState":
        return RelayState(
            timestamp_received_pc=now_iso_ms(),
            event_type=_as_optional_str(payload.get("event_type")),
            relay=_as_optional_bool(payload.get("relay")),
            accepted=_as_optional_bool(payload.get("accepted")),
            reason=_as_optional_str(payload.get("reason")),
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
            temperature_c=_as_optional_float(_first_present(payload, "temperature_c", "tmp_c")),
            raw=payload,
        )


@dataclass
class EnergySample:
    timestamp_received_pc: str
    voltage: Optional[float] = None
    current: Optional[float] = None
    power: Optional[float] = None
    energy: Optional[float] = None
    raw: Dict[str, Any] = field(default_factory=dict)

    @staticmethod
    def from_payload(payload: Dict[str, Any]) -> "EnergySample":
        return EnergySample(
            timestamp_received_pc=now_iso_ms(),
            voltage=_as_optional_float(_first_present(payload, "voltage", "vrms")),
            current=_as_optional_float(_first_present(payload, "current", "irms")),
            power=_as_optional_float(_first_present(payload, "power", "active_power")),
            energy=_as_optional_float(_first_present(payload, "energy", "energy_wh")),
            raw=payload,
        )


@dataclass
class WaveformMessage:
    timestamp_received_pc: str
    sample_count: Optional[int] = None
    sampling_rate_hz: Optional[float] = None
    duration_s: Optional[float] = None
    raw: Dict[str, Any] = field(default_factory=dict)

    @staticmethod
    def from_payload(payload: Dict[str, Any]) -> "WaveformMessage":
        signals = payload.get("signals") if isinstance(payload.get("signals"), dict) else {}
        voltage_samples = signals.get("voltage_v") or payload.get("voltage_samples") or []
        sample_count = _as_optional_int(payload.get("sample_count"))
        if sample_count is None and isinstance(voltage_samples, list):
            sample_count = len(voltage_samples)
        return WaveformMessage(
            timestamp_received_pc=now_iso_ms(),
            sample_count=sample_count,
            sampling_rate_hz=_as_optional_float(_first_present(payload, "sampling_rate_hz", "fs_hz")),
            duration_s=_as_optional_float(payload.get("duration_s")),
            raw=payload,
        )


@dataclass
class CommandEcho:
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


def _first_present(mapping: Dict[str, Any], *keys: str) -> Any:
    for key in keys:
        if key in mapping:
            return mapping.get(key)
    return None


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
        if normalized in ("true", "1", "on", "yes", "relay_on"):
            return True
        if normalized in ("false", "0", "off", "no", "relay_off"):
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

    if topic in COMMAND_TOPICS:
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
            error=parsed_json_error if json_payload is None and topic != LEGACY_TOPIC_RELAY_CMD else None,
        )

    if json_payload is None:
        return ParsedMqttMessage(
            topic=topic,
            kind="invalid_json",
            timestamp_received_pc=timestamp_pc,
            payload_text=payload_text,
            error=parsed_json_error or "Payload is not a JSON object",
        )

    if topic in STATUS_TOPICS:
        return ParsedMqttMessage(topic, "status", timestamp_pc, payload_text, json_payload,
                                 TelemetrySample.from_payload(json_payload))

    if topic in EVENT_TOPICS:
        event_type = json_payload.get("event_type")
        kind = "critical_event" if event_type == "CRITICAL_PROTECTION" else "event"
        return ParsedMqttMessage(topic, kind, timestamp_pc, payload_text, json_payload,
                                 CriticalEvent.from_payload(json_payload))

    if topic in ACK_TOPICS:
        return ParsedMqttMessage(topic, "command_ack", timestamp_pc, payload_text, json_payload,
                                 CommandAck.from_payload(json_payload))

    if topic in RELAY_STATE_TOPICS:
        return ParsedMqttMessage(topic, "relay_state", timestamp_pc, payload_text, json_payload,
                                 RelayState.from_payload(json_payload))

    if topic in LED_STATE_TOPICS:
        return ParsedMqttMessage(topic, "led_state", timestamp_pc, payload_text, json_payload,
                                 LedState.from_payload(json_payload))

    if topic in TEMPERATURE_TOPICS:
        return ParsedMqttMessage(topic, "temperature", timestamp_pc, payload_text, json_payload,
                                 TemperatureSample.from_payload(json_payload))

    if topic in ENERGY_TOPICS:
        return ParsedMqttMessage(topic, "energy", timestamp_pc, payload_text, json_payload,
                                 EnergySample.from_payload(json_payload))

    if topic in WAVEFORM_DATA_TOPICS:
        return ParsedMqttMessage(topic, "waveform", timestamp_pc, payload_text, json_payload,
                                 WaveformMessage.from_payload(json_payload))

    return ParsedMqttMessage(topic, "unknown_json", timestamp_pc, payload_text, json_payload, json_payload)


# -----------------------------------------------------------------------------
# Reusable MQTT client
# -----------------------------------------------------------------------------


def _require_paho_mqtt() -> None:
    if mqtt is None:
        raise RuntimeError(
            "paho-mqtt is required to use SmartPlugMqttClient. "
            "Install it with: pip install paho-mqtt"
        )


class SmartPlugMqttClient:
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
        self.on_command_ack: Optional[Callable[[CommandAck], None]] = None
        self.on_safety_ack: Optional[Callable[[CommandAck], None]] = None  # legacy callback name
        self.on_relay_state: Optional[Callable[[RelayState], None]] = None
        self.on_connection_change: Optional[Callable[[bool, int], None]] = None

        _require_paho_mqtt()
        self.client = self._create_mqtt_client()
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message

    def _create_mqtt_client(self) -> mqtt.Client:
        try:
            return mqtt.Client(
                callback_api_version=mqtt.CallbackAPIVersion.VERSION1,
                client_id=self.client_id,
                clean_session=True,
            )
        except AttributeError:
            return mqtt.Client(client_id=self.client_id, clean_session=True)
        except TypeError:
            return mqtt.Client(client_id=self.client_id, clean_session=True)

    def _on_connect(self, client: mqtt.Client, userdata: Any, flags: Any, rc: int) -> None:
        self.is_connected = rc == 0
        timestamp = now_iso_ms()
        if self.on_connection_change is not None:
            self.on_connection_change(self.is_connected, rc)

        if rc != 0:
            if self.console_print:
                print(f"[{timestamp}] MQTT connection failed with code {rc}")
            return

        if self.console_print:
            print(f"\n[{timestamp}] Connected to MQTT broker at {self.broker}:{self.port}")
            print(f"[{timestamp}] Subscribing to topics: {', '.join(self.subscriptions)}")

        for topic in self.subscriptions:
            result, mid = client.subscribe(topic)
            if self.console_print:
                if result == mqtt.MQTT_ERR_SUCCESS:
                    print(f"[{timestamp}] OK subscribed to: {topic}")
                else:
                    print(f"[{timestamp}] ERROR subscribing to {topic}: result={result}, mid={mid}")

    def _on_disconnect(self, client: mqtt.Client, userdata: Any, rc: int) -> None:
        self.is_connected = False
        if self.on_connection_change is not None:
            self.on_connection_change(False, rc)
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
        elif parsed.kind == "command_ack":
            if self.on_command_ack is not None:
                self.on_command_ack(parsed.data)
            if self.on_safety_ack is not None:
                self.on_safety_ack(parsed.data)
        elif parsed.kind == "relay_state" and self.on_relay_state is not None:
            self.on_relay_state(parsed.data)

    def connect(self) -> None:
        self.client.connect(self.broker, self.port, self.keepalive)

    def start(self) -> None:
        self.connect()
        self.client.loop_start()

    def stop(self) -> None:
        try:
            if self.is_connected:
                self.client.disconnect()
        finally:
            self.client.loop_stop()

    def reconnect(self, broker: str, port: Optional[int] = None) -> None:
        self.stop()
        self.broker = broker
        if port is not None:
            self.port = int(port)
        self.client = self._create_mqtt_client()
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message
        self.start()

    def publish_relay(self, relay_on: bool, qos: int = 1) -> mqtt.MQTTMessageInfo:
        payload = json.dumps(
            {
                "command": "RELAY_ON" if relay_on else "RELAY_OFF",
                "relay": bool(relay_on),
                "source": "GUI",
                "timestamp": now_iso_ms(),
            },
            separators=(",", ":"),
        )
        return self.client.publish(TOPIC_COMMAND_RELAY, payload, qos=qos)

    def publish_safety_limits(self, max_vrms: float, max_iarms: float, qos: int = 1) -> mqtt.MQTTMessageInfo:
        payload = json.dumps(
            {
                "command": "SET_SAFETY_LIMITS",
                "max_vrms": float(max_vrms),
                "max_iarms": float(max_iarms),
                "source": "GUI",
                "timestamp": now_iso_ms(),
            },
            separators=(",", ":"),
        )
        return self.client.publish(TOPIC_COMMAND_CONFIG, payload, qos=qos)

    def publish_waveform_request(
        self,
        sample_count: int = DEFAULT_WAVEFORM_SAMPLE_COUNT,
        sampling_rate_hz: int = DEFAULT_WAVEFORM_SAMPLE_RATE_HZ,
        qos: int = 1,
    ) -> mqtt.MQTTMessageInfo:
        payload = json.dumps(
            {
                "command": "REQUEST_WAVEFORM",
                "sample_count": int(sample_count),
                "sampling_rate_hz": int(sampling_rate_hz),
                "source": "GUI",
                "timestamp": now_iso_ms(),
            },
            separators=(",", ":"),
        )
        return self.client.publish(TOPIC_WAVEFORM_REQUEST, payload, qos=qos)

    def publish_raw(self, topic: str, payload: str, qos: int = 1) -> mqtt.MQTTMessageInfo:
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
            f"V={_fmt(d.vrms, ' Vrms')} | I={_fmt(d.irms, ' Arms', 3)} | "
            f"P={_fmt(d.active_power, ' W')} | Q={_fmt(d.reactive_power, ' var')} | "
            f"S={_fmt(d.apparent_power, ' VA')} | PF={_fmt(d.pf, '', 3)} | f={_fmt(d.frequency, ' Hz')} | "
            f"E={_fmt(d.energy_wh, ' Wh')} | relay={_on_off(d.relay)} | "
            f"no_load={no_load_text} | T={_fmt(d.tmp_c, ' °C')}"
        )

    if parsed.kind == "critical_event" and isinstance(parsed.data, CriticalEvent):
        d = parsed.data
        return (
            f"\n[{ts}] CRITICAL EVENT {parsed.topic} | cause={d.cause or '--'} | "
            f"V={_fmt(d.voltage_vrms, ' Vrms')} | IA={_fmt(d.current_a_arms, ' Arms', 3)} | "
            f"IB={_fmt(d.current_b_arms, ' Arms', 3)} | cycles={_fmt(d.duration_cycles)} | "
            f"action={d.action_taken or '--'} | status={d.system_status or '--'}"
        )

    if parsed.kind == "command_ack" and isinstance(parsed.data, CommandAck):
        d = parsed.data
        result = "ACCEPTED" if d.accepted else "REJECTED" if d.accepted is False else "UNKNOWN"
        return (
            f"\n[{ts}] COMMAND ACK {parsed.topic} | {d.event_type or '--'} | {result} | "
            f"max_vrms={_fmt(d.max_vrms, ' Vrms')} | max_iarms={_fmt(d.max_iarms, ' Arms', 3)} | "
            f"reason={d.reason or '--'}"
        )

    if parsed.kind == "relay_state" and isinstance(parsed.data, RelayState):
        return f"\n[{ts}] RELAY {parsed.topic} | relay={_on_off(parsed.data.relay)}"

    if parsed.kind == "led_state" and isinstance(parsed.data, LedState):
        d = parsed.data
        return f"\n[{ts}] LED {parsed.topic} | r={d.r} g={d.g} b={d.b}"

    if parsed.kind == "temperature" and isinstance(parsed.data, TemperatureSample):
        return f"\n[{ts}] TEMPERATURE {parsed.topic} | T={_fmt(parsed.data.temperature_c, ' °C')}"

    if parsed.kind == "energy" and isinstance(parsed.data, EnergySample):
        d = parsed.data
        return (
            f"\n[{ts}] ENERGY {parsed.topic} | voltage={_fmt(d.voltage)} | "
            f"current={_fmt(d.current)} | power={_fmt(d.power)} | energy={_fmt(d.energy)}"
        )

    if parsed.kind == "waveform" and isinstance(parsed.data, WaveformMessage):
        d = parsed.data
        return (
            f"\n[{ts}] WAVEFORM {parsed.topic} | samples={d.sample_count or '--'} | "
            f"fs={_fmt(d.sampling_rate_hz, ' Hz')} | duration={_fmt(d.duration_s, ' s', 4)}"
        )

    if parsed.kind == "command_echo":
        return f"\n[{ts}] COMMAND ECHO {parsed.topic} | {parsed.payload_text}"

    if parsed.error:
        return f"\n[{ts}] {parsed.kind.upper()} {parsed.topic} | error={parsed.error} | payload={parsed.payload_text}"

    if parsed.json_payload is not None:
        return f"\n[{ts}] {parsed.kind.upper()} {parsed.topic}: {json.dumps(parsed.json_payload, ensure_ascii=False)}"

    return f"\n[{ts}] {parsed.kind.upper()} {parsed.topic}: {parsed.payload_text}"


def print_help() -> None:
    print(
        f"""
Commands:
  relay on                 Publish JSON relay ON to {TOPIC_COMMAND_RELAY}
  relay off                Publish JSON relay OFF to {TOPIC_COMMAND_RELAY}
  on                       Alias for: relay on
  off                      Alias for: relay off
  set <vrms> <iarms>       Publish RMS safety limits to {TOPIC_COMMAND_CONFIG}
                           Example: set 135.0 5.0
  wave                     Request one 512-sample waveform capture
  raw <topic> <payload>    Development helper to publish any payload
  help                     Show this help
  quit                     Exit
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
                print(f"Published relay ON to {TOPIC_COMMAND_RELAY}")
                continue
            if normalized in ("off", "relay off"):
                smartplug.publish_relay(False)
                print(f"Published relay OFF to {TOPIC_COMMAND_RELAY}")
                continue
            if normalized == "wave":
                smartplug.publish_waveform_request()
                print(f"Published waveform request to {TOPIC_WAVEFORM_REQUEST}")
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
                print(f"Published safety limits to {TOPIC_COMMAND_CONFIG}")
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
