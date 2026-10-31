# Atari XL/XE bare-metal emulator for Raspberry Pi

A complete Atari 8-bit home computer emulator that runs **directly on Raspberry Pi
hardware, without Linux**. You switch the Pi on and you are looking at an Atari.

It is built on two upstream projects:

| Component | Project | What it provides |
|---|---|---|
| Hardware access | [Circle](https://github.com/rsta2/circle) | Bare-metal drivers: framebuffer, audio, USB, SD card, GPIO, interrupts, timers |
| Emulation | [Atari800](https://github.com/atari800/atari800) | The emulated machine: 6502 CPU, ANTIC, GTIA, POKEY, PIA, SIO, memory, cartridges, cassettes |

The result is in the same spirit as [BMC64](https://github.com/randyrossi/bmc64)
(a bare-metal Commodore 64 for the Pi). The Atari800 sources and Circle library are used mostly 
unmodified.

---

## What it does

**Emulated machines**
- Atari XL (600XL / 800XL) and XE (65XE / 130XE)
- RAM configurations: 64 kB, 128 kB, 192 kB, 320 kB RAMBO XL, 320 kB Compy Shop

**Display**
- PAL and NTSC, switchable while running
- Upscaled to 1280x720 by default
- Original Atari aspect ratio preserved: the picture fills the screen height and
  is padded left and right with black rather than stretched to 16:9 — or, at the
  press of a menu key, scaled by an exact 2x or 3x for square, pixel-perfect
  output in a border
- PAL colour blending and NTSC artifacting, selectable

**Sound**
- Full POKEY emulation, mono or stereo (two POKEYs)
- Volume control from the OSD menu

**Storage**
- Up to four disk drives, ATR and XFD images, **read and write**
- Cartridges in CAR format
- Cassettes in CAS format, **read and write**, in both standard and turbo timing —
  a turbo tape is detected when it is inserted and needs a power cycle to switch
  the machine into the mode it requires
- Disk and cartridge images live on a **USB drive** or on the **SD card**
  (`SD:/USERDATA`); settings, snapshots and the OS ROM images live on the
  **SD card**

**Input**
- *Mode 1:* a USB keyboard, with the Atari console keys and cursor keys mapped on
- *Mode 2:* a real Atari keyboard wired to the GPIO header as a scan matrix (never tested)
- Two joystick ports, fed by **two USB gamepads**, by **two real 9-pin joysticks**
  on the GPIO header, or by **keys on the USB keyboard** bound in `usbkeys.cfg` —
  one interface at a time, chosen in the menu, and the two ports can be swapped
  from there without a restart

**Other**
- OSD menu (F12, or Shift+Help on an Atari keyboard), drawn in the Atari
  character set, with a power cycle for the cases a reset cannot fix
- `STATIC_OSD=1` by default (the right setting for a slow board): the menu is drawn over the frame already on
  screen instead of repainting the picture under it
- Adaptive fill: if a frame cannot be drawn within its deadline, the picture is
  skipped rather than the emulation, and the skip is reported in the log
- Machine snapshots: save and load
- All settings persisted on the SD card automatically

---

## Getting started: prepare an SD card

Nothing has to be built to run the emulator. The
[releases page](https://github.com/tomsiw/bm800xl/releases) carries two files:

| Asset | What it is |
|---|---|
| `BM800XL-sd.img.zip` | the card image, ready to write: partition table, FAT32 filesystem and all the files in one |
| `BM800XL-sd.zip` | the same files loose, for copying onto a card you formatted yourself (`BM800XL.zip` in older releases) |

### 1. Write the image (the short way)

Unpack `BM800XL-sd.img.zip` to get `BM800XL-sd.img`, a 128 MB image, and write it
to the card:

- **Raspberry Pi Imager**: *Choose OS → Use custom* → pick the `.img`. It also
  takes the `.zip` directly and unpacks it itself.
- **Rufus**: pick the file and answer **yes** to the "DD mode" prompt.
- **balenaEtcher**: either file works.

**There is nothing to format first**, and no FAT32 caveat in this path: the image
brings its own partition table and FAT32 filesystem, and writing it replaces
whatever was on the card. Any card of 128 MB or more will do, and everything below
that size is left unallocated — which is fine, because the emulator keeps its own
media in `USERDATA/` and needs nothing else. If you want that space back for disk
images later, extend the partition with a partition tool afterwards.

### 2. Or copy the files

This is the path to take if you would rather see what lands on the card, or if you
want a particular partition layout of your own. It is the only one that needs the
card prepared first.

**The card must be FAT32.** The Raspberry Pi firmware reads the files from a FAT
partition, so exFAT and NTFS cards will not boot — the screen simply stays dark.
Use one FAT32 partition, the first on the card, marked bootable.

Windows' own formatter refuses FAT32 on anything larger than 32 GB. For a bigger
card, create the partition with `diskpart` and then run `format /FS:FAT32`, or use
a tool such as Rufus, which will force it.

Unpack `BM800XL-sd.zip`. It contains a directory `SD-card-content/`, and it is
**the contents of that directory** that belong in the root of the card — not the
directory itself:

```
kernel8.img        kernel_2712.img    kernel8-rpi4.img
config.txt         cmdline.txt
bootcode.bin       start.elf   start4.elf   fixup.dat   fixup4.dat
bcm*.dtb           armstub8-rpi4.bin
ATARI/             ROMS/        USERDATA/
```

Three kernel images ship, and they are alternatives rather than additions: two of
them cover every supported model and the third is the Pi 4-only optimisation, so
copying them all means the card works in any board without a decision. The
firmware loads the one its model needs and ignores the others, and `config.txt` is
already set up for that — it needs no editing.

### 3. Supply the ROMs

The Atari OS, BASIC and 5200 ROM images are **copyrighted and not included**;
`ROMS/` ships empty on purpose, with a note listing what it wants. Add these to it
— in the image's case after writing the card, which Windows will mount as an
ordinary drive. Copy them in:

| File | Size | Needed for |
|---|---|---|
| `ATARIXL.ROM` | 16384 | XL/XE OS — **the one that matters**: without it there is no real machine |
| `ATARIOSA.ROM` | 10240 | Atari 800 OS rev. A |
| `ATARIOSB.ROM` | 10240 | Atari 800 OS rev. B |
| `ATARIBAS.ROM` | 8192 | Atari BASIC |
| `atari5200.rom` | 2048 | Atari 5200 BIOS |

The emulator matches each file by length and CRC32 against the core's table of
known images, and reports a file it cannot identify in the log. With no usable OS
ROM at all it falls back to **AltirraOS**, a free replacement compiled into the
kernel: it starts and runs, but it is not a genuine Atari OS.

Disk, cartridge and cassette images are *not* ROMs; they belong in `USERDATA/`
(see below) and are the user's own collection.

### 4. Two lines of `settings.cfg`

The settings file is `SD:/ATARI/settings.cfg`. It is plain text, one `KEY=value`
per line, safe to edit by hand, and the emulator rewrites it whenever a setting is
changed in the OSD menu — so edit it with the machine powered off, then power
cycle, and do not be surprised if the file changes back after a menu edit.

Two entries are worth setting before the first run. The shipped file already has
both of them at the values below:

| Line | Values | What it does |
|---|---|---|
| `STATIC_OSD` | `1` (static) or `0` (overlay) | With `1` the menu is drawn on an opaque panel over the frame already on screen and the picture behind it stops updating. With `0` the menu sits on a dimmed, live picture, which costs noticeably more compute power — worth `0` on a Pi 4, usually `1` on a Zero 2 W. |
| `USER_DIR` | `SD:` or `USB:` | `SD:` keeps disk, cartridge and cassette images on the card and offers **only** `SD:/USERDATA` to the file browser. `USB:` offers the whole USB drive. Media are never read from the rest of the card, because that part belongs to the emulator. |

Everything else — machine type, RAM size, video standard, sound, volume, joysticks
— is in the OSD, which opens with **F12**. The full list is in
[the settings file](docs/using.md#the-settings-file).

### 5. Power on

Insert the card, plug in a USB keyboard, and switch the Pi on. Two logs on the
card, `ATARI/bootlog.txt` and `ATARI/runtimelog.txt`, say what happened if the
screen stays dark; see [Troubleshooting](docs/troubleshooting.md).

How to use the machine once it is running — the OSD menu and its keys, loading
disks, cartridges and tapes, the standard and turbo cassette modes, snapshots,
and every setting in the settings file — is in
[Using the emulator](docs/using.md).

For a card built by hand, file by file, including which firmware files your model
needs, see [SD card setup](docs/sd-card-setup.md).

---

## Quick start

If you would rather build the emulator yourself, everything is compiled for 64-bit
(AArch64). **Two images cover every supported model**, and both can sit on one SD
card because the firmware picks between them by name:

```powershell
# kernel8.img - the one to start with: Pi Zero 2 W, Pi 3, Pi 4, Pi 400
pwsh -ExecutionPolicy Bypass -File scripts\build.ps1 -Profile kernel64

# kernel_2712.img - the Raspberry Pi 5
pwsh -ExecutionPolicy Bypass -File scripts\build.ps1 -Profile kernel64-pi5

# confirm the images are sound and named for the right boards
pwsh -ExecutionPolicy Bypass -File scripts\verify.ps1
```

Both build to about 840 KiB, comfortably inside Circle's **2 MiB** kernel limit.
Then follow [Building](docs/building.md), or jump straight to
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
| [Progress](docs/progress.md) | The working ledger: what was changed and why, measured results, known limitations, what has been tested |

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
Makefile.probe       The same for the USB/gamepad probe kernels in probe/
scripts/
  env.ps1            Toolchain paths and profiles (edit here if your tools differ)
  build.ps1          Builds the core, Circle and the image; cleans on target change
  build.bat          One-line wrapper that runs build.ps1 from cmd.exe
  build-armstub.ps1  Builds Circle's Pi 4 ARM stub (armstub8-rpi4.bin)
  build-probe.ps1    Builds the bring-up probes in probe/
  build-diagnostic.ps1
                     Builds a kernel that reports stage colours on screen
  bootlog_test.c     Stand-alone test of the boot log's ring buffer
  fetch-firmware.ps1 Downloads the Raspberry Pi boot firmware for the SD card
  verify.ps1         Checks each image's AArch64 type, entry point, name and size
  fetch-sources.ps1  Fetches the pinned upstream revisions and applies the patch
patches/
  atari800-statesav-size.patch
                     The one change made to the Atari800 sources, with the reason
probe/               Bring-up kernels: USB keyboard, gamepad, display, timing
docs/                The manual and the reference notes listed above
src/
  kernel.cpp/.h      Main application: bring-up order and the frame loop
  main.cpp           Circle entry point
  main_probe.cpp     Same, for the probe kernels
  bootlog.cpp/.h     Boot log, stage colours and the runtime log file
  core/config.h      The Atari800 core's build configuration (replaces autoconf)
  port/              The platform layer: everything the core calls out to
    atari_port.h       The interface the rest of the kernel uses
    platform.c         Platform hooks, machine configuration, snapshots
    sound_bridge.c     POKEY output to the audio device
    audio_out.h        The audio device interface the port uses
    host_services.h    What the port needs from the kernel
    vfs.c/.h           The file system the core sees (over FatFs)
    ui_stub.c          Headless stand-in for the core's own menu
  video/             Framebuffer, scaling, and the drawing primitives
  audio/             The Circle sound device
  input/             USB and GPIO keyboards, USB gamepads, GPIO joysticks
  storage/           SD card and USB file access
  config/            Settings persistence
  osd/               The on-screen menu and its font
external/
  circle/            Circle, unmodified (revision recorded in SOURCES.txt)
  atari800/          Atari800, one documented one-line change
  SOURCES.txt        Pinned upstream revisions and their SHA-256 checksums
ROMS/                The OS ROM images to copy onto the SD card (not in git)
```

---

## What has and has not been tested

This matters, so it is stated plainly rather than buried.

**Verified in this repository (build-time):**

- All kernel images build cleanly and link as AArch64
  (`aarch64-none-elf`), about 835-860 KiB each.
- `scripts/verify.ps1` confirms each image is ELF64/AArch64, has its entry point
  at 0x80000, contains `_start`, carries the filename the firmware looks for on
  that board, and that its `.bss` ends before Circle's 2 MiB kernel region.
- Every module compiles warning-free, including with Circle's own flags
  (`-nostdinc++`, `-fno-exceptions`, `-fno-rtti`, `-std=c++17`).
- The `audio_sink_t` contract, the keyboard interface and the VTAB/VFS redirection
  were each checked against the real upstream headers.

**Verified on hardware (Raspberry Pi 400, 720p, HDMI):**

- It boots to BASIC, and the emulated machine runs.
- Video output at the correct aspect ratio with side bars.
- USB keyboard; the on-screen menu opens, navigates and closes.
- USB gamepads — a generic pad and a Switch Pro — attached before power-on and
  while running, with unplug, re-plug and swap.
- Mounting disk images, cartridges and cassettes from USB storage, and running
  them; ejecting and re-mounting; visiting the menu with an image loaded and
  cold-resetting afterwards.
- All settings are applied at startup and persist across a power cycle —
  machine type, RAM size (64/128/192/320 kB), video standard, and the rest.
- The crash marker ejects all media and restores machine defaults on the next
  boot, so a bad image cannot trap the machine in a crash loop.
- **Sound over HDMI**, at the correct pitch.
- **Frame rate: 49.8 fps against the 50 fps target.** The emulation costs about
  1.5 ms per frame and the picture fill a few milliseconds, so the frame has room
  to spare. Two earlier faults are worth naming because they were the whole
  reason for the low rates: the renderer once wrote the framebuffer one pixel at
  a time into memory the ARM cannot cache (30 fps), and the scaler once walked
  destination pixels and chased three dependent loads for each of them (18 ms of
  a 20 ms frame on a Zero 2 W). Both are fixed; `e2773e4` is the second.
- **OSD**: opening and closing, navigation with the USB keyboard, and the
  power cycle that is the only way out of a cold reset that inherited a console
  key mask from the software that ran before it.
- **Disk write-back**: an ATR mounted read/write is genuinely writable, and its
  mode can be changed while it stays inserted.

**Verified on hardware (Raspberry Pi Zero 2 W, 720p, HDMI, no audio on the display):**

- It boots to BASIC and runs at **49.8 fps in PAL and 59.9 fps in NTSC**, using
  the `kernel64` image (`kernel8.img`), with **no picture skipped**. The same
  board used to skip every second picture in PAL and could only reach 56 fps in
  NTSC, with visible flicker; the picture fill cost 17.9 ms and now costs 4.8 ms,
  so the adaptive fill skip is in the code but no longer fires.
- Boot reaches the first frame in about 6.6 s: 4.1 s of firmware (before any of
  this project's code runs), 0.4 s of Circle's own init, and the rest storage,
  core start-up and the first picture.
- The USB keyboard works **connected directly**. Through a USB hub it is not
  enumerated — see below.

**Verified on hardware (the same image, five boards):**

`kernel8.img` — the `kernel64` build — runs on a **Zero 2 W, a 3B, a 3B+, a 4 and
a 400**, at **720p or 1080p**, and works on all of them: it boots to BASIC, the
picture is correct, the keyboard and menu work, and images load and play.  The two
lists above are what was exercised in detail on the boards used for the feature
work; on the others the check was that the machine runs and is playable.

Every RAM configuration -- 64 / 128 / 192 / 320 (RAMBO) / 320 (Compy Shop) kB --
and both video standards, PAL and NTSC, were exercised across these boards.

The frame rate was not measured on the Pi 3 pair, so what is claimed for them is
that they work, not how much room they have.  The A72-optimised build
(`kernel8-rpi4.img`) has been run on the 4 / 400 class as well and works.

**NOT yet verified:**

- **Pi 5 (and Pi 500).** Its image builds and verifies, but it has never been
  run, and it is the only platform whose page-flip branch has never executed.
- The GPIO keyboard matrix, the 9-pin joystick ports, and hot-plugging USB
  storage.
- Cassette writing. Reading is verified for both standard and turbo tapes.
- Sound on the Zero 2 W: the test display had no speakers, so the sound device
  refused to open, which is the correct behaviour on such a display.

### A USB hub is not enumerated on the Zero 2 W

A keyboard plugged **directly** into the Zero 2 W works. The same keyboard behind
a USB hub does not, and worse, it used to stop the machine before the first
frame: Circle's plug-and-play enumeration found a device it could not read a
descriptor from (`usbdev0-1: Cannot get device descriptor (short)`) and retried
it without returning.

That enumeration is no longer driven during start-up — it is left to the frame
loop, where a bad device costs frames rather than the boot — so a hub no longer
prevents the machine from starting. It still does not deliver its keyboard.

This was confirmed with Circle's own `sample/08-usbkeyboard`, unmodified, which
shows the same behaviour with none of this project's code involved. It is a
limitation of Circle's USB stack on BCM2837, not of the emulator. If a hub is
needed, use one that enumerates correctly on this board, or connect the keyboard
directly.

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

This project's own code — `src/`, `scripts/`, `docs/`, `patches/` and the
Makefiles — is released under the **MIT licence**; see [`LICENSE`](LICENSE).

It builds against two upstream projects that are not MIT and are vendored under
`external/` so that the build is reproducible. They keep their own licences:

| Component | Licence |
|---|---|
| Circle | GNU General Public License **version 3** (`external/circle/LICENSE`) |
| Atari800 | GNU General Public License **version 2 or later** (`external/atari800/COPYING`) |
| FatFs (inside Circle) | permissive, ChaN's terms (`external/circle/addon/fatfs/LICENSE.txt`) |

That distinction matters for a **binary**. The kernel links Circle and Atari800
into one image, so a distributed image is a combined work under the GPL version 3
— with the GPL's obligations, including making the corresponding source
available. MIT applies to this project's source as published; it cannot and does
not relicense the components it links. Atari800's "or later" clause is what makes
the combination lawful: GPLv2-only code could not be merged with Circle's GPLv3.

Atari OS ROM images are **not** part of this repository — `ROMS/` is excluded from
version control and the images must be supplied by whoever runs the emulator.
The OSD font in `src/osd/atari_font.cpp` is a different matter: its shapes were
extracted from an XL/XE OS image and are compiled in as data, so they are
distributed here even though the ROM is not. They are Atari's glyph designs. If
that is a concern for your redistribution, the font is the one piece to replace.

Atari, Atari 800, Atari XL and Atari XE are trademarks of Atari Interactive,
Inc. This project is not affiliated with or endorsed by Atari.
