# AYCE Smart Plug Telemetry GUI and MQTT Contract

This folder contains the PC-side MQTT client, the live Smart Plug GUI and the local Mosquitto broker helper.

## 1. Standard MQTT topic contract

### ESP32 → PC

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

### PC → ESP32

| Topic | Purpose | Payload |
|---|---|---|
| `smartplug/commands/relay` | Relay ON/OFF command | JSON |
| `smartplug/commands/config` | Safety/configuration limits | JSON |
| `smartplug/waveform/request` | Request one 512-sample waveform capture | JSON |

The previous development topics (`smartplug/status`, `smartplug/cmd`, `aice/cmd`, `aice/status`, `smartplug/waveform/chunk`) are still accepted in the PC parser and firmware patch as transition aliases, but the GUI publishes and expects the standardized topics above.

## 2. Expected telemetry payload

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

## 3. Command payloads

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
{"command":"REQUEST_WAVEFORM","sample_count":512,"sampling_rate_hz":6990,"source":"GUI"}
```

The GUI no longer lets the user configure capture time. One request means one packet of 512 samples. At 6.99 kHz this represents about 73.25 ms.

## 4. Running the broker

From `Software/telemetry`:

```cmd
start_mosquitto.bat
```

This uses `mosquitto.conf`:

```conf
listener 1883 0.0.0.0
allow_anonymous true
```

## 5. Running the live GUI

From `Software/telemetry`:

```cmd
python smartplug_gui.py
```

or:

```cmd
start_gui.bat
```

The GUI connects to the broker, waits for `smartplug/telemetry/status`, and switches to the dashboard once telemetry is received.

## 6. Running the console MQTT client

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

## 7. Firmware patch

The standardized firmware-side `module_mqtt.c` is included in:

```text
Software/firmware_patch/module_mqtt.c
```

Replace the ESP32 project's current `module_mqtt.c` with this file, rebuild and flash. The patch publishes on the standardized topics while still subscribing to a few legacy aliases during transition.
