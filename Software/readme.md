# PC Software & Provisioning Setup

This package contains the PC-side tools for the AYCE Smart Plug:

- `telemetry/smartplug_gui.py`: live MQTT GUI.
- `telemetry/mqtt_client.py`: reusable MQTT client and console tool.
- `telemetry/start_mosquitto.bat`: starts the local Mosquitto broker.
- `provisioning/provisioner.py`: BLE provisioning helper, also imported by the GUI.
- `firmware_patch/module_mqtt.c`: ESP32 firmware-side MQTT patch with standardized topics.

## 1. Create and activate the Python environment

Open a terminal in the `Software` directory:

```powershell
python -m venv .venv_pc
.\.venv_pc\Scripts\Activate.ps1
pip install -r requirements.txt
```

For CMD:

```cmd
python -m venv .venv_pc
.\.venv_pc\Scripts\activate.bat
pip install -r requirements.txt
```

## 2. Start Mosquitto

Open a terminal in `Software\telemetry`:

```cmd
start_mosquitto.bat
```

The broker listens on port `1883` and accepts anonymous local development connections.

## 3. Run the GUI

Open another terminal in `Software\telemetry` with the same virtual environment active:

```cmd
python smartplug_gui.py
```

The GUI can send BLE provisioning credentials using the values already used in the provisioning script:

```text
SSID: AICE_HS
Password: Bake_This
Broker: 192.168.137.1
Port: 1883
BLE MAC: E0:72:A1:CE:A3:8A
```

The BLE payload remains compatible with the working firmware provisioning flow: by default it sends only `ssid` and `password`. The broker IP/port are used by the PC GUI to connect to Mosquitto.

## 4. Update the ESP32 firmware

Copy:

```text
firmware_patch\module_mqtt.c
```

into the ESP32 project location where `module_mqtt.c` currently lives. Then rebuild and flash the ESP32.

The standardized MQTT topics are:

```text
ESP32 -> PC
smartplug/telemetry/status
smartplug/telemetry/temperature
smartplug/telemetry/energy
smartplug/events/protection
smartplug/state/relay
smartplug/state/led
smartplug/commands/ack
smartplug/waveform/data

PC -> ESP32
smartplug/commands/relay
smartplug/commands/config
smartplug/waveform/request
```

## 5. Recommended test sequence

1. Start Mosquitto using `telemetry\start_mosquitto.bat`.
2. Run `telemetry\smartplug_gui.py`.
3. Energize the ESP32 Smart Plug.
4. If needed, use the GUI BLE provisioning screen and send credentials.
5. Wait for `smartplug/telemetry/status`; the GUI will switch to the dashboard.
6. Test relay ON/OFF.
7. Test safety limits.
8. Request waveform; the GUI should plot 512 samples and update FFT/THD.

## 6. Optional console testing

```cmd
cd telemetry
python mqtt_client.py --broker 192.168.137.1 --port 1883
```

Then use:

```text
relay on
relay off
set 135.0 5.0
wave
```
