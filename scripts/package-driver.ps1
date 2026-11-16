# Builds, stages and signs the register-proxy driver package. Needs no
# elevation and changes no trust settings: the signing certificate only lands
# in the personal store, non-exportable. From winawdl's package-driver.ps1.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\toolchain.ps1"
. "$PSScriptRoot\driver-settings.ps1"

& "$repoRoot\driver\regproxy\build.ps1" | Out-Null
$tools = Initialize-Toolchain

$package = $DriverSettings.PackageDir
if (Test-Path $package) { Remove-Item $package -Recurse -Force }
New-Item -ItemType Directory -Force $package | Out-Null
foreach ($file in $DriverSettings.PackageFiles) {
  Copy-Item (Join-Path $DriverSettings.BuildDir $file) $package
}

$cert = Get-ChildItem Cert:\CurrentUser\My |
        Where-Object { $_.Subject -eq $DriverSettings.CertSubject -and $_.HasPrivateKey } |
        Sort-Object NotAfter | Select-Object -Last 1
if (-not $cert) {
  $cert = New-SelfSignedCertificate -Type CodeSigningCert `
            -Subject $DriverSettings.CertSubject `
            -CertStoreLocation Cert:\CurrentUser\My `
            -KeyExportPolicy NonExportable -HashAlgorithm SHA256 `
            -NotAfter (Get-Date).AddYears(2)
}

$now = Get-Date
$version = "0.1.$(($now - [datetime]'2026-01-01').Days).$($now.Hour * 60 + $now.Minute)"
& $tools.StampInf -f (Join-Path $package $DriverSettings.InfName) -d * -v $version | Out-Null
if ($LASTEXITCODE -ne 0) { throw "stampinf failed ($LASTEXITCODE)" }

& $tools.Inf2Cat "/driver:$package" /os:10_X64 /uselocaltime
if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed ($LASTEXITCODE)" }

$signed = Get-ChildItem $package -Include *.cat, *.dll -Recurse
& $tools.SignTool sign /fd SHA256 /sha1 $cert.Thumbprint /s My @($signed.FullName)
if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE)" }

Export-Certificate -Cert $cert -FilePath (Join-Path $package $DriverSettings.CertFile) | Out-Null
"Package ready in $package"
"Signed with $($cert.Subject), thumbprint $($cert.Thumbprint)"
