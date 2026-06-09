$ErrorActionPreference = 'Stop'

$launcherDir = $PSScriptRoot
$projectRoot = Split-Path -Parent $launcherDir
$hiddenLauncher = Join-Path $launcherDir 'start_ayce_system_hidden.vbs'
$fallbackBat = Join-Path $launcherDir 'start_ayce_system.bat'
$icon = Join-Path $projectRoot 'assets\ayce_logo.ico'
$desktop = [Environment]::GetFolderPath('Desktop')
$shortcutPath = Join-Path $desktop 'AYCE Smart Plug.lnk'
$wscriptExe = Join-Path $env:SystemRoot 'System32\wscript.exe'

if (-not (Test-Path $hiddenLauncher)) {
    throw "Hidden launcher not found: $hiddenLauncher"
}
if (-not (Test-Path $wscriptExe)) {
    throw "Windows Script Host executable not found: $wscriptExe"
}

$wshShell = New-Object -ComObject WScript.Shell
$shortcut = $wshShell.CreateShortcut($shortcutPath)

# The shortcut launches the VBS helper through wscript.exe. This avoids leaving
# an extra empty PowerShell/launcher console visible next to the broker and GUI.
$shortcut.TargetPath = $wscriptExe
$shortcut.Arguments = "`"$hiddenLauncher`""
$shortcut.WorkingDirectory = $launcherDir
$shortcut.Description = 'Start the AYCE Smart Plug broker and GUI.'
if (Test-Path $icon) {
    $shortcut.IconLocation = $icon
}
$shortcut.Save()

Write-Host "Desktop shortcut created: $shortcutPath" -ForegroundColor Green
Write-Host "Target: $wscriptExe"
Write-Host "Arguments: $($shortcut.Arguments)"
if (Test-Path $icon) {
    Write-Host "Icon:   $icon"
}
Write-Host "Manual fallback: $fallbackBat"
