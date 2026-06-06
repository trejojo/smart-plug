$ErrorActionPreference = 'Stop'

$launcherDir = $PSScriptRoot
$projectRoot = Split-Path -Parent $launcherDir
$supervisor = Join-Path $launcherDir 'Start-AyceSystem.ps1'
$fallbackBat = Join-Path $launcherDir 'start_ayce_system.bat'
$icon = Join-Path $projectRoot 'assets\ayce_logo.ico'
$desktop = [Environment]::GetFolderPath('Desktop')
$shortcutPath = Join-Path $desktop 'AYCE Smart Plug.lnk'
$powershellExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'

if (-not (Test-Path $supervisor)) {
    throw "Supervisor script not found: $supervisor"
}
if (-not (Test-Path $powershellExe)) {
    throw "PowerShell executable not found: $powershellExe"
}

$wshShell = New-Object -ComObject WScript.Shell
$shortcut = $wshShell.CreateShortcut($shortcutPath)

# The shortcut launches hidden PowerShell directly. This avoids leaving an extra
# empty launcher console visible next to the broker console and GUI.
$shortcut.TargetPath = $powershellExe
$shortcut.Arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$supervisor`""
$shortcut.WorkingDirectory = $launcherDir
$shortcut.Description = 'Start the AYCE Smart Plug broker and GUI.'
if (Test-Path $icon) {
    $shortcut.IconLocation = $icon
}
$shortcut.Save()

Write-Host "Desktop shortcut created: $shortcutPath" -ForegroundColor Green
Write-Host "Target: $powershellExe"
Write-Host "Arguments: $($shortcut.Arguments)"
if (Test-Path $icon) {
    Write-Host "Icon:   $icon"
}
Write-Host "Manual fallback: $fallbackBat"
