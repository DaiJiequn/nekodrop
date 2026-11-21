# nekodrop diagnostic report: everything a bug report needs, in one file.
# Start menu -> "nekodrop - diagnostic report", the tray menu, or run this.
# Needs administrator rights (the service's logs and the driver state are
# readable by administrators only); it asks for them itself.
#
# Writes two files to the Desktop:
#   nekodrop-report-<time>.md   summary, bring-up checklist, system, card,
#                               network, key log lines (paste into an issue)
#   nekodrop-logs-<time>.txt    the full logs (attach to the issue)
# SSIDs, MAC addresses, private IP addresses, the user name and the computer
# name are replaced by placeholders. Nothing is sent anywhere.
param(
  [string]$OutDir = '',
  [string]$UserData = '',
  [string]$UserName = '',
  [switch]$NoOpen
)
$ErrorActionPreference = 'Continue'
Set-StrictMode -Off

# --------------------------------------------------------------- elevation
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
  [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
  # The signed-in user's Desktop and engine log, before an elevation that may
  # run as another account.
  $desk = [Environment]::GetFolderPath('Desktop')
  $argLine = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -OutDir `"$desk`" " +
             "-UserData `"$env:LOCALAPPDATA`" -UserName `"$env:USERNAME`""
  if ($NoOpen) { $argLine += ' -NoOpen' }
  try { Start-Process powershell.exe -ArgumentList $argLine -Verb RunAs | Out-Null }
  catch { Write-Host 'nekodrop: the diagnostic report needs administrator rights.' }
  exit
}
if (-not $OutDir) { $OutDir = [Environment]::GetFolderPath('Desktop') }
if (-not $UserData) { $UserData = $env:LOCALAPPDATA }
if (-not $UserName) { $UserName = $env:USERNAME }
New-Item -ItemType Directory -Force $OutDir | Out-Null

$state = Join-Path $env:ProgramData 'nekodrop'
$app = $PSScriptRoot
# Downloaded on its own (for an older nekodrop): the installed copy's folder.
if (-not (Test-Path (Join-Path $app 'NekoDropService.exe'))) { $app = Join-Path $env:ProgramFiles 'nekodrop' }
$stamp = Get-Date -Format 'yyyyMMdd-HHmm'
$reportPath = Join-Path $OutDir "nekodrop-report-$stamp.md"
$logsPath = Join-Path $OutDir "nekodrop-logs-$stamp.txt"
Write-Host 'nekodrop: collecting the diagnostic report...'

# ---------------------------------------------------------------- helpers
function Read-Tail([string]$path, [int]$n) {
  if (-not (Test-Path $path)) { return @() }
  try { return @(Get-Content -LiteralPath $path -Tail $n -Encoding UTF8 -ErrorAction Stop) } catch { return @() }
}
function Read-All([string]$path) {
  if (-not (Test-Path $path)) { return @() }
  try { return @(Get-Content -LiteralPath $path -Encoding UTF8 -ErrorAction Stop) } catch { return @() }
}
function Try-Get([scriptblock]$block) { try { & $block } catch { $null } }
# netsh's output: UTF-8 on some systems, the OEM code page on others (it came
# out garbled on a Traditional Chinese Windows 11, issue #11). Strict UTF-8
# first, then the OEM code page.
function Run-Netsh([string[]]$netshArgs) {
  $tmp = [IO.Path]::GetTempFileName()
  try {
    $p = Start-Process netsh.exe -ArgumentList $netshArgs -NoNewWindow -Wait -PassThru -RedirectStandardOutput $tmp
    $bytes = [IO.File]::ReadAllBytes($tmp)
    try { $text = (New-Object System.Text.UTF8Encoding($false, $true)).GetString($bytes) }
    catch { $text = [Text.Encoding]::GetEncoding([Globalization.CultureInfo]::CurrentCulture.TextInfo.OEMCodePage).GetString($bytes) }
    return @($text -split "`r?`n")
  } catch { return @() } finally { Remove-Item $tmp -ErrorAction SilentlyContinue }
}

# Service log, the rotated copy first (oldest lines first).
$serviceLog = @(Read-All (Join-Path $state 'service.log.1')) + @(Read-All (Join-Path $state 'service.log'))
$engineDir = Join-Path $UserData 'nekodrop'
$engineLog = @(Read-All (Join-Path $engineDir 'engine.log.old')) + @(Read-All (Join-Path $engineDir 'engine.log'))
$installLog = Read-All (Join-Path $state 'install.log')
$restoreLog = Read-All (Join-Path $state 'restore.log')

# --------------------------------------------------------------- redaction
# SSIDs: Windows' saved profiles, the current connection and the names the
# service logged; longest first so a name inside another goes second.
$ssids = New-Object System.Collections.Generic.List[string]
foreach ($line in (Run-Netsh @('wlan', 'show', 'profiles'))) {
  if ($line -match '^\s{4}.+?\s:\s(.+)$') { $ssids.Add($Matches[1].Trim()) }
}
$wlanInterfaces = Run-Netsh @('wlan', 'show', 'interfaces')
foreach ($line in $wlanInterfaces) {
  if ($line -match '^\s*SSID\s*:\s(.+)$') { $ssids.Add($Matches[1].Trim()) }
}
foreach ($line in $serviceLog) {
  foreach ($m in [regex]::Matches($line, '(?:joining|connecting to|chosen in nekodrop|Forget|forget)\s+"([^"]+)"')) { $ssids.Add($m.Groups[1].Value) }
  foreach ($m in [regex]::Matches($line, '^\S+ \S+ Wi-Fi: "([^"]+)"')) { $ssids.Add($m.Groups[1].Value) }
}
$ssidList = @($ssids | Where-Object { $_ -and $_.Length -ge 3 } | Sort-Object -Unique | Sort-Object Length -Descending)
$macMap = @{}
$ipMap = @{}
function Redact([string]$s) {
  if (-not $s) { return $s }
  $i = 0
  foreach ($name in $ssidList) { $i++; $s = $s.Replace($name, "<SSID-$i>") }
  # "event: bss <bssid> <channel> <dBm> <security> <ssid hex>": the name in hex.
  $s = [regex]::Replace($s, '(event: bss \S+ \S+ \S+ \S+ )[0-9A-Fa-f]+', '$1<ssid-hex>')
  $s = [regex]::Replace($s, '\b[0-9A-Fa-f]{2}([:-])[0-9A-Fa-f]{2}(\1[0-9A-Fa-f]{2}){4}\b', {
    param($m) $k = $m.Value.ToLower().Replace('-', ':')
    if (-not $macMap.ContainsKey($k)) { $macMap[$k] = "<MAC-$($macMap.Count + 1)>" }
    $macMap[$k] })
  # Private, link-local and CGNAT IPv4 (version numbers such as 23.150.0.4 stay).
  $s = [regex]::Replace($s, '\b(?:10\.\d{1,3}|192\.168|172\.(?:1[6-9]|2\d|3[01])|169\.254|100\.(?:6[4-9]|[7-9]\d|1[01]\d|12[0-7]))\.\d{1,3}\.\d{1,3}\b', {
    param($m) if (-not $ipMap.ContainsKey($m.Value)) { $ipMap[$m.Value] = "<IP-$($ipMap.Count + 1)>" }
    $ipMap[$m.Value] })
  $s = [regex]::Replace($s, '(?i)\b(?:fe80|f[cd][0-9a-f]{2}|2[0-9a-f]{3}):[0-9a-f:%]{2,}[0-9a-f]', '<IPv6>')
  $s = [regex]::Replace($s, '(?i)([A-Z]:\\Users\\)[^\\\s"]+', '$1<user>')
  if ($UserName -and $UserName.Length -ge 2) { $s = $s.Replace($UserName, '<user>') }
  if ($env:COMPUTERNAME) { $s = $s.Replace($env:COMPUTERNAME, '<pc>') }
  return $s
}

# ------------------------------------------------------------- collection
$os = Try-Get { Get-CimInstance Win32_OperatingSystem }
$cv = Try-Get { Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' }
$cs = Try-Get { Get-CimInstance Win32_ComputerSystem }
$cpu = Try-Get { (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim() }
$dg = Try-Get { Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard -ClassName Win32_DeviceGuard }
$svc = Get-Service NekoDropService -ErrorAction SilentlyContinue
$svcStart = Try-Get { (Get-CimInstance Win32_Service -Filter "Name='NekoDropService'").StartMode }
$exe = Join-Path $app 'NekoDropService.exe'
$version = Try-Get { (Get-Item $exe).VersionInfo.ProductVersion }
$config = Try-Get { Get-Content (Join-Path $state 'service.json') -Raw | ConvertFrom-Json }
$ids = '43F0|A0F0|02F0|06F0|34F0|3DF0|4DF0|2723|2725|7A70|7AF0|51F0|51F1|54F0|7F70'
$card = Try-Get { Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match "PCI\\VEN_8086&DEV_($ids)" } | Select-Object -First 1 }
function Card-Prop([string]$key) {
  if (-not $card) { return '' }
  $v = Try-Get { (Get-PnpDeviceProperty -InstanceId $card.InstanceId -KeyName $key -ErrorAction Stop).Data }
  if ($null -eq $v) { return '' } else { return "$v" }
}
$procs = @('nekodrop-radio', 'awdld', 'nekodrop', 'NekoDropService') | ForEach-Object {
  $p = Get-Process $_ -ErrorAction SilentlyContinue
  if ($p) { "$_ (running)" } else { "$_ (not running)" }
}
$engineRunning = Try-Get { @(Get-CimInstance Win32_Process -Filter "Name='python.exe'" | Where-Object { $_.CommandLine -like '*engine.py*' }).Count }

# Bring-up of the last radio start, from the service log.
# It begins at the last "radio: card" line, or at the "taking the card" line
# just before it (or after it, when the last attempt failed at the bind).
$attempt = @()
$lastRadio = -1; $lastTake = -1
for ($i = 0; $i -lt $serviceLog.Count; $i++) {
  if ($serviceLog[$i] -match 'radio: card 0x') { $lastRadio = $i }
  elseif ($serviceLog[$i] -match 'taking the card from') { $lastTake = $i }
}
$start = $lastRadio
if ($lastTake -gt $lastRadio) { $start = $lastTake }
elseif ($lastTake -ge 0 -and $lastRadio -ge 0) {
  $between = $serviceLog[$lastTake..$lastRadio] | Where-Object { $_ -match 'radio: card 0x' }
  if (@($between).Count -eq 1) { $start = $lastTake }
}
if ($start -ge 0) { $attempt = $serviceLog[$start..($serviceLog.Count - 1)] }
function Stage([string]$name, [string]$ok, [string]$bad, [string]$skipIf = '') {
  $okLine = $attempt | Where-Object { $_ -match $ok } | Select-Object -Last 1
  $badLine = if ($bad) { $attempt | Where-Object { $_ -match $bad } | Select-Object -Last 1 } else { $null }
  if ($skipIf -and -not ($attempt | Where-Object { $_ -match $skipIf })) { return "| $name | - | not applicable |" }
  if ($badLine) { return "| $name | FAIL | ``$(Redact (($badLine -replace '^\S+ \S+ ', '') -replace '\|', '/'))`` |" }
  if ($okLine) { return "| $name | ok | ``$(Redact (($okLine -replace '^\S+ \S+ ', '') -replace '\|', '/'))`` |" }
  return "| $name | ? | not reached |"
}

# --------------------------------------------------------------- the report
$sb = New-Object System.Text.StringBuilder
function W([string]$s = '') { [void]$sb.AppendLine($s) }

W '# nekodrop diagnostic report'
W ''
W "Generated $(Get-Date -Format 'yyyy-MM-dd HH:mm'). SSIDs, MAC and IP addresses, the user and computer names are replaced by placeholders; please look it over before posting."
W ''
W '## Summary'
W ''
W "- nekodrop: $(if ($version) { $version } else { 'not found at ' + (Redact $app) })"
W "- Windows: $(if ($os) { $os.Caption }) $(if ($cv) { $cv.DisplayVersion }) (build $(if ($cv) { "$($cv.CurrentBuild).$($cv.UBR)" }))"
W "- Service: $(if ($svc) { "$($svc.Status), start $svcStart" } else { 'not installed' }); mode $(if ($config) { $config.Mode } else { '?' })"
W "- Card: $(if ($card) { "$($card.FriendlyName) ($($card.InstanceId -replace '\\[^\\]+$', ''))" } else { 'no supported Intel card found' })"
$lastErr = $serviceLog | Where-Object { $_ -match 'start failed|bring-up failed|keeps failing|taking the card failed|LMAC error|UMAC error|firmware error|refusing|cannot give' } | Select-Object -Last 1
# The AirDrop engine's failures too (#4: "none in the log" beside send failures).
$lastEngineErr = $engineLog | Where-Object { $_ -match '"state":"failed"|Traceback|\bERROR\b|send failed|WinError' -and $_ -notmatch 'mdns: (send|query) failed' } | Select-Object -Last 1
W "- Last error: $(if ($lastErr) { '`' + (Redact $lastErr) + '`' } else { 'none in the service log' })"
W "- Last AirDrop engine error: $(if ($lastEngineErr) { '`' + (Redact ($lastEngineErr -replace '`', "'")) + '`' } else { 'none' })"
W ''
W '## Bring-up (last radio start)'
W ''
if ($start -lt 0) {
  W 'No radio start in the service log.'
} else {
  W "From ``$(Redact ($attempt[0] -replace '\|', '/'))``"
  W ''
  W '| Step | Result | Line |'
  W '|---|---|---|'
  W (Stage 'Take the card (driver bind)' 'radio: card 0x' 'taking the card failed|binding .* failed|register proxy did not start|register proxy not openable')
  W (Stage 'Identify the card' 'radio: .*\((AX210|22000) family' 'unsupported card|not an Intel device|card does not answer')
  W (Stage 'DMA memory' 'DMA: \d+ pages in use' 'out of DMA memory for (the rings|receive|the transmit|command|the firmware|the peripheral)|could not lock|DMA: .*fail')
  W (Stage 'Load the firmware' 'radio: firmware \S+: \d+ LMAC' 'firmware has more sections|no image loader|cannot read|firmware file')
  W (Stage 'Firmware alive' 'radio: firmware alive' 'did not come alive|failed ALIVE|no ALIVE notification|card not ready|APM init failed|hardware start failed')
  W (Stage 'PNVM (AX210 family)' 'PNVM: \d+ bytes for this SKU' 'PNVM: none|PNVM: no buffer|PNVM: out of DMA|no PNVM_INIT_COMPLETE|did not take its platform NVM' 'AX210 family')
  W (Stage 'Firmware init (INIT_COMPLETE)' 'radio: NVM: tx antennas' 'no INIT_COMPLETE|INIT_EXTENDED_CFG failed|NVM_ACCESS_COMPLETE failed|reading the NVM failed')
  W (Stage 'Firmware setup commands' 'radio: ready on channel' 'firmware init commands failed|PHY context add failed|MAC context|binding failed')
  W (Stage 'Radio ready' 'radio: ready on channel' 'radio exited during bring-up|never became ready')
  $awdl = $attempt | Where-Object { $_ -match 'rx: \d+ frames, (\d+) AWDL action frames' } | ForEach-Object { [int]$Matches[1] } | Measure-Object -Maximum
  # MIF counts (0.1.9+): awdld takes a peer as valid only after one (#16).
  $mif = $attempt | Where-Object { $_ -match 'AWDL action frames \((\d+) PSF, (\d+) MIF' } | ForEach-Object { [int]$Matches[2] } | Measure-Object -Maximum
  $peers = @($attempt | Where-Object { $_ -match 'event: peer-channel|event: peer-country' }).Count
  W "| AWDL frames heard | $(if ($awdl.Maximum -gt 0) { 'ok' } else { '?' }) | $(if ($awdl.Count) { "up to $($awdl.Maximum) AWDL action frames$(if ($mif.Count) { " ($($mif.Maximum) MIF)" }); $peers peer channel/country events" } else { 'no rx statistics yet' }) |"
  # Transmitting: on a passive 5 GHz channel the radio sends nothing until a
  # beacon (or, 0.1.10+, an AWDL peer) is heard there (#11).
  $txLast = $attempt | Where-Object { $_ -match 'radio: tx: \d+ submitted, \d+ done' } | Select-Object -Last 1
  $txUnlock = $attempt | Where-Object { $_ -match 'transmitting allowed here' } | Select-Object -Last 1
  $txRow = if (-not $txLast) { '?' }
    elseif ($txLast -match 'tx: \d+ submitted, 0 done' -and $txLast -match 'held \(passive channel\)' -and -not $txUnlock) { 'FAIL' }
    else { 'ok' }
  $txWhy = if ($txRow -eq 'FAIL') { 'nothing sent: the channel is passive (the firmware did not take the country) and no beacon or AWDL peer was heard on it' }
    elseif ($txUnlock) { '`' + ($txUnlock -replace '^\S+ \S+ ', '') + '`' }
    elseif ($txLast) { '`' + (($txLast -replace '^\S+ \S+ ', '') -replace '; unicast.*', '') + '`' } else { 'no tx statistics yet' }
  W "| Transmitting | $txRow | $txWhy |"
  # Experimental A-MPDU sending (0.1.15+): its last status line, when it is on.
  $aggLast = $attempt | Where-Object { $_ -match 'radio: aggregate: (idle|asking|agreed)' } | Select-Object -Last 1
  if ($aggLast) {
    $aggRow = if ($aggLast -match 'aggregate: agreed') { 'ok' } elseif ($aggLast -match 'aggregate: asking') { 'asking' } else { '-' }
    W "| A-MPDU sending (experimental) | $aggRow | ``$((($aggLast -replace '^\S+ \S+ radio: \S+ radio: ', '') -replace '\|', '/'))`` |"
  }
  # The latest station event decides: "authenticating" is a step of every join,
  # not a failure (#4, #14, #16 all showed FAIL while connected).
  $wifiLast = $attempt | Where-Object { $_ -match 'event: wifi \S|not supported beside AirDrop|not joining a saved network' } | Select-Object -Last 1
  $wifiRow = if (-not $wifiLast) { 'not applicable' }
    elseif ($wifiLast -match 'event: wifi connected') { 'ok' }
    elseif ($wifiLast -match 'event: wifi (failed|deauth)|not supported beside AirDrop') { 'FAIL' }
    elseif ($wifiLast -match 'not joining a saved network') { 'not applicable' }
    else { 'joining' }
  W "| Wi-Fi station | $(if ($wifiRow -eq 'not applicable') { '-' } else { $wifiRow }) | $(if ($wifiLast) { '`' + (Redact (($wifiLast -replace '^\S+ \S+ ', '') -replace '\|', '/')) + '`' } else { 'no station in this start' }) |"
  W ''
  $fw = $attempt | Where-Object { $_ -match 'LMAC error|UMAC error|SecBoot|UMAC PC' } | Select-Object -Last 4
  if ($fw) {
    W 'Firmware error tables:'
    W ''
    W '```'
    $fw | ForEach-Object { W (Redact $_) }
    W '```'
    W ''
  }
}

W '## System'
W ''
W "- Computer: $(if ($cs) { "$($cs.Manufacturer) $($cs.Model)" })"
W "- CPU: $cpu"
W "- Edition: $(if ($cv) { $cv.EditionID }), $(if ($os) { $os.OSArchitecture })"
W "- VBS: $(if ($dg) { @('off', 'configured', 'running')[[int]$dg.VirtualizationBasedSecurityStatus] } else { '?' }); services running: $(if ($dg) { ($dg.SecurityServicesRunning -join ',') } else { '?' }) (2 = HVCI / Memory Integrity)"
W "- Processes: $($procs -join ', '); AirDrop engine (python): $(if ($engineRunning) { 'running' } else { 'not running' })"
W ''

W '## Wi-Fi card'
W ''
if ($card) {
  W "- Device: $($card.FriendlyName), status $($card.Status), class $($card.Class)"
  W "- Hardware ID: ``$(($card.InstanceId -split '\\')[1])``"
  W "- Driver: service $(Card-Prop 'DEVPKEY_Device_Service'), INF $(Card-Prop 'DEVPKEY_Device_DriverInfPath'), version $(Card-Prop 'DEVPKEY_Device_DriverVersion'), provider $(Card-Prop 'DEVPKEY_Device_DriverProvider')"
  $problem = Card-Prop 'DEVPKEY_Device_ProblemCode'
  W "- Device Manager problem code: $(if ($problem) { $problem } else { '0' }) (0 = none; 10 = cannot start; 28 = no driver; 31/37/39 = driver did not load; 52 = signature)"
  # The NTSTATUS behind a problem code (#18: code 31 with 0xC0000494 on Windows 10).
  $problemStatus = Card-Prop 'DEVPKEY_Device_ProblemStatus'
  if ($problemStatus) { W ("- Problem status: 0x{0:X8}" -f ([int64]$problemStatus -band 0xFFFFFFFF)) }
  W "- Intel driver to give it back to: $(if ($config -and $config.VendorInf) { Split-Path $config.VendorInf -Leaf } else { 'not recorded' })"
  # Windows' own record of the card's driver installs and start failures.
  $devId = ($card.InstanceId -split '\\')[1] -replace '&SUBSYS.*$', ''
  $pnp = @(Try-Get { Get-WinEvent -LogName 'Microsoft-Windows-Kernel-PnP/Configuration' -MaxEvents 400 -ErrorAction Stop |
           Where-Object { $_.Message -like "*$devId*" } | Select-Object -First 8 } | Where-Object { $_ })
  if ($pnp.Count) {
    W '- Recent driver events (Kernel-PnP):'
    W ''
    W '```'
    $pnp | ForEach-Object { W (Redact ("$($_.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss')) [$($_.Id)] " + (($_.Message -split "`r?`n" | Where-Object { $_.Trim() }) -join ' | '))) }
    W '```'
    W ''
  }
  $umdf = @(Try-Get { Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Microsoft-Windows-DriverFrameworks-UserMode' } -MaxEvents 6 -ErrorAction Stop } | Where-Object { $_ })
  if ($umdf.Count) {
    W '- Recent user-mode driver (UMDF) events:'
    W ''
    W '```'
    $umdf | ForEach-Object { W (Redact ("$($_.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss')) [$($_.Id)] " + (($_.Message -split "`r?`n" | Where-Object { $_.Trim() }) -join ' | '))) }
    W '```'
    W ''
  }
  # Windows' device-install log for the register proxy's last install: why a
  # driver bound but did not start shows up here (#18, Windows 10).
  $setupLog = Join-Path $env:windir 'INF\setupapi.dev.log'
  $setupLines = @(Try-Get { Get-Content $setupLog -Tail 6000 -ErrorAction Stop })
  if ($setupLines.Count) {
    # Section starts; the last one that installed the register proxy is the
    # one that explains a failed bind (giving the card back installs Intel's
    # driver afterwards, in its own section).
    $all = @(for ($i = 0; $i -lt $setupLines.Count; $i++) { if ($setupLines[$i] -match '^>>>  \[') { $i } })
    $starts = @(for ($k = 0; $k -lt $all.Count; $k++) {
      $last = if ($k + 1 -lt $all.Count) { $all[$k + 1] - 1 } else { $setupLines.Count - 1 }
      if (($setupLines[$all[$k]..$last] -join ' ') -match 'winawdl_regproxy|RegProxy|WUDFRd') { $all[$k] }
    })
    if ($starts.Count) {
      $from = $starts[-1]
      $section = @($setupLines[$from..($setupLines.Count - 1)])
      $end = [Array]::FindIndex([string[]]$section, [Predicate[string]] { param($l) $l -match '^<<<  \[Exit status' })
      if ($end -ge 0) { $section = $section[0..$end] }
      $keep = @($section | Where-Object { $_ -match '^(>>>|<<<)|!!!|^\s+!|Exit status|[Ss]ervice|WUDF|Install|Driver Node|Selected|Status|error|failed' } | Select-Object -First 45)
      if ($keep.Count) {
        W '- Last device install (setupapi.dev.log):'
        W ''
        W '```'
        $keep | ForEach-Object { W (Redact $_.TrimEnd()) }
        W '```'
        W ''
      }
    }
  }
  $radioCard = $serviceLog | Where-Object { $_ -match 'radio: card 0x|\((AX210|22000) family' } | Select-Object -Last 2
  $radioCard | ForEach-Object { W "- Radio: ``$(Redact ($_ -replace '^\S+ \S+ radio: \S+ radio: ', ''))``" }
} else {
  W 'No Intel Wi-Fi card nekodrop supports is present.'
  $other = Try-Get { Get-PnpDevice -PresentOnly -Class Net | Where-Object { $_.FriendlyName -match 'Wi-?Fi|Wireless|802\.11|WLAN' } | Select-Object -First 3 }
  $other | ForEach-Object { W "- Other wireless device: $($_.FriendlyName) ($(($_.InstanceId -split '\\')[1]))" }
}
if ($config) {
  W "- Settings: mode $($config.Mode), country '$($config.Country)', start channel $($config.StartChannel), peer channel $($config.PeerChannel), on-demand idle $($config.IdleSeconds) s"
}
W ''

W '## Network'
W ''
W '| Adapter | Description | Status | Speed | Metric (IPv4) |'
W '|---|---|---|---|---|'
$ifs = Try-Get { Get-NetIPInterface -AddressFamily IPv4 -ErrorAction Stop }
$presentAdapters = @(Try-Get { Get-NetAdapter -IncludeHidden -ErrorAction Stop | Where-Object { $_.Status -ne 'Not Present' } })
foreach ($a in $presentAdapters) {
  $metric = ($ifs | Where-Object { $_.InterfaceIndex -eq $a.ifIndex } | Select-Object -First 1).InterfaceMetric
  W "| $(Redact $a.Name) | $(Redact $a.InterfaceDescription) | $($a.Status) | $($a.LinkSpeed) | $metric |"
}
W ''
if (-not ($presentAdapters | Where-Object { $_.Name -eq 'nekodrop' })) {
  $hiddenTap = @(Try-Get { Get-NetAdapter -Name 'nekodrop' -IncludeHidden -ErrorAction Stop })
  if ($hiddenTap.Count) {
    W "nekodrop TAP adapter: $($hiddenTap[0].Status) ($($hiddenTap[0].InterfaceDescription))"
  } else {
    W 'nekodrop TAP adapter: missing'
  }
  W ''
}
$routes = @(Try-Get { Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction Stop })
W "Default routes: $($routes.Count)"
foreach ($r in $routes) { W "- $(Redact $r.InterfaceAlias): route metric $($r.RouteMetric), interface metric $(($ifs | Where-Object { $_.InterfaceIndex -eq $r.ifIndex } | Select-Object -First 1).InterfaceMetric)" }
W ''
W 'Windows Wi-Fi (netsh wlan show interfaces; names and addresses removed):'
W ''
W '```'
foreach ($line in $wlanInterfaces) {
  if ($line -match '^\s*$' -or $line -match '(?i)SSID|Physical|物理|Profile|配置文件|Hosted|承载') { continue }
  W (Redact $line.TrimEnd())
}
W '```'
W ''

W '## Bluetooth'
W ''
$bt = @(Try-Get { Get-PnpDevice -PresentOnly -Class Bluetooth -ErrorAction Stop | Where-Object { $_.FriendlyName -notmatch 'Enumerator|RFCOMM|LE Generic|Avrcp|Hands-?free|Audio' } | Select-Object -First 6 } | Where-Object { $_ })
if ($bt.Count) { $bt | ForEach-Object { W "- $(Redact $_.FriendlyName): $($_.Status)" } } else { W 'No Bluetooth adapter found.' }
W ''

W '## Discovery timeline'
W ''
W 'Service (radio ready, AWDL channel and peers, Wi-Fi):'
W ''
W '```'
$lastAwdl = -1
$timeline = foreach ($line in ($serviceLog | Select-Object -Last 4000)) {
  if ($line -match 'ready on channel|event: peer-channel|event: peer-country|event: channel|moving there|stayed passive|starting:|stopping:|Wi-Fi: joining|event: wifi |gateway') { $line }
  elseif ($line -match 'rx: \d+ frames, (\d+) AWDL action frames') {
    $n = [int]$Matches[1]
    if (($lastAwdl -le 0 -and $n -gt 0) -or ($n -lt $lastAwdl)) { $line }
    $lastAwdl = $n
  }
}
$timeline | Select-Object -Last 40 | ForEach-Object { W (Redact $_) }
W '```'
W ''
W 'Client (looking for devices, Bluetooth beacon, peers, transfers):'
W ''
W '```'
$engineLog | Where-Object { $_ -match '"event":"(looking|peer|lost|ask|progress|done|error|engine)"|beacon|Traceback|ERROR|WARNING' } |
  Select-Object -Last 40 | ForEach-Object { W (Redact $_) }
W '```'
W ''

W '## Key log lines'
W ''
W '```'
$serviceLog | Where-Object { $_ -match '(?i)error|fail|refus|cannot|could not|timed out|not supported|exited|restart|giving the card|taking the card|ready on|PNVM|INIT_COMPLETE|NVM:|alive|unsupported' } |
  Select-Object -Last 120 | ForEach-Object { W (Redact $_) }
W '```'
W ''

function Details([string]$title, $lines) {
  W '<details>'
  W "<summary>$title</summary>"
  W ''
  W '```'
  $lines | ForEach-Object { W (Redact $_) }
  W '```'
  W ''
  W '</details>'
  W ''
}
Details 'service.log (last 250 lines)' ($serviceLog | Select-Object -Last 250)
Details 'engine.log (last 150 lines)' ($engineLog | Select-Object -Last 150)
Details 'install.log (last 80 lines)' ($installLog | Select-Object -Last 80)

$report = $sb.ToString()
# GitHub takes at most 65536 characters in an issue body.
if ($report.Length -gt 60000) {
  $report = $report.Substring(0, 60000) + "`n`n(cut at 60000 characters; the full logs are in the .txt file)`n"
}
[IO.File]::WriteAllText($reportPath, $report, (New-Object System.Text.UTF8Encoding($false)))

# ---------------------------------------------------------- full logs file
$full = New-Object System.Text.StringBuilder
foreach ($part in @(
    @('service.log (+ service.log.1)', $serviceLog),
    @('engine.log (+ engine.log.old)', $engineLog),
    @('install.log', $installLog),
    @('restore.log', $restoreLog))) {
  [void]$full.AppendLine("===== $($part[0]) =====")
  foreach ($l in $part[1]) { [void]$full.AppendLine((Redact $l)) }
  [void]$full.AppendLine()
}
[IO.File]::WriteAllText($logsPath, $full.ToString(), (New-Object System.Text.UTF8Encoding($false)))

Write-Host "nekodrop: report written to $reportPath"
Write-Host "nekodrop: full logs written to $logsPath"
if (-not $NoOpen) {
  try { Set-Clipboard -Value $report } catch { }
  Start-Process notepad.exe -ArgumentList "`"$reportPath`""
  Start-Process explorer.exe -ArgumentList "/select,`"$reportPath`""
  Write-Host 'The report is also on the clipboard. Paste it into a GitHub issue and attach the .txt file.'
  Start-Sleep -Seconds 3
}
