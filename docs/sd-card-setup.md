# SD card setup

This is the complete list of what goes on the card, where it goes, and why. The
SD card has to be formatted as **FAT32** and its first partition must be the one
the Pi boots from.

## Directory layout

```
SD card root
├── kernel8.img              <- the emulator: Zero 2 W, Pi 3, Pi 4, Pi 400
├── kernel_2712.img          <- the Pi 5 build (optional, but copy it)
├── armstub8-rpi4.bin        <- Pi 4 only, and only with kernel8-rpi4.img
├── config.txt               <- copied from external/circle/boot/config64.txt
├── start.elf                <- Raspberry Pi firmware
├── fixup.dat
├── bootcode.bin
├── bcm2711-rpi-4-b.dtb      <- device tree for your model
│   (and/or bcm2710-rpi-zero-2-w.dtb, bcm2712-rpi-5-b.dtb, ...)
├── overlays/
│   └── bcm2712d0.dtbo       <- Raspberry Pi 5 only
├── ROMS/
│   ├── ATARIXL.ROM          <- the OS ROMs you supplied
│   ├── ATARIOSA.ROM
│   ├── ATARIOSB.ROM
│   ├── ATARIBAS.ROM
│   └── atari5200.rom
└── ATARI/
    ├── settings.cfg         <- created automatically on first run
    ├── usbkeys.cfg          <- optional: USB keyboard remapping
    └── *.sav                <- snapshots you save
```

The two kernel images are alternatives, not additions: the firmware picks one by
name and ignores the other. Copy both and the card works in every supported
model without further thought. See
[Which image gets loaded](#which-image-gets-loaded).

The `ROMS/` and `ATARI/` directories are created automatically if they are
missing, except that `ROMS/` must contain the ROM images.

## Step by step

### 1. Format the card

One FAT32 partition, marked bootable, as the first partition.

### 2. Copy the Raspberry Pi firmware

A bare-metal kernel still needs the Raspberry Pi's **own boot firmware**. It is
the firmware that reads `config.txt`, sets up the display, loads the kernel image
into memory and starts it. It is Broadcom/Raspberry Pi binary code, so it cannot
be built — it has to be downloaded.

Circle ships only the `config64.txt` template; its `.gitignore` excludes the rest,
because these are large third-party binaries:

```
external/circle/boot/.gitignore
  *.bin   fixup*.dat   start*.elf   *.dtb   *.dtbo   LICENCE.broadcom   COPYING.linux
```

Fetch them with this project's script:

```powershell
# everything, so the card works in any supported model
pwsh -ExecutionPolicy Bypass -File scripts\fetch-firmware.ps1

# or only what one family needs
pwsh -ExecutionPolicy Bypass -File scripts\fetch-firmware.ps1 -For pi3
pwsh -ExecutionPolicy Bypass -File scripts\fetch-firmware.ps1 -For pi4
pwsh -ExecutionPolicy Bypass -File scripts\fetch-firmware.ps1 -For pi5
```

It downloads the same revision Circle pins, from the
[raspberrypi/firmware](https://github.com/raspberrypi/firmware/tree/master/boot)
repository, and **checks each file's size** so a truncated download is caught
here rather than as a black screen later.

Circle's own `make` does the same thing and also builds its ARM stub:

```bash
cd external/circle/boot
make                                     # needs wget
```

Either route is fine. The script exists because Circle's needs `wget` and a Unix
`make`, and because "run make in a subdirectory of a vendored tree" is a step
that is easy to miss — the symptom of missing it is a blank screen with no
message.

#### `start.elf` and `fixup.dat` are a pair

This is the part that confuses people, and the reason the files come in two
matching sets.

| File | What it is |
|---|---|
| `start.elf` | The firmware program itself: it initialises the GPU, reads `config.txt`, loads the kernel image and starts it. |
| `fixup.dat` | A table telling `start.elf` where in memory to place things, once it knows how big the kernel is. |

`start.elf` without its matching `fixup.dat` will not boot. And there are two
generations, so **the names must match within a pair**:

| Models | Firmware pair |
|---|---|
| Pi Zero 2 W, Pi 3 | `start.elf` + `fixup.dat` |
| Pi 4, Pi 400, CM4 | `start4.elf` + `fixup4.dat` |

Mixing a `start` with a `fixup4`, or the reverse, produces no output at all.
Copying both pairs is harmless — each board reads its own.

| File | Needed by |
|---|---|
| `bootcode.bin` | Pi Zero 2 W, Pi 3 — the first-stage bootloader. **Not used** by the Pi 4 or 5, which load their firmware differently. |
| `start.elf`, `fixup.dat` | Pi Zero 2 W, Pi 3 |
| `start4.elf`, `fixup4.dat` | Pi 4, Pi 400, CM4 |
| `bcm2710-rpi-zero-2-w.dtb` | Pi Zero 2 W |
| `bcm2711-rpi-4-b.dtb` | Pi 4 |
| `bcm2711-rpi-400.dtb` | Pi 400 |
| `bcm2711-rpi-cm4.dtb` | CM4 |
| `bcm2712-rpi-5-b.dtb` | Pi 5 |
| `overlays/bcm2712d0.dtbo` | Pi 5 |
| *(none)* | The Pi 5 needs **no** `start`/`fixup`/`bootcode` at all: its firmware lives in an on-board EEPROM and reads the SD card directly. |

Only the files your board uses are read. Copying the set costs about 5.5 MB and
makes one card work in every supported model.

### 3. Copy the firmware files for your model

Copy all of the downloaded files to the root of the SD card, with the overlay in
its own directory:

```powershell
$src = 'external\circle\boot'
Copy-Item "$src\bootcode.bin","$src\start.elf","$src\fixup.dat" D:\
Copy-Item "$src\start4.elf","$src\fixup4.dat" D:\
Copy-Item "$src\bcm27*.dtb" D:\
Copy-Item "$src\LICENCE.broadcom","$src\COPYING.linux" D:\
New-Item -ItemType Directory -Force D:\overlays
Copy-Item "$src\overlays\bcm2712d0.dtbo" D:\overlays\
```

The per-model file list is in [step 2](#2-copy-the-raspberry-pi-firmware) above.
You only need the device tree for the model you are using, but copying several
costs nothing and makes the card work in more machines.

### 4. Configure `config.txt`

Copy Circle's 64-bit template and rename it:

```powershell
Copy-Item external\circle\boot\config64.txt D:\config.txt
```

There is no 32-bit variant to choose between: this emulator is AArch64 only, so
`config64.txt` is always the right one.

Two lines in `config64.txt` are worth knowing about, because they are *commented
out* and changing them will break the picture:

```ini
[pi5]
#framebuffer_depth=32
#framebuffer_ignore_alpha=1
```

Leave them commented out. The emulator asks for a **16 bpp** framebuffer
directly, which the Pi 5 firmware provides. If you uncomment these, the firmware
switches to 32 bpp while the emulator still writes 16 bpp pixels and you will
get a scrambled or discoloured picture — not a black screen, which makes it
confusing to diagnose.

#### Optional: forcing a resolution

The emulator asks for 1280x720 (or 1920x1080 if you choose it in the menu). To
force a specific mode instead, add to `config.txt`:

```ini
hdmi_group=1
hdmi_mode=4        # 720p60;  hdmi_mode=19 is 720p50, 16 is 1080p60
```

Set `hdmi_mode` to match your chosen resolution. Use `hdmi_mode=19` (720p50) or
`hdmi_mode=31` (1080p50) with a PAL machine if you want the refresh rate to
match the emulated frame rate exactly.

### 5. Copy the kernel images

```powershell
# every model except the Pi 5
Copy-Item build\kernel64\kernel8.img D:\

# the Pi 5
Copy-Item build\kernel64-pi5\kernel_2712.img D:\
```

Copy both. They do not conflict: each is a complete emulator for a different
board, and the firmware loads one of them.

#### Which image gets loaded

| Board | File the firmware looks for |
|---|---|
| Pi Zero 2 W, Pi 3, Pi 4, Pi 400, CM3/CM4 | `kernel8.img` |
| Raspberry Pi 5 | `kernel_2712.img` |

This is the firmware's own naming, not a choice this project makes, and it is why
the two images exist rather than one. A single `kernel8.img` cannot serve the Pi 5
because the Pi 5's peripherals are behind the RP1 southbridge, reached over PCIe,
and Circle compiles that support in only for `RASPPI=5` — see
[Supported models](../README.md#why-a-pi-5-needs-its-own-image).

If you copied only `kernel8.img` and put the card in a Pi 5, the firmware will
find no `kernel_2712.img`, fall back to `kernel8.img`, and boot a kernel built
for different hardware. Expect a dark screen rather than an error message.

### 6. The Pi 4 ARM stub (only if you built `kernel64-pi4`)

**Skip this step unless you are using the `kernel64-pi4` image.**

Circle's `config64.txt` contains this, under the `[pi4]` section:

```ini
[pi4]
armstub=armstub8-rpi4.bin
kernel=kernel8-rpi4.img
```

The Pi 4's firmware loads a small ARM stub before the kernel. Circle needs its
own replacement, because the stub is what sets up the GIC — the interrupt
controller — the way Circle's code expects, and what releases the secondary CPU
cores.

**There is nowhere to download it.** Circle ships the assembly source and expects
you to build it:

```
external/circle/boot/armstub/armstub8.S      <- the source
external/circle/boot/armstub8-rpi4.bin       <- the result, 336 bytes
```

Two ways to get it:

```powershell
# On its own
pwsh -ExecutionPolicy Bypass -File scripts\build-armstub.ps1

# Or automatically, as part of building the Pi 4 image
pwsh -ExecutionPolicy Bypass -File scripts\build.ps1 -Profile kernel64-pi4
```

Both put the file in `external/circle/boot/`. Circle's own `make` in
`external/circle/boot` builds it too (`make armstub64`), with the result in the
same place; the script above performs the same four steps using this project's
toolchain paths, so it does not depend on Circle's `Config.mk` being configured.

Then copy it to the root of the SD card:

```powershell
Copy-Item external\circle\boot\armstub8-rpi4.bin D:\
```

**If you are not using `kernel64-pi4`**, you do not need the file — but the
`[pi4]` section of `config64.txt` will still name it. That is harmless: the
firmware only reaches that line on a Pi 4, and a `config.txt` naming a file that
is not present is not an error. Either copy the 336-byte file anyway, or delete
the `armstub=` line from the `[pi4]` section.

> **Does this project need the stub at all?** Not strictly. It exists for FIQ
> support on the Pi 4, 400 and CM4, and this emulator uses no FIQ — Circle's
> `USE_USB_FIQ` is commented out in its default `sysconfig.h`, and nothing in
> `src/` refers to FIQ. Building it costs nothing and removes a reference to a
> missing file, so that is what the build script does.

The Pi 5 needs none of this: its firmware lives in on-board EEPROM, and
`config64.txt` has no `armstub` line for it.

### 7. Copy the OS ROM images

```powershell
New-Item -ItemType Directory -Force D:\ROMS
Copy-Item ROMS\* D:\ROMS\
```

The emulator looks in `SD:/ROMS` and matches each file against the Atari800
core's own table of known ROM images, by length and CRC32. A file with the wrong
size or contents is reported in the log and ignored.

| File | Size | Recognised as |
|---|---|---|
| `ATARIXL.ROM` | 16384 | XL/XE OS |
| `ATARIOSA.ROM` | 10240 | Atari 800 OS rev. A |
| `ATARIOSB.ROM` | 10240 | Atari 800 OS rev. B |
| `ATARIBAS.ROM` | 8192 | Atari BASIC |
| `atari5200.rom` | 2048 | Atari 5200 BIOS |

At least `ATARIXL.ROM` is needed to boot an XL/XE machine. If no usable OS ROM
is found the emulator falls back to **AltirraOS**, a free replacement OS that is
compiled into the kernel, and says so in the log — so it will still start, but
it will not be a genuine Atari OS.

### 8. Prepare the USB drive (optional but expected)

Disk, cartridge and cassette images are read from a **USB drive**, not the SD
card. Format it as FAT32 and put your images anywhere on it:

```
USB drive
├── disks/
│   ├── BOUNTYBOB.ATR
│   └── ... 
├── carts/
│   └── ... 
└── tapes/
    └── ... 
```

Any layout works: the emulator's file browser lets you walk the whole drive.
Images can be written to as well as read (see
[Using the emulator](using.md#writing-back-to-images)), so it is worth not
using the only copy of anything irreplaceable.

The emulator works without a USB drive — you simply cannot load any media.

### 9. First boot

Insert the card, plug in a USB keyboard, and power on. You should see the
emulator's start-up messages on screen briefly, then the Atari's blue
`READY` screen.

Press **F12** to open the menu. Your settings are written to
`SD:/ATARI/settings.cfg` as you change them.

## Verifying the card

Before blaming the build, check:

- The image file is named exactly right for the board: `kernel8.img` for a Zero
  2 W, Pi 3, Pi 4 or Pi 400, and `kernel_2712.img` for a Pi 5. A wrong name means
  the firmware finds nothing to boot, and the screen stays dark with no message.
- `config.txt` is present and is Circle's `config64.txt`, not one written for a
  Linux installation. A `config.txt` containing `cmdline.txt`-style options or a
  `dtoverlay=` for Linux hardware will confuse the firmware.
- The kernel image is smaller than 4 MiB. An over-sized image makes `sysinit()`
  halt silently; there is no error, only a dark screen.

If the screen stays dark, see [Troubleshooting](troubleshooting.md).
