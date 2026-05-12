# SmartPlug Telemetry and GUI Contract

This document defines the PC-side telemetry stack for SmartPlug. It is intended to guide the follow-up GUI work that sits on top of the MQTT client.

The PC application should be organized as:

1. MQTT client layer that subscribes to telemetry and alert topics.
2. GUI layer that renders live values, alerts, and history.
3. Optional command layer that sends control messages back to the ESP32.

## System Goal

The PC should be able to:

- Receive continuous ADE7953 telemetry from the ESP32.
- Receive high-priority protection alerts.
- Send control commands back to the ESP32.
- Store and display historical data in the GUI.

## Broker Assumptions

The MQTT broker runs on the PC and accepts anonymous connections.

Recommended Mosquitto settings:

```conf
listener 1883
allow_anonymous true
```

## Topic Layout

Use separate topic families for each direction and priority level.

### Telemetry from ESP32 to PC

- `smartplug/telemetry` for continuous ADE7953 measurements

### Alerts from ESP32 to PC

- `smartplug/alerts` for critical protection events

### Commands from PC to ESP32

- `smartplug/cmd` for relay control and alert reset commands

During development, the GUI can subscribe to `smartplug/#`, but the application logic should still separate telemetry, alerts, and commands internally.

## Message Format Rules

All payloads should be UTF-8 JSON objects.

General rules:

- Use snake_case keys.
- Include a timestamp when the payload represents an event.
- Keep field names stable once the GUI depends on them.
- Use numeric types for measurements.
- Use short string enums for states and causes.

## 1. Continuous Telemetry Payload

This is the normal stream used by the GUI for live readings and history.

### Example

```json
{
	"event_type": "HEARTBEAT",
	"timestamp": "2026-05-11 18:45:02.123",
	"metrics": {
		"v_rms": 120.1,
		"i_rms": 0.55,
		"pf": 0.98,
		"thd": 0.05,
		"zero_cross_count": 60
	},
	"relay_state": 1
}
```

### Meaning

- `event_type`: Should be `HEARTBEAT` for periodic telemetry.
- `timestamp`: Optional but recommended for chart alignment.
- `metrics`: Nested ADE7953 data.
- `relay_state`: `1` for ON, `0` for OFF.

### GUI Behavior

- Update live gauges from `metrics`.
- Plot charts for `v_rms`, `i_rms`, `pf`, and `thd`.
- Keep a rolling history buffer.
- Show relay state clearly in the dashboard.

## 2. Critical Alert Payload

This is the high-priority path for protection events such as sag, overcurrent, or other lockout conditions.

### Example

```json
{
	"event_type": "CRITICAL_PROTECTION",
	"cause": "OVERCURRENT_SAG",
	"timestamp": "2026-05-11 18:45:02.123",
	"data": {
		"peak_value": 25.4,
		"unit": "Amperes",
		"duration_cycles": 12,
		"v_rms_at_event": 95.2
	},
	"action_taken": "RELAY_OPEN",
	"system_status": "LOCKED_AWAITING_ACK"
}
```

### Meaning

- `event_type`: Should be `CRITICAL_PROTECTION`.
- `cause`: The trigger that caused the event.
- `timestamp`: Time of the protection event.
- `data`: Nested event details.
- `action_taken`: Firmware response.
- `system_status`: Current lockout or recovery state.

### GUI Behavior

- Show a high-priority alert panel.
- Highlight the cause, action taken, and system status.
- Lock or disable controls if the system is awaiting acknowledgment.
- Store the event in a persistent event log.

## 3. Command Payload

The PC should also send commands to the ESP32 through a dedicated topic.

### Topic

- `smartplug/cmd`

### Example Commands

```json
{
	"command_type": "RELAY_ON",
	"timestamp": "2026-05-11 18:45:10.000",
	"source": "GUI"
}
```

```json
{
	"command_type": "RELAY_OFF",
	"timestamp": "2026-05-11 18:45:12.000",
	"source": "GUI"
}
```

```json
{
	"command_type": "RESET_ALERT",
	"timestamp": "2026-05-11 18:45:15.000",
	"source": "GUI"
}
```

### Meaning

- `command_type`: Action requested by the PC.
- `timestamp`: Command time.
- `source`: Usually `GUI`, but can also be `PC_TOOL` or `AUTO_RECOVERY`.

### GUI Behavior

- Publish commands only to `smartplug/cmd`.
- Do not mix commands with telemetry or alerts.
- Wait for a device confirmation topic or state update after issuing a command.

## Recommended Internal Data Models

The GUI and MQTT client should share a common parser layer with these models:

### `TelemetrySample`

- `event_type`
- `timestamp`
- `metrics`
- `relay_state`

### `CriticalEvent`

- `event_type`
- `cause`
- `timestamp`
- `data`
- `action_taken`
- `system_status`

### `Esp32Command`

- `command_type`
- `timestamp`
- `source`

## Suggested Parsing Strategy

1. Parse the MQTT payload as JSON.
2. Check the topic first.
3. If the topic is `smartplug/telemetry`, render live data and history.
4. If the topic is `smartplug/alerts`, render the alert view.
5. If the topic is `smartplug/cmd`, send or log a command.
6. Use `event_type` and `command_type` for secondary routing.

## Notes for ESP32 Firmware

- Publish telemetry at a fixed interval.
- Publish alerts immediately when a protection condition occurs.
- Keep alert payloads high priority.
- Use the command topic only for PC-to-ESP32 control.
- Keep JSON key names stable so the GUI does not break.

## Developer Checklist

- [ ] MQTT broker reachable at `127.0.0.1:1883`
- [ ] Anonymous connections enabled on the broker
- [ ] GUI subscribes to `smartplug/telemetry` and `smartplug/alerts`
- [ ] GUI publishes commands to `smartplug/cmd`
- [ ] JSON payloads are parsed and validated
- [ ] Critical alerts are visually separated from normal telemetry
- [ ] Historical telemetry is stored for analysis and charts

## Implementation Hint

The current Python MQTT client is the right place to centralize parsing and dispatching. The GUI should build on top of that client instead of creating a second, unrelated data path.
