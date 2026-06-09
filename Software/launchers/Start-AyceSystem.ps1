$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationFramework

# Prevent project __pycache__ folders from being created by launcher-based runs.
$env:PYTHONDONTWRITEBYTECODE = '1'

function Show-LauncherError {
    param([string]$Message)
    [System.Windows.MessageBox]::Show($Message, 'AYCE Smart Plug Launcher', 'OK', 'Error') | Out-Null
}

function Get-FirstExistingPath {
    param([string[]]$Candidates)
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return $candidate
        }
    }
    return $null
}

function Stop-ProcessTree {
    param([int]$ProcessId)
    if ($ProcessId -le 0) { return }
    try {
        Start-Process -FilePath 'taskkill.exe' -ArgumentList @('/PID', "$ProcessId", '/T', '/F') -WindowStyle Hidden -Wait -ErrorAction SilentlyContinue | Out-Null
    } catch {
        try { Stop-Process -Id $ProcessId -Force -ErrorAction SilentlyContinue } catch {}
    }
}

function Resolve-PythonRuntime {
    param([string]$ProjectRoot)

    $venvPython = Get-FirstExistingPath @(
        (Join-Path $ProjectRoot '.venv_pc\Scripts\python.exe'),
        (Join-Path $ProjectRoot '.venv\Scripts\python.exe')
    )
    if ($venvPython) {
        $pythonw = Join-Path (Split-Path $venvPython -Parent) 'pythonw.exe'
        if (-not (Test-Path $pythonw)) { $pythonw = $venvPython }
        return [pscustomobject]@{ Python = $venvPython; Pythonw = $pythonw; Source = 'virtual environment' }
    }

    $python = (Get-Command python.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source)
    $pythonw = (Get-Command pythonw.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source)
    if ($python) {
        if (-not $pythonw) {
            $candidate = Join-Path (Split-Path $python -Parent) 'pythonw.exe'
            if (Test-Path $candidate) { $pythonw = $candidate }
        }
        if (-not $pythonw) { $pythonw = $python }
        return [pscustomobject]@{ Python = $python; Pythonw = $pythonw; Source = 'system Python' }
    }

    return $null
}

function Resolve-MosquittoExe {
    $cmd = Get-Command mosquitto.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
    if ($cmd) { return $cmd }
    return Get-FirstExistingPath @(
        'C:\Program Files\Mosquitto\mosquitto.exe',
        'C:\Program Files (x86)\Mosquitto\mosquitto.exe'
    )
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$telemetryDir = Join-Path $projectRoot 'telemetry'
$guiScript = Join-Path $telemetryDir 'smartplug_gui.py'
$mosquittoConf = Join-Path $telemetryDir 'mosquitto.conf'

if (-not (Test-Path $guiScript)) {
    Show-LauncherError "GUI script not found:`n$guiScript"
    exit 1
}
if (-not (Test-Path $mosquittoConf)) {
    Show-LauncherError "Mosquitto configuration not found:`n$mosquittoConf"
    exit 1
}

$pythonRuntime = Resolve-PythonRuntime -ProjectRoot $projectRoot
if (-not $pythonRuntime) {
    Show-LauncherError "Python was not found. Install Python 3 or create .venv_pc/.venv in the Software folder."
    exit 1
}

$dependencyCheck = & $pythonRuntime.Python -B -c "import paho.mqtt.client, bleak" 2>&1
if ($LASTEXITCODE -ne 0) {
    $msg = "Required Python modules are missing for the selected interpreter.`n`nInterpreter: $($pythonRuntime.Python)`nSource: $($pythonRuntime.Source)`n`nInstall them from the Software folder with:`n`npip install -r requirements.txt`n`nDetails:`n$dependencyCheck"
    Show-LauncherError $msg
    exit 1
}

$mosquittoExe = Resolve-MosquittoExe
if (-not $mosquittoExe) {
    Show-LauncherError "Mosquitto was not found. Install Mosquitto or add mosquitto.exe to PATH."
    exit 1
}

# The broker console is intentionally visible. /c is used instead of /k so that
# if Mosquitto exits, the console closes and the supervisor can close the GUI too.
$brokerCommand = 'title AYCE MQTT Broker && "{0}" -c "{1}" -v' -f $mosquittoExe, $mosquittoConf
$brokerProc = Start-Process -FilePath 'cmd.exe' -ArgumentList @('/c', $brokerCommand) -WorkingDirectory $telemetryDir -PassThru
Start-Sleep -Milliseconds 1200

if ($brokerProc.HasExited) {
    Show-LauncherError "Mosquitto closed immediately. Check whether port 1883 is already in use or whether mosquitto.conf is valid."
    exit 1
}

$guiProc = Start-Process -FilePath $pythonRuntime.Pythonw -ArgumentList @('-B', 'smartplug_gui.py') -WorkingDirectory $telemetryDir -PassThru

try {
    while ($true) {
        Start-Sleep -Milliseconds 500
        $brokerProc.Refresh()
        $guiProc.Refresh()

        if ($guiProc.HasExited) {
            if (-not $brokerProc.HasExited) {
                Stop-ProcessTree -ProcessId $brokerProc.Id
            }
            break
        }

        if ($brokerProc.HasExited) {
            if (-not $guiProc.HasExited) {
                Stop-ProcessTree -ProcessId $guiProc.Id
            }
            break
        }
    }
}
finally {
    if ($brokerProc -and -not $brokerProc.HasExited) {
        Stop-ProcessTree -ProcessId $brokerProc.Id
    }
    if ($guiProc -and -not $guiProc.HasExited) {
        Stop-ProcessTree -ProcessId $guiProc.Id
    }
}
