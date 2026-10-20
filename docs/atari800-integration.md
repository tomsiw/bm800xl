# Atari800 integration

How the Atari800 emulation core is configured and driven by this project, and the
exact changes made to the upstream sources.

**Revision used:** Atari800 **7.2.1** (`ATARI800_7_2_1`), pinned in
`external/SOURCES.txt`.

**Changes to upstream:** one, in `src/libatari800/libatari800.h`, described at
the end of this document and stored as
`patches/atari800-statesav-size.patch`. Nothing else is modified. No call site in
the emulation core was edited.

---

## Why the core is compiled directly rather than through `libatari800`

Atari800 can be built as `libatari800`, a static library meant to be embedded.
It was evaluated first and rejected, for one concrete reason: **it hides the
machine configuration.**

Its API exposes the screen, the sound buffer, disk mounting and snapshots, but
not RAM size, machine type, PAL/NTSC, stereo or volume — which are precisely the
settings the OSD menu must change while the emulator runs. Its `init()` takes an
`argv` of command-line options instead, so a settings change would mean tearing
down and rebuilding the whole emulator.

Compiling the core directly, with `LIBATARI800` defined, avoids that. Upstream's
own `LIBATARI800` flag means "the embedding application owns the main loop, the
display and the sound device", which is exactly this situation, and it leaves
the core's public configuration globals reachable.

What is *not* done is reaching into internals. Every global and function this
project touches is declared in a public header of the core.

---

## `src/core/config.h`

Upstream has no `config.h.in` in the tree: autoconf generates it by probing for
an operating system, and there is none here. `src/core/config.h` is therefore
written by hand. Its guiding rule is **define only what is really available** —
every optional subsystem left undefined removes a source file from the build and
an OS dependency from the port.

`HAVE_*` was derived from what newlib (arm-none-eabi-gcc 14.3) and Circle
actually provide, cross-checked against upstream's own hand-written
`src/amiga/config.h`, which is the closest existing "no autoconf" reference in
the tree.

### Feature choices

| Macro | Set | Why |
|---|---|---|
| `LIBATARI800` | yes | the embedding-application mode described above |
| `PAL`, `NTSC` | both | both video standards are compiled in and selected at run time |
| `NEW_CYCLE_EXACT` | yes | cycle-exact ANTIC rendering: mid-scanline colour changes and correct mid-line GTIA writes. This is the single most important accuracy option in the build |
| `PAL_BLENDING` | yes | PAL colour blending. A per-line effect, affordable, and what makes PAL screens look right |
| `FILTER_NTSC`, `NTSC_FILTER` | no | the full NTSC comb filter needs a frame of history and is an order of magnitude more expensive |
| `PLATFORM_MAP_PALETTE` | yes | required by `PAL_BLENDING`; implemented as RGB565 in `platform.c` |
| `SOUND`, `STEREO_SOUND` | yes | POKEY sound, mono and stereo |
| `INTERPOLATE_SOUND` | yes | smoother POKEY output |
| `HAVE_SETJMP` | yes | **essential.** See "Crash containment" below |
| `EMUOS_ALTIRRA` | yes | AltirraOS, a free replacement OS, compiled in as a fallback |
| `USE_UI`, `MONITOR`, `SCREENSHOTS` | no | replaced by this project's OSD menu |
| `PAGED_ATTRIBUTES` | no | upstream's non-default per-byte attribute array; `NEW_CYCLE_EXACT` gives correct colour timing without the extra 92 KB array |
| `WORDS_UNALIGNED_OK` | no | ARM can do unaligned word access, but Circle configures the MMU such that some paths trap, so the fast paths are not built |
| `HAVE_UNISTD_H`, `HAVE_DIRENT_H`, `HAVE_SIGNAL_H`, `HAVE_SYS_*` | no | these headers do not exist on bare metal. Claiming them would pull in OS calls |
| `HAVE_MKSTEMP`, `HAVE_TMPFILE`, `HAVE_UNLINK` | no | a consequence worth knowing: **DCM and gzip-compressed disk images cannot be mounted**, because the core needs a temporary file to decompress them into. Plain ATR and XFD are unaffected |

### File layer redirection

There is no POSIX file API on bare metal, so `config.h` redirects the core's
stdio and directory calls onto `ATARI_VFS_*`, implemented over FatFs in
`src/port/vfs.c`:

```c
#define fopen    ATARI_VFS_Open
#define fread    ATARI_VFS_Read
/* ...and the rest */
```

`FILE` keeps its C library meaning and the VFS hands out its handles as
`FILE *`. This is explained in full in
[architecture.md](architecture.md#the-platform-contract); the short version is
that the core declares its handles as `FILE *`, so a distinct handle type would
be a type error at every declaration, while redefining the typedef collides with
`<stdio.h>`.

---

## OS ROM loading

This deserves its own section because the obvious approach is wrong, and the way
it goes wrong is silent.

The core holds a table of every Atari OS, BASIC and 5200 BIOS image ever shipped,
recording each one's length and CRC32. Its automatic OS selection walks an
ordered list of those entries and takes the first that has *either* data
compiled in *or* a filename registered.

The obvious way to register a file is upstream's own: pass it to
`SYSROM_SetPath()` with one of the four "custom" ROM IDs. Two things break:

1. **The custom slots have a NULL CRC**, so `SYSROM_SetPath()` accepts any file
   of the right length. A truncated or unrelated 16 KB file would load happily,
   and the machine would then fail in a way that looks like an emulator bug
   rather than a bad ROM.

2. **The custom IDs come last in every selection order**, after the built-in
   AltirraOS. Because `EMUOS_ALTIRRA` is defined, AltirraOS always has data, so
   it always wins — and the operator's own ROM files would **never be used**,
   while the log reported that they had been found.

So `src/port/platform.c` identifies each shipped file itself: it reads the file,
computes its CRC32 with the core's own `CRC32_FromFile()`, and looks for the
table entry whose length and checksum match. The file is then registered under
the ID it actually is, which:

- rejects a wrong or damaged file instead of loading it;
- clears any compiled-in data for that slot, so a real OS ROM takes precedence
  over AltirraOS while AltirraOS remains the fallback when no ROM file is
  present;
- lets the core's own selection logic pick the right revision.

The lookup stops at `SYSROM_LOADABLE_SIZE` so the built-in Altirra entries, which
have no checksum, can never be considered a match for a file.

The table of expected files and the range of IDs each could be is at the top of
the ROM section in `platform.c`; adding a ROM means adding one line there.

| File | Length | Recognised as |
|---|---|---|
| `ATARIXL.ROM` | 16384 | one of the XL/XE OS revisions |
| `ATARIOSA.ROM` | 10240 | one of the Atari 800 OS revisions |
| `ATARIOSB.ROM` | 10240 | one of the Atari 800 OS revisions |
| `ATARIBAS.ROM` | 8192 | one of the Atari BASIC revisions |
| `atari5200.rom` | 2048 | one of the 5200 BIOS revisions |

---

## Which sources are compiled

`Makefile.core` lists them. It mirrors upstream's `atari800_SOURCES` plus the
conditionals that apply to this configuration:

| Conditional | Adds |
|---|---|
| `WITH_SOUND` | `pokeysnd.c`, `mzpokeysnd.c`, `remez.c` |
| `WITH_SOUND_LIBATARI800` | `libatari800/sound.c` |
| `WITH_VIDEO_LIBATARI800` (**not** true here) | so `ui_basic.c` and `ui.c` **are** included |
| `WANT_EMUOS_ALTIRRA` | `roms/altirra*.c` |
| `WANT_NEW_CYCLE_EXACT` | `cycle_map.c` |

Plus `libatari800/statesav.c`, which provides the in-memory save-state backend
shared by the core's own save and restore code. `libatari800/api.c` and the rest
of that directory are **not** compiled: they are the wrapper this project
replaces.

`ui.c` and `ui_basic.c` are included only so that the core links and initialises
unchanged; the menu they implement is never reached, because
`src/port/ui_stub.c` supplies `UI_driver` and the UI entry points instead.

**Total: 51 object files**, about 325 KB of code and 1 MB including BSS.

---

## The one change to upstream

`src/libatari800/libatari800.h`, making the save-state buffer size overridable:

```c
#ifndef STATESAV_MAX_SIZE
#define STATESAV_MAX_SIZE 210000
#endif
```

**Why.** Upstream hard-codes 210000 bytes. The in-memory save-state backend stops
writing when the buffer is full and reports **no error**, so a snapshot of a
machine with more than 64 kB of RAM is silently truncated. The arithmetic:

| Emulated RAM | Extended RAM saved | Approximate state size | Fits in 210000? |
|---|---|---|---|
| 64 kB | 0 | 160 KB | yes |
| 128 kB | 80 KB | 237 KB | **no** |
| 192 kB | 128 KB | 300 KB | **no** |
| 320 kB | 256 KB | 435 KB | **no** |

`Makefile.core` passes `-DSTATESAV_MAX_SIZE=524288`, which covers a 320 kB
machine with room to spare.

This is the **only** modification to the Atari800 sources. It is stored as a
patch and applied by `scripts/fetch-sources.ps1`, so it survives re-fetching and
is visible to anyone reviewing the tree. The wrapper is written so that
upstream's value remains the default for every other user of the header.

---

## Crash containment

The 6502 core executes a `CIM` instruction (or an invalid escape opcode) by
`longjmp`-ing out of the frame to a buffer the host armed. If nothing armed it,
the jump would land nowhere and take the whole Raspberry Pi down with it.

`ATARI_RunFrame()` therefore wraps every frame in `setjmp`:

```c
s_frame_error = setjmp (libatari800_cpu_crash);
if (s_frame_error == 0) {
        Atari800_Frame ();
        /* check CPU_cim_encountered and ANTIC_dlist */
}
```

A broken Atari program then costs one frame, is reported through
`ATARI_LastError()`, and the kernel performs a warm reset. This is why
`HAVE_SETJMP` must stay defined: without it, the core calls `exit()` instead,
which on bare metal means the emulator stops dead.

---

## Runtime configuration

Every setting the OSD menu changes maps onto a public part of the core. This
table is the reference for anyone extending the menu.

| Setting | How it is changed | Needs a machine rebuild? |
|---|---|---|
| Machine type (XL / XE / XEGS) | `Atari800_SetMachineType()`, then `Atari800_InitialiseMachine()`, then `Atari800_Coldstart()` | yes |
| RAM size | `MEMORY_ram_size`, then `Atari800_InitialiseMachine()`, then `Atari800_Coldstart()` | yes |
| PAL / NTSC | `Atari800_SetTVMode()` | no |
| Artifacting | `ARTIFACT_Set()` | no |
| Volume | `POKEYSND_SetVolume()`, then `Sound_Setup()` and `Sound_Continue()` | no |
| Stereo | `Sound_desired.channels`, then `Sound_Setup()` and `Sound_Continue()` | no |
| Disk image | `SIO_Mount()` / `SIO_Dismount()` | no |
| Cartridge | `CARTRIDGE_Insert()` / `CARTRIDGE_Remove()` | no, but a cold start is applied so the user sees it immediately |
| Cassette | `CASSETTE_Insert()` / `CASSETTE_Remove()` | no |
| Snapshot | `LIBATARI800_StateSave()` / `LIBATARI800_StateLoad()` | no |

Two behaviours are worth recording because they are not obvious:

**RAM size.** Only the sizes `MEMORY_SizeValid()` accepts work: 64, 128, 192,
320 RAMBO and 320 Compy Shop. **256 is not among them** and has no `PORTB` case
in the core, which is why the menu does not offer it.

**Sound changes need `Sound_Setup()` again.** The POKEY mixer latches the volume
and the sample rate when it is initialised, and `Atari800_SetTVMode()` changes
the timing constants without reopening the audio device. So volume, stereo and
video-standard changes all re-run `Sound_Setup()` and `Sound_Continue()`
afterwards. This is also why `Sound_Setup()` must never be called per frame: it
re-runs the REMEZ filter design for the MZ POKEY engine.

---

## Screen format

Fixed and identical for PAL and NTSC: `Screen_atari` is 384x240 bytes, one byte
per pixel, each an Atari colour index. The visible region is columns 24..359
(`Screen_visible_x1` .. `Screen_visible_x2`). The TV standard changes the number
of scanlines in the frame, not the geometry of this buffer.

The 256-entry palette is `Colours_table`, packed `0x00RRGGBB`, refreshed by
`Colours_Update()` whenever the video standard or the colour settings change.

The core can draw a disk-activity LED and a speed meter straight into this
buffer. `ATARI_Init()` turns all of them off (`Screen_show_disk_led` and
friends), because the emulator should not graffiti the user's screen.

---

## Audio format

16-bit signed little-endian, interleaved for stereo. The default is mono at
44100 Hz. The sample rate is negotiated: `PLATFORM_SoundSetup()` may adjust the
requested rate and buffer size to what the device can do, and the core adapts.

Because a frame is not a whole number of samples, the core occasionally emits one
sample frame fewer (1470 bytes instead of 1472 for NTSC). This is deliberate
jitter compensation in upstream, not a bug, and it is why
`ATARI_AudioBytes()` must be used rather than assuming a fixed frame size.
