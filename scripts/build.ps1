#Requires -Version 5.1
<#
.SYNOPSIS
    Cross-compile the bare-metal Atari XL/XE emulator for Raspberry Pi.

.DESCRIPTION
    Builds, in order:

      1. the Atari800 emulation core  -> build/lib/libatari800.a
      2. the Circle libraries          -> external/circle/lib*/**.a
      3. the emulator application      -> build/<profile>/kernel*.img

    Circle, the core and the application are all built for AArch64.  This project
    does not build AArch32: every model it supports has a 64-bit core, and the
    Pi 5 can only run AArch64.

    Two images cover the supported models, and both can live on one SD card
    because the firmware picks between them by name:

      kernel8.img       Zero 2 W, Pi 3, Pi 4, Pi 400
      kernel_2712.img   Pi 5

    The Pi 5 needs its own build, not merely its own name: its peripherals are
    behind the RP1 southbridge and Circle compiles that support in only for
    RASPPI=5.

.PARAMETER Profile
    kernel64       kernel8.img      Zero 2 W, Pi 3, Pi 4, Pi 400   <- start here
    kernel64-pi5   kernel_2712.img  Raspberry Pi 5
    kernel64-pi4   kernel8-rpi4.img Pi 4/400, tuned for Cortex-A72

.PARAMETER Target
    all (default), core, circle, app, clean

.PARAMETER Jobs
    Parallel make jobs; defaults to the number of logical processors.

.EXAMPLE
    pwsh -ExecutionPolicy Bypass -File scripts\build.ps1 -Profile kernel64
#>
[CmdletBinding()]
param(
    [ValidateSet('kernel64', 'kernel64-pi5', 'kernel64-pi4')]
    [string]$Profile = 'kernel64',

    [ValidateSet('all', 'core', 'circle', 'app', 'clean')]
    [string]$Target = 'all',

    [int]$Jobs = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'env.ps1')

if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

$root   = Get-ProjectRoot
$prof   = (Get-Profiles)[$Profile]
$objDir = "build/coreobj"

# Which target Circle is currently built for.  Every stage that can be run on
# its own has to check this: Circle's objects are shared by all targets, and
# mixing two RASPPI values fails at link time with "file format not recognized",
# or links silently against the wrong board's peripherals.
$circleDir = Join-Path $ExternalDir 'circle'
$circleStamp = Join-Path $circleDir '.built-for'
$circleTarget = "$($prof.AARCH)/$($prof.RASPPI)"
$circlePrevious = if (Test-Path -LiteralPath $circleStamp) {
    (Get-Content -LiteralPath $circleStamp -Raw).Trim()
} else { '' }

function Test-CircleArchitecture {
    if ($circlePrevious -ne '' -and $circlePrevious -ne $circleTarget) {
        Write-Fail "Circle is built for $circlePrevious, but this profile needs $circleTarget"
        throw "Run: .\scripts\build.ps1 -Profile $Profile   (without -Target, so Circle is rebuilt)"
    }
}

Write-Host ""
Write-Host "  bare-metal Atari XL/XE emulator" -ForegroundColor White
Write-Host "  profile : $Profile  ($($prof.Board))" -ForegroundColor Gray
Write-Host "  arch    : AARCH=$($prof.AARCH)  RASPPI=$($prof.RASPPI)  prefix=$($prof.Prefix)" -ForegroundColor Gray
Write-Host "  image   : $($prof.Image)" -ForegroundColor Gray
Write-Host "  jobs    : $Jobs" -ForegroundColor Gray

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------
if ($Target -eq 'clean') {
    Write-Step 'Cleaning'
    Invoke-Msys -Profile $Profile -Command "rm -rf build" | Out-Null
    Invoke-Msys -Profile $Profile -Command "make -C external/circle clean" | Out-Null
    Write-Ok 'clean'
    return
}

# ---------------------------------------------------------------------------
# 1. Atari800 emulation core
# ---------------------------------------------------------------------------
if ($Target -in @('all', 'core')) {
    Write-Step 'Building Atari800 emulation core'

    # The archive is at a fixed path because the core compiles identically for
    # every board: the differences between them live in Circle's drivers, not in
    # the emulation.  The object directory is still stamped per target, so a core
    # left over from another profile cannot slip through.
    $libDir = "build/lib"

    $cmd = "make -f Makefile.core -j$Jobs " +
           "CROSS=$($prof.Prefix) AARCH=$($prof.AARCH) RASPPI=$($prof.RASPPI) " +
           "OBJDIR=$objDir LIBDIR=$libDir"
    $res = Invoke-Msys -Profile $Profile -Command $cmd
    if ($res.ExitCode -ne 0) {
        Write-Fail 'core build failed'
        $res.Output | Select-Object -Last 80 | ForEach-Object { Write-Host "    $_" }
        throw 'core build failed'
    }
    Write-Ok "libatari800.a ($Profile) -> $libDir"
}

# ---------------------------------------------------------------------------
# 2. Circle libraries for this architecture
# ---------------------------------------------------------------------------
if ($Target -in @('all', 'circle')) {
    Write-Step "Building Circle libraries (AARCH=$($prof.AARCH), RASPPI=$($prof.RASPPI))"

    # Circle keeps one set of object files and archives, shared by every target,
    # and its build system has no notion of switching between them.  Building
    # for a different RASPPI without cleaning therefore leaves stale objects
    # behind, and the link fails later with "file format not recognized" -- or,
    # worse, links objects for the wrong board and faults on the Pi.  A stamp
    # records which target the tree was last built for, and a switch cleans
    # first.
    if ($circlePrevious -ne $circleTarget) {
        if ($circlePrevious -ne '') {
            Write-Host "    Circle was built for $circlePrevious; cleaning for $circleTarget" -ForegroundColor Yellow
        }

        # 'makeall clean' runs clean in every one of Circle's own directories
        # and removes only build products, never sources.  The two addons are
        # cleaned separately because makeall does not know about them, and a
        # stale archive there fails the link with "file in wrong format".
        $res = Invoke-Msys -Profile $Profile `
            -Command "cd external/circle && ./makeall clean" -WorkingDirectory $root
        if ($res.ExitCode -ne 0) {
            Write-Fail 'Circle clean failed'
            $res.Output | Select-Object -Last 40 | ForEach-Object { Write-Host "    $_" }
            throw 'Circle clean failed'
        }

        foreach ($addon in @('fatfs', 'SDCard')) {
            $res = Invoke-Msys -Profile $Profile `
                -Command "make -C external/circle/addon/$addon clean"
        }

        Set-Content -LiteralPath $circleStamp -Value $circleTarget -NoNewline

        # The stamp has just changed; keep the variable the app stage checks in
        # step with it, or that stage would reject the build we only just made.
        $circlePrevious = $circleTarget
    }

    # Circle's build system is driven from its own root.  Everything is passed
    # on the make command line (which overrides Config.mk), so the vendored
    # tree stays pristine.
    #
    # The order of these matters, and not in an obvious way.  Circle's makeall
    # script forwards only its first two arguments to the sub-makes it spawns
    # (`make $1 $2`), and its own default for RASPPI is 1.  So the two arguments
    # that must survive are RASPPI and AARCH: with RASPPI third it is silently
    # dropped, and the sub-make falls back to RASPPI=1 -- which fails outright
    # for an AArch64 build ("RASPPI must be set to 3, 4 or 5") and would produce
    # a 32-bit library mismatched with the target otherwise.
    $circleVars = "RASPPI=$($prof.RASPPI) AARCH=$($prof.AARCH)"

    # 'makeall' is Circle's own driver: there is no top-level Makefile.  It
    # walks tools/, lib/ (with usb/gadget, input, fs/fat, sched, net and sound)
    # and runs make in each, forwarding its first two arguments.
    #
    # KERNEL_MAX_SIZE is deliberately not mentioned here or anywhere else in the
    # build.  It is not a setting that only affects the compiler: the firmware
    # reserves the kernel's memory using the same figure and is never told when
    # it changes.  Circle's 2 MiB default is what matches the firmware, so it is
    # left alone.  See external/circle/Config2.mk.
    $res = Invoke-Msys -Profile $Profile `
        -Command "cd external/circle && ./makeall $circleVars -j$Jobs" -WorkingDirectory $root
    if ($res.ExitCode -ne 0) {
        Write-Fail 'Circle build failed'
        $res.Output | Select-Object -Last 80 | ForEach-Object { Write-Host "    $_" }
        throw 'Circle build failed'
    }
    Write-Ok "Circle libraries (AARCH=$($prof.AARCH), RASPPI=$($prof.RASPPI))"

    # addon/fatfs and addon/SDCard are NOT part of Circle's makeall.  FatFs is
    # how everything reads and writes files, and the SDCard addon is how the
    # settings, snapshots and ROM images are reached on the card, so both are
    # needed.
    $addonVars = "RASPPI=$($prof.RASPPI) AARCH=$($prof.AARCH) PREFIX=$Prefix64"

    foreach ($addon in @('fatfs', 'SDCard')) {
        Write-Step "Building Circle addon: $addon"
        $res = Invoke-Msys -Profile $Profile `
            -Command "make -C external/circle/addon/$addon -j$Jobs $addonVars"
        if ($res.ExitCode -ne 0) {
            Write-Fail "$addon addon build failed"
            $res.Output | Select-Object -Last 60 | ForEach-Object { Write-Host "    $_" }
            throw "$addon addon build failed"
        }

        $lib = Join-Path $ExternalDir "circle\addon\$addon"
        $built = Get-ChildItem -LiteralPath $lib -Filter '*.a' -ErrorAction SilentlyContinue
        if (-not $built) { throw "$addon addon produced no library in $lib" }
        Write-Ok ($built.Name -join ', ')
    }
}

# ---------------------------------------------------------------------------
# 3. Application
# ---------------------------------------------------------------------------
if ($Target -in @('all', 'app')) {
    Write-Step "Linking emulator kernel image ($Profile)"

    # Everything Circle provides must already be on disk.  `-Target app` skips
    # building it, so a first run with that target would otherwise fail deep in
    # the link with "no rule to make target .../libusb.a", which says nothing
    # about the actual cause.
    $requiredLibs = @(
        'lib\libcircle.a',
        'lib\usb\libusb.a',
        'lib\input\libinput.a',
        'lib\fs\libfs.a',
        'lib\sched\libsched.a',
        'lib\sound\libsound.a',
        'addon\fatfs\libfatfs.a',
        'addon\SDCard\libsdcard.a'
    )
    $missing = @()
    foreach ($relative in $requiredLibs) {
        $full = Join-Path $ExternalDir "circle\$relative"
        if (-not (Test-Path -LiteralPath $full)) { $missing += $relative }
    }
    if ($missing.Count -gt 0) {
        Write-Fail 'Circle has not been built for this profile'
        foreach ($m in $missing) { Write-Host "         missing: $m" -ForegroundColor Red }
        throw "Run: .\scripts\build.ps1 -Profile $Profile -Target circle   (or omit -Target to build everything)"
    }

    # And that what is there is for the right architecture.
    Test-CircleArchitecture

    # The image is written to build/<profile>/.  Makefile.app is run from that
    # directory because Circle's Rules.mk writes the image, map and disassembly
    # into the working directory, and they belong with the other products for
    # this architecture.  The makefile itself resolves its own sources, the
    # emulation core and Circle by absolute path, so the working directory only
    # decides where the outputs land.
    $outDir = Join-Path $root "build\$Profile"
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null

    # Object files from a different target are not merely stale, they are
    # unusable: they were compiled for another board's peripherals, so linking
    # them either fails with "file in wrong format" or produces an image that
    # faults on the Pi.  The profile directory is stamped with the target it was
    # built for and wiped when that changes, which is what makes switching
    # profiles safe without a manual clean.
    $stampPath = Join-Path $outDir '.target'
    $stamp = "$($prof.AARCH)/$($prof.RASPPI)/$($prof.Prefix)"
    $previous = if (Test-Path -LiteralPath $stampPath) {
        (Get-Content -LiteralPath $stampPath -Raw).Trim()
    } else { '' }

    if ($previous -ne $stamp) {
        if ($previous -ne '') {
            Write-Host "    target changed ($previous -> $stamp); rebuilding objects" -ForegroundColor Yellow
        }
        Remove-Item (Join-Path $outDir 'obj') -Recurse -Force -ErrorAction SilentlyContinue
        Get-ChildItem -LiteralPath $outDir -File |
            Where-Object { $_.Extension -in @('.o', '.elf', '.img', '.lst', '.map') } |
            Remove-Item -Force -ErrorAction SilentlyContinue
        Set-Content -LiteralPath $stampPath -Value $stamp -NoNewline
    }

    # KERNEL_MAX_SIZE is deliberately NOT passed here, and must not be.
    #
    # It is not a build setting that only affects the compiler: the Raspberry Pi
    # firmware reserves memory for the kernel and its stacks using the same
    # figure, and is never told about it.  Passing a larger value makes Circle
    # place its kernel stacks, coherent DMA region and heap above the boundary
    # the firmware still owns, and the kernel then boots partway and dies -- in
    # this project's case, on a USB assertion.
    #
    # Circle's default of 2 MiB is used, and the image is checked against it
    # below rather than the limit being raised.
    $cmd = "make -f $($root -replace '\\','/')/Makefile.app -j$Jobs " +
           "OUT=$($outDir -replace '\\','/') " +
           "CIRCLEHOME=$($root -replace '\\','/')/external/circle " +
           "CORE_LIB=$($root -replace '\\','/')/build/lib/libatari800.a " +
           "KERNEL_IMAGE_NAME=$($prof.Image) " +
           "AARCH=$($prof.AARCH) RASPPI=$($prof.RASPPI) PREFIX=$($prof.Prefix)"
    $res = Invoke-Msys -Profile $Profile -Command $cmd -WorkingDirectory $outDir
    if ($res.ExitCode -ne 0) {
        Write-Fail 'application link failed'
        $res.Output | Select-Object -Last 100 | ForEach-Object { Write-Host "    $_" }
        throw 'application link failed'
    }

    $img = Join-Path $outDir $prof.Image
    if (-not (Test-Path -LiteralPath $img)) { throw "expected image not produced: $img" }

    $size = (Get-Item -LiteralPath $img).Length
    $limit = Get-KernelMaxSizeBytes
    $note = if ($size -gt $limit) { '  ** OVER Circle''s 2 MiB kernel limit **' } else { '' }
    Write-Ok "$($prof.Image)  ($([math]::Round($size/1KB,1)) KiB)$note"

    # -----------------------------------------------------------------------
    # The file size is not the kernel size.
    #
    # .bss is not in the image, so an image of 800 KiB can still be a kernel of
    # 2.2 MB.  Circle reserves a fixed region for the kernel -- MEM_KERNEL_END is
    # MEM_KERNEL_START plus KERNEL_MAX_SIZE, 2 MiB -- and places its kernel
    # stacks and the heap immediately after it.  A kernel whose .bss runs past
    # that boundary is not merely too large: the heap is then allocated on top of
    # the kernel's own data, and the machine dies before it can write a single
    # line of log.  Measure where .bss ends, in the ELF.
    # -----------------------------------------------------------------------
    $elf = Join-Path $outDir 'atari-emulator.elf'
    $nmExe = Join-Path (Join-Path $ArmDev 'bin') 'aarch64-none-elf-nm.exe'
    if (Test-Path -LiteralPath $elf) {
        $endLine = & $nmExe $elf 2>$null |
            Select-String -Pattern '^([0-9a-fA-F]+)\s+B\s+_end\b' |
            Select-Object -First 1
        if ($endLine) {
            $end = [Convert]::ToInt64($endLine.Matches[0].Groups[1].Value, 16)
            $regionEnd = 0x80000 + (Get-KernelMaxSizeBytes)
            if ($end -gt $regionEnd) {
                Write-Fail ("the kernel's .bss ends at 0x{0:X}, past Circle's 2 MiB region (0x{1:X})" -f $end, $regionEnd)
                $heapMsg = "The heap and stacks start at 0x{0:X}; this kernel would corrupt itself before booting." -f $regionEnd
                Write-Host "         $heapMsg" -ForegroundColor Red
                Write-Host "         Move large buffers to the heap instead of declaring them in .bss." -ForegroundColor Red
                throw 'kernel does not fit in Circle''s kernel region'
            }
        }
    }

    # -----------------------------------------------------------------------
    # The Pi 4's ARM stub
    #
    # The Pi 4 firmware loads a small ARM stub before the kernel, and Circle
    # needs its own replacement for the way it sets up interrupts.  It is built
    # from assembly in Circle's tree -- there is nothing to download -- so it is
    # built here as part of the image the SD card needs, rather than being left
    # as something the user has to know about.
    #
    # Only this profile uses it: the Pi 5 has no armstub line at all, and the
    # earlier models use a different one.
    # -----------------------------------------------------------------------
    if ($prof.Contains('NeedsArmStub') -and $prof.NeedsArmStub) {
        Write-Step 'Building the Raspberry Pi 4 ARM stub'

        $armstubDir = Join-Path $ExternalDir 'circle\boot\armstub'
        $commands = @(
            "$($prof.Prefix)gcc -DGIC=1 -o armstub8-rpi4.o -c armstub8.S"
            "$($prof.Prefix)ld --section-start=.text=0 -o armstub8-rpi4.elf armstub8-rpi4.o"
            "$($prof.Prefix)objdump -D armstub8-rpi4.elf > armstub8-rpi4.lst"
            "$($prof.Prefix)objcopy armstub8-rpi4.elf -O binary armstub8-rpi4.bin"
        ) -join ' && '

        $res = Invoke-Msys -Profile $Profile -Command $commands -WorkingDirectory $armstubDir
        if ($res.ExitCode -ne 0) {
            Write-Fail 'the ARM stub could not be built'
            $res.Output | Select-Object -Last 40 | ForEach-Object { Write-Host "    $_" }
            throw 'the ARM stub could not be built'
        }

        $stub = Join-Path $armstubDir 'armstub8-rpi4.bin'
        if (-not (Test-Path -LiteralPath $stub)) {
            throw "expected armstub8-rpi4.bin was not produced in $armstubDir"
        }

        # Circle's own build system copies it up into boot/, which is where the
        # SD card instructions point the user.
        $stubOut = Join-Path $ExternalDir 'circle\boot\armstub8-rpi4.bin'
        Copy-Item -LiteralPath $stub -Destination $stubOut -Force
        Write-Ok "armstub8-rpi4.bin  ($((Get-Item $stubOut).Length) bytes)  -> external\circle\boot"
    }
}
