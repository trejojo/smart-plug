$ErrorActionPreference = 'Stop'

$launcherDir = $PSScriptRoot
$projectRoot = Split-Path -Parent $launcherDir
$hiddenLauncher = Join-Path $launcherDir 'start_ayce_system_hidden.vbs'
$supervisor = Join-Path $launcherDir 'Start-AyceSystem.ps1'
$icon = Join-Path $projectRoot 'assets\ayce_logo.ico'
$desktop = [Environment]::GetFolderPath('Desktop')
$shortcutPath = Join-Path $desktop 'AYCE Smart Plug.lnk'
$wscriptExe = Join-Path $env:SystemRoot 'System32\wscript.exe'

if (-not (Test-Path $hiddenLauncher)) {
    throw "Hidden launcher not found: $hiddenLauncher"
}
if (-not (Test-Path $supervisor)) {
    throw "Supervisor script not found: $supervisor"
}
if (-not (Test-Path $wscriptExe)) {
    throw "Windows Script Host executable not found: $wscriptExe"
}

$wshShell = New-Object -ComObject WScript.Shell
$shortcut = $wshShell.CreateShortcut($shortcutPath)

# The desktop shortcut uses wscript.exe + a tiny .vbs launcher instead of
# powershell.exe directly. This avoids the extra empty Windows Terminal/console
# tab that can appear when PowerShell is the shortcut target on Windows 11.
$shortcut.TargetPath = $wscriptExe
$shortcut.Arguments = "//B //Nologo `"$hiddenLauncher`""
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
