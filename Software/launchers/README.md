# Launcher System

This folder contains the official AYCE PC startup helpers.

## Files

| File | Purpose |
|---|---|
| `create_desktop_shortcut.bat` | Recommended one-time shortcut installer for Windows users. |
| `create_desktop_shortcut.ps1` | PowerShell implementation that creates the desktop shortcut. |
| `start_ayce_system_hidden.vbs` | Hidden Windows Script Host launcher used by the desktop shortcut. |
| `Start-AyceSystem.ps1` | Hidden supervisor that starts and monitors the broker and GUI. |

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
wscript.exe //B //Nologo start_ayce_system_hidden.vbs
```

The VBS launcher then starts `Start-AyceSystem.ps1` hidden. This avoids the extra empty Windows Terminal / PowerShell console that can appear when PowerShell is used directly as the shortcut target on Windows 11.

### Everyday use

Double-click the **AYCE Smart Plug** desktop shortcut.

Expected result:

1. One visible console named **AYCE MQTT Broker** opens for Mosquitto logs.
2. The GUI opens without a separate Python console.
3. The hidden supervisor monitors both processes.

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
5. Locates `mosquitto.exe`.
6. Starts the broker in a **visible console window**.
7. Starts the GUI using **`pythonw.exe -B`** so there is **no Python console window**.
8. Monitors both processes.
9. If one is closed, it closes the other using process-tree cleanup.

## Shutdown behavior

The launcher supervises only the processes it started.

- Closing the GUI closes the broker console and child processes.
- Closing the broker console closes the GUI process.

Cleanup uses:

```text
taskkill /PID <pid> /T /F
```

This is more robust than closing only the direct parent process.

## Why both `.ps1` and `.bat` exist

The PowerShell script contains the real shortcut-creation logic. The `.bat` wrapper exists so Windows users can create the desktop shortcut with a simple double-click.

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
- Mosquitto installation
- `requirements.txt` dependencies installed
- PowerShell not blocked by local policy

### The broker console opens but the GUI does not

Most likely a missing Python dependency. From the `Software` folder run:

```cmd
pip install -r requirements.txt
```
