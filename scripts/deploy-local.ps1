#Requires -RunAsAdministrator
# Development: puts a freshly built radio and service from build\stage into an
# installed nekodrop and restarts the service. Logs to build\deploy.log.
param([string]$Target = "$env:ProgramFiles\nekodrop")
$repo = Split-Path $PSScriptRoot -Parent
Start-Transcript "$repo\build\deploy.log" -Force | Out-Null
try {
  Stop-Service NekoDropService -Force -ErrorAction SilentlyContinue
  Get-Process NekoDropService, nekodrop-radio, awdld -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep 2
  Copy-Item "$repo\build\stage\radio\nekodrop-radio.exe" "$Target\radio\" -Force
  Copy-Item "$repo\build\stage\NekoDropService.exe" "$Target\" -Force
  Set-Service NekoDropService -StartupType Automatic
  Start-Service NekoDropService
  Get-Item "$Target\radio\nekodrop-radio.exe", "$Target\NekoDropService.exe" | Select-Object Name, LastWriteTime
  'deployed'
} catch { $_ | Out-String }
Stop-Transcript | Out-Null
