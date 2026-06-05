# AYCE Smart Plug PC Software

This package contains the PC-side software for the AYCE Smart Plug project.

Included tools:

- `telemetry/smartplug_gui.py`: live MQTT dashboard with BLE provisioning access.
- `telemetry/mqtt_client.py`: reusable MQTT client and optional console tool.
- `telemetry/start_mosquitto.bat`: helper script to start the local Mosquitto broker.
- `telemetry/start_gui.bat`: helper script to start the GUI.
- `provisioning/provisioner.py`: BLE provisioning helper imported by the GUI.
- `requirements.txt`: Python dependencies.

The firmware-side `module_mqtt.c` belongs in the ESP32 firmware project, not in this `Software` folder. It is intentionally not included here.

## 1. Create and activate the Python environment

Open a terminal in the `Software` directory.

PowerShell:

```powershell
python -m venv .venv_pc
.\.venv_pc\Scripts\Activate.ps1
pip install -r requirements.txt
```

CMD:

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

The default broker configuration listens on port `1883` and accepts anonymous local development connections.

## 3. Run the GUI

Open another terminal in `Software\telemetry` with the same virtual environment active:

```cmd
python smartplug_gui.py
```

or:

```cmd
start_gui.bat
```

The GUI opens maximized by default, connects to the configured MQTT broker and waits for `smartplug/telemetry/status`. Once telemetry is received, it switches to the main dashboard.

## 4. BLE provisioning

The GUI can send WiFi credentials over BLE using the same provisioning helper used by the standalone script.

Default values currently shown by the GUI:

```text
SSID: AICE_HS
Password: Bake_This
Broker: 192.168.137.1
Port: 1883
BLE MAC: E0:72:A1:CE:A3:8A
```

The BLE provisioning helper keeps the working firmware flow: by default it sends the WiFi credentials expected by the ESP32 provisioning service. The broker IP and port are used by the PC GUI to reconnect to the Mosquitto broker after provisioning.

## 5. MQTT topic contract

### ESP32 -> PC

```text
smartplug/telemetry/status
smartplug/telemetry/temperature
smartplug/telemetry/energy
smartplug/events/protection
smartplug/state/relay
smartplug/state/led
smartplug/commands/ack
smartplug/waveform/data
```

### PC -> ESP32

```text
smartplug/commands/relay
smartplug/commands/config
smartplug/waveform/request
```

The PC parser still accepts older development aliases for transition/testing, but the GUI publishes the standardized topics above.

## 6. Waveform capture

The GUI requests one fixed waveform capture per request:

```text
512 samples @ 2400 Hz
```

This corresponds to approximately:

```text
213.33 ms, or about 12.80 cycles at 60 Hz
```

The GUI computes FFT, THD, phase angle and time shift locally from the received waveform samples.

## 7. CSV export

The GUI can export three CSV files using a file-save dialog:

- Main metrics CSV from the main dashboard.
- Sample table CSV from the waveform sample table window.
- FFT table CSV from the FFT harmonic table window.

Each export includes local save timestamps and, when available, the measurement/capture timestamp.

## 8. Recommended test sequence

1. Start Mosquitto using `telemetry\start_mosquitto.bat`.
2. Run `telemetry\smartplug_gui.py`.
3. Energize the ESP32 Smart Plug.
4. If needed, use the GUI BLE provisioning screen and send credentials.
5. Wait for `smartplug/telemetry/status`; the GUI will switch to the main dashboard.
6. Test relay ON/OFF.
7. Test safety limits.
8. Request waveform; the GUI should plot 512 samples and update FFT/THD/phase metrics.
9. Test CSV export buttons.

## 9. Optional console testing

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
