# Using the emulator

## The OSD menu

Press **F12** on a USB keyboard, or **Shift+Help** on an Atari keyboard wired to
the GPIO header, to open the menu. Press it again, or press Escape from the main
screen, to close it.

```
┌──────────────────────────────────────────────┐
│ Atari XL/XE                                  │
├──────────────────────────────────────────────┤
│ Disk drives          2 of 4 loaded           │
│ Cartridge            (none)                  │
│ Machine and memory   Atari XL, 64 kB         │
│ Display              PAL, 1280x720           │
│ Sound                80%                     │
│ Snapshots            Save / load / reset     │
│ Close menu                                   │
├──────────────────────────────────────────────┤
│ Atari XL  64 kB  PAL  49.9 fps               │
│ Up/Down move  Left/Right change  Return sel  │
└──────────────────────────────────────────────┘
```

### Keys

| Key | Action |
|---|---|
| Up / Down | Move the selection |
| Left / Right | Change the selected setting, or toggle a disk's read-only flag |
| Return or Space | Select the item |
| Escape | Up one directory in the browser; otherwise back a screen, and close from the main screen |
| Delete | Eject the selected drive, cassette or cartridge |

The picture keeps running behind the menu, dimmed, so you can see what you are
changing.

### What is in the menu

**Disk drives** — Load an image into each of the four drives, and into the
cassette and the cartridge. Selecting a drive opens a file browser. While a
drive with an image is selected, Left/Right toggles its read-only flag, shown as
`[ro]`. **Delete** ejects whatever is selected.

**Cartridge** — Load a cartridge image (CAR or ROM).

**Machine and memory** — The machine type (XL, XE, XE Game System), the RAM size,
the video standard, and a "restore defaults" item that resets the machine
settings without losing your mounted media.

**Display** — PAL or NTSC, and the artifacting mode.

**Sound** — On or off, volume in 5% steps, and stereo (two POKEYs).

**Snapshots** — Save or load a machine snapshot, and cold or warm reset.

### Changing something takes effect immediately

There is no "apply" step. Changing the volume, the video standard, the sound
switch or the artifacting applies at once; changing the RAM size or the machine
type rebuilds and resets the emulated machine, which is what those changes
inherently require.

Two things are the exception, and the menu says so when you change them:

- **A disk's read-only flag** is applied by the emulation core when the image is
  opened, so toggling `[ro]` on a drive that is already loaded does not by itself
  protect the file being written. Re-select the image to reopen it read-only.
- **Switching sound back on** reopens the audio device; if that fails the menu
  reports it.

### Settings are saved as you go

Every change is written to `SD:/ATARI/settings.cfg` immediately, so the emulator
comes back the way you left it even if the power is cut. The file is plain text
and is meant to be readable and editable; see [the settings file](#the-settings-file).

---

## Loading media

Media images live on the **USB drive**, not the SD card. Disk, cartridge and
cassette images are all loaded through the menu:

1. Open the menu with F12.
2. Choose **Disk drives**, then the drive you want.
3. The file browser opens, listing the files that match (ATR and XFD for disks,
   CAR and ROM for cartridges, CAS for cassettes).
4. Directories are shown with a trailing `/`; press Return to enter one, and
   Escape to go back up.
5. Press Return on a file to load it.

### Which formats are supported

| Media | Formats | Read | Write |
|---|---|---|---|
| Disk | ATR, XFD | yes | **yes** |
| Cartridge | CAR, ROM | yes | — |
| Cassette | CAS | yes | **yes** |

Compressed disk images (**DCM**, and **gzip-compressed** ATR or XFD) are **not**
supported. Decompressing them needs a temporary file, and the file layer
deliberately does not claim the temporary-file functions, because on bare metal
there is nowhere for them to go. Uncompress those images on a PC first.

### Writing back to images

Disk and cassette images can be written to:

- A disk image is opened read/write by default. Anything the emulated machine
  writes is written back to the file, so you can format a disk, save a program,
  and the changes persist on the USB drive.
- If a disk is meant to be preserved, set its read-only flag with Left/Right on
  its menu entry. It then appears as `[ro]`, and the core falls back to opening
  it read-only.
- A cassette image is opened read-only by default. Use Left/Right on its menu
  entry to toggle write protection before loading it.

Writing to a real FAT file system from a bare-metal program is the part of this
emulator most likely to surprise you if something goes wrong. Keep a copy of
anything irreplaceable.

---

## Snapshots

A snapshot captures the whole emulated machine: all of its RAM including the
extended memory banks, the chip state (ANTIC, GTIA, POKEY, PIA) and the CPU
registers. Loading one puts the machine back exactly where it was.

**Save snapshot** opens a file browser rooted at `SD:/ATARI`, listing the `.SAV`
files that are already there; picking one overwrites it. **Save as new
snapshot** is the other half: it writes to the first unused `SNAPSHOT1.SAV`,
`SNAPSHOT2.SAV` and so on, so a snapshot can be taken on a card that has none.

The menu has no text entry, so names are generated rather than typed. That is a
deliberate trade: typing would need a whole on-screen keyboard for something
done once per session, and a generated name is one the user can rename later on
a PC.

**Load snapshot** lists the same directory.

What a snapshot does **not** include is the mounted media. Disk, cartridge and
cassette images are re-mounted from the settings when the emulator starts, so
after loading a snapshot the machine has the right memory and chip state, but if
you have swapped disks since the snapshot was taken, the snapshot's view of
"which disk is in drive 1" is not restored. Mount the same images before loading
a snapshot if it matters.

Snapshots are larger than you might expect — roughly 450 KB for a 320 kB
machine, because the whole of the emulated RAM is stored uncompressed. That is
why the emulator raises the core's snapshot buffer size; see
[Atari800 integration](atari800-integration.md#the-one-change-to-upstream).

---

## Resetting

Three ways to reset, matching what a real machine offers:

| Reset | Where | What it does |
|---|---|---|
| Warm reset | Menu → Snapshots → Warm reset | Presses the emulated Reset button. Programs keep their memory. |
| Cold reset | Menu → Snapshots → Cold reset | Power-cycles the machine. Memory is cleared and the OS restarts. |
| Power-on | Power the Pi off and on | The emulator starts with the settings file, and re-mounts the media |

Inserting a cartridge applies a cold start automatically, because that is what a
real Atari does when you plug one in.

An ordinary warm reset — the Reset button on a real machine — is also available
as **F8** on a USB keyboard.

---

## The settings file

`SD:/ATARI/settings.cfg` is created on first run and rewritten whenever you
change something. It is plain text:

```ini
# Atari XL/XE emulator settings
# Written automatically whenever a setting is changed in the OSD menu.
# It is safe to edit by hand; unknown lines are ignored.

MACHINE=0
RAM_KB=64
VIDEO=0
ARTIFACT=3
SOUND=1
VOLUME=80
STEREO=0
RESOLUTION=0
KEYBOARD=0
NUMPAD_CURSOR=1
DISK1_PATH=USB:/disks/BOUNTYBOB.ATR
DISK1_READONLY=0
DISK2_PATH=
DISK2_READONLY=0
CARTRIDGE=
CASSETTE=
CASSETTE_READONLY=1
USER_DIR=USB:/disks
SYSTEM_DIR=SD:/ATARI
```

| Key | Values |
|---|---|
| `MACHINE` | 0 = XL, 1 = XE, 2 = XEGS |
| `RAM_KB` | 64, 128, 192, 320 (RAMBO), 321 (Compy Shop) |
| `VIDEO` | 0 = PAL, 1 = NTSC |
| `ARTIFACT` | 0 = off, 1 = NTSC old, 2 = NTSC new, 3 = PAL simple, 4 = PAL accurate |
| `SOUND` | 0 or 1 |
| `VOLUME` | 0 to 100 |
| `STEREO` | 0 or 1 — two POKEYs |
| `RESOLUTION` | 0 = 1280x720, 1 = 1920x1080 |
| `KEYBOARD` | 0 = USB, 1 = GPIO |
| `NUMPAD_CURSOR` | 0 or 1 — keypad arrows act as cursor keys |
| `USB_JOYSTICKS` | 0 or 1 — read USB gamepads |
| `GPIO_JOYSTICKS` | 0 or 1 — read the two 9-pin ports on the header |
| `DISK1_PATH` … `DISK4_PATH` | a path, or empty for "no disk in this drive" |
| `DISK1_READONLY` … `DISK4_READONLY` | 0 or 1 |
| `CARTRIDGE` | a path, or empty |
| `CASSETTE` | a path, or empty |
| `CASSETTE_READONLY` | 0 or 1 |
| `USER_DIR` | where the media browser last was, e.g. `USB:/disks` |
| `SYSTEM_DIR` | where the emulator's own files are, e.g. `SD:/ATARI` |

A missing or damaged file is not fatal: the defaults are used, unknown lines are
ignored, and a value that cannot be parsed leaves the default in place. This
means you can leave comments in the file, and a half-written file still yields
everything that was intact.

Paths use a volume prefix: `SD:` for the SD card, `USB:` for the USB drive.

### Ranges worth knowing

- `RAM_KB=256` is **not** accepted, and is not offered in the menu. The
  Atari800 core has no 256 kB RAM configuration — its valid sizes are 64, 128,
  192, 320 RAMBO and 320 Compy Shop. Use 320 for a machine expanded to its
  maximum.
- `VOLUME` is applied at once, but the POKEY mixer latches it when it is
  initialised, so the emulator re-runs the sound setup after a change. That is
  handled for you.

---

## Keyboard shortcuts at a glance

With a USB keyboard (Mode 1):

| USB key | Atari key |
|---|---|
| F5 | Help |
| F6 | Start |
| F7 | Select |
| F8 | Option |
| F9 | Break |
| F10 | Reset (warm) |
| F12 | **OSD menu** |
| Arrows | Atari cursor keys |
| Left Ctrl | CONTROL |
| Left Shift | SHIFT |
| Caps Lock | CAPS |
| Num Lock | INVERSE |

Full tables, including every letter, digit and punctuation key, are in
[Keyboard: USB](keyboard-usb.md). Any of them can be moved to another USB key
with [`usbkeys.cfg`](usbkeys.md).

With an Atari keyboard on GPIO (Mode 2), every key behaves as it does on the
real machine, and **Shift+Help** opens the menu. See
[Keyboard: GPIO](keyboard-gpio.md).

---

## Joysticks

Controllers work with no setup at all: plug in a USB gamepad, or wire a real
9-pin joystick to the GPIO header, and it is read.

There are four controller inputs, matching an Atari 800 — each of its two ports
carried a second controller's lines alongside the first, so four-player games
work. A gamepad's D-pad or left stick gives the four directions, and any of its
action buttons fires.

One thing is worth knowing before choosing a keyboard: **a GPIO keyboard and the
GPIO joystick ports need the same header pins**, so only one of the two can be
used. USB gamepads work either way. See
[Joysticks](joystick.md#the-pin-budget) for the pin budget and the wiring tables.

---

## Performance

The emulator paces itself to the emulated machine's real frame rate: 49.86 Hz
for PAL, 59.92 Hz for NTSC.

> **The frame-rate readout in the menu is the target rate, not a measurement.**
> It shows 49.86 or 59.92 according to the selected video standard, and it
> cannot read low. There is no throughput counter anywhere in the kernel, so the
> menu cannot tell you whether the Pi is keeping up — that has to be judged by
> whether the emulation sounds and plays correctly. A real measurement would need
> a frame-time accumulator in the main loop and a way to display it; it does not
> exist yet.

If it struggles:

- Build the optimised profile for the board (`kernel64-pi4`
  for a Pi 4).
- Turn off the accurate PAL blending (`ARTIFACT=4`) and use simple blending
  (`3`) or none (`0`).
- Use NTSC rather than PAL for software that does not care; it is a slightly
  cheaper frame and many games were written for it.
- Keep the resolution at 1280x720 rather than 1920x1080; that is four times less
  video memory bandwidth.
