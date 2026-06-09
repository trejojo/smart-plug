Option Explicit

Dim fso, shell, launcherDir, psScript, psExe, cmd
Set fso = CreateObject("Scripting.FileSystemObject")
Set shell = CreateObject("WScript.Shell")

launcherDir = fso.GetParentFolderName(WScript.ScriptFullName)
psScript = fso.BuildPath(launcherDir, "Start-AyceSystem.ps1")
psExe = shell.ExpandEnvironmentStrings("%SystemRoot%") & "\System32\WindowsPowerShell\v1.0\powershell.exe"

If Not fso.FileExists(psScript) Then
    MsgBox "Launcher script not found:" & vbCrLf & psScript, vbCritical, "AYCE Smart Plug Launcher"
    WScript.Quit 1
End If

cmd = """" & psExe & """ -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File """ & psScript & """"

' Window style 0 keeps the launcher hidden. The broker console, when launched by
' Start-AyceSystem.ps1, remains visible because it is started as a separate process.
shell.Run cmd, 0, False
