@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
cd /d "%SCRIPT_DIR%"
python smartplug_gui.py
exit /b %errorlevel%
