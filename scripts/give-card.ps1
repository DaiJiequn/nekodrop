#Requires -RunAsAdministrator
# Hands the Wi-Fi card back to Intel's driver: removes the proxy package (so
# Windows rebinds the vendor driver) and the certificate trust.
# -RemoveSigningKey also deletes the local signing key. From winawdl's
# uninstall-driver.ps1.
param([switch]$RemoveSigningKey)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\driver-settings.ps1"

'Before:'
Show-WifiCardDriver

foreach ($package in Get-PublishedPackages) {
  pnputil /delete-driver $package /uninstall /force
}
pnputil /scan-devices | Out-Null

$trusted = foreach ($store in $DriverSettings.TrustStores) {
  Get-ChildItem "Cert:\LocalMachine\$store" |
    Where-Object { $_.Subject -eq $DriverSettings.CertSubject }
}
$trusted | Remove-Item

if ($RemoveSigningKey) {
  Get-ChildItem Cert:\CurrentUser\My |
    Where-Object { $_.Subject -eq $DriverSettings.CertSubject } |
    Remove-Item -DeleteKey
}

'After:'
Show-WifiCardDriver
