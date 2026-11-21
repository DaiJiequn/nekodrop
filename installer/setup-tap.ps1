#Requires -RunAsAdministrator
# nekodrop's own TAP-Windows adapter, named "nekodrop", on DHCP. In concurrent
# mode the radio bridges the Wi-Fi station to it, so Windows gets its address
# from the router through it. Never another program's TAP adapter: those may
# carry static addresses (a VPN's), which is what broke the first test.
param([string]$TapSetup = (Join-Path $PSScriptRoot 'tap-windows.exe'))
$ErrorActionPreference = 'Stop'
$name = 'nekodrop'
$tapinstall = Join-Path $env:ProgramFiles 'TAP-Windows\bin\tapinstall.exe'
$tapinf = Join-Path $env:ProgramFiles 'TAP-Windows\driver\OemVista.inf'

function TapAdapters { @(Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue |
                         Where-Object InterfaceDescription -like 'TAP-Windows Adapter V9*') }

if (-not (Get-NetAdapter -Name $name -IncludeHidden -ErrorAction SilentlyContinue)) {
  $before = TapAdapters | ForEach-Object InterfaceGuid
  if (-not (Test-Path $tapinstall)) {
    if (-not (Test-Path $TapSetup)) { throw "TAP-Windows installer not found: $TapSetup" }
    # Installs the driver and tapinstall; usually adds one adapter itself.
    $p = Start-Process $TapSetup -ArgumentList '/S', '/SELECT_UTILITIES=1' -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "TAP-Windows setup failed ($($p.ExitCode))" }
  }
  Start-Sleep 2
  $new = TapAdapters | Where-Object { $before -notcontains $_.InterfaceGuid } | Select-Object -First 1
  if (-not $new) {
    if (-not (Test-Path $tapinstall)) { throw 'tapinstall.exe is missing after TAP-Windows setup' }
    & $tapinstall install $tapinf tap0901 | Out-Host
    Start-Sleep 3
    $new = TapAdapters | Where-Object { $before -notcontains $_.InterfaceGuid } | Select-Object -First 1
  }
  if (-not $new) { throw 'no new TAP adapter appeared' }
  Rename-NetAdapter -Name $new.Name -NewName $name
  "created TAP adapter '$name' ($($new.InterfaceDescription))"
}
# A new adapter's IP interface turns up a few seconds after it does: setting
# it at once failed with "no MSFT_NetIPInterface object" (issue #12).
$deadline = (Get-Date).AddSeconds(30)
while (-not (Get-NetIPInterface -InterfaceAlias $name -AddressFamily IPv4 -ErrorAction SilentlyContinue)) {
  if ((Get-Date) -gt $deadline) {
    $a = Get-NetAdapter -Name $name -IncludeHidden -ErrorAction SilentlyContinue
    throw "the '$name' adapter has no IPv4 interface after 30 s (status: $(if ($a) { $a.Status } else { 'missing' }))"
  }
  Start-Sleep -Milliseconds 500
}
# Addresses from the router, whatever was set before.
Set-NetIPInterface -InterfaceAlias $name -AddressFamily IPv4 -Dhcp Enabled
Get-NetIPAddress -InterfaceAlias $name -AddressFamily IPv4 -PrefixOrigin Manual -ErrorAction SilentlyContinue |
  Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue
Get-NetRoute -InterfaceAlias $name -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue |
  Where-Object Protocol -eq 'NetMgmt' | Remove-NetRoute -Confirm:$false -ErrorAction SilentlyContinue
Set-DnsClientServerAddress -InterfaceAlias $name -ResetServerAddresses
# After any wired adapter: with the automatic metric both got 25 (issue #1).
Set-NetIPInterface -InterfaceAlias $name -InterfaceMetric 75 -ErrorAction SilentlyContinue
Get-NetIPInterface -InterfaceAlias $name -AddressFamily IPv4 | Select-Object InterfaceAlias, Dhcp, ConnectionState
