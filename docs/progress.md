# Project progress

A bare-metal Atari XL/XE emulator for the Raspberry Pi, cross-compiled on Windows.
Circle provides the hardware layer; Atari800 provides the emulation core; this
project is the seam between them.

This document records where the project stands, what has been established by
measurement rather than assumption, and what to do next. It is written to be read
cold, without the history that produced it.

**Last updated:** after commit `7713607`, with the USB keyboard remapping and the
BRK handling both implemented and verified on hardware.

---

## 1. Status at a glance

| Area | State |
|---|---|
| Video | 1280x720, picture 960x720, correct 4:3 with black side bars |
| Frame rate, Pi 400 | **49.8 fps** against a 50 fps target |
| Frame rate, Pi Zero 2 W | **49.7 fps** — the primary target, verified |
| Sound | correct over HDMI; volume applies live; stereo needs a restart |
| Disk / cartridge / cassette | loading verified; eMMC faults fixed |
| Keyboard | USB verified direct; `usbkeys.cfg` remapping verified; **hub unsupported on Zero 2 W** |
| Snapshots | save, save-as-new, load and delete **verified on hardware** |
| OSD menu | opens, navigates, applies settings; no flicker |
| Boot to first frame | ~6.6 s on Zero 2 W, ~7 s on Pi 400 |
| USB gamepads | resolver, hot plug and both pad kinds **verified on hardware** — see section 8 |
| Pi 3 / Pi 5 | images build; never run |

---

## 2. Architecture

```
src/
  kernel.cpp/.h        CAtariKernel: boot stages, frame loop, settings apply
  bootlog.cpp/.h       buffered start-up log, written to SD once mounted
  main.cpp             entry point
  audio/audio_out.cpp  CAtariAudioDevice over CHDMISoundBaseDevice
  video/video_out.cpp  framebuffer, scaler, OSD drawing primitives
  input/               usb_keyboard, usb_keymap, gpio_keyboard, usb_joystick, gpio_joystick
  osd/osd_menu.cpp     the F12 menu
  port/                the C seam: platform.c, sound_bridge.c, vfs.c, atari_port.h
  storage/             FatFs volumes ("SD:", "USB:")
external/
  circle/              unmodified
  atari800/            one permitted patch: STATESAV_MAX_SIZE
probe/gamepad/         standalone USB gamepad probe (section 8)
Makefile.probe         builds the probe; links Circle + SDCard + FatFs only
```

Build profiles (`scripts/build.ps1`):

| Profile | Image | Target |
|---|---|---|
| `kernel64` | `kernel8.img` | **Pi Zero 2 W**, Pi 3, Pi 4 (RASPPI=3) |
| `kernel64-pi4` | `kernel8-rpi4.img` | Pi 4 / 400, tuned for Cortex-A72 |
| `kernel64-pi5` | `kernel_2712.img` | Pi 5 |

```
pwsh -ExecutionPolicy Bypass -File scripts\build.ps1 -Profile kernel64-pi4
```

Switching profiles rebuilds Circle for that RASPPI; the script enforces it.

The probe:

```
make -f Makefile.probe AARCH=64 RASPPI=4 PREFIX=aarch64-none-elf- \
     CIRCLEHOME=<abs>/external/circle OUT=<abs>/build/probe
```

---

## 3. Measured results

### Frame cost, Pi 400 (the measurement that drove the video work)

Before:

```
emulate 1530 us, fill 31148 us, menu 0, present 261, total 32940   (~30 fps)
```

After the renderer fix:

```
emulate 14454 us, fill 5547 us, menu 0, present 0, total 20005     (49.8 fps)
```

`emulate` here includes the frame pacer's idle wait; the true emulation cost is
about **1.5 ms**.

### Boot breakdown, Zero 2 W

```
4079 ms  firmware + Circle's init   (of which Circle's own init is 401 ms)
+~600 ms SD, mounts, settings
+~120 ms framebuffer, sound, core
+~370 ms first frame
= ~6.6 s to the first picture
```

`config.txt` trimming outside this repository (initial_turbo, arm_boost, gpu_mem,
fewer overlays) recovered about **1.5 s** of firmware time on the Pi 400.

`boot timing:` is logged on every boot by reading `CNTPCT_EL0`/`CNTFRQ_EL0`
directly, because Circle's `CTimer` does not yet exist when the interesting part
happens.

### Audio

```
audio: opened 44100 Hz, 1 channel(s), 2-byte samples, 885 frames per video frame
```

885 = `ceil(44100 / 49.86)`: one video frame of audio, which is what
`PLATFORM_SoundAvailable()` must report.

---

## 4. Defects found and fixed

Each entry is a root cause, not a change list. Several cost days and are recorded
here so they are not rediscovered.

| Commit | Defect | Root cause |
|---|---|---|
| `579c092` | 30 fps; OSD flicker | `FillScreen` and `DimRect` wrote the framebuffer one pixel at a time. The framebuffer is not cacheable from the ARM, so every store was a bus transaction: 31 ms per frame in the scaler, 69 ms in the menu dim. Fixed by staging each row in cached memory and copying with `memcpy`. |
| `1bb226e` | sound distorted beyond recognition | `CAtariAudioDevice` overrode `GetChunk()`, which bypasses `GetChunkInternal()` — the only place IEC958 subframe framing (control bits, parity, preamble) is inserted. The DMA was fed raw PCM where S/PDIF was expected. Fixed by removing the override and using Circle's queue via `Write()`. |
| `72b2004` | dead code | the ring buffer became unused; removed |
| `f4a3323` | sound late after using the OSD | the pacer repaid lateness by running frames back to back (only giving up past 200 ms). Each catch-up frame generated 20 ms of audio in no wall-clock time, filling the output queue permanently. Fixed by resynchronising whenever late. |
| `f4a3323` | cartridge restarted on closing the OSD | `ATARI_MountCartridge()` always defers a cold start; the boot-time mount set the flag and nothing cleared it, so the first menu close reset the machine. |
| `a585699` | text console at start-up | removed; it allocated its own framebuffer and was replaced by the emulator's |
| `a585699` | `stereo change deferred (0 -> 1 channels)` on every boot | compared against a static that was only written on the path where this project opens the sink; the core opens it first. Now reads `Sound_out.channels`. |
| `1ed3886` | volume changes did nothing until sound was toggled | `POKEYSND_SetVolume()` only sets a variable; the mixer copies it into gain tables at init (`mzpokeysnd.c`). Fixed by calling `POKEYSND_DoInit()` — no device reopen, which matters because reopening the HDMI sink hangs. |
| `dc94dd0` | Zero 2 W: black screen, no log | `CTimer::GetClockTicks()` used as a clock *frequency* in the keyboard wait, so the wait never expired. Masked on the Pi 400 because its keyboard is built in. |
| `dc94dd0` | Zero 2 W: black screen with a USB hub | USB enumeration was driven during start-up; a device failing its descriptor read was retried forever. Start-up now only scans; enumeration is left to the frame loop. |
| `7b8153a` | a controller plugged in later was never seen; the Switch Pro never appeared at all | `UpdatePlugAndPlay()` was **unreachable**: the only call was in `CUSBAtariKeyboard::Update()`, and nothing ever called `Update()`. So port-status events were never drained, and a failed enumeration was never retried. |
| `aed2e43` | generic pads reported a constant wrong `STICK`; every hat direction read as "up" | the resolver was still only in the probe: the adapter used fixed bits 15–18 and axis trim, and read the hat as degrees when Circle reports the raw HID value 0–7. |
| `7713607` | MAC/65's DDT could not take control: the emulator stopped at its BRK | the core's `LIBATARI800` build treats BRK as "the program ended" and longjmps out of the frame unless `libatari800_continue_on_brk` is set. The instruction is abandoned after its opcode fetch, so the push and the vector never happen. Now `CONTINUE_ON_BRK` in the settings file, default 1, which runs BRK as the machine does. |
| `(working tree)` | the docs say Delete ejects an image, and it never did | the menu only receives the keys the keyboard drivers classify as menu keys, and neither list contained Delete: `MenuKeyForUsage()` on USB and `MenuKeyForPosition()` on GPIO both stopped at the arrows, Return, Escape and Space. The menu's handler for it was unreachable, in the same way `UpdatePlugAndPlay()` was. Delete is now classified on USB, and the GPIO driver maps BACK SPACE to it, because the Atari's Delete is CTRL+BACK SPACE and the menu cannot see modifiers. |
| `(working tree)` | no way to remove an old snapshot | the Snapshots screen could write and read files but never erase one. Erasing now has a screen of its own, *Erase snapshot*, reached like the others: the save and load browsers were the wrong place to put it, because a file could then be lost in a screen opened for another purpose. Return or Delete asks, a second press erases, and any other key cancels **and takes the question off the screen** — leaving it there made a cancelled action look pending. |

---

## 5. Hard-won environment facts

These are properties of Circle and of this project that repeatedly caused silent
failures. They are listed because they are invisible until they bite.

1. **`CLogger` filters below its level before any target.**
   `if (Severity > m_nLogLevel) return;` is the first line of `Write()`. This
   kernel's logger runs at `LogWarning`, so **every `LogNotice` is discarded**.
   Three rounds of joystick instrumentation produced nothing for this reason.
   Diagnostic logs must be `LogWarning` or above.

2. **`CLogger::Initialize()` must be called after `CKernel::Initialize()`**, or the
   target is overwritten.

3. **Never write to FatFs from an interrupt.** It is not reentrant and performs
   disk I/O. Doing so stalls the USB report handler, and once produced
   `emmc: Error occured whilst waiting for transfer complete interrupt`. Record in
   the interrupt, print from the main loop.

4. **`CString::Compare` is not `strcmp`.** For an object with no buffer it returns
   `*pString == '\0'` — a *boolean*. Comparing an empty string therefore reports
   "equal" to everything. Test `GetLength() > 0` first.

5. **Circle passes `m_nDeviceNumber - 1` to gamepad handlers**, so the index is
   **0-based**. A guard of `>= 1` silently discards every call. This bug existed in
   both the probe and the emulator.

6. **`DecodeReport()` only runs when a status handler is registered**
   (`usbgamepad.cpp: ReportHandler`). Registering the handler is what makes the
   state live; polling `GetInitialState()` without it returns a frozen state.

7. **`CUSBGamePadDevice` is a `CUSBFunction`**, not the device. The real USB
   vendor/product IDs come from `GetDevice()->GetName(DeviceNameVendor)`; calling
   `GetName()` on the function itself returns uninitialised storage — it reported
   `ven1b9-b000` for three different pads.

8. **The driver is chosen by `CUSBDeviceFactory`, a static table with no hook.**
   Reproducing its table (device IDs → driver) is the only way to know which driver
   is in use. Anything not in the table falls through to
   `CUSBGamePadStandardDevice`, which assigns button bits in **HID descriptor
   order** — so for those pads the button names mean nothing and a per-device
   mapping is required.

9. **The framebuffer is not cacheable.** Any per-pixel drawing there is a
   performance trap (see section 4).

10. **A USB hub is not enumerated on the Zero 2 W.** Confirmed with Circle's own
    `sample/08-usbkeyboard`, unmodified. Connect peripherals directly.

11. **`UpdatePlugAndPlay()` has to be called, from task level, once per frame.**
    It is the only thing that drains the port-status events Circle's USB interrupt
    queues, and so the only thing that enumerates a device attached after
    start-up. It is also the only thing that runs the failed-enumeration retry.
    A driver whose `Configure()` fails — the Switch Pro's does, if its handshake
    with the pad fails — leaves the root port disabled, and without this call it
    stays disabled for the session. Calling it is cheap when nothing has
    happened; connecting a device can cost a few frames, which is the intended
    trade.

    This was missed for a long time because the call was not absent but
    *unreachable*: `CUSBAtariKeyboard::Update()` made it, and nothing ever called
    `Update()`. A dead method looks like a wired one until a device that needs it
    fails to appear, silently.

12. **`libatari800` treats BRK as the end of the program, not as an instruction.**
    In a `LIBATARI800` build with `HAVE_SETJMP`, the core's 6502 longjmps out of
    the frame on BRK unless `libatari800_continue_on_brk` is set (`cpu.c:904-908`),
    and the jump happens *after* the opcode fetch: the return address is never
    pushed, the I flag is not set, and the vector at `$FFFE` is not taken. A
    program that installs a BRK handler — a debugger is the usual one — is
    therefore broken by the default, silently apart from a frame error in the log.
    The flag is `CONTINUE_ON_BRK` in this project's settings file, default 1.
    The other two `longjmp`s in the core (illegal CIM opcodes, invalid ESC)
    remain unconditional and are what the crash containment still catches.

13. **The menu only ever sees the keys a keyboard driver classifies as menu
    keys.** `ConsumeMenuKey()` is the only route into `CAtariMenu::HandleKey()`,
    and each driver's mapping list is the filter: `MenuKeyForUsage()` for USB,
    `MenuKeyForPosition()` for GPIO. A case in the menu for a key that is not in
    those lists is dead code that looks live, which is how "Delete ejects the
    image" stayed broken and unnoticed. When adding a menu command, add the key
    to both drivers' lists or it will never arrive.

---

## 6. Known limitations

- **Stereo needs a restart.** The channel count is a property of the open device,
  and reopening the HDMI sink hangs on this hardware. Reported honestly in the log.
- **The OSD redraw still costs ~18–30 ms** when it happens. Mitigated by redrawing
  every 10th frame while the menu is open, and at most every 4th frame while keys
  are held; emulation keeps running at full speed throughout.
- **`frame cost` logging is always on**, every 300 frames. Should be gated before
  release.
- **The `stereo change deferred` message** is now accurate, but the underlying
  inability remains.
- **Pi 3 and Pi 5 unverified.** The Pi 5 image has a page-flip branch that has
  never executed.

---

## 7. Testing on hardware

Everything below was verified on a **Pi 400** (720p, HDMI, monitor with speakers)
and a **Pi Zero 2 W** (720p, monitor without audio).

Verified: boot to BASIC; disk, cartridge and cassette mounting; settings applied at
startup and persisted; crash marker recovery; menu navigation; sound; 49.7/49.8 fps;
USB keyboard direct, and hot-plugged; USB gamepads — both a generic pad and a Switch
Pro — attached before power-on and while running, with unplug, re-plug and swap;
snapshots — save, save as new, load, and delete from the browser.

Not verified: GPIO keyboard, 9-pin GPIO joysticks, the second keyboard mode, the
128/192/320/321 kB RAM configurations in use, cassette beyond insertion, Pi 3/Pi 5.

BASIC's `STICK(n)` / `STRIG(n)` are the cheapest joystick test:
`15` = centred, `1` = no fire.

---

## 8. USB gamepads — where this stands

### The probe

`probe/gamepad/` is Circle's `sample/27-usbgamepad` with two additions:

- Circle's logger is teed to the screen **and** `GAMEPAD.LOG` on the SD card. The
  card is mounted and the tee installed *before* USB starts, so enumeration
  messages (IDs, product names, transfer errors) are captured.
- a report identical to the previous one is not logged, so only transitions appear.

Circle timestamps its own output; no timestamping is done by the probe.

It also carries a **resolver**, `ResolveMapping(pId, pState)`, which returns a
`TMapping` of `TDirection` records saying *where* each Atari control comes from —
a button bit, an axis with a threshold and side, or a hat.

### Controllers measured

| Device | Directions from | Fire |
|---|---|---|
| `ven57e-2009` Nintendo Pro (Circle's Switch Pro driver) | button bits: UP `0x8000`, DOWN `0x20000`, LEFT `0x40000`, RIGHT `0x10000` | A `0x200`, B `0x100` |
| `ven79-11` generic | axes: left/right on index 3, up/down on index 4 (0 = left/up, 255 = right/down) | bits `0x2` and `0x4` |
| `ven79-6` Microntek | axes: left/right on index 0, up/down on index 1 | bits `0x2` and `0x4` |

All three verified in the probe, pressing every direction and fire.

On a generic pad the bit numbers are positions in the HID descriptor, not named
buttons, so `0x2` and `0x4` are the pad's second and third declared buttons —
they are spelled as bits, not button numbers, because a button number would
invite the assumption that it means something on the next controller.

Two things that were necessary and are easy to lose:

- a **specific device entry must precede the generic hat rule**, because the
  Microntek reports one hat it never uses for direction;
- the event line originally printed only the first four axes, and the pad under
  test carried up/down on the **fifth**, which made the direction look absent.

### The emulator adapter

`src/input/usb_joystick.cpp` carries the resolver: `TDirection`, `TMapping`,
`ResolveMapping()` and `IsActive()` are the probe's, with two additions to
`TMapping` — whether the left stick steers, and which further button bits mean
fire.

The map is resolved **once**, in `Attach()`, when a controller is first found,
keyed by `m_pDevice->GetDevice()->GetName (DeviceNameVendor)` and by the counts
in `GetInitialState()`. `GetDirections()` and `GetTrigger()` then only ask
`IsActive()`; the map is cleared when the controller is unplugged, so a different
pad plugged into the same slot is resolved again.

The known-ID list is **Circle's own factory table**, not the probe's five
entries. The probe's list had a typo — `ven45e-2f8`, which is not a Circle ID —
and it omitted the PS4, the later Xbox revisions, the wireless receiver and the
8BitDo pads, all of which Circle decodes into the same `TGamePadButton` bits.
Keeping the probe's list would have made those pads *worse* than before the port:
fire is then bit `0x2` of a descriptor, which is not Cross or A, and an Xbox pad
with no hat would have gone looking for its directions on whatever axes it
reports.

Four things changed with the port:

- **The hat was being read as an angle in degrees.** Circle reports the raw HID
  value: 0..7 clockwise from up, the odd values being diagonals, and an
  out-of-range value meaning centred (a standard pad reports its logical maximum,
  a DualShock 4 reports 8). The old `nAngle / 45` turned every one of 0..7 into
  0, so **every hat direction read as "up"** — a large part of the constant wrong
  `STICK` this work started from. A direction now carries a mask over the hat's
  values, so a diagonal pushes both directions.
- **The axes rule now requires five axes**, because it reads index 4. The probe
  tested for four and then used the fifth, which on a four-axis pad produced a
  map in which up and down could never activate.
- **Per-report logging is gone.** The handler copies the report and nothing else.
  The mapping is logged once, at `LogWarning` (this kernel's logger discards
  `LogNotice`), from the main loop. An unplugged controller is reported from
  `IsPresent()` for the same reason: the removed-handler runs in a context this
  driver does not choose, and writing to FatFs there is what stalls USB.
- **A recognised pad keeps its wider fire set and its left stick.** Both depend on
  the bit and axis numbers being the ones Circle's enum defines, which is true
  only for the pads matched by vendor/product ID. An unrecognised pad gets the two
  buttons that were measured, so an arbitrary descriptor bit cannot fire by
  accident.

**Verified on a Pi 400 and a Pi Zero 2 W** with the generic ven79-11 and the
Switch Pro ven57e-2009: `STICK(0)`/`STRIG(0)` read `15`/`1` at rest, all four
directions and the diagonals steer, fire works, and both pads attach, detach and
swap while the machine runs. Keyboard swapping was confirmed at the same time.

### Hot plug never worked, and the Switch Pro was never retried

The port was not the whole story. `CUSBHCIDevice::UpdatePlugAndPlay()` was called
from **nowhere**: `CUSBAtariKeyboard::Update()` contained the call, and that
method was never invoked by anything. Circle queues a port-status event in its
interrupt and enumerates a device only when the event is taken at task level, so
with no call:

- a controller plugged in after start-up was never seen at all — the adapters
  re-scan the name service every frame, but nothing ever registered a new
  device, so they found nothing to attach;
- a device whose configuration failed at power-on was never retried. The Switch
  Pro controller is exactly that case: its driver exchanges a handshake with the
  pad while configuring, and a single failure leaves the root port disabled for
  the whole session. That is why it works in the probe, which calls
  `UpdatePlugAndPlay()` in its loop, and did not work in the emulator, which did
  not call it anywhere.

`RunFrame()` now calls it once per frame, before anything reads the controllers,
and reports `USB device tree changed` when it did something. Keyboard scanning
moved out of that path: enumeration belongs to the kernel and must happen whether
the USB or the GPIO keyboard is selected.

### What the log says about a controller

A controller is named when it is attached and when it is removed, and nothing is
written while it is being used:

```
usbjoy: USB gamepad 1 attached (ven57e-2009): 21 buttons, 0 hats, 4 axes, map buttons (Circle driver)
usbjoy: USB gamepad 1 removed
```

This is what separates "the pad is not attached" from "it is attached and the
mapping is wrong": the attach line names the ID, what the pad reports and the rule
that was chosen. Until it existed, a controller that never enumerated produced no
log line whatsoever.

Per-press direction and fire lines — with the raw button mask, throttled to one
line per 100 ms — existed while the mapping was being brought up and were removed
once every pad had been measured. One line per press is a card write per press in
the frame loop, and the probe remains the tool for that kind of detail.

---

## 9. What to do next, in order

### 1. Port the resolver into the emulator  *(done and verified — `aed2e43`)*

1. Move `TDirection` / `TMapping` / `ResolveMapping()` / `IsActive()` from
   `probe/gamepad/kernel.cpp` into `src/input/usb_joystick.cpp`. They are
   self-contained and proven.
2. Resolve **once**, in `IsPresent()` when the device is first found, keyed by
   `m_pDevice->GetDevice()->GetName (DeviceNameVendor)`. Store the `TMapping` in
   the adapter.
3. Replace the fixed paths in `GetDirections()` and `GetTrigger()` with
   `IsActive (m_Map.Up, …)` etc.
4. Remove the per-report logging in `StatusHandler` — it writes to the SD card from
   the USB interrupt and produced the `emmc` transfer error.
5. Test with `STICK(0)`/`STRIG(0)` on all three controllers, including
   **hot plug** — attach and detach each pad while the machine runs — and the
   Switch Pro, which is the one that never appeared before.

All five are done. The resolver is `aed2e43`; hot plug and the Switch Pro needed
`7b8153a`, which drives `UpdatePlugAndPlay()` from the frame loop — see section 8.
Verified on hardware with the generic pad and the Switch Pro, plugged in both
before and during a session.

### 2. Housekeeping

- Gate the `frame cost` logging behind a build flag.
- Nothing uncommitted: the gamepad work is `aed2e43`, `7b8153a` and `92d4282`.

### 3. USB keyboard remapping  *(implemented and verified — `5498dc7`)*

`SD:/ATARI/usbkeys.cfg` remaps the USB keyboard; [docs/usbkeys.md](usbkeys.md) is
the description and the contract. The unit of a mapping is the **function**, not
the key: the Atari's `*` key has three (`*`, `^` with Shift, cursor right with
Control), and each has its own binding, so moving one leaves the other two where
they are. A stroke may name Shift and Ctrl, so the three can be given to the
three strokes of a single USB key.

The implementation is `src/input/usb_keymap.cpp`, a fixed-size table of bindings
and of removed strokes, parsed once at start-up by the kernel (which owns the
system directory) and installed in `CUSBAtariKeyboard` before it registers any
report handler — so the interrupt handler only ever reads it. The driver resolves
each stroke in the order the document lays down: an exact binding, a plain-key
binding with the held modifiers added, a stroke rule 4 removed, then the
built-in mapping.

The parser and rule 4 were checked against the document's examples with a host
test (`build/keymap_test.cpp`, not committed) covering the letters swap, the
three functions of `*`, `A(None)`, combination selectors, comments containing
`#`, duplicate lines, and every rejection message. It was then run on hardware
with a crafted `usbkeys.cfg`, and behaved as specified.

### 4. Verification debt

- Pi 3 and Pi 5 (the Pi 5 page-flip branch has never run).
- GPIO keyboard and 9-pin joysticks.
- The larger RAM configurations.

### 5. Longer term

- **circle-stdlib migration** (requested early, not started). Worth doing only with
  a specific motivation, since the current `_sbrk` approach works.
- Understand **why reopening the HDMI sink hangs** — that single answer would
  unlock stereo and sample-rate changes without a reboot.

---

## 10. Working conventions

- **Never commit without the user's explicit approval.**
- **One change at a time**, tested on hardware before the next.
- **The build must be verified against the binary**, not the log: strings searched
  in the image caught an image that predated a fix.
- **Beware layout changes masking bugs.** A 336-byte change once flipped the
  symptoms of an unrelated fault.
- **Commit messages record root causes**, not change lists.
- `external/circle` stays unmodified; `external/atari800` carries one permitted
  patch (`STATESAV_MAX_SIZE`).
- `config.txt` and the SD card layout live outside the repository, in
  `atari-sd-backup` (temporary) and on the card (authoritative).

---

## 11. Useful references inside the tree

| Path | Why |
|---|---|
| `external/circle/lib/usb/usbgamepad.cpp` | `ReportHandler`: decoding only when a handler is registered |
| `external/circle/lib/usb/usbdevicefactory.cpp` | the device→driver table |
| `external/circle/lib/sound/soundbasedevice.cpp` | `GetChunkInternal`, IEC958 framing |
| `external/atari800/src/libatari800/sound.c` | the reference platform sound layer |
| `external/circle/sample/27-usbgamepad` | the gamepad reference |
| `external/circle/sample/08-usbkeyboard` | the keyboard reference |
| `external/circle/sample/34-sounddevices` | tone test; identifies which HDMI port carries audio |
| `docs/troubleshooting.md` | what the boot stages and logs mean |
