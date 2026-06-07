"""
AYCE Smart Plug MQTT GUI
--------------------------

Production-oriented GUI for the AYCE Smart Plug project.

Design goals:
    - Use the PC Mosquitto broker and the ESP32 MQTT firmware as the live data source.
    - Keep BLE provisioning available from the GUI using provisioning/provisioner.py.
    - Use one standardized MQTT topic contract for telemetry, state, commands, ACKs and waveform captures.
    - Show waveform, harmonic spectrum, THD and phase information from a 512-sample ADE7953 capture at 2.4 kHz.

Important FFT normalization note:
    For a coherent sine sampled over an integer number of cycles, the DFT bin
    magnitude for a real sine is approximately N*A_peak/2. Therefore, this GUI
    estimates peak amplitude as 2*|X[k]|/N for harmonic bins. THD is then
    computed from peak amplitudes, which is equivalent to using RMS amplitudes
    because the sqrt(2) factor cancels in the ratio.

    The GUI plots the first 20 harmonics in the compact spectrum view. The
    FFT table includes all integer 60 Hz harmonics available up to Nyquist for
    Fs=2400 Hz.
"""

from __future__ import annotations

import csv
import json
import math
import os
import queue
import sys
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime
from typing import Any, Callable, Dict, List, Optional, Tuple

import tkinter as tk
from tkinter import ttk, messagebox, filedialog


# Allow running this file directly from Software/telemetry while importing
# Software/provisioning/provisioner.py and loading shared assets.
SOFTWARE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ASSETS_DIR = os.path.join(SOFTWARE_DIR, "assets")
AYCE_ICON_ICO = os.path.join(ASSETS_DIR, "ayce_logo.ico")
AYCE_ICON_PNG = os.path.join(ASSETS_DIR, "ayce_logo.png")

def configure_windows_dpi_awareness() -> None:
    """Keep the GUI visually close to the 100% Windows scale design.

    Windows can bitmap-scale non-DPI-aware Tkinter apps at 125%/150%, which
    makes the dashboard too large and can clip panels. Declaring the process as
    DPI-aware prevents that OS-level enlargement. The Tk scaling value is fixed
    later, after creating ``tk.Tk()``, to the 96 DPI / 100% baseline.
    """
    if sys.platform != "win32":
        return
    try:
        import ctypes
    except Exception:
        return

    # Prefer system-DPI awareness for predictable desktop-dashboard sizing.
    # The calls can fail if another library already set DPI awareness; that is
    # harmless, so failures are intentionally ignored.
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)  # PROCESS_SYSTEM_DPI_AWARE
        return
    except Exception:
        pass
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass


def configure_tk_100_percent_scaling(root: tk.Tk) -> None:
    """Normalize Tk font/widget scaling to the 100% Windows design baseline."""
    try:
        root.tk.call("tk", "scaling", TK_100_PERCENT_SCALING)
    except tk.TclError:
        pass

if SOFTWARE_DIR not in sys.path:
    sys.path.insert(0, SOFTWARE_DIR)


def configure_window_icon(root: tk.Tk) -> None:
    """Apply the shared AYCE icon to the Tk window/title bar."""
    try:
        if os.path.exists(AYCE_ICON_ICO):
            root.iconbitmap(AYCE_ICON_ICO)
            root.iconbitmap(default=AYCE_ICON_ICO)
            root.wm_iconbitmap(AYCE_ICON_ICO)
    except tk.TclError:
        pass
    try:
        if os.path.exists(AYCE_ICON_PNG):
            icon_photo = tk.PhotoImage(file=AYCE_ICON_PNG)
            root.iconphoto(True, icon_photo)
            root._ayce_icon_photo = icon_photo  # keep a reference alive
    except tk.TclError:
        pass


from mqtt_client import (
    DEFAULT_BROKER,
    DEFAULT_PORT,
    DEFAULT_WAVEFORM_SAMPLE_COUNT,
    SmartPlugMqttClient,
    TOPIC_COMMAND_ACK,
    TOPIC_COMMAND_CONFIG,
    TOPIC_COMMAND_RELAY,
    TOPIC_EVENTS_PROTECTION,
    TOPIC_STATE_RELAY,
    TOPIC_TELEMETRY_ENERGY,
    TOPIC_TELEMETRY_STATUS,
    TOPIC_TELEMETRY_TEMPERATURE,
    TOPIC_WAVEFORM_DATA,
    TOPIC_WAVEFORM_REQUEST,
    ACK_TOPICS,
    ENERGY_TOPICS,
    EVENT_TOPICS,
    RELAY_STATE_TOPICS,
    STATUS_TOPICS,
    TEMPERATURE_TOPICS,
    WAVEFORM_DATA_TOPICS,
)
from provisioning.provisioner import (
    DEFAULT_MQTT_BROKER,
    DEFAULT_MQTT_PORT,
    DEFAULT_WIFI_PASSWORD,
    DEFAULT_WIFI_SSID,
    TARGET_MAC,
    ProvisioningError,
    send_credentials_sync,
)


# =============================================================================
# Constants and theme
# =============================================================================

APP_TITLE = "AYCE Smart Plug Dashboard"

NOMINAL_VRMS = 127.0
NOMINAL_FREQ_HZ = 60.0
NOMINAL_CURRENT_ARMS = 5.0
# GUI-side capture contract. Kept explicit here so the display, FFT and
# CSV exports remain correct even if older MQTT defaults still exist elsewhere.
SAMPLE_RATE_HZ = 2400
WAVEFORM_SAMPLE_COUNT = 512
WAVEFORM_CAPTURE_SECONDS = WAVEFORM_SAMPLE_COUNT / SAMPLE_RATE_HZ
MAX_PLOT_HARMONIC_ORDER = 20
MAX_TABLE_HARMONIC_ORDER = int((SAMPLE_RATE_HZ / 2.0) // NOMINAL_FREQ_HZ)

TOPIC_STATUS = TOPIC_TELEMETRY_STATUS
TOPIC_EVENTS = TOPIC_EVENTS_PROTECTION
TOPIC_RELAY = TOPIC_STATE_RELAY
TOPIC_TEMPERATURE = TOPIC_TELEMETRY_TEMPERATURE
TOPIC_ENERGY = TOPIC_TELEMETRY_ENERGY
TOPIC_ACK = TOPIC_COMMAND_ACK
TOPIC_SAFETY_ACK = TOPIC_COMMAND_ACK
TOPIC_RELAY_CMD = TOPIC_COMMAND_RELAY
TOPIC_SAFETY_CMD = TOPIC_COMMAND_CONFIG
TOPIC_WAVEFORM_CMD = TOPIC_WAVEFORM_REQUEST
SPECIAL_TOPIC_MQTT_CONNECTION = "__internal/mqtt_connection"
SPECIAL_TOPIC_BLE_RESULT = "__internal/ble_result"

PHASE_PROVISIONING = "provisioning"
PHASE_DASHBOARD = "dashboard"
PHASE_RECONNECTING = "reconnecting"

# GUI-side device heartbeat watchdog. This detects when the ESP32 stops
# publishing telemetry while the PC MQTT client remains connected to Mosquitto.
DEVICE_HEARTBEAT_TIMEOUT_S = 5.0
TK_100_PERCENT_SCALING = 96.0 / 72.0  # 1.3333: Tk baseline for 96 DPI / Windows 100% scale
DEVICE_HEARTBEAT_CHECK_MS = 500


class Theme:
    BG = "#0f172a"             # slate 950
    PANEL = "#111827"          # gray 900
    PANEL_2 = "#1f2937"        # gray 800
    CARD = "#172033"
    CARD_HOVER = "#1d2940"
    BORDER = "#334155"         # slate 700
    TEXT = "#e5e7eb"           # gray 200
    MUTED = "#94a3b8"          # slate 400
    SUBTLE = "#64748b"         # slate 500
    ACCENT = "#38bdf8"         # sky 400
    ACCENT_2 = "#22d3ee"       # cyan 400
    GOOD = "#22c55e"           # green 500
    WARN = "#f59e0b"           # amber 500
    BAD = "#ef4444"            # red 500
    PURPLE = "#a78bfa"         # violet 400
    PINK = "#f472b6"           # pink 400
    WHITE = "#ffffff"
    BLACK = "#020617"


# =============================================================================
# Data models
# =============================================================================

@dataclass
class TelemetrySample:
    vrms: float = 0.0
    irms: float = 0.0
    pf: float = 0.0
    active_power: float = 0.0
    reactive_power: float = 0.0
    frequency: float = NOMINAL_FREQ_HZ
    no_load: bool = False
    energy_wh: float = 0.0
    relay: bool = False
    tmp_c: float = 0.0
    timestamp: str = ""

    @property
    def apparent_power_va(self) -> float:
        # Current team decision: display apparent power from the P-Q triangle.
        # S = sqrt(P^2 + Q^2), using active and reactive power delivered by the
        # metering path instead of Vrms*Irms.
        return math.sqrt(max(0.0, self.active_power * self.active_power + self.reactive_power * self.reactive_power))

    @property
    def current_percent_nominal(self) -> float:
        return 100.0 * self.irms / NOMINAL_CURRENT_ARMS if NOMINAL_CURRENT_ARMS > 0 else 0.0


@dataclass
class CriticalEvent:
    cause: str
    timestamp: str
    action_taken: str
    system_status: str
    data: Dict[str, Any] = field(default_factory=dict)


@dataclass
class SafetyLimitAck:
    accepted: bool
    max_vrms: float
    max_iarms: float
    reason: str = ""
    timestamp: str = ""
    event_type: str = ""
    action: str = ""
    command: str = ""


@dataclass
class WaveformPacket:
    timestamp: str
    duration_s: float
    sampling_rate_hz: int
    voltage_samples: List[float]
    current_samples: List[float]
    fundamental_hz: float
    thd_voltage: float
    thd_current: float
    harmonics: List[Dict[str, float]]
    phase_angle_deg: float = 0.0   # Conventional phi = angle(V) - angle(I). Positive means current lags.
    time_shift_s: float = 0.0      # Positive means current lags voltage in time.


# =============================================================================
# Helpers
# =============================================================================

def now_str() -> str:
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


def clamp(value: float, min_value: float, max_value: float) -> float:
    return max(min_value, min(value, max_value))


def safe_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def safe_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def bool_from_any(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        return value.strip().lower() in {"1", "true", "on", "yes"}
    return False


def dft_peak_amplitudes(
    samples: List[float],
    sampling_rate_hz: int,
    fundamental_hz: float,
    max_order: int = MAX_TABLE_HARMONIC_ORDER,
) -> List[float]:
    """
    Estimate peak amplitudes at integer harmonic orders using a direct DFT at
    each harmonic frequency. For coherent real sine signals:
        A_peak ≈ 2*|X[k]|/N
    This keeps the FFT/harmonic plot in physical units rather than raw DFT-bin
    counts that scale with N.
    """
    n = len(samples)
    if n <= 1 or sampling_rate_hz <= 0 or fundamental_hz <= 0:
        return [0.0] * max_order

    amps: List[float] = []
    mean = sum(samples) / n

    for order in range(1, max_order + 1):
        freq = order * fundamental_hz
        if freq > sampling_rate_hz / 2.0 + 1e-9:
            amps.append(0.0)
            continue
        cos_sum = 0.0
        sin_sum = 0.0
        omega = 2.0 * math.pi * freq / sampling_rate_hz
        for idx, raw in enumerate(samples):
            x = raw - mean
            angle = omega * idx
            cos_sum += x * math.cos(angle)
            sin_sum += x * math.sin(angle)
        magnitude = math.sqrt(cos_sum * cos_sum + sin_sum * sin_sum)
        if abs(freq - sampling_rate_hz / 2.0) < 1e-9:
            amps.append((1.0 / n) * magnitude)
        else:
            amps.append((2.0 / n) * magnitude)
    return amps


def fundamental_sine_phase(samples: List[float], sampling_rate_hz: int, fundamental_hz: float) -> float:
    """Return sine-reference phase in radians at the fundamental frequency.

    For x(t)=A·sin(wt+theta), the direct DFT projections satisfy:
        cos_sum ≈ N·A·sin(theta)/2
        sin_sum ≈ N·A·cos(theta)/2
    therefore theta = atan2(cos_sum, sin_sum).
    """
    n = len(samples)
    if n <= 1 or sampling_rate_hz <= 0 or fundamental_hz <= 0:
        return 0.0
    mean = sum(samples) / n
    omega = 2.0 * math.pi * fundamental_hz / sampling_rate_hz
    cos_sum = 0.0
    sin_sum = 0.0
    for idx, raw in enumerate(samples):
        x = raw - mean
        angle = omega * idx
        cos_sum += x * math.cos(angle)
        sin_sum += x * math.sin(angle)
    return math.atan2(cos_sum, sin_sum)


def wrap_angle_rad(angle: float) -> float:
    """Wrap angle to [-pi, pi)."""
    while angle >= math.pi:
        angle -= 2.0 * math.pi
    while angle < -math.pi:
        angle += 2.0 * math.pi
    return angle


def compute_phase_shift(
    voltage_samples: List[float],
    current_samples: List[float],
    sampling_rate_hz: int,
    fundamental_hz: float,
) -> Tuple[float, float]:
    """Compute conventional V-I displacement angle and time shift.

    phi = angle(V) - angle(I). Positive phi means current lags voltage
    (inductive tendency). Negative phi means current leads voltage
    (capacitive tendency).
    """
    if not voltage_samples or not current_samples or sampling_rate_hz <= 0 or fundamental_hz <= 0:
        return 0.0, 0.0
    n = min(len(voltage_samples), len(current_samples))
    v_phase = fundamental_sine_phase(voltage_samples[:n], sampling_rate_hz, fundamental_hz)
    i_phase = fundamental_sine_phase(current_samples[:n], sampling_rate_hz, fundamental_hz)
    phi = wrap_angle_rad(v_phase - i_phase)
    time_shift_s = phi / (2.0 * math.pi * fundamental_hz)
    return math.degrees(phi), time_shift_s


def compute_thd_from_peak_amplitudes(amps: List[float]) -> float:
    if not amps or amps[0] <= 1e-12:
        return 0.0
    harmonic_energy = sum(a * a for a in amps[1:])
    return math.sqrt(harmonic_energy) / amps[0]


def compute_waveform_analysis(
    voltage_samples: List[float],
    current_samples: List[float],
    sampling_rate_hz: int,
    fundamental_hz: float,
) -> Tuple[float, float, List[Dict[str, float]], float, float]:
    v_amps = dft_peak_amplitudes(voltage_samples, sampling_rate_hz, fundamental_hz)
    i_amps = dft_peak_amplitudes(current_samples, sampling_rate_hz, fundamental_hz)
    thd_v = compute_thd_from_peak_amplitudes(v_amps)
    thd_i = compute_thd_from_peak_amplitudes(i_amps)
    phase_angle_deg, time_shift_s = compute_phase_shift(voltage_samples, current_samples, sampling_rate_hz, fundamental_hz)

    harmonics: List[Dict[str, float]] = []
    for order in range(1, MAX_TABLE_HARMONIC_ORDER + 1):
        harmonics.append({
            "order": float(order),
            "frequency_hz": float(order * fundamental_hz),
            "voltage_mag_vpk": round(v_amps[order - 1], 5),
            "current_mag_apk": round(i_amps[order - 1], 7),
        })
    return thd_v, thd_i, harmonics, phase_angle_deg, time_shift_s


def load_type_from_reactive_power(q_var: float, deadband: float = 1.0) -> str:
    if q_var > deadband:
        return "Inductive load"
    if q_var < -deadband:
        return "Capacitive load"
    return "Mostly resistive load"


def phase_note_from_angle(phase_angle_deg: float, deadband_deg: float = 2.0) -> str:
    if phase_angle_deg > deadband_deg:
        return "Current lags voltage. Inductive load."
    if phase_angle_deg < -deadband_deg:
        return "Current leads voltage. Capacitive load."
    return "Voltage and current are nearly in phase. Mostly resistive load."


def displacement_metrics_from_pq(active_power_w: float, reactive_power_var: float) -> Tuple[float, float, float]:
    """Return apparent power, displacement PF and phi angle from P and Q."""
    p = safe_float(active_power_w)
    q = safe_float(reactive_power_var)
    s = math.sqrt(max(0.0, p * p + q * q))
    disp_pf = p / s if s > 1e-12 else 0.0
    phi_deg = math.degrees(math.atan2(q, p)) if (abs(p) > 1e-12 or abs(q) > 1e-12) else 0.0
    return s, disp_pf, phi_deg


# =============================================================================
# Live MQTT source
# =============================================================================

class MqttSmartPlugDataSource:
    """Live data source used by the GUI.

    The MQTT network loop runs in a Paho background thread. Every received MQTT
    message is forwarded to the Tkinter-safe incoming_queue; the GUI consumes it
    from the main thread in _process_incoming_queue().
    """

    def __init__(self, out_queue: "queue.Queue[Tuple[str, str]]", broker: str = DEFAULT_BROKER, port: int = DEFAULT_PORT):
        self.out_queue = out_queue
        self.broker = broker
        self.port = int(port)
        self.client: Optional[SmartPlugMqttClient] = None
        self._lock = threading.Lock()

    def _emit_internal(self, topic: str, payload: Dict[str, Any]) -> None:
        self.out_queue.put((topic, json.dumps(payload)))

    def _build_client(self) -> SmartPlugMqttClient:
        client = SmartPlugMqttClient(
            broker=self.broker,
            port=self.port,
            client_id="AYCE_SmartPlug_GUI",
            console_print=False,
        )
        client.on_any_message = lambda parsed: self.out_queue.put((parsed.topic, parsed.payload_text))
        client.on_connection_change = lambda connected, rc: self._emit_internal(
            SPECIAL_TOPIC_MQTT_CONNECTION,
            {"connected": bool(connected), "rc": int(rc), "broker": self.broker, "port": self.port},
        )
        return client

    def start(self) -> None:
        def worker() -> None:
            try:
                with self._lock:
                    self.client = self._build_client()
                    client = self.client
                client.start()
            except Exception as exc:
                self._emit_internal(
                    SPECIAL_TOPIC_MQTT_CONNECTION,
                    {"connected": False, "rc": -1, "broker": self.broker, "port": self.port, "error": str(exc)},
                )
        threading.Thread(target=worker, daemon=True).start()

    def stop(self) -> None:
        with self._lock:
            client = self.client
            self.client = None
        if client is not None:
            try:
                client.stop()
            except Exception:
                pass

    def reconnect(self, broker: str, port: int = DEFAULT_PORT) -> None:
        self.stop()
        self.broker = broker
        self.port = int(port)
        self.start()

    def _can_publish(self) -> bool:
        return self.client is not None and self.client.is_connected

    def publish_relay(self, turn_on: bool) -> bool:
        if not self._can_publish():
            return False
        assert self.client is not None
        self.client.publish_relay(turn_on)
        return True

    def publish_safety_limits(self, max_vrms: float, max_iarms: float) -> bool:
        if not self._can_publish():
            return False
        assert self.client is not None
        self.client.publish_safety_limits(max_vrms, max_iarms)
        return True

    def request_waveform_capture(self) -> bool:
        if not self._can_publish():
            return False
        assert self.client is not None
        self.client.publish_waveform_request(sample_count=WAVEFORM_SAMPLE_COUNT, sampling_rate_hz=SAMPLE_RATE_HZ)
        return True


# =============================================================================
# Parser/router
# =============================================================================

class MessageParser:
    @staticmethod
    def parse_telemetry(data: Dict[str, Any]) -> TelemetrySample:
        sample = TelemetrySample(
            vrms=safe_float(data.get("vrms", data.get("voltage_vrms", data.get("voltage")))),
            irms=safe_float(data.get("irms", data.get("current_arms", data.get("current")))),
            pf=safe_float(data.get("pf", data.get("power_factor"))),
            active_power=safe_float(data.get("active_power", data.get("power_w", data.get("power")))),
            reactive_power=safe_float(data.get("reactive_power", data.get("reactive_power_var"))),
            frequency=safe_float(data.get("frequency", data.get("frequency_hz")), NOMINAL_FREQ_HZ),
            no_load=bool_from_any(data.get("no_load")),
            energy_wh=safe_float(data.get("energy_wh", data.get("energy"))),
            relay=bool_from_any(data.get("relay")),
            tmp_c=safe_float(data.get("tmp_c", data.get("temperature_c"))),
            timestamp=str(data.get("timestamp", now_str())),
        )

        # Keep the dashboard clean when firmware flags NO LOAD. The line voltage,
        # frequency, relay state, temperature and accumulated energy may still be
        # meaningful, but load-dependent quantities should not keep stale values.
        if sample.no_load:
            sample.irms = 0.0
            sample.pf = 0.0
            sample.active_power = 0.0
            sample.reactive_power = 0.0

        return sample

    @staticmethod
    def parse_event(data: Dict[str, Any]) -> CriticalEvent:
        return CriticalEvent(
            cause=str(data.get("cause", data.get("event_type", "UNKNOWN_EVENT"))),
            timestamp=str(data.get("timestamp", now_str())),
            action_taken=str(data.get("action_taken", "")),
            system_status=str(data.get("system_status", "")),
            data=dict(data.get("data", {})),
        )

    @staticmethod
    def parse_safety_ack(data: Dict[str, Any]) -> SafetyLimitAck:
        return SafetyLimitAck(
            accepted=bool_from_any(data.get("accepted")),
            max_vrms=safe_float(data.get("max_vrms")),
            max_iarms=safe_float(data.get("max_iarms")),
            reason=str(data.get("reason", "")),
            timestamp=str(data.get("timestamp", now_str())),
            event_type=str(data.get("event_type", "")),
            action=str(data.get("action", "")),
            command=str(data.get("command", "")),
        )

    @staticmethod
    def parse_waveform(data: Dict[str, Any]) -> WaveformPacket:
        signals = data.get("signals", {}) or {}
        analysis = data.get("analysis", {}) or {}
        voltage_raw = signals.get("voltage_v", data.get("voltage_samples", data.get("v", [])))
        current_raw = signals.get("current_a", data.get("current_samples", data.get("i", [])))
        voltage_samples = [safe_float(x) for x in voltage_raw]
        current_samples = [safe_float(x) for x in current_raw]
        sampling_rate = int(safe_float(data.get("sampling_rate_hz", data.get("fs_hz")), SAMPLE_RATE_HZ))
        fundamental = safe_float(analysis.get("fundamental_hz", data.get("fundamental_hz")), NOMINAL_FREQ_HZ)

        # Recompute on the PC side from raw samples. This keeps FFT, THD and phase
        # display consistent even if the ESP32 sends samples only.
        thd_v, thd_i, harmonics, phase_angle_deg, time_shift_s = compute_waveform_analysis(
            voltage_samples, current_samples, sampling_rate, fundamental
        )

        return WaveformPacket(
            timestamp=str(data.get("timestamp", now_str())),
            duration_s=safe_float(data.get("duration_s"), len(voltage_samples) / sampling_rate if sampling_rate else 0.0),
            sampling_rate_hz=sampling_rate,
            voltage_samples=voltage_samples,
            current_samples=current_samples,
            fundamental_hz=fundamental,
            thd_voltage=thd_v,
            thd_current=thd_i,
            harmonics=harmonics,
            phase_angle_deg=phase_angle_deg,
            time_shift_s=time_shift_s,
        )


class MessageRouter:
    def __init__(self, app: "SmartPlugApp"):
        self.app = app

    def route(self, topic: str, payload_str: str) -> None:
        try:
            data = json.loads(payload_str)
        except json.JSONDecodeError as exc:
            self.app.log(f"Invalid JSON on {topic}: {exc}", level="error")
            return

        if topic == SPECIAL_TOPIC_MQTT_CONNECTION:
            self.app.on_mqtt_connection_event(data)
        elif topic == SPECIAL_TOPIC_BLE_RESULT:
            self.app.on_ble_result(data)
        elif topic in STATUS_TOPICS:
            self.app.on_telemetry(MessageParser.parse_telemetry(data))
        elif topic in EVENT_TOPICS:
            event_type = data.get("event_type", "")
            if event_type == "CRITICAL_PROTECTION":
                self.app.on_critical_event(MessageParser.parse_event(data))
            else:
                self.app.log(f"Device event: {event_type}", level="info")
        elif topic in RELAY_STATE_TOPICS:
            relay = bool_from_any(data.get("relay"))
            accepted = bool_from_any(data.get("accepted", True))
            reason = str(data.get("reason", ""))
            self.app.on_relay_update(relay, accepted, reason)
        elif topic in TEMPERATURE_TOPICS:
            temp = safe_float(data.get("temperature_c", data.get("tmp_c")))
            self.app.on_temperature_update(temp)
        elif topic in ENERGY_TOPICS:
            self.app.on_energy_packet(data)
        elif topic in ACK_TOPICS:
            self.app.on_command_ack(MessageParser.parse_safety_ack(data))
        elif topic in WAVEFORM_DATA_TOPICS:
            self.app.on_waveform_packet(MessageParser.parse_waveform(data))
        else:
            self.app.log(f"Unhandled topic {topic}", level="rx")


# =============================================================================
# UI components
# =============================================================================

class Card(tk.Frame):
    def __init__(self, parent: tk.Widget, title: str, value: str = "--", unit: str = "",
                 subtitle: str = "", accent: str = Theme.ACCENT):
        super().__init__(parent, bg=Theme.CARD, highlightthickness=1, highlightbackground=Theme.BORDER)
        self.configure(padx=10, pady=6)
        self.accent = accent

        self.title_label = tk.Label(self, text=title.upper(), bg=Theme.CARD, fg=Theme.MUTED,
                                    font=("Segoe UI", 8, "bold"), anchor="w")
        self.title_label.pack(anchor="w", fill="x")

        line = tk.Frame(self, bg=Theme.CARD)
        line.pack(anchor="w", fill="x", pady=(3, 0))
        self.value_label = tk.Label(line, text=value, bg=Theme.CARD, fg=Theme.TEXT,
                                    font=("Segoe UI", 18, "bold"))
        self.value_label.pack(side="left")
        self.unit_label = tk.Label(line, text=unit, bg=Theme.CARD, fg=accent,
                                   font=("Segoe UI", 9, "bold"))
        self.unit_label.pack(side="left", padx=(5, 0), pady=(7, 0))

        self.subtitle_label = tk.Label(self, text=subtitle, bg=Theme.CARD, fg=Theme.SUBTLE,
                                       font=("Segoe UI", 7), anchor="w")
        self.subtitle_label.pack(anchor="w", fill="x", pady=(1, 0))

    def set(self, value: str, unit: Optional[str] = None, subtitle: Optional[str] = None,
            accent: Optional[str] = None) -> None:
        if accent is not None:
            self.accent = accent
            self.unit_label.configure(fg=accent)
        self.value_label.configure(text=value)
        if unit is not None:
            self.unit_label.configure(text=unit)
        if subtitle is not None:
            self.subtitle_label.configure(text=subtitle)


class HeaderBar(tk.Frame):
    def __init__(self, parent: tk.Widget, app: "SmartPlugApp"):
        super().__init__(parent, bg=Theme.BG)
        self.app = app
        self.pack(fill="x", padx=18, pady=(9, 6))

        right = tk.Frame(self, bg=Theme.BG)
        right.pack(side="right", anchor="ne")

        left = tk.Frame(self, bg=Theme.BG)
        left.pack(side="left", fill="x", expand=True)

        title_row = tk.Frame(left, bg=Theme.BG)
        title_row.pack(anchor="w", fill="x")
        tk.Label(title_row, text="AYCE Smart Plug", bg=Theme.BG, fg=Theme.TEXT,
                 font=("Segoe UI", 22, "bold")).pack(side="left", anchor="w")
        self.save_metrics_btn = tk.Button(
            title_row, text="Save metrics CSV", bg=Theme.PANEL_2, fg=Theme.TEXT,
            activebackground=Theme.CARD_HOVER, activeforeground=Theme.TEXT,
            relief="flat", font=("Segoe UI", 9, "bold"), padx=10, pady=5,
            command=app.save_metrics_csv,
        )
        # This button is intentionally shown only on the main dashboard phase.
        # It is hidden during BLE provisioning/reconnection because no live
        # dashboard metrics are expected to be available there.

        tk.Label(left, text="Live MQTT dashboard · BLE provisioning",
                 bg=Theme.BG, fg=Theme.MUTED, font=("Segoe UI", 10)).pack(anchor="w")

        self.connection_badge = tk.Label(right, text="MQTT: DISCONNECTED", bg=Theme.PANEL_2,
                                         fg=Theme.BAD, font=("Segoe UI", 9, "bold"), padx=10, pady=5)
        self.connection_badge.pack(anchor="e")
        self.phase_badge = tk.Label(right, text="MAIN DASHBOARD", bg=Theme.PANEL_2, fg=Theme.MUTED,
                                    font=("Segoe UI", 8, "bold"), padx=10, pady=4)
        self.phase_badge.pack(anchor="e", pady=(5, 0))
        self.clock_label = tk.Label(right, text="--", bg=Theme.BG, fg=Theme.MUTED,
                                    font=("Segoe UI", 9))
        self.clock_label.pack(anchor="e", pady=(5, 0))
        self._tick_clock()

    def _tick_clock(self) -> None:
        self.clock_label.configure(text=datetime.now().strftime("%Y-%m-%d %H:%M:%S"))
        self.after(1000, self._tick_clock)

    def _set_save_metrics_visible(self, visible: bool) -> None:
        if visible:
            if not self.save_metrics_btn.winfo_ismapped():
                self.save_metrics_btn.pack(side="left", padx=(18, 0), pady=(4, 0))
        else:
            if self.save_metrics_btn.winfo_ismapped():
                self.save_metrics_btn.pack_forget()

    def _dev_btn(self, parent: tk.Widget, text: str, command: Callable[[], None]) -> None:
        tk.Button(parent, text=text, bg=Theme.PANEL_2, fg=Theme.MUTED, activebackground=Theme.CARD_HOVER,
                  activeforeground=Theme.TEXT, relief="flat", font=("Segoe UI", 7), padx=6, pady=2,
                  command=command).pack(side="left", padx=(4, 0))

    def set_phase(self, phase: str) -> None:
        readable = {
            PHASE_PROVISIONING: "PROVISIONING / BLE",
            PHASE_DASHBOARD: "MAIN DASHBOARD",
            PHASE_RECONNECTING: "WAITING FOR MQTT",
        }.get(phase, phase.upper())
        self.phase_badge.configure(text=readable)
        if phase == PHASE_DASHBOARD:
            self._set_save_metrics_visible(True)
            self.connection_badge.configure(text="MQTT: CONNECTED", fg=Theme.GOOD)
        elif phase == PHASE_RECONNECTING:
            self._set_save_metrics_visible(False)
            self.connection_badge.configure(text="MQTT: WAITING FOR DEVICE", fg=Theme.WARN)
        else:
            self._set_save_metrics_visible(False)
            self.connection_badge.configure(text="MQTT: NOT CONNECTED", fg=Theme.BAD)


class ProvisioningFrame(tk.Frame):
    def __init__(self, parent: tk.Widget, app: "SmartPlugApp"):
        super().__init__(parent, bg=Theme.BG)
        self.app = app
        self.credentials_sent = False

        container = tk.Frame(self, bg=Theme.BG)
        container.pack(expand=True, fill="both", padx=28, pady=18)
        container.grid_columnconfigure(0, weight=1)
        container.grid_columnconfigure(1, weight=1)

        left = tk.Frame(container, bg=Theme.PANEL, highlightbackground=Theme.BORDER,
                        highlightthickness=1, padx=24, pady=22)
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 12))
        right = tk.Frame(container, bg=Theme.PANEL, highlightbackground=Theme.BORDER,
                         highlightthickness=1, padx=24, pady=22)
        right.grid(row=0, column=1, sticky="nsew", padx=(12, 0))

        tk.Label(left, text="BLE Provisioning", bg=Theme.PANEL, fg=Theme.TEXT,
                 font=("Segoe UI", 22, "bold")).pack(anchor="w")
        self.status_label = tk.Label(
            left,
            text="No MQTT connection detected. You can send credentials over BLE or wait for reconnection.",
            bg=Theme.PANEL, fg=Theme.MUTED, font=("Segoe UI", 11), wraplength=520, justify="left")
        self.status_label.pack(anchor="w", pady=(6, 18))

        form = tk.Frame(left, bg=Theme.PANEL)
        form.pack(fill="x")
        self.ssid_var = tk.StringVar(value=DEFAULT_WIFI_SSID)
        self.pass_var = tk.StringVar(value=DEFAULT_WIFI_PASSWORD)
        self.broker_var = tk.StringVar(value=DEFAULT_MQTT_BROKER)
        self.port_var = tk.StringVar(value=str(DEFAULT_MQTT_PORT))
        self.mac_var = tk.StringVar(value=TARGET_MAC)

        self._form_row(form, "WiFi SSID", self.ssid_var, 0)
        self._form_row(form, "WiFi Password", self.pass_var, 1, show="*")
        self._form_row(form, "MQTT Broker IP", self.broker_var, 2)
        self._form_row(form, "MQTT Port", self.port_var, 3)
        self._form_row(form, "ESP32 BLE MAC", self.mac_var, 4)

        btns = tk.Frame(left, bg=Theme.PANEL)
        btns.pack(fill="x", pady=(18, 0))
        tk.Button(btns, text="Send credentials over BLE",
                  bg=Theme.ACCENT, fg=Theme.BLACK, activebackground=Theme.ACCENT_2,
                  relief="flat", font=("Segoe UI", 10, "bold"), padx=12, pady=8,
                  command=self._send_ble_credentials).pack(side="left")
        tk.Label(right, text="What is happening", bg=Theme.PANEL, fg=Theme.TEXT,
                 font=("Segoe UI", 18, "bold")).pack(anchor="w")
        instructions = (
            "1. Power the Smart Plug.\n"
            "2. If it does not have credentials yet, it should advertise over BLE.\n"
            "3. The GUI sends the SSID, password and broker information to the device.\n"
            "4. The ESP32 restarts WiFi and stops using BLE.\n"
            "5. Once MQTT telemetry is received, the GUI switches to the main dashboard.\n\n"
            "Keep this window open until the first smartplug/telemetry/status packet is received."
        )
        tk.Label(right, text=instructions, bg=Theme.PANEL, fg=Theme.MUTED,
                 font=("Segoe UI", 11), justify="left", wraplength=520).pack(anchor="w", pady=(10, 18))

        self.progress = ttk.Progressbar(right, mode="indeterminate")
        self.progress.pack(fill="x", pady=(6, 16))
        self.progress.start(14)

        self.small_note = tk.Label(right, text="Status: waiting for user action or MQTT reconnection.",
                                   bg=Theme.PANEL, fg=Theme.ACCENT, font=("Segoe UI", 10, "bold"),
                                   wraplength=520, justify="left")
        self.small_note.pack(anchor="w")

    def _form_row(self, parent: tk.Widget, label: str, var: tk.StringVar, row: int, show: Optional[str] = None) -> None:
        tk.Label(parent, text=label, bg=Theme.PANEL, fg=Theme.MUTED,
                 font=("Segoe UI", 10, "bold")).grid(row=row, column=0, sticky="w", pady=7)
        entry = tk.Entry(parent, textvariable=var, bg=Theme.CARD, fg=Theme.TEXT,
                         insertbackground=Theme.TEXT, relief="flat", show=show,
                         font=("Segoe UI", 10))
        entry.grid(row=row, column=1, sticky="ew", padx=(12, 0), pady=7, ipady=7)
        parent.grid_columnconfigure(1, weight=1)

    def _send_ble_credentials(self) -> None:
        self.credentials_sent = True
        self.status_label.configure(
            text="Sending credentials over BLE. Keep the ESP32 powered and advertising.",
            fg=Theme.WARN,
        )
        self.small_note.configure(text="Status: scanning BLE and writing provisioning JSON…")
        self.app.send_ble_credentials(
            ssid=self.ssid_var.get().strip(),
            password=self.pass_var.get(),
            broker_ip=self.broker_var.get().strip(),
            broker_port=safe_int(self.port_var.get(), DEFAULT_MQTT_PORT),
            mac=self.mac_var.get().strip(),
        )

class DashboardFrame(tk.Frame):
    def __init__(self, parent: tk.Widget, app: "SmartPlugApp"):
        super().__init__(parent, bg=Theme.BG)
        self.app = app
        self.current_telemetry = TelemetrySample()

        main = tk.Frame(self, bg=Theme.BG)
        main.pack(expand=True, fill="both", padx=18, pady=(2, 8))
        main.grid_columnconfigure(0, weight=3, uniform="dashboard_cols")
        main.grid_columnconfigure(1, weight=2, uniform="dashboard_cols")
        main.grid_rowconfigure(0, weight=0)
        main.grid_rowconfigure(1, weight=0)
        main.grid_rowconfigure(2, weight=1)

        self._build_status_cards(main)
        self.power_triangle = PowerTrianglePanel(main)
        self.power_triangle.grid(row=0, column=1, rowspan=2, sticky="nsew", padx=(10, 0), pady=(0, 6))

        controls_frame = tk.Frame(main, bg=Theme.BG)
        controls_frame.grid(row=1, column=0, sticky="nsew", padx=(0, 10), pady=(0, 6))
        controls_frame.grid_columnconfigure(0, weight=1, uniform="control_cols")
        controls_frame.grid_columnconfigure(1, weight=1, uniform="control_cols")
        controls_frame.grid_rowconfigure(0, weight=1)

        self.relay_panel = RelayControlPanel(controls_frame, app)
        self.relay_panel.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        self.safety_panel = SafetyLimitsPanel(controls_frame, app)
        self.safety_panel.grid(row=0, column=1, sticky="nsew")

        self.waveform_panel = WaveformAnalysisPanel(main, app)
        self.waveform_panel.grid(row=2, column=0, columnspan=2, sticky="nsew")
        self.status_panel = NullStatusPanel()

    def _build_status_cards(self, parent: tk.Widget) -> None:
        cards_frame = tk.Frame(parent, bg=Theme.BG)
        cards_frame.grid(row=0, column=0, sticky="nsew", padx=(0, 10), pady=(0, 6))
        for col in range(5):
            cards_frame.grid_columnconfigure(col, weight=1, uniform="status_cards")

        self.card_v = Card(cards_frame, "Voltage", "--", "V RMS", f"Nominal: {NOMINAL_VRMS:.0f} V RMS", Theme.ACCENT)
        self.card_i = Card(cards_frame, "Current", "--", "A RMS", f"Nominal limit: {NOMINAL_CURRENT_ARMS:.0f} A RMS", Theme.PURPLE)
        self.card_p = Card(cards_frame, "Active Power", "--", "W", "Real power delivered", Theme.GOOD)
        self.card_q = Card(cards_frame, "Reactive Power", "--", "var", "Reactive component", Theme.ACCENT)
        self.card_s = Card(cards_frame, "Apparent Power", "--", "VA", "√(P² + Q²)", Theme.PURPLE)
        self.card_pf = Card(cards_frame, "Power Factor", "--", "", "True PF, including harmonic distortion", Theme.WARN)
        self.card_f = Card(cards_frame, "Frequency", "--", "Hz", "Mexico grid: 60 Hz", Theme.ACCENT_2)
        self.card_e = Card(cards_frame, "Active Energy", "--", "Wh", "Accumulated", Theme.PINK)
        self.card_t = Card(cards_frame, "Temperature", "--", "°C", "Internal / board temperature", Theme.WARN)
        self.card_load = Card(cards_frame, "Load State", "--", "", "No-load detection", Theme.MUTED)

        cards = [self.card_v, self.card_i, self.card_p, self.card_q, self.card_s,
                 self.card_pf, self.card_f, self.card_e, self.card_t, self.card_load]
        for row in range(2):
            cards_frame.grid_rowconfigure(row, weight=1, uniform="status_rows")
        for idx, card in enumerate(cards):
            card.grid(row=idx // 5, column=idx % 5, sticky="nsew", padx=5, pady=3)

    def update_telemetry(self, sample: TelemetrySample) -> None:
        self.current_telemetry = sample
        v_accent = Theme.GOOD if 114 <= sample.vrms <= 140 else Theme.WARN
        i_accent = Theme.GOOD if sample.irms <= NOMINAL_CURRENT_ARMS else Theme.WARN
        pf_accent = Theme.GOOD if sample.pf >= 0.90 else Theme.WARN if sample.pf >= 0.75 else Theme.BAD
        q_accent = Theme.WARN if sample.reactive_power > 1.0 else Theme.ACCENT if sample.reactive_power < -1.0 else Theme.SUBTLE
        q_subtitle = "Inductive (positive)" if sample.reactive_power > 1.0 else "Capacitive (negative)" if sample.reactive_power < -1.0 else "Nearly resistive"

        self.card_v.set(f"{sample.vrms:.1f}", "V RMS", f"Δ vs 127 V: {sample.vrms - NOMINAL_VRMS:+.1f} V", v_accent)
        self.card_i.set(f"{sample.irms:.3f}", "A RMS", f"{sample.current_percent_nominal:.0f}% of 5 A RMS nominal limit", i_accent)
        self.card_p.set(f"{sample.active_power:.1f}", "W", "Real power delivered", Theme.GOOD)
        self.card_q.set(f"{sample.reactive_power:.1f}", "var", q_subtitle, q_accent)
        self.card_s.set(f"{sample.apparent_power_va:.1f}", "VA", "Calculated as √(P² + Q²)", Theme.PURPLE)
        self.card_pf.set(f"{sample.pf:.3f}", "", "True PF, including harmonic distortion", pf_accent)
        self.card_f.set(f"{sample.frequency:.2f}", "Hz", f"Δ vs 60 Hz: {sample.frequency - NOMINAL_FREQ_HZ:+.2f} Hz")

        if sample.energy_wh >= 1000.0:
            self.card_e.set(f"{sample.energy_wh / 1000.0:.4f}", "kWh", "Accumulated")
        else:
            self.card_e.set(f"{sample.energy_wh:.2f}", "Wh", "Accumulated")

        self.card_t.set(f"{sample.tmp_c:.1f}", "°C")
        if sample.no_load:
            self.card_load.set("NO LOAD", "", "Relay may be open or current is near zero", Theme.SUBTLE)
        else:
            self.card_load.set("ACTIVE", "", "Load detected", Theme.GOOD)

        self.power_triangle.update_triangle(sample)
        self.relay_panel.update_relay(sample.relay)
        self.status_panel.set_telemetry_state("Receiving smartplug/telemetry/status", Theme.GOOD)

    def set_collecting(self, collecting: bool, duration_s: float = 0.0) -> None:
        self.waveform_panel.set_collecting(collecting, duration_s)
        if collecting:
            self.status_panel.set_telemetry_state("Base telemetry paused for 512-sample waveform capture", Theme.WARN)
        else:
            self.status_panel.set_telemetry_state("Receiving smartplug/telemetry/status", Theme.GOOD)


class RelayControlPanel(tk.Frame):
    def __init__(self, parent: tk.Widget, app: "SmartPlugApp"):
        super().__init__(parent, bg=Theme.PANEL, highlightbackground=Theme.BORDER, highlightthickness=1, padx=14, pady=9)
        self.app = app

        # Vertical control layout: same button disposition as the earlier version,
        # but with taller buttons and a one-line operational note.
        tk.Label(self, text="Relay Control", bg=Theme.PANEL, fg=Theme.TEXT,
                 font=("Segoe UI", 14, "bold")).pack(anchor="w")
        self.state_label = tk.Label(self, text="Relay: --", bg=Theme.PANEL, fg=Theme.MUTED,
                                    font=("Segoe UI", 12, "bold"))
        self.state_label.pack(anchor="w", pady=(6, 2))
        self.notice_label = tk.Label(self, text="Ready for manual relay command.", bg=Theme.PANEL,
                                     fg=Theme.SUBTLE, font=("Segoe UI", 9), justify="left", anchor="w")
        self.notice_label.pack(anchor="w", fill="x", pady=(0, 9))

        self.on_btn = tk.Button(self, text="TURN ON", bg=Theme.GOOD, fg=Theme.BLACK,
                                activebackground=Theme.GOOD, relief="flat",
                                font=("Segoe UI", 10, "bold"), padx=12, pady=10,
                                command=lambda: app.publish_relay_command(True))
        self.off_btn = tk.Button(self, text="TURN OFF", bg=Theme.BAD, fg=Theme.WHITE,
                                 activebackground=Theme.BAD, relief="flat",
                                 font=("Segoe UI", 10, "bold"), padx=12, pady=10,
                                 command=lambda: app.publish_relay_command(False))
        self.on_btn.pack(fill="x", pady=(0, 7))
        self.off_btn.pack(fill="x")

    def update_relay(self, is_on: bool) -> None:
        text = "Relay: ON" if is_on else "Relay: OFF"
        color = Theme.GOOD if is_on else Theme.BAD
        self.state_label.configure(text=text, fg=color)
        
        # Visually disable the redundant button and enable the available action
        if is_on:
            self.notice_label.configure(text="Load energized. ADE7953 protections remain active.", fg=Theme.GOOD)
            self.on_btn.configure(state="disabled", bg=Theme.PANEL_2, fg=Theme.MUTED)
            self.off_btn.configure(state="normal", bg=Theme.BAD, fg=Theme.WHITE)
        else:
            self.notice_label.configure(text="Relay open. Load is not energized from the smart plug.", fg=Theme.SUBTLE)
            self.on_btn.configure(state="normal", bg=Theme.GOOD, fg=Theme.BLACK)
            self.off_btn.configure(state="disabled", bg=Theme.PANEL_2, fg=Theme.MUTED)

    def set_protection(self, event: CriticalEvent) -> None:
        self.state_label.configure(text="Relay: OFF", fg=Theme.BAD)
        self.notice_label.configure(
            text=f"Protection trip: {event.cause}. Relay opened by ADE7953 interrupt. Inspect load/wiring, then TURN ON to retry.",
            fg=Theme.WARN,
        )
        # Because the hardware forcefully opened the relay, we sync the GUI buttons to allow turning it back on
        self.on_btn.configure(state="normal", bg=Theme.GOOD, fg=Theme.BLACK)
        self.off_btn.configure(state="disabled", bg=Theme.PANEL_2, fg=Theme.MUTED)

class SafetyLimitsPanel(tk.Frame):
    PRESETS = {
        "General 5 A prototype": (135.0, 5.0, "General profile for the prototype nominal limit."),
        "Small electronics": (132.0, 1.5, "Small chargers, routers or light electronics."),
        "TV / Monitor": (132.0, 2.5, "TV, large monitor or moderate electronic load."),
        "Laptop gaming charger": (135.0, 3.5, "High-power chargers; verify the charger nameplate."),
        "Refrigerator": (135.0, 5.0, "Consider compressor inrush current."),
        "Custom": (135.0, 5.0, "Manually editable values."),
    }

    def __init__(self, parent: tk.Widget, app: "SmartPlugApp"):
        super().__init__(parent, bg=Theme.PANEL, highlightbackground=Theme.BORDER, highlightthickness=1, padx=14, pady=9)
        self.app = app

        tk.Label(self, text="Safety Limits", bg=Theme.PANEL, fg=Theme.TEXT,
                 font=("Segoe UI", 14, "bold")).grid(row=0, column=0, columnspan=2, sticky="w")
        self.preset_var = tk.StringVar(value="General 5 A prototype")
        self.max_v_var = tk.StringVar(value="135.0")
        self.max_i_var = tk.StringVar(value="5.0")
        self.note_var = tk.StringVar(value=self.PRESETS[self.preset_var.get()][2])

        tk.Label(self, text="Preset", bg=Theme.PANEL, fg=Theme.MUTED,
                 font=("Segoe UI", 9, "bold")).grid(row=1, column=0, sticky="w", pady=(7, 2))
        preset = ttk.Combobox(self, textvariable=self.preset_var, values=list(self.PRESETS.keys()), state="readonly")
        preset.grid(row=1, column=1, sticky="ew", pady=(7, 2))
        preset.bind("<<ComboboxSelected>>", lambda _event: self._apply_preset_to_fields())

        self._entry_row("Max voltage", self.max_v_var, "V RMS", 2)
        self._entry_row("Max current", self.max_i_var, "A RMS", 3)

        self.note_label = tk.Label(self, textvariable=self.note_var, bg=Theme.PANEL,
                                   fg=Theme.SUBTLE, font=("Segoe UI", 8), wraplength=430,
                                   justify="left")
        self.note_label.grid(row=4, column=0, columnspan=2, sticky="w", pady=(5, 5))

        self.apply_btn = tk.Button(self, text="Apply limits", bg=Theme.ACCENT, fg=Theme.BLACK,
                                   activebackground=Theme.ACCENT_2, relief="flat",
                                   font=("Segoe UI", 10, "bold"), padx=12, pady=6,
                                   command=self._apply_limits)
        self.apply_btn.grid(row=5, column=0, columnspan=2, sticky="ew", pady=(1, 0))

        self.ack_label = tk.Label(self, text="ACK: --", bg=Theme.PANEL, fg=Theme.MUTED,
                                  font=("Segoe UI", 8), justify="left", anchor="w")
        self.ack_label.grid(row=6, column=0, columnspan=2, sticky="ew", pady=(5, 0))
        self.grid_columnconfigure(1, weight=1)

    def _entry_row(self, label: str, var: tk.StringVar, unit: str, row: int) -> None:
        tk.Label(self, text=label, bg=Theme.PANEL, fg=Theme.MUTED,
                 font=("Segoe UI", 9, "bold")).grid(row=row, column=0, sticky="w", pady=3)
        box = tk.Frame(self, bg=Theme.PANEL)
        box.grid(row=row, column=1, sticky="ew", pady=3)
        entry = tk.Entry(box, textvariable=var, bg=Theme.CARD, fg=Theme.TEXT,
                         insertbackground=Theme.TEXT, relief="flat", font=("Segoe UI", 10), width=10)
        entry.pack(side="left", fill="x", expand=True, ipady=4)
        tk.Label(box, text=unit, bg=Theme.PANEL, fg=Theme.MUTED,
                 font=("Segoe UI", 9)).pack(side="left", padx=(6, 0))

    def _apply_preset_to_fields(self) -> None:
        max_v, max_i, note = self.PRESETS.get(self.preset_var.get(), self.PRESETS["Custom"])
        self.max_v_var.set(f"{max_v:.1f}")
        self.max_i_var.set(f"{max_i:.1f}")
        self.note_var.set(note)

    def _apply_limits(self) -> None:
        try:
            max_v = float(self.max_v_var.get())
            max_i = float(self.max_i_var.get())
        except ValueError:
            messagebox.showerror("Invalid limits", "Voltage and current limits must be numeric.")
            return
        self.app.publish_safety_limits(max_v, max_i)

    def update_ack(self, ack: SafetyLimitAck) -> None:
        if ack.accepted:
            self.ack_label.configure(text=f"ACK: OK · Vmax={ack.max_vrms:.1f} V RMS · Imax={ack.max_iarms:.2f} A RMS", fg=Theme.GOOD)
        else:
            self.ack_label.configure(text=f"ACK: rejected ({ack.reason})", fg=Theme.BAD)


class PowerTrianglePanel(tk.Frame):
    def __init__(self, parent: tk.Widget):
        super().__init__(parent, bg=Theme.PANEL, highlightbackground=Theme.BORDER, highlightthickness=1, padx=16, pady=12)
        header = tk.Frame(self, bg=Theme.PANEL)
        header.pack(fill="x")
        title_box = tk.Frame(header, bg=Theme.PANEL)
        title_box.pack(side="left", fill="x", expand=True)
        tk.Label(title_box, text="Power Triangle", bg=Theme.PANEL, fg=Theme.TEXT,
                 font=("Segoe UI", 16, "bold")).pack(anchor="w")
        tk.Label(title_box, text="P, Q, S and power factor visualization", bg=Theme.PANEL,
                 fg=Theme.MUTED, font=("Segoe UI", 9)).pack(anchor="w")
        self.load_badge = tk.Label(header, text="--", bg=Theme.PANEL_2, fg=Theme.MUTED,
                                   font=("Segoe UI", 9, "bold"), padx=9, pady=4,
                                   highlightbackground=Theme.BORDER, highlightthickness=1)
        self.load_badge.pack(side="right", anchor="ne")

        self.canvas = tk.Canvas(self, height=232, bg=Theme.PANEL, highlightthickness=0)
        self.canvas.pack(fill="both", expand=True, pady=(4, 0))
        self.summary = tk.Label(self, text="Waiting for telemetry…", bg=Theme.PANEL,
                                fg=Theme.MUTED, font=("Segoe UI", 8), justify="left", anchor="w")
        self.summary.pack(anchor="w", fill="x", pady=(5, 0))

        self._display_p: Optional[float] = None
        self._display_q: Optional[float] = None
        self._target_p: float = 0.0
        self._target_q: float = 0.0
        self._animation_active: bool = False

    def update_triangle(self, sample: TelemetrySample) -> None:
        self._target_p = sample.active_power
        self._target_q = sample.reactive_power
        load_type = load_type_from_reactive_power(sample.reactive_power)
        self.load_badge.configure(text=load_type, fg=Theme.WARN if sample.reactive_power > 1.0 else Theme.ACCENT if sample.reactive_power < -1.0 else Theme.GOOD)
        if self._display_p is None:
            self._display_p = self._target_p
            self._display_q = self._target_q
            self._draw_triangle()
            return
        if not self._animation_active:
            self._animation_active = True
            self._animate_triangle()

    def _animate_triangle(self) -> None:
        alpha = 0.11
        self._display_p = (self._display_p or 0.0) + alpha * (self._target_p - (self._display_p or 0.0))
        self._display_q = (self._display_q or 0.0) + alpha * (self._target_q - (self._display_q or 0.0))
        self._draw_triangle()
        err = max(abs(self._target_p - (self._display_p or 0.0)), abs(self._target_q - (self._display_q or 0.0)))
        if err > 0.35:
            self.after(35, self._animate_triangle)
        else:
            self._display_p = self._target_p
            self._display_q = self._target_q
            self._draw_triangle()
            self._animation_active = False

    def _draw_triangle(self) -> None:
        c = self.canvas
        c.delete("all")
        w = max(390, c.winfo_width())
        h = max(230, c.winfo_height())

        plot_top = 28
        plot_bottom = h - 42
        origin_x = 66
        axis_right = w - 38
        base_y = (plot_top + plot_bottom) / 2.0
        axis_left = max(24, origin_x - 32)

        # Geometry remains visually smoothed through _display_p/_display_q.
        # Numeric labels use the latest target values immediately so rapidly
        # changing telemetry remains easy to read.
        P = self._display_p if self._display_p is not None else 0.0
        Q = self._display_q if self._display_q is not None else 0.0
        value_p = self._target_p
        value_q = self._target_q
        value_s, value_disp_pf, value_phi_deg = displacement_metrics_from_pq(value_p, value_q)
        abs_p = abs(P)
        abs_q = abs(Q)

        drawable_w = max(100.0, axis_right - origin_x - 30)
        drawable_h = max(60.0, min(base_y - plot_top, plot_bottom - base_y) - 12)
        scale_candidates = [drawable_w / max(abs_p, 1.0)]
        if abs_q > 1.0:
            scale_candidates.append(drawable_h / max(abs_q, 1.0))
        scale = min(scale_candidates)
        p_px = max(42.0, min(drawable_w, abs_p * scale)) if abs_p > 0.2 else 42.0
        q_px = min(drawable_h, abs_q * scale) if abs_q > 0.2 else 0.0
        if abs_q > 1.0:
            q_px = max(22.0, q_px)

        x2 = origin_x + p_px
        yq = base_y - q_px if Q >= 0 else base_y + q_px

        # Axes centered on the active-power axis so Q can be positive or negative.
        c.create_line(axis_left, base_y, axis_right, base_y, fill=Theme.BORDER, width=2, arrow="last")
        c.create_line(origin_x, plot_bottom, origin_x, plot_top, fill=Theme.BORDER, width=2, arrow="last")
        c.create_text(axis_right - 5, base_y - 14, text="P", fill=Theme.GOOD, font=("Segoe UI", 10, "bold"))
        c.create_text(origin_x + 16, plot_top + 6, text="Q", fill=Theme.WARN, font=("Segoe UI", 10, "bold"))
        c.create_text(origin_x + 18, plot_bottom - 6, text="-Q", fill=Theme.ACCENT, font=("Segoe UI", 10, "bold"))

        if abs_p <= 0.2 and abs_q <= 0.2:
            c.create_text(w / 2, h / 2, text="Waiting for non-zero P/Q", fill=Theme.MUTED, font=("Segoe UI", 10, "bold"))
        else:
            c.create_polygon(origin_x, base_y, x2, base_y, x2, yq, fill="#16243a", outline="")
            c.create_line(origin_x, base_y, x2, base_y, fill=Theme.GOOD, width=4, arrow="last")
            c.create_line(x2, base_y, x2, yq, fill=Theme.ACCENT, width=4, arrow="last")
            c.create_line(origin_x, base_y, x2, yq, fill=Theme.PURPLE, width=4, arrow="last")

            c.create_text(min(axis_right - 115, x2 - 64), base_y - 18, text=f"P={value_p:.1f} W", fill=Theme.GOOD,
                          font=("Segoe UI", 9, "bold"))
            q_anchor = "w" if x2 < w - 120 else "e"
            q_x = x2 + 20 if q_anchor == "w" else x2 - 8
            c.create_text(q_x, (base_y + yq) / 2, anchor=q_anchor, text=f"Q={value_q:.1f} var", fill=Theme.ACCENT,
                          font=("Segoe UI", 9, "bold"))

            s_text = f"S={value_s:.1f} VA"
            s_x = origin_x + 0.52 * (x2 - origin_x)
            s_y = base_y + 0.52 * (yq - base_y)
            s_box_half_w = max(46, min(76, 4 * len(s_text) + 14))
            c.create_rectangle(s_x - s_box_half_w, s_y - 14, s_x + s_box_half_w, s_y + 14, fill=Theme.PANEL, outline=Theme.PURPLE)
            c.create_text(s_x, s_y, text=s_text, fill=Theme.PURPLE, font=("Segoe UI", 9, "bold"))

        self.summary.configure(text=f"Displacement PF={value_disp_pf:.3f} · φ={value_phi_deg:+.1f}° · From latest P, Q, S · Ignores harmonic distortion")


class MiniMetric(tk.Frame):
    def __init__(self, parent: tk.Widget, title: str, accent: str):
        super().__init__(parent, bg=Theme.CARD, highlightbackground=Theme.BORDER, highlightthickness=1)
        self.configure(padx=8, pady=4)
        tk.Label(self, text=title.upper(), bg=Theme.CARD, fg=Theme.MUTED,
                 font=("Segoe UI", 7, "bold")).pack(anchor="w")
        self.value = tk.Label(self, text="--", bg=Theme.CARD, fg=accent,
                              font=("Segoe UI", 10, "bold"))
        self.value.pack(anchor="w")

    def set(self, text: str, color: Optional[str] = None) -> None:
        self.value.configure(text=text)
        if color:
            self.value.configure(fg=color)


class WaveformAnalysisPanel(tk.Frame):
    def __init__(self, parent: tk.Widget, app: "SmartPlugApp"):
        super().__init__(parent, bg=Theme.PANEL, highlightbackground=Theme.BORDER, highlightthickness=1, padx=14, pady=10)
        self.app = app
        self.packet: Optional[WaveformPacket] = None
        self.signal_var = tk.StringVar(value="Both")

        header = tk.Frame(self, bg=Theme.PANEL)
        header.pack(fill="x")
        tk.Label(header, text="Waveform Capture · FFT · THD", bg=Theme.PANEL, fg=Theme.TEXT,
                 font=("Segoe UI", 14, "bold")).pack(side="left")
        controls = tk.Frame(header, bg=Theme.PANEL)
        controls.pack(side="right")
        tk.Label(controls, text=f"{WAVEFORM_SAMPLE_COUNT} samples · ~{1000.0 * WAVEFORM_CAPTURE_SECONDS:.2f} ms", bg=Theme.PANEL, fg=Theme.MUTED,
                 font=("Segoe UI", 9, "bold")).pack(side="left", padx=(0, 8))
        ttk.Combobox(controls, textvariable=self.signal_var, values=["Both", "Voltage", "Current"],
                     state="readonly", width=10).pack(side="left", padx=(0, 8))
        self.signal_var.trace_add("write", lambda *_: (self.draw_waveform(), self.draw_fft()))
        tk.Button(controls, text="Request waveform", bg=Theme.ACCENT, fg=Theme.BLACK,
                  activebackground=Theme.ACCENT_2, relief="flat", padx=10, pady=5,
                  font=("Segoe UI", 9, "bold"), command=self._request_waveform).pack(side="left")

        self.status_label = tk.Label(
            self,
            text="No waveform capture yet. Request one 512-sample capture to visualize instantaneous samples, harmonic content, THD and V-I phase.",
            bg=Theme.PANEL, fg=Theme.MUTED, font=("Segoe UI", 10), anchor="w", justify="left")
        self.status_label.pack(fill="x", pady=(4, 5))

        strip = tk.Frame(self, bg=Theme.PANEL)
        strip.pack(fill="x", pady=(0, 4))
        for col in range(6):
            strip.grid_columnconfigure(col, weight=1, uniform="wave_metrics")
        self.thd_v_metric = MiniMetric(strip, "THD voltage", Theme.ACCENT)
        self.thd_i_metric = MiniMetric(strip, "THD current", Theme.PURPLE)
        self.samples_metric = MiniMetric(strip, "Samples", Theme.GOOD)
        self.fs_metric = MiniMetric(strip, "Sampling", Theme.WARN)
        self.phase_metric = MiniMetric(strip, "Phase angle", Theme.ACCENT_2)
        self.shift_metric = MiniMetric(strip, "Time shift", Theme.ACCENT_2)
        metrics = [self.thd_v_metric, self.thd_i_metric, self.samples_metric, self.fs_metric, self.phase_metric, self.shift_metric]
        for idx, metric in enumerate(metrics):
            metric.grid(row=0, column=idx, sticky="ew", padx=(0 if idx == 0 else 6, 0))

        self.phase_note_label = tk.Label(self, text="ⓘ Waiting for V-I phase data.", bg=Theme.PANEL,
                                         fg=Theme.MUTED, font=("Segoe UI", 9, "bold"), anchor="w")
        self.phase_note_label.pack(fill="x", pady=(0, 5))

        body = tk.Frame(self, bg=Theme.PANEL)
        body.pack(expand=True, fill="both")
        body.grid_columnconfigure(0, weight=1, uniform="analysis_cols")
        body.grid_columnconfigure(1, weight=1, uniform="analysis_cols")
        body.grid_rowconfigure(0, weight=1)

        wave_box = tk.Frame(body, bg=Theme.CARD, highlightbackground=Theme.BORDER, highlightthickness=1)
        wave_box.grid(row=0, column=0, sticky="nsew", padx=(0, 6))
        wave_box.grid_columnconfigure(0, weight=1)
        wave_box.grid_rowconfigure(1, weight=1)

        wave_header = tk.Frame(wave_box, bg=Theme.CARD)
        wave_header.grid(row=0, column=0, sticky="ew", padx=10, pady=(6, 1))
        self.wave_title_label = tk.Label(wave_header, text="Instantaneous waveform (Both)", bg=Theme.CARD, fg=Theme.TEXT,
                                         font=("Segoe UI", 10, "bold"))
        self.wave_title_label.pack(side="left")
        self.wave_legend = self._build_signal_legend(wave_header, voltage_unit="[V]", current_unit="[A]")
        self.wave_legend.pack(side="left", padx=(18, 0))
        tk.Button(wave_header, text="Sample table", bg=Theme.PANEL_2, fg=Theme.TEXT,
                  activebackground=Theme.BORDER, relief="flat", padx=8, pady=2,
                  font=("Segoe UI", 8, "bold"), command=self.show_sample_table).pack(side="right")
        self.wave_canvas = tk.Canvas(wave_box, bg=Theme.CARD, highlightthickness=0, height=230)
        self.wave_canvas.grid(row=1, column=0, sticky="nsew", padx=8, pady=(4, 6))

        fft_box = tk.Frame(body, bg=Theme.CARD, highlightbackground=Theme.BORDER, highlightthickness=1)
        fft_box.grid(row=0, column=1, sticky="nsew", padx=(6, 0))
        fft_box.grid_columnconfigure(0, weight=1)
        fft_box.grid_rowconfigure(1, weight=1)
        fft_header = tk.Frame(fft_box, bg=Theme.CARD)
        fft_header.grid(row=0, column=0, sticky="ew", padx=10, pady=(6, 1))
        self.fft_title_label = tk.Label(fft_header, text="Harmonic spectrum / FFT (Both)", bg=Theme.CARD, fg=Theme.TEXT,
                                        font=("Segoe UI", 10, "bold"))
        self.fft_title_label.pack(side="left")
        self.fft_legend = self._build_signal_legend(fft_header, voltage_unit="[Vpk]", current_unit="[Apk]")
        self.fft_legend.pack(side="left", padx=(18, 0))
        tk.Button(fft_header, text="FFT table", bg=Theme.PANEL_2, fg=Theme.TEXT,
                  activebackground=Theme.BORDER, relief="flat", padx=8, pady=2,
                  font=("Segoe UI", 8, "bold"), command=self.show_fft_table).pack(side="right")
        self.fft_canvas = tk.Canvas(fft_box, bg=Theme.CARD, highlightthickness=0, height=230)
        self.fft_canvas.grid(row=1, column=0, sticky="nsew", padx=8, pady=(2, 6))

        self.after(500, self._draw_placeholders)

    def _build_signal_legend(self, parent: tk.Widget, voltage_unit: str, current_unit: str) -> tk.Frame:
        legend = tk.Frame(parent, bg=Theme.CARD)
        legend.voltage_label = tk.Label(legend, text=f"━ Voltage {voltage_unit}", bg=Theme.CARD,
                                        fg=Theme.ACCENT, font=("Segoe UI", 8, "bold"))
        legend.current_label = tk.Label(legend, text=f"━ Current {current_unit}", bg=Theme.CARD,
                                        fg=Theme.PURPLE, font=("Segoe UI", 8, "bold"))
        legend.voltage_label.pack(side="left")
        legend.current_label.pack(side="left", padx=(12, 0))
        return legend

    def _update_signal_legend(self, legend: tk.Frame, mode: str) -> None:
        # Each label keeps its own foreground color. Do not collapse this into a
        # single tk.Label, otherwise Voltage and Current render with one color.
        voltage_label = getattr(legend, "voltage_label", None)
        current_label = getattr(legend, "current_label", None)
        if voltage_label is None or current_label is None:
            return
        if mode == "Voltage":
            voltage_label.pack(side="left")
            current_label.pack_forget()
        elif mode == "Current":
            voltage_label.pack_forget()
            current_label.pack(side="left")
        else:
            voltage_label.pack(side="left")
            current_label.pack(side="left", padx=(12, 0))

    def _request_waveform(self) -> None:
        self.app.publish_waveform_request()

    def _draw_placeholders(self) -> None:
        if self.packet is None:
            self.wave_canvas.delete("all")
            self.fft_canvas.delete("all")
            self._center_text(self.wave_canvas, "Waiting for waveform capture")
            self._center_text(self.fft_canvas, "FFT preview will appear here")
            self.thd_v_metric.set("--")
            self.thd_i_metric.set("--")
            self.samples_metric.set("--")
            self.fs_metric.set(f"{SAMPLE_RATE_HZ} Hz")
            self.phase_metric.set("--")
            self.shift_metric.set("--")

    def _center_text(self, canvas: tk.Canvas, text: str) -> None:
        w = max(260, canvas.winfo_width())
        h = max(135, canvas.winfo_height())
        pad_x = min(38, max(22, int(w * 0.08)))
        pad_y = min(28, max(18, int(h * 0.16)))
        canvas.create_rectangle(pad_x, pad_y, w - pad_x, h - pad_y, outline=Theme.BORDER, dash=(4, 4))
        canvas.create_text(w / 2, h / 2, text=text, fill=Theme.MUTED,
                           font=("Segoe UI", 10, "bold"), justify="center",
                           width=max(120, w - 2 * pad_x - 18))

    def set_collecting(self, collecting: bool, duration_s: float = 0.0) -> None:
        if collecting:
            self.status_label.configure(text=f"Collecting {WAVEFORM_SAMPLE_COUNT} instantaneous samples at {SAMPLE_RATE_HZ} Hz… Base telemetry is temporarily paused.", fg=Theme.WARN)
        else:
            if self.packet is None:
                self.status_label.configure(text="No waveform capture yet. Request one 512-sample capture to visualize instantaneous samples, harmonic content, THD and V-I phase.", fg=Theme.MUTED)
            else:
                ms = 1000.0 * self.packet.duration_s
                cycles = self.packet.duration_s * self.packet.fundamental_hz
                self.status_label.configure(text=f"Waveform capture received: {len(self.packet.voltage_samples)} samples · {ms:.2f} ms · ~{cycles:.2f} cycles at {self.packet.fundamental_hz:.0f} Hz.", fg=Theme.GOOD)

    def update_packet(self, packet: WaveformPacket) -> None:
        self.packet = packet
        self.set_collecting(False)
        self.thd_v_metric.set(f"{100.0 * packet.thd_voltage:.2f} %")
        self.thd_i_metric.set(f"{100.0 * packet.thd_current:.2f} %")
        self.samples_metric.set(f"{len(packet.voltage_samples):,}")
        self.fs_metric.set(f"{packet.sampling_rate_hz} Hz")
        self.phase_metric.set(f"{packet.phase_angle_deg:+.1f} °")
        self.shift_metric.set(f"{1000.0 * packet.time_shift_s:+.2f} ms")
        self.phase_note_label.configure(text="ⓘ " + phase_note_from_angle(packet.phase_angle_deg), fg=Theme.ACCENT)
        self.draw_waveform()
        self.draw_fft()

    def _update_titles(self) -> None:
        mode = self.signal_var.get()
        self.wave_title_label.configure(text=f"Instantaneous waveform ({mode})")
        self.fft_title_label.configure(text=f"Harmonic spectrum / FFT ({mode})")
        self._update_signal_legend(self.wave_legend, mode)
        self._update_signal_legend(self.fft_legend, mode)

    def draw_waveform(self) -> None:
        self._update_titles()
        c = self.wave_canvas
        c.delete("all")
        if self.packet is None:
            self._center_text(c, "Waiting for waveform capture")
            return
        mode = self.signal_var.get()
        if not self.packet.voltage_samples and not self.packet.current_samples:
            self._center_text(c, "No samples available")
            return

        w = max(320, c.winfo_width())
        h = max(200, c.winfo_height())
        pad_l, pad_r, pad_t, pad_b = 72, 62 if mode == "Both" else 22, 24, 52
        plot_w = max(20, w - pad_l - pad_r)
        plot_h = max(40, h - pad_t - pad_b)
        n = min(len(self.packet.voltage_samples), len(self.packet.current_samples)) if mode == "Both" else len(self.packet.voltage_samples if mode == "Voltage" else self.packet.current_samples)
        if n < 2:
            return
        t0 = 0.0
        t1 = (n - 1) / self.packet.sampling_rate_hz

        c.create_rectangle(pad_l, pad_t, pad_l + plot_w, pad_t + plot_h, outline=Theme.BORDER)
        tick_count = 6
        for i in range(tick_count):
            frac = i / (tick_count - 1)
            x = pad_l + frac * plot_w
            tick_t = t0 + frac * (t1 - t0)
            c.create_line(x, pad_t, x, pad_t + plot_h, fill="#24324a")
            c.create_line(x, pad_t + plot_h, x, pad_t + plot_h + 5, fill=Theme.MUTED)
            c.create_text(x, pad_t + plot_h + 17, text=f"{tick_t:.3f}", fill=Theme.MUTED, font=("Segoe UI", 7))

        def y_bounds(samples: List[float]) -> Tuple[float, float]:
            ymin, ymax = min(samples), max(samples)
            margin = 0.10 * max(1e-9, ymax - ymin)
            return ymin - margin, ymax + margin

        def draw_y_axis(samples: List[float], side: str, unit_label: str, color: str) -> Tuple[float, float]:
            ymin, ymax = y_bounds(samples)
            span = max(1e-9, ymax - ymin)
            for i in range(5):
                frac = i / 4
                y = pad_t + frac * plot_h
                value = ymax - frac * span
                c.create_line(pad_l, y, pad_l + plot_w, y, fill="#24324a")
                if side == "left":
                    c.create_line(pad_l - 5, y, pad_l, y, fill=Theme.MUTED)
                    c.create_text(pad_l - 8, y, anchor="e", text=f"{value:.2f}", fill=color, font=("Segoe UI", 7))
                else:
                    x_axis = pad_l + plot_w
                    c.create_line(x_axis, y, x_axis + 5, y, fill=Theme.MUTED)
                    c.create_text(x_axis + 8, y, anchor="w", text=f"{value:.2f}", fill=color, font=("Segoe UI", 7))
            if side == "left":
                c.create_text(18, pad_t + plot_h / 2, text=unit_label, angle=90, fill=color, font=("Segoe UI", 8, "bold"))
            else:
                c.create_text(w - 16, pad_t + plot_h / 2, text=unit_label, angle=90, fill=color, font=("Segoe UI", 8, "bold"))
            return ymin, ymax

        def draw_trace(samples: List[float], ymin: float, ymax: float, color: str) -> None:
            span = max(1e-9, ymax - ymin)
            view = samples[:n]
            points: List[float] = []
            for idx, value in enumerate(view):
                x = pad_l + (idx / (n - 1)) * plot_w
                y = pad_t + plot_h - ((value - ymin) / span) * plot_h
                points.extend([x, y])
            if len(points) >= 4:
                c.create_line(points, fill=color, width=2, smooth=True)

        if mode == "Both":
            v = self.packet.voltage_samples[:n]
            i = self.packet.current_samples[:n]
            v_min, v_max = draw_y_axis(v, "left", "Voltage [V]", Theme.ACCENT)
            i_min, i_max = draw_y_axis(i, "right", "Current [A]", Theme.PURPLE)
            # Zero reference for voltage axis.
            if v_min <= 0.0 <= v_max:
                y_zero = pad_t + plot_h - ((0.0 - v_min) / (v_max - v_min)) * plot_h
                c.create_line(pad_l, y_zero, pad_l + plot_w, y_zero, fill=Theme.BORDER, dash=(3, 3))
            draw_trace(v, v_min, v_max, Theme.ACCENT)
            draw_trace(i, i_min, i_max, Theme.PURPLE)
        else:
            samples = self.packet.voltage_samples if mode == "Voltage" else self.packet.current_samples
            unit_label = "Voltage [V]" if mode == "Voltage" else "Current [A]"
            color = Theme.ACCENT if mode == "Voltage" else Theme.PURPLE
            ymin, ymax = draw_y_axis(samples[:n], "left", unit_label, color)
            if ymin <= 0.0 <= ymax:
                y_zero = pad_t + plot_h - ((0.0 - ymin) / (ymax - ymin)) * plot_h
                c.create_line(pad_l, y_zero, pad_l + plot_w, y_zero, fill=Theme.BORDER, dash=(3, 3))
            draw_trace(samples[:n], ymin, ymax, color)

        c.create_text(pad_l + plot_w / 2, h - 10, text="Time [s]", fill=Theme.MUTED, font=("Segoe UI", 8, "bold"))

    def draw_fft(self) -> None:
        self._update_titles()
        c = self.fft_canvas
        c.delete("all")
        if self.packet is None:
            self._center_text(c, "FFT preview will appear here")
            return
        mode = self.signal_var.get()
        harmonics = self.packet.harmonics[:MAX_PLOT_HARMONIC_ORDER]
        if not harmonics:
            self._center_text(c, "No FFT data available")
            return

        w = max(320, c.winfo_width())
        h = max(200, c.winfo_height())
        # In Both mode the spectrum uses two independent Y axes because voltage
        # and current are different physical quantities. The right padding leaves
        # space for the current axis labels.
        pad_l, pad_r, pad_t, pad_b = 76, 68 if mode == "Both" else 22, 24, 58
        plot_w = max(20, w - pad_l - pad_r)
        plot_h = max(40, h - pad_t - pad_b)

        v_values = [safe_float(h.get("voltage_mag_vpk")) for h in harmonics]
        i_values = [safe_float(h.get("current_mag_apk")) for h in harmonics]
        freqs = [int(round(safe_float(h.get("frequency_hz", (idx + 1) * NOMINAL_FREQ_HZ)))) for idx, h in enumerate(harmonics)]

        def axis_max(values: List[float]) -> float:
            raw = max(max(values) if values else 0.0, 1e-9)
            # A small headroom avoids bars touching the plot border.
            return raw * 1.12

        max_v = axis_max(v_values)
        max_i = axis_max(i_values)
        max_single = max_v if mode == "Voltage" else max_i if mode == "Current" else max(max_v, max_i)

        c.create_rectangle(pad_l, pad_t, pad_l + plot_w, pad_t + plot_h, outline=Theme.BORDER)

        def draw_axis(max_val: float, side: str, label: str, color: str, draw_grid: bool = False) -> None:
            for j in range(5):
                frac = j / 4
                y = pad_t + frac * plot_h
                value = max_val * (1.0 - frac)
                if draw_grid:
                    c.create_line(pad_l, y, pad_l + plot_w, y, fill="#24324a")
                if side == "left":
                    c.create_line(pad_l - 5, y, pad_l, y, fill=Theme.MUTED)
                    c.create_text(pad_l - 8, y, anchor="e", text=f"{value:.2f}", fill=color, font=("Segoe UI", 7))
                    c.create_text(18, pad_t + plot_h / 2, text=label, angle=90, fill=color, font=("Segoe UI", 8, "bold"))
                else:
                    axis_x = pad_l + plot_w
                    c.create_line(axis_x, y, axis_x + 5, y, fill=Theme.MUTED)
                    c.create_text(axis_x + 8, y, anchor="w", text=f"{value:.3f}", fill=color, font=("Segoe UI", 7))
                    c.create_text(w - 16, pad_t + plot_h / 2, text=label, angle=90, fill=color, font=("Segoe UI", 8, "bold"))

        if mode == "Both":
            draw_axis(max_v, "left", "Voltage [Vpk]", Theme.ACCENT, draw_grid=True)
            draw_axis(max_i, "right", "Current [Apk]", Theme.PURPLE, draw_grid=False)
        elif mode == "Voltage":
            draw_axis(max_v, "left", "Voltage [Vpk]", Theme.ACCENT, draw_grid=True)
        else:
            draw_axis(max_i, "left", "Current [Apk]", Theme.PURPLE, draw_grid=True)

        group_count = max(1, len(harmonics))
        group_w = plot_w / group_count
        for idx in range(group_count):
            x_group = pad_l + idx * group_w
            if mode == "Both":
                bar_w = max(2, group_w * 0.30)
                v_x0 = x_group + group_w * 0.18
                i_x0 = x_group + group_w * 0.52
                y_base = pad_t + plot_h
                v_y0 = y_base - (v_values[idx] / max_v) * plot_h
                i_y0 = y_base - (i_values[idx] / max_i) * plot_h
                c.create_rectangle(v_x0, v_y0, v_x0 + bar_w, y_base, fill=Theme.ACCENT, outline="")
                c.create_rectangle(i_x0, i_y0, i_x0 + bar_w, y_base, fill=Theme.PURPLE, outline="")
            else:
                bar_w = max(3, group_w * 0.52)
                val = v_values[idx] if mode == "Voltage" else i_values[idx]
                color = Theme.ACCENT if mode == "Voltage" else Theme.PURPLE
                x0 = x_group + group_w * 0.24
                y_base = pad_t + plot_h
                y0 = y_base - (val / max_single) * plot_h
                c.create_rectangle(x0, y0, x0 + bar_w, y_base, fill=color, outline="")

            label_x = x_group + group_w / 2
            c.create_text(label_x, pad_t + plot_h + 20, text=str(freqs[idx]), angle=90,
                          fill=Theme.MUTED, font=("Segoe UI", 7, "bold"))

        if mode == "Both":
            c.create_text(pad_l, 10, anchor="w", text="FFT amplitude · dual Y axes", fill=Theme.MUTED, font=("Segoe UI", 8, "bold"))
        else:
            unit = "Vpk" if mode == "Voltage" else "Apk"
            c.create_text(pad_l, 10, anchor="w", text=f"FFT amplitude [{unit}]", fill=Theme.MUTED, font=("Segoe UI", 8, "bold"))
        c.create_text(pad_l + plot_w / 2, h - 8, text="Frequency [Hz] / Harmonic order", fill=Theme.MUTED, font=("Segoe UI", 8, "bold"))

    def show_sample_table(self) -> None:
        if self.packet is None:
            messagebox.showinfo("Sample table", "Request a waveform capture first to populate the sample table.")
            return
        win = tk.Toplevel(self)
        win.title("AYCE Smart Plug · Instantaneous Sample Table")
        win.geometry("720x540")
        win.configure(bg=Theme.BG)
        win.transient(self.winfo_toplevel())

        header = tk.Frame(win, bg=Theme.BG)
        header.pack(fill="x", padx=14, pady=(12, 6))
        tk.Label(header, text="Instantaneous Sample Table", bg=Theme.BG, fg=Theme.TEXT,
                 font=("Segoe UI", 14, "bold")).pack(anchor="w")
        tk.Label(header, text=f"Capture timestamp: {self.packet.timestamp} · Fs={self.packet.sampling_rate_hz} Hz · {len(self.packet.voltage_samples)} samples",
                 bg=Theme.BG, fg=Theme.MUTED, font=("Segoe UI", 9)).pack(anchor="w", pady=(2, 0))

        table_frame = tk.Frame(win, bg=Theme.BG)
        table_frame.pack(expand=True, fill="both", padx=14, pady=8)
        columns = ("idx", "time", "v", "i")
        tree = ttk.Treeview(table_frame, columns=columns, show="headings", height=18)
        tree.heading("idx", text="Sample")
        tree.heading("time", text="Time [s]")
        tree.heading("v", text="Voltage [V]")
        tree.heading("i", text="Current [A]")
        tree.column("idx", width=90, anchor="center")
        tree.column("time", width=140, anchor="e")
        tree.column("v", width=150, anchor="e")
        tree.column("i", width=150, anchor="e")
        scroll = ttk.Scrollbar(table_frame, orient="vertical", command=tree.yview)
        tree.configure(yscrollcommand=scroll.set)
        tree.pack(side="left", expand=True, fill="both")
        scroll.pack(side="right", fill="y")

        n = min(len(self.packet.voltage_samples), len(self.packet.current_samples))
        for idx in range(n):
            t = idx / self.packet.sampling_rate_hz if self.packet.sampling_rate_hz else 0.0
            tree.insert("", "end", values=(idx, f"{t:.6f}", f"{self.packet.voltage_samples[idx]:.5f}", f"{self.packet.current_samples[idx]:.7f}"))

        footer = tk.Frame(win, bg=Theme.BG)
        footer.pack(fill="x", padx=14, pady=(0, 12))
        tk.Button(footer, text="Save CSV", bg=Theme.ACCENT, fg=Theme.BLACK,
                  activebackground=Theme.ACCENT_2, relief="flat", padx=12, pady=4,
                  command=self.save_sample_csv).pack(side="left")
        tk.Button(footer, text="Close", bg=Theme.PANEL_2, fg=Theme.TEXT,
                  activebackground=Theme.BORDER, relief="flat", padx=12, pady=4,
                  command=win.destroy).pack(side="right")

    def save_sample_csv(self) -> None:
        if self.packet is None:
            messagebox.showinfo("Save CSV", "No waveform capture available yet.")
            return
        filename = filedialog.asksaveasfilename(
            title="Save sample table CSV",
            defaultextension=".csv",
            filetypes=[("CSV files", "*.csv"), ("All files", "*.*")],
            initialfile="smartplug_samples.csv",
        )
        if not filename:
            return
        save_ts = now_str()
        n = min(len(self.packet.voltage_samples), len(self.packet.current_samples))
        with open(filename, "w", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            writer.writerow(["capture_timestamp", "save_timestamp", "sample_index", "time_s", "voltage_v", "current_a"])
            for idx in range(n):
                t = idx / self.packet.sampling_rate_hz if self.packet.sampling_rate_hz else 0.0
                writer.writerow([self.packet.timestamp, save_ts, idx, f"{t:.9f}", self.packet.voltage_samples[idx], self.packet.current_samples[idx]])
        messagebox.showinfo("Save CSV", f"Sample table saved:\n{filename}")

    def show_fft_table(self) -> None:
        if self.packet is None:
            messagebox.showinfo("FFT table", "Request a waveform capture first to populate the harmonic table.")
            return

        win = tk.Toplevel(self)
        win.title("AYCE Smart Plug · FFT Harmonic Table")
        win.geometry("680x540")
        win.configure(bg=Theme.BG)
        win.transient(self.winfo_toplevel())

        header = tk.Frame(win, bg=Theme.BG)
        header.pack(fill="x", padx=14, pady=(12, 6))
        tk.Label(header, text="FFT / Harmonic Table", bg=Theme.BG, fg=Theme.TEXT,
                 font=("Segoe UI", 14, "bold")).pack(anchor="w")
        tk.Label(header,
                 text=("Peak amplitudes normalized as 2·|DFT bin|/N. "
                       "THD is computed from harmonics relative to the 60 Hz fundamental. "
                       "Table includes all integer 60 Hz harmonics up to Nyquist."),
                 bg=Theme.BG, fg=Theme.MUTED, font=("Segoe UI", 9),
                 wraplength=630, justify="left").pack(anchor="w", pady=(2, 0))

        table_frame = tk.Frame(win, bg=Theme.BG)
        table_frame.pack(expand=True, fill="both", padx=14, pady=8)
        columns = ("order", "freq", "vpk", "apk")
        tree = ttk.Treeview(table_frame, columns=columns, show="headings", height=18)
        tree.heading("order", text="Harmonic")
        tree.heading("freq", text="Frequency [Hz]")
        tree.heading("vpk", text="Voltage [Vpk]")
        tree.heading("apk", text="Current [Apk]")
        tree.column("order", width=90, anchor="center")
        tree.column("freq", width=130, anchor="center")
        tree.column("vpk", width=150, anchor="e")
        tree.column("apk", width=150, anchor="e")
        scroll = ttk.Scrollbar(table_frame, orient="vertical", command=tree.yview)
        tree.configure(yscrollcommand=scroll.set)
        tree.pack(side="left", expand=True, fill="both")
        scroll.pack(side="right", fill="y")

        for h in self.packet.harmonics:
            order = int(safe_float(h.get("order")))
            freq = safe_float(h.get("frequency_hz"))
            vpk = safe_float(h.get("voltage_mag_vpk"))
            apk = safe_float(h.get("current_mag_apk"))
            tree.insert("", "end", values=(order, f"{freq:.0f}", f"{vpk:.2f}", f"{apk:.4f}"))

        footer = tk.Frame(win, bg=Theme.BG)
        footer.pack(fill="x", padx=14, pady=(0, 12))
        tk.Label(footer,
                 text=f"THD_V={100*self.packet.thd_voltage:.2f}%   ·   THD_I={100*self.packet.thd_current:.2f}%   ·   Fs={self.packet.sampling_rate_hz} Hz",
                 bg=Theme.BG, fg=Theme.GOOD, font=("Segoe UI", 10, "bold")).pack(side="left")
        tk.Button(footer, text="Save CSV", bg=Theme.ACCENT, fg=Theme.BLACK,
                  activebackground=Theme.ACCENT_2, relief="flat", padx=12, pady=4,
                  command=self.save_fft_csv).pack(side="right", padx=(8, 0))
        tk.Button(footer, text="Close", bg=Theme.PANEL_2, fg=Theme.TEXT,
                  activebackground=Theme.BORDER, relief="flat", padx=12, pady=4,
                  command=win.destroy).pack(side="right")

    def save_fft_csv(self) -> None:
        if self.packet is None:
            messagebox.showinfo("Save CSV", "No waveform capture available yet.")
            return
        filename = filedialog.asksaveasfilename(
            title="Save FFT table CSV",
            defaultextension=".csv",
            filetypes=[("CSV files", "*.csv"), ("All files", "*.*")],
            initialfile="smartplug_fft.csv",
        )
        if not filename:
            return
        save_ts = now_str()
        with open(filename, "w", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            writer.writerow(["capture_timestamp", "save_timestamp", "sampling_rate_hz", "fundamental_hz", "thd_voltage_percent", "thd_current_percent", "harmonic_order", "frequency_hz", "voltage_mag_vpk", "current_mag_apk"])
            for h in self.packet.harmonics:
                writer.writerow([
                    self.packet.timestamp, save_ts, self.packet.sampling_rate_hz, self.packet.fundamental_hz,
                    100.0 * self.packet.thd_voltage, 100.0 * self.packet.thd_current,
                    int(safe_float(h.get("order"))), safe_float(h.get("frequency_hz")),
                    safe_float(h.get("voltage_mag_vpk")), safe_float(h.get("current_mag_apk")),
                ])
        messagebox.showinfo("Save CSV", f"FFT table saved:\n{filename}")


class NullStatusPanel:
    """No-op sink used after removing the visible System Status panel."""
    def set_telemetry_state(self, *args: Any, **kwargs: Any) -> None: pass
    def set_protection(self, *args: Any, **kwargs: Any) -> None: pass
    def clear_protection_after_retry(self, *args: Any, **kwargs: Any) -> None: pass
    def set_command(self, *args: Any, **kwargs: Any) -> None: pass
    def set_ack(self, *args: Any, **kwargs: Any) -> None: pass
    def set_waveform(self, *args: Any, **kwargs: Any) -> None: pass
    def add(self, *args: Any, **kwargs: Any) -> None: pass


class StatusActivityPanel(tk.Frame):
    def __init__(self, parent: tk.Widget):
        super().__init__(parent, bg=Theme.PANEL, highlightbackground=Theme.BORDER, highlightthickness=1, padx=14, pady=10)
        tk.Label(self, text="System Status", bg=Theme.PANEL, fg=Theme.TEXT,
                 font=("Segoe UI", 14, "bold")).pack(anchor="w")
        tk.Label(self, text="Clean operational messages instead of console trace", bg=Theme.PANEL,
                 fg=Theme.MUTED, font=("Segoe UI", 9)).pack(anchor="w", pady=(0, 8))

        # Scrollable body: prevents lower cards from being clipped when the
        # window height is limited, while keeping Power Triangle and System
        # Status visually aligned in the same dashboard column.
        scroll_shell = tk.Frame(self, bg=Theme.PANEL)
        scroll_shell.pack(expand=True, fill="both")
        self.scroll_canvas = tk.Canvas(scroll_shell, bg=Theme.PANEL, highlightthickness=0, bd=0)
        self.scrollbar = ttk.Scrollbar(scroll_shell, orient="vertical", command=self.scroll_canvas.yview)
        self.scroll_canvas.configure(yscrollcommand=self.scrollbar.set)
        self.scroll_canvas.pack(side="left", expand=True, fill="both")
        self.scrollbar.pack(side="right", fill="y")

        self.rows_container = tk.Frame(self.scroll_canvas, bg=Theme.PANEL)
        self.canvas_window = self.scroll_canvas.create_window((0, 0), window=self.rows_container, anchor="nw")
        self.rows_container.bind("<Configure>", self._update_scrollregion)
        self.scroll_canvas.bind("<Configure>", self._resize_scroll_window)
        self.scroll_canvas.bind_all("<MouseWheel>", self._on_mousewheel)

        self.telemetry_label = self._row("Telemetry", "Waiting for data…", Theme.MUTED)
        self.protection_label = self._row("Protection", "No active protection event", Theme.GOOD)
        self.command_label = self._row("Last command", "--", Theme.MUTED)
        self.ack_label = self._row("Safety ACK", "--", Theme.MUTED)
        self.waveform_label = self._row("Waveform", "No capture requested yet", Theme.MUTED)
        self.activity_label = self._row("Latest activity", "Dashboard ready", Theme.ACCENT)

        help_box = tk.Frame(self.rows_container, bg=Theme.CARD, highlightbackground=Theme.BORDER, highlightthickness=1)
        help_box.pack(fill="x", pady=(8, 0), padx=(0, 4))
        tk.Label(help_box, text="Protection flow", bg=Theme.CARD, fg=Theme.TEXT,
                 font=("Segoe UI", 10, "bold")).pack(anchor="w", padx=10, pady=(8, 2))
        msg = (
            "ADE7953 interrupt → relay opens → event is published. "
            "User reviews the load/wiring, then turns the relay ON to retry. "
            "If the fault remains, another trip will be generated."
        )
        tk.Label(help_box, text=msg, bg=Theme.CARD, fg=Theme.MUTED, justify="left",
                 wraplength=520, font=("Segoe UI", 8)).pack(anchor="w", padx=10, pady=(0, 10))

    def _update_scrollregion(self, _event: Optional[tk.Event] = None) -> None:
        self.scroll_canvas.configure(scrollregion=self.scroll_canvas.bbox("all"))

    def _resize_scroll_window(self, event: tk.Event) -> None:
        self.scroll_canvas.itemconfigure(self.canvas_window, width=event.width)

    def _on_mousewheel(self, event: tk.Event) -> None:
        # Only scroll when pointer is over the System Status panel.
        widget = self.winfo_containing(event.x_root, event.y_root)
        if widget is not None and (widget == self or str(widget).startswith(str(self))):
            self.scroll_canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")

    def _row(self, title: str, value: str, color: str) -> tk.Label:
        box = tk.Frame(self.rows_container, bg=Theme.CARD, highlightbackground=Theme.BORDER, highlightthickness=1)
        box.pack(fill="x", pady=3, padx=(0, 4))
        tk.Label(box, text=title.upper(), bg=Theme.CARD, fg=Theme.MUTED,
                 font=("Segoe UI", 7, "bold")).pack(anchor="w", padx=10, pady=(6, 0))
        label = tk.Label(box, text=value, bg=Theme.CARD, fg=color,
                         font=("Segoe UI", 8, "bold"), justify="left", anchor="w", height=1)
        label.pack(anchor="w", fill="x", padx=10, pady=(1, 6))
        return label

    def _shorten(self, text: str, max_chars: int = 92) -> str:
        return text if len(text) <= max_chars else text[:max_chars - 1] + "…"

    def set_telemetry_state(self, text: str, color: str) -> None:
        self.telemetry_label.configure(text=self._shorten(text), fg=color)

    def set_protection(self, event: CriticalEvent) -> None:
        v = safe_float(event.data.get("voltage_vrms"))
        i = safe_float(event.data.get("current_a_arms"))
        self.protection_label.configure(
            text=self._shorten(f"{event.cause} · relay opened · V={v:.1f} V RMS · I={i:.3f} A RMS"),
            fg=Theme.WARN,
        )

    def clear_protection_after_retry(self) -> None:
        self.protection_label.configure(text="Relay ON after user inspection", fg=Theme.ACCENT)

    def set_command(self, text: str) -> None:
        self.command_label.configure(text=self._shorten(text), fg=Theme.PURPLE)

    def set_ack(self, ack: SafetyLimitAck) -> None:
        color = Theme.GOOD if ack.accepted else Theme.BAD
        msg = f"OK · Vmax={ack.max_vrms:.1f} V RMS · Imax={ack.max_iarms:.2f} A RMS" if ack.accepted else f"Rejected · {ack.reason}"
        self.ack_label.configure(text=self._shorten(msg), fg=color)

    def set_waveform(self, text: str, color: str) -> None:
        self.waveform_label.configure(text=self._shorten(text), fg=color)

    def add(self, message: str, level: str = "info") -> None:
        color = {
            "info": Theme.TEXT,
            "rx": Theme.ACCENT,
            "tx": Theme.PURPLE,
            "warning": Theme.WARN,
            "error": Theme.BAD,
            "success": Theme.GOOD,
        }.get(level, Theme.TEXT)
        self.activity_label.configure(text=self._shorten(message), fg=color)


# =============================================================================
# Main app
# =============================================================================

class SmartPlugApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title(APP_TITLE)
        configure_window_icon(self.root)
        self.root.geometry("1400x900")
        self.root.minsize(1160, 760)
        self.root.configure(bg=Theme.BG)
        try:
            self.root.state("zoomed")
        except tk.TclError:
            # Some non-Windows Tk builds do not support the zoomed state.
            pass


        self.incoming_queue: "queue.Queue[Tuple[str, str]]" = queue.Queue()
        self.router = MessageRouter(self)
        self.data_source = MqttSmartPlugDataSource(self.incoming_queue, broker=DEFAULT_BROKER, port=DEFAULT_PORT)
        self.phase = PHASE_PROVISIONING
        self.latest_telemetry = TelemetrySample()
        self.collecting_waveform = False
        self._waveform_req_id = 0
        self.last_telemetry_monotonic: Optional[float] = None
        self.device_online = False
        self._heartbeat_lost_reported = False

        self._setup_ttk_style()
        self.header = HeaderBar(self.root, self)
        self.content = tk.Frame(self.root, bg=Theme.BG)
        self.content.pack(expand=True, fill="both")

        self.provisioning_frame = ProvisioningFrame(self.content, self)
        self.dashboard_frame = DashboardFrame(self.content, self)

        self._bind_keys()
        self.set_phase(PHASE_PROVISIONING)
        self.data_source.start()
        self.root.after(100, self._process_incoming_queue)
        self.root.after(DEVICE_HEARTBEAT_CHECK_MS, self._check_device_heartbeat)
        self.log(f"GUI initialized. Connecting to MQTT broker {DEFAULT_BROKER}:{DEFAULT_PORT}.", level="success")

    def _setup_ttk_style(self) -> None:
        style = ttk.Style()
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure("TCombobox", fieldbackground=Theme.CARD, background=Theme.CARD,
                        foreground=Theme.TEXT, arrowcolor=Theme.TEXT)
        style.map("TCombobox", fieldbackground=[("readonly", Theme.CARD)], foreground=[("readonly", Theme.TEXT)])
        style.configure("Horizontal.TProgressbar", troughcolor=Theme.CARD, background=Theme.ACCENT,
                        bordercolor=Theme.BORDER, lightcolor=Theme.ACCENT, darkcolor=Theme.ACCENT)

    def _bind_keys(self) -> None:
        self.root.bind("<F1>", lambda _e: self.set_phase(PHASE_PROVISIONING))
        self.root.bind("<F2>", lambda _e: self.set_phase(PHASE_DASHBOARD))
        self.root.bind("<F3>", lambda _e: self.set_phase(PHASE_RECONNECTING))

    def set_phase(self, phase: str) -> None:
        self.phase = phase
        self.header.set_phase(phase)
        self.provisioning_frame.pack_forget()
        self.dashboard_frame.pack_forget()
        if phase == PHASE_DASHBOARD:
            self.dashboard_frame.pack(expand=True, fill="both")
            self.log("Dashboard phase active. Receiving live MQTT telemetry.", level="success")
        elif phase == PHASE_RECONNECTING:
            self.provisioning_frame.pack(expand=True, fill="both")
            self.provisioning_frame.status_label.configure(
                text="MQTT is disconnected or not responding yet. If credentials were already sent, wait here while the Smart Plug finishes restarting and reconnecting to the broker.",
                fg=Theme.WARN,
            )
            self.provisioning_frame.small_note.configure(text="Status: waiting for MQTT reconnection. The GUI will return to the dashboard when smartplug/telemetry/status is received.")
            self.log("Waiting for MQTT reconnection.", level="warning")
        else:
            self.provisioning_frame.pack(expand=True, fill="both")
            self.provisioning_frame.status_label.configure(
                text="No MQTT connection detected. You can send credentials over BLE or wait for reconnection.",
                fg=Theme.MUTED,
            )
            self.provisioning_frame.small_note.configure(text="Status: waiting for user action or MQTT device telemetry.")
            self.log("BLE provisioning phase active.", level="info")

    def _process_incoming_queue(self) -> None:
        try:
            processed = 0
            while processed < 50:
                topic, payload = self.incoming_queue.get_nowait()
                self.router.route(topic, payload)
                processed += 1
        except queue.Empty:
            pass
        finally:
            self.root.after(100, self._process_incoming_queue)

    def _check_device_heartbeat(self) -> None:
        """Return to the provisioning/reconnection screen if device telemetry stops.

        This watchdog intentionally does not depend on the MQTT client's
        disconnect callback. The PC can remain connected to Mosquitto while the
        ESP32 is powered off, rebooting or in BLE pairing mode. The device is
        considered online only while fresh smartplug/telemetry/status packets
        keep arriving.
        """
        try:
            if self.phase == PHASE_DASHBOARD and not self.collecting_waveform and self.last_telemetry_monotonic is not None:
                elapsed_s = time.monotonic() - self.last_telemetry_monotonic
                if elapsed_s > DEVICE_HEARTBEAT_TIMEOUT_S:
                    self.device_online = False
                    if not self._heartbeat_lost_reported:
                        self._heartbeat_lost_reported = True
                        self.log(
                            f"Device heartbeat lost: no telemetry for {elapsed_s:.1f} s. Returning to provisioning/reconnection screen.",
                            level="warning",
                        )
                    self.set_phase(PHASE_RECONNECTING)
                    self.provisioning_frame.status_label.configure(
                        text=(
                            "Smart Plug telemetry was lost. If the device is in pairing mode, send credentials over BLE; "
                            "otherwise keep the broker running and wait for MQTT telemetry to return."
                        ),
                        fg=Theme.WARN,
                    )
                    self.provisioning_frame.small_note.configure(
                        text=(
                            f"Status: no smartplug/telemetry/status for more than {DEVICE_HEARTBEAT_TIMEOUT_S:.0f} s. "
                            "The GUI will return to the main dashboard when telemetry is received."
                        )
                    )
        finally:
            self.root.after(DEVICE_HEARTBEAT_CHECK_MS, self._check_device_heartbeat)

    # Incoming handlers --------------------------------------------------------
    def on_telemetry(self, sample: TelemetrySample) -> None:
        self.latest_telemetry = sample
        self.last_telemetry_monotonic = time.monotonic()
        self.device_online = True
        self._heartbeat_lost_reported = False
        if self.phase in (PHASE_RECONNECTING, PHASE_PROVISIONING):
            self.set_phase(PHASE_DASHBOARD)
        if self.phase == PHASE_DASHBOARD:
            self.dashboard_frame.update_telemetry(sample)

    def on_critical_event(self, event: CriticalEvent) -> None:
        if self.phase == PHASE_DASHBOARD:
            self.dashboard_frame.relay_panel.set_protection(event)
            self.dashboard_frame.status_panel.set_protection(event)
        self.log(f"Protection trip: {event.cause}. Relay opened by ADE7953 interrupt.", level="error")

    def on_relay_update(self, relay: bool, accepted: bool, reason: str = "") -> None:
        if self.phase == PHASE_DASHBOARD:
            self.dashboard_frame.relay_panel.update_relay(relay)
            if relay and reason == "manual_reclose_after_protection":
                self.dashboard_frame.status_panel.clear_protection_after_retry()
        level = "success" if accepted else "warning"
        self.log(f"Relay {'ON' if relay else 'OFF'} · {reason}", level=level)

    def on_temperature_update(self, temperature_c: float) -> None:
        self.log(f"Temperature packet received: {temperature_c:.1f} °C", level="rx")

    def on_energy_packet(self, data: Dict[str, Any]) -> None:
        energy_wh = safe_float(data.get("energy_wh", data.get("energy")))
        active_power = safe_float(data.get("active_power", data.get("power")))
        self.log(f"Energy update: {energy_wh:.3f} Wh · P={active_power:.1f} W", level="rx")

    def on_command_ack(self, ack: SafetyLimitAck) -> None:
        level = "success" if ack.accepted else "warning"
        event_type = ack.event_type or ack.command or ack.action or "COMMAND_ACK"

        if event_type == "SAFETY_LIMITS_UPDATE" or ack.max_vrms > 0 or ack.max_iarms > 0:
            self.dashboard_frame.safety_panel.update_ack(ack)
            self.dashboard_frame.status_panel.set_ack(ack)
            self.log(f"Safety ACK: {ack.reason}", level=level)
        elif event_type == "WAVEFORM_REQUEST":
            if not ack.accepted:
                self.collecting_waveform = False
                self.dashboard_frame.set_collecting(False)
                self.dashboard_frame.status_panel.set_waveform(f"Waveform rejected: {ack.reason}", Theme.BAD)
            self.log(f"Waveform ACK: {ack.reason}", level=level)
        elif event_type == "RELAY_COMMAND":
            self.log(f"Relay command ACK: {ack.reason}", level=level)
        else:
            self.log(f"Command ACK {event_type}: {ack.reason}", level=level)

    def on_safety_ack(self, ack: SafetyLimitAck) -> None:
        self.on_command_ack(ack)

    def on_mqtt_connection_event(self, data: Dict[str, Any]) -> None:
        connected = bool_from_any(data.get("connected"))
        broker = data.get("broker", DEFAULT_BROKER)
        port = data.get("port", DEFAULT_PORT)
        if connected:
            self.header.connection_badge.configure(text=f"MQTT: CONNECTED {broker}:{port}", fg=Theme.GOOD)
            self.provisioning_frame.small_note.configure(text="Status: broker connected. Waiting for device telemetry…")
            if self.phase == PHASE_PROVISIONING:
                self.set_phase(PHASE_RECONNECTING)
            self.log(f"Connected to MQTT broker {broker}:{port}. Waiting for Smart Plug telemetry.", level="success")
        else:
            error = data.get("error", "")
            self.header.connection_badge.configure(text="MQTT: DISCONNECTED", fg=Theme.BAD)
            self.provisioning_frame.small_note.configure(text=f"Status: MQTT disconnected. {error}".strip())
            if self.phase == PHASE_DASHBOARD:
                self.set_phase(PHASE_RECONNECTING)
            self.log(f"MQTT disconnected from {broker}:{port}. {error}", level="warning")

    def on_ble_result(self, data: Dict[str, Any]) -> None:
        ok = bool_from_any(data.get("ok"))
        if ok:
            broker = str(data.get("broker_ip", DEFAULT_BROKER))
            port = safe_int(data.get("broker_port", DEFAULT_PORT), DEFAULT_PORT)
            self.provisioning_frame.status_label.configure(
                text="Credentials sent over BLE. Waiting for the ESP32 to reconnect through WiFi/MQTT.",
                fg=Theme.ACCENT,
            )
            self.provisioning_frame.small_note.configure(text=f"Status: reconnecting GUI MQTT client to {broker}:{port}…")
            self.data_source.reconnect(broker, port)
            self.set_phase(PHASE_RECONNECTING)
            self.log(f"BLE provisioning completed. Reconnecting MQTT client to {broker}:{port}.", level="success")
        else:
            err = str(data.get("error", "unknown BLE provisioning error"))
            self.provisioning_frame.status_label.configure(text=f"BLE provisioning failed: {err}", fg=Theme.BAD)
            self.provisioning_frame.small_note.configure(text="Status: verify ESP32 BLE advertising, MAC address and Windows Bluetooth permissions.")
            self.log(f"BLE provisioning failed: {err}", level="error")

    def send_ble_credentials(self, ssid: str, password: str, broker_ip: str, broker_port: int, mac: str) -> None:
        def worker() -> None:
            try:
                result = send_credentials_sync(
                    ssid=ssid,
                    password=password,
                    target_mac=mac,
                    broker_ip=broker_ip,
                    broker_port=broker_port,
                    include_mqtt_fields=False,
                )
                result.update({"ok": True})
                self.incoming_queue.put((SPECIAL_TOPIC_BLE_RESULT, json.dumps(result)))
            except ProvisioningError as exc:
                self.incoming_queue.put((SPECIAL_TOPIC_BLE_RESULT, json.dumps({"ok": False, "error": str(exc)})))
            except Exception as exc:
                self.incoming_queue.put((SPECIAL_TOPIC_BLE_RESULT, json.dumps({"ok": False, "error": str(exc)})))
        threading.Thread(target=worker, daemon=True).start()
        self.log(f"BLE provisioning started: ssid={ssid} broker={broker_ip}:{broker_port} mac={mac}", level="tx")

    def on_waveform_packet(self, packet: WaveformPacket) -> None:
        self.collecting_waveform = False
        self.dashboard_frame.set_collecting(False)
        self.dashboard_frame.waveform_panel.update_packet(packet)
        self.dashboard_frame.status_panel.set_waveform(
            f"Waveform OK · {len(packet.voltage_samples)} samples · {1000*packet.duration_s:.2f} ms · THD_V={100*packet.thd_voltage:.2f}%",
            Theme.GOOD,
        )
        self.log("Waveform received; FFT/THD updated.", level="success")

    # Outgoing commands --------------------------------------------------------
    def publish_relay_command(self, turn_on: bool) -> None:
        payload = json.dumps({"command": "RELAY_ON" if turn_on else "RELAY_OFF", "relay": bool(turn_on), "source": "GUI", "timestamp": now_str()})
        self._publish_to_device(TOPIC_RELAY_CMD, payload, lambda: self.data_source.publish_relay(turn_on))

    def publish_safety_limits(self, max_vrms: float, max_iarms: float) -> None:
        payload = json.dumps({"command": "SET_SAFETY_LIMITS", "max_vrms": max_vrms, "max_iarms": max_iarms, "source": "GUI", "timestamp": now_str()})
        self._publish_to_device(TOPIC_SAFETY_CMD, payload, lambda: self.data_source.publish_safety_limits(max_vrms, max_iarms))

    def publish_waveform_request(self) -> None:
        if self.collecting_waveform:
            self.log("Waveform request ignored: capture already in progress.", level="warning")
            return
        payload = json.dumps({"command": "REQUEST_WAVEFORM", "sample_count": WAVEFORM_SAMPLE_COUNT, "sampling_rate_hz": SAMPLE_RATE_HZ, "source": "GUI", "timestamp": now_str()})
        ok = self._publish_to_device(TOPIC_WAVEFORM_CMD, payload, self.data_source.request_waveform_capture)
        if ok:
            self.collecting_waveform = True
            self.dashboard_frame.set_collecting(True, WAVEFORM_CAPTURE_SECONDS)
            self.dashboard_frame.status_panel.set_waveform(f"Collecting {WAVEFORM_SAMPLE_COUNT} samples at {SAMPLE_RATE_HZ} Hz…", Theme.WARN)
            
            # --- NEW: 2-second timeout condition ---
            self._waveform_req_id += 1
            current_req = self._waveform_req_id
            self.root.after(2000, lambda: self._check_waveform_timeout(current_req))

    def _check_waveform_timeout(self, req_id: int) -> None:
        # If we are still marked as collecting and the request ID matches, it means we never got the packet.
        if self.collecting_waveform and self._waveform_req_id == req_id:
            self.collecting_waveform = False
            self.dashboard_frame.set_collecting(False)
            self.dashboard_frame.status_panel.set_waveform("Waveform request timed out (2s).", Theme.BAD)
            self.log("Waveform request timed out after 2 seconds without response.", level="error")

    def _publish_to_device(self, topic: str, payload: str, publish_fn: Callable[[], bool]) -> bool:
        try:
            ok = publish_fn()
        except Exception as exc:
            self.log(f"TX failed on {topic}: {exc}", level="error")
            ok = False

        if hasattr(self, "dashboard_frame"):
            try:
                display_payload = "request 512-sample waveform" if topic == TOPIC_WAVEFORM_CMD else payload
                self.dashboard_frame.status_panel.set_command(f"{topic} → {display_payload}")
            except Exception:
                pass

        if ok:
            self.log(f"TX {topic}: {payload}", level="tx")
        else:
            self.log(f"TX not sent; MQTT is not connected: {topic}", level="error")
        return ok

    def log(self, message: str, level: str = "info") -> None:
        # Keep terminal trace for development, but GUI shows only clean status labels.
        print(f"[{datetime.now().strftime('%H:%M:%S')}] {level.upper()}: {message}")
        try:
            self.dashboard_frame.status_panel.add(message, level=level)
        except Exception:
            pass


    def save_metrics_csv(self) -> None:
        sample = self.latest_telemetry
        if not sample.timestamp:
            messagebox.showinfo("Save metrics CSV", "No telemetry sample has been received yet.")
            return
        filename = filedialog.asksaveasfilename(
            title="Save main metrics CSV",
            defaultextension=".csv",
            filetypes=[("CSV files", "*.csv"), ("All files", "*.*")],
            initialfile="smartplug_metrics.csv",
        )
        if not filename:
            return

        s_va, disp_pf, phi_deg = displacement_metrics_from_pq(sample.active_power, sample.reactive_power)
        save_ts = now_str()
        load_type = load_type_from_reactive_power(sample.reactive_power)
        with open(filename, "w", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            writer.writerow([
                "save_timestamp", "telemetry_timestamp", "vrms", "irms", "true_pf_including_thd",
                "active_power_w", "reactive_power_var", "apparent_power_va", "frequency_hz",
                "energy_wh", "temperature_c", "relay", "no_load", "load_type",
                "displacement_pf_from_pq", "phase_angle_deg_from_pq",
            ])
            writer.writerow([
                save_ts, sample.timestamp, sample.vrms, sample.irms, sample.pf,
                sample.active_power, sample.reactive_power, s_va, sample.frequency,
                sample.energy_wh, sample.tmp_c, sample.relay, sample.no_load, load_type,
                disp_pf, phi_deg,
            ])
        messagebox.showinfo("Save metrics CSV", f"Main metrics saved:\n{filename}")

    def on_close(self) -> None:
        self.data_source.stop()
        self.root.destroy()


# =============================================================================
# Entrypoint
# =============================================================================

if __name__ == "__main__":
    configure_windows_dpi_awareness()
    root = tk.Tk()
    configure_tk_100_percent_scaling(root)
    app = SmartPlugApp(root)
    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.mainloop()
