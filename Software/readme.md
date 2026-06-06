# AYCE Smart Plug PC Software

This folder contains the PC-side software for the AYCE Smart Plug project. It includes the live GUI, the reusable MQTT client, the BLE provisioning helper, and a launcher system that starts the local Mosquitto broker and the GUI together.

## Folder structure

```text
Software/
├─ README.md
├─ requirements.txt
├─ launchers/
│  ├─ README.md
│  ├─ start_ayce_system.bat
│  ├─ Start-AyceSystem.ps1
│  ├─ create_desktop_shortcut.ps1
│  ├─ create_desktop_shortcut.bat
│  └─ assets/
│     └─ ayce_logo.ico
├─ telemetry/
│  ├─ README.md
│  ├─ smartplug_gui.py
│  ├─ mqtt_client.py
│  └─ mosquitto.conf
└─ provisioning/
   ├─ README.md
   └─ provisioner.py
```

## What each folder does

- **`launchers/`**: startup and desktop-shortcut helpers. This is the official entry point for normal use.
- **`telemetry/`**: the GUI, MQTT logic, waveform/FFT processing, and local Mosquitto configuration.
- **`provisioning/`**: BLE helper used to send Wi-Fi credentials to the AYCE ESP32.

## Before using the system

Complete the following PC setup steps first.

### 1) Install Python

Install a recent Windows version of Python 3.

### 2) Install Mosquitto MQTT Broker

Install **Mosquitto** on Windows. The launcher looks for `mosquitto.exe` first in `PATH`, then in the common default locations:

- `C:\Program Files\Mosquitto\mosquitto.exe`
- `C:\Program Files (x86)\Mosquitto\mosquitto.exe`

### 3) Install Python dependencies

From the `Software` folder run either one of the following approaches.

#### Option A: without a virtual environment

```cmd
pip install -r requirements.txt
```

#### Option B: with a virtual environment

```cmd
python -m venv .venv_pc
.\.venv_pc\Scripts\activate.bat
pip install -r requirements.txt
```

You do **not** need a virtual environment to use this project. The launcher supports both modes:

- If `.venv_pc` or `.venv` exists, it uses that Python first.
- Otherwise, it uses the system Python installation.

### 4) Force the Wi-Fi adapter to prefer 2.4 GHz

Because the ESP32 connects through **2.4 GHz Wi-Fi**, configure the PC wireless adapter accordingly.

In Windows:

1. Open **Device Manager**.
2. Open **Network adapters**.
3. Right-click your Wi-Fi adapter and open **Properties**.
4. Open the **Advanced** tab.
5. Set **Preferred Band** to **2.4 GHz**.

### 5) Create a Windows mobile hotspot in 2.4 GHz mode

The AYCE device will connect to the hotspot whose credentials you send over BLE.

In Windows:

1. Open **Settings**.
2. Go to **Network & Internet**.
3. Open **Mobile hotspot**.
4. Enable hotspot sharing over **Wi-Fi**.
5. Set the hotspot **Band** to **2.4 GHz**.
6. Set the hotspot **Name** and **Password** to the exact credentials you want to send to AYCE over BLE.

> Important: the hotspot SSID and password configured on the PC must match the credentials sent to the AYCE device during BLE provisioning.

## Normal startup workflow

For normal use, the recommended startup flow is:

1. Create the desktop shortcut once using `launchers/create_desktop_shortcut.bat` or `launchers/create_desktop_shortcut.ps1`.
2. Use the generated **AYCE Smart Plug** desktop shortcut from then on.
3. Each time you open that shortcut:
   - a **visible Mosquitto broker console** opens,
   - the **GUI starts without a Python console**,
   - the launcher supervises both processes.

### Coupled shutdown behavior

The launcher supervises only the processes it started.

- If you close the **GUI**, the launcher closes the Mosquitto broker console it started.
- If you close the **broker console**, the launcher closes the GUI it started.

This keeps normal startup and shutdown simple while avoiding interference with unrelated external processes.

## BLE provisioning flow

The GUI starts in the provisioning / reconnection phase until status telemetry is received.

Typical flow:

1. Open the system using the desktop shortcut.
2. Make sure the Windows mobile hotspot is active in **2.4 GHz** mode.
3. In the GUI provisioning screen, enter:
   - Wi-Fi SSID
   - Wi-Fi password
   - broker IP / hostname
   - broker port
   - AYCE BLE MAC address
4. Send credentials over BLE.
5. The ESP32 connects to the hotspot and then to the local MQTT broker.
6. Once status telemetry starts arriving, the GUI switches to the main dashboard.

## Waveform capture contract

The GUI and console tool use the fixed capture request:

```text
512 samples @ 2400 Hz
```

This corresponds to approximately:

```text
213.33 ms, or about 12.80 cycles at 60 Hz
```

## Recommended first test

1. Install Mosquitto.
2. Install Python dependencies.
3. Configure the Wi-Fi adapter preferred band to **2.4 GHz**.
4. Configure and enable the Windows mobile hotspot in **2.4 GHz** mode.
5. Create the desktop shortcut.
6. Open the desktop shortcut.
7. Use BLE provisioning if needed.
8. Wait for `smartplug/telemetry/status` telemetry.
9. Test relay control, safety limits, and waveform capture.

## Quick troubleshooting

### The GUI does not start

Check that:

- Python is installed.
- `paho-mqtt` and `bleak` are installed.
- `smartplug_gui.py` is present in `telemetry/`.

### The broker console does not start

Check that:

- Mosquitto is installed.
- `mosquitto.exe` is in `PATH` or in the default install folder.
- `telemetry/mosquitto.conf` exists.

### AYCE does not connect

Check that:

- the PC hotspot is enabled,
- the hotspot is set to **2.4 GHz**,
- the SSID and password sent over BLE exactly match the hotspot,
- the broker IP/host entered in the GUI matches the PC running Mosquitto.

### A teammate uses a virtual environment and I do not

That is supported. The launcher automatically prefers `.venv_pc` or `.venv` when they exist, and otherwise falls back to the system Python installation.

## Additional documentation

- `launchers/README.md`
- `telemetry/README.md`
- `provisioning/README.md`
