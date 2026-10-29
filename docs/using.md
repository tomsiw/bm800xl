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
│ Snapshots            Save / load / erase     │
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

The menu is drawn in the Atari's own character set, the 8x8 glyphs doubled to
16x16 — compiled into the emulator as data, so it looks the same whatever OS
image is loaded. The row spacing and the number of rows are derived from the
font rather than fixed, which is what lets the same code use a larger or smaller
one.

While the menu is open the sound is silenced. Only the output is muted: the
emulated machine keeps running and keeps producing samples, so a tune or a
sound effect comes back in step when the menu closes rather than stuttering
back to life.

### What is in the menu

**Disk drives** — Load an image into each of the four drives, and into the
cassette and the cartridge. Selecting a drive opens a file browser. While a
drive with an image is selected, Left/Right toggles its read-only flag, shown as
`[ro]`. **Delete** ejects whatever is selected.

**Cartridge** — Load a cartridge image (CAR or ROM).

**Joysticks** — Which interface feeds the two Atari joystick ports: **USB
gamepads**, the two **9-pin ports** on the GPIO header, or keys on the **USB
keyboard** (bound in `usbkeys.cfg` as `Joy1_Up` and so on). One at a time, never
more, because they would be competing for the same two ports -- and the 9-pin
ports need the header pins the GPIO keyboard uses, while the keyboard interface
needs the USB keyboard. The second row, **Swap ports**,
exchanges the two: gamepad 1 then drives joystick 2 and gamepad 2 drives joystick
1, or the same for the two 9-pin ports. Swapping applies at once; changing the
interface needs the Pi switched off and on, because the controllers are created
before the emulator starts, and the menu says so when you change it.

**Machine and memory** — The machine type (XL, XE, XE Game System), the RAM size,
the video standard, and a "restore defaults" item that resets the machine
settings without losing your mounted media.

**Display** — PAL or NTSC, the artifacting mode, the output resolution, and the
aspect ratio
(1280x720 or 1920x1080). Changing the resolution stores the new value but cannot
take effect until the Raspberry Pi is switched off and on again, because the
framebuffer is created before the emulator is: the menu says so, and the next time
the board starts it comes up in the mode you chose.

That is a restart of the **Pi**, not of the emulated machine — the menu's *Power
cycle* item rebuilds the emulated Atari and leaves the framebuffer alone, so it
does not make a resolution change take effect. The tape-mode notice works the
other way round: it is satisfied by that same *Power cycle* item.

**Sound** — On or off, volume in 5% steps, and stereo (two POKEYs).

**Snapshots** — Save, load or erase a machine snapshot. Warm reset, cold reset and
power cycle are separate items on the main screen.

`Aspect ratio` is worth its own note. **`4:3`** is the picture as a
television showed it: 336x240 fitted to the screen height, every pixel
stretched a little unevenly to 4:3. **`2x`** and **`3x`** scale the original
resolution by an integer instead -- 672x480 and 1008x720, every Atari pixel a
square block of two or three -- so the picture is pixel-exact but smaller, and
sits in a border. `4x` -- 1344x960 -- is offered only when the display
can hold it, which in practice means 1080p. The integer modes are also cheaper to draw, which is what
makes `3x` a good choice at 1080p on a slow board: it is 1008x720 whatever the
output resolution is.

Changing it applies at once; a factor the framebuffer cannot hold is reduced
until it fits and the menu says which one it used.

### Changing something takes effect immediately

There is no "apply" step. Changing the volume, the video standard, the sound
switch or the artifacting applies at once; changing the RAM size or the machine
type rebuilds and resets the emulated machine, which is what those changes
inherently require; and toggling a disk's `[ro]` flag re-opens that drive so the
new mode reaches the machine.

One thing is the exception, and the menu says so when you change it:

- **Switching sound back on** reopens the audio device; if that fails the menu
  reports it.

### On a slower board: `STATIC_OSD`

The menu normally repaints the emulated picture behind it and sits on a dimmed copy
of it, so you can see the machine carrying on while the OSD is open. Repainting a
960x720 picture with the menu on top does not fit in one 20 ms frame on a Pi Zero
2 W, and the machine misses its frame deadline — games that depend on 50 Hz timing
can crash.

Set `STATIC_OSD=1` in `settings.cfg` and the menu is drawn over the frame that is
already on screen, on an opaque panel instead — the picture underneath stops
updating while the menu is open, and closing the menu repaints as usual. That is one
line in the settings file, not a menu item: it describes the board rather than the
session. The default, `0`, is the dimmed-picture behaviour.

The picture *outside* the menu is no longer the reason to need this: the fill was
made about four times faster in `e2773e4`, from 18 ms to 4.8 ms on a Zero 2 W, so
a game frame now fits at both PAL and NTSC. `STATIC_OSD` still saves the OSD's own
redraw cost on a slow board, and it is the difference between a menu that opens
smoothly and one that does not.

### Settings are saved as you go

Every change is written to `SD:/ATARI/settings.cfg` immediately, so the emulator
comes back the way you left it even if the power is cut. The file is plain text
and is meant to be readable and editable; see [the settings file](#the-settings-file).

---

## Loading media

Media images live on the **USB drive** by default. Disk, cartridge and cassette
images are all loaded through the menu:

1. Open the menu with F12.
2. Choose **Disk drives**, then the drive you want.
3. The file browser opens, listing the files that match (ATR and XFD for disks,
   CAR and ROM for cartridges, CAS for cassettes).
4. Directories are shown with a trailing `/`; press Return to enter one, and
   Escape to go back up.
5. Press Return on a file to load it.

### Keeping media on the SD card instead

The volume is a setting, not a build option. `USER_DIR` in `settings.cfg` names
it, and the volume part is what matters:

| `USER_DIR` | Where media live | What the browser can reach |
|---|---|---|
| `USB:` or `USB:/disks` | the USB drive | the whole USB drive |
| `SD:` or `SD:/USERDATA` | the SD card | **`SD:/USERDATA` only** |
| `SD:/USERDATA/GAMES` | the SD card | `SD:/USERDATA/GAMES` downward |

With `SD:`, the directory `SD:/USERDATA` is created if it is missing, the browser
opens inside it, and Escape from it leaves the browser rather than climbing into
the rest of the card — the settings file, snapshots and ROM images there are the
emulator's, not the collection. Editing the setting is what switches volumes: set
it back to `USB:` to return to the USB drive. It is remembered across restarts,
because the emulator writes the directory it was browsing into the same key, and
that directory is always on the volume in use.

Switching volumes does not move any files, and the paths of the images that were
loaded last time are kept as they were. An image on the volume that is no longer
there cannot be mounted at start-up: the log says `Cannot mount <path> in drive
<n>`, and the machine comes up with that drive empty while the menu still shows
the path it was given. Load an image from the current volume to replace it.

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
  its menu entry. It then appears as `[ro]`, and the drive is re-opened read-only
  **at once** — the machine sees the new mode without the disk being re-inserted.
- The flag belongs to the drive, not to the image: ejecting a disk keeps it, so
  re-inserting the same image gives the same mode, and inserting a different one
  starts the way the drive was left. The row shows it either way.
- A cassette image is opened read/write by default, like a disk. Use Left/Right
  on its entry to protect it; the row then says `[ro]`, and the tape keeps its
  position when the protection changes.

That menu entry is the only thing that decides. An ATR image can carry a
write-protect flag of its own, in the last byte of its 16-byte header, and many
images in circulation have it set; it is ignored here, so a disk mounted without
`[ro]` is writable even if the image asks not to be. What is *not* ignored is the
file system: if the image file is marked read-only, or the card is locked, the
drive comes up read-only instead, and the log says
`disk <n>: <path> cannot be opened for writing`. `disk <n>: mounted <path>
(read/write)` or `(read only)` in the log is what the drive actually is.

Writing to a real FAT file system from a bare-metal program is the part of this
emulator most likely to surprise you if something goes wrong. Keep a copy of
anything irreplaceable.

### Standard and turbo tapes

Cassette images come in two kinds, and they need different machinery inside the
emulated machine:

- a **standard** tape (600 baud, recorded by an Atari) is loaded through a patched
  OS routine that copies whole records at once, so it loads almost instantly;
- a **turbo** tape (any of the popular fast loaders) holds raw tape signal, which
  only real serial timing can read. That mode turns the patch off, so loading runs
  at tape speed — and disk input/output is slower too while the machine is built
  that way.

The type is read from the image itself when it is inserted, and the machine has to
be rebuilt to switch between the two, because that is when the OS image is patched.
So if you insert a tape that needs the other mode, the menu says:

```
Turbo tape - power cycle required
Standard tape - power cycle required
```

Leaving the menu — F12, Escape on the main screen, or any of *Cold reset*, *Warm
reset* and *Close menu* — performs that power cycle instead of the action you
picked, and the tape is still in the drive afterwards. Be aware of what that means:
**a power cycle clears the machine's memory**, so an unsaved program is lost at
that moment. That is the price of having the tape loadable at all; a cold reset
cannot do it, because the OS patches are installed when the machine is built.

A tape named in the settings file is classified before the machine starts, so the
first boot already has the right mode and no cycle is needed.

A standard tape in a machine built for turbo loading still works, just slowly;
ejecting a turbo tape keeps the machine in real-timing mode until the next power
cycle.

One consequence of the patched OS is worth knowing: **the built-in SELF TEST
reports a ROM error while the fast-load patch is active.** The patch works by
overwriting a few bytes of the OS ROM image in memory, which is precisely what the
self-test's ROM check is looking for. Real-timing mode — any turbo tape, power
cycle — presents an unmodified ROM, and the test passes. See
[SELF TEST reports a ROM error](troubleshooting.md#self-test-reports-a-rom-error).

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

**Erase snapshot** is a screen of its own, and the only place a snapshot can be
removed: the Save and Load browsers never delete anything, so a file cannot be
lost from a screen you opened to load it. Pick *Erase snapshot*, highlight the
file, and press **Return** (or **Delete** — both mean "this one" here). The first
press asks (`Erase SNAPSHOT3.SAV? Press again to confirm`); the second removes
it. Anything else cancels, and the question disappears with the key that
cancelled it: moving the selection, going up a directory, choosing another file,
or letting the question time out. A press much later in the same place therefore
starts the question again rather than removing something you had forgotten
choosing.

Erasing removes the file from the card, so it cannot be undone from the menu.
Snapshot names are `SNAPSHOTn.SAV` in the system directory (`SD:/ATARI`), which
is one thing to check on a PC if space runs short.

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

Four ways, and they are not interchangeable:

| Action | Where | What it does |
|---|---|---|
| Warm reset | Menu → Warm reset | Presses the emulated Reset button. Programs keep their memory. |
| Cold reset | Menu → Cold reset | The CPU's RESET line: PIA, ANTIC and the CPU are reset and the OS cold-starts. RAM keeps its contents, and POKEY and GTIA keep their registers — on a real machine neither chip has a reset pin. |
| Power cycle | Menu → Power cycle | A new machine: RAM cleared, every chip at its power-up state, the ROMs loaded again, and the media mounted again from the settings. |
| Power-on | Power the Pi off and on | The emulator starts with the settings file, and re-mounts the media |

Cold reset is enough for most things. It is not enough when the software that was
running left a chip in a state the OS reads while booting. GTIA is the usual
culprit: a program that masks a console key out of it leaves the machine believing
that key is held, so a cold reset boots as though OPTION were down — BASIC is
skipped, and with nothing in the drive the boot ends in BOOT ERROR. Power cycle
clears that, because it builds the machine again rather than resetting it.

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
ASPECT=4:3
KEYBOARD=0
NUMPAD_CURSOR=1
CONTINUE_ON_BRK=1
STATIC_OSD=0
JOY_INTERFACE=USB
JOY_SWAP=0
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
| `RESOLUTION` | 0 = 1280x720, 1 = 1920x1080 — the same setting as the menu's Display → Resolution; it takes effect the next time the Pi is switched off and on |
| `ASPECT` | `4:3`, `2X`, `3X` or `4X` -- the same setting as the menu's Display -> Aspect ratio, applied at once; `4X` needs a display that can hold 1344x960, so in practice 1080p |
| `KEYBOARD` | 0 = USB, 1 = GPIO |
| `NUMPAD_CURSOR` | 0 or 1 — keypad arrows act as cursor keys |
| `CONTINUE_ON_BRK` | 0 or 1 — 1 (the default) runs BRK as the machine does; 0 stops the frame and logs it |
| `STATIC_OSD` | 0 or 1 — 1 draws the menu over a frozen picture on an opaque panel; 0 (the default) repaints and dims the picture behind it |
| `JOY_INTERFACE` | `USB`, `GPIO` or `KEYBOARD` — which interface feeds the two joystick ports; the same setting as the menu's Joysticks → Interface, and it takes effect the next time the Pi is switched off and on |
| `JOY_SWAP` | 0 or 1 — 1 exchanges the two joystick ports, and takes effect immediately |
| `DISK1_PATH` … `DISK4_PATH` | a path, or empty for "no disk in this drive" |
| `DISK1_READONLY` … `DISK4_READONLY` | 0 or 1 |
| `CARTRIDGE` | a path, or empty |
| `CASSETTE` | a path, or empty |
| `CASSETTE_READONLY` | 0 or 1 |
| `USER_DIR` | the volume media live on, and where the browser last was: `USB:/disks`, or `SD:/USERDATA` for the SD card |
| `SYSTEM_DIR` | where the emulator's own files are, e.g. `SD:/ATARI` |

A missing or damaged file is not fatal: the defaults are used, unknown lines are
ignored, and a value that cannot be parsed leaves the default in place. This
means you can leave comments in the file, and a half-written file still yields
everything that was intact.

One consequence of that rule is worth spelling out, because it affects anyone
upgrading: `USB_JOYSTICKS` and `GPIO_JOYSTICKS` — the two switches this version
replaced with `JOY_INTERFACE` — are **no longer read at all**. An older file still
carrying both, set to 1, therefore leaves the interface at its default, which is
**USB gamepads**. That is deliberate: honouring the old pair would put existing
users onto the GPIO interface, and the two 9-pin ports have never been the ones
being used.

### `CONTINUE_ON_BRK`

`CONTINUE_ON_BRK=1`, the default, is what the machine does: BRK pushes the return
address, sets the I flag and jumps through the vector at `$FFFE`. Programs that
use BRK deliberately depend on that — MAC/65's **DDT** is the classic example,
where BRK is how the debugger regains control at a breakpoint.

`CONTINUE_ON_BRK=0` is a diagnostic. The emulation core is a library that can be
built to treat BRK as "the program has ended", and in that mode a frame that
executes one is abandoned and the log says:

```
Frame error: the emulated machine executed a BRK instruction
```

Use it when a crashing program ends somewhere you cannot explain and you want
the first BRK reported. It is not in the OSD menu, because it is a diagnostic
rather than something to change while playing. Like every setting in this file,
it is read at start-up.

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

There are **two** controller inputs, because an XL/XE has two ports: `STICK(0)`
and `STICK(1)`. A gamepad's D-pad or left stick gives the four directions, and any
of its action buttons fires. Which interface is in use — two USB gamepads or the
two 9-pin ports — and whether the two ports are swapped, is chosen in the menu:
**F12 → Joysticks**.

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
> cannot read low. The **measured** rate is in the runtime log instead:
> `SD:/ATARI/runtimelog.txt` carries a `stats` line every 30 seconds with
> the achieved frame rate and frame count, and — only when there were any — the
> number of pictures that were skipped because a frame did not fit. See
> [Getting diagnostic information](troubleshooting.md#getting-diagnostic-information).

A low `stats` figure means the emulation itself is being starved. A normal frame
rate with a `skipped` count means only the display is struggling: the emulated
machine keeps its 50 or 60 frames a second and the picture updates less often.

If it struggles:

- Build the optimised profile for the board (`kernel64-pi4`
  for a Pi 4).
- Turn off the accurate PAL blending (`ARTIFACT=4`) and use simple blending
  (`3`) or none (`0`).
- Use NTSC rather than PAL for software that does not care; it is a slightly
  cheaper frame and many games were written for it.
- Keep the resolution at 1280x720 rather than 1920x1080. The picture is drawn in
  the framebuffer, so the cost is proportional to its area: 1080p is 2.25 times
  the pixels of 720p, and on a Pi Zero 2 W that difference is the whole frame
  budget — the picture fill alone goes from about 4.8 ms to about 10.8 ms, and
  the adaptive skip starts leaving every second picture undrawn.
- On a slow board, set `STATIC_OSD=1` so that opening the menu does not have to
  repaint the picture behind it.
