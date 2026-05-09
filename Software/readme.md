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

- **SmartPlug not found:** Check if the ESP32 is powered and advertising BLE.

- **Connection timeout:** The device may have disconnected. Try again.

- **Service UUID not found:** Firmware and client UUID definitions may be mismatched.

---

## 4. MQTT Broker & Telemetry Setup

After the ESP32 is provisioned and connected to Wi-Fi, it can send telemetry data via MQTT to your PC.

### A. Install Mosquitto MQTT Broker (Windows)

1. Download Mosquitto from: https://mosquitto.org/download/
2. Run the installer (choose default options)
3. During installation, Mosquitto will be registered as a Windows Service

### B. Configure Mosquitto

1. Locate the configuration file:
   - Default path: `C:\Program Files\mosquitto\mosquitto.conf`

2. Replace its contents with the configuration from `mosquitto.conf` in this repository, or manually add:

   ```conf
   listener 1883
   allow_anonymous true
   ```

3. Save the file

### C. Start the MQTT Broker

**Option 1: Using Windows Services (Recommended)**
- Open `Services.msc`
- Find "Mosquitto Broker"
- Click "Start" (or set to auto-start)

**Option 2: Command Line**
```powershell
mosquitto -c "C:\Program Files\mosquitto\mosquitto.conf"
```

**Verify it's running:**
```powershell
netstat -an | findstr 1883
```
You should see a line with `LISTENING` on port 1883.

### D. Run the MQTT Telemetry Client

Once the broker is running and the ESP32 is connected to Wi-Fi, start the telemetry listener:

```powershell
python telemetry/mqtt_client.py
```

### What the Client Does

- Connects to the local MQTT broker (127.0.0.1:1883)
- Subscribes to all SmartPlug topics (smartplug/*)
- Displays received messages with timestamps
- Pretty-prints JSON payloads for readability

### Troubleshooting

- **Connection refused:** Broker is not running. Start Mosquitto first.
- **No messages received:** Check that the ESP32 is connected to Wi-Fi (check IP in ESP32 logs)
- **Port 1883 in use:** Another application is using the port. Change the port in mosquitto.conf and mqtt_client.py

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