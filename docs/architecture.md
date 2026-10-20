# Architecture

This document explains how the pieces fit together, for anyone reading or
changing the code. It is deliberately about *why* the boundaries are where they
are, not a restatement of what each function does.

## The problem

Two codebases with opposite assumptions have to be joined:

- **Atari800** is a portable C program written to run on an operating system. It
  expects `fopen`, a heap, a clock, a screen it can blit into and a sound device
  to push samples at. It has its own front end, its own menus and its own file
  selector.
- **Circle** is a bare-metal C++ environment. There is no operating system, no
  POSIX, and the standard C library is newlib with no underlying system to talk
  to.

Neither can be used as-is. The join is made in one direction only: **the
emulation core is compiled as a portable library and every outward-facing
dependency it has is satisfied by this project.** Circle is used unmodified.

## Layers

```
    ┌──────────────────────────────────────────────────────────────┐
    │ main.cpp → CAtariKernel                                      │
    │   Circle bring-up, the frame loop, CAtariMenuHost            │
    └───────────────┬──────────────────────────────────────────────┘
                    │
      ┌─────────────┴──────────────┬───────────────┬──────────────┐
      │                            │               │              │
 ┌────▼─────┐  ┌──────────┐  ┌─────▼─────┐  ┌──────▼─────┐  ┌─────▼─────┐
 │ video/   │  │ audio/   │  │ input/    │  │ storage/   │  │ osd/      │
 │ RGB565   │  │ PWM/I2S  │  │ USB, GPIO │  │ FatFs      │  │ menu      │
 │ scaler   │  │ sink     │  │ keyboards │  │            │  │           │
 └──────────┘  └────┬─────┘  └─────┬─────┘  └──────┬─────┘  └─────┬─────┘
                    │              │               │              │
      ┌─────────────▼──────────────▼───────────────▼──────────────▼─────┐
      │ src/port/  -- the platform layer                                │
      │   atari_port.h   the interface the kernel uses                  │
      │   platform.c     PLATFORM_* hooks, machine config, snapshots     │
      │   sound_bridge.c PLATFORM_Sound* + the audio_sink_t seam         │
      │   vfs.c          the file system the core sees                   │
      │   ui_stub.c      headless stand-in for the core's own UI         │
      └─────────────────────────────┬───────────────────────────────────┘
                                    │
      ┌─────────────────────────────▼───────────────────────────────────┐
      │ Atari800 emulation core (external/atari800, unmodified + 1 line) │
      │   compiled with src/core/config.h instead of autoconf's          │
      └─────────────────────────────────────────────────────────────────┘
```

## The key decision: build against the core, not the wrapped library

Atari800 ships a `libatari800` static library whose whole purpose is to be
embedded by another program. Using it looks like the obvious move, and it was
the first approach considered. It was rejected for a specific reason:

**`libatari800` hides the machine configuration.** Its `init()` takes an `argv`,
and its API exposes the screen, the sound buffer, disk mounting and snapshots —
but not the RAM size, the machine type, PAL/NTSC, stereo or volume. Those are
exactly the settings this emulator's OSD menu has to change at run time. The
wrapped library would have to be bypassed to reach them anyway, and its own
front end (`src/libatari800/main.c`) would collide with the platform layer this
project needs.

So the core is compiled directly instead, with `LIBATARI800` defined. That macro
is upstream's own flag meaning "the embedding application owns the main loop,
the display and the sound device" — precisely this situation. It switches off
the parts of the core that would fight with this project, while leaving the
emulator itself completely intact.

The practical consequence is that `src/port/platform.c` reads and writes the
core's public configuration globals directly:

| Setting | What is changed |
|---|---|
| Machine type | `Atari800_SetMachineType()` |
| RAM size | `MEMORY_ram_size`, then `Atari800_InitialiseMachine()` |
| PAL / NTSC | `Atari800_SetTVMode()` |
| Artifacting | `ARTIFACT_Set()` |
| Volume, stereo | `POKEYSND_SetVolume()`, `Sound_desired.channels` |
| Disk drives | `SIO_Mount()` / `SIO_Dismount()` |
| Snapshots | `LIBATARI800_StateSave()` / `LIBATARI800_StateLoad()` |

Those are all documented parts of the core's public interface, not internals.

## The platform contract

The core calls out through two header interfaces, both of which this project
implements completely:

**`platform.h`** — the host platform:

| Hook | Implementation |
|---|---|
| `PLATFORM_Initialise` | `platform.c` — nothing to do; configuration happens in `ATARI_Init()` |
| `PLATFORM_Exit`, `PLATFORM_Keyboard`, `PLATFORM_PORT`, `PLATFORM_TRIG` | `platform.c` — no-op or "nothing connected" |
| `PLATFORM_DisplayScreen` | `platform.c` — nothing to do; the kernel reads the screen after the frame |
| `PLATFORM_Time`, `PLATFORM_Sleep` | `platform.c` — delegated to the kernel's clock and idle callbacks |
| `PLATFORM_SoundSetup/Exit/Pause/Continue/Available/Write` | `sound_bridge.c` |
| `PLATFORM_GetPixelFormat`, `PLATFORM_MapRGB` | `platform.c` — RGB565, needed by PAL blending |

**The file functions.** The core accesses files only through `fopen`, `fclose`,
`fread`, `fwrite`, `fseek`, `ftell`, `rewind`, `feof`, `fflush`, `ferror` and
`opendir`/`readdir`/`closedir`. `src/core/config.h` redirects those names onto
`ATARI_VFS_*`, which is a small implementation over FatFs. **No call site in the
emulation core was edited** — the redirection is complete, so the core compiles
verbatim.

One subtlety is worth recording, because it is the kind of thing that wastes an
afternoon: the VFS hands out its handles *as* `FILE *`. The core declares its
own handles as `FILE *`, so using a distinct handle type would have made every
one of those declarations a type error; and redefining the `FILE` typedef
collides with the C library's own definition in `<stdio.h>`. Since every stdio
call in the core is redirected, a VFS handle can never reach newlib, so
borrowing the type is safe as well as convenient.

## The audio seam

The core negotiates a sound format with the platform: it proposes a sample rate,
a sample size and a channel count, and `PLATFORM_SoundSetup` may adjust them to
what the hardware can actually do. That is a good interface and it is kept.

What is added is `audio_sink_t` in `src/port/audio_out.h`: five function
pointers — open, close, free-space, write, pad-silence — that `sound_bridge.c`
uses and `src/audio/audio_out.cpp` implements over Circle's PWM sound device.
The seam exists so that the emulation core's timing decisions (sample rate,
buffer length) stay with the core, while the mechanism for making a noise stays
with the hardware layer.

One hardware behaviour shapes the design: Circle's `Write()` **never blocks** —
it accepts what fits and reports how much. Samples that do not fit are dropped
rather than delaying the emulation, which is the correct trade for a real-time
stream: a dropped audio sample is inaudible, whereas a stalled emulator is not.

## The video path

The emulated screen is 384x240 bytes, one byte per pixel, each byte an Atari
colour index (0..255); the part a television shows is columns 24..359. That is
the format for both PAL and NTSC — the geometry does not change, only the number
of scanlines in the frame.

`CAtariVideoOut::FillScreen()` turns that into a 1280x720 (or 1920x1080) RGB565
framebuffer:

1. A 256-entry palette is converted from `Colours_table`'s 0x00RRGGBB to RGB565.
   Rebuilding it each frame costs 256 iterations — far less than watching for
   palette changes would.
2. Each output column maps to a source column through a table built once at
   initialisation, because the scaling never changes afterwards.
3. The picture is scaled to the full screen height and centred horizontally, so
   the roughly 4:3 Atari display keeps its shape and the sides are black. Not
   stretching 4:3 to 16:9 is the whole point: it would turn every circle into an
   ellipse.

Double buffering uses Circle's framebuffer paging — the framebuffer is allocated
twice as tall as the screen and the visible window is moved between halves — but
**only on `RASPPI <= 4`**. Circle's own drawing layer deliberately does not use
virtual-height double buffering on the Pi 5, and the behaviour of the virtual
offset there is not established, so on the Pi 5 the emulator draws straight into
the visible buffer. The cost is possible tearing; the alternative is risking a
stuck or absent picture.

## Emulation loop, in order

`CAtariKernel::RunFrame()` does this once per emulated frame:

1. Check whether the menu was requested (F12, or Shift+Help) and toggle it.
2. If the menu is open, drain the key presses it consumes. Keys it does not use
   still reach the emulated machine, which is what lets the user type while the
   menu is on screen.
3. `ATARI_RunFrame()` — one emulated frame. This pulls the keyboard state through
   the callback, runs the core, and paces the result against the real frame rate
   using the host clock.
4. `FillScreen()` — scale the finished screen into the framebuffer.
5. If the menu is open, draw it over the picture.
6. `Present()` — page-flip.

Frame pacing deserves a note because it is easy to get subtly wrong. A PAL frame
is 20055.9 µs, not a whole number. Truncating that to 20055 would lose 0.9 µs per
frame — about 2.7 seconds per hour of drift. `ATARI_RunFrame()` therefore keeps a
32.32 fixed-point accumulator and carries the remainder, so the emulated machine
stays in step with a real one over long runs.

## Why there is a `ui_stub.c`

`Atari800_Initialise()` calls `UI_Initialise()` unconditionally, and the core's
own UI (`ui.c` plus `ui_basic.c`) is a complete on-screen menu with its own file
browser. This project replaces all of that with its own OSD menu, so the
upstream UI is not compiled in.

What remains is `src/port/ui_stub.c`: a few dozen lines that supply
`UI_driver`, the UI entry points, and the handful of globals the core expects
(`UI_is_active`, the file-directory arrays, and so on). They all decline
politely. This keeps the core's initialisation sequence — the tested upstream
one — completely intact while giving this project control of the interface.

## What is deliberately not compiled

Every exclusion is a decision, and most of them are about removing an operating
system dependency rather than saving space:

| Not compiled | Because |
|---|---|
| `videomode.c` | display geometry for upstream's SDL/X11 front ends; only reachable with `SUPPORTS_CHANGE_VIDEOMODE`, which this project does not use. Its configuration-file output also needs stdio this project redirects |
| `monitor.c` | the debugger. One `MONITOR_Exit()` call remains in the core's shutdown path; a two-line stub in `ui_stub.c` satisfies it |
| `sound_oss.c`, `sdl/sound.c` | native sound backends; `PLATFORM_Sound*` is this project's |
| `atari_x11.c`, `atari_curses.c`, `atari_rpi.c`, `atari_ps2.c` | upstream's front ends for other hosts |
| `file_export.c`, `pokeyrec.c` | video and audio recording |
| `rdevice.c`, `netsio.c`, `download.c` | network and remote-device features |
| `ide.c`, `pbi_*.c` | PBI and IDE extensions |
| `af80.c`, `bit3.c`, `xep80*.c`, `voicebox.c`, `votrax*.c` | optional 80-column and speech hardware |
| `filter_ntsc.c` | the full NTSC comb filter: it needs a frame of history and is far more expensive than the rest of the emulator |

`sysrom.c` **is** compiled: it is what loads the OS ROM images from the SD card
and validates them by length and CRC32.

## Memory

The kernel image must stay under Circle's `KERNEL_MAX_SIZE`, which this project
raises to 4 MiB. The default is 2 MiB, and overrunning it makes `sysinit()` halt
**silently with no output at all** — a dark screen and nothing to explain it.

The largest part of the kernel's memory use is not code but the snapshot buffer:
`emulator_state_t` carries a `state[]` array sized by `STATESAV_MAX_SIZE`, and
this project raises that from upstream's 210000 to 524288 so that a snapshot of a
320 kB machine fits. Upstream's value is silently too small for anything above
64 kB of emulated RAM — the in-memory save backend stops writing when the buffer
is full and reports no error. See
[Atari800 integration](atari800-integration.md).

Other notable allocations, all made once at start-up:

| Buffer | Size | Why a whole image is loaded |
|---|---|---|
| `Screen_atari` | 92 KB | the emulation core allocates it |
| Emulated XE RAM | up to 278 KB | 320 kB machine |
| Snapshot buffer | 512 KB | see above |
| Disk images | up to ~2 MB each | FatFs has `FF_USE_FASTSEEK` disabled, so random sector access into an ATR is slow; the core reads sectors through the VFS and the OS caches nothing, which is why disk images are small enough to leave alone |
