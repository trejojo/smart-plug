# Telemetry GUI and MQTT Layer

This folder contains the AYCE live dashboard, the reusable MQTT client, and the local Mosquitto broker configuration.

## Files

| File | Purpose |
|---|---|
| `smartplug_gui.py` | Main AYCE GUI. Handles provisioning phase, dashboard, waveform capture, FFT, THD, CSV export, user commands, and the GUI window icon. |
| `mqtt_client.py` | Reusable MQTT client and optional console tool for AYCE topics. |
| `mosquitto.conf` | Local Mosquitto configuration used by the launcher. |

The shared AYCE icon used by the GUI window/title bar is stored in:

```text
Software/assets/ayce_logo.ico
Software/assets/ayce_logo.png
```

## Functional overview

### `smartplug_gui.py`

Main responsibilities:

- shows the provisioning / reconnection phase before telemetry is received,
- connects to the MQTT broker,
- switches to the main dashboard when status telemetry arrives,
- displays electrical metrics and temperature,
- sends relay and safety-limit commands,
- requests and plots waveform captures,
- computes FFT, THD, phase angle, and time shift locally,
- exports CSV snapshots and tables,
- applies the AYCE icon to the GUI window/title bar.

### `mqtt_client.py`

Main responsibilities:

- centralizes MQTT topic names,
- parses incoming JSON payloads,
- publishes relay/config/waveform commands,
- offers an optional console-oriented diagnostic interface.

## MQTT topic contract

### ESP32 -> PC

| Topic | Purpose | Payload |
|---|---|---|
| `smartplug/telemetry/status` | Main dashboard telemetry / device heartbeat | JSON |
| `smartplug/telemetry/temperature` | Temperature-only update | JSON |
| `smartplug/telemetry/energy` | Energy/basic electrical update | JSON |
| `smartplug/events/protection` | ADE7953 protection event | JSON |
| `smartplug/state/relay` | Relay state update | JSON |
| `smartplug/state/led` | RGB LED state update | JSON |
| `smartplug/commands/ack` | ACK for GUI commands | JSON |
| `smartplug/waveform/data` | One waveform capture packet | JSON |

### PC -> ESP32

| Topic | Purpose | Payload |
|---|---|---|
| `smartplug/commands/relay` | Relay ON/OFF request | JSON |
| `smartplug/commands/config` | Safety limits request | JSON |
| `smartplug/waveform/request` | Fixed waveform capture request | JSON |


## Expected status telemetry payload

`smartplug/telemetry/status` should include the official `apparent_power` field in VA. This value is expected to come from the ADE7953 as true apparent power, including harmonic distortion.

```json
{
  "vrms": 127.20,
  "irms": 2.135,
  "pf": 0.943,
  "active_power": 255.80,
  "reactive_power": 90.40,
  "apparent_power": 271.30,
  "frequency": 60.02,
  "no_load": false,
  "energy_wh": 12.30,
  "relay": true,
  "tmp_c": 31.40
}
```

`Apparent Power` in the GUI uses `apparent_power` directly. The Power Triangle still draws its geometry from P and Q only, while the S label shows true apparent power from MQTT.

## Waveform capture contract

The fixed capture request is:

```text
512 samples @ 2400 Hz
```

Equivalent duration:

```text
512 / 2400 = 0.21333 s ≈ 213.33 ms
```

At 60 Hz, that is approximately:

```text
12.80 cycles
```

Preferred request payload:

```json
{"command":"REQUEST_WAVEFORM","sample_count":512,"sampling_rate_hz":2400,"source":"GUI"}
```

Preferred waveform response shape:

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

## Dashboard behavior

### Startup and reconnection flow

The GUI starts in the provisioning/reconnection phase and waits for `smartplug/telemetry/status`.

Status telemetry acts as the device heartbeat. If it stops arriving for more than the GUI timeout, the interface returns to the provisioning/reconnection phase.

### No-load handling

When `no_load = true`, the GUI preserves the received voltage, frequency, energy, temperature, and relay state, but locally forces load-dependent values to zero for a clean display.

### Power triangle animation

The visual triangle geometry uses smoothed internal display values so the triangle changes size smoothly.

The numeric labels for **P**, **Q**, and **S** use the latest target telemetry immediately. This keeps the values readable when measurements change rapidly.

### Waveform and harmonic views

The GUI:

- plots instantaneous waveform samples,
- computes FFT locally,
- shows voltage and current spectra with separate Y axes,
- computes THD for voltage and current,
- computes fundamental V-I phase angle and time shift.

## Window icon

`smartplug_gui.py` loads the shared AYCE icon from `Software/assets/` using Tkinter's `iconbitmap(...)` and `iconphoto(...)`. This supports the visible GUI window/title-bar icon.

The Windows taskbar can still show the Python icon when the GUI runs through `pythonw.exe`. This version intentionally does not include additional Win32/AppUserModelID taskbar-forcing code because that approach was not reliable enough in testing.

## CSV export

Available exports:

- main metrics snapshot CSV,
- waveform sample table CSV,
- FFT harmonic table CSV.

## Running manually for diagnostics

Launcher-based runs are preferred. Manual runs may create `__pycache__` unless `-B` is used.

### Run the GUI directly

From `Software`:

```cmd
python -B telemetry\smartplug_gui.py
```

### Run the console MQTT tool

From `Software`:

```cmd
python -B telemetry\mqtt_client.py --broker 192.168.137.1 --port 1883
```

Useful console commands include:

```text
relay on
relay off
set 135.0 5.0
wave
```

## Broker configuration

`mosquitto.conf` currently enables a simple local development broker:

```conf
listener 1883 0.0.0.0
allow_anonymous true
```

## About `__pycache__`

`__pycache__` folders can appear if Python scripts are executed manually without `-B`. They are safe to delete and are not included in the distributed zip.

## THD display note

The waveform panel displays THD values computed from harmonics up to the 20th harmonic.
