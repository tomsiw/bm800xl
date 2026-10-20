#Requires -Version 5.1
<#
.SYNOPSIS
    Checks that the built kernel images are sound and for the right board.

.DESCRIPTION
    A build that merely produced files is not proof of much.  This script reads
    each image's ELF header and asserts:

      * the machine type is AArch64;
      * the entry point is at 0x80000, which is where Circle's linker script
        places it for AArch64;
      * the image is smaller than Circle's kernel size limit (2 MiB), because exceeding
        it makes sysinit() halt silently with no console output;
      * _start exists, so the image can actually boot;
      * the image is named what the firmware looks for on the board it was
        built for -- kernel8.img for the Zero 2 W through the Pi 4, and
        kernel_2712.img for the Pi 5.  Getting this wrong means the firmware
        never loads the file, or loads a build for a different board.

    Run it after scripts/build.ps1 to confirm a build is sound.

.PARAMETER Profile
    Which profile to check.  Default: every profile that has been built.

.EXAMPLE
    pwsh -ExecutionPolicy Bypass -File scripts\verify.ps1
#>
[CmdletBinding()]
param(
    [string[]]$Profile = @('kernel64', 'kernel64-pi5', 'kernel64-pi4')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'env.ps1')

$root = Get-ProjectRoot
$profiles = Get-Profiles
$anyFailed = $false
$armDev = 'C:\Tools\Gnu\ArmDev\arm-none-eabi\bin'

# AArch64 only: this project builds no 32-bit image, and asserting that is the
# point -- an AArch32 object slipping into the link is the failure mode worth
# catching, not one worth tolerating.
$readElfExe = Join-Path $armDev 'aarch64-none-elf-readelf.exe'
$nmExe      = Join-Path $armDev 'aarch64-none-elf-nm.exe'

# What the firmware looks for, per RASPPI.  An image under the wrong name is
# never loaded, which presents as "nothing on screen" with no other clue.
$expectedImage = @{
    '3' = 'kernel8.img'
    '4' = 'kernel8-rpi4.img'
    '5' = 'kernel_2712.img'
}

foreach ($name in $Profile) {
    $prof = $profiles[$name]
    if (-not $prof) { continue }

    $img = Join-Path $root "build\$name\$($prof.Image)"
    if (-not (Test-Path -LiteralPath $img)) {
        Write-Host "  --   $name : not built ($($prof.Image) missing)" -ForegroundColor DarkGray
        continue
    }

    $issues = New-Object System.Collections.Generic.List[string]

    # The image must be named what the firmware looks for on this board.
    $wanted = $expectedImage["$($prof.RASPPI)"]
    if ($wanted -and $prof.Image -ne $wanted) {
        $issues.Add("RASPPI=$($prof.RASPPI) boots '$wanted', but this profile produces '$($prof.Image)'")
    }

    $size = (Get-Item -LiteralPath $img).Length
    if ($size -gt (Get-KernelMaxSizeBytes)) {
        $issues.Add("image is $([math]::Round($size/1KB,1)) KiB, over Circle's 2 MiB kernel limit")
    }
    if ($size -lt 64KB) {
        $issues.Add("image is only $size bytes, which is too small to be a kernel")
    }

    # The ELF is written next to the image and carries the header we need.
    $elf = Join-Path $root "build\$name\atari-emulator.elf"
    if (-not (Test-Path -LiteralPath $elf)) {
        $issues.Add("atari-emulator.elf is missing, so the image cannot be checked")
    }
    else {
        $header = & $readElfExe -h $elf 2>&1 | Out-String

        if ($header -notmatch 'Machine:\s+AArch64') {
            $machine = if ($header -match 'Machine:\s+(.+)') { $Matches[1].Trim() } else { '?' }
            $issues.Add("machine type is '$machine', expected 'AArch64'")
        }

        if ($header -notmatch 'Class:\s+ELF64') {
            $issues.Add("ELF class is not ELF64; a 32-bit object has been linked in")
        }

        # Circle's linker script places an AArch64 kernel at 0x80000.  If the
        # entry point is elsewhere the firmware jumps into nothing.
        if ($header -notmatch 'Entry point address:\s+0x80000') {
            $entry = if ($header -match 'Entry point address:\s+(\S+)') { $Matches[1] } else { '?' }
            $issues.Add("entry point is $entry, expected 0x80000")
        }

        $symbols = & $nmExe $elf 2>&1 | Out-String
        if ($symbols -notmatch '_start') {
            $issues.Add("the _start symbol is missing, so the image cannot boot")
        }
    }

    if ($issues.Count -eq 0) {
        Write-Host ("  OK   {0,-15} {1,-18} {2,8} KiB  {3}" -f `
            $name, $prof.Image, [math]::Round($size/1KB,1), $prof.Board) -ForegroundColor Green
    }
    else {
        Write-Host ("  FAIL {0,-14} {1}" -f $name, $prof.Image) -ForegroundColor Red
        foreach ($issue in $issues) { Write-Host "         - $issue" -ForegroundColor Red }
        $anyFailed = $true
    }
}

if ($anyFailed) {
    Write-Host ""
    Write-Host "  Verification FAILED." -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "  All built images verified." -ForegroundColor Green
exit 0
