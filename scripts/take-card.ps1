#Requires -RunAsAdministrator
# Gives the Intel Wi-Fi card to the register-proxy driver, so the radio daemon
# can drive it. WINDOWS LOSES WI-FI while the card is taken: have a cable or
# USB tethering up first. give-card.ps1 hands the card back to Intel's driver.
# From winawdl's install-driver.ps1.
param([switch]$Force)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\toolchain.ps1"
. "$PSScriptRoot\driver-settings.ps1"

$package = $DriverSettings.PackageDir
$inf = Join-Path $package $DriverSettings.InfName
$certFile = Join-Path $package $DriverSettings.CertFile
if (-not (Test-Path $certFile)) { throw 'Run scripts\package-driver.ps1 first.' }
$hardwareId = Get-WifiCardHardwareId
if (-not $hardwareId) { throw 'No Intel AX201-family card found.' }
if (-not (Test-OtherNetwork) -and -not $Force) {
  throw ('No other network connection is up. Taking the card turns Wi-Fi off; ' +
         'plug in a cable or USB tethering first, or pass -Force.')
}

'Before:'
Show-WifiCardDriver

foreach ($store in $DriverSettings.TrustStores) {
  Import-Certificate -FilePath $certFile -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
}
pnputil /add-driver $inf
if ($LASTEXITCODE -ne 0) { throw "pnputil could not add the package ($LASTEXITCODE)" }

# Intel's driver ranks higher, so the switch has to be forced.
$tools = Initialize-Toolchain
& $tools.DevCon update $inf $hardwareId
$devconExit = $LASTEXITCODE

'After:'
Show-WifiCardDriver
if ($devconExit -eq 1) { 'devcon reports that a reboot is needed.' }
if ($devconExit -gt 1) { throw "devcon could not bind the driver ($devconExit)" }
