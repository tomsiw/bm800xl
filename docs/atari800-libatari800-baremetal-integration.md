# Atari800 core → bare-metal ARM (Raspberry Pi) integration report

**Source tree:** `external/atari800` (atari800/atari800 master, `AC_INIT(Atari800, 7.2.1)` — `configure.ac:24`)
**Method:** static read of the sources with `read`/`grep`/`glob`. **No file in `external/atari800` was modified and nothing was compiled/built.** All "computed" numbers below are derived from the code with the cited expressions; they are marked *(computed, not run)*.
**Target configuration assumed throughout:** the upstream `--target=libatari800` build semantics (`configure.ac:488-491`, `configure.ac:831-835`, `configure.ac:1017-1024`), i.e. `LIBATARI800` defined, `SOUND` defined, `SOUND_CALLBACK` **not** defined, no `GUI_SDL`/`BASIC`/`USE_CURSES`.

---

## 0. Executive summary (the facts that drive the port)

| Question | Answer | Evidence |
|---|---|---|
| Which host platform symbols must I write for the library to link? | **None.** The `libatari800` target ships complete stubs for every `PLATFORM_*` the core calls. The host only supplies libc/libm (+ optionally overrides). | `src/libatari800/{main,exit,input,video,sound}.c`; grep of all `PLATFORM_*` uses |
| Entry points | `libatari800_init()`, `libatari800_next_frame()`, `libatari800_exit()` | `src/libatari800/libatari800.h:269,277,307` |
| Framebuffer | `Screen_atari`, **1 byte/pixel palette index**, 384×240, stride 384 B, 92 160 B total, identical for PAL and NTSC | `src/screen.h:14-22`, `src/screen.c:45,148`, `src/antic.c:585,663-673,175,2876,3151` |
| Default display window | x = 24…359 (336 px, `Screen_visible_x1..x2`), y = 0…239 | `src/screen.c:62-65`, `src/screen.h:30-40` |
| Output PCM | 16-bit **signed** little-endian, **mono**, 44 100 Hz, ≈736 samples (1472 B) per NTSC frame / 885 (1770 B) per PAL frame, polled once per frame | `src/sound.c:43-48`, `src/libatari800/sound.c:51-70`, `src/mzpokeysnd.c:2308-2331` |
| Save state | whole machine to/from a host buffer, but the buffer is capped at **210 000 B**, which is **too small for ≥128 KB machines** | `src/libatari800/libatari800.h:122,147-157`, `src/statesav.c:650-689`, `src/memory.c:342-429` |
| 256 KB RAM option | **Does not exist.** Legal sizes are 8/16/24/32/40/48/52/64/128/192/320(RAMBO)/320(COMPY)/576/1088 | `src/memory.c:189-195`, `src/memory.c:757-776` |
| OS ROMs without a filesystem | Set `SYSROM_roms[id].data` to your embedded ROM image; `SYSROM_LoadImage()` `memcpy`s from it and never touches files. Altirra free ROMs are built in when `EMUOS_ALTIRRA=1` (default ON). | `src/sysrom.c:521-528`, `src/sysrom.c:84-116` |
| Frame timing | **You own it.** `Atari800_Sync()` is *not* compiled for libatari800 and neither is `Atari800_Frame()`'s main loop driver. | `src/atari.c:1230-1443` (`#ifndef LIBATARI800`), `src/atari.c:185` |
| Compile-time gotcha | For `--target=libatari800`, `NEW_CYCLE_EXACT` is **not** put into `config.h` although `cycle_map.c` **is** compiled (yes, this is an upstream inconsistency). | `configure.ac:831-835` vs `configure.ac:836-871`, `configure.ac:873`, `src/Makefile.am:240-242` |

---

## 1. Host platform contract

### 1.1 The platform-symbol surface (what the library needs, and where it is already provided)

Grep of every `PLATFORM_*` *use* in the files that are compiled for `--target=libatari800`:

| Symbol (exact prototype) | Used at | Provided by (libatari800 build) |
|---|---|---|
| `int PLATFORM_Initialise(int *argc, char *argv[])` | `src/atari.c:918` | `src/libatari800/main.c:77` |
| `int PLATFORM_Exit(int run_monitor)` | `src/atari.c:1162`, `src/monitor.c:2955`, `src/libatari800/main.c:115` | `src/libatari800/exit.c:39` |
| `int PLATFORM_Keyboard(void)` | `src/libatari800/api.c:215` | `src/libatari800/input.c:43` |
| `void PLATFORM_DisplayScreen(void)` | `src/libatari800/api.c:234` | `src/libatari800/video.c:33` (empty body) |
| `int PLATFORM_PORT(int num)` | `src/input.c:549,564` | `src/libatari800/input.c:425` |
| `int PLATFORM_TRIG(int num)` | `src/input.c:606` | `src/libatari800/input.c:438` |
| `int PLATFORM_Configure(char *option, char *parameters)` | `src/cfg.c:357` (only `#ifdef SUPPORTS_PLATFORM_CONFIGURE`) | `src/libatari800/main.c:67` |
| `void PLATFORM_ConfigSave(FILE *fp)` | `src/cfg.c:499` (only `#ifdef SUPPORTS_PLATFORM_CONFIGSAVE`) | `src/libatari800/main.c:72` |
| `int PLATFORM_SoundSetup(Sound_setup_t *setup)` | `src/sound.c:220` | `src/libatari800/sound.c:51` |
| `void PLATFORM_SoundExit(void)` | `src/sound.c:265` | `src/libatari800/sound.c:72` |
| `void PLATFORM_SoundPause(void)` | `src/sound.c:280` | `src/libatari800/sound.c:77` |
| `void PLATFORM_SoundContinue(void)` | `src/sound.c:292` | `src/libatari800/sound.c:81` |
| `unsigned int PLATFORM_SoundAvailable(void)` | `src/sound.c:360` (only `#ifndef SOUND_CALLBACK`) | `src/libatari800/sound.c:86` |
| `void PLATFORM_SoundWrite(UBYTE const *buffer, unsigned int size)` | `src/sound.c:372` (only `#ifndef SOUND_CALLBACK`) | `src/libatari800/sound.c:107` |
| `PLATFORM_SoundLock/Unlock` | `src/sound.c:387…493` | only exist under `SOUND_CALLBACK`; `platform.h:163-164` defines them as empty macros otherwise |
| `void PLATFORM_Sleep(double s)` / `double PLATFORM_Time(void)` | `src/util.c:553,579` | **only** if `SUPPORTS_PLATFORM_TIME` / `SUPPORTS_PLATFORM_SLEEP` are defined — see §1.3/§2 |
| `void PLATFORM_PaletteUpdate(void)` | `src/colours.c:187` | only under `SUPPORTS_PLATFORM_PALETTEUPDATE` (not defined for libatari800) |

Prototypes live in `src/platform.h:16-19, 31, 36, 49, 54, 123-160`.

**Conclusion:** in the stock `libatari800` configuration the host must provide **zero** platform symbols. There is no unresolved `PLATFORM_*`, `Sound_*`, `Screen_*`, `UI_*` or `Curse*` external in the core for this file set (verified by grepping all uses and matching each against a definition inside the compiled set; the only other `PLATFORM_*` definitions in the tree live in the non-compiled port files `atari_basic.c`, `atari_curses.c`, `atari_ps2.c`, `atari_rpi.c`, `atari_x11.c`, `sdl/*`, `dos/*`, `falcon/*`, `javanvm/*`, `amiga/*`, `dc/*`, `wince/*`, `android/*`).

`UI_*` needs are covered too: the only `UI_*` references in the compiled set are `UI_Initialise`, `UI_Run`, `UI_SelectCartType`, `UI_is_active`, `UI_alt_function`, `UI_atari_files_dir`, `UI_saved_files_dir`, `UI_n_atari_files_dir`, `UI_n_saved_files_dir`, `UI_show_hidden_files` (`src/atari.c:922,1030-1043,1454,1475`; `src/cfg.c:166-178,401-405`) plus `UI_crash_address/_afterCIM/_code` inside `#ifdef CRASH_MENU` (`src/cpu.c:2349-2352`, `src/esc.c:145-148` — `CRASH_MENU` is **off** for libatari800, `configure.ac:834`). All of them are defined by `src/libatari800/main.c:143-163`. If you delete that file you must supply exactly those twelve symbols yourself.

### 1.2 What the host *may* substitute (the useful override points)

*(A) Recommended for first bring-up: keep everything.* You implement no platform code at all; after each `libatari800_next_frame()` you read `libatari800_get_screen_ptr()` / `libatari800_get_sound_buffer()` and you get timing from your own timer. Downside: PCM is copied into the emulator's RAM buffer by `memcpy` (`src/libatari800/sound.c:109`), i.e. one extra copy per frame.

*(B) Replace `src/libatari800/video.c`* (drop it from the build) and provide:
```c
void PLATFORM_DisplayScreen(void);   /* platform.h:19 — called at the end of every next_frame */
```
Your implementation is a blit from `Screen_atari`. This is the natural place to do your palette→RGB / scaling, because it is called exactly once per emulated frame (`src/libatari800/api.c:234`).

*(C) Replace `src/libatari800/sound.c`* (drop it from the build) and provide the six `PLATFORM_Sound*` functions **plus** the four globals referenced by `src/libatari800/api.c` and `src/libatari800/statesav.c`-adjacent code, declared in `src/libatari800/sound.h:7-13`:
```c
extern UBYTE *LIBATARI800_Sound_array;       /* api.c:342 returns this */
extern unsigned int sound_array_fill;        /* api.c:351 */
extern unsigned int sound_hw_buffer_size;    /* api.c:361 */
extern double sample_residual;               /* api.c:461,481 (state save/restore) */
```
Keep the contract from `src/platform.h:107-166`: `PLATFORM_SoundSetup` may *modify* `*setup` (that is how the stock backend sets `buffer_frames`, `src/libatari800/sound.c:57-58`), must return TRUE/FALSE, output starts paused and `PLATFORM_SoundContinue()` is called to start it. If you want true push/DMA audio, this is where you push; then you do not need the 4 globals only if you also stop using `libatari800_get_sound_buffer*()` (your own copy of `api.c` then).

*(D) Set `SUPPORTS_PLATFORM_TIME`* (and optionally `SUPPORTS_PLATFORM_SLEEP`) in `config.h` and provide:
```c
double PLATFORM_Time(void);   /* platform.h:54; used by Util_time(), util.c:553 */
void   PLATFORM_Sleep(double s); /* platform.h:49; used by Util_sleep(), util.c:579 */
```
**This is effectively mandatory** — see §1.3 / §8 (R1): `util.c:568` is a `#error` if none of the time sources is available.

### 1.3 C runtime contract (the real dependency)

Init path (`libatari800_init → Atari800_Initialise → …`) reaches, unconditionally for this configuration:

* `Util_malloc/realloc/free` → `malloc/realloc/free` (`util.c:295-315`)
* `memcpy/memset/memmove/memcmp/strcpy/strncpy/strlen/strcmp/strncmp/strchr/strrchr/strstr/strtod/tolower` (throughout)
* `snprintf`/`sprintf` (`util.c:363`, `devices.c:1891`), `vsnprintf` (`log.c:56`, guarded by `HAVE_VSNPRINTF` — **must be enabled**, otherwise `log.c:58` uses `vsprintf` into an 8 192-byte stack buffer), `printf` (`log.c:42` `PRINT`, `util.c:300,311`)
* `fopen/fclose/fgets/fread/fseek/ftell` — `CFG_LoadConfig` (`cfg.c:108`), `SYSROM_FindInDir` (`sysrom.c:304`), `Atari800_LoadImage` (`atari.c:306`, only if a ROM is not embedded)
* `getenv("HOME")` — `Util_GetHomeDir` (`util.c:629`) called from `cfg.c:99` on the default-config path
* `opendir/readdir/closedir` — `SYSROM_FindInDir` (`sysrom.c:293-358`), called unconditionally from `atari.c:488`; **`DIR`/`struct dirent` are used unconditionally but `<dirent.h>` is included only under `HAVE_DIRENT_H`** (`sysrom.c:28-30`, same pattern `devices.c:58-60` with unconditional use at `devices.c:237-276`, `atari.c` only inside `#ifdef HAVE_DOWNLOAD`) → you must provide a `dirent.h`-compatible header (or patch `sysrom.c`/`devices.c`). `opendir()` returning `NULL` is handled gracefully (`sysrom.c:293-294`, `devices.c:245`).
* `exit()` — `util.c:301,312` (OOM), `colours.c:151` (invalid `Atari800_tv_mode`)
* `rand()` — `mzpokeysnd.c:2297,2301,2322,2326,2363,2371` (dither noise; no `srand()` call anywhere in the file)
* `setjmp/longjmp` — **define `HAVE_SETJMP`** (`libatari800/api.c:57,217-223`, `src/libatari800/cpu_crash.h:8`, `cpu.c:2356`, `esc.c:152`). Without it an invalid ESC opcode falls through to `Atari800_Exit(TRUE); exit(0);` (`esc.c:155-157`) and kills the application.
* `fopen/fclose` on the config path must fail *cleanly* (`cfg.c:109` handles `NULL` and returns FALSE). `SYSROM_FindInDir`'s `fopen` failure is handled (`sysrom.c:304-306`).
* libm (`-lm` is added by upstream for this target, `configure.ac:399`): `modf` (`pokeysnd.c:1069`, `mzpokeysnd.c:2344`), `ceil` (`libatari800/sound.c:58`, `pokeysnd.c:276`), `floor` (`util.c:291`, `mzpokeysnd.c:2296`), `pow` (`colours.c:134`, `colours_ntsc.c`/`colours_pal.c`), `fabs` (`util.h:103`, `remez.c`), `cos`/`sin` (`remez.c:169,233,419-448,572-625`).
* `tmpfile`/`mkstemp`/`tmpnam`/`unlink` — only for compressed (DCM/GZ) disk images, see R6.

Not needed: pthreads, sockets, termios, `select`, `usleep`, `nanosleep`, `system`, `popen`, `signal` (all only in files or `#ifdef`s that are off in this configuration — see §3 and §8).

---

## 2. Compile-time configuration (recommended `config.h`)

There is **no `src/config.h.in`** in the tree (autoheader generates it; `AC_CONFIG_HEADERS([src/config.h])`, `configure.ac:27`). Hand-written `config.h` templates exist for other ports and are the best-documented reference for the full autoheader symbol set: `src/dc/config.h`, `src/amiga/config.h`, `src/wince/port/config.h`. `src/Makefile.am` and every source do `#include "config.h"`, so you must also pass `-I<src dir>`.

### 2.1 Reference set produced by `./configure --target=libatari800`

From `configure.ac` (libatari800 branch and the unconditional option blocks):

```
LIBATARI800=1                     (configure.ac:489)
SUPPORTS_PLATFORM_CONFIGURE=1     (configure.ac:490)
BUFFERED_LOG=1                    (configure.ac:887)
SOUND=1                           (configure.ac:1110)   [sound can NOT be disabled: configure.ac:1022-1023 errors out]
NONLINEAR_MIXING=1                (configure.ac:1116-1119)
INTERPOLATE_SOUND=1               (configure.ac:1121-1124)
STEREO_SOUND=1                    (configure.ac:1126-1129)
CONSOLE_SOUND=1                   (configure.ac:1131-1134) / WANT_CONSOLE_SOUND=yes (configure.ac:1021)
PBI_MIO=1, PBI_BB=1               (configure.ac:970-979)
PBI_XLD=1, VOICEBOX=1             (configure.ac:1140-1147)
AUDIO_RECORDING=1                 (configure.ac:1148-1151)
VIDEO_RECORDING=1, SCREENSHOTS=1  (configure.ac:1210-1218, evaluated because with_video=check != no)
VIDEO_CODEC_ZMBV=1 [,VIDEO_CODEC_PNG, AUDIO_CODEC_MP3, IMAGE_CODEC_PNG if libs found]
IDE=1                             (configure.ac:1271-1274)
POKEYREC=1                        (configure.ac:1282-1285)
EMUOS_ALTIRRA=1                   (configure.ac:895-898)
NETSIO=1 if the build host has pthread  (configure.ac:982-1000)
HAVE_SETJMP, HAVE_* (autoheader AC_CHECK results)
and NOT: SOUND_CALLBACK (configure.ac:1020), NEW_CYCLE_EXACT (see below), CRASH_MENU, MONITOR_*,
         PAGED_ATTRIB, CYCLES_PER_OPCODE, SUPPORTS_PLATFORM_{SLEEP,TIME,CONFIGINIT,PALETTEUPDATE},
         SUPPORTS_CHANGE_VIDEOMODE, SUPPORTS_ROTATE_VIDEOMODE, PLATFORM_MAP_PALETTE,
         GUI_SDL, BASIC, USE_CURSES, XEP80_EMULATION, NTSC_FILTER, PAL_BLENDING, R_IO_DEVICE,
         DOWNLOAD/HAVE_DOWNLOAD, AF80, BIT3, PBI_PROTO80, WORDS_UNALIGNED_OK (on arm*)
```

**Two traps in that reference set:**

1. `NEW_CYCLE_EXACT` is *never* `AC_DEFINE`d for `--target=libatari800` — the `A8_OPTION(newcycleexact)` call lives in the `elif` branch that a libatari800 target never enters (`configure.ac:831` vs `836-871`). But `AM_CONDITIONAL([WANT_NEW_CYCLE_EXACT])` is built from the variable set at `configure.ac:832`, and that is `yes`, so `src/Makefile.am:240-242` adds `cycle_map.c` to the build. Net effect upstream: `cycle_map.c` is compiled but never called (`ANTIC_cpu2antic_ptr` etc. only exist under `#ifdef NEW_CYCLE_EXACT`, `antic.c:58-71`), wasting two `int[14760]` arrays (~118 KB of BSS, `cycle_map.c:28-29`). **Decide explicitly:** either `#define NEW_CYCLE_EXACT 1` and compile `cycle_map.c` (accurate intra-scanline colour changes, more CPU), or leave the macro off and *do not compile* `cycle_map.c`.
2. `USE_COLOUR_TRANSLATION_TABLE` is never defined by `configure` (grep over the whole tree: only source usages, `gtia.h:100-111`, `antic.c`, `ui_basic.c`, and one legacy X11 CFLAGS block in `atari_x11.c:107`). **Leave it undefined** — that is what makes the framebuffer 1 byte/pixel. If you define it, `ANTIC_cl[]`/`GTIA_colour_translation_table[]` become 16-bit RGB565-ish words and each pixel occupies 2 bytes (`antic.c:585`, `gtia.h:100-104`), which the SDL/X11/DOS ports do.

### 2.2 Recommended bare-metal `config.h` (drop-in)

```c
#ifndef CONFIG_H
#define CONFIG_H

/* --- package identity (atari.h:12 uses PACKAGE_VERSION) --- */
#define PACKAGE_VERSION "7.2.1"
#define PACKAGE_STRING  "Atari800 7.2.1"
#define PACKAGE_NAME    "Atari800"
#define PACKAGE_BUGREPORT "pstehlik@sophics.cz"
#define PACKAGE_TARNAME "atari800"
#define PACKAGE         "atari800"
#define VERSION         "7.2.1"

/* --- target --- */
#define LIBATARI800 1
#define SUPPORTS_PLATFORM_CONFIGURE 1     /* libatari800/main.c provides PLATFORM_Configure */

/* --- video: byte-per-pixel framebuffer, full colour emulation --- */
#define EMUOS_ALTIRRA 1                   /* built-in free OS/BASIC so no ROM files needed */
/* #define NEW_CYCLE_EXACT 1 */          /* see §2.1 trap 1; if set, compile cycle_map.c */
#undef  PAGED_ATTRIB
#undef  CYCLES_PER_OPCODE
#undef  CRASH_MENU                      /* MUST stay off: UI_crash_* live in the excluded ui.c */
#undef  CURSES_BASIC
#undef  BASIC
#undef  GUI_SDL
#undef  USE_CURSES
#undef  USE_NCURSES
#undef  DIRTYRECT                       /* Android-only; off => no Screen_dirty at all */
#undef  BITPL_SCR
#undef  SUPPORTS_CHANGE_VIDEOMODE
#undef  PLATFORM_MAP_PALETTE
#undef  SUPPORTS_PLATFORM_PALETTEUPDATE

/* --- sound: full POKEY, but no file recording --- */
#define SOUND 1
#define NONLINEAR_MIXING 1
#define INTERPOLATE_SOUND 1
#define STEREO_SOUND 1
#define CONSOLE_SOUND 1
#undef  CLIP_SOUND
#undef  SOUND_CALLBACK                 /* keep the pull interface (PLATFORM_SoundAvailable/Write) */
#undef  AUDIO_RECORDING
#undef  VIDEO_RECORDING
#undef  SCREENSHOTS                    /* off => no file_export.c, no codecs/*.c */

/* --- optional device emulation: keep only what you need --- */
#undef  IDE                            /* ide.c: fopen/fseeko/ftello, HAVE_INTTYPES_H */
#undef  POKEYREC                       /* pokeyrec.c: debug-register dump to a file */
#undef  PBI_MIO
#undef  PBI_BB                         /* pbi_bb.c:66-68 dereferences a NULL FILE on failure */
#undef  PBI_XLD
#undef  VOICEBOX                       /* off => votrax.c/votraxsnd.c/vtxsmpls.inc not needed */
#undef  NETSIO                         /* needs pthread + sockets */
#undef  R_IO_DEVICE                    /* needs sockets */
#undef  R_NETWORK
#undef  R_SERIAL
#undef  XEP80_EMULATION
#undef  AF80
#undef  BIT3
#undef  PBI_PROTO80
#undef  NTSC_FILTER
#undef  PAL_BLENDING
#undef  EVENT_RECORDING
#undef  HAVE_DOWNLOAD
#undef  LINUX_JOYSTICK
#undef  USE_UI_BASIC_ONSCREEN_KEYBOARD

/* --- monitor: none of it is wanted, no readline/ANSI/UTF8 --- */
#undef  MONITOR_ASSEMBLER
#undef  MONITOR_BREAK
#undef  MONITOR_BREAKPOINTS
#undef  MONITOR_HINTS
#undef  MONITOR_PROFILE
#undef  MONITOR_TRACE
#undef  MONITOR_ANSI
#undef  MONITOR_UTF8
#undef  MONITOR_READLINE

/* --- libc/libm feature selection for a bare-metal newlib --- */
#define HAVE_SETJMP 1                  /* REQUIRED: avoids exit(0) in esc.c:155-157 */
#define HAVE_VSNPRINTF 1               /* REQUIRED: else log.c:58 uses vsprintf into 8 KB */
#define HAVE_STRNCPY 1                 /* newlib strncpy is fine; avoids util.c:116 fallback */
#define HAVE_STRERROR 1
#define HAVE_ERRNO_H 1
#define HAVE_VPRINTF 1
#define HAVE_DIRENT_H 1                /* you MUST supply <dirent.h>, see §1.3 */
#define HAVE_CLOCK 1                   /* or SUPPORTS_PLATFORM_TIME, see below */
#undef  HAVE_GETTIMEOFDAY
#undef  HAVE_UCLOCK
#undef  SUPPORTS_PLATFORM_TIME         /* alternative: define + implement PLATFORM_Time() */
#undef  SUPPORTS_PLATFORM_SLEEP        /* define + implement PLATFORM_Sleep() to avoid busy-wait */
#undef  HAVE_GETCWD                    /* off => Util_getcwd() returns "." (util.c:620-623) */
#undef  HAVE_STAT
#undef  HAVE_RENAME
#undef  HAVE_MKDIR
#undef  MKDIR_TAKES_ONE_ARG
#undef  HAVE_RMDIR
#undef  HAVE_CHMOD
#undef  HAVE_UNLINK
#undef  HAVE_TMPFILE                   /* see R6 before enabling DCM/GZ disk images */
#undef  HAVE_TMPNAM
#undef  HAVE_MKSTEMP
#undef  HAVE_FDOPEN
#undef  HAVE_MKTEMP
#define HAVE_ROUND 1                   /* newlib has round(); else util.c:289 uses floor() */
#define HAVE_SNPRINTF 1
#undef  HAVE_SYSTEM                    /* off => no system()/popen() in devices.c/monitor.c */
#undef  HAVE_SIGNAL
#undef  HAVE_SELECT
#undef  HAVE_NANOSLEEP
#undef  HAVE_USLEEP
#undef  HAVE_LIBZ
#undef  HAVE_LIBPNG
#undef  HAVE_OPENGL
#undef  HAVE_WINDOWS_H
#undef  HAVE_UNISTD_H                  /* unless your newlib really has usable unistd.h */
#undef  HAVE_STRINGS_H
#undef  HAVE_SYS_TIME_H
#undef  HAVE_TIME_H
#undef  HAVE_IO_H
#undef  HAVE_DIRECT_H
#undef  HAVE_FCNTL_H
#undef  HAVE_FILE_H
#undef  HAVE_UNIXIO_H

/* --- machine model: little-endian 32-bit ARM --- */
#undef  WORDS_BIGENDIAN
#undef  WORDS_UNALIGNED_OK             /* keep off for safety; costs speed (memory.h:12-32) */
#undef  DIR_SEP_BACKSLASH
#undef  DOS_DRIVES
#undef  SYSTEM_WIDE_CFG_FILE
/* #define DEFAULT_CFG_NAME ".atari800.cfg"  */  /* cfg.h:23-25 default is fine */
#undef  DREAMCAST
#undef  PS2
#undef  RPI
#undef  ANDROID
#undef  X11
#undef  SDL
#undef  SDL2
#undef  FALCON
#undef  CLOSEDIR_VOID
#undef  STDC_HEADERS
#undef  HAVE_WCHAR_H

#endif
```

**Macro-combination warnings**

* `CRASH_MENU` must be **off**: with it on, `cpu.c:2349-2352` and `esc.c:145-148` reference `UI_crash_address/_afterCIM/_code`, which are defined only in the excluded `ui.c:134-136` → link error.
* `SOUND_CALLBACK` must be **off**, otherwise `Sound_Callback()` (called from a *thread* you would have to own) is required and `PLATFORM_SoundLock/Unlock` become real functions (`platform.h:143-166`, only `sdl/`, `dos/` and `javanvm/` implement them).
* `PAL_BLENDING` needs `PLATFORM_GetPixelFormat` + `PLATFORM_MapRGB` (`pal_blending.c:85-95`, `platform.h:92-105`), i.e. a 16-bit display surface. Do not enable it for a byte-per-pixel framebuffer.
* `NTSC_FILTER` drags in `filter_ntsc.c` + `atari_ntsc/*` and expects a high-res/16-bit output path.
* `DIRTYRECT` doubles the video RAM (`Screen_dirty`, `screen.c:47,152`) and is only used by the Android port; leave off.
* `AUDIO_RECORDING`/`VIDEO_RECORDING`/`SCREENSHOTS` off is what removes `file_export.c` + the whole `codecs/` directory; they are all fopen-based.
* `EMUOS_ALTIRRA` is a plain `#if EMUOS_ALTIRRA` in `sysrom.h:61` and `sysrom.c:43,110,174` — if you leave it undefined it evaluates to 0, which is fine, but you then have **no OS ROM at all** without files.
* `SUPPORTS_PLATFORM_TIME`/`SLEEP`: if you define them, `src/platform.h:49,54` declare `PLATFORM_Sleep`/`PLATFORM_Time` and *you* must implement them (§1.2-D). If you define neither them nor one of `HAVE_GETTIMEOFDAY/HAVE_UCLOCK/HAVE_CLOCK/HAVE_WINDOWS_H/DJGPP`, `util.c:568` fails the build with `#error No function found for Util_time()`.
* `WORDS_UNALIGNED_OK`: `configure.ac:562` disables it for `arm*` (enables for `aarch64*`). It changes `MEMORY_dGetWord` to a raw unaligned 16-bit load (`memory.h:13-17`) — works on ARMv7-A normal memory but will fault on some ARM configurations; keep it off for the first port.

---

## 3. Exact `.c` files to compile

Files below are taken from `src/Makefile.am` (`atari800_SOURCES` at lines 57-88, the `libatari800_a_SOURCES` list at lines 24-34, and the conditional blocks), cross-checked against the `AM_CONDITIONAL`s in `configure.ac`.

### 3.1 `src/libatari800/` — 8 files (always)
| File | Why |
|---|---|
| `api.c` | the public API (`libatari800_init/next_frame/exit/get_*`) — `Makefile.am:26` |
| `main.c` | `LIBATARI800_Frame`, `PLATFORM_Initialise/Configure/ConfigSave`, UI stubs — `Makefile.am:28` |
| `init.c` | `LIBATARI800_Initialise`, config hook — `Makefile.am:29` |
| `exit.c` | `PLATFORM_Exit` — `Makefile.am:30` |
| `input.c` | `PLATFORM_Keyboard/PORT/TRIG`, `LIBATARI800_Mouse` — `Makefile.am:31` |
| `video.c` | `PLATFORM_DisplayScreen` (empty) + `LIBATARI800_Video_Initialise` — `Makefile.am:32` |
| `statesav.c` | `LIBATARI800_StateSave/StateLoad` buffer plumbing — `Makefile.am:33` |
| `sound.c` | `PLATFORM_SoundSetup/Exit/Pause/Continue/Available/Write` — `Makefile.am:34` (also re-listed at `:128`) |

### 3.2 `src/` — 28 files unconditional + 13 conditional = 41 files
Unconditional (`Makefile.am:57-88`):
`acidtest.c`, `afile.c`, `antic.c`, `atari.c`, `binload.c`, `cartridge.c`, `cartridge_info.c`, `cassette.c`, `compfile.c`, `cfg.c`, `cpu.c`, `crc32.c`, `devices.c`, `esc.c`, `gtia.c`, `img_tape.c`, `log.c`, `memory.c`, `monitor.c`, `pbi.c`, `pia.c`, `pokey.c`, `rtime.c`, `sio.c`, `sysrom.c`, `util.c`
plus `roms/altirra_5200_os.c`, `roms/altirra_5200_charset.c` (`Makefile.am:83-84`).

Conditional, **all ON** in the recommended configuration:
| Files | Conditional | configure.ac / Makefile.am |
|---|---|---|
| `pokeysnd.c`, `mzpokeysnd.c`, `remez.c` | `WITH_SOUND` (always true here, `configure.ac:1095`) | `:103-108` |
| `sound.c` | `WITH_SOUND_LIBATARI800` (`with_sound=libatari800`) | `:127-129`, `configure.ac:1019,1102` |
| `input.c`, `statesav.c` | `!WITH_VIDEO_NO` (video backend is "libatari800") | `:227` |
| `artifact.c`, `colours.c`, `colours_ntsc.c`, `colours_pal.c`, `colours_external.c`, `screen.c` | `!WITH_VIDEO_NO && !WANT_CURSES_BASIC` | `:231-239` |
| `cycle_map.c` | `WANT_NEW_CYCLE_EXACT` (=yes even for libatari800) | `:240-242`, `configure.ac:832,873` |
| `roms/altirraos_800.c`, `roms/altirraos_xl.c`, `roms/altirra_basic.c` | `WANT_EMUOS_ALTIRRA` | `:246-251`, `configure.ac:900` |

**Total: 8 + 28 + 5 + 8 (+1 cycle_map if `NEW_CYCLE_EXACT`) = 49–50 files.**
(`remez.c` is *not* optional while MZ POKEY is on: `MZPOKEYSND_Init` computes its FIR filter at runtime through `remez_filter_table()` → `REMEZ_CreateFilter()` (`mzpokeysnd.c:1370-1376`, `remez.c:513`).)

### 3.3 Optional files (only if you enable the matching macro)
| Feature | Files | Blocking dependency (all in-code) |
|---|---|---|
| `IDE` | `ide.c` | `fopen("rb+")`, `fseeko/ftello`, `HAVE_INTTYPES_H` (`ide.c:210-221`, `ide_internal.h:4-8`); `IDE_Initialise` runs during init (`atari.c:885-887`) |
| `POKEYREC` | `pokeyrec.c` | `fopen` only with `-pokeyrec` (`pokeyrec.c:113-118`); `POKEYREC_Initialise` runs during init |
| `PBI_MIO` / `PBI_BB` / either | `pbi_mio.c`, `pbi_bb.c`, `pbi_scsi.c` | `fopen` + `Atari800_LoadImage`; `pbi_bb.c:66-68` does `Util_flen(fopen(...))` with no NULL check |
| `PBI_XLD`/`VOICEBOX` | `pbi_xld.c`, `voicebox.c`, `votrax.c`, `votraxsnd.c` | `Atari800_LoadImage` for the XLD ROMs (`pbi_xld.c:83-104`); `votraxsnd.c` `#include`s the 387 KB `vtxsmpls.inc` |
| `SCREENSHOTS`/`VIDEO_RECORDING`/`AUDIO_RECORDING` | `file_export.c` + `codecs/image.c`, `codecs/image_pcx.c`, `codecs/container.c`, `codecs/container_wav.c`, `codecs/container_avi.c`, `codecs/audio.c`, `codecs/audio_pcm.c`, `codecs/audio_adpcm.c`, `codecs/audio_mulaw.c`, `codecs/video.c`, `codecs/video_mrle.c`, `codecs/video_zmbv.c` (+ `codecs/image_png.c`, `codecs/video_mpng.c` if libpng, + `codecs/audio_mp3.c`/`container_mp3.c` if libmp3lame) | `fopen` (`file_export.c:382`, `codecs/container.c:219`), libpng/lame |
| `NETSIO` | `netsio.c` | pthread + UDP sockets (`netsio.c:257,613`), and `configure` refuses to build without pthread (`configure.ac:990-996`) |
| `R_IO_DEVICE` | `rdevice.c` | sockets (`rdevice.c:756,1242`) |
| `XEP80_EMULATION` | `xep80.c`, `xep80_fonts.c` | `fopen` of a charset file (`xep80_fonts.c:1728`) |
| `NTSC_FILTER` | `filter_ntsc.c`, `atari_ntsc/atari_ntsc.c` | high-res/16-bit output path |
| `PAL_BLENDING` | `pal_blending.c` | needs `PLATFORM_GetPixelFormat/MapRGB` |

### 3.4 Files that must be EXCLUDED and why
* **`ui.c`, `ui_basic.c`** — excluded by design for this target (`Makefile.am:228-230`: added only `if !WITH_VIDEO_LIBATARI800`). They are the whole text/on-screen UI, use `opendir/getcwd/fopen/FILE*` dialogues and define the UI globals that `src/libatari800/main.c:143-163` stubs instead.
* **`videomode.c`** — only under `WITH_VIDEO_SDL`/`SDL2` (`Makefile.am:193-213`); it would require `PLATFORM_SetVideoMode` etc.
* **`atari_basic.c`** — only for `--with-video=no` (`Makefile.am:223-224`). Note: for `--target=libatari800` you must **not** pass `--with-video=no`, because `AM_CONDITIONAL(WITH_VIDEO_NO)` (`configure.ac:811`) would then be true and `Makefile.am:225-244` would drop `input.c`, `statesav.c`, `screen.c` and all `colours*.c` while compiling `atari_basic.c` — the library would not link.
* **Platform backends** — `atari_curses.c`, `atari_x11.c`, `atari_ps2.c`, `atari_rpi.c`, `sdl/*` (incl. `SDL_win32_main.c`), `dos/*`, `falcon/*`, `javanvm/*`, `amiga/*`, `dc/*`, `wince/*`, `android/*`.
* **Sound backends** — `sound_oss.c`, `sdl/sound.c`, `falcon/sound.c`, `dos/sound_dos.c`, `dos/dos_sb.c`, `javanvm/sound.c`.
* **`netsio.c`/`netsiowin.c`**, **`rdevice.c`**, **`download.c`**, **`af80.c`**, **`bit3.c`**, **`pbi_proto80.c`** (the last three only in the SDL video block, `Makefile.am:201,212`).
* **`file_export.c` + `codecs/*.c`** when recording/screenshots are off.
* **`joycfg.c`, `mkimg.c`** — `EXTRA_DIST` helper programs, never in `atari800_SOURCES` (`Makefile.am:397`).
* **`libatari800/guess_settings.c`, `libatari800/libatari800_test.c`** — `noinst_PROGRAMS` (`Makefile.am:35-41`), not part of `libatari800.a`.
* **`vtxsmpls.inc`** — a C *data include* pulled in by `votraxsnd.c`, not a translation unit.

---

## 4. Runtime configuration without re-linking

All of the following are plain exported globals (declared `extern` in a header, defined non-`static` in a `.c`), so a host linked against the library can poke them directly. Where a lighter call exists I give it; otherwise I say "full re-init".

### (a) RAM size — `MEMORY_ram_size`
* Declaration: `src/memory.h:47` `extern int MEMORY_ram_size;` (definition `src/memory.c:49`, default 64).
* Legal values (`MEMORY_SizeValid`, `src/memory.c:189-195`): 8, 16, 24, 32, 40, 48, 52, 64, 128, 192, `MEMORY_RAM_320_RAMBO` (=320), `MEMORY_RAM_320_COMPY_SHOP` (=321), 576, 1088.
* **`256` is not a legal value** and has no `switch` case in `MEMORY_HandlePORTB` (`memory.c:757-776`) → 256 KB banking would be broken. Use 192 or 320 RAMBO instead.
* Consumed by: `AllocXEMemory` (`memory.c:154-174`), `MEMORY_InitialiseMachine` (`memory.c:197-...`), `HandlePORTB` (`memory.c:751-805`), `load_roms` (`atari.c:323`), `MEMORY_StateSave` (`memory.c:411-429`).
* To change: set `MEMORY_ram_size`, then `Atari800_InitialiseMachine()` (`atari.h:144`, `atari.c:353`) — which re-runs `load_roms()` + `MEMORY_InitialiseMachine()` + `Devices_UpdatePatches()` — then `Atari800_Coldstart()` (`atari.h:143` comment: "You should call Atari800_Coldstart() after it"). **No `libatari800_exit()`+`init()` cycle required.**
* Equivalent no-filesystem init-time route: pass `-atari|-1200|-xl|-xe|-320xe|-rambo|-576xe|-1088xe|-xegs|-5200` in the `argv` you hand to `libatari800_init` (`atari.c:515-612`). E.g. `-xe` sets `MACHINE_XLXE` + `MEMORY_ram_size = 128` (`atari.c:545-547`), `-rambo` sets `MEMORY_RAM_320_RAMBO` (`atari.c:567`).
* Config-file strings (if you ever have a file): `RAM_SIZE=320 (RAMBO)` / `320 (COMPY SHOP)` / a number (`cfg.c:258-270`).

### (b) XL vs XE machine type — `Atari800_machine_type` + friends
* Declaration: `src/atari.h:51` `extern int Atari800_machine_type;` values `Atari800_MACHINE_800/XLYE/5200` (`atari.h:43-49`); setter `void Atari800_SetMachineType(int type)` (`atari.h:52`, `atari.c:209-220`) — the header explicitly says "Don't change this variable directly".
* There is **no separate XL and XE machine type**: both are `Atari800_MACHINE_XLXE`. The difference is RAM size + `Atari800_builtin_basic` + `Atari800_keyboard_leds` + `Atari800_f_keys` (`atari.h:58-82`): `-xl` = XLXE/64 KB/basic on (`atari.c:535-544`), `-xe` = XLXE/128 KB/basic on (`atari.c:545-554`), `-xegs` sets `Atari800_builtin_game = TRUE` (`atari.c:602`).
* Related exported knobs: `Atari800_builtin_basic`, `Atari800_keyboard_leds`, `Atari800_f_keys`, `Atari800_jumper` (+`Atari800_UpdateJumper()`), `Atari800_builtin_game`, `Atari800_keyboard_detached` (+`Atari800_UpdateKeyboardDetached()`), `Atari800_disable_basic`, `Atari800_os_version` (`atari.h:58-102`).
* To change: `Atari800_SetMachineType(...)`, set the flags above, then **`Atari800_InitialiseMachine()` + `Atari800_Coldstart()`** (the header comment at `atari.h:54`). No re-init cycle needed.
* Config strings: `MACHINE_TYPE=Atari 400/800|Atari XL/XE|Atari 5200` (`cfg.c:245-257`), plus `BUILTIN_BASIC`, `KEYBOARD_LEDS`, `F_KEYS`, `BUILTIN_GAME`, `KEYBOARD_DETACHED`, `1200XL_JUMPER` (`cfg.c:295-306`).

### (c) PAL vs NTSC — `Atari800_tv_mode`
* Declaration: `src/atari.h:95` `extern int Atari800_tv_mode;` values `Atari800_TV_PAL` = 312 and `Atari800_TV_NTSC` = 262 (`atari.h:86-87`); NTSC is default-clear, the library starts in PAL (`atari.c:173` initialises it to `Atari800_TV_PAL`).
* Use the setter `void Atari800_SetTVMode(int mode)` (`atari.h:197`, `atari.c:1717-1733`). It updates `Colours_SetVideoSystem()` (palette), `ARTIFACT_SetTVMode()`, and — importantly — re-runs `POKEYSND_Init(..., Sound_out.freq, Sound_out.channels, ...)` (`atari.c:1728-1731`) so the audio resampler's ticks-per-sample match the new frame rate.
* **Caveat:** `Atari800_SetTVMode` does *not* call `Sound_Setup()`/`PLATFORM_SoundSetup()`, so the libatari800 backend's `sound_hw_buffer_size` and its `sample_diff`/`sample_residual` jitter arithmetic (computed from the *old* refresh rate in `libatari800/sound.c:56-67`) stay stale. For a runtime switch, follow it with `Sound_Setup(); Sound_Continue();` (`src/sound.h:66`, `src/sound.c:204,285`) — that also recomputes `process_buffer_size` (`sound.c:240-242`) and re-runs the POKEY filter design.
* Init-time: `-pal` / `-ntsc` (`atari.c:622-625`); config string `DEFAULT_TV_MODE=PAL|NTSC` (`cfg.c:271-278`).
* No `libatari800_exit()`/`init()` cycle needed.

### (d) Stereo / mono — `Sound_desired.channels` (not `POKEYSND_stereo_enabled` alone!)
* The authoritative knob for this backend is `extern Sound_setup_t Sound_desired;` (`src/sound.h:47`), field `.channels` (1 = mono, 2 = stereo, `sound.h:30-31`; default 1, `sound.c:43-48`). `Sound_Setup()` copies it to `Sound_out` (`sound.c:219`), passes `Sound_out.channels` as `num_pokeys` to `POKEYSND_Init` (`sound.c:245`) and *derives* `POKEYSND_stereo_enabled = (Sound_out.channels == 2)` (`sound.c:238`).
* `int POKEYSND_stereo_enabled` (`src/pokeysnd.h:88`, defined `pokeysnd.c:153`) by itself only affects POKEY register mirroring (`pokey.c:113,214`) and the legacy Ron-Fries mixer (`pokeysnd.c:650-1018`); the **MZ** mixer uses the channel count (`mzpokeysnd.c:1390` `num_cur_pokeys = num_pokeys`). Therefore passing `-stereo` to `libatari800_init` (`atari.c:634-639` sets only `POKEYSND_stereo_enabled`) is **overridden again by `Sound_Setup()`** and produces mono.
* Recipes: (1) before `libatari800_init`, set `Sound_desired.channels = 2;` (and optionally `POKEYSND_stereo_enabled = TRUE`); or (2) at runtime: `Sound_desired.channels = 2; Sound_Setup(); Sound_Continue();`. Config-file route: `STEREO_POKEY=1` sets *both* (`cfg.c:234-238`).
* Related: `int POKEYSND_num_pokeys` (`pokeysnd.h:83`), `int POKEYSND_enable_new_pokey` (`pokeysnd.h:87`, default TRUE = MZ engine, `pokeysnd.c:145`; config `ENABLE_NEW_POKEY`, `cfg.c:229-232`), `int POKEYSND_snd_flags` (`pokeysnd.h:84`, bit `POKEYSND_BIT16`, `pokeysnd.h:80`).

### (e) Sound volume — `POKEYSND_volume` / `POKEYSND_SetVolume()`
* `extern int POKEYSND_volume;` (`src/pokeysnd.h:85`, default `0x100` = 256, `pokeysnd.c:156`); setter `void POKEYSND_SetVolume(int vol)` with `0..100` clamped (`pokeysnd.h:111`, `pokeysnd.c:1027-1035`) → stores `vol * 0x100 / 100`.
* **With the default MZ POKEY engine the volume is latched at init time**: `MZPOKEYSND_Init` computes `volume.s8/s16` from `POKEYSND_volume` (`mzpokeysnd.c:1393-1394`) and the mixer uses only that struct (`mzpokeysnd.c:2362,2370`). So a runtime volume change needs `Sound_Setup(); Sound_Continue();` (which re-runs `POKEYSND_Init` → `MZPOKEYSND_Init`). With `POKEYSND_enable_new_pokey = FALSE` the legacy mixer reads `POKEYSND_volume` per sample (`pokeysnd.c:1046,1048`) and a runtime change takes effect immediately.
* Init-time: `-volume <0..100>` → `POKEYSND_SetVolume` (`sound.c:145-154`). There is **no** config-file option for volume (grep of `cfg.c`/`sound.c`: only `SOUND_ENABLED/RATE/BITS/BUFFER_MS/LATENCY`, `sound.c:92-124`).
* `Sound_enabled` (`sound.h:55`) and `Sound_desired.freq/sample_size` are also runtime-settable but, again, only take effect through `Sound_Setup()`. Init-time equivalents: `-nosound`/`-sound` (`sound.c:136-139`), `-dsprate N` (`sound.c:140-144`), `-audio8`/`-audio16` (`sound.c:155-158`).

### Summary table
| Setting | Symbol / call | Header | Re-init needed? |
|---|---|---|---|
| RAM size | `MEMORY_ram_size` → `Atari800_InitialiseMachine()` + `Atari800_Coldstart()` | `memory.h:47`, `atari.h:144,137` | no; machine re-init only |
| XL/XE/800/5200 | `Atari800_SetMachineType()` + flags → `Atari800_InitialiseMachine()` + `Atari800_Coldstart()` | `atari.h:52,144,137` | no; machine re-init only |
| PAL/NTSC | `Atari800_SetTVMode(mode)`; + `Sound_Setup();Sound_Continue();` for audio | `atari.h:197`, `sound.h:66,13` | no |
| stereo/mono | `Sound_desired.channels` → `Sound_Setup()` + `Sound_Continue()` | `sound.h:47,66,13` | sound re-setup only |
| volume | `POKEYSND_SetVolume(0..100)` → `Sound_Setup()` + `Sound_Continue()` (MZ engine) | `pokeysnd.h:111` | sound re-setup only |
| any of the above, hard reset | `libatari800_exit(); libatari800_init(argc,argv);` | `libatari800.h:307,269` | full (also frees/reallocs everything) |

---

## 5. Video output contract

**Entry points / order per frame** (exact order from `src/libatari800/main.c:105-138`):

```
Devices_Frame(); INPUT_Frame(); GTIA_Frame(); ANTIC_Frame(TRUE);
INPUT_DrawMousePointer();
Screen_DrawAtariSpeed(Util_time()); Screen_DrawDiskLED(); Screen_Draw1200LED();
POKEY_Frame(); Sound_Update(); Atari800_nframes++;
```
then `libatari800_next_frame()` calls `PLATFORM_DisplayScreen()` (`libatari800/api.c:234`) and returns. So **after `libatari800_next_frame()` returns non-zero, `Screen_atari` holds one complete frame.**

**Buffer**
* Symbol `ULONG *Screen_atari` — `src/screen.h:14`, defined `src/screen.c:45`; allocated in `Screen_Initialise` as `Util_malloc(Screen_HEIGHT * Screen_WIDTH)` = `240 * 384` = **92 160 bytes**, zeroed (`screen.c:147-150`).
* Declared dimensions: `Screen_WIDTH 384`, `Screen_HEIGHT 240` (`screen.h:21-22`) and the array is *one byte per pixel*: the "`ULONG *`" type is only for 32-bit alignment (`antic.c:244-248`), and `ANTIC_Frame` advances the write pointer by `Screen_WIDTH / 2` **UWORD**s per line (`antic.c:2876`, `antic.c:3151`), i.e. 384 bytes/row.
* Row-major, top-of-screen first: `ANTIC_Frame` draws ANTIC lines 8…247 into rows 0…239 (`antic.c:2870-2874`, loop condition `ANTIC_ypos < (Screen_HEIGHT + 8)` at `antic.c:3154`; comment "lines 8..247 are on screen" at `antic.c:445`).
* **Stride = 384 bytes, for PAL and NTSC alike** — the geometry is *not* TV-mode dependent: `ANTIC_Frame` always produces 240 rows and `Atari800_tv_mode` only controls the extra VBLANK scanlines after row 239 (`antic.c:3198-3201`).

**Pixel format**
* One byte per pixel holding the **Atari colour index 0…255** (4 bits hue / 4 bits luminance), *not* RGB.
* Mechanism: `GTIA_COLOUR_TO_WORD(dest,src)` = `(((UWORD)(src)) << 8) | (src)` when `USE_COLOUR_TRANSLATION_TABLE` is **not** defined (`gtia.h:106-111`), `ANTIC_cl[128]` is a `UWORD` table of duplicated colour bytes (`antic.c:559`, filled by the GTIA register writes, `gtia.c:643-1029`), and `COLOUR(x)` re-reads them as a `UWORD` at byte offset `x` (`antic.c:585`) so that a single 16-bit store covers exactly two adjacent 1-byte pixels. `WRITE_VIDEO`/`WRITE_VIDEO_LONG` (`antic.c:175-179` without `DIRTYRECT`) write those words into `Screen_atari`.
* Host-side palette: `int Colours_table[256]` (`colours.h:6`, defined `colours.c:66`), packed `0x00RRGGBB`; accessors `Colours_GetR/G/B(x)` (`colours.h:52-54`). It is filled at `Atari800_Initialise` time by `Colours_Initialise` → `UpdateModeDependentPointers(Atari800_tv_mode)` + `UpdatePalette()` (`colours.c:283-357`, `173-181`) and refreshed by `Colours_Update()` (`colours.c:183-189`) which `Atari800_SetTVMode` calls for you (`atari.c:1722`). Default palette comes from `colours_ntsc.c` / `colours_pal.c`; an external 768-byte palette is only used if the user configured one (`colours_external.c:37`, needs a file).
* The emulator never converts to RGB for you in this build — there is no `PLATFORM_MapRGB`/`GetPixelFormat` (`platform.h:91-105` is compiled out).

**Visible vs overscan geometry**
* `int Screen_visible_x1 = 24; x2 = 360; y1 = 0; y2 = Screen_HEIGHT;` (`screen.c:62-65`) → **the displayable window is x ∈ [24,359] (336 px) × y ∈ [0,239] (240 rows)**, i.e. 336×240 out of the 384×240 buffer; `screen.h:20` states "You should never display anything outside the middle 336 columns".
* For libatari800 these globals are *never* modified (the only code that writes them, `videomode.c:303-317`, is not compiled), so they keep those defaults for both PAL and NTSC.
* For reference, the Atari800 convention for the "TV" area (used by the SDL port's `VIDEOMODE`) is 336 px wide (`videomode.c:326-328`) and 224 lines for NTSC / 240 lines for PAL (`videomode.c:338-347`), vertically centred by `VIDEOMODE_src_offset_top = (240 - h)/2` (`videomode.c:485`). If you want the classic NTSC look, crop your blit source to rows 8…231.
* Nothing in the framebuffer tells you PAL vs NTSC geometry — only `Atari800_tv_mode` does (`atari.h:95`).

**Overlays written directly into the emulated framebuffer** (they are in your picture, not in a separate layer):
* `Screen_DrawAtariSpeed()` – only if `Screen_show_atari_speed` (default FALSE, `screen.c:67`), draws at `screen.c:650-670`.
* `Screen_DrawDiskLED()` – **default TRUE** (`Screen_show_disk_led`, `screen.c:68`), draws the drive-activity glyph at `Screen_atari + Screen_visible_x2 - SMALLFONT_WIDTH …` (`screen.c:672-704`).
* `Screen_Draw1200LED()` – default TRUE (`screen.c:70`) but only when `Atari800_keyboard_leds` (1200XL mode), `screen.c:706-712`.
* `Screen_DrawStatusText()` – only from `Atari800_Frame`, not used by libatari800 (`libatari800/main.c:133` calls `Screen_Draw1200LED` but not `Screen_DrawStatusText`).
* To get a pristine framebuffer set `Screen_show_disk_led = FALSE; Screen_show_1200_leds = FALSE; Screen_show_atari_speed = FALSE;` (all exported ints, `screen.h:42-46`).

**No host symbol is required for video.** `PLATFORM_DisplayScreen()` is an empty function in this target (`libatari800/video.c:33-34`); replace that file (and keep the prototype `platform.h:19`) to blit inside the library's call sequence.

---

## 6. Audio output contract

**Call chain.** `libatari800_init()` → `Atari800_Initialise()` → … → `Sound_Initialise(argc,argv)` (argv parsing only, `libatari800/main.c:97`, `sound.c:126`) → … → `Sound_Setup()` if `Sound_enabled` (`atari.c:1085-1098`) → `Sound_Continue()`. Then, every `libatari800_next_frame()`: `POKEY_Frame(); Sound_Update();` (`libatari800/main.c:135-136`) → `UpdateSyncBuffer()` → `POKEYSND_UpdateProcessBuffer()` → `WriteOut()` → `PLATFORM_SoundAvailable()` / `PLATFORM_SoundWrite()` (`sound.c:465-473`, `380-378`, `358-377`).

**Pull or push?** For this build `SOUND_CALLBACK` is **undefined** (`configure.ac:1020`), so it is a **push-by-library / pull-by-host** design:
* `PLATFORM_SoundAvailable()` returns the whole hardware buffer size; `PLATFORM_SoundWrite()` in the stock backend simply `memcpy`s into the library's own buffer and records the byte count (`libatari800/sound.c:86-111`).
* The host therefore does *not* have to implement anything: after each `libatari800_next_frame()` it calls
  ```c
  UBYTE *pcm = libatari800_get_sound_buffer();          /* libatari800.h:287 */
  int    len = libatari800_get_sound_buffer_len();      /* libatari800.h:289 — ALWAYS use this */
  ```
  and copies/queues `len` bytes. `libatari800_get_sound_buffer_allocated_size()` (`libatari800.h:291`) gives the maximum.
* If you prefer an interrupt/DMA-driven push, replace `src/libatari800/sound.c` with your own `PLATFORM_SoundSetup/Exit/Pause/Continue/Available/Write` (plus the four globals listed in §1.2-C if you keep `api.c`). `PLATFORM_SoundWrite` is then called with `size ≤ Sound_out.buffer_frames * channels * sample_size` (`platform.h:157-160`), once per emulated frame.

**Format**
* Sample size/freq/channels default: `Sound_desired = { 44100, 2, 1, 0 }` → freq 44 100 Hz, **16-bit**, **mono** (`sound.c:43-48`; field order `sound.h:23-37`).
* 16-bit is **signed, system-endian (little-endian on ARM)**: `sound.h:26-29`; the MZ mixer writes `SWORD` values directly (`mzpokeysnd.c:2308-2331`, `2360-2365`); the legacy mixer converts with `- 0x80` and `POKEYSND_volume` scaling (`pokeysnd.c:1037-1057`). 8-bit mode is **unsigned** (`sound.h:27`, `mzpokeysnd.c:2296`, `pokeysnd.c:1046`). There is **no `POKEYSND_SIGNED_SAMPLES`** in this build (that macro is Falcon-only, `configure.ac:1083`).
* Channels: mono = interleaved POKEY chip 0 only; stereo = interleaved **chip0, chip1 per frame** (L,R) (`mzpokeysnd.c:2356-2373` writes `num_cur_pokeys` samples per frame slot).
* To change the rate/format at init: `Sound_desired.freq` / `Sound_desired.sample_size` before `libatari800_init`, or `-dsprate N` / `-audio8` / `-audio16` in argv (`sound.c:140-158`); legal freq clamp 1000…65535 (8192 minimum when MZ POKEY is on) (`sound.c:206-215`). `sound.c:221-231` rejects a backend that returns an out-of-range rate.

**Buffer sizes**
* `PLATFORM_SoundSetup` (stock) computes `buffer_frames = ceil(freq / fps)` where fps is `Atari800_FPS_PAL` (49.8607597) or `Atari800_FPS_NTSC` (59.9227434) (`libatari800/sound.c:56-58`, `atari.h:88-91`), then
  `sound_hw_buffer_size = buffer_frames * sample_size * channels` (`libatari800/sound.c:60`).
  * NTSC, 44 100 Hz, 16-bit mono: **736 frames = 1 472 bytes/frame**
  * PAL, 44 100 Hz, 16-bit mono: **885 frames = 1 770 bytes/frame**
  *(computed, not run)*
* Jitter: the frame rate is not an integer divisor of the sample rate, so `sample_diff = buffer_frames - freq/fps` (≈0.0545 at NTSC/44.1 k, `libatari800/sound.c:66`) is accumulated in `sample_residual` and one sample-frame is dropped from the buffer whenever it exceeds 1.0 (`libatari800/sound.c:86-105`) — i.e. **occasionally 1470 instead of 1472 bytes** (matches the API doc "one out of about every 19 frames", `api.c:329-334`). **Always use `libatari800_get_sound_buffer_len()`, never a hard-coded 1472.**
* The library also keeps an internal latency ring (`sync_buffer`) sized `(latency_frames + 5*buffer_frames) * channels * sample_size` with default `Sound_latency = 20` ms (`sound.c:69,475-495`) → ≈9 124 bytes at NTSC/44.1 k/16-bit mono *(computed)*. Change with `Sound_SetLatency(ms)` (`sound.h:81`) or `-snddelay ms` (`sound.c:169-172`).
* **There is no host-side queue**: `PLATFORM_SoundWrite` overwrites `LIBATARI800_Sound_array` every frame. The host must consume/copy the frame's PCM before the next `libatari800_next_frame()`. Underflow is handled by repeating the last sample frame rather than glitching (`sound.c:331-342`).
* Total latency you should expect = emulator latency (20 ms + 5 frames) + your own ring.

**Driving it from a bare-metal timer:** call `libatari800_next_frame()` from your frame timer (1/49.86 s PAL or 1/59.92 s NTSC — `libatari800_get_fps()`, `api.c:404-406`) and immediately hand the PCM block to your I2S/PWM DMA ring. The emulator does **not** sleep, sync or adjust itself in this build: `Atari800_Sync()` is compiled out (`atari.c:1231-1443` is inside `#ifndef LIBATARI800`, and `atari.c:185` declares it anyway) and `Sound_AdjustSpeed()` (`sound.c:497-527`) is only used by `Atari800_Sync`. If your DAC needs a clock-derived sample rate, run the emulator slightly fast/slow and let the emulator's own latency ring absorb the drift, or implement a resampler on your side.

---

## 7. Save-state (snapshot) contract

**API** (all in `src/libatari800/libatari800.h`):
```c
typedef struct {                                     /* libatari800.h:140-157 */
    union { statesav_tags_t tags;  UBYTE tags_storage[128];  };
    union { statesav_flags_t flags; UBYTE flags_storage[128]; };
    UBYTE state[STATESAV_MAX_SIZE];                  /* STATESAV_MAX_SIZE = 210000 (line 122) */
} emulator_state_t;

void libatari800_get_current_state(emulator_state_t *state);   /* line 303, api.c:456 */
void libatari800_restore_state(emulator_state_t *state);       /* line 305, api.c:476 */
```
* Save: `libatari800_get_current_state(&st)` → `LIBATARI800_StateSave(st.state, &st.tags)` (`api.c:458`) → `StateSav_SaveAtariState(NULL, NULL, 0)` (`libatari800/statesav.c:40`) → through the LIBATARI800 in-memory `gzFile` shim (`statesav.c:650-670`, `73-102`) writes into the caller's `state[]` array.
* Load: `libatari800_restore_state(&st)` → `LIBATARI800_StateLoad(st.state)` → `StateSav_ReadAtariState(NULL, NULL)` (`libatari800/statesav.c:45`), then restores `MEMORY_selftest_enabled`, `Atari800_nframes` and `sample_residual` from `st.flags` (`api.c:479-481`).
* Also carried: `st.tags` is filled by `STATESAV_TAG()` (`statesav.h:24`) with byte offsets: `size`, `cpu`, `pc`, `base_ram`, `base_ram_attrib`, `antic`, `gtia`, `pia`, `pokey` (`statesav.h:24` + `libatari800.h:128-138`); the serialized stream starts with `"ATARI800"` + version byte (SAVE_VERSION_NUMBER = 8, `statesav.c:71,347`) and version 3..8 states can be read (`statesav.c:458-463`).
* **The size actually used is `st.tags.size` after the save** (`STATESAV_TAG(size)` at the end, `statesav.c:404`) — read it if you want to store only the used prefix (the state is position-dependent but has no per-field offsets inside).

**Size limits — a real gap**
* The hard bound is `STATESAV_MAX_SIZE = 210000` (`libatari800.h:122`), used as both the buffer size and the read/write limit (`statesav.c:656` `unclen = STATESAV_MAX_SIZE`; bounds checks `statesav.c:676,685`).
* Contents of a libatari800 state (SaveVerbose = 0 always, `libatari800/statesav.c:40`):
  * `MEMORY_mem[65536]` (`memory.c:366`) + `MEMORY_attrib[65536]` (`memory.c:369`, because `PAGED_ATTRIB` is off) + `under_cartA0BF[8192]` (`memory.c:401`) + `under_atarixl_os[16384]` (`memory.c:405`) = **155 648 B**
  * extended XE RAM `atarixe_memory_size = (1 + (ram_size-64)/16) * 16384` (`memory.c:159`, saved at `memory.c:426`): 0 B @64 KB, **81 920 B @128 KB**, 147 456 B @192 KB, 278 528 B @320 KB
  * plus CPU/ANTIC/GTIA/PIA/POKEY/cartridge/SIO/PBI state (a few KB; the cartridge image itself is *not* saved, only type/state/name — `cartridge.c:2114-2138`)
* **Therefore (computed, not run): a 128 KB machine needs ≈ 237 KB > 210 000, and 192/320 KB machines need ≈ 300/435 KB.** With the stock `STATESAV_MAX_SIZE` the write side silently truncates: `mem_write()` returns 0 when the bound is hit (`statesav.c:683-689`) and the resulting `GetGZErrorText()` (`statesav.c:122`) does **not** set `nFileError`, so all later writes fail silently too; a truncated state read back leaves the un-read fields untouched rather than reporting an error (`statesav.c:676-680`, same "no error" behaviour).
* Mitigation for the port: (a) restrict the host to 64 KB RAM if you want stock save states; or (b) raise `STATESAV_MAX_SIZE` in **both** `src/libatari800/libatari800.h:122` and `src/statesav.h:26` (the latter is only used in the non-LIBATARI800 branch, `statesav.h:20-28`) and size your `emulator_state_t` accordingly; the in-memory backend will then use the larger bound automatically (`statesav.c:656`).

---

## 8. Risks and gaps (everything that needs extra host work)

**R1 — `Util_time()` is a hard build error unless you pick a time source.** `util.c:550-569` ends with `#error No function found for Util_time()`. For a bare-metal build the cleanest option is `SUPPORTS_PLATFORM_TIME` + your own `double PLATFORM_Time(void)` (`platform.h:52-55`, `util.c:552-553`); otherwise `HAVE_CLOCK` and a working newlib `clock()`/`_times`. `Util_time()` is called on the audio path every frame (`sound.c:394,375`) and by `Screen_DrawAtariSpeed` (`libatari800/main.c:132`), so it must be monotonic and reasonably fast.

**R2 — `dirent.h`/`opendir` are needed to *compile*, not just to run.** `sysrom.c:284-360` (called from `atari.c:488` at every init) and `devices.c:237-276` use `DIR`, `struct dirent.d_name`, `opendir`, `readdir`, `closedir` unconditionally while only including `<dirent.h>` under `HAVE_DIRENT_H` (`sysrom.c:28-30`, `devices.c:58-60`). Either ship a stub `dirent.h` + `opendir` returning `NULL` (handled gracefully: `sysrom.c:293`, `devices.c:245`) or patch those two functions. *(I could not verify whether your specific newlib/arm-none-eabi and Circle toolchain ship `dirent.h` and the `_open/_read/_fstat` syscalls those stubs need — check that first.)*

**R3 — stdio is required for the boot path unless you stub it.** On init: `CFG_LoadConfig` → `Util_GetHomeDir` → `getenv("HOME")` (`util.c:629`) and `fopen(rtconfig_filename,"r")` (`cfg.c:108`); `SYSROM_FindInDir(CFG_data_dir, TRUE)` → `opendir`+`fopen` (`atari.c:488`, `sysrom.c:293-320`); `Log_print` → `printf` via `BUFFERED_LOG` (`log.c:42,68-75`). All failures are handled, so a minimal `fopen`/`opendir` returning `NULL`/`getenv` returning `NULL` is enough — but they must be *linkable* and must not fault. The `atari.c:474` `fopen(".atari800-check","w")` probe is skipped automatically when `argv[0]` is `NULL`, which is what `libatari800_init` does when you pass a list without a program name (`api.c:106-117`, `atari.c:467`).

**R4 — everything else filesystem-backed (name the file and the call).**
* `Atari800_LoadImage` → `fopen/fread/fclose` (`atari.c:301-318`). Reached only via `SYSROM_LoadImage` when the ROM has no built-in `data` pointer (`sysrom.c:521-528`), or from `PBI_MIO/XLD/BB` and `AFILE_OpenFile` paths. Avoid it with `EMUOS_ALTIRRA=1` or by assigning ROM data yourself: `SYSROM_t SYSROM_roms[SYSROM_SIZE]` is a **public, writable** array (`sysrom.h:73-82`) — set `.data` (e.g. `SYSROM_roms[SYSROM_BB01R2].data = xl_rom;`, the struct already has the right `.size`) and `SYSROM_LoadImage` will `memcpy` from it (`sysrom.c:523-525`). Note `SYSROM_ChooseROMs` only accepts an entry if `data != NULL || filename[0] != '\0'` (`sysrom.c:485-488`). The workspace already contains matching ROM images (`ROMS/ATARIXL.ROM` = 0x4000, `ROMS/ATARIXL.ROM`/`ATARIOSA.ROM` = 0x2800, `ATARIBAS.ROM` = 0x2000, `atari5200.rom` = 0x800) which match the expected sizes in `sysrom.c:85-104`.
* Disk images: `SIO_Mount` (`sio.c:214-231`) opens the image with `Util_fopen(filename, "rb+"|"rb", …)` and does all sector I/O through that `FILE *` (`sio.c:861-884`, `SIO_FormatDisk` re-opens with `fopen(fname,"wb")` at `sio.c:919`). `libatari800_mount_disk_image()`/`libatari800_mount_disk()` (`api.c:251,500`) are thin wrappers over `SIO_Mount`. **The host must provide a stdio that can back ATR/XFD images** — either a real FS (FatFs + newlib FILE layer), or `fopen` mapped onto in-RAM images keyed by the pseudo-filename. Read-only mounts work with `"rb"` only.
* Cartridges: `CARTRIDGE_Insert*` → `fopen` (`cartridge.c:1723`), `CARTRIDGE_WriteImage` → `fopen("wb")` (`cartridge.c:1610`); cassettes: `img_tape.c:151,155,265`.
* Boot/exec files: `AFILE_OpenFile` → `AFILE_DetectFileType` → `fopen` (`afile.c:48`), `binload.c:193`.
* `monitor.c` has `fopen`/`popen`/`system` (`monitor.c:556,1794,2184,2239,2311,2314,3837`) but all of those are inside `#ifdef MONITOR_*`/`HAVE_SYSTEM` blocks that are off here (grep of the guards; `MONITOR_Exit()` at `monitor.c:953` is unconditional and is called from `atari.c:1211`).
* `devices.c` H: device and P: printer: `Util_fopen` (`devices.c:849,962,965`), `opendir/readdir` (`:244,257`), `rename/mkdir/rmdir` (`:386,441,471`), `system()` in the P: spooler (`:1892`, under `#ifdef HAVE_SYSTEM`). The directory-abstraction macros (`DO_RENAME/DO_LOCK/DO_MKDIR/DO_RMDIR`) are only defined when `HAVE_RENAME/HAVE_CHMOD/HAVE_MKDIR/HAVE_RMDIR` are, so leaving them undefined compiles the H: device without those operations.
* `file_export.c:382` and `codecs/container.c:219` only when recording/screenshots are enabled (off in the recommendation).
* `statesav.c` file mode (`fopen`, `gzopen`) is bypassed entirely for LIBATARI800 (`statesav.c:73-102, 650-670`), so snapshots need **no** FS.

**R5 — `exit()` paths inside the core.** `util.c:301,312` (out of memory) and `colours.c:151` (invalid `Atari800_tv_mode` — anything other than 262/312 calls `Atari800_ErrExit(); exit(1)`). With `HAVE_SETJMP` defined the "invalid ESC opcode" path long-jumps instead of reaching `esc.c:157 exit(0)`; with it undefined, an unknown `ESCRTS` opcode will terminate your application. `mzpokeysnd.c:1196,1239` and `devices.c:1977,2030` `exit()`s are inside `#if 0`/`#ifdef BASIC` and are not compiled.

**R6 — compressed disk images need a temp file abstraction.** `SIO_Mount` uses `Util_tmpopen()` for DCM (`sio.c:249`) and `.gz`/ATZ images (`sio.c:273`). `util.h:208-224` selects between `tmpfile()` (`HAVE_TMPFILE`), `Util_uniqopen` + `Util_unlink` (`HAVE_UNLINK`), or — **if neither is defined — `Util_uniqopen(NULL, "wb+")`, which passes a `NULL` filename into `snprintf`/`Util_fileexists` (`util.c:516-518`) and will fault.** Plain ATR/XFD never takes this path, so with no tmpfile support you must not mount DCM/GZ images.

**R7 — libc behaviours newlib may not give you.** `malloc/free/realloc` with a heap (the emulator allocates at several points during init: `Screen_atari` 92 KB `screen.c:148`, `MEMORY_os/basic/xegame` are static (`memory.h:97-99`), XE RAM `memory.c:163`, `atarixe_memory`, sound `sync_buffer`/`process_buffer` (`sound.c:242,491`), `LIBATARI800_Sound_array` (`libatari800/sound.c:64`), and — if enabled — `bb_ram`/`mio_ram`). `Util_malloc` deliberately `exit(1)`s on failure (`util.c:295-304`). `rand()` (`mzpokeysnd.c`) must exist. `vsnprintf` is used if `HAVE_VSNPRINTF` is defined (`log.c:55`) — keep it defined. `strtod` (`util.c:249`). `modf/ceil/floor/pow/cos/sin/fabs` need `-lm` and a newlib built with the math functions you use (all are soft-float-safe on ARM; `remez.c` in particular runs at every `Sound_Setup`).

**R8 — performance/accuracy notes found in the sources.**
* `NEW_CYCLE_EXACT` is off in the stock libatari800 `config.h` (see §2.1 trap 1) although `cycle_map.c` is built. If you turn it on you pay per-scanline colour-change accuracy; if you leave it off, do not compile `cycle_map.c` (saves ~118 KB BSS, `cycle_map.c:28-29`).
* Dynamic palette changes *inside* a scanline are only exact with `NEW_CYCLE_EXACT`; simpler PAL blending exists in the *default* build (`ANTIC_pal_blending`, `antic.c:73`, honoured at `antic.c:3156-3185`) and is switched on by `ARTIFACT_mode == ARTIFACT_PAL_SIMPLE` (`artifact.c:84`) — controllable via the `ARTIFACT`/`-artifacting` option family, so you can turn it off at runtime if you want raw pixels.
* `Sound_Setup()` re-runs the whole REMEZ filter design (`mzpokeysnd.c:1370-1376` → `remez.c:513`) — do not call it per frame; it is cheap enough for user-triggered settings changes but not for a per-frame "keep in sync" strategy.
* `Antic800_tv_mode`-dependent sound constants are latched in `PLATFORM_SoundSetup`/`MZPOKEYSND_Init` (`libatari800/sound.c:56`, `mzpokeysnd.c:1271`), so after a PAL/NTSC switch you must call `Sound_Setup()` (see §4c) or the audio rate will drift.

**R9 — thread assumptions: none in the recommended build.** `SOUND_CALLBACK` off removes the callback-thread contract; `NETSIO` (pthread) is off; no `signal()` use (`CTRL_C_HANDLER` is not defined for this target). `rtime.c` (`RTIME_Initialise`) reads the emulated Atari's POKEY timer, not the host clock, but check `RTIME` config options if you care.

**R10 — things I could not confirm.**
* Whether `--target=libatari800` (upstream) really links on your toolchain: I did not build anything, so the "zero host symbols" conclusion is a *static* result (all `PLATFORM_*` uses matched to definitions inside the compiled set) and could still be broken by a symbol I did not enumerate. A `nm -u` of the library is the definitive check.
* The exact state size figures in §7 are computed from the save-function contents, not measured.
* Whether your newlib/Circle combination provides `dirent.h`, `opendir`, `tmpfile`, `setjmp`, `getenv`, `rand`, `modf`, `vsnprintf` — all of which the sources assume exist when the corresponding macro/target is in play.
* `src/config.h.in` does not exist in the tree (autoheader output), so the autoheader symbol list in `src/dc/config.h`, `src/amiga/config.h`, `src/wince/port/config.h` is my proxy for the "expected" macro spelling.

---

## 9. Minimal bring-up recipe (derived from the above)

1. Copy `src/dc/config.h` as a skeleton, apply §2.2.
2. Compile the §3.1 + §3.2 + enabled-§3.3 file list with `-I src -I src/libatari800` (the sources use both `"libatari800/x.h"` and `"../x.h"`), link with `-lm`.
3. Provide: newlib heap, `dirent.h` stub, `fopen`/`opendir`/`getenv` stubs that fail cleanly, `PLATFORM_Time()` (with `SUPPORTS_PLATFORM_TIME`), `setjmp/longjmp`, `vsnprintf`, `rand`, `modf/ceil/floor/pow/cos/sin/fabs`.
4. Embed `ROMS/ATARIXL.ROM` (+`ATARIBAS.ROM`) as byte arrays and assign `SYSROM_roms[SYSROM_XL_CUSTOM or SYSROM_BB01R2].data` — or rely on `EMUOS_ALTIRRA=1` for a zero-ROM boot.
5. `libatari800_init(-1, args)` with `args = { "-xe", "-ntsc", "-sound", "-audio16", "-volume", "80", NULL }` (or set the globals directly per §4).
6. Loop: call `libatari800_next_frame(&input)` from your frame timer; on non-zero return, blit `Screen_atari` (crop x 24…359, y 0 or 8…231 for NTSC) and hand `libatari800_get_sound_buffer()`/`_len()` to your audio ring; use `libatari800_mount_disk()`/`libatari800_reboot_with_file()` for media; use `libatari800_get_current_state()`/`_restore_state()` for snapshots (RAM ≤ 64 KB unless you raise `STATESAV_MAX_SIZE`).
7. Error handling: check the `libatari800_next_frame()` return code against `libatari800_error_message()` (`libatari800.h:260-267`, `api.c:133-158`).
