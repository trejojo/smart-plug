# BLE Provisioning Helper

This folder contains the PC-side BLE provisioning helper used by the AYCE GUI.

## Files

| File | Purpose |
|---|---|
| `provisioner.py` | Sends Wi-Fi credentials to the AYCE ESP32 over BLE. |

## Role in the overall system

The AYCE ESP32 must receive Wi-Fi credentials before it can join the Windows hotspot and connect to the MQTT broker.

This helper is used in two ways:

1. **Indirectly by the GUI**: the provisioning screen imports and calls `send_credentials_sync(...)`.
2. **Directly from the command line**: useful for isolated BLE testing.

## What is sent

The provisioning payload includes the Wi-Fi data needed by the ESP32 firmware, typically:

- hotspot SSID
- hotspot password
- optionally broker-related fields, depending on the selected mode

The current PC-side workflow keeps `include_mqtt_fields=False` by default to preserve the working firmware provisioning flow.

## Expected user workflow

1. Configure a Windows mobile hotspot in **2.4 GHz** mode.
2. Set the hotspot name and password.
3. Open the AYCE GUI.
4. In the provisioning screen, enter:
   - Wi-Fi SSID
   - Wi-Fi password
   - broker host/IP
   - broker port
   - BLE MAC address
5. Send credentials.
6. Wait for the AYCE device to connect to Wi-Fi and begin publishing MQTT telemetry.

## Running the helper directly

Launcher-based GUI use is preferred. Manual Python execution may create `__pycache__` unless `-B` is used.

From the `Software` folder:

```cmd
python -B provisioning\provisioner.py --ssid MY_HOTSPOT --password MY_PASSWORD --mac E0:72:A1:CE:A3:8A
```

Optional flags depend on the script interface. Use:

```cmd
python -B provisioning\provisioner.py --help
```

## About `__pycache__`

`__pycache__` folders are Python bytecode caches. They are not necessary for distribution and can be deleted safely.

The launcher sets `PYTHONDONTWRITEBYTECODE=1` and uses Python `-B` for normal GUI startup to avoid generating project `__pycache__` folders.

## Notes

- The ESP32 uses **2.4 GHz Wi-Fi**. Make sure the Windows hotspot is configured accordingly.
- The SSID and password sent over BLE must exactly match the active hotspot.
- The broker host/IP entered in the GUI should point to the PC running Mosquitto.
