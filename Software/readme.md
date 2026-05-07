# PC Software & Provisioning Setup

This guide explains how to set up your local PC environment to communicate with the SmartPlug via Bluetooth Low Energy (BLE) and handle telemetry data.

---

## 1. Local Environment Setup

Before running the scripts, you must create an isolated Python environment to avoid library conflicts.

### A. Create the Virtual Environment

Open your terminal in the `Software` directory and run:

```powershell
python -m venv .venv_pc
```

### B. Activate the Environment

#### PowerShell

```powershell
.\.venv_pc\Scripts\Activate.ps1
```

#### Command Prompt (CMD)

```cmd
.\.venv_pc\Scripts\activate.bat
```

### C. Install Dependencies

Once the environment is active (you should see `(.venv_pc)` in your terminal), install the required libraries:

```powershell
pip install -r requirements.txt
```

---

## 2. VS Code Configuration

To ensure VS Code uses the correct libraries and provides proper IntelliSense, follow these steps:

### How to Set `.venv_pc` as Active in VS Code

1. Open the Command Palette with:

   ```text
   Ctrl + Shift + P
   ```

2. Run:

   ```text
   Python: Select Interpreter
   ```

3. Select the interpreter that points to your local virtual environment:

   ```text
   .venv_pc\Scripts\python.exe
   ```

4. Reload the VS Code window by opening the Command Palette again and running:

   ```text
   Developer: Reload Window
   ```

> After reloading, verify that the bottom-right status bar in VS Code shows `(.venv_pc)`.

---

## 3. Usage: Provisioning the Device

### Prerequisites

- Ensure your PC Bluetooth is turned ON
- Enable your Hotspot (set to 2.4 GHz band)
- Ensure the ESP32 is powered and in BLE Advertising mode

### Run Provisioning Script

```powershell
python provisioning/provisioner.py
```

### At this point "Provisioner.py" should...

1. Scan for a nearby SmartPlug device (15-second timeout)
2. Connect via BLE
3. Verify the provisioning service UUID
4. Send Wi-Fi credentials as a JSON payload

### Troubleshooting

1. SmartPlug not found: Check if the ESP32 is powered and advertising BLE.

2. Connection timeout: The device may have disconnected. Try again.

3. Service UUID not found: Firmware and client UUID definitions may be mismatched.

---

## Configuration Reference

### Service UUID (Provisioning)

```text
f0debc9a-7856-3412-7856-341278563412
```

### Characteristic UUID (JSON Credentials)

```text
f1debc9a-7856-3412-7856-341278563412
```

### Device Name

```text
SmartPlug
```