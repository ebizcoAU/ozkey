# Link libozcore.a into the SDK's app_mcu as a separate app_mcu_oz.elf (ozkey-49 P3/P3b).
# By default every global function in libozcore.a is forced in as a GC root, so the size
# is the upper bound (it includes all of Monocypher). -RootPattern '^oz_lock_' roots only the
# lock API the app calls, which is the realistic size. The SDK's own app_mcu.elf is untouched.
# The bring-up images (ozkey-49 P4 item 4b) add libozselftest.a; the roots pick the image:
#   -Libs build-ak1037\libozselftest.a,build-ak1037\libozcore.a -Name app_mcu_ozselftest -RootPattern '^oz_selftest_ak1037_'
#   -Libs build-ak1037\libozselftest.a,build-ak1037\libozcore.a -Name app_mcu_ozlock -RootPattern '^oz_lockapp_ak1037_'
# Each image's MSH entries (FSymTab, KEEP in app_mcu.lds) come in with its object.
#
# Prereqs: app_mcu built with Makefile.cli (its objects in app_mcu\build), and
#          make -f port/ak1037/Makefile run in the ozcore directory.
# Usage:   powershell -NoProfile -ExecutionPolicy Bypass -File port\ak1037\link_app_mcu.ps1
param(
    [string]$Sdk = 'C:\Users\sshuser\ak1037\AnyMicro1037_SDK_V1.02',
    [string]$Tc  = 'C:\Users\sshuser\ak1037\SysGCC\risc-v\bin\riscv64-unknown-elf-',
    [string]$RootPattern = '.',
    [string[]]$Libs = @('build-ak1037\libozcore.a'),
    [string]$Name = 'app_mcu_oz'
)
$ErrorActionPreference = 'Stop'
$oz  = (Resolve-Path "$PSScriptRoot\..\..").Path
# powershell -File passes "a,b" as one string, so split it here.
$libs = $Libs -split ',' | Where-Object { $_ } | ForEach-Object { "$oz\$_" }
$app = "$Sdk\app_mcu"

# Roots: global text symbols of libozcore.a.
$roots = $libs | ForEach-Object { & "${Tc}nm" -g --defined-only $_ } | Where-Object { $_ -match ' T (\S+)$' } |
         ForEach-Object { $Matches[1] } | Where-Object { $_ -match $RootPattern }

# The SDK link line, taken from Makefile.cli (OBJS + LDFLAGS) so nothing drifts.
$mk   = Get-Content "$app\Makefile.cli"
$objs = (($mk | Where-Object { $_ -like 'OBJS := *' }) -replace '^OBJS := ', '') -split ' '
$ld   = ((($mk | Where-Object { $_ -like 'LDFLAGS := *' }) -replace '^LDFLAGS := ', '') `
         -replace '\$\(TARGET\)', $Name) -split ' ' | Where-Object { $_ }

# A response file keeps the command line under the Windows limit. GCC reads '\' in it
# as an escape, so the library path goes in with forward slashes.
$rsp = @($objs) + '-Wl,--start-group' + ($libs | ForEach-Object { $_ -replace '\\', '/' }) + '-Wl,--end-group' +
       ($roots | ForEach-Object { "-Wl,--undefined=$_" }) + $ld +
       '-o' + "build/$Name.elf"
[IO.File]::WriteAllLines("$app\build\$Name.rsp", [string[]]$rsp)

Push-Location $app
try {
    & "${Tc}gcc" "@build/$Name.rsp"
    if ($LASTEXITCODE -ne 0) { throw "link failed: $LASTEXITCODE" }
    "roots: $($roots.Count)"
    & "${Tc}size" build/app_mcu.elf "build/$Name.elf"
} finally { Pop-Location }
