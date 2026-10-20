# ============================================================================
#  atari-pi-baremetal : build the linkage probe kernel
# ----------------------------------------------------------------------------
#  The emulator starts and dies with a black screen on hardware where Circle's
#  own sample program runs perfectly.  Everything that could be checked from the
#  build has been ruled out:
#
#    * the card, the firmware, config.txt and the armstub (Circle's sample runs)
#    * the boot filename (the sample runs renamed to kernel8-rpi4.img)
#    * the kernel load address (both enter at 0x80000)
#    * the image size and Circle's size guard (the guard passes)
#    * static constructors (all are plain data initialisation)
#
#  What is left is a difference in how this project is *linked*.  The one
#  structural difference from the working sample is that this project links
#  newlib's libc and libm in front of Circle's own C library, so newlib's
#  strlen/memset/memcpy/snprintf are used in preference to Circle's, inside a
#  program that Circle's own code also runs in.
#
#  This builds a probe kernel that tests exactly that, and nothing else.  It is
#  built by the same makefile, with the same libraries in the same order, with
#  the same compiler flags and linker script, and the emulation core is still
#  linked in -- so the image has the same character as the real one.  Its only
#  action is to print a series of markers to the screen.
#
#  Reading the result:
#
#    Text appears
#        The linkage is sound.  The fault is inside the emulator's own start-up,
#        and the next step is to bisect that.
#
#    Black screen
#        The fault is the linkage, not the emulator.  That would explain why
#        every check on the emulator's code has come up clean.
#
#  Unlike the emulator, this program keeps running and blinks the activity LED,
#  so "it is running but drawing nothing" can be told from "it stopped".
# ============================================================================

[CmdletBinding()]
param(
    # Must match the board the card will go in.
    [ValidateSet('pi4', 'pi5', 'pi3')]
    [string]$For = 'pi4',

    # Which libraries to link.
    #
    #   full    the emulation core, newlib and Circle -- what this project
    #           normally links, and what produces the black screen
    #   circle  Circle's own library only -- what the sample that boots links
    #
    # Build both.  If 'circle' boots and 'full' does not, the linkage is the
    # fault.  If neither boots, the probe program itself is at fault.  One
    # result on its own would be ambiguous.
    [ValidateSet('full', 'circle')]
    [string]$Linkage = 'full',

    # Which probe source to build.
    #
    #   ours    this project's probe (src/main_probe.cpp)
    #   circle  Circle's own 03-screentext sample, built unchanged through this
    #           makefile.  That sample is known to boot on the card, so it is the
    #           control: it separates 'the build is broken' from 'our probe
    #           source is broken'.
    [ValidateSet('ours', 'circle')]
    [string]$ProbeSource = 'ours'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'env.ps1')

$root     = Get-ProjectRoot
$profiles = Get-Profiles

$profileName = switch ($For) {
    'pi4' { 'kernel64-pi4' }
    'pi5' { 'kernel64-pi5' }
    'pi3' { 'kernel64' }
}
$prof = $profiles[$profileName]

# A separate directory and image name per variant, so that both can be built and
# kept without either overwriting the other.
$variant = if ($ProbeSource -eq 'circle') { 'sample' } else { $Linkage }
$suffix = "-probe-$variant"
$image  = "kernel8-probe-$variant.img"
$outDir = Join-Path $root "build\$profileName$suffix"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

Write-Host ""
Write-Host "  Linkage probe kernel" -ForegroundColor White
Write-Host "  board   : $For  (RASPPI=$($prof.RASPPI))" -ForegroundColor Gray
Write-Host "  linkage : $Linkage" -ForegroundColor Gray
Write-Host "  target  : $image" -ForegroundColor Gray
Write-Host ""

$probeLink = if ($Linkage -eq 'circle') { ' PROBE_LINK=circle' } else { '' }
$probeSrc  = if ($ProbeSource -eq 'circle') { ' PROBE_SRC=circle' } else { '' }

$cmd = "make -f $($root -replace '\\','/')/Makefile.app -j4 " +
       "OUT=$($outDir -replace '\\','/') " +
       "CIRCLEHOME=$($root -replace '\\','/')/external/circle " +
       "CORE_LIB=$($root -replace '\\','/')/build/lib/libatari800.a " +
       "KERNEL_IMAGE_NAME=$image PROBE=1$probeLink$probeSrc " +
       "AARCH=$($prof.AARCH) RASPPI=$($prof.RASPPI) PREFIX=$($prof.Prefix)"

$res = Invoke-Msys -Profile $profileName -Command $cmd -WorkingDirectory $outDir
if ($res.ExitCode -ne 0) {
    Write-Host "  FAIL the probe did not build" -ForegroundColor Red
    $res.Output | Select-Object -Last 40 | ForEach-Object { Write-Host "    $_" }
    exit 1
}

$imagePath = Join-Path $outDir $image
if (-not (Test-Path -LiteralPath $imagePath)) {
    Write-Host "  FAIL no image was produced" -ForegroundColor Red
    exit 1
}

$size = (Get-Item -LiteralPath $imagePath).Length

# Verify here rather than on the Pi: a wrong architecture wastes a test cycle.
$readElf = 'C:\Tools\Gnu\ArmDev\arm-none-eabi\bin\aarch64-none-elf-readelf.exe'
$elf = Join-Path $outDir 'atari-emulator.elf'
if (Test-Path -LiteralPath $elf) {
    $header = & $readElf -h $elf 2>&1 | Out-String
    if ($header -notmatch 'Machine:\s+AArch64' -or $header -notmatch 'Entry point address:\s+0x80000') {
        Write-Host "  FAIL not a valid AArch64 image at 0x80000" -ForegroundColor Red
        exit 1
    }
}

Write-Host ("  OK   {0}  ({1:N0} bytes)" -f $image, $size) -ForegroundColor Green
Write-Host "       $imagePath"
Write-Host ""
Write-Host "  Copy it to the card as kernel8-rpi4.img and power on:" -ForegroundColor White
Write-Host ""
Write-Host "    Copy-Item D:\kernel8-rpi4.img D:\kernel8-rpi4-real.img" -ForegroundColor Yellow
Write-Host "    Copy-Item `"$imagePath`" D:\kernel8-rpi4.img" -ForegroundColor Yellow
Write-Host ""
Write-Host "  PROBE A..G should appear, then the activity LED blinks slowly." -ForegroundColor Gray
