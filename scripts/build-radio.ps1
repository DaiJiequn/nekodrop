# Builds the Intel radio daemon (winawdl_radio.exe) and its self-test (MSVC by
# default, -Clang for clang-cl), copies the Qu/QuZ firmware beside it and runs
# the self-test.
param([switch]$Clang)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\toolchain.ps1"
$repo = Split-Path $PSScriptRoot -Parent
$tools = Initialize-Toolchain
$compiler = $tools.Cl
if ($Clang) {
  if (-not (Test-Path $tools.ClangCl)) { throw "clang-cl not found at $($tools.ClangCl)" }
  $compiler = $tools.ClangCl
}
$build = "$repo\build\iwxradio"
& $tools.CMake -S "$repo\iwxradio" -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$($tools.Ninja)" `
  "-DCMAKE_CXX_COMPILER=$compiler" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
& $tools.CMake --build $build
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

$fwSource = "$repo\third_party\ForbiddenItlwm\itlwm\firmware"
$fwDest = "$build\firmware"
New-Item -ItemType Directory -Force $fwDest | Out-Null
Get-ChildItem $fwSource -Filter 'iwlwifi-Qu*-hr-b0-68.ucode' | Copy-Item -Destination $fwDest -Force
Copy-Item "$repo\third_party\ForbiddenItlwm\iwlwifi-firmware-license" "$fwDest\LICENSE.iwlwifi-firmware" -Force

& "$build\iwx_selftest.exe" $fwDest
if ($LASTEXITCODE -ne 0) { throw 'self-test failed' }
Get-ChildItem $build -Filter *.exe | Select-Object Name, Length
