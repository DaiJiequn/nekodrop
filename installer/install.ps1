#Requires -RunAsAdministrator
# Installs nekodrop from the staged folder it sits in. Changes, in order:
#   1. copies the files to %ProgramFiles%\nekodrop
#   2. makes a code-signing certificate on this PC, trusts it, signs the
#      register-proxy driver with it and deletes its private key (nothing else
#      can ever be signed with what this PC now trusts); adds the driver to the
#      driver store, which takes over nothing by itself
#   3. installs TAP-Windows if no tap0901 adapter exists (concurrent mode)
#   4. two inbound firewall rules, link-local addresses only
#   5. the NekoDropService service, started
# The Wi-Fi card is only taken by the service, and given back on uninstall.
# -DryRun says what would happen and changes nothing.
param([switch]$DryRun, [string]$Target = "$env:ProgramFiles\nekodrop")
$ErrorActionPreference = 'Stop'
$Source = $PSScriptRoot
$CertSubject = 'CN=nekodrop driver (signed on this PC)'
$TrustStores = 'Root', 'TrustedPublisher'
$FirewallGroup = 'nekodrop AirDrop'
$Service = 'NekoDropService'

function Step($what, [scriptblock]$do) {
  "  $what"
  if (-not $DryRun) { & $do }
}

if (-not (Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'PCI\\VEN_8086&DEV_(43F0|A0F0|02F0|06F0|34F0|3DF0|4DF0)' })) {
  throw 'This PC has no Intel Wi-Fi 6 AX201-family card; nekodrop cannot run here.'
}
$runtime = & dotnet --list-runtimes 2>$null | Where-Object { $_ -match '^Microsoft\.WindowsDesktop\.App 8\.' }
if (-not $runtime) { throw '.NET 8 Desktop Runtime is required: winget install Microsoft.DotNet.DesktopRuntime.8' }

'Files'
Step 'stop an earlier service' {
  if (Get-Service $Service -ErrorAction SilentlyContinue) { Stop-Service $Service -Force -ErrorAction SilentlyContinue }
  Get-Process nekodrop, nekodrop-radio, awdld -ErrorAction SilentlyContinue | Stop-Process -Force
}
Step "copy to $Target" {
  New-Item -ItemType Directory -Force $Target | Out-Null
  Copy-Item "$Source\*" $Target -Recurse -Force -Exclude 'install.ps1', 'tap-windows.exe'
}

'Driver'
$driver = Join-Path $Target 'driver'
Step 'sign the register-proxy driver with a certificate made and trusted on this PC' {
  foreach ($store in $TrustStores) {
    Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -eq $CertSubject } | Remove-Item
  }
  $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject `
            -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy NonExportable `
            -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(10)
  try {
    $public = Join-Path $driver 'nekodrop-signing.cer'
    Export-Certificate -Cert $cert -FilePath $public | Out-Null
    foreach ($store in $TrustStores) { Import-Certificate -FilePath $public -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null }
    foreach ($file in 'winawdl_regproxy.cat', 'winawdl_regproxy.dll') {
      $r = Set-AuthenticodeSignature -FilePath (Join-Path $driver $file) -Certificate $cert -HashAlgorithm SHA256
      if ($r.Status -ne 'Valid') { throw "$file could not be signed: $($r.StatusMessage)" }
    }
  } finally {
    Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey
  }
}
Step 'add it to the driver store (binds nothing)' {
  pnputil /add-driver (Join-Path $driver 'winawdl_regproxy.inf') | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "pnputil could not add the driver ($LASTEXITCODE)" }
}

'Network'
Step 'TAP-Windows adapter for Wi-Fi beside AirDrop' {
  $tap = Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | Where-Object { $_.InterfaceDescription -like 'TAP-Windows*' }
  if (-not $tap -and (Test-Path "$Source\tap-windows.exe")) {
    $p = Start-Process "$Source\tap-windows.exe" -ArgumentList '/S', '/SELECT_UTILITIES=0' -Wait -PassThru
    if ($p.ExitCode -ne 0) { "    TAP-Windows did not install ($($p.ExitCode)); concurrent mode will fall back to on-demand" }
  }
}
Step 'firewall: AirDrop receiver and discovery, from link-local addresses only' {
  Get-NetFirewallRule -Group $FirewallGroup -ErrorAction SilentlyContinue | Remove-NetFirewallRule
  $common = @{ Group = $FirewallGroup; Direction = 'Inbound'; Action = 'Allow'; Profile = 'Any'; RemoteAddress = 'fe80::/64' }
  New-NetFirewallRule @common -DisplayName 'nekodrop AirDrop receiver' -Protocol TCP -LocalPort 8771 | Out-Null
  New-NetFirewallRule @common -DisplayName 'nekodrop AirDrop discovery' -Protocol UDP -LocalPort 5353 | Out-Null
}

'Service'
Step 'install and start NekoDropService (LocalSystem, automatic)' {
  $exe = Join-Path $Target 'NekoDropService.exe'
  if (-not (Get-Service $Service -ErrorAction SilentlyContinue)) {
    New-Service -Name $Service -BinaryPathName "`"$exe`"" -DisplayName 'nekodrop AirDrop' `
      -Description 'AirDrop on the built-in Intel Wi-Fi card: owns the card while AirDrop is on.' -StartupType Automatic | Out-Null
    sc.exe failure $Service reset= 86400 actions= restart/5000/restart/30000/""/0 | Out-Null
  }
  Start-Service $Service
}
if ($DryRun) { 'Dry run: nothing was changed.' } else { 'nekodrop is installed.' }
