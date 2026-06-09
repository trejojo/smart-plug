# AYCE Smart Plug PC Software

This folder contains the PC-side software for the AYCE Smart Plug project. It includes the live GUI, the reusable MQTT client, the BLE provisioning helper, and a launcher system that starts or reuses the local Mosquitto broker and opens the GUI.

## Folder structure

```text
Software/
├─ README.md
├─ requirements.txt
├─ assets/
│  ├─ ayce_logo.ico
│  └─ ayce_logo.png
├─ launchers/
│  ├─ README.md
│  ├─ start_ayce_system_hidden.vbs
│  ├─ Start-AyceSystem.ps1
│  ├─ create_desktop_shortcut.ps1
│  ├─ create_desktop_shortcut.bat
│  └─ start_ayce_system.bat
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

- **`assets/`**: shared AYCE icon files. Both the desktop shortcut and the Tkinter GUI use the same icon.
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
.\.venv_pc\Scripts\activate
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

1. Run `launchers/create_desktop_shortcut.bat` once.
2. Use the generated **AYCE Smart Plug** desktop shortcut from then on.

Each time you open that desktop shortcut:

- a **visible Mosquitto broker console** opens when the launcher starts Mosquitto itself,
- if Mosquitto is already running on port `1883`, the launcher uses that existing broker and opens only the GUI,
- the **GUI starts without a Python console**,
- the shortcut and GUI use the shared AYCE icon,
- the hidden PowerShell supervisor monitors both processes.

### Why the extra empty launcher console should not appear

The desktop shortcut launches `start_ayce_system_hidden.vbs` through `wscript.exe`. That hidden helper starts the PowerShell supervisor without leaving an extra empty launcher console next to the broker console and GUI.

`start_ayce_system.bat` remains available only as a manual fallback.

### Broker detection and shutdown behavior

The launcher checks port `1883` before starting Mosquitto.

- If port `1883` is free, the launcher starts its own visible **AYCE MQTT Broker** console. Closing the GUI closes that broker console and its process tree. Closing that broker console closes the GUI process tree.
- If port `1883` is already used by `mosquitto.exe`, usually because Mosquitto started as a Windows service after reboot, the launcher uses that existing broker and does **not** open or close an extra broker console.
- If port `1883` is used by another process, the launcher stops and shows an error.

When the launcher starts its own broker, cleanup uses Windows `taskkill /PID <pid> /T /F` so child processes are also closed.

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

## Main telemetry payload

The GUI expects `apparent_power` inside the existing `smartplug/telemetry/status` payload. No new MQTT topic is required.

Expected fields include:

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

`apparent_power` is the true apparent power in VA from the ADE7953 and includes harmonic distortion. The GUI does not calculate this value from `sqrt(P² + Q²)` anymore.

## Waveform capture contract

The GUI and console tool use the fixed capture request:

```text
512 samples @ 2400 Hz
```

This corresponds to approximately:

```text
213.33 ms, or about 12.80 cycles at 60 Hz
```

The displayed THD values include harmonics up to the 20th.

## Power triangle behavior

The power triangle shape is still animated smoothly for a better visual transition. The numeric labels for **P**, **Q**, and **S** update immediately from the latest telemetry so they stay readable even when the measurements change quickly.

The triangle geometry still uses **P** and **Q** only. The visual hypotenuse is based on the P-Q geometry, while the displayed **S** label uses true apparent power from `apparent_power`, including harmonics.

## About `__pycache__`

`__pycache__` folders are generated by Python to store compiled bytecode files (`.pyc`). They are not source files and are not required for distribution.

This package does not include `__pycache__` folders. The launcher sets:

```text
PYTHONDONTWRITEBYTECODE=1
```

and runs the GUI with `python -B` / `pythonw -B` behavior to avoid creating project `__pycache__` folders during normal launcher-based use.

If you run scripts manually without `-B`, Python may create `__pycache__` again. It is safe to delete those folders.

## Recommended first test

1. Install Mosquitto.
2. Install Python dependencies.
3. Configure the Wi-Fi adapter preferred band to **2.4 GHz**.
4. Configure and enable the Windows mobile hotspot in **2.4 GHz** mode.
5. Run `launchers/create_desktop_shortcut.bat` once.
6. Open the desktop shortcut.
7. Use BLE provisioning if needed.
8. Wait for `smartplug/telemetry/status` telemetry.
9. Test relay control, safety limits, waveform capture, and shutdown behavior.

## Quick troubleshooting

### The GUI does not start

Check that:

- Python is installed.
- `paho-mqtt` and `bleak` are installed.
- `telemetry/smartplug_gui.py` exists.

### The broker console does not start

This can be normal if Mosquitto is already running as a Windows service. In that case, the launcher reuses the existing broker and opens only the GUI.

To check whether something is already listening on port `1883`:

```cmd
netstat -ano | findstr :1883
```

If no broker is already running and the launcher must start Mosquitto itself, check that:

- Mosquitto is installed.
- `mosquitto.exe` is in `PATH` or in the default install folder.
- `telemetry/mosquitto.conf` exists.

If port `1883` is occupied by something other than `mosquitto.exe`, the launcher will stop and show an error.


### Error after reboot: `Mosquitto closed immediately`

If a launcher version tries to start Mosquitto and shows this message:

```text
Mosquitto closed immediately. Check whether port 1883 is already in use or whether mosquitto.conf is valid.
```

the most common cause is that **Mosquitto was already started automatically by Windows as a service** during boot. In that situation, port `1883` is already occupied, so a second Mosquitto instance cannot bind to the same port and closes immediately.

Confirm the cause with:

```cmd
netstat -ano | findstr :1883
```

Then check which process owns the PID shown in the last column:

```cmd
tasklist /FI "PID eq <PID>"
```

If the process is `mosquitto.exe`, the broker is already running.

Recommended AYCE setup: set the Mosquitto Windows service to **Manual** instead of **Automatic**. This lets the AYCE launcher control when the broker starts and stops.

Option A, using Windows Services:

1. Press `Win + R`.
2. Run `services.msc`.
3. Find **Mosquitto Broker** or **mosquitto**.
4. Stop the service if it is running.
5. Open **Properties**.
6. Set **Startup type** to **Manual**.

Option B, using Command Prompt as Administrator:

```cmd
sc stop mosquitto
sc config mosquitto start= demand
```

After this, restart the AYCE desktop shortcut. If Mosquitto is not already running, the launcher will open the visible **AYCE MQTT Broker** console itself.

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
