# Launcher System

This folder contains the official AYCE PC startup helpers.

## Files

| File | Purpose |
|---|---|
| `create_desktop_shortcut.bat` | Recommended one-time shortcut installer for Windows users. |
| `create_desktop_shortcut.ps1` | PowerShell implementation that creates the desktop shortcut. |
| `start_ayce_system_hidden.vbs` | Hidden Windows Script Host launcher used by the desktop shortcut. |
| `Start-AyceSystem.ps1` | Hidden PowerShell supervisor that starts/reuses the broker and opens the GUI. |
| `start_ayce_system.bat` | Manual fallback launcher. The desktop shortcut does not point to this file. |

The shared icon is stored outside this folder:

```text
Software/assets/ayce_logo.ico
Software/assets/ayce_logo.png
```

## Normal use

### One-time setup

Run once:

```text
create_desktop_shortcut.bat
```

This creates a desktop shortcut named:

```text
AYCE Smart Plug
```

The shortcut launches:

```text
wscript.exe start_ayce_system_hidden.vbs
```

The VBS helper then starts the PowerShell supervisor hidden. This avoids leaving an extra empty launcher console visible next to the broker and GUI.

### Everyday use

Double-click the **AYCE Smart Plug** desktop shortcut.

Expected result:

1. If port `1883` is free, one visible console named **AYCE MQTT Broker** opens for Mosquitto logs.
2. If port `1883` is already used by `mosquitto.exe`, usually because Mosquitto started as a Windows service after reboot, no broker console is opened; the GUI uses that existing broker.
3. The GUI opens without a separate Python console.
4. The hidden supervisor monitors the processes it started.

## What `Start-AyceSystem.ps1` does

The supervisor performs the following steps:

1. Resolves the project root and required file paths.
2. Sets `PYTHONDONTWRITEBYTECODE=1` to avoid project `__pycache__` generation during normal use.
3. Detects Python automatically:
   - first `.venv_pc`,
   - then `.venv`,
   - otherwise system Python.
4. Verifies required Python modules:
   - `paho-mqtt`
   - `bleak`
5. Checks whether TCP port `1883` is already listening.
6. If port `1883` is used by `mosquitto.exe`, it reuses that existing broker.
7. If port `1883` is free, it locates `mosquitto.exe` and starts a visible **AYCE MQTT Broker** console.
8. Starts the GUI using **`pythonw.exe -B`** so there is **no Python console window**.
9. Monitors the GUI and, when applicable, the broker process started by the launcher.

## Broker detection and shutdown behavior

The launcher supervises only the processes it started.

- **Launcher-started broker:** closing the GUI closes the broker console and child processes. Closing the broker console closes the GUI process.
- **Existing Mosquitto broker:** if port `1883` is already occupied by `mosquitto.exe`, the launcher uses it and does not close it when the GUI exits.
- **Other process on port 1883:** if port `1883` is occupied by something other than Mosquitto, the launcher stops and shows an error.

When the launcher starts its own broker, cleanup uses:

```text
taskkill /PID <pid> /T /F
```

This is more robust than closing only the direct parent process.

## Why `.vbs`, `.ps1`, and `.bat` all exist

The PowerShell script contains the real launcher logic. The VBS helper allows the desktop shortcut to start PowerShell hidden without showing an extra empty console. The `.bat` files are convenience wrappers for Windows users.

- `create_desktop_shortcut.bat` is the recommended way to create the shortcut.
- `start_ayce_system_hidden.vbs` is used by the generated desktop shortcut.
- `start_ayce_system.bat` is kept only as a manual fallback.

## If a virtual environment exists

No problem. The launcher auto-detects `.venv_pc` or `.venv`. If neither exists, it falls back to system Python.

## About `__pycache__`

Launcher-based GUI runs should not create project `__pycache__` folders because the supervisor sets `PYTHONDONTWRITEBYTECODE=1` and starts the GUI with `-B`.

If `__pycache__` appears after manual script execution, it can be deleted safely.

## Troubleshooting

### Shortcut was not created

Run `create_desktop_shortcut.bat` instead of double-clicking the `.ps1` file.

### The shortcut opens but nothing happens

Check:

- Python installation
- Mosquitto installation, unless Mosquitto is already running as a service
- `requirements.txt` dependencies installed
- Windows Script Host and PowerShell not blocked by local policy

### No broker console appears

This can be normal. If Mosquitto is already running on port `1883`, the launcher uses that existing broker and opens only the GUI.

To check manually:

```cmd
netstat -ano | findstr :1883
```


### `Mosquitto closed immediately` after reboot

This message means the launcher attempted to start Mosquitto, but the broker process closed immediately:

```text
Mosquitto closed immediately. Check whether port 1883 is already in use or whether mosquitto.conf is valid.
```

The usual cause is not the GUI or firmware. It is that Windows already started Mosquitto as a background service at boot, so port `1883` is already occupied.

Check port `1883`:

```cmd
netstat -ano | findstr :1883
```

Then identify the process:

```cmd
tasklist /FI "PID eq <PID>"
```

If it is `mosquitto.exe`, the broker is already running. For the AYCE workflow, the cleanest setup is to make the Mosquitto service **Manual** instead of **Automatic**, so the AYCE launcher can start the visible broker console when needed.

Using Windows Services:

1. Press `Win + R`.
2. Run `services.msc`.
3. Find **Mosquitto Broker** or **mosquitto**.
4. Click **Stop** if it is running.
5. Open **Properties**.
6. Set **Startup type** to **Manual**.

Using Command Prompt as Administrator:

```cmd
sc stop mosquitto
sc config mosquitto start= demand
```

Then close any old AYCE windows and open the **AYCE Smart Plug** desktop shortcut again.

### The broker console opens but the GUI does not

Most likely a missing Python dependency. From the `Software` folder run:

```cmd
pip install -r requirements.txt
```
