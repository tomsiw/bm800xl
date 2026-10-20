#Requires -Version 5.1
<#
.SYNOPSIS
    Fetches the upstream source trees this project builds against.

.DESCRIPTION
    Reads external/SOURCES.txt, downloads each revision, verifies its SHA-256,
    extracts it into external/, and applies any patch listed for it.

    GitHub is reached through PowerShell's own HTTP stack rather than through
    Git.  That is not a preference: Git for Windows uses schannel for TLS, and
    in some environments (restricted tokens, certain corporate TLS setups) its
    credential acquisition fails with "schannel: AcquireCredentialsHandle
    failed", while the same URL fetches perfectly over Invoke-WebRequest.
    Downloading a codeload archive also avoids needing Git at all.

.PARAMETER Force
    Re-download and re-extract even if the tree already looks present.  This
    DISCARDS local changes to the upstream trees, including the patches this
    script applies.

.EXAMPLE
    pwsh -ExecutionPolicy Bypass -File scripts\fetch-sources.ps1
#>
[CmdletBinding()]
param(
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'env.ps1')

$root     = Get-ProjectRoot
$extDir   = Get-ExternalDir
$sources  = Join-Path $extDir 'SOURCES.txt'

if (-not (Test-Path -LiteralPath $sources)) {
    throw "Cannot find $sources"
}

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'

# Git for Windows is used to APPLY patches, because MSYS2 does not ship the
# `patch` program.  `git apply` works on a plain unified diff and needs no
# repository.
$gitExe = 'C:\Program Files\Git\cmd\git.exe'
if (-not (Test-Path -LiteralPath $gitExe)) {
    $gitCmd = Get-Command git -ErrorAction SilentlyContinue
    $gitExe = if ($gitCmd) { $gitCmd.Source } else { $null }
}

function Get-ArchiveSubdirectory {
    param([Parameter(Mandatory)][string]$Tarball)

    # The tarball's root directory is the only top-level entry; reading it from
    # the archive avoids guessing how GitHub named it.
    $entries = & tar.exe -tzf $Tarball 2>$null
    if ($LASTEXITCODE -ne 0) { throw "Cannot read $Tarball" }
    $first = $entries | Select-Object -First 1
    return ($first -split '/')[0]
}

Write-Host ""
Write-Host "  Fetching upstream sources into $extDir" -ForegroundColor White

$lines = Get-Content -LiteralPath $sources | Where-Object {
    $_.Trim() -ne '' -and -not $_.TrimStart().StartsWith('#')
}

foreach ($line in $lines) {
    $parts = $line.Trim() -split '\s+'
    if ($parts.Count -lt 5) {
        Write-Warn "skipping malformed line: $line"
        continue
    }

    $name    = $parts[0]
    $url     = $parts[1]
    $subdir  = $parts[2]
    $sha     = $parts[3].ToLowerInvariant()
    $patch   = $parts[4]

    $target  = Join-Path $extDir $name
    $tarball = Join-Path $extDir "$name.tar.gz"

    Write-Step "$name"

    if ((Test-Path -LiteralPath $target) -and -not $Force) {
        Write-Ok "already present at $target (use -Force to re-fetch)"
        continue
    }

    # --- download ---------------------------------------------------------
    if (-not (Test-Path -LiteralPath $tarball) -or $Force) {
        Write-Host "    downloading $url" -ForegroundColor DarkGray
        try {
            Invoke-WebRequest -Uri $url -OutFile $tarball -UseBasicParsing -TimeoutSec 600
        }
        catch {
            throw "Download failed for $name : $($_.Exception.Message)"
        }
    }
    else {
        Write-Host "    using cached $tarball" -ForegroundColor DarkGray
    }

    # --- verify -----------------------------------------------------------
    $actual = (Get-FileHash -LiteralPath $tarball -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $sha) {
        Remove-Item -LiteralPath $tarball -Force
        throw @"
SHA-256 mismatch for $name.
  expected $sha
  actual   $actual
The download has been removed.  Either GitHub served different content than
when this project was pinned, or the transfer was corrupted.  If upstream has
legitimately moved, update external/SOURCES.txt deliberately and re-run.
"@
    }
    Write-Ok "sha256 verified"

    # --- extract ----------------------------------------------------------
    if (Test-Path -LiteralPath $target) {
        Remove-Item -LiteralPath $target -Recurse -Force
    }

    $temp = Join-Path ([System.IO.Path]::GetTempPath()) "atari-pi-fetch-$name"
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $temp | Out-Null

    & tar.exe -xzf $tarball -C $temp
    if ($LASTEXITCODE -ne 0) { throw "Extraction failed for $name" }

    $extracted = Join-Path $temp $subdir
    if (-not (Test-Path -LiteralPath $extracted)) {
        # Fall back to whichever single directory the archive produced.
        $dirs = Get-ChildItem -LiteralPath $temp -Directory
        if ($dirs.Count -ne 1) {
            throw "Cannot identify the extracted directory for $name in $temp"
        }
        $extracted = $dirs[0].FullName
    }

    Move-Item -LiteralPath $extracted -Destination $target
    Remove-Item -LiteralPath $temp -Recurse -Force
    Write-Ok "extracted to external/$name"

    # --- patch ------------------------------------------------------------
    if ($patch -ne '-' -and $patch -ne '') {
        $patchPath = Join-Path $root $patch
        if (-not (Test-Path -LiteralPath $patchPath)) {
            throw "Patch file not found: $patchPath"
        }
        if (-not $gitExe) {
            throw "Applying $patch needs Git for Windows (git apply), which was not found."
        }

        Write-Host "    applying $patch" -ForegroundColor DarkGray
        & $gitExe -C $target apply --verbose $patchPath 2>&1 | ForEach-Object {
            Write-Host "      $_" -ForegroundColor DarkGray
        }
        if ($LASTEXITCODE -ne 0) {
            throw "Applying $patch to external/$name failed"
        }
        Write-Ok "patch applied"
    }
}

Write-Host ""
Write-Host "  Sources ready." -ForegroundColor Green
Write-Host "  Next: pwsh -ExecutionPolicy Bypass -File scripts\build.ps1 -Profile kernel64" -ForegroundColor Gray
Write-Host ""
