# REL-PKG-1, build-host half (Windows): ONE release lock image from THIS tree.
# relpkg.sh (Mac) extracts the commit into a fresh, empty rel-<sha7> directory,
# with port\ak1037\oz_build_sha.h, then runs:
#   powershell -NoProfile -ExecutionPolicy Bypass -File port\ak1037\burn\relpkg.ps1 -Variant fp0_v0
# Never OZ_DEBUG_HOOKS; link_app_mcu.ps1 refuses a release image with hooks.
param(
    [string]$Variant = 'fp0_v0',
    [string]$Sdk = 'C:\Users\Arthur\ak1037\AnyMicro1037_SDK_V1.02',
    [string]$TcW = 'C:\SysGCC\risc-v\bin\riscv64-unknown-elf-'
)
$ErrorActionPreference = 'Continue'
$oz  = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$Tc  = $TcW -replace '\\', '/'
$app = "$Sdk\app_mcu"
$name = "nano_${Variant}_lock"
if (-not (Test-Path "$oz\port\ak1037\oz_build_sha.h")) { "REFUSED: no port\ak1037\oz_build_sha.h in $oz"; exit 1 }
"tree $oz; " + (Get-Content "$oz\port\ak1037\oz_build_sha.h")

"== ozcore libs (release)"
Push-Location $oz
& make -f port/ak1037/Makefile selftest -j8 SDK=$($Sdk -replace '\\','/') PREFIX=$Tc 2>&1 | Select-String -Pattern 'warning|error' | ForEach-Object { $_.Line }
$rc = $LASTEXITCODE; Pop-Location
"make rc=$rc"; if ($rc -ne 0) { exit 1 }

"== app objects $Variant"
Push-Location $app
& make -f "Makefile.nano_$Variant" -j8 "TC=$Tc" 2>&1 | Select-String -Pattern 'error:' | Where-Object { $_.Line -notmatch 'main_ozkie|undefined reference' } | Select-Object -First 5 | ForEach-Object { $_.Line }
Pop-Location

"== link $name"
& powershell -NoProfile -ExecutionPolicy Bypass -File "$oz\port\ak1037\link_app_mcu.ps1" -Sdk $Sdk -Tc $TcW -Makefile "Makefile.nano_$Variant" `
    -Libs 'build-ak1037\libozselftest.a,build-ak1037\libozcore.a' -Name $name -RootPattern '^oz_lockapp_ak1037_' 2>&1 |
    Select-String 'debug hooks|^\s*\d+\s+\d+|error|throw' | ForEach-Object { $_.Line }
$rc = $LASTEXITCODE; "link rc=$rc"; if ($rc -ne 0) { exit 1 }
$undef = (& "${TcW}nm" -u "$app\build\$name.elf" 2>$null | Measure-Object).Count
"undefined symbols: $undef"; if ($undef -ne 0) { exit 1 }

"== burn package"
# mkimage.ps1 writes into the SDK's SHARED burn folder, the one the operator
# burns the bench (_dbg) image from. Save those three files first and put them
# back after, so a release build never changes what the next bench burn flashes
# (found 2026-11-20). This release's files go to .\out\ in this tree.
$shared = @("$app\bin\ozkie_app.bin", "$app\bin\ozkv_blank.bin", "$Sdk\tools\burntool\config_ozkie.txt")
$keep = "$oz\bench-keep"; $outDir = "$oz\out"
New-Item -ItemType Directory -Force $keep, $outDir | Out-Null
foreach ($f in $shared) { if (Test-Path $f) { Copy-Item $f $keep -Force } }
& powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\mkimage.ps1" -Name $name -Sdk $Sdk -Tc $TcW 2>&1 | ForEach-Object { "$_" }
$rc = $LASTEXITCODE; "mkimage rc=$rc"
if ($rc -eq 0) { foreach ($f in $shared) { Copy-Item $f $outDir -Force } }
foreach ($f in $shared) {
    $k = Join-Path $keep (Split-Path $f -Leaf)
    if (Test-Path $k) { Copy-Item $k $f -Force } else { Remove-Item $f -ErrorAction SilentlyContinue }
}
"bench burn folder restored: " + (($shared | ForEach-Object { (Get-FileHash $_ -ErrorAction SilentlyContinue).Hash.Substring(0,12) }) -join ' ')
if ($rc -ne 0) { exit 1 }
"relpkg.ps1 OK $name"
exit 0
