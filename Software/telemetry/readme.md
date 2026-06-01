# SmartPlug MQTT Telemetry and GUI Contract

This document defines the current PC-side MQTT contract for the SmartPlug GUI. It is aligned with the present ESP32 implementation in `module_mqtt.c`.

The firmware is still expected to evolve. For that reason, the PC application should not let the GUI depend directly on raw MQTT payloads. Instead, the recommended architecture is:

1. **MQTT client layer**: subscribes to firmware topics and publishes commands.
2. **Parser/normalization layer**: converts current raw payloads into stable Python data models.
3. **GUI layer**: renders live values, alerts, history, and controls using those data models.
4. **Command layer**: sends relay commands and safety-limit updates back to the ESP32.

The updated `mqtt_client.py` implements layers 1, 2 and part of 4 so a GUI can be built on top of it.

---

## 1. System Goal

The PC should be able to:

- Receive continuous ADE7953 telemetry from the ESP32.
- Receive high-priority protection events.
- Receive relay, LED, temperature and energy-specific updates when published.
- Send relay ON/OFF commands to the ESP32.
- Send RMS voltage/current safety limits to the ESP32.
- Store and display historical data in the GUI.
- Keep a local PC-side timestamp for charting until the firmware adds a timestamp to all telemetry packets.

---

## 2. Broker Assumptions

The MQTT broker runs on the PC and accepts anonymous connections during development.

Recommended Mosquitto settings:

```conf
listener 1883
allow_anonymous true
```

Default PC-side broker configuration in `mqtt_client.py`:

```python
MQTT_BROKER = "192.168.137.1"
MQTT_PORT = 1883
```

The broker IP may be overridden from the console:

```bash
python mqtt_client.py --broker 192.168.137.1 --port 1883
```

---

## 3. Current Topic Layout

### ESP32 to PC

| Topic | Direction | Purpose | Payload type |
|---|---:|---|---|
| `smartplug/status` | ESP32 → PC | Main combined telemetry package for the GUI dashboard | JSON |
| `smartplug/events` | ESP32 → PC | Critical protection events | JSON |
| `smartplug/relay` | ESP32 → PC | Relay state update | JSON |
| `smartplug/led` | ESP32 → PC | RGB LED state update | JSON |
| `smartplug/temperature` | ESP32 → PC | Temperature-only update | JSON |
| `smartplug/energy` | ESP32 → PC | Energy-only/basic electrical update | JSON |
| `aice/status` | ESP32 → PC | Safety-limit update acknowledgment | JSON |

### PC to ESP32

| Topic | Direction | Purpose | Payload type |
|---|---:|---|---|
| `smartplug/cmd` | PC → ESP32 | Relay control | Plain text |
| `aice/cmd` | PC → ESP32 | Safety-limit update | JSON |

During development, the PC client subscribes to:

```text
smartplug/#
aice/#
```

For a production GUI, it is still acceptable to subscribe broadly, but the parser should route each topic independently.

---

## 4. Main Dashboard Telemetry

### Topic

```text
smartplug/status
```

### Current payload

```json
{
  "vrms": 127.00,
  "irms": 1.234,
  "pf": 0.987,
  "active_power": 123.45,
  "reactive_power": 12.34,
  "frequency": 60.00,
  "no_load": false,
  "energy_wh": 25,
  "relay": true,
  "tmp_c": 32.50
}
```

### Field meaning

| Field | Meaning | GUI unit/format |
|---|---|---|
| `vrms` | RMS line voltage | V RMS |
| `irms` | RMS current | A RMS |
| `pf` | Power factor | Unitless |
| `active_power` | Active power | W |
| `reactive_power` | Reactive power | var |
| `frequency` | Line frequency | Hz |
| `no_load` | No-load detection flag | Boolean / badge |
| `energy_wh` | Accumulated energy | Wh |
| `relay` | Relay state | ON/OFF |
| `tmp_c` | Temperature | °C |

### GUI behavior

The GUI should use this as the primary dashboard topic:

- Update live gauges/cards.
- Plot `vrms`, `irms`, `active_power`, `reactive_power`, `pf`, `frequency`, `energy_wh` and `tmp_c`.
- Show `relay` as a switch/state indicator.
- Show `no_load` as a status badge.
- Add a PC receive timestamp when the packet arrives because the current firmware payload does not include a telemetry timestamp.

Recommended normalized Python model:

```python
TelemetrySample(
    timestamp_received_pc="2026-06-01T17:30:00.123",
    vrms=127.00,
    irms=1.234,
    pf=0.987,
    active_power=123.45,
    reactive_power=12.34,
    frequency=60.00,
    no_load=False,
    energy_wh=25,
    relay=True,
    tmp_c=32.50,
)
```

---

## 5. Critical Protection Events

### Topic

```text
smartplug/events
```

### Current payload

```json
{
  "event_type": "CRITICAL_PROTECTION",
  "cause": "OVERVOLTAGE",
  "timestamp": 123456789,
  "data": {
    "voltage_vrms": 145.20,
    "current_a_arms": 4.500,
    "current_b_arms": 0.000,
    "duration_cycles": 3
  },
  "action_taken": "RELAY_OPEN",
  "system_status": "LOCKED_AWAITING_ACK"
}
```

### Field meaning

| Field | Meaning | GUI unit/format |
|---|---|---|
| `event_type` | Event category | String enum |
| `cause` | Protection cause | String enum |
| `timestamp` | Device timestamp in ms, when available | ms |
| `data.voltage_vrms` | RMS voltage during event | V RMS |
| `data.current_a_arms` | Channel A RMS current during event | A RMS |
| `data.current_b_arms` | Channel B RMS current during event | A RMS |
| `data.duration_cycles` | Duration of fault condition | cycles |
| `action_taken` | Firmware response | String enum |
| `system_status` | State after action | String enum |

### GUI behavior

- Show a high-priority alert panel.
- Highlight `cause`, `action_taken` and `system_status`.
- Log the event persistently.
- Consider disabling relay controls when `system_status` is `LOCKED_AWAITING_ACK`, unless a reset/acknowledgment flow is later implemented.

---

## 6. Safety-Limit Update Command

### Topic

```text
aice/cmd
```

### Current payload expected by ESP32

```json
{
  "max_vrms": 135.0,
  "max_iarms": 5.0
}
```

### Field meaning

| Field | Meaning | Unit |
|---|---|---|
| `max_vrms` | Maximum allowed RMS voltage | V RMS |
| `max_iarms` | Maximum allowed RMS current | A RMS |

### Expected acknowledgment topic

```text
aice/status
```

### Current acknowledgment payload

```json
{
  "event_type": "SAFETY_LIMITS_UPDATE",
  "accepted": true,
  "max_vrms": 135.00,
  "max_iarms": 5.000,
  "reason": "applied"
}
```

### GUI behavior

- Provide input fields for maximum voltage and maximum current.
- Publish the JSON command to `aice/cmd`.
- Wait for `aice/status`.
- If `accepted` is `true`, show that the limits were applied.
- If `accepted` is `false`, show `reason`.

---

## 7. Relay Control Command

### Topic

```text
smartplug/cmd
```

### Current payload expected by ESP32

The current firmware expects **plain text**, not JSON:

```text
RELAY_ON
```

```text
RELAY_OFF
```

### GUI behavior

- Publish `RELAY_ON` to close/enable the relay.
- Publish `RELAY_OFF` to open/disable the relay.
- Do not send JSON relay commands until the firmware is updated to parse them.
- Use `smartplug/status` or `smartplug/relay` to confirm the resulting relay state.

---

## 8. Auxiliary Status Topics

These topics may be useful for debugging or secondary GUI widgets, but `smartplug/status` should remain the main source for the dashboard.

### Relay

Topic:

```text
smartplug/relay
```

Payload:

```json
{
  "event_type": "RELAY_STATE",
  "relay": true
}
```

### LED

Topic:

```text
smartplug/led
```

Payload:

```json
{
  "r": 0,
  "g": 0,
  "b": 255
}
```

### Temperature

Topic:

```text
smartplug/temperature
```

Payload:

```json
{
  "event_type": "TEMPERATURE",
  "temperature_c": 32.5
}
```

### Energy

Topic:

```text
smartplug/energy
```

Current payload:

```json
{
  "voltage": 127.0,
  "current": 1.234,
  "power": 123.45,
  "energy": 25
}
```

Note: the current `smartplug/energy` payload uses generic field names. For the GUI, prefer `smartplug/status` because it has clearer field names: `vrms`, `irms`, `active_power` and `energy_wh`.

---

## 9. Updated Python MQTT Client

The updated `mqtt_client.py` has two intended uses.

### 9.1 Console testing

Run:

```bash
python mqtt_client.py --broker 192.168.137.1 --port 1883
```

Available commands:

```text
relay on
relay off
on
off
set <vrms> <iarms>
raw <topic> <payload>
help
quit
```

Examples:

```text
relay on
relay off
set 135.0 5.0
```

### 9.2 GUI integration

A GUI should import and reuse the MQTT client instead of opening a second MQTT path.

Minimal example:

```python
from mqtt_client import SmartPlugMqttClient

mqtt_client = SmartPlugMqttClient(broker="192.168.137.1")

mqtt_client.on_status = lambda sample: print(sample.vrms, sample.irms, sample.active_power)
mqtt_client.on_critical_event = lambda event: print(event.cause, event.system_status)
mqtt_client.on_safety_ack = lambda ack: print(ack.accepted, ack.reason)

mqtt_client.start()

mqtt_client.publish_relay(True)
mqtt_client.publish_safety_limits(max_vrms=135.0, max_iarms=5.0)
```

For Tkinter, PyQt, PySide or another GUI framework, do not update widgets directly from MQTT callbacks. MQTT callbacks run in a background thread. The safer pattern is to poll `mqtt_client.message_queue` from the GUI main thread.

---

## 10. Recommended GUI Screens

### Main dashboard

Use `smartplug/status`.

Recommended widgets:

- Voltage card: `vrms` in V RMS.
- Current card: `irms` in A RMS.
- Active power card: `active_power` in W.
- Reactive power card: `reactive_power` in var.
- Power factor card: `pf`.
- Frequency card: `frequency` in Hz.
- Energy card: `energy_wh` in Wh.
- Temperature card: `tmp_c` in °C.
- Relay state indicator: `relay`.
- No-load status badge: `no_load`.

### Controls panel

Publish to:

- `smartplug/cmd` for relay control.
- `aice/cmd` for safety limits.

### Alerts panel

Subscribe to:

- `smartplug/events`.
- `aice/status`.

Recommended alert behavior:

- Show critical protection events prominently.
- Keep an event history table.
- Show safety-limit ACK or rejection messages.

---

## 11. Known Inconsistencies in the Current Firmware Contract

These are not blockers, but the GUI should be designed with them in mind.

1. `smartplug/status` does not include `event_type`.
2. `smartplug/status` does not include a device timestamp.
3. Relay commands on `smartplug/cmd` are plain text, while safety-limit commands on `aice/cmd` are JSON.
4. Topic prefixes are mixed: `smartplug/...` and `aice/...`.
5. Some payloads use clear names such as `vrms` and `irms`, while `smartplug/energy` uses generic names such as `voltage`, `current`, `power` and `energy`.
6. The future standardized contract may add new packet types, so the GUI should rely on the parser/normalization layer instead of raw JSON access.

---

## 12. Suggested Future Standardization

When the firmware is updated later, a cleaner long-term structure could be:

```text
smartplug/status
smartplug/events
smartplug/safety/cmd
smartplug/safety/status
smartplug/cmd
```

A future normalized telemetry packet could look like:

```json
{
  "event_type": "STATUS",
  "timestamp_ms": 123456789,
  "metrics": {
    "vrms": 127.0,
    "irms": 1.234,
    "pf": 0.987,
    "active_power_w": 123.45,
    "reactive_power_var": 12.34,
    "frequency_hz": 60.0,
    "energy_wh": 25,
    "temperature_c": 32.5
  },
  "states": {
    "relay": true,
    "no_load": false
  }
}
```

Until that update happens, the GUI should stay compatible with the current as-built contract documented above.

---

## 13. Developer Checklist

- [ ] Mosquitto reachable at the configured broker IP and port.
- [ ] PC client subscribes to `smartplug/#` and `aice/#`.
- [ ] GUI uses `smartplug/status` as the main dashboard source.
- [ ] GUI treats `irms` and `max_iarms` as A RMS.
- [ ] GUI publishes relay commands as plain text to `smartplug/cmd`.
- [ ] GUI publishes safety limits as JSON to `aice/cmd`.
- [ ] GUI listens to `aice/status` after updating safety limits.
- [ ] GUI listens to `smartplug/events` for protection events.
- [ ] GUI keeps PC-side receive timestamps until the firmware adds timestamps.
- [ ] GUI does not update widgets directly from MQTT callback threads.
