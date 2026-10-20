# ============================================================================
#  atari-pi-baremetal : shared build environment
# ----------------------------------------------------------------------------
#  Dot-source this file from the other scripts.  It locates the two tool
#  installations on this machine and exports the paths used by every build
#  step:
#
#      C:\Tools\Gnu\ArmDev\arm-none-eabi\bin   aarch64-none-elf-* (AArch64)
#      C:\Tools\Gnu\msys64\usr\bin             bash / make / tar / sed ...
#
#  Only the AArch64 compiler is used: this project builds 64-bit images only.
#  The toolchain bundle also contains an arm-none-eabi (AArch32) compiler, which
#  is left alone.
#
#  Nothing here writes outside the project tree.
# ============================================================================

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# --- Project layout ---------------------------------------------------------
$script:ProjectRoot = Split-Path -Parent $PSScriptRoot
$script:ExternalDir = Join-Path $ProjectRoot 'external'
$script:BuildDir    = Join-Path $ProjectRoot 'build'
$script:SrcDir      = Join-Path $ProjectRoot 'src'

function Get-ProjectRoot { $script:ProjectRoot }
function Get-ExternalDir { $script:ExternalDir }
function Get-BuildDir { $script:BuildDir }

# --- Toolchain locations ----------------------------------------------------
$ToolRoot  = 'C:\Tools\Gnu'
$ArmDev    = Join-Path $ToolRoot 'ArmDev\arm-none-eabi'
$MsysRoot  = Join-Path $ToolRoot 'msys64'
$MsysBin   = Join-Path $MsysRoot 'usr\bin'

foreach ($p in @($ArmDev, $MsysBin)) {
    if (-not (Test-Path -LiteralPath $p)) {
        throw "Required tool directory not found: $p"
    }
}

# Toolchain bin directories (separate host/bin and target/bin trees exist in
# the Arm GNU Toolchain layout; collect every directory that holds the gcc
# launcher so the build works regardless of which layout is installed).
$script:ToolchainBins = @()
foreach ($candidate in @(
        (Join-Path $ArmDev 'bin'),
        (Join-Path $ArmDev 'arm-none-eabi\bin'))) {
    if ((Test-Path -LiteralPath $candidate) -and
        (Get-ChildItem -LiteralPath $candidate -Filter '*-gcc.exe' -ErrorAction SilentlyContinue)) {
        $script:ToolchainBins += $candidate
    }
}
if ($script:ToolchainBins.Count -eq 0) {
    throw "No ARM cross compiler (*-gcc.exe) found under $ArmDev"
}

# --- Toolchain prefix -------------------------------------------------------
#  AArch64 only.  See the profile table below for why.
$script:Prefix64 = 'aarch64-none-elf-'

# --- Target board profiles --------------------------------------------------
#  This project builds AArch64 only.  Circle selects both the compiler CPU and
#  the boot filename from RASPPI, so RASPPI is what decides which models an
#  image can run on.
#
#  RASPPI -> boot filename, for AARCH=64:
#      3 -> kernel8.img        Zero 2 W, Pi 3, Pi 4, Pi 400, CM3/CM4
#      4 -> kernel8-rpi4.img   Pi 4, Pi 400, CM4      (Cortex-A72 optimised)
#      5 -> kernel_2712.img    Pi 5                   (Cortex-A76, RP1)
#
#  Why a Pi 5 needs its own image, and cannot simply fall back to kernel8.img:
#  the Pi 5's peripherals live behind the RP1 southbridge, reached over PCIe,
#  and Circle compiles that support in only when RASPPI == 5.  An image built for
#  RASPPI == 3 -- which is what kernel8.img is -- contains no RP1 code and would
#  reach for the BCM2711 peripherals instead.  Circle's own documentation states
#  the Pi 5 image "has the name kernel_2712.img", and lists the files it needs.
#  So the Pi 5 is a genuinely separate build, not a tuning variant.
#
#  Why the Pi 5 does not need to be told which file to load: the firmware looks
#  for kernel_2712.img first on a Pi 5 and kernel8.img elsewhere, so both files
#  can sit on one card and each model picks the right one.  See
#  docs/sd-card-setup.md.
$script:Profiles = [ordered]@{
    'kernel64' = [ordered]@{
        AARCH     = 64
        RASPPI    = 3
        Prefix    = $script:Prefix64
        Image     = 'kernel8.img'
        CircleTarget = 'kernel8'
        Board     = 'Pi Zero 2 W, Pi 3, Pi 4, Pi 400, CM3/CM4 (AArch64, Cortex-A53)'
        Config    = 'config64.txt'
    }
    'kernel64-pi5' = [ordered]@{
        AARCH     = 64
        RASPPI    = 5
        Prefix    = $script:Prefix64
        Image     = 'kernel_2712.img'
        CircleTarget = 'kernel_2712'
        Board     = 'Raspberry Pi 5 (AArch64, Cortex-A76, RP1)'
        Config    = 'config64.txt'
    }
    'kernel64-pi4' = [ordered]@{
        AARCH     = 64
        RASPPI    = 4
        Prefix    = $script:Prefix64
        Image     = 'kernel8-rpi4.img'
        CircleTarget = 'kernel8-rpi4'
        Board     = 'Pi 4, Pi 400, CM4 (AArch64, Cortex-A72 optimised)'
        Config    = 'config64.txt'

        # The Pi 4's firmware loads an ARM stub before the kernel, and Circle
        # ships a replacement that it needs for its own interrupt setup.  It is
        # built from assembly in Circle's tree rather than downloaded, so this
        # profile builds it too.  See docs/sd-card-setup.md.
        NeedsArmStub = $true
    }
}

# Circle's kernel image size ceiling.
#
# This is Circle's own default from include/circle/sysconfig.h -- 2 MiB -- and it
# is deliberately NOT raised.  KERNEL_MAX_SIZE is not really a build setting: the
# Raspberry Pi firmware reserves space for the kernel and its stacks using this
# same figure, without ever being told about it.  Raising it in the build makes
# Circle place its kernel stacks, coherent DMA region and heap in memory the
# firmware still considers its own, and the result is a kernel that boots partway
# and then dies.
#
# This project's image is about 1.14 MB including .bss, so 2 MiB is ample.  The
# value is reported by the build script and checked by verify.ps1 so that an
# image approaching the limit is noticed, but it is not passed to the compiler.
# See external/circle/Config2.mk for the full explanation.
$script:KernelMaxSizeBytes = 2MB

function Get-Profiles { $script:Profiles }
function Get-KernelMaxSizeBytes { $script:KernelMaxSizeBytes }

# --- Environment for an MSYS2 / bash driven build ---------------------------
#  Circle's build system is POSIX oriented.  We drive it through the MSYS2
#  bash shell that is installed on this machine, which gives us a real
#  GNU make, a POSIX shell and the usual core utilities.
function Get-MsysEnv {
    param(
        [Parameter(Mandatory = $true)][string]$Profile
    )

    $prof = $script:Profiles[$Profile]
    if (-not $prof) { throw "Unknown profile: $Profile" }

    # arm-none-eabi and aarch64-none-elf are the ANSI/Unicode builds; MSYS2's
    # MSYSTEM must be MSYS (not MINGW64) so that PATH uses '/c/...' form.
    $pathParts = @($MsysBin) + $script:ToolchainBins + @(
        (Join-Path $MsysRoot 'usr\bin'),
        "$env:SystemRoot\System32",
        "$env:SystemRoot",
        $env:SystemRoot + '\System32\Wbem'
    )

    $env = @{
        'MSYSTEM'   = 'MSYS'
        'PATH'      = ($pathParts -join ';')
        'TERM'      = 'dumb'
        'CHERE_INVOKING' = '1'
    }

    # Convert a Windows path to the /c/... form MSYS2 understands.
    $env['PROJECT_ROOT_MSYS'] = ConvertTo-MsysPath $script:ProjectRoot
    $env['EXTERNAL_MSYS']     = ConvertTo-MsysPath $script:ExternalDir
    $env['BUILD_MSYS']        = ConvertTo-MsysPath $script:BuildDir
    $env['SRC_MSYS']          = ConvertTo-MsysPath $script:SrcDir

    return $env
}

function ConvertTo-MsysPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = [System.IO.Path]::GetFullPath($Path)
    $drive = $full.Substring(0, 1).ToLowerInvariant()
    $rest = $full.Substring(2) -replace '\\', '/'
    return "/$drive$rest"
}

function Get-BashExe {
    $bash = Join-Path $MsysBin 'bash.exe'
    if (-not (Test-Path -LiteralPath $bash)) { throw "bash.exe not found at $bash" }
    return $bash
}

# --- Run a build command inside MSYS2 ---------------------------------------
#  The environment is applied to the child process directly (not the caller),
#  so repeated invocations cannot leak into each other.
function Invoke-Msys {
    param(
        [Parameter(Mandatory = $true)][string]$Profile,
        [Parameter(Mandatory = $true)][string]$Command,
        [string]$WorkingDirectory,
        [switch]$Quiet
    )

    $envMap = Get-MsysEnv -Profile $Profile
    $bash = Get-BashExe

    # The working directory has to be set *inside* the shell, by prefixing the
    # command with cd.
    #
    # `bash -l` is a login shell, so it sources /etc/profile and that resets the
    # working directory to the user's home.  Passing a Windows working directory
    # to the process therefore has no effect on the build: make would run in
    # $HOME, find none of the relative paths it was given, decide there was
    # nothing to do, and report success while producing nothing at all.
    if (-not $WorkingDirectory) { $WorkingDirectory = $script:ProjectRoot }
    $quotedDir = (ConvertTo-MsysPath $WorkingDirectory).Replace("'", "'\''")
    $Command = "cd '$quotedDir' && $Command"

    $old = @{}
    foreach ($k in $envMap.Keys) {
        $old[$k] = [Environment]::GetEnvironmentVariable($k, 'Process')
        [Environment]::SetEnvironmentVariable($k, $envMap[$k], 'Process')
    }
    # Make sure an inherited ARM toolchain path cannot shadow ours.
    $savedPath = $env:PATH
    $env:PATH = $envMap['PATH']

    try {
        if (-not $Quiet) { Write-Host "  [msys] $Command" -ForegroundColor DarkGray }

        # Two things are deliberate here.
        #
        # First, stderr is redirected to stdout *inside* the shell, so that
        # PowerShell only ever sees ordinary stdout.  Compilers write warnings
        # there, and with $ErrorActionPreference set to 'Stop' -- which this
        # script sets -- a native command's stderr output can raise a
        # terminating error and abort a build that had in fact succeeded.
        #
        # Second, the whole invocation is wrapped in try/catch and the error
        # preference is relaxed for its duration.  A build must be judged by its
        # exit code, never by whether some tool wrote a diagnostic; a warning
        # about a function that will never be called is not a build failure.
        $previousPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $output = & $bash -lc "$Command 2>&1"
            $code = $LASTEXITCODE
        }
        catch {
            # Something in the call was reported as terminating.  Record it in
            # the output and let the exit code decide; a failure here means we
            # have no exit code at all, which is a genuine failure.
            $output = @($output) + @("*** $($_.Exception.Message)")
            $code = if ($null -ne $LASTEXITCODE) { $LASTEXITCODE } else { 1 }
        }
        finally {
            $ErrorActionPreference = $previousPreference
        }
    }
    finally {
        $env:PATH = $savedPath
        foreach ($k in $old.Keys) {
            [Environment]::SetEnvironmentVariable($k, $old[$k], 'Process')
        }
    }

    return [PSCustomObject]@{ ExitCode = $code; Output = $output }
}

# --- Small reporting helpers ------------------------------------------------
function Write-Step   { param([string]$m) Write-Host "`n==> $m" -ForegroundColor Cyan }
function Write-Ok     { param([string]$m) Write-Host "    OK   $m" -ForegroundColor Green }
function Write-Warn   { param([string]$m) Write-Host "    WARN $m" -ForegroundColor Yellow }
function Write-Fail   { param([string]$m) Write-Host "    FAIL $m" -ForegroundColor Red }

function Assert-LastExit {
    param([Parameter(Mandatory = $true)]$Result, [Parameter(Mandatory = $true)][string]$What)
    if ($Result.ExitCode -ne 0) {
        Write-Fail "$What (exit $($Result.ExitCode))"
        $Result.Output | Select-Object -Last 60 | ForEach-Object { Write-Host "    $_" }
        throw "$What failed"
    }
    Write-Ok $What
    return $Result
}
