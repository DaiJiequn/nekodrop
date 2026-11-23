# One-shot release of nekodrop 2.0.1 (Stable): bump, commit, tag, wait for CI,
# publish the installer as the latest release, delete the 2.0.0 and 0.1.39 releases (tags kept).
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
Set-Location $repo
$v = '2.0.1'

# Version
foreach ($f in 'app\client\NekoDrop.csproj', 'app\service\NekoDrop.Service.csproj') {
  (Get-Content $f -Raw) -replace '<Version>2\.0\.0</Version>', "<Version>$v</Version>" | Set-Content $f -NoNewline -Encoding utf8
}
(Get-Content installer\nekodrop.iss -Raw) -replace '#define AppVersion "2\.0\.0"', "#define AppVersion `"$v`"" | Set-Content installer\nekodrop.iss -NoNewline -Encoding utf8
(Get-Content scripts\build-installer.ps1 -Raw) -replace 'nekodrop-2\.0\.0-windows', "nekodrop-$v-windows" | Set-Content scripts\build-installer.ps1 -NoNewline -Encoding utf8

$notes = @"
> **Stable Release / 稳定版（Latest）** — 取代有问题的 2.0.0 / 0.1.39。Previous stable: v0.1.35.
> Full release history: [CHANGELOG.md](https://github.com/zheznanohana/nekodrop/blob/main/CHANGELOG.md)

### 中文
- 主窗口改为托盘旁的弹出面板：贴屏幕右下角，点外面自动收起，单击托盘图标开 / 关
- 修复右上角出现两套最小化 / 最大化 / 关闭按钮
- 修复窗口比屏幕高时底部显示不全；设备名和状态不再被裁切
- 雷达放大，设备不再压住「本机」；删除雷达上的箭头
- 接收时「本机」外圈显示进度，完成后变绿显示对号；接受 / 拒绝气泡保留

### English
- The main window is now a flyout by the tray: bottom-right corner, closes when you click elsewhere, the tray icon toggles it
- Fixed a second set of minimize / maximize / close buttons in the corner
- Fixed windows taller than the screen being cut off; device names and states are no longer clipped
- A larger radar, devices no longer cover "This PC"; the arrows on the radar are gone
- While receiving, a progress ring around "This PC", then green with a check mark; the accept / decline bubble stays
"@

# CHANGELOG: current stable + entry
$cl = Get-Content CHANGELOG.md -Raw
$cl = $cl -replace '当前稳定版为 \*\*`v0\.1\.35`\*\*', "当前稳定版为 **``v$v``**"
$entry = "## v$v (Stable / 稳定版) — $(Get-Date -Format yyyy-MM-dd)`n`n" + (($notes -split "`n" | Select-Object -Skip 3) -join "`n").Trim() + "`n`n---`n`n"
$i = $cl.IndexOf('## v2.0.0')
$cl = $cl.Substring(0, $i) + $entry + $cl.Substring($i)
Set-Content CHANGELOG.md $cl -NoNewline -Encoding utf8

# Commit (client changes only; kdriver\ stays out), push, tag
git add app/client app/service/NekoDrop.Service.csproj installer/nekodrop.iss scripts/build-installer.ps1 scripts/release-2.0.1.ps1 CHANGELOG.md
git commit -m "Client 2.0.1: flyout by the tray, one set of title buttons, fits the screen, larger radar without arrows, receiving ring on this PC; bump version to $v" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
git push origin HEAD:main
git tag "v$v"
git push origin "v$v"

# Wait for the tag's CI build, then fetch the installer
Start-Sleep 20
$run = gh run list --workflow build.yml --branch "v$v" -L 1 --json databaseId -q '.[0].databaseId'
"CI run $run"
gh run watch $run --exit-status
$dl = Join-Path $repo "build\release-$v"
if (Test-Path $dl) { Remove-Item $dl -Recurse -Force }
gh run download $run -n installer -D $dl
$exe = Get-ChildItem $dl -Filter "nekodrop-$v-setup.exe" | Select-Object -First 1
if (-not $exe) { throw "installer not found in $dl" }

# Release as stable/latest, then drop the faulty ones (their tags stay)
$nf = Join-Path $dl notes.md
Set-Content $nf $notes -Encoding utf8
gh release create "v$v" $exe.FullName --title "nekodrop $v (Stable)" --notes-file $nf --latest
gh release delete v2.0.0 --yes
gh release delete v0.1.39 --yes
gh release list -L 10
'done'
