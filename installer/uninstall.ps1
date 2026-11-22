#Requires -RunAsAdministrator
# Removes nekodrop: gives the Wi-Fi card back to its vendor driver first, then
# removes the service, the driver package, the certificate trust, the firewall
# rules, nekodrop's TAP adapter, settings and the files. Received files stay
# where they were saved. Best effort: a step that fails is reported in
# %ProgramData%\nekodrop-uninstall.log and the rest still runs (#22: failures
# were silent and left parts behind).
param([string]$Target = "$env:ProgramFiles\nekodrop")
$ErrorActionPreference = 'Continue'
$Service = 'NekoDropService'
$CertSubject = 'CN=nekodrop driver (signed on this PC)'
$Log = Join-Path $env:ProgramData 'nekodrop-uninstall.log'
"nekodrop uninstall $(Get-Date -Format s), target $Target" | Set-Content $Log -ErrorAction SilentlyContinue
$failures = 0
function Try-Step($what, [scriptblock]$do) {
  $what
  try { & $do 2>&1 | ForEach-Object { "  $_" } | Add-Content $Log -ErrorAction SilentlyContinue }
  catch { $script:failures++; "  FAILED: $what - $($_.Exception.Message)" | Tee-Object -Append $Log }
}

Try-Step 'Giving the Wi-Fi card back' {
  if (Get-Service $Service -ErrorAction SilentlyContinue) {
    Stop-Service $Service -Force -ErrorAction SilentlyContinue  # the card is given back below either way
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Service $Service).Status -ne 'Stopped' -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
  }
  # Everything running from the install folder, the AirDrop engine's python.exe too (#22).
  $root = [IO.Path]::GetFullPath($Target).TrimEnd('\') + '\'
  # Never by path under a drive root or a folder that is not nekodrop's.
  $byPath = $root.Length -gt 4 -and ((Test-Path (Join-Path $root 'NekoDropService.exe')) -or (Test-Path (Join-Path $root 'python\python.exe')))
  @(Get-Process nekodrop, nekodrop-radio, nekodrop-radio-rtl, awdld -ErrorAction SilentlyContinue) +
  @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
      try { $byPath -and $_.Path -and $_.Path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) } catch { $false } }) |
    Stop-Process -Force -ErrorAction SilentlyContinue
  $svc = Join-Path $Target 'NekoDropService.exe'
  if (Test-Path $svc) { & $svc --give-back }
  else {
    # A failed update can leave the folder without the service: restore.ps1
    # gives the card back on its own, from the same folder or this script's.
    $restore = @((Join-Path $Target 'restore.ps1'), (Join-Path $PSScriptRoot 'restore.ps1')) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($restore) { & $restore } else { throw "neither NekoDropService.exe nor restore.ps1 is in $Target" }
  }
}

Try-Step 'Service' {
  if (Get-Service $Service -ErrorAction SilentlyContinue) {
    sc.exe delete $Service | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "sc.exe delete exited $LASTEXITCODE" }
  }
}

Try-Step 'Driver package and certificate' {
  $store = [xml](pnputil /enum-drivers /format xml | Out-String)
  $store.PnpUtil.Driver | Where-Object { $_.OriginalName -eq 'winawdl_regproxy.inf' -and $_.ProviderName -eq 'winairdrop project' } |
    ForEach-Object {
      pnputil /delete-driver $_.DriverName /uninstall /force | Out-Null
      if ($LASTEXITCODE -ne 0) { throw "pnputil /delete-driver $($_.DriverName) exited $LASTEXITCODE" }
    }
  foreach ($s in 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$s" | Where-Object { $_.Subject -eq $CertSubject } | Remove-Item -ErrorAction Stop
  }
}

Try-Step 'Firewall' {
  Get-NetFirewallRule -Group 'nekodrop AirDrop' -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction Stop
}

# Only the adapter setup-tap.ps1 made and named "nekodrop"; the TAP-Windows
# driver stays, other programs (VPNs) may use it (#22).
Try-Step 'TAP adapter' {
  foreach ($a in @(Get-NetAdapter -Name 'nekodrop' -IncludeHidden -ErrorAction SilentlyContinue |
                   Where-Object { $_.InterfaceDescription -like 'TAP-Windows Adapter V9*' })) {
    if ($a.PnPDeviceID) {
      pnputil /remove-device $a.PnPDeviceID | Out-Null
      if ($LASTEXITCODE -ne 0) { throw "pnputil /remove-device exited $LASTEXITCODE" }
    }
  }
}

Try-Step 'Files' {
  Get-AppxPackage -AllUsers -Name nekodrop.shell -ErrorAction SilentlyContinue | Remove-AppxPackage -AllUsers -ErrorAction SilentlyContinue
  $client = Join-Path $Target 'nekodrop.exe'
  if (Test-Path $client) { & $client --unregister }
  foreach ($dir in $Target, "$env:ProgramData\nekodrop", "$env:LOCALAPPDATA\nekodrop") {
    if (Test-Path $dir) { Remove-Item $dir -Recurse -Force -ErrorAction Stop }
  }
}
if ($failures) { "nekodrop is removed, with $failures step(s) failed: see $Log" } else { 'nekodrop is removed.' }
