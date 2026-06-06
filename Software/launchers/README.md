# Launcher System

This folder contains the official AYCE PC startup helpers.

## Files

| File | Purpose |
|---|---|
| `start_ayce_system.bat` | Main Windows entry point. Intended target for the desktop shortcut. |
| `Start-AyceSystem.ps1` | PowerShell supervisor that starts and monitors the broker and GUI. |
| `create_desktop_shortcut.ps1` | Creates the AYCE desktop shortcut with the AYCE icon. |
| `create_desktop_shortcut.bat` | Convenience wrapper to run the shortcut-creation PowerShell script on Windows. |
| `assets/ayce_logo.ico` | Icon used for the desktop shortcut. |

## Normal use

### One-time setup

Run one of the following once:

- `create_desktop_shortcut.bat` (recommended for most Windows users)
- or `create_desktop_shortcut.ps1`

This creates a desktop shortcut named:

```text
AYCE Smart Plug
```

The shortcut points to:

```text
launchers/start_ayce_system.bat
```

and uses:

```text
launchers/assets/ayce_logo.ico
```

as its icon.

### Everyday use

Double-click the **AYCE Smart Plug** desktop shortcut.

That shortcut launches `start_ayce_system.bat`, which in turn starts the hidden PowerShell supervisor `Start-AyceSystem.ps1`.

## What `Start-AyceSystem.ps1` does

The supervisor performs the following steps:

1. Resolves the project root and required file paths.
2. Detects Python automatically:
   - first `.venv_pc`,
   - then `.venv`,
   - otherwise system Python.
3. Verifies required Python modules:
   - `paho-mqtt`
   - `bleak`
4. Locates `mosquitto.exe`.
5. Starts the broker in a **visible console window**.
6. Starts the GUI using **`pythonw.exe`** so there is **no Python console window**.
7. Monitors both processes.
8. If one is closed, it closes the other.

## Shutdown behavior

The launcher supervises only the processes it started.

- Closing the GUI closes the broker console started by the launcher.
- Closing the broker console closes the GUI started by the launcher.

## Why there is both `.ps1` and `.bat`

The PowerShell scripts contain the main logic, but many Windows systems do not reliably execute `.ps1` files by double-clicking.

For that reason:

- `start_ayce_system.bat` is the safest shortcut target.
- `create_desktop_shortcut.bat` is the safest one-click shortcut installer.

The `.ps1` files remain the implementation layer.

## If a virtual environment exists

No problem. The launcher auto-detects `.venv_pc` or `.venv`. If neither exists, it falls back to system Python.

## If Mosquitto is already running elsewhere

The launcher is designed for the local AYCE workflow where it starts its own broker console. It supervises only the broker instance it launches itself.

## Troubleshooting

### Shortcut was not created

Try `create_desktop_shortcut.bat` instead of double-clicking the `.ps1` file.

### The shortcut opens but nothing happens

Check:

- Python installation
- Mosquitto installation
- `requirements.txt` dependencies installed
- PowerShell not blocked by local policy

### The broker console opens but the GUI does not

Most likely a missing Python dependency. Install:

```cmd
pip install -r ..equirements.txt
```
