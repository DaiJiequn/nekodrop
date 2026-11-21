#Requires -RunAsAdministrator
# One-click rollback: puts the PC's Wi-Fi back the way Windows had it, whatever
# state nekodrop left it in (service running or crashed, card held by the
# register proxy or by the shelved kernel driver). Needs nothing from nekodrop
# itself, so it also works when nekodrop's own files are broken.
#   1. stops nekodrop and turns its service off (it would take the card again)
#   2. binds Intel's driver to the card again (the package Windows used before)
#   3. removes nekodrop's driver packages from the driver store
#   4. adds back saved Wi-Fi networks Windows lost (from nekodrop's backup)
#   5. turns the "nekodrop" TAP adapter off and makes sure WLAN AutoConfig runs
# nekodrop stays installed; run the installer again to use it, or
# -Uninstall to remove it completely afterwards.
# Log: %ProgramData%\nekodrop\restore.log
param([switch]$Uninstall)
$ErrorActionPreference = 'Continue'
$state = Join-Path $env:ProgramData 'nekodrop'
New-Item -ItemType Directory -Force $state | Out-Null
Start-Transcript (Join-Path $state 'restore.log') -Force | Out-Null

# Every card nekodrop's radio can take (iwxradio/src/cards.h).
$ids = '43F0|A0F0|02F0|06F0|34F0|3DF0|4DF0|2723|2725|7A70|7AF0|51F0|51F1|54F0|7F70'

'1. Stopping nekodrop'
$svc = Get-Service NekoDropService -ErrorAction SilentlyContinue
if ($svc) {
  Stop-Service NekoDropService -Force -ErrorAction SilentlyContinue
  Set-Service NekoDropService -StartupType Disabled
  '   service stopped and disabled'
}
Get-Process nekodrop-radio, awdld -ErrorAction SilentlyContinue | Stop-Process -Force
Get-CimInstance Win32_Process -Filter "Name='python.exe'" |
  Where-Object { $_.CommandLine -like '*airdrop*engine.py*' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

'2. Giving the Wi-Fi card back to Intel''s driver'
$dev = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match "PCI\\VEN_8086&DEV_($ids)" } | Select-Object -First 1
if (-not $dev) {
  '   no Intel Wi-Fi card nekodrop knows is present; nothing to give back'
} else {
  "   card: $($dev.InstanceId), now $($dev.FriendlyName) ($($dev.Status))"
  # Intel's package: the one nekodrop recorded before taking the card, else
  # the newest Intel network package in the driver store.
  $vendor = $null
  foreach ($f in (Join-Path $state 'vendor-inf.txt'), (Join-Path $state 'service.json')) {
    if (-not (Test-Path $f)) { continue }
    $text = Get-Content $f -Raw
    if ($f -like '*.json') { try { $text = ($text | ConvertFrom-Json).VendorInf } catch { $text = $null } }
    if ($text -and (Test-Path $text.Trim())) { $vendor = $text.Trim(); break }
  }
  if (-not $vendor) {
    $store = [xml](pnputil /enum-drivers /format xml | Out-String)
    $pkg = $store.PnpUtil.Driver | Where-Object { $_.ProviderName -match 'Intel' -and $_.ClassName -eq 'Net' } |
           Sort-Object { [version]($_.DriverVersion -split ' ')[-1] } -Descending -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if ($pkg) { $vendor = Join-Path $env:windir "INF\$($pkg.DriverName)" }
  }
  $bound = $false
  if ($vendor) {
    "   binding $vendor"
    $hwid = ($dev.InstanceId -split '&SUBSYS')[0]
    # INSTALLFLAG_FORCE: Intel's INF ranks higher anyway, but the proxy is bound now.
    $job = Start-Job -ScriptBlock {
      param($h, $v)
      Add-Type -Namespace NekoRestore -Name NewDev -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("newdev.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, SetLastError = true)]
public static extern bool UpdateDriverForPlugAndPlayDevicesW(System.IntPtr hwnd, string hardwareId, string infPath, uint flags, out bool reboot);
'@
      $r = $false
      [NekoRestore.NewDev]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero, $h, $v, 1, [ref]$r)
    } -ArgumentList $hwid, $vendor
    # The call has been seen to block for minutes: wait at most 90 s, then check.
    if (Wait-Job $job -Timeout 90) { $bound = [bool](Receive-Job $job | Select-Object -Last 1) }
    Remove-Job $job -Force -ErrorAction SilentlyContinue
  } else {
    '   Intel''s driver package was not found in the driver store'
  }

  '3. Removing nekodrop''s driver packages'
  $store = [xml](pnputil /enum-drivers /format xml | Out-String)
  $store.PnpUtil.Driver |
    Where-Object { $_.OriginalName -in 'winawdl_regproxy.inf', 'nekodrop-wlan.inf' } |
    ForEach-Object { pnputil /delete-driver $_.DriverName /uninstall /force | Out-Null; "   removed $($_.OriginalName) ($($_.DriverName))" }

  # If the bind did not take, let Plug and Play choose again: with nekodrop's
  # packages gone, Intel's is the best match left.
  $now = Get-PnpDevice -InstanceId $dev.InstanceId
  $service = (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data
  if (-not $bound -or $service -in 'WUDFRd', 'nekodrop-wlan') {
    '   re-detecting the card'
    pnputil /remove-device $dev.InstanceId | Out-Null
    pnputil /scan-devices | Out-Null
    Start-Sleep 5
  }
}

'4. Saved Wi-Fi networks'
# The service backs up Windows' saved networks before taking the card; add
# back any the Wi-Fi interface lacks now (it can come back under a new GUID).
$backup = Join-Path $state 'wlan-backup'
if (Test-Path $backup) {
  Start-Service WlanSvc -ErrorAction SilentlyContinue
  for ($i = 0; $i -lt 30 -and -not ((netsh wlan show interfaces | Out-String) -match 'GUID'); $i++) { Start-Sleep 1 }
  # The interface's own profiles, read back through netsh (its text output is localized).
  $tmp = Join-Path $state 'wlan-current'
  Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force $tmp | Out-Null
  netsh wlan export profile folder="$tmp" | Out-Null
  $have = @(Get-ChildItem $tmp -Filter *.xml | ForEach-Object { try { ([xml](Get-Content $_.FullName -Raw)).WLANProfile.name } catch { } })
  Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
  $added = 0
  foreach ($f in Get-ChildItem $backup -Filter *.xml) {
    try { $name = ([xml](Get-Content $f.FullName -Raw)).WLANProfile.name } catch { continue }
    if ($have -contains $name) { continue }
    netsh wlan add profile filename="$($f.FullName)" user=all | Out-Null
    if ($LASTEXITCODE -eq 0) { $added++ }
  }
  "   restored $added saved network(s)"
} else {
  '   no backup (nekodrop before 0.1.9 kept none)'
}

'5. Network'
Get-NetAdapter -Name nekodrop -ErrorAction SilentlyContinue | Disable-NetAdapter -Confirm:$false -ErrorAction SilentlyContinue
Set-Service WlanSvc -StartupType Automatic -ErrorAction SilentlyContinue
Start-Service WlanSvc -ErrorAction SilentlyContinue

''
$after = if ($dev) { Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match "PCI\\VEN_8086&DEV_($ids)" } | Select-Object -First 1 }
if ($after) {
  $service = (Get-PnpDeviceProperty -InstanceId $after.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data
  "Wi-Fi card: $($after.FriendlyName), driver service $service, status $($after.Status)"
  if ($service -in 'WUDFRd', 'nekodrop-wlan') {
    'The card is still not on Intel''s driver. Restart the PC; if Wi-Fi is still missing, install Intel''s Wi-Fi driver from intel.com or Windows Update.'
  } else {
    'Done: Windows Wi-Fi is back. A restart is not needed, but does no harm.'
  }
}
if ($Uninstall) {
  $un = Join-Path $env:ProgramFiles 'nekodrop\uninstall.ps1'
  if (Test-Path $un) { 'Uninstalling nekodrop'; & $un }
}
Stop-Transcript | Out-Null
