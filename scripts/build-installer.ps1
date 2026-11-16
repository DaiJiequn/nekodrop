# Builds every part of nekodrop, stages it in build\stage, and makes
# build\nekodrop-<version>-setup.exe with Inno Setup 6 (or a zip without it).
#   radio\    nekodrop-radio.exe (user-mode Intel driver) + Intel firmware
#   awdl\     awdld.exe (winawdl's AWDL daemon, OWL) + wintun.dll
#   airdrop\  winawdl's AirDrop engine (Python, MIT)
#   python\   the Python embeddable distribution
#   driver\   the register-proxy UMDF driver package (signed on the target PC)
#   nekodrop.exe (client), NekoDropService.exe (service), install/uninstall scripts
# -Offline skips the downloads (Python embeddable, TAP-Windows) and uses what
# build\downloads already holds.
param([switch]$Offline, [string]$PythonVersion = '3.12.8', [string]$TapVersion = '9.24.7-I601')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\toolchain.ps1"
$repo = Split-Path $PSScriptRoot -Parent
$stage = "$repo\build\stage"
$downloads = "$repo\build\downloads"
$tools = Initialize-Toolchain
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage, $downloads | Out-Null

'radio'
& "$PSScriptRoot\build-radio.ps1" | Out-Null
New-Item -ItemType Directory -Force "$stage\radio\firmware" | Out-Null
Copy-Item "$repo\build\iwxradio\winawdl_radio.exe" "$stage\radio\nekodrop-radio.exe"
Copy-Item "$repo\build\iwxradio\firmware\*" "$stage\radio\firmware\"

'awdl'
& "$PSScriptRoot\build-awdl.ps1" -MinGW "$repo\third_party\winlibs\mingw64\bin" | Out-Null
New-Item -ItemType Directory -Force "$stage\awdl" | Out-Null
Copy-Item "$repo\third_party\winawdl\build\awdl\awdld.exe", "$repo\third_party\winawdl\deps\wintun\wintun.dll" "$stage\awdl\"

'airdrop engine'
New-Item -ItemType Directory -Force "$stage\airdrop" | Out-Null
Copy-Item "$repo\third_party\winawdl\airdrop\*.py", "$repo\third_party\winawdl\airdrop\LICENSE" "$stage\airdrop\"
# sender.py looks for the Bluetooth beacon script in ..\scripts beside its own folder.
New-Item -ItemType Directory -Force "$stage\scripts" | Out-Null
Copy-Item "$repo\third_party\winawdl\scripts\airdrop-beacon.ps1" "$stage\scripts\"

'driver'
& "$repo\driver\regproxy\build.ps1" | Out-Null
New-Item -ItemType Directory -Force "$stage\driver" | Out-Null
Copy-Item "$repo\driver\regproxy\build\winawdl_regproxy.inf", "$repo\driver\regproxy\build\winawdl_regproxy.dll" "$stage\driver\"
$now = Get-Date
& $tools.StampInf -f "$stage\driver\winawdl_regproxy.inf" -d * -v "0.1.$(($now - [datetime]'2026-01-01').Days).$($now.Hour * 60 + $now.Minute)" | Out-Null
& $tools.Inf2Cat "/driver:$stage\driver" /os:10_X64 /uselocaltime | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed ($LASTEXITCODE)" }

'service and client'
dotnet publish "$repo\app\service\NekoDrop.Service.csproj" -c Release -o "$stage" --nologo -v q
if ($LASTEXITCODE -ne 0) { throw 'service publish failed' }
dotnet publish "$repo\app\client\NekoDrop.csproj" -c Release -o "$stage" --nologo -v q
if ($LASTEXITCODE -ne 0) { throw 'client publish failed' }
Copy-Item "$repo\installer\install.ps1", "$repo\installer\uninstall.ps1" $stage

'downloads'
$py = "$downloads\python-$PythonVersion-embed-amd64.zip"
$tap = "$downloads\tap-windows-$TapVersion.exe"
if (-not $Offline) {
  if (-not (Test-Path $py)) {
    Invoke-WebRequest "https://www.python.org/ftp/python/$PythonVersion/python-$PythonVersion-embed-amd64.zip" -OutFile $py -UseBasicParsing
  }
  if (-not (Test-Path $tap)) {
    Invoke-WebRequest "https://build.openvpn.net/downloads/releases/tap-windows-$TapVersion-Win10.exe" -OutFile $tap -UseBasicParsing
  }
}
if (Test-Path $py) {
  Expand-Archive $py "$stage\python" -Force
  # The embeddable distribution's ._pth fixes sys.path: list the engine's folder
  # there, since its modules import each other.
  $pth = Get-ChildItem "$stage\python" -Filter 'python*._pth' | Select-Object -First 1
  Add-Content $pth.FullName '..\airdrop'
} else { '  (no Python embeddable: the client falls back to a Python on PATH)' }
if (Test-Path $tap) {
  $sig = Get-AuthenticodeSignature $tap
  if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'OpenVPN') {
    throw "tap-windows installer is not validly signed by OpenVPN: $($sig.Status)"
  }
  Copy-Item $tap "$stage\tap-windows.exe"
}

'licenses'
New-Item -ItemType Directory -Force "$stage\licenses" | Out-Null
Copy-Item "$repo\third_party\ForbiddenItlwm\LICENSE" "$stage\licenses\ForbiddenItlwm-GPL-2.0.txt"
Copy-Item "$repo\third_party\ForbiddenItlwm\iwlwifi-firmware-license" "$stage\licenses\iwlwifi-firmware.txt"
Copy-Item "$repo\third_party\winawdl\LICENSE" "$stage\licenses\winawdl.txt" -ErrorAction SilentlyContinue
Copy-Item "$repo\third_party\winawdl\deps\wintun\LICENSE.txt" "$stage\licenses\wintun.txt"

$iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
if ($iscc) {
  & $iscc "$repo\installer\nekodrop.iss" | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed ($LASTEXITCODE)" }
  Get-ChildItem "$repo\build\nekodrop-*-setup.exe" | Select-Object Name, Length
} else {
  $zip = "$repo\build\nekodrop-0.1.0-windows-x64.zip"
  Compress-Archive "$stage\*" $zip -Force
  "Inno Setup not found: made $zip instead (unzip, then run install.ps1 as administrator)."
}
