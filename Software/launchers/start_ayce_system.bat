@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
set "PS_SCRIPT=%SCRIPT_DIR%Start-AyceSystem.ps1"

if not exist "%PS_SCRIPT%" (
    echo Launcher script not found: "%PS_SCRIPT%"
    pause
    exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%PS_SCRIPT%"
exit /b %errorlevel%
