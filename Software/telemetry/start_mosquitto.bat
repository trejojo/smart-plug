@echo off
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "CONFIG_PATH=%SCRIPT_DIR%mosquitto.conf"
set "MOSQUITTO_EXE="

if not exist "%CONFIG_PATH%" (
    echo Mosquitto config not found: "%CONFIG_PATH%"
    exit /b 1
)

for %%I in (mosquitto.exe) do set "MOSQUITTO_EXE=%%~$PATH:I"

if not defined MOSQUITTO_EXE (
    if exist "C:\Program Files\Mosquitto\mosquitto.exe" (
        set "MOSQUITTO_EXE=C:\Program Files\Mosquitto\mosquitto.exe"
    ) else if exist "C:\Program Files (x86)\Mosquitto\mosquitto.exe" (
        set "MOSQUITTO_EXE=C:\Program Files (x86)\Mosquitto\mosquitto.exe"
    )
)

if not defined MOSQUITTO_EXE (
    echo Mosquitto executable not found. Install Mosquitto or add it to PATH.
    exit /b 1
)

echo Starting Mosquitto...
echo Executable: %MOSQUITTO_EXE%
echo Config:     %CONFIG_PATH%
echo.

"%MOSQUITTO_EXE%" -c "%CONFIG_PATH%" -v
exit /b %errorlevel%
