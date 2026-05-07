# ESP32-S3 Firmware Development Setup Guide

This folder contains the firmware for the **SmartPlug** project. This guide explains how to set up the environment using both Visual Studio Code (VS Code) and the standalone Windows Terminal (Command Line) to ensure a robust development workflow.

---

## 1. VS Code Configuration (Recommended IDE)

To maintain consistency across the team, we recommend creating a dedicated **VS Code Profile** and installing the following extensions:

### Required Extensions
* **C/C++**: Core support for C and C++.
* **C/C++ DevTools**: Enhanced development tools for C++ projects.
* **C/C++ Extension Pack**: A bundle of essential extensions for C/C++ development.
* **C/C++ Themes**: Visual themes optimized for code readability.
* **CMake Tools**: Support for the CMake build system used by ESP-IDF.
* **Dev Containers**: (Optional) For developing inside a Docker container.
* **ESP-IDF Extension**: The official Espressif extension for building, flashing, and monitoring.
* **GitLens**: Advanced Git capabilities to track changes and authorship.
* **Pylance**: High-performance Python language support.
* **Python**: Core Python support (required for ESP-IDF scripts).
* **Python Environments**: Management of Python virtual environments.

---

## 2. Standalone ESP-IDF Setup 

You can manage the entire project via **Windows Terminal (PowerShell or CMD)**. This method is more reliable as it removes the IDE abstraction layer. 
(We had a lot of issues by trying with the VSCode extenssion)

### A. Download the Offline Installer
1.  Go to the [ESP-IDF Tools Installer](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/windows-setup.html#esp-idf-tools-installer).
2.  Download the **Universal Online Installer** or **Offline Installer** (v5.x recommended).
3.  During installation, it will download Python, Git, and the Xtensa toolchains.

### B. Setting up Environment Variables
The installer creates a shortcut called **"ESP-IDF 5.x PowerShell"** or **"ESP-IDF 5.x Command Prompt"**. 
* **Using the Shortcut:** Open it, and it will automatically run `export.ps1` or `export.bat` to add `idf.py` to your PATH.
* **Using Windows Terminal (Manual):** If you want to use your own terminal, navigate to your ESP-IDF installation directory and run:
    ```powershell
    # For PowerShell
    . ./export.ps1
    # For Command Prompt
    export.bat
    ```

### C. Building and Flashing from Terminal
Once your environment is set up, navigate to the `Firmware/ESP_SmartPlug` directory and use these commands:

1.  **Set Target:** Tell the system you are using the ESP32-S3.
    ```bash
    idf.py set-target esp32s3
    ```
2.  **Build Project:**
    ```bash
    idf.py build
    ```
3.  **Flash to Device:** (Replace `COMx` with your port, e.g., `COM3`)
    ```bash
    idf.py -p COMx flash
    ```
4.  **Monitor Output:**
    ```bash
    idf.py -p COMx monitor
    ```
    *Note: Press `Ctrl + ]` to exit the monitor.*

---

## 3. Project Structure
* `/main`: Contains the C source files (`module_ble.c`, `module_nvs.c`, etc.).
* `/components`: Custom drivers for ADE7953 and sensors.
* `sdkconfig`: Project configuration (managed via `idf.py menuconfig`).

## 4. Troubleshooting
* **PermissionError:** Ensure VS Code or any other serial monitor is closed before flashing.
* **Missing Header Files:** If VS Code shows red squiggles, ensure you have run `idf.py build` at least once so the `compile_commands.json` is generated.
