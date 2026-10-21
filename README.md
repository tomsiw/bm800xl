# Atari XL/XE bare-metal emulator for Raspberry Pi

A complete Atari 8-bit home computer emulator that runs **directly on Raspberry Pi
hardware, without Linux**. You switch the Pi on and you are looking at an Atari.

It is built on two upstream projects:

| Component | Project | What it provides |
|---|---|---|
| Hardware access | [Circle](https://github.com/rsta2/circle) | Bare-metal drivers: framebuffer, audio, USB, SD card, GPIO, interrupts, timers |
| Emulation | [Atari800](https://github.com/atari800/atari800) | The emulated machine: 6502 CPU, ANTIC, GTIA, POKEY, PIA, SIO, memory, cartridges, cassettes |

The result is in the same spirit as [BMC64](https://github.com/randyrossi/bmc64)
(a bare-metal Commodore 64 for the Pi) and draws on
[Altirra](https://www.virtualdub.org/altirra.html) for emulation accuracy
priorities. **No code was taken from BMC64.** The Atari800 sources are used
unmodified apart from one documented one-line change (see
[Atari800 integration](docs/atari800-integration.md)).

---

## What it does

**Emulated machines**
- Atari XL (600XL / 800XL) and XE (65XE / 130XE), plus the XE Game System
- RAM configurations: 64 kB, 128 kB, 192 kB, 320 kB RAMBO XL, 320 kB Compy Shop

> There is deliberately **no 256 kB option**. The Atari800 core has no 256 kB
> RAM configuration — `MEMORY_SizeValid()` accepts 64, 128, 192,
> 320 RAMBO and 320 Compy Shop but not 256. A "256 kB RAMBO XL" upgrade in the
> real world expands in 16 kB banks, and the closest supported equivalent is
> the 320 kB RAMBO configuration, which is what this emulator offers instead.

**Display**
- PAL and NTSC, switchable while running
- Upscaled to 1280x720 by default, or 1920x1080
- Original Atari aspect ratio preserved: the picture fills the screen height and
  is padded left and right with black rather than stretched to 16:9
- PAL colour blending and NTSC artifacting, selectable

**Sound**
- Full POKEY emulation, mono or stereo (two POKEYs)
- Volume control from the OSD menu

**Storage**
- Up to four disk drives, ATR and XFD images, **read and write**
- Cartridges in CAR format
- Cassettes in CAS format, **read and write**
- Disk and cartridge images live on a **USB drive**; settings, snapshots and the
  OS ROM images live on the **SD card**

**Input**
- *Mode 1:* a USB keyboard, with the Atari console keys and cursor keys mapped on
- *Mode 2:* a real Atari keyboard wired to the GPIO header as a scan matrix

**Other**
- OSD menu (F12, or Shift+Help on an Atari keyboard)
- Machine snapshots: save and load
- All settings persisted on the SD card automatically

---

## Quick start

Everything is built for 64-bit (AArch64). **Two images cover every supported
model**, and both can sit on one SD card because the firmware picks between them
by name:

```powershell
# kernel8.img - the one to start with: Pi Zero 2 W, Pi 3, Pi 4, Pi 400
pwsh -ExecutionPolicy Bypass -File scripts\build.ps1 -Profile kernel64

# kernel_2712.img - the Raspberry Pi 5
pwsh -ExecutionPolicy Bypass -File scripts\build.ps1 -Profile kernel64-pi5

# confirm the images are sound and named for the right boards
pwsh -ExecutionPolicy Bypass -File scripts\verify.ps1
```

Both build to about 800 KiB, comfortably inside Circle's 4 MiB kernel limit. Then
follow [Building](docs/building.md), or jump straight to
[docs/sd-card-setup.md](docs/sd-card-setup.md) if you already have the images.

---

## Supported models

`RASPPI` selects the compiler target **and** the boot filename, so it is what
decides which models an image can run on.

| Build profile | Image file | Runs on |
|---|---|---|
| `kernel64` | `kernel8.img` | Pi Zero 2 W, Pi 3, Pi 4, Pi 400, CM3/CM4 |
| `kernel64-pi5` | `kernel_2712.img` | Raspberry Pi 5 |
| `kernel64-pi4` | `kernel8-rpi4.img` | Pi 4, Pi 400, CM4 — Cortex-A72-optimised |

**Recommended:** build `kernel64` and `kernel64-pi5`. Copy both files to the SD
card and each board loads the right one; you do not have to decide in advance
which Pi the card will go into.

`kernel64-pi4` is optional. It is the same code compiled for the Pi 4's
Cortex-A72 instead of the more general Cortex-A53, so it will not run on a Zero 2
W or Pi 3 — but it is the fastest option if the card is only ever going to be in
a Pi 4.

### Why a Pi 5 needs its own image

This is not a tuning difference, and it is worth understanding because the
symptom of getting it wrong is a blank screen.

The Pi 5's peripherals — GPIO, USB, the SD interface — do not hang off the SoC
the way they do on earlier models. They live behind the **RP1 southbridge**,
which is reached over PCIe. Circle compiles that support in only when
`RASPPI=5`; there are around 290 `RASPPI` conditionals in the library, covering
things as fundamental as which interrupt controller and which GPIO registers to
use.

So a `kernel8.img` built for `RASPPI=3` contains no RP1 code at all, and on a Pi 5
it would address the BCM2711 peripherals that are not there. Circle's own
documentation is explicit that the Pi 5 image *"has the name kernel_2712.img"* and
lists the extra files it needs. Hence the separate build.

### A single kernel, as far as the firmware is concerned

The Pi 5 firmware looks for `kernel_2712.img` first and `kernel8.img` on every
other model. Put both files on the card and nothing needs configuring: the right
one is loaded without you choosing. This is why the two images coexist rather
than replacing one another.

There is no 32-bit build. Every supported model has a 64-bit core, and the Pi 5
can only run AArch64, so an AArch32 image would add a second toolchain and a
second set of build products to cover no model that the 64-bit one does not.

---

## Documentation

| Document | Contents |
|---|---|
| [Building](docs/building.md) | Toolchain, commands, what each build step produces, how to verify an image |
| [SD card setup](docs/sd-card-setup.md) | Exactly which files go where, and the `config.txt` needed |
| [Using the emulator](docs/using.md) | OSD menu, settings file, snapshots, disk and cassette handling |
| [Keyboard: USB](docs/keyboard-usb.md) | Complete USB key to Atari key mapping |
| [Keyboard: GPIO](docs/keyboard-gpio.md) | Wiring a real Atari keyboard: pin table and matrix |
| [Joysticks](docs/joystick.md) | USB gamepads, and 9-pin Atari joysticks on GPIO |
| [Architecture](docs/architecture.md) | How the pieces fit together, for anyone modifying it |
| [Atari800 integration](docs/atari800-integration.md) | How the core is configured and the one change made to it |
| [Troubleshooting](docs/troubleshooting.md) | What to do when there is no picture, no sound, or no disk |

Two longer reference documents are also kept here. They are working notes rather
than manuals, but they record the API details the port depends on, each cited to
the exact upstream file and line:

| Reference | Contents |
|---|---|
| [Circle API guide](docs/circle-implementation-guide.md) | Verified behaviour of this Circle revision: models, boot filenames, framebuffer, sound, USB, FatFs, GPIO, timing — including what could *not* be established from the sources |
| [Atari800 core analysis](docs/atari800-libatari800-baremetal-integration.md) | The emulation core's integration surface: platform contract, source list, runtime configuration, screen and audio formats, save states |

---

## Repository layout

```
Makefile.core        Builds the Atari800 core + this project's platform layer
                     into build/lib/libatari800.a
Makefile.app         Links the kernel image, driven by Circle's own Rules.mk
scripts/
  env.ps1            Toolchain paths and profiles (edit here if your tools differ)
  build.ps1          Builds the core, Circle and the image; cleans on target change
  build-armstub.ps1  Builds Circle's Pi 4 ARM stub (armstub8-rpi4.bin)
  fetch-firmware.ps1 Downloads the Raspberry Pi boot firmware for the SD card
  verify.ps1         Checks each image's AArch64 type, entry point, name and size
  fetch-sources.ps1  Fetches the pinned upstream revisions and applies the patch
patches/
  atari800-statesav-size.patch
                     The one change made to the Atari800 sources, with the reason
src/
  kernel.cpp/.h      Main application: bring-up order and the frame loop
  main.cpp           Circle entry point
  core/config.h      The Atari800 core's build configuration (replaces autoconf)
  port/              The platform layer: everything the core calls out to
    atari_port.h       The interface the rest of the kernel uses
    platform.c         Platform hooks, machine configuration, snapshots
    sound_bridge.c     POKEY output to the audio device
    vfs.c/.h           The file system the core sees (over FatFs)
    ui_stub.c          Headless stand-in for the core's own menu
  video/             Framebuffer, scaling, and the drawing primitives
  audio/             The Circle sound device
  input/             USB and GPIO keyboards, USB gamepads, GPIO joysticks
  storage/           SD card and USB file access
  config/            Settings persistence
  osd/               The on-screen menu
external/
  circle/            Circle, unmodified (revision recorded in SOURCES.txt)
  atari800/          Atari800, one documented one-line change
  SOURCES.txt        Pinned upstream revisions and their SHA-256 checksums
ROMS/                The OS ROM images to copy onto the SD card
```

---

## What has and has not been tested

This matters, so it is stated plainly rather than buried.

**Verified in this repository (build-time):**

- All kernel images build cleanly and link as AArch64
  (`aarch64-none-elf`), about 830 KiB each.
- `scripts/verify.ps1` confirms each image is ELF64/AArch64, has its entry point
  at 0x80000, contains `_start`, carries the filename the firmware looks for on
  that board, and is under Circle's kernel size limit.
- Every module compiles warning-free, including with Circle's own flags
  (`-nostdinc++`, `-fno-exceptions`, `-fno-rtti`, `-std=c++17`).
- The `audio_sink_t` contract, the keyboard interface and the VTAB/VFS redirection
  were each checked against the real upstream headers.

**Verified on hardware (Raspberry Pi 400, 720p, HDMI):**

- It boots to BASIC, and the emulated machine runs.
- Video output at the correct aspect ratio with side bars.
- USB keyboard; the on-screen menu opens, navigates and closes.
- Mounting disk images, cartridges and cassettes from USB storage, and running
  them; ejecting and re-mounting; visiting the menu with an image loaded and
  cold-resetting afterwards.
- All settings are applied at startup and persist across a power cycle —
  machine type, RAM size (64/128/192/320 kB), video standard, and the rest.
- The crash marker ejects all media and restores machine defaults on the next
  boot, so a bad image cannot trap the machine in a crash loop.
- **Sound over HDMI**, at the correct pitch when the frame rate keeps up.

**NOT yet verified:**

- Pi Zero 2 W, Pi 3, Pi 4 and Pi 5: only the Pi 400 has been run.
- The GPIO keyboard matrix, the 9-pin joystick ports, and USB gamepads.
- Writing back to disk and cassette images; hot-plugging USB storage.
- Frame rate: about 30 fps against a 50 fps (PAL) target on the Pi 400. The
  emulation itself costs ~3 ms per frame, so the remainder is this project's
  rendering and presentation path. At 30 fps the emulator produces only ~60% of
  the audio samples the sound device consumes, so music plays with gaps and
  sounds slowed — a symptom of the frame rate, not of the audio path.
- Cassette loading beyond insertion.

### HDMI audio requires the HDMI0 port

The firmware only enables HDMI audio packets on **HDMI0**. Video works on both
ports, so a machine can look perfectly normal while the sound device refuses to
start with `Requires HDMI display with audio support` — a statement about the
port, not about the display.

On the Pi 400 used for testing, HDMI0 turned out to be the port **second from
the power connector**, not the nearest one. Circle's own `sample/34-sounddevices`
is the quickest way to identify which port is which on a given board: it plays a
tone and prints its progress, with no emulator involved.

`hdmi_force_edid_audio=1` is set in `config.txt` for displays whose EDID does not
advertise audio clearly. It is harmless on a display without speakers.


---

## Licence

The emulation core is Atari800, which is **GPL v2 or later**. Circle is
**GPL v3 or later**. This project links both, so it is distributed under the
**GNU General Public License version 3 or later**; see `COPYING`.

Atari, Atari 800, Atari XL and Atari XE are trademarks of Atari Interactive,
Inc. This project is not affiliated with or endorsed by Atari. The OS ROM images
in `ROMS/` are the property of their respective rights holders and are included
only because the operator of this repository supplied them.
