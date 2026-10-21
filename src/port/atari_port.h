/*
 * atari_port.h -- the interface between the emulator kernel and the Atari800
 *                 emulation core.
 *
 * This header is the *only* thing the rest of the kernel needs to know about
 * Atari800.  Everything below it -- which files exist in the core, which
 * compile-time features are enabled, how the core's globals are named -- stays
 * inside src/port/.
 *
 * All coordinates and machine descriptions in this interface are in terms of
 * the emulated Atari, not the Raspberry Pi.
 */

#ifndef ATARI_PI_ATARI_PORT_H
#define ATARI_PI_ATARI_PORT_H

#include <stddef.h>
#include <stdint.h>

#include "joystick_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Host services
 *
 * The emulation core needs a monotonic clock (for its own timing) and a way to
 * idle until the next frame is due.  Both are supplied by the kernel, because
 * only it owns the hardware timer.  If they are not installed the core still
 * runs, but frame pacing falls back to running as fast as possible.
 * ------------------------------------------------------------------------- */

/* Returns microseconds since start-up; must never go backwards. */
typedef uint64_t (*atari_clock_fn)(void *context);

/* Sleeps for up to MICROSECONDS, and may return early. */
typedef void (*atari_idle_fn)(void *context, uint32_t microseconds);

void ATARI_SetHostServices(atari_clock_fn clock_fn, atari_idle_fn idle_fn,
                           void *context);

/* ---------------------------------------------------------------------------
 * Emulated machine description
 * ------------------------------------------------------------------------- */

/*
 * Total emulated RAM in kilobytes.
 *
 * The Atari800 core accepts a fixed set of sizes; ATARI_RAM_KB_* names the ones
 * this project offers.  Note that there is deliberately no 256 kB entry: the
 * Atari800 core has no 256 kB RAM configuration (MEMORY_SizeValid() in the
 * core accepts 64, 128, 192, 320 RAMBO and 320 Compy Shop, but not 256).
 * 320 kB RAMBO is the closest supported equivalent and is what a "256 kB
 * RAMBO XL" upgrade is normally expected to behave like once expanded.
 */
typedef enum {
	ATARI_RAM_KB_64   = 64,		/* stock 800XL / 65XE */
	ATARI_RAM_KB_128  = 128,	/* 130XE */
	ATARI_RAM_KB_192  = 192,	/* 192 kB expansion */
	ATARI_RAM_KB_320  = 320,	/* RAMBO XL 320 kB */
	ATARI_RAM_KB_320C = 321		/* Compy Shop 320 kB */
} atari_ram_kb_t;

/* Which XL/XE personality to emulate.  Both are the same hardware family in
 * the core; the difference is the fitted RAM, the presence of the F1-F4 keys
 * and the keyboard LEDs of a 1200XL. */
typedef enum {
	ATARI_MACHINE_XL = 0,		/* 600XL / 800XL */
	ATARI_MACHINE_XE,		/* 65XE / 130XE */
	ATARI_MACHINE_XEGS		/* XE Game System */
} atari_machine_t;

typedef enum {
	ATARI_VIDEO_PAL = 0,		/* 312 lines, 49.86 Hz */
	ATARI_VIDEO_NTSC		/* 262 lines, 59.92 Hz */
} atari_video_standard_t;

/* Emulated colour artifacting / PAL blending mode. */
typedef enum {
	ATARI_ARTIFACT_NONE = 0,
	ATARI_ARTIFACT_NTSC_OLD,
	ATARI_ARTIFACT_NTSC_NEW,
	ATARI_ARTIFACT_PAL_SIMPLE,	/* ANTIC-level PAL blending */
	ATARI_ARTIFACT_PAL_BLEND	/* per-pixel accurate PAL blending */
} atari_artifact_t;

/* ---------------------------------------------------------------------------
 * Start-up configuration
 * ------------------------------------------------------------------------- */

typedef struct {
	atari_machine_t machine;
	atari_video_standard_t video;
	atari_ram_kb_t ram_kb;
	atari_artifact_t artifact;

	/* POKEY output. */
	int sound_enabled;
	int sound_volume;		/* 0 .. 100 */
	int sound_stereo;		/* POKEY #2 present */
	unsigned sound_sample_rate;	/* Hz, 8000 .. 48000 */

	/*
	 * Where the emulator looks for the OS ROM images.  On the SD card these
	 * are expected in a "ROMS" directory next to the kernel image, so the
	 * value is normally "SD:/ROMS".
	 */
	const char *rom_directory;
} atari_config_t;

/* Fills CFG with the defaults this project ships with. */
void ATARI_DefaultConfig(atari_config_t *cfg);

/*
 * Brings the emulation core up.
 *
 * Loads the OS ROMs, allocates emulated memory for the requested RAM size and
 * resets the machine to its power-on state.  Returns 1 on success and 0 on
 * failure; on failure ATARI_LastError() describes what went wrong.
 */
int ATARI_Init(const atari_config_t *cfg);

/* Shuts the emulation core down and releases its buffers. */
void ATARI_Exit(void);

/* ---------------------------------------------------------------------------
 * Running
 * ------------------------------------------------------------------------- */

/*
 * Emulates exactly one Atari frame.
 *
 * On return the emulated screen is complete and can be read with
 * ATARI_ScreenIndices(); the audio produced during the frame is available
 * through ATARI_AudioBuffer()/ATARI_AudioBytes().
 *
 * Returns 1 normally, 0 if the emulated CPU hit a condition the core could not
 * continue from (see ATARI_LastError()).
 */
int ATARI_RunFrame(void);

/* Number of frames emulated since start-up. */
uint32_t ATARI_FrameNumber(void);

/* Frames per second of the current video standard, as a double. */
double ATARI_Fps(void);

/* ---------------------------------------------------------------------------
 * Emulated screen
 * ------------------------------------------------------------------------- */

/*
 * The emulated screen, as one byte per pixel holding an Atari colour index
 * (0..255).  The layout is fixed: 384 bytes per row, 240 rows, and the part
 * that a real television would show is columns 24..359.
 *
 * The core buffers this internally; the pointer stays valid until ATARI_Exit().
 */
const uint8_t *ATARI_ScreenIndices(void);

#define ATARI_SCREEN_STRIDE   384
#define ATARI_SCREEN_HEIGHT   240
#define ATARI_SCREEN_X_ORIGIN 24
#define ATARI_SCREEN_VISIBLE_WIDTH 336

/* The current 256-entry palette, packed 0x00RRGGBB, in host byte order. */
const uint32_t *ATARI_Palette(void);

/* ---------------------------------------------------------------------------
 * Audio
 * ------------------------------------------------------------------------- */

/*
 * PCM produced during the last ATARI_RunFrame().
 *
 * The format is fixed by the core: 16-bit signed little-endian samples,
 * ATARI_GetSoundChannels() channels interleaved.  The buffer is refilled every
 * frame, so it must be consumed before the next ATARI_RunFrame() call.
 */
const uint8_t *ATARI_AudioBuffer(void);
unsigned ATARI_AudioBytes(void);

/* The sample rate actually in use, which may differ from the requested one. */
unsigned ATARI_SoundSampleRate(void);
unsigned ATARI_SoundChannels(void);

/* ---------------------------------------------------------------------------
 * Keyboard
 * ------------------------------------------------------------------------- */

/*
 * The Atari keyboard state is carried in three variables that the emulation
 * core reads once per frame:
 *
 *   INPUT_key_code    the 6-bit KBCODE of the key being pressed, or a special
 *                     code for the console keys; AKEY_NONE when no key is down
 *   INPUT_key_shift   non-zero while Shift (or Control) is held
 *   INPUT_key_consol  which of START / SELECT / OPTION are held, inverted:
 *                     a clear bit means pressed
 *
 * They are set through the callback below, which the platform layer invokes
 * once at the start of every frame, just before the core emulates it.  This
 * mirrors how upstream's own ports drive the core.
 */

/* Key codes, taken from the core's akey.h.  Letters are the Atari KBCODE
 * values, which are NOT sequential -- see ATARI_KEY_LETTER. */
#define ATARI_KEY_NONE             (-1)
#define ATARI_KEY_BREAK            (-5)
#define ATARI_KEY_START            (-10)
#define ATARI_KEY_SELECT           (-11)
#define ATARI_KEY_OPTION           (-12)

/* Regular keys. */
#define ATARI_KEY_HELP             0x11	/* same physical key as START on XL/XE */
#define ATARI_KEY_DOWN             0x8f
#define ATARI_KEY_LEFT             0x86
#define ATARI_KEY_RIGHT            0x87
#define ATARI_KEY_UP               0x8e
#define ATARI_KEY_BACKSPACE        0x34
#define ATARI_KEY_DELETE_CHAR      0xb4
#define ATARI_KEY_DELETE_LINE      0x74
#define ATARI_KEY_INSERT_CHAR      0xb7
#define ATARI_KEY_INSERT_LINE      0x77
#define ATARI_KEY_ESCAPE           0x1c
#define ATARI_KEY_ATARI            0x27
#define ATARI_KEY_CAPSLOCK         0x7c
#define ATARI_KEY_CAPSTOGGLE       0x3c
#define ATARI_KEY_TAB              0x2c
#define ATARI_KEY_RETURN           0x0c
#define ATARI_KEY_SPACE            0x21
/*
 * '<' and '>' each have a matrix position of their own on the Atari, which is
 * why they are not "Shift + comma/fullstop" the way a PC keyboard spells them.
 * Their shifted forms are Clear and Insert line respectively, so setting
 * AKEY_SHFT on them does NOT give Clear -- Clear has to be its own code.
 * See ATARI_ApplyModifiers() in platform.c for how the PC keys reach them.
 */
#define ATARI_KEY_LESS             0x36	/* '<' ; shifted is Clear */
#define ATARI_KEY_GREATER          0x37	/* '>' ; shifted is Insert line */
#define ATARI_KEY_CLEAR            0x76	/* AKEY_SHFT | AKEY_LESS */
#define ATARI_KEY_MINUS            0x0e
#define ATARI_KEY_EQUAL            0x0f
#define ATARI_KEY_PLUS             0x06
#define ATARI_KEY_ASTERISK         0x07
#define ATARI_KEY_SLASH            0x26
#define ATARI_KEY_SEMICOLON        0x02
#define ATARI_KEY_COMMA            0x20
#define ATARI_KEY_FULLSTOP         0x22
#define ATARI_KEY_COLON            0x42
#define ATARI_KEY_AT               0x75
#define ATARI_KEY_BRACKETLEFT      0x60
#define ATARI_KEY_BRACKETRIGHT     0x62
#define ATARI_KEY_BACKSLASH        0x46
#define ATARI_KEY_BAR              0x4f
#define ATARI_KEY_UNDERSCORE       0x4e
#define ATARI_KEY_CIRCUMFLEX       0x47
#define ATARI_KEY_F1               0x03
#define ATARI_KEY_F2               0x04
#define ATARI_KEY_F3               0x13
#define ATARI_KEY_F4               0x14
#define ATARI_KEY_0                0x32
#define ATARI_KEY_1                0x1f
#define ATARI_KEY_2                0x1e
#define ATARI_KEY_3                0x1a
#define ATARI_KEY_4                0x18
#define ATARI_KEY_5                0x1d
#define ATARI_KEY_6                0x1b
#define ATARI_KEY_7                0x33
#define ATARI_KEY_8                0x35
#define ATARI_KEY_9                0x30

/*
 * The keyboard state the callback has to fill in.
 */
typedef struct {
	int key_code;		/* an ATARI_KEY_* value, or a KBCODE */
	int shift;		/* Shift (and Control) held */
	int control;
	int start;		/* console keys, non-zero = pressed */
	int select;
	int option;
} atari_keyboard_state_t;

/* The callback fills STATE with the keys currently held down. */
typedef void (*atari_keyboard_fn)(void *context, atari_keyboard_state_t *state);

/*
 * Installs the keyboard source.  It is called once per frame; pass NULL to
 * release the keyboard (all keys up).
 */
void ATARI_SetKeyboardSource(atari_keyboard_fn callback, void *context);

/* Converts an upper-case letter 'A'..'Z' to its Atari KBCODE. */
unsigned ATARI_KeyCodeForLetter(char letter);

/* Applies the currently-held Shift/Control combination to a base key code. */
int ATARI_ApplyModifiers(int base_key_code, int shift, int control);

/* ---------------------------------------------------------------------------
 * Machine control
 * ------------------------------------------------------------------------- */

/* Reboots the emulated machine.  COLD = power cycle, otherwise a Reset key. */
void ATARI_ColdReset(void);
void ATARI_WarmReset(void);

/* Changes that take effect immediately. */
void ATARI_SetVideoStandard(atari_video_standard_t video);
void ATARI_SetArtifact(atari_artifact_t artifact);
void ATARI_SetVolume(int volume_0_100);
void ATARI_SetStereo(int enabled);

/*
 * Turns the emulated sound on or off at the POKEY level.
 *
 * This is not amplifier muting: with sound off the core skips producing audio
 * entirely, so the emulation also gets back the time it would have spent on it.
 * It is separate from the volume, which scales what is produced.
 */
void ATARI_SetSoundEnabled(int enabled);

/* ---------------------------------------------------------------------------
 * Controllers
 *
 * The emulation core has four controller inputs, which is what an Atari 800
 * had: each of its two ports carried a second controller's lines alongside the
 * first, for four-player games.  Index 0 is the first port's main controller,
 * 1 is that port's second, 2 and 3 are the second port's pair.
 *
 * A source is polled once per frame.  Registering NULL clears the slot, which
 * is how a source that failed to start is reported.  The count is
 * ATARI_JOYSTICK_COUNT, from joystick_types.h.
 * ------------------------------------------------------------------------- */

/*
 * Provides the controller that answers for INDEX, or clears the slot if
 * pJoystick is NULL.
 *
 * The handle is an atari_joystick_t, not a C++ object: this header is included
 * from C, and the drivers are C++.  A C++ driver hands over CAtariJoystick::
 * Handle().  The object must outlive every call, because the platform layer
 * holds the pointer and calls through it once per frame.
 */
void ATARI_SetJoystick(int index, atari_joystick_t *pJoystick);

/* \return The controller registered for INDEX, or NULL if there is none. */
atari_joystick_t *ATARI_GetJoystick(int index);

/* ---------------------------------------------------------------------------
 * Machine state snapshot, for diagnostics
 *
 * A machine that never produces a picture is either not running its OS at all
 * or running something broken, and the two look identical from outside -- a
 * black screen either way.  These are the values that tell them apart.
 *
 * It is a plain struct of integers so that this C header stays usable from both
 * languages, and it is filled once per machine start rather than continuously.
 * ------------------------------------------------------------------------- */

typedef struct {
	int	os_version;	/* SYSROM index of the chosen OS; -1 means none */
	int	machine_type;	/* Atari800_MACHINE_* */
	int	ram_kb;
	unsigned antic_dlist;	/* display list address; 0 means never programmed */
	unsigned antic_dmactl;	/* bit 0x20 enables display list DMA */
	unsigned antic_scan;	/* scan line ANTIC has reached, from ANTIC_xpos */
	unsigned antic_mode;	/* ANTIC_xpos within the line, in cycles */
	unsigned cpu_pc;	/* where the emulated 6502 is */
	unsigned cpu_a;		/* accumulator, for a quick sanity check */
	unsigned portb;		/* the OS/bank select register */
	unsigned frames;	/* frames emulated since power-on */
	unsigned rom_files;	/* ROM images this project loaded at start-up */
	unsigned char has_basic;/* false when the BASIC ROM is absent */
	unsigned char pending;	/* taken but not yet collected */
	unsigned char taken;	/* a snapshot exists */
} atari_state_snapshot_t;

/*
 * Collects the snapshot, if one is waiting, and clears the waiting flag.
 *
 * \return 1 if SNAPSHOT was filled, 0 if there was nothing to collect.
 */
int ATARI_TakeStateSnapshot(atari_state_snapshot_t *snapshot);

/*
 * Whether the last frame error was a CPU crash.
 *
 * A crash leaves the emulated machine somewhere it cannot continue from, so it
 * is worth restarting.  The other frame errors -- a missing display list, an
 * invalid one -- describe a machine that is running but not drawing, and
 * restarting it on every frame only produces a flicker that hides the cause.
 * The caller uses this to tell the two apart.
 */
int ATARI_LastErrorIsCpuCrash(void);

/* ---------------------------------------------------------------------------
 * Runtime log
 * ------------------------------------------------------------------------- */

/*
 * Receives one line of text for the runtime log.
 *
 * This header is C and the runtime log is a C++ class, so the sink is installed
 * as a plain function pointer rather than being called directly.  That is what
 * lets the platform layer -- and through it the emulation core's disk handling
 * -- record what it is doing into RUNTIMELOG.TXT.
 *
 * Every line is written and flushed by the sink, so the last line in the file
 * is the last thing that happened before a hang.  For a fault that nothing else
 * can report, surviving is the whole value of the log.
 */
typedef void (*atari_runtime_log_fn)(const char *message);

void ATARI_SetRuntimeLog(atari_runtime_log_fn fn);

/*
 * Writes a line to the runtime log, if a sink was installed.
 *
 * Safe to call at any time, including before one is installed, in which case it
 * does nothing.
 */
void ATARI_RuntimeLog(const char *format, ...);
/*
 * Changes to the fitted RAM or the machine personality that require the
 * emulated machine to be rebuilt.  The machine is reset to power-on as a
 * side effect.  Returns 1 on success.
 */
int ATARI_SetRamSize(atari_ram_kb_t ram_kb);
int ATARI_SetMachine(atari_machine_t machine);

atari_ram_kb_t ATARI_GetRamSize(void);
atari_machine_t ATARI_GetMachine(void);
atari_video_standard_t ATARI_GetVideoStandard(void);

/* ---------------------------------------------------------------------------
 * Media
 * ------------------------------------------------------------------------- */

/*
 * Mounts a disk image into drive DRIVE (1..8).
 *
 * The image may be an ATR or an XFD file.  When READ_ONLY is zero the image is
 * opened for writing and any sector the emulated machine writes is written back
 * to it.  Pass a NULL or empty PATH to eject whatever is in the drive.
 *
 * Returns 1 on success.
 */
int ATARI_MountDisk(int drive, const char *path, int read_only);

/* Ejects the image in DRIVE.  Returns 1 on success. */
int ATARI_UnmountDisk(int drive);

/*
 * Performs a cold start that a cartridge insertion asked for, if one is pending.
 *
 * Inserting a cartridge cannot reset the machine on the spot: the call arrives
 * from inside the menu, and running the emulated machine there -- with another
 * image already open on the same storage -- is what hung the machine.  The
 * request is held until the menu closes, and the kernel calls this at that
 * transition.  Does nothing when nothing is pending.
 */
void ATARI_ApplyPendingColdStart(void);

/*
 * Drops a pending cold start without performing it.
 *
 * Needed once, at the end of bring-up.  Mounting the saved cartridge asks for a
 * deferred reset, which is the right behaviour from the menu but not while the
 * machine is being initialised: it has just been cold-started, and the cartridge
 * is mapped as it is read.  Without this the request stays pending and fires the
 * first time the menu closes, restarting the machine for no reason.
 */
void ATARI_ClearPendingColdStart(void);

/*
 * Writes the emulated machine's state to the runtime log.
 *
 * Called from the exception handler, where the host has already failed.  The
 * question worth answering in that moment is what the *emulated* 6502 was
 * doing: its program counter names the ROM routine, and the ANTIC and SIO state
 * say whether it was in the serial handler or drawing.  All of it is read from
 * plain globals, so this is safe in a faulted context.
 */
void ATARI_LogCrashState(void);

/*
 * Crash marker.
 *
 * ATARI_MarkCrashed() records that the emulator died, in a small file beside
 * the settings.  ATARI_CrashMarkerConsume() returns 1 once if such a record
 * exists and clears it, so the next boot can start from a clean machine.
 *
 * This exists because doing a full settings rewrite inside the exception
 * handler would mean trusting the disk layer while the CPU is in an unknown
 * state.  Writing one byte is the smallest risk; the decisions are taken on the
 * next boot, where the machine is healthy again.
 */
void ATARI_MarkCrashed(void);
int ATARI_CrashMarkerConsume(void);

/* Mounts a cartridge image (CAR format) or removes the current one. */
int ATARI_MountCartridge(const char *path);

/* Mounts a cassette image (CAS format) or removes the current one. */
int ATARI_MountCassette(const char *path, int read_only);

/* Inserts a bootable executable (XEX) and reboots into it. */
int ATARI_BootFile(const char *path);

/* ---------------------------------------------------------------------------
 * Snapshots
 * ------------------------------------------------------------------------- */

/*
 * Saves or restores a complete machine snapshot.
 *
 * A snapshot covers the emulated RAM, the chip state and the CPU registers --
 * everything needed to resume exactly where the machine was.  Mounted media is
 * not part of the snapshot and is re-mounted from the settings instead.
 *
 * Returns 1 on success.
 */
int ATARI_SaveSnapshot(const char *path);
int ATARI_LoadSnapshot(const char *path);

/* ---------------------------------------------------------------------------
 * Diagnostics
 * ------------------------------------------------------------------------- */

/* A human readable description of the last failure, or "" if none. */
const char *ATARI_LastError(void);

/* The core's version banner. */
const char *ATARI_CoreVersion(void);

#ifdef __cplusplus
}
#endif

#endif /* ATARI_PI_ATARI_PORT_H */
