@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
set "PS_SCRIPT=%SCRIPT_DIR%create_desktop_shortcut.ps1"

if not exist "%PS_SCRIPT%" (
    echo Shortcut creation script not found: "%PS_SCRIPT%"
    pause
    exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PS_SCRIPT%"
set "ERR=%errorlevel%"
if not "%ERR%"=="0" (
    echo.
    echo Shortcut creation failed.
    pause
    exit /b %ERR%
)

echo.
echo Desktop shortcut created successfully.
pause
exit /b 0
