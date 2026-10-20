# Building

Everything is cross-compiled **on Windows**. Nothing needs Linux or WSL.

## What you need

| Tool | Where this project expects it | Notes |
|---|---|---|
| ARM GNU Toolchain (AArch64) | `C:\Tools\Gnu\ArmDev\arm-none-eabi\bin` | `aarch64-none-elf-gcc` |
| MSYS2 | `C:\Tools\Gnu\msys64` | provides `bash`, GNU make and the core utilities |
| Git for Windows | `C:\Program Files\Git` | only used by `scripts/fetch-sources.ps1` |

Only the **AArch64** compiler is used. The same toolchain bundle also contains an
`arm-none-eabi` (AArch32) compiler, which this project does not use: every
supported model has a 64-bit core, and the Pi 5 can only run AArch64, so a 32-bit
build would mean a second toolchain and a second set of products for no model the
64-bit image does not already cover.

The one piece of 32-bit-looking work is Circle's **ARM stub** for the Pi 4, which
is assembled from `armstub8.S` — with the AArch64 prefix, despite the name.
`scripts/build-armstub.ps1` builds it and the `kernel64-pi4` profile builds it
automatically. See
[SD card setup](sd-card-setup.md#6-the-pi-4-arm-stub-only-if-you-built-kernel64-pi4).

The paths are collected in one place — `scripts/env.ps1` — so if your tools live
elsewhere, that is the only file to change.

The toolchain version used while developing this was
**Arm GNU Toolchain 14.3.Rel1**. Circle tracks a newer 15.2.Rel1; 14.3 works and
is what the build has been verified with.

## Get the upstream sources

The two upstream projects are vendored under `external/`. If you cloned this
repository they are already there.

```
external/circle/     Circle bare-metal library
external/atari800/   Atari800 emulator
```

If you need to re-fetch them — for example after a fresh checkout without the
vendored trees, or to confirm you have the pinned revisions — run:

```powershell
pwsh -ExecutionPolicy Bypass -File scripts\fetch-sources.ps1
```

That script downloads the exact revisions recorded in
`external/SOURCES.txt`, verifies each archive's SHA-256, extracts it, and applies
the one patch this project needs (see
[Atari800 integration](atari800-integration.md#the-one-change-to-upstream)). The
SHA-256 check is the point of the file: a mismatch means GitHub served something
other than what was pinned, and the fetch stops rather than building an unknown
tree.

It downloads through PowerShell's own HTTP stack rather than through Git. That is
not a preference: Git for Windows uses schannel for TLS, and in some environments
its credential acquisition fails with `schannel: AcquireCredentialsHandle
failed`, while the same URL fetches perfectly over `Invoke-WebRequest`. Fetching
a codeload archive also avoids needing Git at all — except to apply the patch,
for which the script uses `git apply`.

## Prepare the SD card

Building produces the kernel image. The card also needs the Raspberry Pi's own
boot firmware, which is downloaded rather than built:

```powershell
pwsh -ExecutionPolicy Bypass -File scripts\fetch-firmware.ps1
```

See [SD card setup](sd-card-setup.md) for the full file list, where each file
goes, and which model needs which. The short version:

```
kernel8.img  kernel_2712.img        the emulator (from build/)
start.elf  fixup.dat                firmware, Pi Zero 2 W and Pi 3
start4.elf  fixup4.dat              firmware, Pi 4 and Pi 400
bootcode.bin                        first-stage loader, Pi Zero 2 W and Pi 3
bcm27*.dtb  overlays/               device tree for your model
config.txt                          copied from external/circle/boot/config64.txt
```

## Build

### The short version

```powershell
# kernel8.img - Pi Zero 2 W, Pi 3, Pi 4, Pi 400
pwsh -ExecutionPolicy Bypass -File scripts\build.ps1 -Profile kernel64

# kernel_2712.img - Raspberry Pi 5
pwsh -ExecutionPolicy Bypass -File scripts\build.ps1 -Profile kernel64-pi5
```

The finished images appear in `build/<profile>/`:

```
build/kernel64/kernel8.img              <- copy to the SD card
build/kernel64/atari-emulator.elf       <- for debugging
build/kernel64/atari-emulator.map       <- linker map
build/kernel64/atari-emulator.lst       <- disassembly
```

The ELF, map and disassembly are named after this project's internal target
rather than after the boot image, because Circle's own link rules write them
under the makefile's target name.

### `build.ps1` options

| Option | Meaning |
|---|---|
| `-Profile` | `kernel64` (default), `kernel64-pi5`, `kernel64-pi4` |
| `-Target` | `all` (default), `core`, `circle`, `app`, `clean` |
| `-Jobs N` | Parallel make jobs; defaults to your CPU count |

`-Target` is useful while working on one part: `-Target core` rebuilds only the
emulation core, `-Target app` only relinks the image.

### What the build does, in order

1. **The emulation core** — compiles the Atari800 sources listed in
   `Makefile.core`, using this project's `src/core/config.h` instead of
   autoconf's, together with the platform layer in `src/port/`. The result is
   `build/lib/libatari800.a`.
2. **Circle** — runs Circle's own `makeall` to build its libraries, then builds
   the `addon/fatfs` and `addon/SDCard` addons, which `makeall` skips.
3. **The image** — compiles this project's modules and links everything with
   Circle's `Rules.mk`, producing the kernel image.

### Switching profiles

> **Circle is rebuilt when the profile changes.** Its object files are not
> separated by `RASPPI`, and around 290 places in the library compile differently
> depending on it — which interrupt controller, which GPIO registers, whether RP1
> support exists at all. Mixing two values would link objects for the wrong board,
> so the build script stamps the tree with the target it was built for and runs
> Circle's `clean` when that changes. Expect a few minutes the first time.

The per-profile object directory is stamped the same way, so switching back and
forth is safe rather than a source of confusing link errors.

## Verify the build

A healthy build prints, at the end:

```
    OK   kernel8.img  (801.9 KiB)
```

Then confirm the images really are what they claim to be:

```powershell
pwsh -ExecutionPolicy Bypass -File scripts\verify.ps1
```

```
  OK   kernel64        kernel8.img           801.9 KiB  Pi Zero 2 W, Pi 3, Pi 4, Pi 400, CM3/CM4
  OK   kernel64-pi5    kernel_2712.img         832 KiB  Raspberry Pi 5 (AArch64, Cortex-A76, RP1)
  OK   kernel64-pi4    kernel8-rpi4.img      820.4 KiB  Pi 4, Pi 400, CM4 (AArch64, Cortex-A72 optimised)

  All built images verified.
```

It reads each image's ELF header and checks that it is ELF64/AArch64, that the
entry point is 0x80000 (where Circle's linker script places an AArch64 kernel),
that `_start` exists, that the image carries the filename the firmware looks for
on that board, and that it is under the 4 MiB kernel limit. A build that produced
files is not by itself proof of much; this is.

The image must stay under the **4 MiB kernel limit**, which
`scripts/build.ps1` passes to Circle as `KERNEL_MAX_SIZE=4 * MEGABYTE`. Neither
makefile carries that value itself, so a build driven by hand rather than by the
script would use Circle's 2 MiB default. This matters more than it looks: if the
image exceeds the limit then `sysinit()` **halts silently with no console output
at all** — the screen simply stays dark, with nothing to tell you why. Both the
build script and `verify.ps1` warn if the image crosses the limit.

### Keeping more than one image

Building one profile does not invalidate another's *image*, so you can build all
three and keep them. What it does invalidate is the shared state:

- **Circle's object files** are shared by every `RASPPI` value and Circle has no
  notion of switching between them. `build.ps1` stamps the tree with the target it
  was last built for and runs Circle's own `clean` when that changes. Without this
  you would link objects for the wrong board — which either fails with "file
  format not recognized" or, worse, succeeds and then faults on the Pi.
- **The Atari800 core archive** is `build/lib/libatari800.a`. It is reused across
  profiles because the core compiles identically for all three: the differences
  are in Circle's drivers, not in the emulation. The core's object directory
  `build/coreobj` is stamped the same way, so a stale core cannot slip through.
- **This project's objects** live under `build/<profile>/obj`, stamped with
  `.target`, and are wiped when the target changes.

So switching profiles is safe, at the cost of rebuilding Circle each time. If you
just want a card that works in every model, build `kernel64` and `kernel64-pi5`
once and copy both files; you never have to switch again.

## Pinned revisions

Reproducibility matters for a project that patches nothing and depends on two
moving upstreams, so the exact revisions are recorded in
`external/SOURCES.txt`. Each line is:

```
<name> <archive-url> <archive-subdirectory> <sha256-of-archive> <patch-or-->
```

so, in full:

```
circle https://codeload.github.com/rsta2/circle/tar.gz/refs/heads/master \
       circle-master 569e5bf8...8723 -
atari800 https://codeload.github.com/atari800/atari800/tar.gz/refs/tags/ATARI800_7_2_1 \
       atari800-ATARI800_7_2_1 c85cd024...a832a0a \
       patches/atari800-statesav-size.patch
```

The **SHA-256 is the point of the file**. It is checked after the download: a
mismatch means the server produced something other than the revision that was
pinned, and the fetch stops rather than building an unknown tree. GitHub does not
guarantee that a tarball for a given revision is byte-identical forever, so if
this check ever fails, the archive should be inspected rather than the hash
simply replaced.

Circle is pinned to a commit on `master` rather than to a tag, because the
framebuffer and RP1 behaviour this project relies on is newer than the latest
release. The revision is therefore part of this project's contract: changing it
means re-checking the assumptions recorded in
[circle-implementation-guide.md](circle-implementation-guide.md).

`scripts/fetch-sources.ps1` reads that file and fetches those exact revisions.

## Troubleshooting the build

**`bash.exe: *** fatal error - NtCreateDirectoryObject(...): 0xC0000022`**
MSYS2 could not create its shared objects. This happens when the build runs
under a restricted Windows job or token. Run the build from a normal PowerShell
window.

**`fatal: unable to access ... schannel: AcquireCredentialsHandle failed`**
Git could not reach the network. `scripts/fetch-sources.ps1` uses PowerShell's
own HTTP stack rather than Git's, precisely so that this does not happen; if you
see it, you ran `git clone` by hand.

**`/usr/bin/bash: make: command not found`**
`scripts/env.ps1` builds a `PATH` for the MSYS2 shell and the script sets it per
invocation. If you are running `make` by hand, use `scripts/build.ps1` instead,
or set `MSYSTEM=MSYS` and put `C:\Tools\Gnu\msys64\usr\bin` first on `PATH`.

**The image builds but the Pi shows nothing.** That is almost always the
`config.txt` and firmware files rather than the build. See
[Troubleshooting](troubleshooting.md).
