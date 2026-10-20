/*
 * config.h -- build configuration for the bare-metal Raspberry Pi port of the
 *             Atari800 emulation core.
 *
 * This file replaces the autoconf-generated `config.h` of the upstream
 * Atari800 project.  It is written by hand because there is no autoconf on a
 * bare-metal target: there is no operating system to probe for.
 *
 * The guiding rule is: **define only what is really available**.  Every
 * optional subsystem that is left undefined removes a source file from the
 * build and removes an OS dependency from the port.  What remains is the
 * portable emulation core -- CPU, ANTIC, GTIA, POKEY, PIA, memory, SIO,
 * devices, cartridges, cassettes and save states -- which this project drives
 * from its own platform layer.
 *
 * The HAVE_* block was derived from what newlib (arm-none-eabi-gcc 14.3) plus
 * Circle actually provide, cross-checked against upstream's own hand-written
 * `src/amiga/config.h` which is the closest existing "no autoconf" reference.
 */

#ifndef CONFIG_H_
#define CONFIG_H_

/* ==========================================================================
 * 1. Host compiler / libc capabilities
 * ========================================================================== */

#define STDC_HEADERS 1

/* Headers that exist in newlib. */
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_ERRNO_H 1
#define HAVE_TIME_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_MATH_H 1
#define HAVE_STDARG_H 1
#define HAVE_LIMITS_H 1

/* POSIX headers that do NOT exist.  Defining these would pull in OS calls. */
/* #undef HAVE_UNISTD_H */
/* #undef HAVE_SYS_TIME_H */
/* #undef HAVE_SYS_TYPES_H */
/* #undef HAVE_SYS_STAT_H */
/* #undef HAVE_SYS_IOCTL_H */
/* #undef HAVE_SYS_SELECT_H */
/* #undef HAVE_SYS_SOUNDCARD_H */
/* #undef HAVE_TERMIOS_H */
/* #undef HAVE_DIRENT_H */
/* #undef HAVE_FCNTL_H */
/* #undef HAVE_SIGNAL_H */
/* #undef HAVE_WINDOWS_H */
/* #undef HAVE_FILE_H */
/* #undef HAVE_MEMORY_H */

/* Functions provided by newlib. */
#define HAVE_ATEXIT 1
#define HAVE_FFLUSH 1
#define HAVE_FLOOR 1
#define HAVE_MODF 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMSET 1
#define HAVE_REWIND 1
#define HAVE_SNPRINTF 1
#define HAVE_STRCASECMP 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define HAVE_STRNCPY 1
#define HAVE_STRRCHR 1
#define HAVE_STRSTR 1
#define HAVE_STRTOL 1
#define HAVE_VPRINTF 1
#define HAVE_VSNPRINTF 1
#define HAVE_ROUND 1
#define HAVE_FSEEKO 1
#define HAVE_FTELLO 1

/*
 * setjmp/longjmp are present in newlib and are essential here: the CPU core
 * uses them to abandon a frame when the emulated machine executes a CIM
 * instruction or an invalid escape opcode, instead of taking the whole
 * Raspberry Pi down with it.  See ATARI_RunFrame().
 */
#define HAVE_SETJMP 1

/*
 * The emulation core can fall back on AltirraOS, a free replacement for the
 * Atari OS ROMs, when no OS ROM image is available.  Shipping this means the
 * emulator still boots if the ROMS directory is missing or damaged, instead of
 * refusing to start.
 */
#define EMUOS_ALTIRRA 1

/* Functions deliberately NOT claimed, because newlib's versions either do not
 * exist or would reach into an operating system we do not have.  The emulation
 * core has portable fallbacks for all of them. */
/* #undef HAVE_CHMOD */
/* #undef HAVE_CLOCK */
/* #undef HAVE_FDOPEN */
/* #undef HAVE_FSTAT */
/* #undef HAVE_GETCWD */
/* #undef HAVE_GETTIMEOFDAY */
/* #undef HAVE_LOCALTIME */
/* #undef HAVE_MKDIR */
/* #undef HAVE_MKSTEMP */
/* #undef HAVE_MKTEMP */
/* #undef HAVE_NANOSLEEP */
/* #undef HAVE_OPENDIR */
/* #undef HAVE_RENAME */
/* #undef HAVE_RMDIR */
/* #undef HAVE_SELECT */
/* #undef HAVE_SIGNAL */
/* #undef HAVE_STAT */
/* #undef HAVE_SYSTEM */
/* #undef HAVE_TMPFILE */
/* #undef HAVE_TMPNAM */
/* #undef HAVE_UCLOCK */
/* #undef HAVE_UNLINK */
/* #undef HAVE_USLEEP */

/* Processors here are little endian; unaligned words are architecturally
 * supported on ARMv7/v8 but we do not build the fast paths that rely on it,
 * because Circle runs some code with the MMU configured to trap them. */
/* #undef WORDS_BIGENDIAN */
/* #undef WORDS_UNALIGNED_OK */
/* #undef HAVE_STAT_EMPTY_STRING_BUG */
/* #undef LSTAT_FOLLOWS_SLASHED_SYMLINK */
/* #undef TIME_WITH_SYS_TIME */
/* #undef TM_IN_SYS_TIME */
/* #undef CLOSEDIR_VOID */
/* #undef RETSIGTYPE */

/* ==========================================================================
 * 2. Target identification
 *
 * The port supplies its own platform layer, so upstream must not try to build
 * one of its own ports.  `LIBATARI800` switches the core into "library"
 * behaviour, where the embedding application owns the main loop, the screen
 * presentation and the sound device.  That is exactly our situation.
 * ========================================================================== */

#define LIBATARI800 1

/* No upstream front end: this project provides the entry point and the menu. */
/* #undef RPI */
/* #undef SDL */
/* #undef X11 */
/* #undef MOTIF */
/* #undef XVIEW */
/* #undef SHM */
/* #undef DOSVGA */
/* #undef FALCON */
/* #undef PS2 */
/* #undef ANDROID */
/* #undef __PLUS */
/* #undef USE_CURSES */
/* #undef USE_NCURSES */
/* #undef CURSES_BASIC */
/* #undef GUI_SDL */
/* #undef DONT_DISPLAY */
/* #undef LIBATARI800_REINIT_VIDEO */

/* No sound backend from upstream: PLATFORM_Sound* is ours. */
/* #undef SOUND_OSS */
/* #undef SOUND_SDL */
/* #undef SOUND_PA */
/* #undef SOUND_WIN */
/* #undef SOUND_DUMMY */
/* #undef SOUND_LIBATARI800 */
/* #undef SOUND_CALLBACK */

/* ==========================================================================
 * 3. Emulated machine
 *
 * XL/XE family only.  `Atari800_machine_type` selects between the XL and XE
 * personalities; `MEMORY_ram_size` selects 64/128/192/320 kB.  There is no
 * 256 kB configuration in the core, which is why this project does not offer
 * one; see docs/using.md.
 * ========================================================================== */

/* #undef ATARI5200 */
/* #undef PBI_BB */
/* #undef PBI_MIO */
/* #undef PBI_PROTO80 */
/* #undef PBI_SCSI */
/* #undef PBI_XLD */
/* #undef IDE */
/* #undef VOICEBOX */
/* #undef VOTRAX */
/* #undef AF80 */
/* #undef BIT3 */
/* #undef XEP80_EMULATION */
/* #undef R_IO_DEVICE */
/* #undef R_SERIAL */
/* #undef NETWORK */
/* #undef NETSIO */
/* #undef SIO_SOUND */
/* #undef DOS_DRIVES */
/* #undef BACK_SLASH */

/* ==========================================================================
 * 4. Video system
 *
 * PAL and NTSC are compiled in and chosen at run time by Atari800_SetTVMode().
 * ========================================================================== */

#define PAL 1
#define NTSC 1

/* Cycle-exact ANTIC rendering: allows colour changes inside a scanline and
 * correct mid-line GTIA register writes.  This is upstream's default and it is
 * what makes the vast majority of demos and games render correctly.  It is the
 * single most important accuracy option in the build. */
#define NEW_CYCLE_EXACT 1

/* The per-byte attribute array is NOT enabled: it is upstream's non-default
 * mode and needs a second 384x240 array plus extra per-cycle bookkeeping.
 * NEW_CYCLE_EXACT gives us correct colour timing without it. */
/* #undef PAGED_ATTRIBUTES */
/* #undef PAGED_ATTRIB */
/* #undef CYCLES_PER_OPCODE */
/* #undef BITPL_SCR */
/* #undef DIRTYRECT */
/* #undef CLIENTUPDATE */
/* #undef USE_COLOUR_TABLES */
/* #undef VERY_SLOW */

/* PAL colour blending (the "PAL artifacting" look) is enabled -- it is a
 * per-line effect and affordable.  The full NTSC 2-frame comb filter is not,
 * because it needs a frame of history and is very expensive. */
#define PAL_BLENDING 1
/* #undef FILTER_NTSC */
/* #undef NTSC_FILTER */
#define ATARI_PALETTE_PRESET_STANDARD 1

/* ==========================================================================
 * 5. Sound
 * ========================================================================== */

#define SOUND 1
#define STEREO 1
#define STEREO_SOUND 1
#define INTERPOLATE_SOUND 1
#define CONSOLE_SOUND 1
#define SOUND_GAIN 2
/* #undef CLIP_SOUND */
/* #undef SUPPORTS_SOUND_REINIT */
/* #undef POKEYSND_SIGNED_SAMPLES */

/* ==========================================================================
 * 6. Input and user interface
 *
 * The interactive front end (file selector, menus, monitor, screenshots) is
 * replaced by this project's own OSD menu, which drives the core's exported
 * globals directly.
 * ========================================================================== */

#define KBD_LED 1
/* #undef USE_UI */
/* #undef MONITOR */
/* #undef MONITOR_ASSEMBLER */
/* #undef MONITOR_BREAK */
/* #undef MONITOR_BREAKPOINTS */
/* #undef MONITOR_HINTS */
/* #undef MONITOR_PROFILE */
/* #undef MONITOR_TRACE */
/* #undef SCREENSHOTS */
/* #undef FILE_EXPORT */
/* #undef AUDIO_RECORDING */
/* #undef VIDEO_RECORDING */
/* #undef EVENT_RECORDING */
/* #undef POKEYREC */
/* #undef ACIDTEST */
/* #undef BASIC */
/* #undef BENCHMARK */
/* #undef CRASH_MENU */
/* #undef ONSCREENKEYBOARD */
/* #undef LINUX_JOYSTICK */
/* #undef SVGA_JOYMOUSE */
/* #undef PCKEYBOARD_ROM_KEYS */
/* #undef BUFFERED_LOG */
/* #undef USE_CLOCK */
/* #undef ALTERNATE_SYNC_WITH_HOST */

/* ==========================================================================
 * 7. Platform hooks
 *
 * Only the hooks this project implements are enabled.  The rest are absent so
 * that the core never calls into code we have not written.
 * ========================================================================== */

#define SUPPORTS_PLATFORM_SLEEP 1
#define SUPPORTS_PLATFORM_TIME 1
/* #undef SUPPORTS_CHANGE_VIDEOMODE */

/* PAL_BLENDING needs the native pixel format so that it can pre-blend the
 * palette in the same format the framebuffer uses.  We output RGB565, and both
 * hooks are implemented in src/port/platform.c. */
#define PLATFORM_MAP_PALETTE 1

/* #undef SUPPORTS_PLATFORM_CONFIGINIT */
/* #undef SUPPORTS_PLATFORM_CONFIGURE */
/* #undef SUPPORTS_PLATFORM_CONFIGSAVE */
/* #undef SUPPORTS_PLATFORM_PALETTEUPDATE */

/* ==========================================================================
 * 8. Package identification
 * ========================================================================== */

#define PACKAGE "atari800"
#define PACKAGE_NAME "atari800"
#define PACKAGE_TARNAME "atari800"
#define PACKAGE_VERSION "5.2.0-baremetal"
#define VERSION "5.2.0-baremetal"
#define PACKAGE_STRING "atari800 5.2.0-baremetal"
#define PACKAGE_BUGREPORT "https://github.com/atari800/atari800"
#define PACKAGE_URL ""
#define ATARI800_TITLE "Atari XL/XE bare-metal emulator for Raspberry Pi"
#define DEFAULT_CFG_NAME "ATARI800.CFG"

/* ==========================================================================
 * 9. File layer redirection
 *
 * There is no POSIX file API on a bare-metal target.  Rather than emulate one
 * on top of newlib -- which would drag in an operating system we do not have --
 * the whole emulation core is redirected onto this project's own virtual file
 * system, which is a thin, read/write, long-filename capable layer over FatFs
 * (see src/port/vfs.c and src/port/vfs.h).
 *
 * The core only ever touches files through these functions and the FILE type,
 * so redirecting the names is sufficient and complete.  It also means that the
 * SD card and the external USB drive are addressed uniformly, using paths such
 * as "SD:/ROMS/ATARIXL.ROM" and "USB:/disks/GAME.ATR".
 *
 * The macro names are the C standard ones on purpose: that is what makes this a
 * redirection rather than a port of the core's source.
 *
 * `FILE` keeps its C library meaning.  It is a typedef of the library's own
 * stream structure, so redefining it would make every prototype in <stdio.h>
 * conflict.  The VFS layer therefore hands out its own handles *as* `FILE *`
 * (see the header comment in src/port/vfs.h): every stdio call in the core is
 * redirected to the VFS, so such a handle can never reach the C library and be
 * dereferenced as a stream.
 *
 * `DIR` is different: it is only ever obtained from the redirected opendir, so
 * declaring it here as the VFS type is what lets the core's directory code
 * compile unchanged.  `struct dirent` comes from vfs.h for the same reason.
 * ========================================================================== */

#include "vfs.h"

typedef atari_vfs_dir_t DIR;

/*
 * <stdio.h> declares feof() and ferror() as macros.  Including it before the
 * redirection below lets the redirection win everywhere afterwards, and the
 * explicit undef keeps the compiler from warning about the redefinition.
 */
#include <stdio.h>
#undef feof
#undef ferror

#define fopen    ATARI_VFS_Open
#define fclose   ATARI_VFS_Close
#define fread    ATARI_VFS_Read
#define fwrite   ATARI_VFS_Write
#define fseek    ATARI_VFS_Seek
#define ftell    ATARI_VFS_Tell
#define rewind   ATARI_VFS_Rewind
#define fflush   ATARI_VFS_Flush
#define feof     ATARI_VFS_Eof
#define ferror   ATARI_VFS_Error
#define opendir  ATARI_VFS_OpenDir
#define readdir  ATARI_VFS_ReadDir
#define closedir ATARI_VFS_CloseDir

#endif /* CONFIG_H_ */
