# ============================================================================
#  atari-pi-baremetal : build Circle's screen test as a diagnostic kernel
# ----------------------------------------------------------------------------
#  When a black screen appears, the first question is whether *any* kernel runs
#  on the card at all.  Guessing at that wastes a lot of time, and the emulator
#  is a large program with many ways to fail.
#
#  This builds Circle's own 03-screentext sample -- about twenty lines that
#  initialise the screen, print the character set, and blink the activity LED --
#  as a kernel image alongside this project's.  It uses none of this project's
#  code.
#
#  How to use it:
#
#    1. Run this script; it writes build/diagnostic/kernel8-diagnostic.img
#    2. Copy that file to the SD card
#    3. Add this line to the [pi4] section of config.txt on the card:
#
#           kernel=kernel8-diagnostic.img
#
#    4. Power on.
#
#  If text appears, the card, firmware, config.txt, armstub and boot chain are
#  all working, and the problem is inside this project.  If the screen stays
#  black, none of that can be blamed on the emulator -- and the next thing to
#  suspect is the boot chain itself.
#
#  The image is deliberately named differently from any real one so that copying
#  it cannot overwrite the working kernel, and so that it is obvious on the card
#  what it is.
# ============================================================================

[CmdletBinding()]
param(
    # Which board the diagnostic is for.  This must match the machine you will
    # put the card in: the Pi 4 family reads kernel8-rpi4.img from [pi4].
    [ValidateSet('pi4', 'pi5', 'pi3')]
    [string]$For = 'pi4'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'env.ps1')

$root     = Get-ProjectRoot
$sample   = Join-Path $root 'external\circle\sample\03-screentext'
$outDir   = Join-Path $root 'build\diagnostic'
$profiles = Get-Profiles

# Map the board to one of this project's profiles, which already carries the
# right RASPPI and toolchain prefix.
$profileName = switch ($For) {
    'pi4' { 'kernel64-pi4' }
    'pi5' { 'kernel64-pi5' }
    'pi3' { 'kernel64' }
}
$prof = $profiles[$profileName]

if (-not (Test-Path -LiteralPath $sample)) {
    throw "Circle's sample is missing: $sample`nRun scripts\fetch-sources.ps1 first."
}

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

Write-Host ""
Write-Host "  Circle screen-test diagnostic kernel" -ForegroundColor White
Write-Host "  board   : $For  (RASPPI=$($prof.RASPPI), $($prof.Prefix))" -ForegroundColor Gray
Write-Host "  sample  : external\circle\sample\03-screentext" -ForegroundColor Gray
Write-Host ""

# Circle's own sample build.  It has its own Makefile, which uses ../Rules.mk,
# so the flags come from the same place this project's do.  The image name is
# whichever Circle produces for that RASPPI value.
$target = switch ($prof.RASPPI) {
    3 { 'kernel8' }
    4 { 'kernel8-rpi4' }
    5 { 'kernel_2712' }
    default { throw "unexpected RASPPI $($prof.RASPPI)" }
}

$cmd = "make -C external/circle/sample/03-screentext clean >/dev/null 2>&1; " +
       "make -C external/circle/sample/03-screentext " +
       "RASPPI=$($prof.RASPPI) AARCH=$($prof.AARCH) PREFIX64=$($prof.Prefix)"

$res = Invoke-Msys -Profile $profileName -Command $cmd -WorkingDirectory $root
if ($res.ExitCode -ne 0) {
    Write-Host "  FAIL the sample did not build" -ForegroundColor Red
    $res.Output | Select-Object -Last 40 | ForEach-Object { Write-Host "    $_" }
    exit 1
}

$built = Join-Path $sample "$target.img"
if (-not (Test-Path -LiteralPath $built)) {
    Write-Host "  FAIL no image was produced (expected $built)" -ForegroundColor Red
    $res.Output | Select-Object -Last 30 | ForEach-Object { Write-Host "    $_" }
    exit 1
}

$destination = Join-Path $outDir 'kernel8-diagnostic.img'
Copy-Item -LiteralPath $built -Destination $destination -Force

$size = (Get-Item -LiteralPath $destination).Length

# Sanity-check it here rather than on the Pi: a wrong architecture would waste a
# whole test cycle.
$readElf = 'C:\Tools\Gnu\ArmDev\arm-none-eabi\bin\aarch64-none-elf-readelf.exe'
$elf = Join-Path $sample "$target.elf"
if (Test-Path -LiteralPath $elf) {
    $header = & $readElf -h $elf 2>&1 | Out-String
    if ($header -notmatch 'Machine:\s+AArch64') {
        Write-Host "  FAIL the sample is not AArch64" -ForegroundColor Red
        exit 1
    }
}

Write-Host ("  OK   kernel8-diagnostic.img  ({0:N0} bytes)" -f $size) -ForegroundColor Green
Write-Host "       $destination"
Write-Host ""
Write-Host "  Next:" -ForegroundColor White
Write-Host "    1. Copy it to the root of the SD card." -ForegroundColor Gray
Write-Host "    2. Add this line to the [pi4] section of config.txt on the card:" -ForegroundColor Gray
Write-Host "" 
Write-Host "           kernel=kernel8-diagnostic.img" -ForegroundColor Yellow
Write-Host ""
Write-Host "    3. Power on.  Circle's character set should appear." -ForegroundColor Gray
Write-Host ""
Write-Host "  Text on screen means the card, firmware, config.txt, armstub and" -ForegroundColor Gray
Write-Host "  boot chain all work, and the fault is in this project." -ForegroundColor Gray
Write-Host "  A black screen means the fault is in the boot chain, not the emulator." -ForegroundColor Gray
