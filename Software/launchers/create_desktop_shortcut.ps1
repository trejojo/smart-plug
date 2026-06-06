$ErrorActionPreference = 'Stop'

$launcherDir = $PSScriptRoot
$target = Join-Path $launcherDir 'start_ayce_system.bat'
$icon = Join-Path $launcherDir 'assets\ayce_logo.ico'
$desktop = [Environment]::GetFolderPath('Desktop')
$shortcutPath = Join-Path $desktop 'AYCE Smart Plug.lnk'

if (-not (Test-Path $target)) {
    throw "Launcher target not found: $target"
}

$wshShell = New-Object -ComObject WScript.Shell
$shortcut = $wshShell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $target
$shortcut.WorkingDirectory = $launcherDir
$shortcut.Description = 'Start the AYCE Smart Plug broker and GUI.'
if (Test-Path $icon) {
    $shortcut.IconLocation = $icon
}
$shortcut.Save()

Write-Host "Desktop shortcut created: $shortcutPath" -ForegroundColor Green
Write-Host "Target: $target"
if (Test-Path $icon) {
    Write-Host "Icon:   $icon"
}
