# Telemetry GUI and MQTT Layer

This folder contains the AYCE live dashboard, the reusable MQTT client, and the local Mosquitto broker configuration.

## Files

| File | Purpose |
|---|---|
| `smartplug_gui.py` | Main AYCE GUI. Handles provisioning phase, dashboard, waveform capture, FFT, THD, CSV export, user commands, and the GUI window icon. |
| `mqtt_client.py` | Reusable MQTT client and optional console tool for AYCE topics. |
| `mosquitto.conf` | Local Mosquitto configuration used by the launcher. |

The shared AYCE icon used by the GUI is stored in:

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
- applies the AYCE icon to the GUI window and Windows taskbar.

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

`smartplug/telemetry/status` should include the main dashboard measurements in one JSON payload:

```json
{
  "vrms": 129.80,
  "irms": 0.551,
  "active_power": 36.60,
  "reactive_power": -13.50,
  "apparent_power": 70.25,
  "pf": -0.512,
  "frequency": 60.00,
  "energy_wh": 67.09,
  "tmp_c": 43.10,
  "relay": true,
  "no_load": false
}
```

`apparent_power` is the official field for true apparent power in VA. It comes from the ADE7953 and includes the effect of harmonics. The GUI does not use alternate field names for this value.

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

When `no_load = true`, the GUI preserves the received voltage, frequency, energy, temperature, and relay state, but locally forces load-dependent values to zero for a clean display, including current, P, Q, S and true PF.

### Apparent power and power factor

The **Apparent Power** card displays `apparent_power` received from the ESP32/ADE7953. This is true apparent power and includes harmonic distortion.

The **Power Factor** card displays the `pf` value received from telemetry. This is treated as true PF and includes harmonics.

### Power triangle animation

The visual triangle geometry uses smoothed internal display values so the triangle changes size smoothly.

The numeric labels for **P**, **Q**, and **S** use the latest target telemetry immediately. This keeps the values readable when measurements change rapidly.

The triangle geometry uses **P** and **Q** only. The visual hypotenuse is based on `sqrt(P² + Q²)`, while the **S** label shows true apparent power from `apparent_power`, including harmonics. The displacement PF shown below the triangle is calculated locally from P and Q and is signed by load type: positive for inductive, negative for capacitive.

### Waveform and harmonic views

The GUI:

- plots instantaneous waveform samples,
- computes FFT locally,
- shows voltage and current spectra with separate Y axes,
- computes THD for voltage and current using harmonics up to the 20th,
- computes fundamental V-I phase angle and time shift.

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


## Mosquitto service note

If Windows starts Mosquitto automatically as a service, port `1883` may already be occupied before the AYCE launcher opens. In that case, a launcher that tries to start a second Mosquitto instance may show:

```text
Mosquitto closed immediately. Check whether port 1883 is already in use or whether mosquitto.conf is valid.
```

Recommended AYCE setup: set the Mosquitto Windows service to **Manual** and let the AYCE launcher start the broker when needed.

Check the port:

```cmd
netstat -ano | findstr :1883
```

Check the process:

```cmd
tasklist /FI "PID eq <PID>"
```

Set the service to manual from an Administrator Command Prompt:

```cmd
sc stop mosquitto
sc config mosquitto start= demand
```
