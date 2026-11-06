# Package a linked image for Anyka_BurnCli_Tool (ozkey-49 P4 item 5).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File port\ak1037\burn\mkimage.ps1 -Name app_mcu_ozlock
#
# 1. app_mcu\build\<Name>.elf -> app_mcu\bin\ozkie_app.bin: .bootcode + .data, then .flash
#    (the SDK's own layout, same as the reference app_mcu.bin), and checks it fits PROG.
# 2. app_mcu\bin\ozkv_blank.bin: the OZKV partition, all 0xFF, so the first boot formats it.
#    Burning it wipes the lock's identity and bonds; drop that download line to keep them.
# 3. tools\burntool\config_ozkie.txt: config_ozkie.txt from the repo, in the UTF-16LE + BOM
#    and CRLF the tool reads.
# Then, from tools\burntool: Anyka_BurnCli_Tool.exe -b 2000000 -f config_ozkie.txt
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$Sdk = 'C:\Users\sshuser\ak1037\AnyMicro1037_SDK_V1.02',
    [string]$Tc  = 'C:\Users\sshuser\ak1037\SysGCC\risc-v\bin\riscv64-unknown-elf-'
)
$ErrorActionPreference = 'Stop'
$build = "$Sdk\app_mcu\build"
$bin   = "$Sdk\app_mcu\bin"
$elf   = "$build\$Name.elf"
$cfgIn = "$PSScriptRoot\config_ozkie.txt"
if (-not (Test-Path $elf)) { throw "no $elf (link it with link_app_mcu.ps1 -Name $Name)" }

& "${Tc}objcopy" -O binary -j .bootcode -j .data $elf "$build\$Name.ram.bin"
if ($LASTEXITCODE -ne 0) { throw "objcopy ram failed" }
& "${Tc}objcopy" -O binary -j .flash $elf "$build\$Name.flash.bin"
if ($LASTEXITCODE -ne 0) { throw "objcopy flash failed" }
$img = [IO.File]::ReadAllBytes("$build\$Name.ram.bin") + [IO.File]::ReadAllBytes("$build\$Name.flash.bin")
[IO.File]::WriteAllBytes("$bin\ozkie_app.bin", [byte[]]$img)

# Partition sizes (KiB) straight from the config, so the check can't drift from it.
$cfg  = Get-Content $cfgIn
$part = @{}
foreach ($l in $cfg) {
    if ($l -match '^\s*partition\d+\s*=\s*(\w+)\s*,.*,\s*(\d+)\s*$') { $part[$Matches[1]] = [int]$Matches[2] * 1024 }
}
$prog = $part['PROG']
if ($img.Length -gt $prog) { throw "ozkie_app.bin is $($img.Length) B, PROG is $prog B" }
"ozkie_app.bin  {0:N0} B of PROG {1:N0} B ({2:N0} B free)  sha256 {3}" -f $img.Length, $prog,
    ($prog - $img.Length), (Get-FileHash "$bin\ozkie_app.bin").Hash

$kv = New-Object byte[] $part['OZKV']
for ($i = 0; $i -lt $kv.Length; $i++) { $kv[$i] = 0xFF }
[IO.File]::WriteAllBytes("$bin\ozkv_blank.bin", $kv)
"ozkv_blank.bin {0:N0} B" -f $kv.Length

# The tool reads UTF-16LE with a BOM and CRLF line ends, like the SDK's own configs.
[IO.File]::WriteAllText("$Sdk\tools\burntool\config_ozkie.txt", (($cfg -join "`r`n") + "`r`n"),
                        (New-Object Text.UnicodeEncoding($false, $true)))
"config_ozkie.txt written to $Sdk\tools\burntool"
