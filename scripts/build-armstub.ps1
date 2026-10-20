# ============================================================================
#  atari-pi-baremetal : build the Circle ARM stub for the Raspberry Pi 4
# ----------------------------------------------------------------------------
#  The Pi 4's firmware loads a small ARM stub before the kernel.  Circle needs
#  its own replacement, which sets up the GIC the way Circle's interrupt code
#  expects and releases the secondary CPU cores.
#
#  There is nowhere to download it from: Circle ships the assembly source
#  (external/circle/boot/armstub/armstub8.S) and expects you to build it.  This
#  script does exactly what Circle's own `make armstub64` does, using the
#  AArch64 toolchain this project already uses, and puts the result where
#  Circle's build system would put it.
#
#  The file is 336 bytes.  Only the Pi 4, Pi 400 and CM4 load it: the Pi 5 takes
#  its firmware from on-board EEPROM and has no armstub line, and the earlier
#  models use a different stub.
#
#  This project does not use FIQ, so it does not strictly need the stub -- but
#  Circle's config64.txt names it under [pi4], and a config.txt referring to a
#  file that is not on the card is worth not having.
# ============================================================================

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'env.ps1')

$root        = Get-ProjectRoot
$armstubDir  = Join-Path $root 'external\circle\boot\armstub'
$bootDir     = Join-Path $root 'external\circle\boot'
$source      = Join-Path $armstubDir 'armstub8.S'
$output      = Join-Path $bootDir 'armstub8-rpi4.bin'

if (-not (Test-Path -LiteralPath $source)) {
    throw "Circle's armstub source is missing: $source`nRun scripts\fetch-sources.ps1 first."
}

$prefix = $Prefix64
$relative = 'external/circle/boot/armstub'
$relativeBoot = 'external/circle/boot'

# Circle's armstub Makefile does four things: compile, link at address 0,
# disassemble for reference, and strip to a raw binary.  They are spelled out
# rather than driven through that makefile so that the PREFIX64 variable cannot
# be missed -- unset, it would silently try to use a 32-bit compiler on a
# 64-bit source.
$commands = @(
    "$($prefix)gcc -DGIC=1 -o armstub8-rpi4.o -c armstub8.S"
    "$($prefix)ld --section-start=.text=0 -o armstub8-rpi4.elf armstub8-rpi4.o"
    "$($prefix)objdump -D armstub8-rpi4.elf > armstub8-rpi4.lst"
    "$($prefix)objcopy armstub8-rpi4.elf -O binary armstub8-rpi4.bin"
) -join ' && '

$result = Invoke-Msys -Profile 'kernel64-pi4' -Command $commands -WorkingDirectory $armstubDir
if ($result.ExitCode -ne 0) {
    Write-Host "  FAIL the ARM stub could not be built" -ForegroundColor Red
    $result.Output | Select-Object -Last 30 | ForEach-Object { Write-Host "    $_" }
    exit 1
}

# Circle's build system copies the result up into boot/, which is where the SD
# card instructions tell you to look for it.
$built = Join-Path $armstubDir 'armstub8-rpi4.bin'
if (-not (Test-Path -LiteralPath $built)) {
    Write-Host "  FAIL the build reported success but produced no armstub8-rpi4.bin" -ForegroundColor Red
    exit 1
}
Copy-Item -LiteralPath $built -Destination $output -Force

$size = (Get-Item -LiteralPath $output).Length
Write-Host ""
Write-Host ("  OK   armstub8-rpi4.bin  ({0} bytes)" -f $size) -ForegroundColor Green
Write-Host "       $output"
Write-Host ""
Write-Host "  Copy it to the root of the SD card next to kernel8-rpi4.img." -ForegroundColor Gray
Write-Host "  Only the Pi 4, Pi 400 and CM4 load it." -ForegroundColor Gray
