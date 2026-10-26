# Troubleshooting

## The screen tells you how far it got

The emulator paints a solid colour as each start-up stage begins. If it stops
responding, **the colour left on the screen names the stage it died in** — you do
not need a serial cable or a debugger. Read it as a traffic light that got
further:

| Colour on screen | Stage reached | What it means |
|---|---|---|
| **Nothing at all** (black) | — | The kernel never ran. This is a boot-chain problem, not an emulator bug — see below. |
| **Red** | power-on | Circle started, but the text console or a Circle subsystem failed. |
| **Orange** | text console | Circle's console is up; USB is being started. |
| **Amber** | USB | The USB host controller is being started. |
| **Yellow** | SD card | The SD card interface is being started. |
| **Yellow-green** | file systems | The SD card and USB drive are being mounted. |
| **Green** | settings | Settings are being read. |
| **Spring green** | framebuffer | The framebuffer is being created — **this is the first stage that proves HDMI works**. |
| **Cyan** | sound | The sound device is being opened. |
| **Sky blue** | emulation core | The Atari800 core is starting. |
| **Violet** | input | Keyboard and controllers are being set up. |
| **White, then the Atari screen** | running | Start-up succeeded. |

A colour means *that stage was reached*: the machine stopped inside it, or
between it and the next colour.

If start-up fails in a way the code can detect, the screen instead shows a
heading, the stage reached, and the last few dozen messages from the log. That
is the single most useful thing to photograph and send on.

**Note the two problems this solves.** Circle's logger writes to the firmware
text console, which the emulator's own framebuffer then replaces — so log output
was being thrown away at exactly the point things start to go wrong. And a hang is
indistinguishable from a failure on a black screen; a colour is not.

## The log file

As soon as the SD card is mounted — the earliest moment a file can be written —
everything is also written to:

```
SD:/ATARI/bootlog.txt
```

Each line is flushed as it is written, so the file is complete even if the
machine hangs immediately afterwards or the power is cut. **Read it on a PC**;
that is the point of it. It is plain text:

```
[       0 ms] Atari XL/XE emulator for Raspberry Pi
[       0 ms] Build: Feb 14 2026 11:02:31
[       0 ms] --- stage: power-on (Circle started) ---
[      12 ms] SD card interface started
[      15 ms] SD card mounted, system directory is SD:/ATARI
[      18 ms] Boot log open at SD:/ATARI/bootlog.txt
[      21 ms] No USB drive found; disk, cartridge and cassette images ...
[      24 ms] Settings loaded from SD:/ATARI/settings.cfg
[      31 ms] --- stage: framebuffer created ---
[      44 ms] Display: 1280x720, picture 960x720
[      47 ms] Sound device started
[     120 ms] Emulation core started: Atari800 7.2.1
[     180 ms] --- stage: running (first frame drawn) ---
```

If start-up fails, the file ends with the reason rather than simply stopping:

```
[     190 ms] FAILED: The emulation core could not be started
=== end of boot log ===
```

### The previous session's log

There is also `SD:/ATARI/bootlog.prev.txt`, which is the *previous* boot's log.
It exists for the awkward case where the machine dies before it can write
anything at all — a corrupted card, or a failure inside the SD interface itself.
Without it, opening a new log would erase the only evidence there was.

Both are shown on the failure screen, newest session last.

### What the log cannot capture

If the file is missing entirely, the SD card was never mounted, and the problem
is earlier than this: see the colour table above, and
[Is the firmware actually loading the kernel?](#6-is-the-firmware-actually-loading-the-kernel).
A **red** screen with no log file means Circle started but the SD interface did
not.

Messages from before the card was mounted are buffered in memory and written out
as soon as the log opens, so nothing is lost in the ordinary case — the file
starts with a note saying so.

---

## Nothing on screen at all

The screen stays completely dark, with no text and no picture. Work through
these in order; the first three account for almost every case.

### 1. Is the kernel image named correctly?

The firmware looks for an exact filename, and a wrong name means it finds nothing
to boot — silently. There is no error message, only a dark screen.

| Build profile | File the firmware loads | Boards |
|---|---|---|
| `kernel64` | `kernel8.img` | Pi Zero 2 W, Pi 3, Pi 4, Pi 400 |
| `kernel64-pi5` | `kernel_2712.img` | Raspberry Pi 5 |
| `kernel64-pi4` | `kernel8-rpi4.img` | Pi 4, Pi 400 (A72-optimised) |

Check the name character by character. `kernel_2712.img` has an underscore, and
`kernel8-rpi4.img` will only be loaded on a Pi 4 — on a Zero 2 W or Pi 3 the
firmware looks for `kernel8.img` and will not find it.

### 2. Is the image larger than 2 MiB?

Circle's `sysinit()` checks the kernel against `KERNEL_MAX_SIZE`, which is 2 MiB,
and if it is over, **halts silently**. No message, no colour, nothing.

```powershell
Get-Item build\kernel64\kernel8.img | Select-Object Length
```

The built images are about 820–845 KiB, so this is unlikely unless something has
gone wrong in the link. `verify.ps1` reports the image size and checks where the
kernel's `.bss` ends against Circle's region.

### 3. Is `config.txt` right, and are the firmware files present?

See [SD card setup](sd-card-setup.md). The two things that catch people out:

- Using a `config.txt` written for a Linux installation. Circle's own
  `config64.txt` is what you want.
- Missing firmware files — `start.elf` and `fixup.dat` (or `start4.elf` and
  `fixup4.dat` on a Pi 4), plus the device tree. These are **downloaded**, not
  built, and Circle's `boot/` directory does not contain them:

  ```powershell
  pwsh -ExecutionPolicy Bypass -File scripts\fetch-firmware.ps1
  ```

  The two files of a pair must match: a `start.elf` with a `fixup4.dat` will not
  boot. See [SD card setup](sd-card-setup.md#2-copy-the-raspberry-pi-firmware).

On a Pi 5 there are no firmware files to copy at all — the firmware is in
on-board EEPROM — but you do need `bcm2712-rpi-5-b.dtb` and
`overlays/bcm2712d0.dtbo`.

If you are using `kernel64-pi4`, the Pi 4 also wants `armstub8-rpi4.bin` on the
card. That file cannot be downloaded: it is built from assembly in Circle's tree,
by `scripts\build-armstub.ps1` or automatically as part of the `kernel64-pi4`
build. See
[SD card setup](sd-card-setup.md#6-the-pi-4-arm-stub-only-if-you-built-kernel64-pi4).

### 4. Is it the right image for the board?

Every supported model has its own expected filename, and the Pi 5 in particular
needs a build made for it rather than a renamed one:

| Board | Needs | A `kernel8.img` will not work because |
|---|---|---|
| Pi Zero 2 W, Pi 3, Pi 4, Pi 400 | `kernel8.img` | — |
| Raspberry Pi 5 | `kernel_2712.img` | its peripherals are behind the RP1 southbridge, and Circle compiles that support in only for `RASPPI=5` |

If only `kernel8.img` is on the card and it is put in a Pi 5, the firmware falls
back to it and boots a kernel built for different hardware. That looks like a dark
screen, not an error.

---

### 5. Getting the full log over serial

If none of the above explains it, the kernel's complete start-up log can be sent
to the UART instead of the screen. This needs a USB-to-TTL adapter (about £5)
connected to **GPIO 14 (TXD, pin 8)**, **GPIO 15 (RXD, pin 10)** and ground, at
**115200 baud, 8N1**.

Create `cmdline.txt` on the SD card containing one line:

```
logdev=ttyS1 loglevel=4
```

Circle then writes every log message to the serial port as well, including the
messages from before the framebuffer exists — which the on-screen report cannot
show, because there is no screen yet at that point.

The serial port shares GPIO 14/15 with the **GPIO keyboard**, so if that keyboard
mode is selected, the two cannot be used together. With a USB keyboard they do
not conflict.

### 6. Is the firmware actually loading the kernel?

A rainbow splash that never goes away, and a black screen after
`disable_splash=1`, both point at the boot chain rather than at the emulator. In
that case the kernel may never have run at all — which is why the diagnostic
colours above matter: **any** colour proves the kernel started.

Check, in order:

```powershell
# 1. does the SD card have the file the [pi4] section names?
Get-ChildItem D:\*.img | Select-Object Name,Length

# 2. are the firmware pairs and the ARM stub present?
Get-ChildItem D:\start*.elf,D:\fixup*.dat,D:\bootcode.bin,D:\armstub*.bin
Get-ChildItem D:\bcm27*.dtb
```

The `[pi4]` section of Circle's `config64.txt` — which the Pi 400 also reads,
along with the Pi 4B, CM4 and CM4S — names **two** files:

```ini
[pi4]
armstub=armstub8-rpi4.bin
kernel=kernel8-rpi4.img
```

Both must be on the card, with those exact names. If `armstub8-rpi4.bin` is
missing, the firmware has no ARM stub to load and the kernel never starts: the
splash stays, or the screen goes black. Build it with:

```powershell
pwsh -ExecutionPolicy Bypass -File scripts\build-armstub.ps1
```

If you would rather not use the stub, remove the `armstub=` line from the
`[pi4]` section instead — but the file is 336 bytes and building it is one
command.

Also confirm `config.txt` really contains the `[pi4]` section. Circle's template
has it; a `config.txt` written by hand from an older example may not.

---

## The picture is scrambled or the colours are wrong

Almost always the Pi 5 framebuffer depth. In `config.txt`, under `[pi5]`:

```ini
#framebuffer_depth=32
#framebuffer_ignore_alpha=1
```

**Leave these commented out.** The emulator asks for a 16 bpp framebuffer and
writes RGB565 pixels. If the firmware is switched to 32 bpp, the emulator keeps
writing 16 bpp data into a 32 bpp buffer, which produces a scrambled image — not
a black screen, which is what makes it confusing.

If the colours are wrong in a *regular* way — red and blue swapped — that is the
firmware's `framebuffer_swap` setting. Remove it from `config.txt`, or set it to
the opposite value, and try one solid red and one solid blue test screen to
confirm which way round it is. Circle has no way to query this setting, so it
cannot be detected automatically.

---

## No sound

1. **Is sound enabled?** F12 → Sound → Enabled. The volume is a separate setting;
   check both.
2. **Is anything plugged into the audio output?** The Pi Zero, Pi Zero 2 W and
   Pi 3 have **no 3.5 mm jack**. Audio comes out of the PWM pins (GPIO 12 and 13)
   and needs a simple RC low-pass filter and an amplifier. On the Pi 4 and Pi 5
   the jack exists, but the Pi 5 needs a suitable adapter.
3. **HDMI audio** is not used by this emulator. It is deliberately routed through
   the PWM device, which is the one Circle sound device with the same API on
   every model from the Pi Zero 2 W to the Pi 5.
4. **Check the log.** Start-up messages report whether audio was opened. If it
   says the sound device could not be opened, the emulator runs silently rather
   than refusing to start — that is intended, but it does mean a failure here is
   not fatal and therefore easy to miss.

---

## The USB keyboard does not work

1. **Is it plugged in before power-on, or is hot-plug working?** Start-up no longer
   waits for a keyboard: it scans what the USB stack has already enumerated and
   carries on, so a keyboard plugged in later is picked up by the frame loop. The
   log says nothing at start-up, because that scan runs before Circle has
   registered a keyboard and would report "absent" on a machine where one works a
   frame later. If no keyboard has appeared after about two seconds of running, the
   log says so once, and says `Keyboard connected` if one turns up afterwards.
2. **Is it behind a USB hub? On a Pi Zero 2 W, that is the most likely cause.** A
   directly-connected keyboard works; behind a hub it may not be enumerated at
   all, and then it is simply invisible. Circle's own `sample/08-usbkeyboard`
   behaves identically with none of this project's code involved, so this is a
   limit of Circle's USB stack on BCM2837. Connect the keyboard directly.
3. **Is the keyboard source set to USB?** F12 → the keyboard mode setting. If it
   is set to GPIO, the emulator is looking at the GPIO header, not at USB.
4. **Is a key typing the wrong Atari character?** The built-in mapping is by key
   position, not by the legend on the cap, and a few keys deliberately trade one
   character for another (`'` types the Atari `*`). If that does not suit your
   keyboard, remap it: see [Remapping the USB keyboard](usbkeys.md).
5. **Does the Pi supply enough power?** A keyboard plus a USB drive on a Pi Zero
   2 W can brown out the board. Use a powered hub.
6. **On the Pi 5**, USB is provided by two xHCI controllers behind PCIe; both are
   initialised, so any port should work. If it does not, try another port — this
   is one of the areas flagged as needing real-hardware verification.

If the machine does not reach the first frame **at all** when a hub is attached,
that was an older bug: enumeration used to be driven during start-up, and a device
that never returned a descriptor was retried without end. Current builds leave
that enumeration to the frame loop, so a bad device costs frames rather than the
boot. If you see this on a current build, the log will show the last
`input: …` step reached.

---

## A joystick or gamepad does not work

First, which kind is it?

**A USB gamepad.** Check the log for `Watching 4 USB gamepad inputs`. That line
appears at start-up and means the driver is running; it does not mean a pad is
attached, because one can be plugged in later. If nothing is attached at start-up
the next line says so:

```
[    7720 ms] kernel: Watching 4 USB gamepad inputs; plug one in at any time
[    7721 ms] kernel: No USB gamepad is attached yet; one plugged in later is noticed without restarting
```

If a controller is attached and nothing happens, the likely cause is that it is
not being recognised as a gamepad. Circle identifies controllers by their USB
class and their report descriptor; a device that identifies as a keyboard, a
mouse or a vendor-specific device is not read here. When a controller **is**
recognised, the log names it, what it reports and which mapping it was given:

```
usbjoy: USB gamepad 1 attached (ven57e-2009): 21 buttons, 0 hats, 4 axes, map buttons (Circle driver)
```

If that line never appears, plug the controller in and watch for two things:

```
kernel: USB device tree changed: something was attached, removed, or enumeration was retried
usbjoy: USB gamepad 1 attached (ven79-11): 10 buttons, 0 hats, 5 axes, map axes
```

The first means USB noticed the device. If it appears without the second, Circle
found a device but could not make a gamepad of it — that is the failure the attach
line distinguishes from a pad that was never seen at all.

Once a controller is attached, nothing more is written until it is removed:

```
usbjoy: USB gamepad 1 removed
```

Presses are not logged: one line per press would be a card write per press in the
frame loop. If the mapping itself is in doubt — the attach line named a rule, but
the wrong direction moves — `probe/gamepad` is the tool for it: it prints every
change of buttons, axes and hats with the directions they resolve to.

**A 9-pin joystick.** Check the log for this line, once per port:

```
GPIO joystick port 1 ready on GPIO 4, 5, 6, 8 and 9
```

If it says the port did not initialise, the message names the pin that was
already in use. The usual cause is that the GPIO keyboard is selected: they need
the same header pins, and only one of the two can have them. See
[Joysticks](joystick.md#the-pin-budget).

If the port is ready but nothing moves, the wiring is the thing to check:

- each switch goes between its **GPIO pin** and a **ground** pin (6, 9, 14, 20,
  25, 30, 34 or 39) — not between two GPIO pins;
- the joystick's **pin 7 must not be connected to anything**. It carries five
  volts on a real Atari and would damage the Pi;
- the joystick's pin 8 is the common ground, and every switch shares it.

A wrong direction is worth reading carefully: the pins are listed in
[Joysticks](joystick.md#pin-assignment), and up/down and left/right being swapped
is the most common mistake in a hand-made cable.

---

## The GPIO keyboard does not work

1. **Is the keyboard source set to GPIO?** F12 → the keyboard mode setting. It
   defaults to USB.
2. **Check the wiring against the table** in [Keyboard: GPIO](keyboard-gpio.md).
   The single most common mistake is connecting a row line to a column line.
3. **Check the ground.** Every switch needs a return path to ground. The Atari
   keyboard matrix has no ground connection per key — the return lines carry the
   signal — but the four console keys each need their own ground.
4. **Are the pull-ups enabled?** The driver configures the column lines as inputs
   with pull-ups. If you have wired external pull-up resistors, that is fine too,
   but do not leave the column lines floating.
5. **On a Pi 5**, GPIO lives behind the RP1 southbridge. The driver accounts for
   this, but this is another area flagged as needing real-hardware verification.

---

## No disk, cartridge or cassette images are found

1. **Is the USB drive plugged in?** The file browser says "No USB drive detected"
   when it is not. Plug it in and reopen the menu — the emulator re-checks each
   time the browser opens, so it does not need a reboot.
2. **Is it formatted FAT32?** The FatFs configuration here has **no exFAT
   support** and **no 64-bit LBA support**, so a drive formatted exFAT, or a
   drive larger than 2 TB, will not mount. Reformat as FAT32.
3. **Is the file extension right?** The browser filters by extension: ATR and XFD
   for disks, CAR and ROM for cartridges, CAS for cassettes. A file with the
   wrong extension will not be listed.
4. **Is the image compressed?** DCM and gzip-compressed ATR/XFD images are
   **not supported** — decompressing them requires a temporary file, which this
   file layer deliberately does not provide. Uncompress them on a PC first.

---

## SELF TEST reports a ROM error

**This is expected with the SIO patch enabled, and it is not a fault in the
emulated memory.**

The XL/XE self-test adds up the OS ROM and compares the total with the value the
ROM stores for itself. Atari800's fast-load SIO patch works by writing ESC
opcodes **into the ROM image in memory** — `ESC_PatchOS()` in
`external/atari800/src/esc.c` — so nine bytes of a 16 KB XL OS no longer match:

| Address | Becomes | Purpose |
|---|---|---|
| `$E459` | `F2 xx 60` | fast SIO entry |
| `$FD13` | `F2 xx` | cassette leader wait, load |
| `$FD60` | `F2 xx` | cassette leader wait, save |
| `$C31D` | `EA EA` | disables the OS's own boot-time checksum warning |

The last row is why the machine boots and runs normally: the core suppresses the
OS's boot-time complaint about the checksum it has just invalidated. It cannot
suppress the self-test's separate verification, which is why that one still fails.
The RAM tests pass, because the emulated RAM is untouched — only the ROM contents
differ.

**To see the self-test pass**, run with the patch off. The patch mode follows the
tape type (`src/port/platform.c`): insert any **turbo** `.cas` and power cycle,
and the machine comes up in real-timing mode with an unmodified ROM — the log says
`SIO patch off, real timing`. The price is that every load then runs at true SIO
speed.

If the ROM test still fails in that mode, the ROM image itself is not pristine:
many circulating XL OS files are already patched for high-speed SIO, and their
stored checksum does not match their contents. The boot log records which revision
the core identified from the image's CRC.

---

## A disk image will not load

If it is an ATR or XFD and it is on a working FAT32 USB drive, the likely cause
is that the image is damaged or is not really an ATR. The core reads and
validates the ATR header when mounting and reports the failure. The log has the
detail; the menu shows a "Failed: ..." message.

---

## Snapshots are truncated or will not load

A snapshot of a machine with more than 64 kB of RAM is larger than the Atari800
core's default save-state buffer, and upstream reports the overflow **silently**.
This project raises the buffer size to 512 KB, which covers a 320 kB machine.

If snapshots misbehave, check that `-DSTATESAV_MAX_SIZE=524288` is actually being
passed — it is set in `Makefile.core`. Building the core without it re-creates
the upstream bug.

---

## The emulator resets itself, or a game crashes

If the emulated CPU executes one of the illegal "crash" opcodes, the core
abandons the frame and the kernel performs a warm reset. This is deliberate
containment: the alternative is that the jump lands nowhere and takes the
Raspberry Pi down with it.

The log records `Frame error: ...` with the core's description. A single reset is
usually a game doing something the emulator cannot handle; a loop of resets means
the image is broken or the wrong machine type (XL versus XE) is selected.

**BRK is not in that class by default.** It is a normal 6502 instruction and the
emulator runs it as the machine does, because that is how a debugger takes
control — MAC/65's DDT, for instance. If the log instead says

```
Frame error: the emulated machine executed a BRK instruction
```

then `CONTINUE_ON_BRK=0` has been set in the settings file on purpose, and the
machine is still running: the frame was abandoned, not the emulator. Set it back
to `1` to stop reporting BRKs. See [the settings file](using.md#the-settings-file).

---

## Performance is poor

The menu shows the frame rate it is *aiming* for — 49.86 fps in PAL, 59.92 in
NTSC — not the rate achieved. The achieved rate is in the `stats` line of the
runtime log (see [Getting diagnostic information](#getting-diagnostic-information)). A Zero 2 W reaches both, so a low figure is not normal on any
supported board.

Two things worth knowing before changing anything:

- The emulated machine is never slowed down to fix the picture. If a frame cannot
  be drawn within its deadline, the picture is skipped instead, and the periodic
  `stats` line in the runtime log says how many were skipped ("N skipped"); the
  emulated frame count stays at 50 or 60. So a *low fps* means the emulation
  itself is being starved, while *skipped pictures with normal fps* means only the
  display is struggling.
- Accurate PAL blending is per-pixel and expensive; it is the first display option
  to turn off.

If the frame rate is low:

- Build the optimised profile for the board — `kernel64-pi4` for a Pi 4.
- Turn off accurate PAL blending (F12 → Display → Artifacting → `PAL blending
  (accurate)` → `None` or `PAL blending (simple)`).
- Use the 720p output rather than 1080p: the fill costs what the picture area
  costs, and 1080p is 2.25 times the pixels — about 4.8 ms against 10.8 ms on a
  Zero 2 W, which is the difference between drawing every frame and skipping
  every second one.
- On a board that cannot keep up, `STATIC_OSD=1` makes the OSD cheap; see
  [Using the emulator](using.md).

---

## Getting diagnostic information

The emulator writes start-up messages and any errors to the screen and to the
serial port at **115200 baud, 8N1**.

The serial console is configured by Circle's `cmdline.txt` options; on a bare
board the UART is on GPIO 14 (TX) and GPIO 15 (RX). The GPIO keyboard does not
use those pins, so a serial console and a GPIO keyboard can coexist.

The log tells you, in order:

- whether USB started
- whether the SD card mounted, and where the system directory is
- whether a USB drive was found
- the display resolution and whether it is double buffered
- whether audio opened
- whether the emulation core started, and its version
- whether an OS ROM was found, or whether it fell back to AltirraOS
- whether a keyboard was found, and which kind

Most problems can be identified from that list alone.

Once the machine is running, the log keeps a heartbeat so that a hang is
distinguishable from a healthy machine:

- `stats: …` every **thirty seconds** — the achieved frame rate and frame count,
  and the number of pictures skipped in that window when there were any. If the
  file stops growing, the machine stopped there.
- `boot t=30s` / `boot t=60s` after a cold reset — the same idea for a machine
  that is running but never boots, with `PC`, `DMACTL`, the display list address,
  `PORTB` (the RAM bank the OS has selected) and the error count.
- `OSD: opened` / `OSD: closed` whenever the menu is toggled, whichever way it was
  toggled — useful for telling a crash while the menu was up from one after it.

Both are deliberately sparse: every line is written and flushed to the card, and
the machine's frame budget is the thing being measured.
