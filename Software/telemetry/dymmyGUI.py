"""
AYCE Smart Plug Modular Dummy GUI
---------------------------------

Modern dummy GUI prototype for the AYCE Smart Plug project.

Design goals:
    - Keep data source modular: dummy today, MQTT tomorrow.
    - Use the current firmware/MQTT topic contract as much as possible:
        RX: smartplug/status, smartplug/events, smartplug/relay,
            smartplug/temperature, smartplug/energy, aice/status
        TX: smartplug/cmd with RELAY_ON / RELAY_OFF
            aice/cmd with {"max_vrms": ..., "max_iarms": ...}
    - Include a future waveform capture flow:
        TX: smartplug/waveform/cmd
        RX: smartplug/waveform
    - Show waveform, harmonic spectrum and THD using dummy samples generated at
      2400 samples/s.

Important FFT normalization note:
    For a coherent sine sampled over an integer number of cycles, the DFT bin
    magnitude for a real sine is approximately N*A_peak/2. Therefore, this GUI
    estimates peak amplitude as 2*|X[k]|/N for harmonic bins. THD is then
    computed from peak amplitudes, which is equivalent to using RMS amplitudes
    because the sqrt(2) factor cancels in the ratio.

    With Fs=2400 Hz and 60 Hz fundamental, the 20th harmonic is exactly at the
    Nyquist limit. It is shown in the UI because it is part of the project goal,
    but in real hardware it is better to sample above 2400 Hz if the 20th
    harmonic must be measured robustly.
"""

from __future__ import annotations

import json
import math
import queue
import random
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime
from typing import Any, Callable, Dict, List, Optional, Tuple

import tkinter as tk
from tkinter import ttk, messagebox


# =============================================================================
# Constants and theme
# =============================================================================

APP_TITLE = "AYCE Smart Plug Dashboard - Dummy Mode v8"

NOMINAL_VRMS = 127.0
NOMINAL_FREQ_HZ = 60.0
NOMINAL_CURRENT_ARMS = 5.0
SAMPLE_RATE_HZ = 2400
MAX_HARMONIC_ORDER = 20

TOPIC_STATUS = "smartplug/status"
TOPIC_EVENTS = "smartplug/events"
TOPIC_RELAY = "smartplug/relay"
TOPIC_TEMPERATURE = "smartplug/temperature"
TOPIC_ENERGY = "smartplug/energy"
TOPIC_SAFETY_ACK = "aice/status"
TOPIC_RELAY_CMD = "smartplug/cmd"
TOPIC_SAFETY_CMD = "aice/cmd"
TOPIC_WAVEFORM_CMD = "smartplug/waveform/cmd"       # Proposed future topic
TOPIC_WAVEFORM_DATA = "smartplug/waveform"          # Proposed future topic

PHASE_PROVISIONING = "provisioning"
PHASE_DASHBOARD = "dashboard"
PHASE_RECONNECTING = "reconnecting"


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
        return max(0.0, self.vrms * self.irms)

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


@dataclass
class WaveformPacket:
    timestamp: str
    duration_s: int
    sampling_rate_hz: int
    voltage_samples: List[float]
    current_samples: List[float]
    fundamental_hz: float
    thd_voltage: float
    thd_current: float
    harmonics: List[Dict[str, float]]


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
    max_order: int = MAX_HARMONIC_ORDER,
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
    centered = samples
    # The waveform should be AC-centered, but subtracting a small DC offset makes
    # the harmonic amplitudes more stable if a future packet includes bias.
    mean = sum(centered) / n

    for order in range(1, max_order + 1):
        freq = order * fundamental_hz
        if freq > sampling_rate_hz / 2.0 + 1e-9:
            amps.append(0.0)
            continue
        cos_sum = 0.0
        sin_sum = 0.0
        omega = 2.0 * math.pi * freq / sampling_rate_hz
        for idx, raw in enumerate(centered):
            x = raw - mean
            angle = omega * idx
            cos_sum += x * math.cos(angle)
            sin_sum += x * math.sin(angle)
        # Normalized peak amplitude. At the exact Nyquist bin this is not ideal,
        # but the 20th harmonic at Fs=2400 Hz is included for visualization.
        amps.append((2.0 / n) * math.sqrt(cos_sum * cos_sum + sin_sum * sin_sum))
    return amps


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
) -> Tuple[float, float, List[Dict[str, float]]]:
    v_amps = dft_peak_amplitudes(voltage_samples, sampling_rate_hz, fundamental_hz)
    i_amps = dft_peak_amplitudes(current_samples, sampling_rate_hz, fundamental_hz)
    thd_v = compute_thd_from_peak_amplitudes(v_amps)
    thd_i = compute_thd_from_peak_amplitudes(i_amps)

    harmonics: List[Dict[str, float]] = []
    for order in range(1, MAX_HARMONIC_ORDER + 1):
        harmonics.append({
            "order": float(order),
            "frequency_hz": float(order * fundamental_hz),
            "voltage_mag_vpk": round(v_amps[order - 1], 5),
            "current_mag_apk": round(i_amps[order - 1], 7),
        })
    return thd_v, thd_i, harmonics


# =============================================================================
# Dummy source: replace this class with MQTT later
# =============================================================================

class DummySmartPlugDataSource:
    def __init__(self, out_queue: "queue.Queue[Tuple[str, str]]"):
        self.out_queue = out_queue
        self.running = False
        self.thread: Optional[threading.Thread] = None

        self.relay_state = True
        self.system_locked = False
        self.telemetry_paused = False

        self.energy_wh = 0.0
        self.temperature_c = 31.0
        self.max_vrms = 135.0
        self.max_iarms = 5.0
        self.last_status: Optional[TelemetrySample] = None
        self._last_energy_time = time.time()

    def start(self) -> None:
        self.running = True
        self.thread = threading.Thread(target=self._run_loop, daemon=True)
        self.thread.start()

    def stop(self) -> None:
        self.running = False

    def _emit(self, topic: str, payload: Dict[str, Any]) -> None:
        self.out_queue.put((topic, json.dumps(payload)))

    def _run_loop(self) -> None:
        while self.running:
            if not self.telemetry_paused:
                self._emit_status_packet()

                if random.random() < 0.14:
                    self._emit_temperature_packet()
                if random.random() < 0.12:
                    self._emit_energy_packet()

                # Very low probability random protection for demo. Use header
                # dev buttons if you want deterministic testing.
                if random.random() < 0.006 and not self.system_locked:
                    self.emit_critical_event(random.choice(["OVERCURRENT", "OVERVOLTAGE"]))

            time.sleep(1.0)

    def _emit_status_packet(self) -> None:
        if self.relay_state:
            vrms = random.gauss(NOMINAL_VRMS, 1.2)
            irms = clamp(random.gauss(2.2, 0.85), 0.15, 5.25)
            pf = clamp(random.gauss(0.93, 0.035), 0.78, 0.999)
            freq = random.gauss(NOMINAL_FREQ_HZ, 0.035)
            no_load = False
        else:
            vrms = random.gauss(NOMINAL_VRMS, 1.1)
            irms = random.uniform(0.0, 0.035)
            pf = 0.0
            freq = random.gauss(NOMINAL_FREQ_HZ, 0.035)
            no_load = True

        apparent = vrms * irms
        active_power = apparent * pf
        reactive_power = math.sqrt(max(0.0, apparent * apparent - active_power * active_power))

        now = time.time()
        dt_hours = max(0.0, (now - self._last_energy_time) / 3600.0)
        self._last_energy_time = now
        self.energy_wh += active_power * dt_hours

        self.temperature_c += random.gauss(0.0, 0.04)
        self.temperature_c = clamp(self.temperature_c, 28.0, 44.0)

        packet = {
            "vrms": round(vrms, 2),
            "irms": round(irms, 3),
            "pf": round(pf, 3),
            "active_power": round(active_power, 2),
            "reactive_power": round(reactive_power, 2),
            "frequency": round(freq, 3),
            "no_load": no_load,
            "energy_wh": round(self.energy_wh, 3),
            "relay": self.relay_state,
            "tmp_c": round(self.temperature_c, 2),
            "timestamp": now_str(),
        }
        self.last_status = MessageParser.parse_telemetry(packet)
        self._emit(TOPIC_STATUS, packet)

    def _emit_temperature_packet(self) -> None:
        self._emit(TOPIC_TEMPERATURE, {
            "event_type": "TEMPERATURE",
            "temperature_c": round(self.temperature_c, 2),
            "timestamp": now_str(),
        })

    def _emit_energy_packet(self) -> None:
        if self.last_status is None:
            return
        self._emit(TOPIC_ENERGY, {
            "event_type": "ENERGY",
            "vrms": round(self.last_status.vrms, 2),
            "irms": round(self.last_status.irms, 3),
            "active_power": round(self.last_status.active_power, 2),
            "reactive_power": round(self.last_status.reactive_power, 2),
            "energy_wh": round(self.energy_wh, 3),
            "timestamp": now_str(),
        })

    def set_relay(self, turn_on: bool) -> None:
        reason = "applied"
        if turn_on and self.system_locked:
            # This models the operational flow described by the user: the ADE7953
            # interrupt opened the relay; the user manually checks the load and
            # intentionally turns the relay on again to retry. If the problem is
            # still present, another critical event will be emitted.
            self.system_locked = False
            reason = "manual_reclose_after_protection"

        self.relay_state = bool(turn_on)
        self._emit(TOPIC_RELAY, {
            "event_type": "RELAY",
            "relay": self.relay_state,
            "accepted": True,
            "reason": reason,
            "timestamp": now_str(),
        })

    def set_safety_limits(self, max_vrms: float, max_iarms: float) -> None:
        accepted = 80.0 <= max_vrms <= 160.0 and 0.1 <= max_iarms <= 10.0
        reason = "applied" if accepted else "out_of_allowed_dummy_range"
        if accepted:
            self.max_vrms = max_vrms
            self.max_iarms = max_iarms
        self._emit(TOPIC_SAFETY_ACK, {
            "event_type": "SAFETY_LIMITS_UPDATE",
            "accepted": accepted,
            "max_vrms": round(max_vrms, 2),
            "max_iarms": round(max_iarms, 3),
            "reason": reason,
            "timestamp": now_str(),
        })

    def emit_critical_event(self, cause: str = "OVERVOLTAGE") -> None:
        self.system_locked = True
        self.relay_state = False
        voltage = self.last_status.vrms if self.last_status else 142.0
        current = self.last_status.irms if self.last_status else 6.2
        if cause == "OVERVOLTAGE":
            voltage = max(voltage, self.max_vrms + random.uniform(2.0, 9.0))
        elif cause == "OVERCURRENT":
            current = max(current, self.max_iarms + random.uniform(0.4, 2.0))

        self._emit(TOPIC_EVENTS, {
            "event_type": "CRITICAL_PROTECTION",
            "cause": cause,
            "timestamp": now_str(),
            "data": {
                "voltage_vrms": round(voltage, 2),
                "current_a_arms": round(current, 3),
                "current_b_arms": 0.0,
                "duration_cycles": random.randint(2, 8),
            },
            "action_taken": "RELAY_OPEN",
            "system_status": "LOCKED_AWAITING_USER_RETRY",
        })
        self._emit(TOPIC_RELAY, {
            "event_type": "RELAY",
            "relay": False,
            "accepted": True,
            "reason": "opened_by_ade7953_interrupt",
            "timestamp": now_str(),
        })

    def request_waveform_capture(self, duration_s: int) -> None:
        duration_s = int(clamp(duration_s, 1, 5))
        if self.telemetry_paused:
            return
        self.telemetry_paused = True
        worker = threading.Thread(target=self._waveform_capture_worker, args=(duration_s,), daemon=True)
        worker.start()

    def _waveform_capture_worker(self, duration_s: int) -> None:
        time.sleep(duration_s)
        packet = self._generate_waveform_packet(duration_s)
        self._emit(TOPIC_WAVEFORM_DATA, packet)
        self.telemetry_paused = False

    def _generate_waveform_packet(self, duration_s: int) -> Dict[str, Any]:
        n = SAMPLE_RATE_HZ * duration_s
        freq = NOMINAL_FREQ_HZ
        dt = 1.0 / SAMPLE_RATE_HZ

        vrms = self.last_status.vrms if self.last_status else NOMINAL_VRMS
        irms = self.last_status.irms if self.last_status else 2.0
        pf = clamp(self.last_status.pf if self.last_status else 0.93, 0.1, 1.0)
        phi = math.acos(pf)

        v_peak = vrms * math.sqrt(2.0)
        i_peak = irms * math.sqrt(2.0)

        # Dummy harmonic content. Values are fractions of the fundamental peak.
        # We include several odd harmonics; the analysis routine can estimate up
        # to the 20th order.
        v_harmonics = {1: 1.0, 3: 0.025, 5: 0.018, 7: 0.012, 9: 0.006, 11: 0.004}
        i_harmonics = {1: 1.0, 3: 0.065, 5: 0.040, 7: 0.025, 9: 0.012, 11: 0.008, 13: 0.006}

        voltage_samples: List[float] = []
        current_samples: List[float] = []
        for k in range(n):
            t = k * dt
            v = 0.0
            i = 0.0
            for order, frac in v_harmonics.items():
                v += v_peak * frac * math.sin(2.0 * math.pi * freq * order * t)
            for order, frac in i_harmonics.items():
                phase = phi if order == 1 else phi + 0.2 * order
                i += i_peak * frac * math.sin(2.0 * math.pi * freq * order * t - phase)
            v += random.gauss(0.0, 0.25)
            i += random.gauss(0.0, 0.006)
            voltage_samples.append(round(v, 3))
            current_samples.append(round(i, 5))

        thd_v, thd_i, harmonics = compute_waveform_analysis(voltage_samples, current_samples, SAMPLE_RATE_HZ, freq)

        return {
            "event_type": "WAVEFORM_CAPTURE",
            "timestamp": now_str(),
            "duration_s": duration_s,
            "sampling_rate_hz": SAMPLE_RATE_HZ,
            "signals": {
                "voltage_v": voltage_samples,
                "current_a": current_samples,
            },
            "analysis": {
                "fundamental_hz": freq,
                "thd_voltage": round(thd_v, 5),
                "thd_current": round(thd_i, 5),
                "harmonics": harmonics,
                "fft_normalization": "peak_amplitude = 2*abs(DFT_bin)/N",
            },
        }


# =============================================================================
# Parser/router
# =============================================================================

class MessageParser:
    @staticmethod
    def parse_telemetry(data: Dict[str, Any]) -> TelemetrySample:
        return TelemetrySample(
            vrms=safe_float(data.get("vrms")),
            irms=safe_float(data.get("irms")),
            pf=safe_float(data.get("pf")),
            active_power=safe_float(data.get("active_power")),
            reactive_power=safe_float(data.get("reactive_power")),
            frequency=safe_float(data.get("frequency"), NOMINAL_FREQ_HZ),
            no_load=bool_from_any(data.get("no_load")),
            energy_wh=safe_float(data.get("energy_wh")),
            relay=bool_from_any(data.get("relay")),
            tmp_c=safe_float(data.get("tmp_c")),
            timestamp=str(data.get("timestamp", now_str())),
        )

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
        )

    @staticmethod
    def parse_waveform(data: Dict[str, Any]) -> WaveformPacket:
        signals = data.get("signals", {}) or {}
        analysis = data.get("analysis", {}) or {}
        voltage_samples = [safe_float(x) for x in signals.get("voltage_v", [])]
        current_samples = [safe_float(x) for x in signals.get("current_a", [])]
        sampling_rate = int(safe_float(data.get("sampling_rate_hz"), SAMPLE_RATE_HZ))
        fundamental = safe_float(analysis.get("fundamental_hz"), NOMINAL_FREQ_HZ)

        # Recompute on the PC side from raw samples. This is deliberate: later the
        # ESP32 can send only samples and the GUI can still produce FFT/THD.
        thd_v, thd_i, harmonics = compute_waveform_analysis(voltage_samples, current_samples, sampling_rate, fundamental)

        return WaveformPacket(
            timestamp=str(data.get("timestamp", now_str())),
            duration_s=int(safe_float(data.get("duration_s"), 0.0)),
            sampling_rate_hz=sampling_rate,
            voltage_samples=voltage_samples,
            current_samples=current_samples,
            fundamental_hz=fundamental,
            thd_voltage=thd_v,
            thd_current=thd_i,
            harmonics=harmonics,
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

        if topic == TOPIC_STATUS:
            self.app.on_telemetry(MessageParser.parse_telemetry(data))
        elif topic == TOPIC_EVENTS:
            event_type = data.get("event_type", "")
            if event_type == "CRITICAL_PROTECTION":
                self.app.on_critical_event(MessageParser.parse_event(data))
            else:
                self.app.log(f"Device event: {event_type}", level="info")
        elif topic == TOPIC_RELAY:
            relay = bool_from_any(data.get("relay"))
            accepted = bool_from_any(data.get("accepted", True))
            reason = str(data.get("reason", ""))
            self.app.on_relay_update(relay, accepted, reason)
        elif topic == TOPIC_TEMPERATURE:
            temp = safe_float(data.get("temperature_c", data.get("tmp_c")))
            self.app.on_temperature_update(temp)
        elif topic == TOPIC_ENERGY:
            self.app.on_energy_packet(data)
        elif topic == TOPIC_SAFETY_ACK:
            self.app.on_safety_ack(MessageParser.parse_safety_ack(data))
        elif topic == TOPIC_WAVEFORM_DATA:
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

        title_box = tk.Frame(self, bg=Theme.BG)
        title_box.pack(side="left", fill="x", expand=True)
        tk.Label(title_box, text="AYCE Smart Plug", bg=Theme.BG, fg=Theme.TEXT,
                 font=("Segoe UI", 22, "bold")).pack(anchor="w")
        tk.Label(title_box, text="Dummy GUI prototype · MQTT/BLE-ready architecture",
                 bg=Theme.BG, fg=Theme.MUTED, font=("Segoe UI", 10)).pack(anchor="w")

        right = tk.Frame(self, bg=Theme.BG)
        right.pack(side="right", anchor="ne")
        self.connection_badge = tk.Label(right, text="MQTT: SIMULATED", bg=Theme.PANEL_2,
                                         fg=Theme.ACCENT, font=("Segoe UI", 9, "bold"), padx=10, pady=5)
        self.connection_badge.pack(anchor="e")
        self.phase_badge = tk.Label(right, text="PHASE: --", bg=Theme.PANEL_2, fg=Theme.MUTED,
                                    font=("Segoe UI", 8, "bold"), padx=10, pady=4)
        self.phase_badge.pack(anchor="e", pady=(5, 0))

        # Always visible in dummy mode. These are intentionally small so they do
        # not dominate the production-like UI.
        dev = tk.Frame(right, bg=Theme.BG)
        dev.pack(anchor="e", pady=(6, 0))
        self._dev_btn(dev, "BLE", lambda: app.set_phase(PHASE_PROVISIONING))
        self._dev_btn(dev, "Dashboard", lambda: app.set_phase(PHASE_DASHBOARD))
        self._dev_btn(dev, "MQTT lost", lambda: app.set_phase(PHASE_RECONNECTING))
        self._dev_btn(dev, "OV trip", lambda: app.data_source.emit_critical_event("OVERVOLTAGE"))
        self._dev_btn(dev, "OC trip", lambda: app.data_source.emit_critical_event("OVERCURRENT"))

    def _dev_btn(self, parent: tk.Widget, text: str, command: Callable[[], None]) -> None:
        tk.Button(parent, text=text, bg=Theme.PANEL_2, fg=Theme.MUTED, activebackground=Theme.CARD_HOVER,
                  activeforeground=Theme.TEXT, relief="flat", font=("Segoe UI", 7), padx=6, pady=2,
                  command=command).pack(side="left", padx=(4, 0))

    def set_phase(self, phase: str) -> None:
        readable = {
            PHASE_PROVISIONING: "PHASE: PROVISIONING / BLE",
            PHASE_DASHBOARD: "PHASE: DASHBOARD",
            PHASE_RECONNECTING: "PHASE: WAITING FOR MQTT",
        }.get(phase, f"PHASE: {phase.upper()}")
        self.phase_badge.configure(text=readable)
        if phase == PHASE_DASHBOARD:
            self.connection_badge.configure(text="MQTT: CONNECTED (SIM)", fg=Theme.GOOD)
        elif phase == PHASE_RECONNECTING:
            self.connection_badge.configure(text="MQTT: RECONNECTING (SIM)", fg=Theme.WARN)
        else:
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

        tk.Label(left, text="Provisioning BLE", bg=Theme.PANEL, fg=Theme.TEXT,
                 font=("Segoe UI", 22, "bold")).pack(anchor="w")
        self.status_label = tk.Label(
            left,
            text="No MQTT connection detected. You can send credentials over BLE or wait for reconnection.",
            bg=Theme.PANEL, fg=Theme.MUTED, font=("Segoe UI", 11), wraplength=520, justify="left")
        self.status_label.pack(anchor="w", pady=(6, 18))

        form = tk.Frame(left, bg=Theme.PANEL)
        form.pack(fill="x")
        self.ssid_var = tk.StringVar(value="AYCE_HS")
        self.pass_var = tk.StringVar(value="Bake_This")
        self.broker_var = tk.StringVar(value="192.168.137.1")
        self.mac_var = tk.StringVar(value="D8:3B:DA:8A:2B:A6")

        self._form_row(form, "WiFi SSID", self.ssid_var, 0)
        self._form_row(form, "WiFi Password", self.pass_var, 1, show="*")
        self._form_row(form, "MQTT Broker IP", self.broker_var, 2)
        self._form_row(form, "ESP32 BLE MAC", self.mac_var, 3)

        btns = tk.Frame(left, bg=Theme.PANEL)
        btns.pack(fill="x", pady=(18, 0))
        tk.Button(btns, text="Send credentials over BLE (simulated)",
                  bg=Theme.ACCENT, fg=Theme.BLACK, activebackground=Theme.ACCENT_2,
                  relief="flat", font=("Segoe UI", 10, "bold"), padx=12, pady=8,
                  command=self._send_ble_dummy).pack(side="left")
        tk.Button(btns, text="View MQTT setup instructions", bg=Theme.PANEL_2, fg=Theme.TEXT,
                  activebackground=Theme.CARD_HOVER, relief="flat", padx=12, pady=8,
                  command=self._open_mqtt_instructions).pack(side="left", padx=(10, 0))

        tk.Label(right, text="What is happening", bg=Theme.PANEL, fg=Theme.TEXT,
                 font=("Segoe UI", 18, "bold")).pack(anchor="w")
        instructions = (
            "1. Energiza el Smart Plug.\n"
            "2. If it does not have credentials yet, it should advertise over BLE.\n"
            "3. The GUI sends SSID, password and broker information to the device.\n"
            "4. The ESP32 restarts WiFi and stops using BLE.\n"
            "5. Once MQTT connects, the GUI switches to the main dashboard.\n\n"
            "In this dummy version, use the small test buttons in the upper-right corner."
        )
        tk.Label(right, text=instructions, bg=Theme.PANEL, fg=Theme.MUTED,
                 font=("Segoe UI", 11), justify="left", wraplength=520).pack(anchor="w", pady=(10, 18))

        self.progress = ttk.Progressbar(right, mode="indeterminate")
        self.progress.pack(fill="x", pady=(6, 16))
        self.progress.start(14)

        self.small_note = tk.Label(right, text="Status: waiting for user action or simulated MQTT reconnection.",
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

    def _send_ble_dummy(self) -> None:
        self.credentials_sent = True
        self.status_label.configure(
            text="Credentials sent over BLE in simulated mode. The Smart Plug should restart and attempt to connect to the MQTT broker.",
            fg=Theme.ACCENT,
        )
        self.small_note.configure(text="Status: credentials sent. Waiting for simulated MQTT connection…")
        self.app.log(f"BLE provisioning dummy: ssid={self.ssid_var.get()} broker={self.broker_var.get()}", level="tx")
        self.app.set_phase(PHASE_RECONNECTING)

    def _open_mqtt_instructions(self) -> None:
        win = tk.Toplevel(self)
        win.title("MQTT Setup Instructions")
        win.geometry("760x560")
        win.configure(bg=Theme.BG)
        tk.Label(win, text="Basic MQTT setup for testing",
                 bg=Theme.BG, fg=Theme.TEXT, font=("Segoe UI", 18, "bold")).pack(anchor="w", padx=20, pady=(18, 8))
        text = tk.Text(win, bg=Theme.PANEL, fg=Theme.TEXT, insertbackground=Theme.TEXT,
                       relief="flat", wrap="word", font=("Consolas", 10), padx=14, pady=14)
        text.pack(expand=True, fill="both", padx=20, pady=(0, 20))
        instructions = """
Preliminary guide for PC / local broker

1) Install Python dependencies:
   pip install bleak==0.21.1 paho-mqtt==1.6.1

2) Install Mosquitto MQTT broker.

3) Minimum Mosquitto configuration for local testing:
   listener 1883
   allow_anonymous true

4) Verify that the broker is listening on the IP address sent over BLE.
   En muchos hotspots de Windows puede usarse una IP como 192.168.137.1.

5) Expected flow:
   GUI -> BLE -> ESP32: WiFi / broker credentials
   ESP32: restarts WiFi, stops BLE and connects MQTT
   ESP32 -> MQTT -> GUI: smartplug/status

Note:
These instructions are preliminary. Later, the setup should include OS-specific steps
and troubleshooting for firewall, port 1883 and network issues.
""".strip()
        text.insert("1.0", instructions)
        text.configure(state="disabled")


class DashboardFrame(tk.Frame):
    def __init__(self, parent: tk.Widget, app: "SmartPlugApp"):
        super().__init__(parent, bg=Theme.BG)
        self.app = app
        self.current_telemetry = TelemetrySample()

        main = tk.Frame(self, bg=Theme.BG)
        main.pack(expand=True, fill="both", padx=18, pady=(2, 8))
        # Two-column dashboard: left side for live controls/waveform,
        # right side for power triangle + system status. Keeping these widgets
        # in the same right column guarantees equal width.
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
        self.waveform_panel.grid(row=2, column=0, sticky="nsew", padx=(0, 10))
        self.status_panel = StatusActivityPanel(main)
        self.status_panel.grid(row=2, column=1, sticky="nsew", padx=(10, 0))

    def _build_status_cards(self, parent: tk.Widget) -> None:
        cards_frame = tk.Frame(parent, bg=Theme.BG)
        cards_frame.grid(row=0, column=0, sticky="nsew", padx=(0, 10), pady=(0, 6))
        for col in range(5):
            cards_frame.grid_columnconfigure(col, weight=1, uniform="status_cards")

        self.card_v = Card(cards_frame, "Voltage", "--", "V RMS", f"Nominal: {NOMINAL_VRMS:.0f} V RMS", Theme.ACCENT)
        self.card_i = Card(cards_frame, "Current", "--", "A RMS", f"Nominal limit: {NOMINAL_CURRENT_ARMS:.0f} A RMS", Theme.PURPLE)
        self.card_p = Card(cards_frame, "Active Power", "--", "W", "Real power", Theme.GOOD)
        self.card_q = Card(cards_frame, "Reactive Power", "--", "var", "Quadrature power", Theme.WARN)
        self.card_s = Card(cards_frame, "Apparent Power", "--", "VA", "Vrms × Irms", Theme.ACCENT_2)
        self.card_pf = Card(cards_frame, "Power Factor", "--", "", "cos φ", Theme.WARN)
        self.card_f = Card(cards_frame, "Frequency", "--", "Hz", "Mexico grid: 60 Hz", Theme.ACCENT_2)
        self.card_e = Card(cards_frame, "Energy", "--", "Wh", "Accumulated", Theme.PINK)
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

        self.card_v.set(f"{sample.vrms:.1f}", "V RMS", f"Δ vs 127 V: {sample.vrms - NOMINAL_VRMS:+.1f} V", v_accent)
        self.card_i.set(f"{sample.irms:.3f}", "A RMS", f"{sample.current_percent_nominal:.0f}% of 5 A RMS nominal", i_accent)
        self.card_p.set(f"{sample.active_power:.1f}", "W", "Real power delivered", Theme.GOOD)
        self.card_q.set(f"{sample.reactive_power:.1f}", "var", "Reactive component", Theme.WARN)
        self.card_s.set(f"{sample.apparent_power_va:.1f}", "VA", "Calculated as Vrms × Irms", Theme.ACCENT_2)
        self.card_pf.set(f"{sample.pf:.3f}", "", "High is better for real power transfer", pf_accent)
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
        self.status_panel.set_telemetry_state("Receiving smartplug/status", Theme.GOOD)

    def set_collecting(self, collecting: bool, duration_s: int = 0) -> None:
        self.waveform_panel.set_collecting(collecting, duration_s)
        if collecting:
            self.status_panel.set_telemetry_state("Base telemetry paused for waveform capture", Theme.WARN)
        else:
            self.status_panel.set_telemetry_state("Receiving smartplug/status", Theme.GOOD)


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
        if is_on:
            self.notice_label.configure(text="Load energized. ADE7953 protections remain active.", fg=Theme.GOOD)
        else:
            self.notice_label.configure(text="Relay open. Load is not energized from the smart plug.", fg=Theme.SUBTLE)

    def set_protection(self, event: CriticalEvent) -> None:
        self.state_label.configure(text="Relay: OFF", fg=Theme.BAD)
        self.notice_label.configure(
            text=f"Protection trip: {event.cause}. Relay opened by ADE7953 interrupt. Inspect load/wiring, then TURN ON to retry.",
            fg=Theme.WARN,
        )

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
        super().__init__(parent, bg=Theme.PANEL, highlightbackground=Theme.BORDER, highlightthickness=1, padx=16, pady=14)
        tk.Label(self, text="Power Triangle", bg=Theme.PANEL, fg=Theme.TEXT,
                 font=("Segoe UI", 16, "bold")).pack(anchor="w")
        tk.Label(self, text="P, Q, S and power factor visualization", bg=Theme.PANEL,
                 fg=Theme.MUTED, font=("Segoe UI", 9)).pack(anchor="w", pady=(0, 8))
        self.canvas = tk.Canvas(self, height=205, bg=Theme.PANEL, highlightthickness=0)
        self.canvas.pack(fill="both", expand=True)
        self.summary = tk.Label(self, text="Waiting for telemetry…", bg=Theme.PANEL,
                                fg=Theme.MUTED, font=("Segoe UI", 10), justify="left")
        self.summary.pack(anchor="w", pady=(6, 0))

    def update_triangle(self, sample: TelemetrySample) -> None:
        c = self.canvas
        c.delete("all")
        w = max(360, c.winfo_width())
        h = max(210, c.winfo_height())
        origin_x = 48
        base_y = h - 38
        max_w = w - 120
        max_h = h - 86

        P = abs(sample.active_power)
        Q = abs(sample.reactive_power)
        S = max(sample.apparent_power_va, math.sqrt(P * P + Q * Q), 1.0)
        scale = min(max_w / max(P, 1.0), max_h / max(Q, 1.0)) if Q > 1 else max_w / max(P, 1.0)
        scale = min(scale, 1.0) if S > 500 else scale
        p_px = min(max_w, max(40, P * scale))
        q_px = min(max_h, max(20, Q * scale)) if Q > 1 else 0

        x2 = origin_x + p_px
        y2 = base_y
        x3 = x2
        y3 = base_y - q_px

        c.create_line(origin_x - 10, base_y, w - 24, base_y, fill=Theme.BORDER, width=2)
        c.create_line(origin_x, base_y + 10, origin_x, 24, fill=Theme.BORDER, width=2)
        c.create_text(w - 28, base_y - 12, text="P", fill=Theme.GOOD, font=("Segoe UI", 10, "bold"))
        c.create_text(origin_x + 16, 24, text="Q", fill=Theme.WARN, font=("Segoe UI", 10, "bold"))

        c.create_polygon(origin_x, base_y, x2, y2, x3, y3, fill="#16243a", outline="")
        c.create_line(origin_x, base_y, x2, y2, fill=Theme.GOOD, width=4)
        c.create_line(x2, y2, x3, y3, fill=Theme.WARN, width=4)
        c.create_line(origin_x, base_y, x3, y3, fill=Theme.ACCENT, width=4)

        c.create_text((origin_x + x2) / 2, base_y + 18, text=f"P={P:.1f} W", fill=Theme.GOOD,
                      font=("Segoe UI", 9, "bold"))

        # Q label is vertical so it remains readable without overflowing the
        # right edge when active power is high.
        q_text = f"Q={Q:.1f} var"
        q_label_x = min(w - 28, x3 + 22)
        c.create_text(q_label_x, (y2 + y3) / 2, angle=90, text=q_text, fill=Theme.WARN,
                      font=("Segoe UI", 9, "bold"))

        # S label is offset away from the hypotenuse and placed on a small
        # background tag so the line does not cross the text.
        s_text = f"S≈{S:.1f} VA"
        s_x = (origin_x + x3) / 2 - 14
        s_y = (base_y + y3) / 2 - 24
        bbox_pad_x, bbox_pad_y = 6, 3
        approx_w = 7 * len(s_text)
        c.create_rectangle(s_x - approx_w/2 - bbox_pad_x, s_y - 9 - bbox_pad_y,
                           s_x + approx_w/2 + bbox_pad_x, s_y + 9 + bbox_pad_y,
                           fill=Theme.PANEL, outline=Theme.BORDER)
        c.create_text(s_x, s_y, text=s_text, fill=Theme.ACCENT, font=("Segoe UI", 9, "bold"))

        angle_deg = math.degrees(math.acos(clamp(sample.pf, -1.0, 1.0))) if sample.pf > 0 else 0.0
        self.summary.configure(text=f"PF={sample.pf:.3f} · φ≈{angle_deg:.1f}° · P={P:.1f} W · Q={Q:.1f} var · S≈{S:.1f} VA")


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
        self.signal_var = tk.StringVar(value="Voltage")
        self.duration_var = tk.StringVar(value="2")
        self.scroll_var = tk.IntVar(value=0)

        header = tk.Frame(self, bg=Theme.PANEL)
        header.pack(fill="x")
        tk.Label(header, text="Waveform Capture · FFT · THD", bg=Theme.PANEL, fg=Theme.TEXT,
                 font=("Segoe UI", 14, "bold")).pack(side="left")
        controls = tk.Frame(header, bg=Theme.PANEL)
        controls.pack(side="right")
        tk.Label(controls, text="Duration", bg=Theme.PANEL, fg=Theme.MUTED,
                 font=("Segoe UI", 9)).pack(side="left", padx=(0, 4))
        ttk.Combobox(controls, textvariable=self.duration_var, values=["1", "2", "3", "4", "5"],
                     state="readonly", width=4).pack(side="left")
        tk.Label(controls, text="s", bg=Theme.PANEL, fg=Theme.MUTED).pack(side="left", padx=(3, 8))
        tk.Button(controls, text="Request waveform", bg=Theme.ACCENT, fg=Theme.BLACK,
                  activebackground=Theme.ACCENT_2, relief="flat", padx=10, pady=5,
                  font=("Segoe UI", 9, "bold"), command=self._request_waveform).pack(side="left")

        self.status_label = tk.Label(self,
            text="No waveform capture yet. Request a 1–5 s capture to visualize instantaneous samples, harmonic content and THD.",
            bg=Theme.PANEL, fg=Theme.MUTED, font=("Segoe UI", 10), anchor="w", justify="left")
        self.status_label.pack(fill="x", pady=(4, 5))

        strip = tk.Frame(self, bg=Theme.PANEL)
        strip.pack(fill="x", pady=(0, 6))
        for col in range(4):
            strip.grid_columnconfigure(col, weight=1)
        self.thd_v_metric = MiniMetric(strip, "THD voltage", Theme.ACCENT)
        self.thd_i_metric = MiniMetric(strip, "THD current", Theme.PURPLE)
        self.samples_metric = MiniMetric(strip, "Samples", Theme.GOOD)
        self.fs_metric = MiniMetric(strip, "Sampling", Theme.WARN)
        for idx, metric in enumerate([self.thd_v_metric, self.thd_i_metric, self.samples_metric, self.fs_metric]):
            metric.grid(row=0, column=idx, sticky="ew", padx=(0 if idx == 0 else 6, 0))

        body = tk.Frame(self, bg=Theme.PANEL)
        body.pack(expand=True, fill="both")
        body.grid_columnconfigure(0, weight=3)
        body.grid_columnconfigure(1, weight=2)
        body.grid_rowconfigure(0, weight=1)

        wave_box = tk.Frame(body, bg=Theme.CARD, highlightbackground=Theme.BORDER, highlightthickness=1)
        wave_box.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        wave_box.grid_columnconfigure(0, weight=1)
        wave_box.grid_rowconfigure(2, weight=1)

        wave_header = tk.Frame(wave_box, bg=Theme.CARD)
        wave_header.grid(row=0, column=0, sticky="ew", padx=10, pady=(6, 1))
        tk.Label(wave_header, text="Instantaneous waveform", bg=Theme.CARD, fg=Theme.TEXT,
                 font=("Segoe UI", 10, "bold")).pack(side="left")
        ttk.Combobox(wave_header, textvariable=self.signal_var, values=["Voltage", "Current"],
                     state="readonly", width=10).pack(side="right")
        self.signal_var.trace_add("write", lambda *_: (self.draw_waveform(), self.draw_fft()))

        # The scroll control is intentionally above the plot so it remains
        # visible on 1080p displays even when the bottom of the window is close
        # to the taskbar. It moves the plotted time window across the buffer.
        scroll_frame = tk.Frame(wave_box, bg=Theme.CARD)
        scroll_frame.grid(row=1, column=0, sticky="ew", padx=10, pady=(0, 2))
        tk.Label(scroll_frame, text="Waveform window", bg=Theme.CARD, fg=Theme.MUTED,
                 font=("Segoe UI", 8, "bold")).pack(side="left", padx=(0, 8))
        self.scroll = tk.Scale(scroll_frame, from_=0, to=0, orient="horizontal", variable=self.scroll_var,
                               bg=Theme.CARD, fg=Theme.MUTED, troughcolor=Theme.ACCENT,
                               activebackground=Theme.ACCENT_2, highlightthickness=0,
                               showvalue=False, length=360, sliderlength=20, width=8,
                               state="disabled", font=("Segoe UI", 7), command=lambda _v: self.draw_waveform())
        self.scroll.pack(side="left", fill="x", expand=True)
        self.scroll_value_label = tk.Label(scroll_frame, text="0.000 s", bg=Theme.CARD, fg=Theme.MUTED,
                                           font=("Segoe UI", 8, "bold"), width=9, anchor="e")
        self.scroll_value_label.pack(side="left", padx=(8, 0))

        self.wave_canvas = tk.Canvas(wave_box, bg=Theme.CARD, highlightthickness=0, height=190)
        self.wave_canvas.grid(row=2, column=0, sticky="nsew", padx=8, pady=(1, 6))

        fft_box = tk.Frame(body, bg=Theme.CARD, highlightbackground=Theme.BORDER, highlightthickness=1)
        fft_box.grid(row=0, column=1, sticky="nsew")
        fft_header = tk.Frame(fft_box, bg=Theme.CARD)
        fft_header.pack(fill="x", padx=10, pady=(6, 1))
        tk.Label(fft_header, text="Harmonic spectrum / FFT", bg=Theme.CARD, fg=Theme.TEXT,
                 font=("Segoe UI", 10, "bold")).pack(side="left")
        tk.Button(fft_header, text="FFT table", bg=Theme.PANEL_2, fg=Theme.TEXT,
                  activebackground=Theme.BORDER, relief="flat", padx=8, pady=2,
                  font=("Segoe UI", 8, "bold"), command=self.show_fft_table).pack(side="right")
        self.fft_canvas = tk.Canvas(fft_box, bg=Theme.CARD, highlightthickness=0, height=205)
        self.fft_canvas.pack(fill="both", expand=True, padx=8, pady=(2, 6))

        self.after(500, self._draw_placeholders)

    def _request_waveform(self) -> None:
        try:
            duration = int(self.duration_var.get())
        except ValueError:
            duration = 2
        self.app.publish_waveform_request(int(clamp(duration, 1, 5)))

    def _draw_placeholders(self) -> None:
        if self.packet is None:
            self.wave_canvas.delete("all")
            self.fft_canvas.delete("all")
            self._center_text(self.wave_canvas, "Waiting for waveform capture")
            self._center_text(self.fft_canvas, "FFT preview will appear here")
            self.thd_v_metric.set("--")
            self.thd_i_metric.set("--")
            self.samples_metric.set("--")
            self.fs_metric.set("2400 Hz")
            self.scroll.configure(state="disabled", to=0)
            self.scroll_value_label.configure(text="0.000 s")

    def _center_text(self, canvas: tk.Canvas, text: str) -> None:
        w = max(260, canvas.winfo_width())
        h = max(135, canvas.winfo_height())
        pad_x = min(38, max(22, int(w * 0.08)))
        pad_y = min(28, max(18, int(h * 0.16)))
        canvas.create_rectangle(pad_x, pad_y, w - pad_x, h - pad_y, outline=Theme.BORDER, dash=(4, 4))
        canvas.create_text(
            w / 2,
            h / 2,
            text=text,
            fill=Theme.MUTED,
            font=("Segoe UI", 10, "bold"),
            justify="center",
            width=max(120, w - 2 * pad_x - 18),
        )

    def set_collecting(self, collecting: bool, duration_s: int = 0) -> None:
        if collecting:
            self.status_label.configure(text=f"Collecting instantaneous samples for {duration_s} s… Base telemetry is temporarily paused.", fg=Theme.WARN)
        else:
            if self.packet is None:
                self.status_label.configure(text="No waveform capture yet. Request a 1–5 s capture to visualize instantaneous samples, harmonic content and THD.", fg=Theme.MUTED)
            else:
                self.status_label.configure(text="Waveform capture received. Use the horizontal slider to move through captured cycles.", fg=Theme.GOOD)

    def update_packet(self, packet: WaveformPacket) -> None:
        self.packet = packet
        window_samples = min(packet.sampling_rate_hz // 5, len(packet.voltage_samples))
        max_start = max(0, len(packet.voltage_samples) - window_samples)
        self.scroll.configure(to=max_start, resolution=max(1, packet.sampling_rate_hz // 60), state="normal")
        self.scroll_var.set(0)
        self.scroll_value_label.configure(text="0.000 s")
        self.set_collecting(False)
        self.thd_v_metric.set(f"{100.0 * packet.thd_voltage:.2f} %")
        self.thd_i_metric.set(f"{100.0 * packet.thd_current:.2f} %")
        self.samples_metric.set(f"{len(packet.voltage_samples):,}")
        self.fs_metric.set(f"{packet.sampling_rate_hz} Hz")
        self.draw_waveform()
        self.draw_fft()

    def draw_waveform(self) -> None:
        c = self.wave_canvas
        c.delete("all")
        if self.packet is None:
            self._center_text(c, "Waiting for waveform capture")
            return
        signal_name = self.signal_var.get()
        samples = self.packet.voltage_samples if signal_name == "Voltage" else self.packet.current_samples
        unit = "V" if signal_name == "Voltage" else "A"
        accent = Theme.ACCENT if signal_name == "Voltage" else Theme.PURPLE
        if not samples:
            self._center_text(c, "No samples available")
            return

        w = max(320, c.winfo_width())
        h = max(200, c.winfo_height())
        # Extra bottom padding is intentional: it leaves room for time ticks and
        # prevents the lower part of the sine wave / axis labels from being clipped.
        pad_l, pad_r, pad_t, pad_b = 58, 18, 22, 50
        plot_w = max(20, w - pad_l - pad_r)
        plot_h = max(40, h - pad_t - pad_b)
        window_samples = min(self.packet.sampling_rate_hz // 5, len(samples))
        start = int(clamp(self.scroll_var.get(), 0, max(0, len(samples) - window_samples)))
        end = start + window_samples
        view = samples[start:end]
        if len(view) < 2:
            return
        ymin, ymax = min(view), max(view)
        margin = 0.10 * max(1e-9, ymax - ymin)
        ymin -= margin
        ymax += margin
        span = max(1e-9, ymax - ymin)
        y_zero = pad_t + plot_h - ((0.0 - ymin) / span) * plot_h if ymin <= 0.0 <= ymax else pad_t + plot_h / 2

        c.create_rectangle(pad_l, pad_t, pad_l + plot_w, pad_t + plot_h, outline=Theme.BORDER)
        c.create_line(pad_l, y_zero, pad_l + plot_w, y_zero, fill=Theme.BORDER, dash=(3, 3))

        t0 = start / self.packet.sampling_rate_hz
        t1 = end / self.packet.sampling_rate_hz
        self.scroll_value_label.configure(text=f"{t0:.3f} s")

        # Time grid and tick labels in seconds.
        tick_count = 5
        for i in range(tick_count):
            frac = i / (tick_count - 1)
            x = pad_l + frac * plot_w
            tick_t = t0 + frac * (t1 - t0)
            c.create_line(x, pad_t, x, pad_t + plot_h, fill="#24324a")
            c.create_line(x, pad_t + plot_h, x, pad_t + plot_h + 5, fill=Theme.MUTED)
            c.create_text(x, pad_t + plot_h + 17, text=f"{tick_t:.3f}", fill=Theme.MUTED, font=("Segoe UI", 7))
        for i in range(1, 4):
            y = pad_t + i * plot_h / 4
            c.create_line(pad_l, y, pad_l + plot_w, y, fill="#24324a")

        points: List[float] = []
        for idx, value in enumerate(view):
            x = pad_l + (idx / (len(view) - 1)) * plot_w
            y = pad_t + plot_h - ((value - ymin) / span) * plot_h
            points.extend([x, y])
        if len(points) >= 4:
            c.create_line(points, fill=accent, width=2, smooth=True)

        c.create_text(pad_l + plot_w / 2, h - 10, text="time [s]", fill=Theme.MUTED, font=("Segoe UI", 8, "bold"))
        c.create_text(14, pad_t + plot_h / 2, text=f"{unit}", angle=90, fill=accent, font=("Segoe UI", 8, "bold"))
        c.create_text(pad_l, 10, anchor="w", text=f"{signal_name} instantaneous [{unit}]", fill=accent, font=("Segoe UI", 9, "bold"))
        c.create_text(pad_l + plot_w, 10, anchor="e", text=f"Fs={self.packet.sampling_rate_hz} Hz", fill=Theme.MUTED, font=("Segoe UI", 8))
        c.create_text(pad_l - 6, pad_t, anchor="e", text=f"{ymax:.2f}", fill=Theme.MUTED, font=("Segoe UI", 7))
        c.create_text(pad_l - 6, pad_t + plot_h, anchor="e", text=f"{ymin:.2f}", fill=Theme.MUTED, font=("Segoe UI", 7))

    def draw_fft(self) -> None:
        c = self.fft_canvas
        c.delete("all")
        if self.packet is None:
            self._center_text(c, "FFT preview will appear here")
            return
        signal_name = self.signal_var.get()
        key = "voltage_mag_vpk" if signal_name == "Voltage" else "current_mag_apk"
        unit = "Vpk" if signal_name == "Voltage" else "Apk"
        accent = Theme.ACCENT if signal_name == "Voltage" else Theme.PURPLE
        harmonics = self.packet.harmonics[:MAX_HARMONIC_ORDER]
        values = [safe_float(h.get(key)) for h in harmonics]
        freqs = [int(round(safe_float(h.get("frequency_hz", (idx + 1) * NOMINAL_FREQ_HZ)))) for idx, h in enumerate(harmonics)]
        max_val = max(max(values) if values else 1.0, 1e-9)

        w = max(260, c.winfo_width())
        h = max(200, c.winfo_height())
        # More bottom padding so the x-axis can show actual harmonic frequencies.
        pad_l, pad_r, pad_t, pad_b = 52, 18, 24, 58
        plot_w = max(20, w - pad_l - pad_r)
        plot_h = max(40, h - pad_t - pad_b)
        c.create_rectangle(pad_l, pad_t, pad_l + plot_w, pad_t + plot_h, outline=Theme.BORDER)
        for i in range(1, 4):
            y = pad_t + i * plot_h / 4
            c.create_line(pad_l, y, pad_l + plot_w, y, fill="#24324a")

        bar_gap = 2
        bar_w = max(3, (plot_w / max(1, len(values))) - bar_gap)
        for idx, val in enumerate(values):
            x0 = pad_l + idx * (bar_w + bar_gap) + 2
            x1 = x0 + bar_w
            y1 = pad_t + plot_h
            y0 = y1 - (val / max_val) * plot_h
            c.create_rectangle(x0, y0, x1, y1, fill=accent if idx == 0 else Theme.WARN, outline="")
            freq = freqs[idx] if idx < len(freqs) else int((idx + 1) * NOMINAL_FREQ_HZ)
            label_x = (x0 + x1) / 2
            # Label every bar with frequency [Hz]. Rotating keeps the axis readable.
            c.create_text(label_x, pad_t + plot_h + 22, text=str(freq), angle=90,
                          fill=Theme.MUTED, font=("Segoe UI", 7, "bold"))

        c.create_text(pad_l, 10, anchor="w", text=f"FFT amplitude [{unit}]", fill=accent, font=("Segoe UI", 8, "bold"))
        c.create_text(pad_l - 5, pad_t, anchor="e", text=f"{max_val:.2f}", fill=Theme.MUTED, font=("Segoe UI", 7))
        c.create_text(11, pad_t + plot_h / 2, text=unit, angle=90, fill=Theme.MUTED, font=("Segoe UI", 8, "bold"))
        c.create_text(pad_l + plot_w / 2, h - 8, text="Frequency [Hz]", fill=Theme.MUTED, font=("Segoe UI", 8, "bold"))



    def show_fft_table(self) -> None:
        """Open a compact harmonic table with normalized peak amplitudes."""
        if self.packet is None:
            messagebox.showinfo("FFT table", "Request a waveform capture first to populate the harmonic table.")
            return

        win = tk.Toplevel(self)
        win.title("AYCE Smart Plug · FFT Harmonic Table")
        win.geometry("620x520")
        win.configure(bg=Theme.BG)
        win.transient(self.winfo_toplevel())

        header = tk.Frame(win, bg=Theme.BG)
        header.pack(fill="x", padx=14, pady=(12, 6))
        tk.Label(header, text="FFT / Harmonic Table", bg=Theme.BG, fg=Theme.TEXT,
                 font=("Segoe UI", 14, "bold")).pack(anchor="w")
        tk.Label(header,
                 text=("Peak amplitudes normalized as 2·|DFT bin|/N. "
                       "THD is computed from harmonic amplitudes relative to the 60 Hz fundamental."),
                 bg=Theme.BG, fg=Theme.MUTED, font=("Segoe UI", 9),
                 wraplength=570, justify="left").pack(anchor="w", pady=(2, 0))

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

        for h in self.packet.harmonics[:MAX_HARMONIC_ORDER]:
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
        tk.Button(footer, text="Close", bg=Theme.PANEL_2, fg=Theme.TEXT,
                  activebackground=Theme.BORDER, relief="flat", padx=12, pady=4,
                  command=win.destroy).pack(side="right")


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
        self.root.geometry("1400x900")
        self.root.minsize(1160, 760)
        self.root.configure(bg=Theme.BG)

        self.incoming_queue: "queue.Queue[Tuple[str, str]]" = queue.Queue()
        self.router = MessageRouter(self)
        self.data_source = DummySmartPlugDataSource(self.incoming_queue)
        self.phase = PHASE_PROVISIONING
        self.latest_telemetry = TelemetrySample()
        self.collecting_waveform = False

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
        self.log("Dummy GUI initialized. Use top-right test controls to switch phases.", level="success")

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
            self.log("Dashboard phase active. Telemetry source is dummy smartplug/status.", level="success")
        elif phase == PHASE_RECONNECTING:
            self.provisioning_frame.pack(expand=True, fill="both")
            self.provisioning_frame.status_label.configure(
                text="MQTT is disconnected or not responding yet. If credentials were already sent, wait here while the Smart Plug finishes restarting and reconnecting to the broker.",
                fg=Theme.WARN,
            )
            self.provisioning_frame.small_note.configure(text="Status: waiting for MQTT reconnection. The GUI will return to the dashboard when smartplug/status is received.")
            self.log("Waiting for MQTT reconnection.", level="warning")
        else:
            self.provisioning_frame.pack(expand=True, fill="both")
            self.provisioning_frame.status_label.configure(
                text="No MQTT connection detected. You can send credentials over BLE or wait for reconnection.",
                fg=Theme.MUTED,
            )
            self.provisioning_frame.small_note.configure(text="Status: waiting for user action or simulated MQTT reconnection.")
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

    # Incoming handlers --------------------------------------------------------
    def on_telemetry(self, sample: TelemetrySample) -> None:
        self.latest_telemetry = sample
        if self.phase == PHASE_RECONNECTING:
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
        energy_wh = safe_float(data.get("energy_wh"))
        active_power = safe_float(data.get("active_power"))
        self.log(f"Energy update: {energy_wh:.3f} Wh · P={active_power:.1f} W", level="rx")

    def on_safety_ack(self, ack: SafetyLimitAck) -> None:
        self.dashboard_frame.safety_panel.update_ack(ack)
        self.dashboard_frame.status_panel.set_ack(ack)
        level = "success" if ack.accepted else "warning"
        self.log(f"Safety ACK: {ack.reason}", level=level)

    def on_waveform_packet(self, packet: WaveformPacket) -> None:
        self.collecting_waveform = False
        self.dashboard_frame.set_collecting(False)
        self.dashboard_frame.waveform_panel.update_packet(packet)
        self.dashboard_frame.status_panel.set_waveform(
            f"Waveform OK · {packet.duration_s}s · {len(packet.voltage_samples):,} samples · THD_V={100*packet.thd_voltage:.2f}%",
            Theme.GOOD,
        )
        self.log("Waveform received; FFT/THD updated.", level="success")

    # Outgoing commands --------------------------------------------------------
    def publish_relay_command(self, turn_on: bool) -> None:
        payload = "RELAY_ON" if turn_on else "RELAY_OFF"
        self._publish_to_device(TOPIC_RELAY_CMD, payload)
        self.data_source.set_relay(turn_on)

    def publish_safety_limits(self, max_vrms: float, max_iarms: float) -> None:
        payload = json.dumps({"max_vrms": max_vrms, "max_iarms": max_iarms})
        self._publish_to_device(TOPIC_SAFETY_CMD, payload)
        self.data_source.set_safety_limits(max_vrms, max_iarms)

    def publish_waveform_request(self, duration_s: int) -> None:
        duration_s = int(clamp(duration_s, 1, 5))
        if self.collecting_waveform:
            self.log("Waveform request ignored: capture already in progress.", level="warning")
            return
        payload = json.dumps({"duration_s": duration_s, "source": "GUI", "timestamp": now_str()})
        self._publish_to_device(TOPIC_WAVEFORM_CMD, payload)
        self.collecting_waveform = True
        self.dashboard_frame.set_collecting(True, duration_s)
        self.dashboard_frame.status_panel.set_waveform(f"Collecting {duration_s} s of instantaneous samples…", Theme.WARN)
        self.data_source.request_waveform_capture(duration_s)

    def _publish_to_device(self, topic: str, payload: str) -> None:
        # Future MQTT integration point.
        if hasattr(self, "dashboard_frame"):
            try:
                if topic == TOPIC_RELAY_CMD:
                    display_payload = payload
                elif topic == TOPIC_SAFETY_CMD:
                    display_payload = payload.replace('"', '')
                elif topic == TOPIC_WAVEFORM_CMD:
                    try:
                        duration = json.loads(payload).get("duration_s", "?")
                        display_payload = f"request {duration}s waveform"
                    except Exception:
                        display_payload = "waveform request"
                else:
                    display_payload = payload
                self.dashboard_frame.status_panel.set_command(f"{topic} → {display_payload}")
            except Exception:
                pass
        self.log(f"TX {topic}: {payload}", level="tx")

    def log(self, message: str, level: str = "info") -> None:
        # Keep terminal trace for development, but GUI shows only clean status labels.
        print(f"[{datetime.now().strftime('%H:%M:%S')}] {level.upper()}: {message}")
        try:
            self.dashboard_frame.status_panel.add(message, level=level)
        except Exception:
            pass

    def on_close(self) -> None:
        self.data_source.stop()
        self.root.destroy()


# =============================================================================
# Entrypoint
# =============================================================================

if __name__ == "__main__":
    root = tk.Tk()
    app = SmartPlugApp(root)
    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.mainloop()
