# Project progress

A bare-metal Atari XL/XE emulator for the Raspberry Pi, cross-compiled on Windows.
Circle provides the hardware layer; Atari800 provides the emulation core; this
project is the seam between them.

This document records where the project stands, what has been established by
measurement rather than assumption, and what to do next. It is written to be read
cold, without the history that produced it.

**Last updated:** after commit `cfab5ee`; the Shift+arrow fix and this
documentation update are in the commit that carries this line. USB keyboard
remapping, BRK handling and snapshot erasing are implemented and verified on
hardware; the output is muted while the OSD is open; the build and verification
scripts refuse a kernel that does not fit in Circle's 2 MB region; the OSD is
drawn in the Atari character set
at twice its size, compiled in as data, its layout follows the font, and the panel
is centred in the picture; the menu can warm-reset, cold-reset and power-cycle the
machine, the last of which is the only way out of a cold reset that inherited a
console-key mask from the software that ran before it; user media can live on the
USB drive or in `SD:/USERDATA`, chosen by the settings file; a disk mounted
read/write is genuinely writable, with its mode changeable while it stays inserted;
a cassette image is classified when it is inserted, so a standard tape loads fast
and a turbo tape gets the real-timing mode a power cycle away; `STATIC_OSD=1` lets
a slow board open the menu without losing the frame deadline; the picture fill was
made five times faster by scaling from the source, so a Zero 2 W now holds 49.8 fps
at PAL and 59.9 at NTSC with no picture skipped, the adaptive skip remaining as a
safety net; the output resolution, the pixel scale -- 4:3, or an exact 2x/3x/4x,
the last only where the display can hold it -- the scanline effect and its
strength, and the joystick interface are chosen in the menu -- USB gamepads, the
9-pin ports or keys on the USB keyboard -- the resolution and the interface on the
next power cycle and the joystick port swap at once; and the runtime log carries a
30-second stats line with the frame budget, the skip count when there was one,
and the OSD's open and close. Checked on a Pi 400 and a Zero 2 W.

---

## 1. Status at a glance

| Area | State |
|---|---|
| Video | 1280x720, picture 960x720, correct 4:3 with black side bars |
| Frame rate, Pi 400 | **49.8 fps** against a 50 fps target |
| Frame rate, Pi Zero 2 W | **49.8 fps PAL, 59.9 fps NTSC**, no picture skipped — the primary target, verified |
| Sound | correct over HDMI; volume applies live; stereo needs a restart |
| Disk / cartridge / cassette | loading verified; eMMC faults fixed; media from the USB drive or `SD:/USERDATA`, chosen in the settings file; standard tapes load fast, turbo tapes after the automatic power cycle |
| Keyboard | USB verified direct; `usbkeys.cfg` remapping verified; **hub unsupported on Zero 2 W** |
| Snapshots | save, save-as-new, load and erase **verified on hardware** |
| OSD menu | opens, navigates, applies settings; Atari character set at 2x; warm reset, cold reset and power cycle; no flicker; `STATIC_OSD` for boards where the picture fill will not fit beside the emulation |
| Activity LED | lit while the machine runs, dark while a file is read or written, 1 Hz blink after a failure; replaces the SD driver's per-transfer blinking |
| Pixel scaling | 4:3 aspect fit, or exact 2x/3x in a border, chosen in the menu and applied live; 2x costs under half the fill, 3x at 1080p costs what 720p 4:3 used to; 4x (1344x960) is offered only where it fits, i.e. 1080p |
| Scanlines | off/on and the 85/90/95 % factor, both applied live; the dim band repeats once per upscaled Atari line; at 1080p 4:3 the fill is 7.4 ms off and 7.8 ms on, so the effect is nearly free; also free at 2x and 720p |
| Boot to first frame | ~6.6 s on Zero 2 W, ~7 s on Pi 400 |
| USB gamepads | resolver, hot plug and both pad kinds **verified on hardware** — see section 8 |
| Joystick ports | two inputs (`STICK(0)`, `STICK(1)`); one interface at a time, chosen in the OSD as USB gamepads, the two 9-pin GPIO ports, or keys on the USB keyboard (`Joy1_Up` .. `Joy2_Fire` in `usbkeys.cfg`, nothing bound by default), with the ports swappable live; the 9-pin path has never been run on hardware |
| Pi 3, Pi 4 / 400 | `kernel8.img` runs on a Zero 2 W, 3B, 3B+, 4 and 400 at 720p or 1080p, and so does the A72 build on the 4 / 400 |
| Pi 5 | image builds; never run |

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

The separate frame-cost line has since been removed from the kernel: it answered
its question. Since `8f79f42` the 30-second `stats` line carries the same split --
`frame` (the work: emulation + fill + OSD) and `pace` (the sleep to the deadline) --
so these numbers can be read without a special build. The figures above are still
the ones the renderer work was judged by, and they are worth keeping:

### Fill cost, Zero 2 W (temporary measurement, `e2773e4`)

The fill was split into its two halves to decide where to optimise:

```
before:  stage 16,383 us   copy 1,516 us   (fill 17.9 ms of a 20 ms frame)
after:   stage  3,305 us   copy 1,490 us   (fill  4.8 ms)
```

`stage` is the scaling loop, `copy` is the write into the un-cacheable
framebuffer. The copy was expected to dominate and did not: 1.38 MB at 920 MB/s
is 1.5 ms, so nothing was wrong with the framebuffer path and a DMA engine would
have optimised 8% of the cost. The scaling loop was the whole story, and it was
slow for a structural reason — three dependent loads per *destination* pixel on
an in-order core.

Rewriting it to walk the *source* and write each pixel's run of two or three
destinations, plus scaling each source row once instead of three times, cut it
five-fold. The effect on the machine:

| | before | after |
|---|---|---|
| PAL, Zero 2 W | 49.2–49.8 fps, ~half the pictures skipped | **49.8 fps, nothing skipped** |
| NTSC, Zero 2 W | 56.0 fps against 60, visible flicker | **59.9 fps, nothing skipped** |

The adaptive skip is still there, but it is a safety net now rather than the
normal state of the machine.


### Scanlines, Pi 3B+ at 1080p (`e540f93`)

| Mode | emulation | fill | work | pace | period |
|---|---|---|---|---|---|
| 4:3, scanlines off | 2.2 | 7.3 | 9.5 | 10.1 | 19.6 ms |
| 4:3, scanlines on | 2.2 | 14.4 | 16.6 | 3.1 | 19.7 ms |
| 2x, scanlines on | 2.2 | 8.2 | 10.4 | ~9 | ~20 ms |

`work` is emulation + fill; `period` is work + pace. The `2x` row is approximate
because its figures were read to the nearest millisecond. Scanlines roughly double
the fill at 1080p 4:3, which leaves almost no pace there; those windows logged one
skipped picture in three, while the 720p and `2x` cases fit comfortably.
`emulation` stayed at 2.2 ms throughout.

### Scanlines after the dark palette

Measured at 1080p 4:3 after the dark-palette renderer; the table above is the
renderer it replaced.

| Mode | emulation | fill | work | pace | period |
|---|---|---|---|---|---|
| 4:3, scanlines off | 2.2 | 7.4 | 9.6 | 10.1 | 19.7 ms |
| 4:3, scanlines on | 2.3 | 7.8 | 10.1 | 9.7 | 19.8 ms |

The fill drops from 14.4 ms on to 7.8 ms, and the one skipped picture the old
renderer logged is gone.  `work` is emulation + fill; `period` is work + pace,
rounded to the nearest tenth.  The four aspects and both `STATIC_OSD` settings
were checked; the integer scales and 720p remain cheap.

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
here so they are not rediscovered. A row marked *(this commit)* is the commit that
carries this table; a commit cannot name its own hash.

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
| `3c74b8b` | the two 9-pin joystick ports could never be used | the USB loop filled **all four** controller slots with driver objects even when no pad was attached — deliberately, so a later plug-in is noticed without polling — and the GPIO loop skipped any port that already had a handle. So the slots the 9-pin ports wanted always held placeholders that reported themselves absent, and nothing in the log said why. Replaced rather than patched: one interface at a time, two inputs, chosen in the OSD. |
| `9981c01` | the docs say Delete ejects an image, and it never did | the menu only receives the keys the keyboard drivers classify as menu keys, and neither list contained Delete: `MenuKeyForUsage()` on USB and `MenuKeyForPosition()` on GPIO both stopped at the arrows, Return, Escape and Space. The menu's handler for it was unreachable, in the same way `UpdatePlugAndPlay()` was. Delete is now classified on USB, and the GPIO driver maps BACK SPACE to it, because the Atari's Delete is CTRL+BACK SPACE and the menu cannot see modifiers. |
| `9981c01` | no way to remove an old snapshot | the Snapshots screen could write and read files but never erase one. Erasing now has a screen of its own, *Erase snapshot*, reached like the others: the save and load browsers were the wrong place to put it, because a file could then be lost in a screen opened for another purpose. Return or Delete asks, a second press erases, and any other key cancels **and takes the question off the screen** — leaving it there made a cancelled action look pending. |
| `6712e7b` | a long directory listing ran past the bottom of the menu panel | the browser drew a fixed 20 rows, but the panel holds about 14 at the font the menu used: the last rows were drawn over the status line and the footer, and a transient message was drawn on top of them. The row count is derived from the panel and the font now, and limited by the message line. |
| `fabd27b` | a disk mounted read/write was still write-protected, and one mounted read-only could not be made writable | three faults, each enough on its own. The ATR header's write-protect byte forced the drive read-only whatever mode the file was opened in, and the menu's `[ro]` setting was never consulted. `ATARI_VFS_Open` decoded `"rb+"` as read-only, so even a read/write mount dropped every sector the machine wrote while `SIO_WriteSector` reported success. And the mode is fixed when the file is opened, so toggling `[ro]` changed only the setting — while ejecting reset it to read-only, so re-inserting the same image always came back protected. All three are fixed, the mount log now reports the mode the drive actually has, and the cassette had the same shape of bug in its toggle. |
| `a7f62b6` | the OSD panel sat against the left edge of the picture at 720p, so it looked like it leaned left | the centring code clamped the computed margin against `MenuMarginX`, which is an absolute position and starts at 40, not a margin. At 720p the picture is 960 wide and a 900-wide panel needs a 30-pixel margin, so `30 > 40` was false and the margin was dropped: flush left, 30 pixels short on the right. The panel is centred whenever it fits now, and falls back to the picture's edge otherwise; the vertical expression had the same shape and was fixed with it. Only 720p showed the fault, because the 1080p margin (270) is larger than 40. |
| `1c48244` | a cold reset left some software in charge: BOOT ERROR instead of BASIC, as though OPTION were still held | a cold start is the CPU's RESET line, and POKEY and GTIA have no reset pin, so GTIA's console-key mask survived — and software can set it by writing `$D01F`, where a masked key reads back as pressed. RAM survived as well. There was no way out of that state, because no reset clears what the OS was misreading. *Power cycle* on the main menu now builds the machine again: RAM zeroed, every chip at power-up, ROMs reloaded, media mounted again. The audio device is deliberately left open across the rebuild, since reopening the HDMI sink hangs this hardware. |
| `e540f93` | after closing the menu, the black side margins could alternate between black and the panel's edge strips on every flip, while the middle of the picture stayed steady | `InvalidatePadding()` deferred clearing to the next fill, and a fill composes one half of the double buffer and then sets the once-only flag, so the other half kept the panel's edges until something repainted it whole. An integer scale can make the picture narrower than the 900-pixel OSD panel, which draws over the margins, so the fault appeared there. Both halves are now cleared in one bounded pass when the menu closes. |
| `a243567` | the black bars above and below the picture were rewritten on every fill at integer scales | the once-only flag guarded only the side strips; the top and bottom block ran on every fill.  At 2x on 1080p that is 1.15 million pixels a frame against 0.32 million of picture -- several milliseconds rewriting bytes that cannot have changed.  `ClearMargins()` now clears the top and bottom bars and both side strips in both halves when the flag says they are stale, and the OSD-close path calls it immediately. |
| `a6ff28d` | the transparent OSD was slow because the panel dim spent three integer divisions per pixel | a 900x480 panel is 1.3 million divisions per redraw.  The per-channel results are tabulated once per call -- five bits for red and blue, six for green -- and looked up in the pixel loop; the values are unchanged, so the panel looks the same.  Only `STATIC_OSD=0` calls `DimRect()`. |
| `53cdf22` | after closing the menu with `STATIC_OSD=0`, the machine sometimes stuttered and the sound was choppy, especially after an aspect or scanline change or the thirty-second log line | the setting save and the runtime-log write are long SD blocks.  The pacer resynchronises, but the core's own audio sync buffer can stay full; `Sound_Update()` then sleeps one hardware buffer at a time inside `Atari800_Frame()`, charged to emulation, and once production and consumption match it never drains.  The menu-close transition now pauses the core's audio, rebuilds the buffer with `Sound_SetLatency()` and continues; the device is not reopened, and a short silence on close is expected. |
| `c1334b6` | scanlines doubled the 1080p 4:3 fill and left only 3.1 ms of pace, with one skipped picture per window | the dark row was produced by darkening every pixel of the already-scaled row -- three integer multiplies per destination pixel of every dark row.  A 256-entry dark palette is now built once per frame, and each row run builds only the lit row, the dark row, or both; the per-destination-row memcpy sequence is unchanged, so the framebuffer write pattern and its audio/pacing interaction stay comparable to the renderer it replaces.  Fill 14.4 -> 7.8 ms, frame 10.1, pace 9.7, nothing skipped. |
| `cfab5ee` | `SCANLINES` and `SCANLINE_FACTOR` in `settings.cfg` were ignored on the next boot, and `JOY_INTERFACE=USB` reset the saved aspect ratio | the `SCANLINES` branch assigned the parser's stale `bValue` without calling `ParseBool()`; `SCANLINE_FACTOR` tested the same stale `bValue` without calling `ParseInt()`, so neither value ever reached the settings; and the `JOY_INTERFACE` USB branch also wrote `nAspect = AtariAspect43`.  All three now parse and validate their own values; the other boolean keys already used `ParseBool()`. |
| (this commit) | Robbo would not shoot left or right with Shift+arrow, although up and down worked | the Atari cursor keys are Ctrl+Plus/Asterisk/Minus/Equal.  ORing Shift onto Left or Right produced Ctrl+Shift+Plus/Asterisk, which the core's ghost-key filter drops; Up and Down are Ctrl+Shift+Minus/Equal, which that filter does not match.  The shift switch now maps the four arrows to Plus/Asterisk/Minus/Equal before the shift bit is added -- the same codes the SDL frontend sends -- so Shift+arrow produces the legal shifted symbols. |

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

14. **The kernel image has a fixed 2 MB region, and `.bss` counts.** Circle's
    `MEM_KERNEL_END` is `MEM_KERNEL_START` (0x80000) plus `KERNEL_MAX_SIZE`
    (2 MB), and it places the kernel stack and the heap immediately after that
    boundary (`memorymap64.h`). A 640 KB static buffer added while reworking the
    OSD pushed `_end` to 0x2AB300 — 173 KB past the boundary — and the machine
    then did nothing at all: black screen, no boot log, nothing on the card to
    read, because the stack and then the heap were sitting inside the kernel's
    own data. The image file is not the kernel: `.bss` is not in it, so its size
    said 816 KB while the kernel was 2.2 MB. `build.ps1` and `verify.ps1` now read
    `_end` from the ELF and refuse an image that does not fit; large buffers
    belong on the heap. Current headroom is about 450 KB, which is not a lot —
    check before adding another buffer.

15. **The Atari character set is indexed by internal codes, not ASCII, and its
    glyphs start blank.** At `$E000` the ROM holds space at `$00`, digits at
    `$10`-`$19`, `A`-`Z` at `$21`-`$3A` and lower case at `$61`-`$7A`: the OS
    subtracts `$20` from ATASCII below `$60` and leaves the rest alone. Reading
    the set with ASCII indices gives graphics, not letters. And the first row of
    nearly every glyph is empty, so "the first byte is zero" is not a way to test
    whether a ROM contains a character set — that check rejected a perfectly good
    ATARIXL.ROM twice before it was found.

16. **A disk image carries its own write-protect flag, and the file layer's
    modes are not C's.** The last byte of an ATR's 16-byte header marks the image
    write-protected, and the core honours it regardless of how the file was
    opened; the drive then reports bit 3 of its status, which SpartaDOS calls
    error 144. This project's `[ro]` menu entry is the decision that counts, so
    `ignore_header_writeprotect` is set once at start-up — a plain global in
    `sio.c` with no setter, declared `extern` in the port. Separately, `fopen` is
    `ATARI_VFS_Open` inside the core, and `"rb+"` must ask FatFs for `FA_WRITE`:
    without it the open succeeds, every write is refused, and `SIO_WriteSector`
    reports success anyway. Read-only is also fixed at open time, so changing it
    means re-opening the drive — an open image cannot be switched.

17. **A cold reset is not a power cycle, because two chips have no reset pin.**
    `Atari800_Coldstart()` resets PIA, ANTIC and the CPU, and the core's own
    comment says POKEY and GTIA are not reset: both keep their registers, and RAM
    is only marked cold — it keeps its contents. GTIA's console-key mask is the
    one that bites: software that writes `$D01F` can mask a key out, and a masked
    key reads back as pressed, so after a cold reset the OS boots as though Option
    were held, skips BASIC and ends in BOOT ERROR with an empty drive. Nothing
    short of building the machine again clears that, which is what the menu's
    *Power cycle* does. Rebuilding also means `Atari800_Exit()` without
    `ATARI_Exit()`, so the audio device stays open — and `PLATFORM_SoundSetup()`
    reuses an open device whose stream shape has not changed, because reopening
    the HDMI sink hangs this hardware.

18. **The SIO patch and the timed cassette path are mutually exclusive, and the
    choice is baked in at build time.** `ESC_enable_sio_patch` is on by default;
    with it on, `POKEY_Scanline()` skips `CASSETTE_AddScanLine()` and the patched
    SIO copies tape records straight out of the image — which refuses `fsk `
    blocks outright (`IMG_TAPE_ReadToMemory`, and `SkipToData` skips them whole).
    So a standard tape loads instantly and a **turbo tape cannot load at all**.
    Turning the patch off gives the timed path, which reads anything but runs at
    tape speed. The switch cannot be flipped in a running machine: the patches are
    written into the OS ROM image in memory, and `ESC_Remove()` only clears the
    dispatcher, leaving the ESC opcode behind. It must be set before
    `Atari800_InitialiseMachine()` — i.e. at a build, which is why changing it
    takes a power cycle. A tape is turbo when it has fsk *payload*: fsk blocks
    with no data blocks, or fsk blocks after the data has begun. A standard tape
    whose leader is an fsk block stays standard.

19. **The SIO patch breaks the built-in SELF TEST's ROM check, by design.** The
    patch is ESC opcodes written into the OS ROM image in memory via
    `MEMORY_dPutByte()` (`esc.c:101`), bypassing the CPU's write protection: at
    `$E459` (SIO entry), `$FD13` and `$FD60` (cassette open, XL OS) and — a
    telling one — `$C31D`, where two `EA` bytes replace the OS's boot-time
    checksum test, with the comment *"Disable setting NGFLAG on wrong OS
    checksum"*. The core suppresses the OS's own complaint because it caused it;
    it cannot suppress the self-test's separate verification, so the self-test
    reports a ROM error while the machine is otherwise perfectly healthy, and the
    RAM tests pass. Confirmed on a Zero 2 W: with a turbo `.cas` inserted and a
    power cycle (real-timing mode, `ESC_enable_sio_patch = FALSE`, unmodified
    ROM), the ROM test passes. This is upstream behaviour, not a port defect, and
    the documentation now says so rather than leaving a reader to conclude the
    emulator is broken.

20. **The core can sleep inside a frame, and it is charged to emulation.**
    `src/sound.c:405` — when the core's own staging buffer has no room for a frame's
    audio it loops on `Util_sleep (buffer_frames / freq)`, one whole hardware buffer
    per iteration, *inside* `Atari800_Frame()`.  Two iterations measured 21 ms of
    "emulation" against a true 2.3 ms.  Whether it is reached depends on the sink's
    queue depth at that instant, which depends on how long the previous frame's fill
    and sleep were — so it appears and clears with the rendering cost, and looks
    like a renderer fault.  Our sink never blocks and drops what it cannot take;
    `PLATFORM_SoundAvailable()` reporting only the real free space is what lets the
    core enter that loop.

21. **Circle's keyboard device has room for exactly one raw key handler.**  `m_pKeyStatusHandlerRaw`
    is a single pointer in `CUSBKeyboardDevice`, and registering again replaces rather than adds;
    there is no getter for the held keys either, so a second driver cannot poll instead.  Two drivers
    here need those reports -- the Atari keyboard, and the keyboard joystick -- so neither owns the
    device: `src/input/usb_key_reports.cpp` claims the handler and forwards every report to a list
    of listeners, and the report for a keyboard that has gone is an empty one so that nothing stays
    held.  Any future driver that wants key reports registers there, not on the device.

---

## 6. Known limitations

- **Settings are applied, and logged, only when they change.** `ApplySettings()` diffs the
  incoming settings against the ones the machine is configured for: one line per field that
  actually moved, nothing at all when nothing did.  The old behaviour re-applied every
  setting on every menu key press, which filled the log and restarted the emulated
  machine twice for a RAM or machine change.

  The duplicated `apply:` blocks that prompted this were **not** a fault in the key
  path: one menu action that changes several settings at once calls in more than once,
  which is correct, and it only looked wrong while every call printed the whole list.
  With the diff, the second call prints only the fields that the first had not already
  applied, and nothing at all when they are the same.
- **Audio latency is two buffers, and both were too big.**  The HDMI DMA chunk is
  a multiple of `IEC958_SUBFRAMES_PER_BLOCK` (384 words); at 1920 that is 43.5 ms per
  buffer and there are two of them, and the output queue was another 100 ms, so a key
  click arrived ~90-100 ms late against a real machine's nothing.  Now 384 words and a
  35 ms queue: ~26 ms typical.  The chunk cannot be any multiple of anything -- 480
  aborts the boot with an assertion.
- **Snapshots are named after the media they hold**, and `STATIC_OSD` is on by
  default: `Snap_<disk 1>_<cassette>_<cartridge>_<n>.sav`, every part optional, so a
  card of snapshots can be read without loading them.
- **A snapshot is a container, not just the core's state.**  Ahead of the core's blob it
  carries the host block: the machine settings and the mounted media with their flags.
  Loading applies that first -- a 320 kB snapshot has to go back into a 320 kB machine --
  then the machine, then checks each image still exists and empties and reports the ones
  that do not.  Snapshots written before this are refused rather than half-restored.
- **The volume is applied in the audio sink, not in POKEY.** Changing it used to
  re-initialise the mixer, which reset the serial timing the cassette is read
  through and broke a tape load until the Pi was power-cycled; the sink scales the
  samples instead, so the cost is nothing at 100% and about 5 us a frame below it.
- **Stereo needs a restart.** The channel count is a property of the open device,
  and reopening the HDMI sink hangs on this hardware. Reported honestly in the log.
- **The OSD redraw is still heavy enough to throttle.** The transparent panel
  dims 900x480 pixels; `a6ff28d` replaced its three divisions per pixel with
  5/6-bit tables, so the redraw no longer does 1.3 million divisions, but it is
  still several milliseconds and is drawn every 10th frame (every 4th while keys
  are held).  With `STATIC_OSD=0`, a long settings save or runtime-log write can
  leave the core's audio sync buffer full; the menu-close transition resets it,
  so closing may be followed by a short silence rather than a standing stutter.
- **The fill was the frame budget on a Zero 2 W, and is no longer.** Repainting
  960x720 used to cost 18 ms of a 20 ms frame, so a frame that drew could not
  finish in time and the adaptive fill skip had to leave every second picture
  alone — 49.2–49.8 fps at about 25 pictures a second. `e2773e4` moved the scaling
  loop from the destination to the source and made it five times faster, taking
  the fill to 4.8 ms; the skip is now a safety net rather than the normal state
  of the machine, and the stats line reports no skipped pictures at all.
  `STATIC_OSD=1` still applies while the menu is open, where the picture is not
  worth repainting at all.
- **The `stereo change deferred` message** is now accurate, but the underlying
  inability remains.
- **The Pi 5 is unverified.** Its image has a page-flip branch that has never
  executed.  The A53 image, by contrast, has run on a Zero 2 W, a 3B, a 3B+, a 4
  and a 400, at 720p or 1080p.
- **The OSD font is the one piece of this repository that is not ours to
  license.** `src/osd/atari_font.cpp` holds the XL/XE character shapes, extracted
  from an OS image. The ROM images themselves are excluded from version control,
  but these 128 glyphs are compiled in, so publishing the repository publishes
  them. Everything else is clean: this project's code is MIT, Circle is GPLv3,
  Atari800 is GPLv2-or-later, FatFs is permissive, and the combination of the two
  GPLs is lawful because of the "or later" clause.

---

## 7. Testing on hardware

Everything below was verified on a **Pi 400** (720p, HDMI, monitor with speakers)
and a **Pi Zero 2 W** (720p, monitor without audio).

Verified: boot to BASIC; disk, cartridge and cassette mounting, and cassette
reading at both standard and turbo timing; disk write-back from a read/write
mount; settings applied at startup and persisted; crash marker recovery; menu
navigation, including the power cycle; `STATIC_OSD=1`; sound; 49.8 fps PAL and
59.9 fps NTSC with nothing skipped; USB keyboard direct, and hot-plugged; USB
gamepads — both a generic pad and a Switch Pro — attached before power-on and
while running, with unplug, re-plug and swap; snapshots — save, save as new, load,
and erase from the Erase snapshot screen; the Joysticks screen — opening it,
stepping the interface and the swap row, the footer messages, and the two-port
order surviving a power cycle; the keyboard interface — `Joy1_*` keys moving `STICK(0)` while the rest of the keyboard still types, and a mapped key no longer typing its character.

Not verified: GPIO keyboard, 9-pin GPIO joysticks, the second keyboard mode,
cassette writing, hot-plugging USB storage, sound on the Zero 2 W, the Pi 5.

Verified since, on a Zero 2 W, a 3B, a 3B+, a 4 and a 400: every RAM
configuration -- 64 / 128 / 192 / 320 (RAMBO) / 320 (Compy Shop) kB -- and NTSC
as well as PAL on the 4 / 400 class.  Those two were the last doubts on that list.

The OSD margin clearing and the audio-debt reset were exercised with
`STATIC_OSD=1` and `=0` at 1080p and 720p: all four aspects keep black margins
after closing the menu, and the recipe that used to stutter -- an aspect or
scanline change, or a log write with the menu open -- no longer leaves the
machine stuttering.

The dark-palette scanline renderer was checked at 1080p in 4:3, 2x, 3x and 4x,
with `STATIC_OSD=1` and `=0`: the pattern is unchanged, scanlines-on fill at 4:3
is 7.8 ms against 7.4 ms off, and nothing is skipped.

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

### Open items, in the order I would attack them

1. **The core can sleep inside a frame, and it is charged to emulation.**
   `external/atari800/src/sound.c:405`: when the core's staging buffer has no room
   for a frame's audio it loops on `Util_sleep (buffer_frames / freq)` inside
   `Atari800_Frame()`.  Two iterations measured 21 ms of "emulation" against a
   true 2.3 ms.  It appears and clears with the rendering cost, because that
   changes the sink's queue depth at the moment of the write.  A trial that made
   `PLATFORM_SoundAvailable()` always report at least one buffer removed the
   stall, but produced 703 skipped pictures in 1405 frames with a `frame` of only
   11.6 ms -- unexplained, and reverted.  The skip decision
   (`ATARI_LastFrameOverran()`) disagreeing with the budget figures is part of the
   same item.
2. **SD release artefacts.**  They predate everything since the aspect work.  One
   `New-SdCardRelease.ps1` run brings the zip and the image level.
   `C:\Projects\ai\deepseek\SD\SD-card-content\ATARI\settings.cfg` has `ASPECT`
   and `STATIC_OSD=1` but not `SCANLINES` or `SCANLINE_FACTOR` (the emulator
   rewrites the file on the first change).
3. **Pi 5 / Pi 500.**  The image builds but has never run, and it is the only
   platform whose page-flip branch has never executed.  Rebuild
   `kernel_2712.img` after any change.
4. **Unverified on hardware.**  The GPIO keyboard, the 9-pin joysticks, hot
   plugging USB storage, cassette writing, and sound on a Zero 2 W (the test
   display has no speakers).
5. **Stereo needs a restart.**  The channel count belongs to the open device, and
   reopening the HDMI sink hangs on this hardware.  Understanding why would
   unlock live stereo and sample-rate changes.
6. **OSD font licensing.**  `src/osd/atari_font.cpp` holds glyph shapes extracted
   from an XL/XE OS image and compiled in; the ROMs are gitignored, these 128
   glyphs are not, so publishing the repository publishes them.  This is the one
   unresolved licensing exposure.

### Done, and decided against

#### USB gamepad resolver — done and verified (`aed2e43`; hot plug `7b8153a`)

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

#### Housekeeping

- ~~Gate the `frame cost` logging behind a build flag.~~ Done the other way round
  in `64d93ba`: the separate measurement and its line were removed.  `8f79f42`
  later put the budget back into the `stats` line itself -- what a frame cost and
  how much of it was sleep -- because the two had been added together and the sum
  was misleading.
- The gamepad work is `aed2e43`, `7b8153a` and `92d4282`.

#### USB keyboard remapping — done and verified (`5498dc7`)

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

#### Scanlines — the dim-row renderer (`e540f93`, superseded)

Atari800's scanline simulation darkens alternate display lines to suggest a
television.  Ours is a Display row (`Scanlines: off/on`) and a factor
(`85%`/`90%`/`95%`), live, with `SCANLINES` and `SCANLINE_FACTOR` in the settings
file.  Atari800's own implementation is in its SDL frontend only — neither the
core nor `libatari800` has any scanline code — so there was nothing to adopt.

The pattern repeats once per upscaled Atari line, not every second row: at 720p a
line is three destination rows, so the period is
`destHeight / 240` rows lit then the same number dark (5 at 1080p, 3 at 720p, 2 at
2x).  Every second row was tried first and read as a fine comb.

The first renderer was the **dim row**: the scaled row was copied, and a darkened
copy of it was built once per run for the dark blocks.  Cost measured on a 3B+ at
1080p: fill 14.4 ms against 7.3 ms off.  A dimmed **palette** — 256 entries once
per frame, dark rows written straight from the source columns — was tried twice
then and measured ~9.3 ms, but both attempts were withdrawn: one stalled for 21 ms
inside `Atari800_Frame()` -- since traced to the core's sound overflow loop, not
the renderer -- and the other's skip count did not match the budget.  The
dark-palette renderer below is a different shape: it keeps the row buffers and the
framebuffer write sequence, and changes only where the dim bytes come from.

#### Scanlines — dark palette (the commit that carries this)

The dim-row pass is gone.  A 256-entry dark palette is built once per frame, and
the scaler chooses per row run: the lit row only, the dark row only, or both.
Runs are mixed only at 1080p 4:3, where the 5-row period straddles the 4/5-row
runs; at 720p and the integer scales a run is all lit or all dark and only one row
is built.  The per-destination-row `memcpy` sequence is unchanged, so the
framebuffer write pattern -- and the audio/pacing interaction -- is the same as the
renderer it replaces.

Measured at 1080p 4:3: fill 7.4 ms off and 7.8 ms on, frame 10.1 ms, pace 9.7 ms,
nothing skipped; the dim-row renderer was 14.4 ms on.  The four aspects and both
`STATIC_OSD` settings were checked.

#### Decided against

- ~~**circle-stdlib migration**~~ — **decided against**, and not to be revisited
  without a concrete problem to solve. The `_sbrk` approach works, and a port of
  the whole standard library for its own sake would be a large change with no
  failure to justify it.

---

## 10. Working conventions

- **Never commit without the user's explicit approval.**
- **One change at a time**, tested on hardware before the next.
- **Documentation is updated before the commit and included in it.** The ledger,
  `using.md`, `troubleshooting.md` and `README.md` are edited in the same commit as
  the behaviour they describe.  A separate "record in the ledger" commit means the
  code commit was incomplete.
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
