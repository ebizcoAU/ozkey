# Link libozcore.a into the SDK's app_mcu as a separate app_mcu_oz.elf (ozkey-49 P3/P3b).
# By default every global function in libozcore.a is forced in as a GC root, so the size
# is the upper bound (it includes all of Monocypher). -RootPattern '^oz_lock_' roots only the
# lock API the app calls, which is the realistic size. The SDK's own app_mcu.elf is untouched.
#
# Prereqs: app_mcu built with Makefile.cli (its objects in app_mcu\build), and
#          make -f port/ak1037/Makefile run in the ozcore directory.
# Usage:   powershell -NoProfile -ExecutionPolicy Bypass -File port\ak1037\link_app_mcu.ps1
param(
    [string]$Sdk = 'C:\Users\sshuser\ak1037\AnyMicro1037_SDK_V1.02',
    [string]$Tc  = 'C:\Users\sshuser\ak1037\SysGCC\risc-v\bin\riscv64-unknown-elf-',
    [string]$RootPattern = '.'
)
$ErrorActionPreference = 'Stop'
$oz  = (Resolve-Path "$PSScriptRoot\..\..").Path
$lib = "$oz\build-ak1037\libozcore.a"
$app = "$Sdk\app_mcu"

# Roots: global text symbols of libozcore.a.
$roots = & "${Tc}nm" -g --defined-only $lib | Where-Object { $_ -match ' T (\S+)$' } |
         ForEach-Object { $Matches[1] } | Where-Object { $_ -match $RootPattern }

# The SDK link line, taken from Makefile.cli (OBJS + LDFLAGS) so nothing drifts.
$mk   = Get-Content "$app\Makefile.cli"
$objs = (($mk | Where-Object { $_ -like 'OBJS := *' }) -replace '^OBJS := ', '') -split ' '
$ld   = ((($mk | Where-Object { $_ -like 'LDFLAGS := *' }) -replace '^LDFLAGS := ', '') `
         -replace '\$\(TARGET\)', 'app_mcu_oz') -split ' ' | Where-Object { $_ }

# A response file keeps the command line under the Windows limit. GCC reads '\' in it
# as an escape, so the library path goes in with forward slashes.
$rsp = @($objs) + ($lib -replace '\\', '/') + ($roots | ForEach-Object { "-Wl,--undefined=$_" }) + $ld +
       '-o' + 'build/app_mcu_oz.elf'
[IO.File]::WriteAllLines("$app\build\app_mcu_oz.rsp", [string[]]$rsp)

Push-Location $app
try {
    & "${Tc}gcc" "@build/app_mcu_oz.rsp"
    if ($LASTEXITCODE -ne 0) { throw "link failed: $LASTEXITCODE" }
    "roots: $($roots.Count)"
    & "${Tc}size" build/app_mcu.elf build/app_mcu_oz.elf
} finally { Pop-Location }
