# AYCE Smart Plug Telemetry GUI and MQTT Contract

This folder contains the PC-side MQTT client, the live Smart Plug GUI and the local Mosquitto broker helper.

## 1. Files in this folder

| File | Purpose |
|---|---|
| `smartplug_gui.py` | Main live MQTT GUI. Includes BLE provisioning access, dashboard visualization, waveform/FFT analysis and CSV exports. |
| `mqtt_client.py` | Reusable MQTT layer and optional console client. |
| `start_mosquitto.bat` | Starts Mosquitto with the local `mosquitto.conf`. |
| `start_gui.bat` | Starts the GUI. |
| `mosquitto.conf` | Local broker configuration for development/testing. |

The firmware-side `module_mqtt.c` is not part of this folder. It should remain in the ESP32 firmware project.

## 2. Standard MQTT topic contract

### ESP32 -> PC

| Topic | Purpose | Payload |
|---|---|---|
| `smartplug/telemetry/status` | Main dashboard telemetry | JSON |
| `smartplug/telemetry/temperature` | Temperature-only update | JSON |
| `smartplug/telemetry/energy` | Energy/basic electrical update | JSON |
| `smartplug/events/protection` | Critical ADE7953 protection event | JSON |
| `smartplug/state/relay` | Relay state update | JSON |
| `smartplug/state/led` | RGB LED state update | JSON |
| `smartplug/commands/ack` | ACK for relay/config/waveform commands | JSON |
| `smartplug/waveform/data` | 512-sample waveform capture | JSON |

### PC -> ESP32

| Topic | Purpose | Payload |
|---|---|---|
| `smartplug/commands/relay` | Relay ON/OFF command | JSON |
| `smartplug/commands/config` | Safety/configuration limits | JSON |
| `smartplug/waveform/request` | Request one 512-sample waveform capture | JSON |

The PC parser still accepts older development topics such as `smartplug/status`, `smartplug/cmd`, `aice/cmd`, `aice/status`, `smartplug/waveform/chunk` and `smartplug/waveform` for transition/testing. The GUI publishes the standardized topics listed above.

## 3. Expected status telemetry payload

`smartplug/telemetry/status`:

```json
{
  "vrms": 127.20,
  "irms": 2.135,
  "pf": 0.943,
  "active_power": 255.80,
  "reactive_power": 90.40,
  "frequency": 60.02,
  "no_load": false,
  "energy_wh": 12.3,
  "relay": true,
  "tmp_c": 31.4
}
```

When `no_load` is `true`, the GUI keeps voltage, frequency, active energy, relay state and temperature as received, but locally displays load-dependent values as zero:

```text
irms = 0
pf = 0
active_power = 0
reactive_power = 0
apparent_power = 0
```

This prevents stale current/power values from remaining visible while keeping the dashboard clean: the load card simply shows `NO LOAD`.

## 4. Command payloads

Relay command to `smartplug/commands/relay`:

```json
{"command":"RELAY_ON","relay":true,"source":"GUI"}
```

or:

```json
{"command":"RELAY_OFF","relay":false,"source":"GUI"}
```

Safety limits to `smartplug/commands/config`:

```json
{"command":"SET_SAFETY_LIMITS","max_vrms":135.0,"max_iarms":5.0,"source":"GUI"}
```

Waveform request to `smartplug/waveform/request`:

```json
{"command":"REQUEST_WAVEFORM","sample_count":512,"sampling_rate_hz":2400,"source":"GUI"}
```

The GUI no longer lets the user configure capture duration. One request means one packet of 512 samples. At 2400 Hz this represents approximately 213.33 ms, or about 12.80 cycles at 60 Hz.

## 5. Waveform packet expected by the GUI

The GUI accepts the preferred waveform payload shape:

```json
{
  "event_type": "WAVEFORM_CAPTURE",
  "timestamp": "2026-06-05 14:32:18.123",
  "sample_count": 512,
  "duration_s": 0.213333,
  "sampling_rate_hz": 2400,
  "signals": {
    "voltage_v": [0.0, 12.3, 24.5],
    "current_a": [0.0, 0.12, 0.23]
  }
}
```

The PC GUI recomputes harmonic magnitudes, THD, V-I phase angle and time shift from the raw samples. In `Both` mode, the waveform plot uses a left voltage axis and right current axis; the FFT plot also uses independent voltage/current Y axes.

## 6. Dashboard calculations

The GUI displays:

- Main `Power Factor`: true PF received in telemetry. The card subtitle is `True PF, including THD`.
- `Apparent Power`: calculated locally as `sqrt(P^2 + Q^2)`.
- `Power Triangle`: uses the sign of reactive power. Positive Q plots upward as inductive; negative Q plots downward as capacitive.
- `Displacement PF`: calculated locally from P, Q and S only; it intentionally ignores harmonic distortion.
- Waveform phase note: calculated from the fundamental components of voltage and current. Positive phase means current lags voltage; negative phase means current leads voltage.

## 7. CSV export

Available exports:

| Location | Button | Content |
|---|---|---|
| Main dashboard header | `Save metrics CSV` | One-row snapshot of the latest main metrics and GUI-side P/Q/S calculations. |
| Sample table window | `Save CSV` | Per-sample index, time, voltage and current. |
| FFT table window | `Save CSV` | Harmonic order, frequency, voltage peak magnitude and current peak magnitude. |

Each button opens a Windows file-save dialog so the user can choose the destination.

## 8. Device heartbeat watchdog

The GUI does not rely on the MQTT client disconnect callback to detect that the ESP32 is gone. The PC may remain connected to the local Mosquitto broker even when the Smart Plug powers off, reboots or enters BLE pairing mode.

Instead, the GUI treats `smartplug/telemetry/status` as the device heartbeat. If no status telemetry is received for more than 3 seconds while the main dashboard is active, the GUI returns to the provisioning/reconnection screen and waits for telemetry to return. The timeout is defined in `smartplug_gui.py` as:

```python
DEVICE_HEARTBEAT_TIMEOUT_S = 3.0
```

This watchdog is a GUI-side fallback. A future firmware-level MQTT Last Will / availability topic would be a more formal device-state signal, but it is not required for the current PC-side behavior.

## 9. Running the broker

From `Software/telemetry`:

```cmd
start_mosquitto.bat
```

This uses `mosquitto.conf`:

```conf
listener 1883 0.0.0.0
allow_anonymous true
```

## 10. Running the live GUI

From `Software/telemetry`:

```cmd
python smartplug_gui.py
```

or:

```cmd
start_gui.bat
```

The GUI opens maximized by default, connects to the broker, waits for `smartplug/telemetry/status`, and switches to the main dashboard once telemetry is received.

## 11. Running the console MQTT client

From `Software/telemetry`:

```cmd
python mqtt_client.py --broker 192.168.137.1 --port 1883
```

Useful commands inside the console:

```text
relay on
relay off
set 135.0 5.0
wave
```
