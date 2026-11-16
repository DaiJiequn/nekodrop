#Requires -RunAsAdministrator
# Starts the Intel radio daemon on one AWDL channel, then winawdl's AWDL daemon
# on top of it. Ctrl+C stops both. Run scripts\take-card.ps1 first.
# Then, in a second window, winawdl's receiver:
#   third_party\winawdl\scripts\airdrop-receive.ps1
# -Country is the regulatory domain to ask the card for (CN, US, DE, JP...);
#   5 GHz channels need one that allows them.
# -MacType listener|ibss|sta selects the firmware MAC context type; listener
#   is what ForbiddenItlwm uses, the other two are fallbacks to try if the
#   card does not acknowledge the iPhone's frames.
param([ValidateSet(6, 44, 149)][int]$Channel = 6, [string]$Country = 'ZZ',
      [ValidateSet('listener', 'ibss', 'sta')][string]$MacType = 'listener',
      [ValidateSet(0, 20, 40, 80)][int]$Width = 0, [string]$DataRate = '24',
      [switch]$Detail, [switch]$Redact)
$ErrorActionPreference = 'Stop'

$repo  = Split-Path $PSScriptRoot -Parent
$radio = "$repo\build\iwxradio\winawdl_radio.exe"
$awdld = "$repo\third_party\winawdl\build\awdl\awdld.exe"
if (-not (Test-Path $radio)) { throw 'Run scripts\build-radio.ps1 first.' }
if (-not (Test-Path $awdld)) { throw 'Run scripts\build-awdl.ps1 first.' }

$radioArgs = @("$Channel", '--country', $Country, '--mac-type', $MacType, '--data-rate', $DataRate)
if ($Width) { $radioArgs += '--width', "$Width" }
$radioProcess = Start-Process $radio -ArgumentList $radioArgs -PassThru -NoNewWindow

$awdldArgs = @('-c', $Channel)
if ($Detail) { $awdldArgs += '-v' }
if ($Redact) { $awdldArgs += '-r' }
& $awdld @awdldArgs

if (-not $radioProcess.WaitForExit(5000)) { Stop-Process -Id $radioProcess.Id }
