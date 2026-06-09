Option Explicit

Dim shell, fso, scriptDir, psScript, command
Set shell = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")

scriptDir = fso.GetParentFolderName(WScript.ScriptFullName)
psScript = fso.BuildPath(scriptDir, "Start-AyceSystem.ps1")

shell.CurrentDirectory = scriptDir
command = "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File " & Chr(34) & psScript & Chr(34)

' Window style 0 keeps the supervisor hidden. The supervisor will still open
' the AYCE MQTT Broker console intentionally and will start the GUI with pythonw.
shell.Run command, 0, False
