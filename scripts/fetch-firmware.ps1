# ============================================================================
#  atari-pi-baremetal : download the Raspberry Pi boot firmware
# ----------------------------------------------------------------------------
#  A bare-metal kernel still needs the Raspberry Pi's own boot firmware: it is
#  the firmware that reads config.txt, sets up the display, loads the kernel
#  image into memory and starts it.  None of it is ours and none of it can be
#  built here -- it is Broadcom/Raspberry Pi binary code -- so the only way to
#  get it is to download it.
#
#  Files and which model needs them:
#
#    start.elf, fixup.dat       Pi Zero 2 W, Pi 3        3.0 MB + 7 KB
#    start4.elf, fixup4.dat     Pi 4, Pi 400, CM4        2.3 MB + 5 KB
#    bootcode.bin               Pi 1-3, Zero, Zero 2 W   53 KB   (not Pi 4/5)
#    bcm27*.dtb                 device tree, one per model
#    overlays/bcm2712d0.dtbo    Pi 5 only
#
#  Why fixup.dat and start.elf come as a pair, since this is the question that
#  prompts most of the confusion: start.elf is the firmware program itself, and
#  fixup.dat tells it where in memory to put things once it has read the
#  kernel's size.  A start.elf without its matching fixup will not boot.  The
#  Pi 4 uses the "4" variants, hence start4.elf with fixup4.dat -- mixing a
#  "start" with a "fixup4", or the other way round, is a common mistake and
#  produces no output at all.
#
#  The Pi 5 needs none of them: its firmware lives in an on-board EEPROM, so it
#  reads the SD card directly.  Only its device tree and overlay are needed.
#
#  Circle downloads these itself with `make` in external/circle/boot, which
#  needs wget.  This script does the same job through PowerShell's own HTTP
#  stack, so it works without wget, without a Unix make, and on a machine where
#  MSYS2 is not installed -- and it verifies each file is the right size, so a
#  truncated download is caught here rather than as a black screen later.
# ============================================================================

[CmdletBinding()]
param(
    # Download only what one model family needs.  Default: everything, so the
    # card works in any board you own.
    [ValidateSet('all', 'pi3', 'pi4', 'pi5')]
    [string]$For = 'all',

    # Re-download files that are already present.
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'env.ps1')

$root   = Get-ProjectRoot
$bootDir = Join-Path $root 'external\circle\boot'

# The firmware revision Circle pins, from external/circle/boot/Makefile.  Using
# the same one means the files match what Circle was tested against; a newer
# revision would probably work but is not what this project was verified with.
$revision = '0641c5bf25d185d5bf25d8dcafd1da6dc8fdbf68'
$baseUrl  = "https://github.com/raspberrypi/firmware/raw/$revision/boot"

# name -> expected size in bytes, as served by that revision.
#
# The size is the integrity check.  There are no published checksums for these
# files, and the firmware is updated in place at a pinned revision only rarely,
# so an exact-size match is a cheap and effective way to catch a truncated or
# error-page download.  A mismatch is reported rather than ignored, because the
# failure mode of a corrupt firmware file is a dark screen with no explanation.
$files = [ordered]@{
    # Bootloader for Pi 1-3 and the Zeros.  The Pi 4 and 5 do not use it.
    'bootcode.bin'                = 52624
    # Firmware pair for Pi Zero 2 W and Pi 3.
    'start.elf'                   = 3023264
    'fixup.dat'                   = 7365
    # Firmware pair for Pi 4, Pi 400, CM4.
    'start4.elf'                  = 2299008
    'fixup4.dat'                  = 5496
    # Device trees.  One is used per model; copying the set makes a card work in
    # any of them.
    'bcm2710-rpi-zero-2-w.dtb'    = 33672
    'bcm2711-rpi-4-b.dtb'         = 56247
    'bcm2711-rpi-400.dtb'         = 56251
    'bcm2711-rpi-cm4.dtb'         = 56768
    'bcm2712-rpi-5-b.dtb'         = 78723
    'bcm2712d0-rpi-5-b.dtb'       = 78727
    # Licence files.  Broadcom's firmware licence and the Linux kernel's, which
    # covers the device trees.  Not needed to boot, but they belong with the
    # files they cover.
    'LICENCE.broadcom'            = 1594
    'COPYING.linux'               = 18693
}

# Only fetched for Pi 5.
$overlays = [ordered]@{
    'bcm2712d0.dtbo' = 1363
}

# Which entries each family actually needs.
#
# Every family's list is complete in itself rather than being filtered out of
# one master list, because the two sets of files are not the same kind of thing:
# the overlays live in a subdirectory and are downloaded from a different URL.
# Filtering one list against another is how the Pi 5 overlay came to be silently
# skipped -- the overlay's name was never in the master list to be found.
$forPi3 = @(
    'bootcode.bin', 'start.elf', 'fixup.dat',
    'bcm2710-rpi-zero-2-w.dtb'
)
$forPi4 = @(
    'start4.elf', 'fixup4.dat',
    'bcm2711-rpi-4-b.dtb', 'bcm2711-rpi-400.dtb', 'bcm2711-rpi-cm4.dtb'
)
$forPi5 = @(
    'bcm2712-rpi-5-b.dtb', 'bcm2712d0-rpi-5-b.dtb',
    'overlays/bcm2712d0.dtbo'
)

# The licence files cover the firmware and the device trees, so they always
# travel with them.
$alwaysWanted = @('LICENCE.broadcom', 'COPYING.linux')

# Every name this script knows how to fetch, for the "unknown name" guard below.
$knownFiles = @($files.Keys) + @($overlays.Keys | ForEach-Object { "overlays/$_" })

$wanted = switch ($For) {
    'pi3'   { @($forPi3) }
    'pi4'   { @($forPi4) }
    'pi5'   { @($forPi5) }
    default { @($forPi3) + @($forPi4) + @($forPi5) }
}
$wanted = @($wanted) + $alwaysWanted

# Guards against a typo in one of the lists above going unnoticed: a name that
# no URL is known for would otherwise be skipped in silence, which is exactly
# the bug this rewrite fixes.
foreach ($name in $wanted) {
    if ($knownFiles -notcontains $name) {
        throw "internal error: '$name' is selected for $For but has no download URL"
    }
}

if (-not (Test-Path -LiteralPath $bootDir)) {
    throw "Circle's boot directory is missing: $bootDir`nRun scripts\fetch-sources.ps1 first."
}

Write-Host ""
Write-Host "  Raspberry Pi boot firmware" -ForegroundColor White
Write-Host "  revision : $revision" -ForegroundColor Gray
Write-Host "  for      : $For" -ForegroundColor Gray
Write-Host "  into     : external\circle\boot" -ForegroundColor Gray
Write-Host ""

# GitHub serves these through a redirect to a CDN; Invoke-WebRequest follows it.
$ProgressPreference = 'SilentlyContinue'
$downloaded = 0
$skipped = 0
$failed = @()

function Get-FirmwareFile {
    param([string]$Name, [string]$Url, [int]$ExpectedSize)

    $target = Join-Path $bootDir $Name

    if ((Test-Path -LiteralPath $target) -and -not $Force) {
        $existing = (Get-Item -LiteralPath $target).Length
        if ($ExpectedSize -eq 0 -or $existing -eq $ExpectedSize) {
            Write-Host ("  --   {0,-28} already present" -f $Name) -ForegroundColor DarkGray
            $script:skipped++
            return
        }
        Write-Host ("  ..   {0,-28} wrong size, re-fetching" -f $Name) -ForegroundColor Yellow
    }

    try {
        Invoke-WebRequest -Uri $Url -OutFile $target -UseBasicParsing
    }
    catch {
        $script:failed += "$Name : $($_.Exception.Message)"
        Write-Host ("  FAIL {0,-28} {1}" -f $Name, $_.Exception.Message) -ForegroundColor Red
        return
    }

    $size = (Get-Item -LiteralPath $target).Length

    if ($ExpectedSize -ne 0 -and $size -ne $ExpectedSize) {
        $script:failed += "$Name : got $size bytes, expected $ExpectedSize"
        Write-Host ("  FAIL {0,-28} {1} bytes, expected {2}" -f $Name, $size, $ExpectedSize) -ForegroundColor Red
        Remove-Item -LiteralPath $target -Force
        return
    }

    Write-Host ("  OK   {0,-28} {1,10:N0} bytes" -f $Name, $size) -ForegroundColor Green
    $script:downloaded++
}

# Regular files at the root of boot/.
foreach ($name in @($wanted | Where-Object { $files.Contains($_) })) {
    Get-FirmwareFile -Name $name -Url "$baseUrl/$name" -ExpectedSize $files[$name]
}

# Overlays, which live in boot/overlays and are downloaded from there.  This is
# a separate loop because the URL and the destination both differ by one path
# component, not because the selection differs.
$overlayNames = @($wanted | Where-Object { $_ -like 'overlays/*' })
if ($overlayNames.Count -gt 0) {
    $overlayDir = Join-Path $bootDir 'overlays'
    New-Item -ItemType Directory -Force -Path $overlayDir | Out-Null

    foreach ($name in $overlayNames) {
        Get-FirmwareFile -Name $name `
                         -Url "$baseUrl/$name" `
                         -ExpectedSize $overlays[(Split-Path $name -Leaf)]
    }
}

Write-Host ""

if (@($failed).Count -gt 0) {
    Write-Host "  $(@($failed).Count) file(s) could not be fetched:" -ForegroundColor Red
    @($failed) | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
    Write-Host ""
    Write-Host "  If the network is unavailable, download the same files by hand from" -ForegroundColor Yellow
    Write-Host "  https://github.com/raspberrypi/firmware/tree/$revision/boot" -ForegroundColor Yellow
    Write-Host "  and put them in external\circle\boot\." -ForegroundColor Yellow
    exit 1
}

Write-Host "  $downloaded downloaded, $skipped already present." -ForegroundColor Green
Write-Host ""
Write-Host "  Copy these to the root of the SD card:" -ForegroundColor Gray
Write-Host "    bootcode.bin start.elf fixup.dat start4.elf fixup4.dat" -ForegroundColor Gray
Write-Host "    the bcm27*.dtb files, and overlays\bcm2712d0.dtbo" -ForegroundColor Gray
Write-Host ""
Write-Host "  Only the files your board uses are read; copying the set makes one" -ForegroundColor Gray
Write-Host "  card work in every supported model.  See docs/sd-card-setup.md." -ForegroundColor Gray
