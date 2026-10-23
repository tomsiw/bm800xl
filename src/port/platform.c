/*
 * platform.c -- the host platform layer for the Atari800 emulation core.
 *
 * This file is the bridge between the portable Atari800 core and this project.
 * It has three jobs:
 *
 *   1. Implement the platform hooks the core calls into
 *      (PLATFORM_Time, PLATFORM_Sleep, PLATFORM_Initialise,
 *       PLATFORM_DisplayScreen, PLATFORM_Keyboard, ...).
 *
 *   2. Own the emulation loop.  Upstream's own front ends own this loop; here
 *      it is ATARI_RunFrame(), which drives exactly one Atari frame and hands
 *      the finished screen and the audio produced during it to the caller.
 *
 *   3. Own the machine configuration -- RAM size, XL/XE personality, PAL/NTSC,
 *      sound settings, ROM images and mounted media -- and apply it the way the
 *      core expects.
 *
 * The core is compiled with LIBATARI800 defined, which is the flag upstream
 * uses to mean "the embedding application owns the main loop, the display and
 * the sound device".  That is precisely the situation here, so none of the
 * core's own front ends are compiled in.
 */

#include "config.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "atari.h"
#include "afile.h"
#include "akey.h"
#include "antic.h"
#include "atari_port.h"
#include "host_services.h"
#include "joystick_types.h"
#include "artifact.h"
#include "cartridge.h"
#include "cassette.h"
#include "colours.h"
#include "crc32.h"
#include "esc.h"
#include "input.h"
#include "log.h"
#include "memory.h"
#include "platform.h"
#include "pia.h"
#include "pokeysnd.h"
#include "screen.h"
#include "sio.h"
#include "sound.h"
#include "statesav.h"
#include "sysrom.h"
#include "util.h"

#include "libatari800/libatari800.h"
#include "libatari800/cpu_crash.h"
#include "libatari800/sound.h"
#include "libatari800/statesav.h"

/*
 * The CPU core abandons a frame by longjmp-ing to this buffer when the
 * emulated machine executes a CIM instruction or an invalid escape opcode.
 * ATARI_RunFrame() arms it with setjmp on every frame, so the jump always lands
 * in a defined place and the Raspberry Pi itself is never harmed by a bad
 * Atari program.
 */
#ifdef HAVE_SETJMP
jmp_buf libatari800_cpu_crash;

/*
 * Set by the CPU core when it encounters a CIM instruction.  It is declared
 * through the core's own header rather than by hand: the two declarations have
 * to agree on the type.  Declaring it as an int when the core defines a single
 * byte makes the AArch64 linker refuse the link, because a 32-bit access to a
 * one-byte object is not representable ("relocation truncated to fit").
 */
#include "cpu.h"
#endif

/* ===========================================================================
 * Host services
 *
 * ATARI_RunFrame() needs a microsecond clock and a way to idle until the next
 * frame is due.  Those live in the kernel (they need a running Circle timer),
 * so they are supplied from outside and recorded here.
 * ======================================================================== */

/*
 * The core honours an ATR image's own write-protect flag -- the last byte of its
 * sixteen-byte header -- whatever mode the file was opened in, and it offers no
 * setter for the flag that turns that off.  This project's menu decides per disk
 * instead, with its [ro] entry, so an image carrying the flag could never be
 * written: the drive reports write-protected and the machine refuses, even
 * though read/write was asked for.  The core's rule is therefore turned off
 * once, in ATARI_Init().
 *
 * Declared here rather than from a core header because the core does not put it
 * in one: it is a plain global in sio.c.
 */
extern int ignore_header_writeprotect;

typedef uint64_t (*host_clock_fn)(void *context);
typedef void (*host_idle_fn)(void *context, uint32_t microseconds);

static host_clock_fn s_host_clock;
static host_idle_fn s_host_idle;
static void *s_host_context;

/*
 * Installs the host's clock and idle functions.
 *
 * The clock must return microseconds and must not go backwards.  The idle
 * function should do nothing if the requested delay has already elapsed.
 */
void ATARI_SetHostServices(host_clock_fn clock_fn, host_idle_fn idle_fn, void *context)
{
	s_host_clock = clock_fn;
	s_host_idle = idle_fn;
	s_host_context = context;
}

/* ===========================================================================
 * PLATFORM_* hooks
 * ======================================================================== */

#ifdef SUPPORTS_PLATFORM_TIME
double PLATFORM_Time(void)
{
	if (s_host_clock == 0) {
		return 0.0;
	}
	return (double)s_host_clock(s_host_context) / 1000000.0;
}
#endif

#ifdef SUPPORTS_PLATFORM_SLEEP
void PLATFORM_Sleep(double seconds)
{
	if (s_host_idle == 0 || seconds <= 0.0) {
		return;
	}
	s_host_idle(s_host_context, (uint32_t)(seconds * 1000000.0));
}
#endif

/*
 * Called by the core's initialisation, before the machine is configured.
 * Everything that needs a configured machine happens in ATARI_Init() instead.
 */
int PLATFORM_Initialise(int *argc, char *argv[])
{
	(void)argc;
	(void)argv;
	return TRUE;
}

int PLATFORM_Exit(int run_monitor)
{
	(void)run_monitor;
	return 0;
}

/*
 * Called once per frame by the emulation core, after the frame has been
 * rendered into Screen_atari.  The frame is published as soon as it is
 * complete, so there is nothing to do here.
 */
void PLATFORM_DisplayScreen(void)
{
}

/*
 * The core asks for the keyboard here.  The real work is done by
 * PushKeyboardState(), which runs at the top of every frame; this hook exists
 * so that the core sees a consistent answer if it ever asks.
 */
int PLATFORM_Keyboard(void)
{
	return AKEY_NONE;
}

/*
 * The emulated controller ports.
 *
 * The Atari reads a port as active low -- a bit that is CLEAR means that
 * direction is held, and a trigger that is LOW means the button is pressed --
 * because on the real machine each switch connects its line to ground.
 *
 * Four sources are supported, paired into the two physical ports: port 0
 * carries sources 0 and 1 in its low and high nibble, and port 1 carries
 * sources 2 and 3.  That is the wiring of an Atari 800, whose two ports each had
 * a second controller's lines alongside for four-player games.
 *
 * A source that is absent contributes its idle state -- all four bits set,
 * trigger open -- rather than zero, so that a machine with no joystick reads as
 * centred instead of as every direction held at once.
 */
static atari_joystick_t *s_pJoystick[ATARI_JOYSTICK_COUNT];

void ATARI_SetJoystick(int index, atari_joystick_t *pJoystick)
{
	if (index >= 0 && index < ATARI_JOYSTICK_COUNT) {
		s_pJoystick[index] = pJoystick;
	}
}

atari_joystick_t *ATARI_GetJoystick(int index)
{
	if (index < 0 || index >= ATARI_JOYSTICK_COUNT) {
		return 0;
	}

	return s_pJoystick[index];
}

/*
 * Directions for source N, as the bits that ARE held.
 *
 * Joysticks report the directions that are held, and ATARI_JOY_UP and friends
 * are the bits a port reading CLEARS, so the sets are inverses of each other.
 */
static unsigned JoystickDirections(int source)
{
	if (source < 0 || source >= ATARI_JOYSTICK_COUNT) {
		return 0;
	}
	if (s_pJoystick[source] == 0 || !s_pJoystick[source]->is_present(s_pJoystick[source])) {
		return 0;
	}

	return s_pJoystick[source]->get_directions(s_pJoystick[source]);
}

int PLATFORM_PORT(int num)
{
	unsigned low;
	unsigned high;

	if (num == 0) {
		low = 0;
		high = 1;
	}
	else if (num == 1) {
		low = 2;
		high = 3;
	}
	else {
		/* Only two ports exist on an Atari XL/XE. */
		return 0xff;
	}

	/*
	 * The directions held have their bits set; a port reading has those bits
	 * cleared, so the four direction bits are built from the inverse.
	 */
	unsigned low_bits = (~JoystickDirections((int)low)) & 0x0f;
	unsigned high_bits = (~JoystickDirections((int)high)) & 0x0f;

	return (int)(low_bits | (high_bits << 4));
}

int PLATFORM_TRIG(int num)
{
	if (num < 0 || num >= ATARI_JOYSTICK_COUNT) {
		return ATARI_JOY_TRIG_OPEN;
	}
	if (s_pJoystick[num] == 0 || !s_pJoystick[num]->is_present(s_pJoystick[num])) {
		return ATARI_JOY_TRIG_OPEN;
	}

	return s_pJoystick[num]->get_trigger(s_pJoystick[num]);
}

/* ---------------------------------------------------------------------------
 * Native pixel format
 *
 * Only needed by the core's "accurate PAL blending" mode, which pre-blends the
 * palette in whatever format the display uses.  This project's display is
 * RGB565, so that is what is described here.  The lookup tables it fills are
 * unused unless ARTIFACT_PAL_BLEND is selected, but the hooks have to exist for
 * the core to link.
 * ------------------------------------------------------------------------- */

#ifdef PLATFORM_MAP_PALETTE

void PLATFORM_GetPixelFormat(PLATFORM_pixel_format_t *format)
{
	format->bpp = 16;
	format->rmask = 0xf800;		/* red   in bits 15..11 */
	format->gmask = 0x07e0;		/* green in bits 10..5  */
	format->bmask = 0x001f;		/* blue  in bits  4..0  */
}

void PLATFORM_MapRGB(void *dest, int const *palette, int size)
{
	UWORD *out = (UWORD *)dest;
	int i;

	for (i = 0; i < size; ++i) {
		int rgb = palette[i];
		unsigned r = (unsigned)((rgb >> 16) & 0xff) >> 3;
		unsigned g = (unsigned)((rgb >> 8) & 0xff) >> 2;
		unsigned b = (unsigned)(rgb & 0xff) >> 3;

		out[i] = (UWORD)((r << 11) | (g << 5) | b);
	}
}

#endif /* PLATFORM_MAP_PALETTE */

/* ===========================================================================
 * ROM images
 * ======================================================================== */

/*
 * Checksum-based ROM identification
 *
 * The core's ROM table records, for every OS and BASIC image ever shipped, its
 * length and CRC32.  That is the only reliable way to tell which OS revision a
 * file is, and it matters: the core's automatic OS selection consults that table,
 * so a file registered under the wrong entry is simply never chosen.
 *
 * The obvious alternative -- registering each file under one of the core's four
 * "custom" slots, which is what upstream's own command line does -- does not work
 * here.  Two things go wrong with it:
 *
 *   1. The custom slots have a NULL CRC, so the core accepts any file of the
 *      right length.  A truncated or unrelated ROM would load happily and the
 *      machine would fail in a way that looks like an emulator bug.
 *
 *   2. More seriously, the custom IDs come *last* in every automatic selection
 *      order, after the built-in AltirraOS.  With EMUOS_ALTIRRA defined,
 *      AltirraOS always has data, so it always wins and the operator's own ROM
 *      files would never be used at all -- while the log cheerfully reported
 *      that the ROMs had been found.
 *
 * So each file is measured and matched against the table by hand, and
 * registered under the ID it actually is.  Registering normally also clears any
 * built-in data for that ID, which is what lets a real OS ROM take precedence
 * over AltirraOS while still leaving AltirraOS as the fallback when no ROM file
 * is present.
 */

/*
 * The ROM files this project expects, and the range of ROM IDs each could be.
 *
 * The ranges are the spans of consecutive IDs in the core's table: the OS ROMs
 * first, then the 5200 BIOSes, then the BASIC ROMs.  SYSROM_LOADABLE_SIZE marks
 * the end of the file-loadable entries, so no built-in Altirra entry is ever
 * considered a match for a file.
 */
typedef struct {
	const char *file_name;	/* as shipped, spelling as supplied */
	int first_id;		/* first ROM ID this file could be */
	int last_id;		/* last ROM ID this file could be */
	const char *description;
} rom_file_t;

static const rom_file_t s_rom_files[] = {
	{ "ATARIXL.ROM",  SYSROM_A_NTSC,       SYSROM_BB01R59A,    "XL/XE OS" },
	{ "ATARIOSA.ROM", SYSROM_A_NTSC,       SYSROM_BB01R59A,    "Atari 800 OS" },
	{ "ATARIOSB.ROM", SYSROM_A_NTSC,       SYSROM_BB01R59A,    "Atari 800 OS" },
	{ "ATARIBAS.ROM", SYSROM_BASIC_A,      SYSROM_BASIC_C,     "Atari BASIC" },
	{ "atari5200.rom", SYSROM_5200,        SYSROM_5200A,       "Atari 5200 BIOS" },
};

#define ROM_FILE_COUNT (sizeof s_rom_files / sizeof s_rom_files[0])

/* Builds "<directory>/<name>" into TARGET. */
static void JoinPath(char *target, size_t target_size, const char *directory, const char *name)
{
	size_t dir_len = strlen(directory);
	size_t name_len = strlen(name);
	int need_separator;

	if (dir_len == 0) {
		if (name_len + 1 > target_size) {
			target[0] = '\0';
			return;
		}
		memcpy(target, name, name_len + 1);
		return;
	}

	need_separator = directory[dir_len - 1] != '/';

	if (dir_len + (need_separator ? 1 : 0) + name_len + 1 > target_size) {
		target[0] = '\0';
		return;
	}

	memcpy(target, directory, dir_len);
	if (need_separator) {
		target[dir_len++] = '/';
	}
	memcpy(target + dir_len, name, name_len + 1);
}

/*
 * Finds the ROM ID whose recorded length and CRC32 match PATH.
 *
 * Returns the ID, or -1 if the file cannot be read or is not one of the images
 * the core knows about.  The core's own table is the source of truth here, so
 * this stays correct if the table changes.
 */
static int IdentifyRomFile(const char *path, int first_id, int last_id, ULONG *pCrc,
			   long *pLength)
{
	FILE *file;
	ULONG crc;
	long length;
	int id;

	file = fopen(path, "rb");
	if (file == 0) {
		return -1;
	}

	if (!CRC32_FromFile(file, &crc)) {
		fclose(file);
		return -1;
	}

	length = ATARI_VFS_Size(file);
	fclose(file);

	if (length <= 0) {
		return -1;
	}

	if (pCrc != 0) {
		*pCrc = crc;
	}
	if (pLength != 0) {
		*pLength = length;
	}

	/*
	 * The Altirra images are compiled into the kernel and have no CRC, so they
	 * can never match a file; stopping before SYSROM_LOADABLE_SIZE keeps that
	 * explicit rather than relying on CRC_NULL never equalling a real checksum.
	 */
	if (last_id >= SYSROM_LOADABLE_SIZE) {
		last_id = SYSROM_LOADABLE_SIZE - 1;
	}

	for (id = first_id; id <= last_id; ++id) {
		/*
		 * A zero CRC marks a table entry that has no expected checksum: the
		 * built-in Altirra images, and the four "custom" slots.  Neither can
		 * be identified from a file, so neither is a match.  The core spells
		 * this value as its own private CRC_NULL, which it does not export,
		 * so the value is repeated here with this note.
		 */
		if (SYSROM_roms[id].crc32 == 0) {
			continue;
		}
		if ((long)SYSROM_roms[id].size == length
		    && SYSROM_roms[id].crc32 == crc) {
			return id;
		}
	}

	return -1;
}

/*
 * Registers the OS and BASIC ROM images found in DIRECTORY.
 *
 * Returns the number of images registered.  A file that is absent, unreadable
 * or not recognised is skipped: the core falls back to AltirraOS for the OS and
 * to no BASIC, and the caller reports what happened.
 */
static int LoadRomImages(const char *directory)
{
	unsigned i;
	int loaded = 0;

	for (i = 0; i < ROM_FILE_COUNT; ++i) {
		char path[128];
		ULONG crc;
		long length;
		int id;

		JoinPath(path, sizeof path, directory, s_rom_files[i].file_name);
		if (path[0] == '\0') {
			continue;
		}

		id = IdentifyRomFile(path, s_rom_files[i].first_id,
				     s_rom_files[i].last_id, &crc, &length);
		if (id < 0) {
			Log_print("%s: %s is missing or is not a known image",
				  s_rom_files[i].file_name, s_rom_files[i].description);
			continue;
		}

		/* Record it under the ID it really is, so the core can choose it. */
		strcpy(SYSROM_roms[id].filename, path);
		SYSROM_roms[id].unset = FALSE;

		/*
		 * A file takes precedence over any image compiled into the kernel for
		 * the same slot.  This is the whole point: with AltirraOS built in,
		 * leaving its data in place would shadow the operator's own ROM.
		 */
		SYSROM_roms[id].data = NULL;

		Log_print("%s: %s is %s, %ld bytes, CRC32 %08lx",
			  s_rom_files[i].file_name, path,
			  s_rom_files[i].description, length, (unsigned long)crc);
		++loaded;
	}

	return loaded;
}

/* ===========================================================================
 * Emulator state
 * ======================================================================== */

static atari_config_t s_config;
static int s_initialised;
static char s_last_error[160];

/*
 * There used to be a static here recording "channels the sink was opened with,
 * zero if nothing is open".  It was only ever written on the path where this file
 * opens the sink itself, but at start-up the core opens it first -- Sound_Init
 * calls PLATFORM_SoundSetup during initialisation, before ApplySoundConfiguration
 * runs -- so the variable read zero while sound was audibly working, and the log
 * said "stereo change deferred until restart (0 -> 1 channels)" on a machine that
 * was already running mono.  The negotiated shape is in Sound_out, which the core
 * fills in from whatever the device accepted, so that is what is read instead.
 */

/* Rate used when the configured one is missing or out of range. */
#define SOUND_RATE_FALLBACK_HZ	48000
static int s_frame_error;

/*
 * How long ANTIC is given to be set up before "no display list" is an error.
 *
 * The Atari OS enables display DMA and writes its display list within the first
 * frame or two of a cold start.  A second of emulated time is far more than that
 * needs, and short enough that a genuinely broken program is still reported
 * quickly.  At the NTSC rate that is 60 frames, PAL 50.
 */
#define DLIST_GRACE_FRAMES 60

/*
 * How long the display may stay blank before it counts as a fault.
 *
 * Five seconds of emulated time.  This has to clear the longest legitimate
 * blank, which is SELF TEST at power-on and disk loading, both of which leave
 * the display off for a second or two.  It is deliberately generous: a false
 * report here is drawn over a working screen and is far more damaging than a
 * late one.
 */
#define BLANK_FRAMES_BEFORE_ERROR	(50 * 5)

/*
 * Every frame from the OS's point of view, in 6502 cycles.
 *
 * A PAL Atari frame is 312 scan lines of 114 cycles; NTSC is 262 of 114.  Used
 * only to report how much emulated CPU time has passed, which is what tells a
 * slow machine from a stuck one.
 */
#define CYCLES_PER_FRAME_PAL	(312 * 114)
#define CYCLES_PER_FRAME_NTSC	(262 * 114)

/*
 * Frames emulated since the machine last started or was reset.
 *
 * Reset by every path that restarts the emulated machine, so that a machine
 * that has just been reset is given the same grace period as one that has just
 * been powered on.
 */
static unsigned s_frames_since_reset;

/*
 * Consecutive frames in which ANTIC had no display list.
 *
 * Reset whenever a display list exists.  Used to tell a screen that is briefly
 * blank -- which is normal, and happens at every start-up and around SELF TEST
 * -- from a display that has genuinely been left off for good.
 */
static unsigned s_blank_frames;

/*
 * Rate measurement, reported once about ten seconds in.
 *
 * The achieved frame rate is the single number that says whether the host is
 * keeping up with the emulated machine, and it cannot be inferred from the log
 * timestamps of anything else.
 */
static uint64_t s_rate_start_us;
static unsigned s_rate_start_frames;

/*
 * Boot watchdog.
 *
 * Armed by every cold reset.  For the first minute afterwards it writes a short
 * line every half minute saying where the emulated machine is and -- the point
 * of it -- how many disk reads have happened in total.  If the count does not
 * move while the frame count does, the machine is running but is not talking to
 * the disk, which is the difference between a boot that is slow and a boot that
 * never started.
 *
 * Sparse on purpose: the stats line already reports frames, PC and disk reads
 * every fifteen seconds, so this one only has to catch the case where the log
 * shows no progress at all, and it adds the two fields the stats line lacks --
 * PORTB and the error count.  Two lines cover the minute in which a real machine
 * boots; a program left running for an hour writes nothing more.
 */
#define BOOT_LOG_INTERVAL_US	30000000ULL
#define BOOT_LOG_MAX		2

static int	s_boot_log_armed;
static uint64_t s_boot_log_start_us;
static unsigned s_boot_log_count;

/*
 * A snapshot of the emulated machine's state, taken once per start.
 *
 * See ATARI_TakeStateSnapshot().  The fields are plain integers so that this can
 * cross into C++ without a header that both languages can use.
 */
atari_state_snapshot_t s_state_snapshot;

/*
 * How many ROM images were accepted at start-up.
 *
 * Kept because a machine with no OS ROM behaves very differently from one with,
 * and "the ROM file is present" and "the ROM file was accepted" are not the same
 * statement -- the core validates length and CRC.
 */
static int s_rom_files_loaded;

/*
 * Whether the most recent frame error was a CPU crash.
 *
 * The distinction matters to the caller: a crash is worth restarting from, a
 * machine that is merely not drawing is not.  See ATARI_LastErrorIsCpuCrash().
 */
static int s_last_error_is_cpu_crash;

/*
 * Frame boundary tracing budget.
 *
 * Temporary, for tracking down a hang that happens inside a frame.  Bounded so
 * that turning it on cannot fill the card: once the budget is spent the log goes
 * quiet, and the lines already written say what was happening when it stopped.
 */
/*
 * Frame boundary tracing budget.
 *
 * Kept deliberately small.  It was dense for 400 frames at first, which produced
 * a log so long that its tail was hard to find; the useful question turned out
 * not to be "which frame blocked" but "is the machine talking to its disk", and
 * that is what the boot watchdog answers.  A short dense burst after each reset
 * is still worth having, because it catches a fault in the first moments of a
 * boot without needing to reproduce it twice.
 */
#define DIAG_FRAME_INTERVAL	60	/* frames between samples, once settled */
#define DIAG_DENSE_FRAMES	60	/* every frame for this long after a reset */
#define DIAG_MAX_FRAME_LOGS	400	/* lines per boot before tracing stops */

static unsigned s_diag_frame_logs;


static atari_keyboard_fn s_keyboard_fn;
static void *s_keyboard_context;

static uint64_t s_next_frame_time;	/* microseconds, against the host clock */
static uint64_t s_frame_phase;		/* fractional tick accumulator, in 1/65536 us */

static void SetError(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vsnprintf(s_last_error, sizeof s_last_error, format, args);
	va_end(args);
}

const char *ATARI_LastError(void)
{
	return s_last_error;
}

const char *ATARI_CoreVersion(void)
{
	return ATARI800_TITLE;
}

void ATARI_DefaultConfig(atari_config_t *cfg)
{
	if (cfg == 0) {
		return;
	}
	cfg->machine = ATARI_MACHINE_XL;
	cfg->video = ATARI_VIDEO_PAL;
	cfg->ram_kb = ATARI_RAM_KB_64;
	cfg->artifact = ATARI_ARTIFACT_PAL_SIMPLE;
	cfg->sound_enabled = 1;
	cfg->sound_volume = 80;
	cfg->sound_stereo = 0;
	cfg->sound_sample_rate = 44100;
	cfg->rom_directory = "SD:/ROMS";
}

/* ===========================================================================
 * Keyboard
 * ======================================================================== */

void ATARI_SetKeyboardSource(atari_keyboard_fn callback, void *context)
{
	s_keyboard_fn = callback;
	s_keyboard_context = context;
}

unsigned ATARI_KeyCodeForLetter(char letter)
{
	/*
	 * The Atari keyboard is wired in a matrix and the KBCODE values are not
	 * sequential, so letters are looked up rather than computed.  These
	 * values come from akey.h in the emulation core.
	 */
	static const unsigned char codes[26] = {
		0x3f, /* A */ 0x15, /* B */ 0x12, /* C */ 0x3a, /* D */
		0x2a, /* E */ 0x38, /* F */ 0x3d, /* G */ 0x39, /* H */
		0x0d, /* I */ 0x01, /* J */ 0x05, /* K */ 0x00, /* L */
		0x25, /* M */ 0x23, /* N */ 0x08, /* O */ 0x0a, /* P */
		0x2f, /* Q */ 0x28, /* R */ 0x3e, /* S */ 0x2d, /* T */
		0x0b, /* U */ 0x10, /* V */ 0x2e, /* W */ 0x16, /* X */
		0x2b, /* Y */ 0x17  /* Z */
	};

	if (letter >= 'a' && letter <= 'z') {
		letter = (char)(letter - 'a' + 'A');
	}
	if (letter < 'A' || letter > 'Z') {
		return (unsigned)ATARI_KEY_NONE;
	}
	return codes[letter - 'A'];
}

int ATARI_ApplyModifiers(int base_key_code, int shift, int control)
{
	if (base_key_code < 0) {
		return base_key_code;
	}

	if (shift) {
		/*
		 * Setting AKEY_SHFT is not always enough.
		 *
		 * The Atari matrix uses one bit for Shift, and the OS looks the
		 * character up from the key position plus that bit.  That works only
		 * where the shifted character sits on the same key.  Several do not:
		 * '<', '>', '|' and Clear each have a matrix position of their own, so
		 * OR-ing the shift bit onto the unshifted key produces a different
		 * character or an unrelated key code entirely.
		 *
		 * The two that matter most are comma and fullstop.  The Atari puts '['
		 * and ']' on Shift+comma and Shift+fullstop, while a PC prints '<' and
		 * '>' there.  Honouring the PC legend is the only way '<' and '>' can be
		 * typed from a PC keyboard at all, because no PC key maps to the Atari's
		 * own '<' and '>' positions.  Nothing is lost: '[' and ']' keep their
		 * dedicated PC keys, which map to ATARI_KEY_BRACKETLEFT/RIGHT.
		 *
		 * The rest follow the same principle -- the character on the PC key cap
		 * is the one intended -- and each replaces a combination that previously
		 * produced a wrong or meaningless key code.
		 */
		switch (base_key_code) {
		case ATARI_KEY_COMMA:
			return ATARI_KEY_LESS;		/* '<' */

		case ATARI_KEY_FULLSTOP:
			return ATARI_KEY_GREATER;	/* '>' */

		case ATARI_KEY_BACKSLASH:
			return ATARI_KEY_BAR;		/* '|' */

		case ATARI_KEY_ESCAPE:
			return ATARI_KEY_CLEAR;		/* the Atari Clear key */

		case ATARI_KEY_DELETE_CHAR:
			return ATARI_KEY_DELETE_LINE;	/* Shift+Delete */

		case ATARI_KEY_INSERT_CHAR:
			return ATARI_KEY_INSERT_LINE;	/* Shift+Insert */

		default:
			break;
		}

		base_key_code |= AKEY_SHFT;
	}

	if (control) {
		base_key_code |= AKEY_CTRL;
	}
	return base_key_code;
}

/*
 * Reads the keyboard once per frame and stores it into the core's globals.
 */
static void PushKeyboardState(void)
{
	atari_keyboard_state_t state;

	memset(&state, 0, sizeof state);
	state.key_code = ATARI_KEY_NONE;

	if (s_keyboard_fn != 0) {
		s_keyboard_fn(s_keyboard_context, &state);
	}

	/*
	 * Both keyboard drivers report the *base* key -- the one printed on the
	 * keycap -- and the modifier state separately, which is how a real
	 * keyboard behaves.  The Atari, however, carries Shift and Control as bits
	 * 6 and 7 of the key code it delivers to POKEY, so they have to be folded
	 * in here.  Without this, Shift-A would arrive as a plain 'A'.
	 *
	 * ATARI_ApplyModifiers() leaves the core's special (negative) codes alone,
	 * because those are not key codes at all: they are commands like "warm
	 * start" which the frame loop acts on.
	 */
	INPUT_key_code = ATARI_ApplyModifiers (state.key_code, state.shift, state.control);

	/*
	 * Bit 3 of SKSTAT is what the hardware reports for Shift, and it is
	 * separate from the key code above.
	 */
	INPUT_key_shift = (state.shift || state.control) ? 1 : 0;

	/*
	 * The console keys are negative logic: a cleared bit means pressed.
	 */
	INPUT_key_consol = INPUT_CONSOL_NONE;
	if (state.option) {
		INPUT_key_consol &= ~INPUT_CONSOL_OPTION;
	}
	if (state.select) {
		INPUT_key_consol &= ~INPUT_CONSOL_SELECT;
	}
	if (state.start) {
		INPUT_key_consol &= ~INPUT_CONSOL_START;
	}
}

/* ===========================================================================
 * Configuration applied to the core
 * ======================================================================== */

static int CoreRamSize(atari_ram_kb_t ram_kb)
{
	switch (ram_kb) {
	case ATARI_RAM_KB_64:
		return 64;
	case ATARI_RAM_KB_128:
		return 128;
	case ATARI_RAM_KB_192:
		return 192;
	case ATARI_RAM_KB_320:
		return MEMORY_RAM_320_RAMBO;
	case ATARI_RAM_KB_320C:
		return MEMORY_RAM_320_COMPY_SHOP;
	default:
		return 64;
	}
}

static int CoreMachineType(atari_machine_t machine)
{
	/* The XL, XE and XEGS are all the same hardware family to the core. */
	(void)machine;
	return Atari800_MACHINE_XLXE;
}

static int CoreTvMode(atari_video_standard_t video)
{
	return video == ATARI_VIDEO_PAL ? Atari800_TV_PAL : Atari800_TV_NTSC;
}

static int CoreArtifact(atari_video_standard_t video, atari_artifact_t artifact)
{
	switch (artifact) {
	case ATARI_ARTIFACT_NTSC_OLD:
		return ARTIFACT_NTSC_OLD;
	case ATARI_ARTIFACT_NTSC_NEW:
		return ARTIFACT_NTSC_NEW;
	case ATARI_ARTIFACT_PAL_SIMPLE:
		return ARTIFACT_PAL_SIMPLE;
	case ATARI_ARTIFACT_PAL_BLEND:
#ifdef PAL_BLENDING
		/* Accurate PAL blending is only meaningful on a PAL machine. */
		return video == ATARI_VIDEO_PAL ? ARTIFACT_PAL_BLEND : ARTIFACT_NONE;
#else
		return ARTIFACT_NONE;
#endif
	case ATARI_ARTIFACT_NONE:
	default:
		return ARTIFACT_NONE;
	}
}

/* Applies the machine personality that XE/XEGS machines add on top of an XL. */
static void ApplyMachinePersonality(atari_machine_t machine)
{
	/*
	 * BASIC is present and switched in.
	 *
	 * Two separate flags, and both matter:
	 *
	 *   Atari800_builtin_basic  the machine has a BASIC ROM at all.
	 *   Atari800_disable_basic  the user wants it bypassed, as though Option
	 *                           were held down during a cold start.
	 *
	 * The second one defaults to TRUE in the core's own source
	 * (atari.c: Atari800_disable_basic = TRUE) because the standalone emulator
	 * only ever changes it from the command line with -basic or -nobasic.  This
	 * project passes no command line, so the default stood -- and the effect was
	 * that EVERY cold start simulated Option being held.
	 *
	 * The symptom was specific and is worth recording, because it looks like a
	 * hardware fault rather than a setting: a cold reset went to the built-in
	 * SELF TEST, while a warm reset went to BASIC.  Only a cold start arms
	 * GTIA_consol_override (in Atari800_Coldstart), so only a cold start let the
	 * stale flag clear the Option bit and suppress BASIC.  The machine was
	 * faithfully reporting "Option held" because it had been told to.
	 *
	 * A real XL/XE with BASIC fitted and nothing held boots to READY, so the flag
	 * is cleared here.  Option still works normally through the keyboard: the
	 * input layer sets the console bit directly, which is a different path.
	 */
	Atari800_builtin_basic = TRUE;
	Atari800_disable_basic = FALSE;

	Atari800_keyboard_leds = FALSE;
	Atari800_f_keys = FALSE;
	Atari800_builtin_game = FALSE;
	Atari800_keyboard_detached = FALSE;
	Atari800_jumper = FALSE;

	switch (machine) {
	case ATARI_MACHINE_XE:
		Atari800_keyboard_leds = TRUE;
		break;
	case ATARI_MACHINE_XEGS:
		Atari800_keyboard_leds = TRUE;
		Atari800_builtin_game = TRUE;
		break;
	case ATARI_MACHINE_XL:
	default:
		/* 800XL: four function keys, no keyboard LEDs. */
		Atari800_f_keys = TRUE;
		break;
	}
}

/*
 * Requests a sound configuration from the core and starts output.
 *
 * The core's Sound_Setup() calls back into PLATFORM_SoundSetup() (implemented
 * in sound_bridge.c), which owns the real audio device.
 */
static void ApplySoundConfiguration(void)
{
	if (!s_config.sound_enabled) {
		if (Sound_enabled) {
			Sound_Exit();
		}
		return;
	}

	Sound_desired.freq = s_config.sound_sample_rate;
	Sound_desired.sample_size = 2;	/* 16-bit signed */
	Sound_desired.channels = s_config.sound_stereo ? 2 : 1;
	Sound_desired.buffer_ms = 20;

	/*
	 * A zero sample rate hangs the audio device.
	 *
	 * The settings struct can carry 0 here -- it arrives that way from the menu,
	 * whose copy of the settings is not guaranteed to have the rate filled in.
	 * Opening a sink at 0 Hz does not fail cleanly: it divides the clock by the
	 * rate and the machine stops, with no exception and no log line after this
	 * one.  That is exactly what used to freeze the menu on *any* setting
	 * change, because every change funnels through here.
	 *
	 * Clamping to the rate this port uses everywhere else turns a hang into a
	 * working machine, and the log records that it happened.
	 */
	if (Sound_desired.freq < 8000 || Sound_desired.freq > 192000) {
		ATARI_RuntimeLog("sound: sample rate %d Hz is not usable, using %d Hz",
				 Sound_desired.freq, SOUND_RATE_FALLBACK_HZ);
		Sound_desired.freq = SOUND_RATE_FALLBACK_HZ;
	}

	/*
	 * Restarting the audio device hangs.
	 *
	 * The sink is opened once at boot, and every settings change used to tear it
	 * down and start it again through Sound_Setup().  On this hardware that
	 * second start never returns -- the whole machine stops inside it, with no
	 * exception and with the timer heartbeat dead, which is why changing any
	 * setting froze the menu.
	 *
	 * Until the HDMI path is known to restart cleanly, an already-open sink is
	 * left alone and only the volume is updated.  The cost is honest and small:
	 * a change to the stereo setting does not take effect until the machine is
	 * restarted, and it is logged when that happens.
	 */
	if (Sound_enabled) {
		/*
		 * Compare against what the device actually runs with, not against a
		 * private note of what we think we opened.
		 */
		if (Sound_desired.channels != (int) Sound_out.channels) {
			ATARI_RuntimeLog("sound: stereo change deferred until restart "
					 "(%u -> %u channels)",
					 (unsigned) Sound_out.channels,
					 (unsigned) Sound_desired.channels);
		}

		POKEYSND_SetVolume(s_config.sound_volume);
		return;
	}

	ATARI_RuntimeLog("sound: opening the %d-channel sink at %d Hz",
			 Sound_desired.channels, Sound_desired.freq);

	if (Sound_Setup()) {
		POKEYSND_SetVolume(s_config.sound_volume);
		Sound_Continue();

		ATARI_RuntimeLog("sound: sink open, %u channel(s) at %u Hz",
				 (unsigned) Sound_out.channels,
				 (unsigned) Sound_out.freq);
	}
	else {
		ATARI_RuntimeLog("sound: sink FAILED to open");
		SetError("the audio device could not be opened");
	}
}


/* ===========================================================================
 * Cassette image types, and the SIO mode each one needs
 *
 * A CAS image is a sequence of 8-byte block headers, each followed by its data.
 * "data" blocks hold records that the patched SIO copies straight into memory;
 * "fsk " blocks hold raw tape signal, which only the timed path can read.  A
 * turbo tape therefore needs ESC_enable_sio_patch off, and a standard one is far
 * faster with it on.  The two are mutually exclusive in the core, and the mode
 * is fixed when the machine is built -- the OS ROM is patched then -- so
 * changing it takes a power cycle.
 * ======================================================================== */

typedef enum {
	CASSETTE_TYPE_UNKNOWN = 0,
	CASSETTE_TYPE_STANDARD,
	CASSETTE_TYPE_TURBO
} cassette_type_t;

static cassette_type_t s_cassette_type = CASSETTE_TYPE_UNKNOWN;
static int s_bCassetteMounted = 0;	/* the type above belongs to a mounted tape */
static int s_bRealTimingApplied = 0;	/* what the running machine was built with */

static int BlockIs(const unsigned char *pHeader, const char *pIdentifier)
{
	return pHeader[0] == (unsigned char) pIdentifier[0]
	    && pHeader[1] == (unsigned char) pIdentifier[1]
	    && pHeader[2] == (unsigned char) pIdentifier[2]
	    && pHeader[3] == (unsigned char) pIdentifier[3];
}

/*
 * Works out whether a CAS image needs the timed tape path.
 *
 * Not "any fsk block": a standard tape often carries its leader as one, and the
 * patched path skips fsk blocks harmlessly.  What makes a tape turbo is fsk
 * payload -- fsk blocks with no data blocks at all, or fsk blocks after the data
 * has begun.
 */
static cassette_type_t ClassifyCassette(const char *path)
{
	FILE *pFile;
	unsigned char Header[8];
	int bSawData = 0;
	int bSawFsk = 0;
	int bFskAfterData = 0;
	unsigned nBlocks = 0;

	pFile = ATARI_VFS_Open(path, "rb");
	if (pFile == 0) {
		return CASSETTE_TYPE_UNKNOWN;
	}

	if (ATARI_VFS_Read(Header, 1, sizeof Header, pFile) != sizeof Header
	    || !BlockIs(Header, "FUJI")) {
		ATARI_VFS_Close(pFile);
		return CASSETTE_TYPE_UNKNOWN;	/* not a CAS image */
	}

	/* The description follows the header, of the length the header carries. */
	if (ATARI_VFS_Seek(pFile, (long) (Header[4] | (Header[5] << 8)), SEEK_CUR) != 0) {
		ATARI_VFS_Close(pFile);
		return CASSETTE_TYPE_UNKNOWN;
	}

	for (;;) {
		unsigned nLength;

		/* A corrupt image must not spin here; no real tape has this many. */
		if (++nBlocks > 4096) {
			break;
		}
		if (ATARI_VFS_Read(Header, 1, sizeof Header, pFile) != sizeof Header) {
			break;
		}

		nLength = Header[4] | (Header[5] << 8);

		if (BlockIs(Header, "fsk ")) {
			bSawFsk = 1;
			if (bSawData) {
				bFskAfterData = 1;
			}
		}
		else if (BlockIs(Header, "data")) {
			bSawData = 1;
		}

		/* "baud" blocks are skipped too: their payload is setting only. */
		if (ATARI_VFS_Seek(pFile, (long) nLength, SEEK_CUR) != 0) {
			break;
		}
	}

	ATARI_VFS_Close(pFile);

	if (!bSawFsk) {
		return CASSETTE_TYPE_STANDARD;
	}

	return (!bSawData || bFskAfterData) ? CASSETTE_TYPE_TURBO
					   : CASSETTE_TYPE_STANDARD;
}

int ATARI_ClassifyCassette(const char *path)
{
	s_cassette_type = (path != 0 && path[0] != '\0')
			? ClassifyCassette(path) : CASSETTE_TYPE_UNKNOWN;

	if (s_cassette_type == CASSETTE_TYPE_TURBO) {
		ATARI_RuntimeLog("cassette: turbo tape (%s)", path);
	}
	else if (s_cassette_type == CASSETTE_TYPE_STANDARD) {
		ATARI_RuntimeLog("cassette: standard tape (%s)", path);
	}

	return (int) s_cassette_type;
}

int ATARI_CassetteNeedsRebuild(void)
{
	if (!s_bCassetteMounted) {
		return 0;		/* no tape: nothing to ask for */
	}

	return (s_cassette_type == CASSETTE_TYPE_TURBO) != s_bRealTimingApplied;
}

const char *ATARI_CassetteModeMessage(void)
{
	if (!ATARI_CassetteNeedsRebuild()) {
		return 0;
	}

	/*
	 * Plain ASCII: the message is drawn in the Atari character set, which has
	 * no em dash.
	 */
	return s_cassette_type == CASSETTE_TYPE_TURBO
	       ? "Turbo tape - power cycle required"
	       : "Standard tape - power cycle required";
}

/* ===========================================================================
 * Bring-up
 * ======================================================================== */

/*
 * Declared here as well as at its definition further down, because ATARI_Init()
 * has to clear it: mounting a cartridge during bring-up sets it, and the boot
 * cold start has already done the work by then.  Repeated tentative definitions
 * of a file-scope static are allowed in C.
 */
static int s_bColdStartPending;

int ATARI_Init(const atari_config_t *cfg)
{
	int argc;
	int loaded_roms;

	if (s_initialised) {
		ATARI_Exit();
	}

	if (cfg == 0) {
		ATARI_DefaultConfig(&s_config);
	}
	else {
		s_config = *cfg;
	}
	if (s_config.rom_directory == 0) {
		s_config.rom_directory = "SD:/ROMS";
	}

	/*
	 * A missing sample rate is not "unspecified", it is fatal.
	 *
	 * The settings have no field for it, so a caller that fills atari_config_t
	 * from them leaves this at zero.  Opening a sink at 0 Hz divides the audio
	 * clock by the rate and stops the machine -- no exception, no log line after
	 * it -- which is what used to freeze the menu whenever a setting changed.
	 * Validate here, once, so no caller can poison it.
	 */
	if (s_config.sound_sample_rate < 8000 || s_config.sound_sample_rate > 192000) {
		s_config.sound_sample_rate = SOUND_RATE_FALLBACK_HZ;
	}

	s_last_error[0] = '\0';
	s_frame_error = 0;

	/*
	 * Load the OS ROM images first: the core picks an OS revision when the
	 * machine is configured, so the candidates have to be known already.
	 * Without an OS ROM the machine cannot boot, and AltirraOS (compiled into
	 * the core) is used as a fallback rather than failing outright.
	 */
	loaded_roms = LoadRomImages(s_config.rom_directory);
	s_rom_files_loaded = loaded_roms;
	if (loaded_roms == 0) {
		Log_print("No OS ROM images found in %s; using the built-in AltirraOS",
		          s_config.rom_directory);
	}
	/*
	 * Stop the core from scanning for ROM images itself.  It would otherwise
	 * need the POSIX directory API, and this project already knows exactly
	 * which files it has.
	 */
	SYSROM_SetDefaults();

	/*
	 * Configure the emulated machine before the core initialises it.  These
	 * are the core's public configuration globals; the library wrapper would
	 * otherwise expose them only as command line options.
	 */
	Atari800_SetMachineType(CoreMachineType(s_config.machine));
	ApplyMachinePersonality(s_config.machine);
	MEMORY_ram_size = CoreRamSize(s_config.ram_kb);
	Atari800_tv_mode = CoreTvMode(s_config.video);
	Atari800_refresh_rate = 1;
	Atari800_auto_frameskip = FALSE;
	Atari800_turbo = FALSE;

	/*
	 * The menu's per-disk [ro] setting is the one that decides, so the image's
	 * own write-protect flag is not consulted -- see the declaration above.
	 * Done before the core starts, because a snapshot restore remounts the
	 * disks from inside the core, where ATARI_MountDisk() is not involved.
	 */
	ignore_header_writeprotect = TRUE;

	/*
	 * Keep the emulated screen pristine: the core can draw a disk activity
	 * LED and a speed meter directly into the framebuffer, which this project
	 * does not want.
	 */
	Screen_show_disk_led = FALSE;
	Screen_show_sector_counter = FALSE;
	Screen_show_atari_speed = FALSE;
	Screen_show_1200_leds = FALSE;

	/*
	 * The multimedia statistics overlay only exists when the core is built
	 * with audio or video recording, which this project is not, so the
	 * variable is not compiled in at all and must not be assigned.
	 */
#if defined(AUDIO_RECORDING) || defined(VIDEO_RECORDING)
	Screen_show_multimedia_stats = FALSE;
#endif

	/*
	 * The tape type decides whether the OS gets patched for fast SIO.
	 *
	 * The two are mutually exclusive in the core: with the patch installed, the
	 * patched SIO copies tape records straight into memory and cannot read fsk
	 * blocks; with it removed, POKEY's timed path advances the tape and can read
	 * anything, but every load runs at tape speed.  The OS ROM is patched when
	 * the machine is built and ESC_Remove() only clears the dispatcher without
	 * restoring the bytes, so this can only be chosen here, before the build --
	 * which is why changing it takes a power cycle.
	 */
	s_bRealTimingApplied = (s_cassette_type == CASSETTE_TYPE_TURBO);
	ESC_enable_sio_patch = s_bRealTimingApplied ? FALSE : TRUE;

	ATARI_RuntimeLog("cassette: %s; SIO patch %s",
	                 s_cassette_type == CASSETTE_TYPE_TURBO ? "turbo tape"
			 : "standard tape (or none)",
			 s_bRealTimingApplied ? "off, real timing"
					      : "on, fast load");

	/*
	 * Atari800_Initialise() runs the whole start-up sequence in the core's
	 * documented order: it configures the custom chips, initialises the
	 * platform layer and the sound device, then builds the machine and loads
	 * the ROM images into it.  It expects an argv whose first entry is a
	 * program name; there are no options to pass because the machine has
	 * already been configured above.
	 */
	{
		char *argv[2];
		argv[0] = (char *)"atari-pi";
		argv[1] = 0;
		argc = 1;

		if (!Atari800_Initialise(&argc, argv)) {
			SetError("the emulation core failed to start up");
			return 0;
		}
	}

	if (Atari800_os_version < 0) {
		SetError("no usable OS ROM: put ATARIXL.ROM in %s on the SD card",
		         s_config.rom_directory);
		return 0;
	}

	ApplySoundConfiguration();
	ATARI_SetArtifact(s_config.artifact);

	/*
	 * The machine's identity, and the two flags that decide whether a cold boot
	 * reaches BASIC or falls through to the built-in diagnostics.
	 *
	 * Worth a line in the log because the two machines look identical from the
	 * outside until the screen appears, and because "BASIC bypassed" is exactly
	 * what a phantom Option press looks like.
	 */
	Log_print("Machine: type %d, %d kB, BASIC rom %s, BASIC bypassed %s",
	          Atari800_machine_type, MEMORY_ram_size,
	          MEMORY_have_basic ? "present" : "MISSING",
	          Atari800_disable_basic ? "YES" : "no");

	/*
	 * ANCHOR: the core is up and the machine is about to be reset.  Anything
	 * that goes wrong from here is a runtime fault and belongs in the runtime
	 * log, so this line marks the boundary between the two logs.
	 */
	ATARI_RuntimeLog("core initialised: machine %d, %d kB, OS %d, BASIC %s",
	                 Atari800_machine_type, MEMORY_ram_size,
	                 Atari800_os_version,
	                 MEMORY_have_basic ? "present" : "MISSING");

	/*
	 * Re-assert the machine configuration now that the core is up.
	 *
	 * The machine type and RAM size are written before the core initialises it,
	 * which is where they need to be for the core's own setup to see them.  But
	 * that is exactly the shape of the video-standard bug: a value assigned
	 * during configuration can be superseded by the core's initialisation, and
	 * the only symptom is a machine that quietly differs from the settings.
	 *
	 * Setting them again here, immediately before the first cold start, costs
	 * nothing and guarantees that the machine which boots is the one the
	 * settings describe.  The line below records both the requested and the
	 * effective values, so a mismatch would be visible rather than inferred.
	 */
	Atari800_SetMachineType(CoreMachineType(s_config.machine));
	ApplyMachinePersonality(s_config.machine);
	MEMORY_ram_size = CoreRamSize(s_config.ram_kb);

	ATARI_RuntimeLog("machine configured: type %d, %d kB RAM "
			 "(settings asked for type %d, %d kB)",
			 (int) Atari800_machine_type, (int) MEMORY_ram_size,
			 (int) s_config.machine, (int) s_config.ram_kb);

	/*
	 * The remaining core settings, in the same requested-versus-effective shape.
	 *
	 * The video standard is the one that caused trouble silently: the palette is
	 * only refreshed by a call that does nothing when the mode already matches,
	 * so a wrong standard showed up as wrong colours with nothing in the log to
	 * say what the emulator believed it was doing.
	 */
	ATARI_RuntimeLog("video configured: %s, artifact mode %d "
			 "(settings asked for %s, artifact mode %d)",
			 Atari800_tv_mode == Atari800_TV_PAL ? "PAL" : "NTSC",
			 (int) s_config.artifact,
			 s_config.video == ATARI_VIDEO_PAL ? "PAL" : "NTSC",
			 (int) s_config.artifact);

	ATARI_RuntimeLog("sound configured: %s, volume %d, %s, %d Hz",
			 s_config.sound_enabled ? "on" : "off",
			 (int) s_config.sound_volume,
			 s_config.sound_stereo ? "stereo" : "mono",
			 (int) s_config.sound_sample_rate);

	/*
	 * Refresh the palette for the configured video standard.
	 *
	 * The mode was assigned directly during configuration (Atari800_tv_mode
	 * above), which tells the core which standard to emulate but does not touch
	 * the palette: only Atari800_SetTVMode() does that, and it does nothing when
	 * the mode it is given already matches.  So a machine configured as NTSC came
	 * up still showing PAL colours, and the menu -- which reads the setting, not
	 * the palette -- disagreed with the picture.
	 *
	 * Calling these directly is the only way to force the refresh, precisely
	 * because the public entry point short-circuits on equality.
	 */
	Colours_SetVideoSystem(Atari800_tv_mode);
	ARTIFACT_SetTVMode(Atari800_tv_mode);

	s_initialised = 1;
	ATARI_ColdReset();

	/*
	 * The boot cold start has happened, so any reset requested while the machine
	 * was being brought up is now redundant.
	 *
	 * Mounting the cartridge from the settings happens during initialisation, and
	 * ATARI_MountCartridge() always asks for a deferred cold start -- which is
	 * correct when a user inserts a cartridge at runtime, but not here, because
	 * the cold reset above already covers it.  Leaving the flag set meant the
	 * request stayed pending until the first time the menu was closed, and the
	 * machine then reset itself: boot with a cartridge in settings, open the OSD,
	 * close it without touching anything, and the cartridge restarted from the
	 * top.
	 */
	s_bColdStartPending = 0;

	return 1;
}

void ATARI_Exit(void)
{
	if (!s_initialised) {
		return;
	}

	Sound_Exit();
	Atari800_Exit(FALSE);
	s_initialised = 0;
}

/* ===========================================================================
 * Frame loop
 * ======================================================================== */

uint32_t ATARI_FrameNumber(void)
{
	return (uint32_t)Atari800_nframes;
}

double ATARI_Fps(void)
{
	return Atari800_tv_mode == Atari800_TV_PAL ? Atari800_FPS_PAL : Atari800_FPS_NTSC;
}

/* Microseconds between frames, held as a 32.32 fixed point value. */
static uint64_t FramePeriodFixed(void)
{
	double fps = ATARI_Fps();

	if (fps <= 0.0) {
		return (uint64_t)20000 << 16;
	}
	return (uint64_t)(1000000.0 / fps * 65536.0 + 0.5);
}

int ATARI_TakeStateSnapshot(atari_state_snapshot_t *snapshot)
{
	if (snapshot == 0 || !s_state_snapshot.pending) {
		return 0;
	}

	*snapshot = s_state_snapshot;
	s_state_snapshot.pending = 0;

	return 1;
}

int ATARI_LastErrorIsCpuCrash(void)
{
	return s_last_error_is_cpu_crash;
}

/* ===========================================================================
 * Runtime log
 * ======================================================================== */

static atari_runtime_log_fn s_runtime_log_fn;

void ATARI_SetRuntimeLog(atari_runtime_log_fn fn)
{
	s_runtime_log_fn = fn;
}

void ATARI_RuntimeLog(const char *format, ...)
{
	char Buffer[192];
	va_list args;

	if (s_runtime_log_fn == 0 || format == 0) {
		return;
	}

	va_start(args, format);
	vsnprintf(Buffer, sizeof Buffer, format, args);
	va_end(args);

	s_runtime_log_fn(Buffer);
}

int ATARI_RunFrame(void)
{
	/*
	 * Entry marker.
	 *
	 * Together with the exit marker below and the begin/end pair around
	 * Atari800_Frame(), this splits a frame into four regions, which is what
	 * localises a hang that is NOT inside the emulation: the last marker in the
	 * log says which region stopped.
	 *
	 *   pre   -> PushKeyboardState and setup
	 *   begin -> Atari800_Frame()
	 *   end   -> frame checks and pacing
	 *   post  -> the caller's drawing, then the next pre
	 */
	/*
	 * Per-frame tracing removed on purpose.
	 *
	 * "pre", "kbd enter/done", "begin", "end" and "post" did their job: they
	 * localised the fault to a window and ruled the keyboard out.  What is left
	 * is a log of several thousand lines that a person cannot read, competing
	 * for the card with the disk trace, which is the thing now being examined.
	 *
	 * Only two markers remain, every tenth frame, and they bracket the one step
	 * that has not been cleared.
	 */

	if (!s_initialised) {
		return 0;
	}

	/*
	 * Keyboard bracketing removed.
	 *
	 * It did its job: both lines printed on every frame, including the one that
	 * died, which proved the callback was not the fault.  Keeping it now only
	 * adds two lines per frame to a log whose whole purpose is to be read by a
	 * human.
	 */
	PushKeyboardState();


	s_frame_error = 0;

	/*
	 * Frames emulated since the machine last started or was reset.
	 *
	 * Used to tell "ANTIC has not been set up yet" from "ANTIC has been set up
	 * and is now broken".  Counted here rather than read from the core so that
	 * it follows this project's own resets.
	 */
	s_frames_since_reset++;

	/*
	 * Atari800_Frame() emulates exactly one frame and leaves the finished
	 * screen in Screen_atari.  It also handles the special key codes that mean
	 * "cold start" or "warm start" by acting on them.
	 *
	 * A frame is emulated inside a setjmp guard because the CPU core abandons
	 * a frame by longjmp if the emulated machine hits an instruction it cannot
	 * execute.  Without this the jump would land nowhere and take the kernel
	 * with it; with it, a broken Atari program costs one frame and is reported
	 * through ATARI_LastError().
	 */
#ifdef HAVE_SETJMP
	/*
	 * The only step between the keyboard read and the emulation that has not
	 * been cleared.  "enter" with no matching "returned" puts the fault here.
	 */
	s_frame_error = setjmp(libatari800_cpu_crash);
	if (s_frame_error == 0) {
		 
		Atari800_Frame();

		/*
		 * One-shot state report, written to the boot log.
		 *
		 * A machine that never produces a picture looks identical whatever the
		 * cause, and these are the values that tell the cases apart.  It goes to
		 * the log file on the SD card so that it can be read without a serial
		 * cable or a photograph.
		 *
		 * Recorded once per machine start and then left alone: sampling it
		 * repeatedly was useful while this fault was being found, but a screen
		 * full of repeated output is not worth having during normal running.
		 */
		if (!s_state_snapshot.taken) {
			s_state_snapshot.os_version = Atari800_os_version;
			s_state_snapshot.machine_type = Atari800_machine_type;
			s_state_snapshot.ram_kb = MEMORY_ram_size;
			s_state_snapshot.antic_dlist = (unsigned) ANTIC_dlist;
			s_state_snapshot.antic_dmactl = (unsigned) ANTIC_DMACTL;
			s_state_snapshot.antic_scan = (unsigned) (ANTIC_xpos / 114);
			s_state_snapshot.antic_mode = (unsigned) ANTIC_xpos;
			s_state_snapshot.cpu_pc = (unsigned) CPU_regPC;
			s_state_snapshot.cpu_a = (unsigned) CPU_regA;
			s_state_snapshot.portb = (unsigned) PIA_PORTB;
			s_state_snapshot.frames = (unsigned) Atari800_nframes;
			s_state_snapshot.rom_files = (unsigned) s_rom_files_loaded;
			s_state_snapshot.has_basic = MEMORY_have_basic ? 1 : 0;
			s_state_snapshot.pending = 1;
			s_state_snapshot.taken = 1;
		}

		if (CPU_cim_encountered) {
			s_frame_error = LIBATARI800_CPU_CRASH;
		}
		else if (ANTIC_dlist == 0) {
			/*
			 * ANTIC has no display list *at this instant*.
			 *
			 * This is NOT by itself a fault, and treating it as one was wrong
			 * twice over.  ANTIC_dlist is zero from reset until the OS enables
			 * display DMA, so it is zero for the first frames of every start-up;
			 * and later the OS legitimately clears it whenever it switches the
			 * display off -- while loading from disk, and around SELF TEST,
			 * which is why this fired *after* the machine was plainly working.
			 * Reporting it produced a looping error message drawn over a working
			 * Atari screen, which looked like a failure and hid the real one.
			 *
			 * Only a display that stays off is worth reporting.  The count of
			 * consecutive blank frames is kept, and the error is raised once that
			 * becomes long enough to be a fault rather than a screen blank.
			 */
			s_blank_frames++;
			if (s_blank_frames > BLANK_FRAMES_BEFORE_ERROR) {
				s_frame_error = LIBATARI800_DLIST_ERROR;
			}
		}
		else {
			s_blank_frames = 0;
		}
	}

	if (s_frame_error != 0) {
		/*
		 * Report through the core's own error interface, so that the
		 * description shown to the user is the core's rather than a
		 * paraphrase of it here.
		 */
		libatari800_error_code = s_frame_error;
		SetError("%s", libatari800_error_message());
		ATARI_RuntimeLog("Frame error: %s", libatari800_error_message());
		s_last_error_is_cpu_crash = (s_frame_error == LIBATARI800_CPU_CRASH);
		libatari800_error_code = 0;
	}
	else {
		s_last_error_is_cpu_crash = 0;
	}
#else
	Atari800_Frame();
#endif

	/*
	 * Pace the emulation to the real frame rate.
	 *
	 * The emulated machine has to run at the speed of the real thing, so a frame
	 * is due every 1/fps of a second and the emulator waits for the next one.
	 * Anything slower than that means the host is not keeping up, which is felt
	 * as sluggish input rather than as a low frame count.
	 *
	 * The period is not a whole number of microseconds (a PAL frame is 20055.9
	 * us), so the remainder is carried in s_frame_phase rather than being
	 * truncated, which keeps the emulated machine in step with a real one over
	 * long runs.
	 */
	if (s_host_clock != 0 && s_host_idle != 0) {
		uint64_t now = s_host_clock(s_host_context);

		if (s_next_frame_time == 0) {
			s_next_frame_time = now;
			s_frame_phase = 0;

			/*
			 * Start the rate measurement when pacing starts, so that the
			 * reported figure covers emulation and not start-up.
			 */
			s_rate_start_us = now;
			s_rate_start_frames = Atari800_nframes;
		}

		s_frame_phase += FramePeriodFixed();
		s_next_frame_time += s_frame_phase >> 16;
		s_frame_phase &= 0xffff;

		if (now < s_next_frame_time) {
			s_host_idle(s_host_context, (uint32_t)(s_next_frame_time - now));
		}
		else if (s_next_frame_time < now) {
			/*
			 * Late: drop the debt instead of catching up.
			 *
			 * The emulator used to repay lateness by running the next frames
			 * back to back, and only gave up once it was 200 ms behind.  That is
			 * wrong for audio, because each of those catch-up frames produces a
			 * full frame of sound -- 882 samples, 20 ms -- in no wall-clock time
			 * at all.  A menu redraw costs 25-30 ms against a 20 ms frame, so
			 * opening the OSD built up a small debt, closing it repaid that debt
			 * as a burst, and the burst filled the output queue.  Production then
			 * matched consumption again and the queue never drained, so the
			 * delay was permanent -- which is exactly what was heard: no latency
			 * until the OSD had been used, and a clear delay afterwards.
			 *
			 * Resynchronising to now lets the machine run a fraction of a frame
			 * slow, which for an emulator is invisible, instead of generating
			 * audio faster than the speaker can play it.
			 */
			s_next_frame_time = now;
			s_frame_phase = 0;
		}

		/*
		 * The emulated machine's health, once every fifteen seconds.
		 *
		 * The achieved frame rate is the one number that says whether the host
		 * is keeping up, and it cannot be inferred from anything else in the
		 * log.  It also doubles as a heartbeat: if the file stops growing, the
		 * machine stopped there.
		 *
		 * Deliberately terse and rate limited.  The log has to stay readable
		 * and the card has to stay mostly idle -- a line every frame would wear
		 * out the card and bury the one line that matters.
		 */
		if (s_rate_start_us != 0
		    && now - s_rate_start_us >= 15000000ULL)
		{
			unsigned frames = (unsigned)(Atari800_nframes - s_rate_start_frames);
			unsigned long long elapsed_us = now - s_rate_start_us;
			unsigned fps10 = (unsigned)((frames * 10000000ULL)
						    / (elapsed_us ? elapsed_us : 1));
			unsigned target = (unsigned)(ATARI_Fps() + 0.5);
			unsigned long reads = 0;
			unsigned long long bytes = 0;
			unsigned long errors = 0;

			ATARI_VFS_GetStats(&reads, &bytes, &errors);

			ATARI_RuntimeLog("stats: %u.%u fps (%u target), %u frames, "
					 "PC 0x%04x, DMACTL 0x%02x, dlist 0x%04x, "
					 "disk reads %lu (%lu kB, %lu errors)",
					 fps10 / 10, fps10 % 10, target,
					 frames,
					 (unsigned)CPU_regPC,
					 (unsigned)ANTIC_DMACTL,
					 (unsigned)ANTIC_dlist,
					 reads,
					 (unsigned long)(bytes / 1024),
					 errors);

			/* Restart the measurement so each line covers its own window. */
			s_rate_start_us = now;
			s_rate_start_frames = Atari800_nframes;
		}

		/*
		 * Boot watchdog: a short line every five seconds for the first minute
		 * after a cold reset.
		 *
		 * The host can be perfectly healthy while the emulated machine never
		 * boots -- frames keep completing, nothing is written, and the screen
		 * simply sits there.  What distinguishes that is the disk read count: if
		 * it does not advance while the frame count does, the machine is not
		 * talking to the disk at all, and the question becomes why it never
		 * tries rather than why a transfer is stuck.
		 */
		if (s_boot_log_armed)
		{
			s_boot_log_armed = 0;
			s_boot_log_start_us = now;
			s_boot_log_count = 0;
		}

		if (s_boot_log_start_us != 0
		    && s_boot_log_count < BOOT_LOG_MAX
		    && now - s_boot_log_start_us
		       >= (uint64_t) (s_boot_log_count + 1) * BOOT_LOG_INTERVAL_US)
		{
			unsigned long reads = 0;
			unsigned long long bytes = 0;
			unsigned long errors = 0;

			ATARI_VFS_GetStats(&reads, &bytes, &errors);

			s_boot_log_count++;

			ATARI_RuntimeLog("boot t=%us: frames %u, PC 0x%04x, "
					 "DMACTL 0x%02x, dlist 0x%04x, "
					 "PORTB 0x%02x, reads %lu, errors %lu",
					 s_boot_log_count
					 * (unsigned) (BOOT_LOG_INTERVAL_US / 1000000ULL),
					 (unsigned) Atari800_nframes,
					 (unsigned) CPU_regPC,
					 (unsigned) ANTIC_DMACTL,
					 (unsigned) ANTIC_dlist,
					 (unsigned) PIA_PORTB,
					 reads, errors);
		}
	}

	return s_frame_error ? 0 : 1;
}

/* ===========================================================================
 * Emulated screen and audio
 * ======================================================================== */

const uint8_t *ATARI_ScreenIndices(void)
{
	return (const uint8_t *)Screen_atari;
}

const uint32_t *ATARI_Palette(void)
{
	return (const uint32_t *)Colours_table;
}

const uint8_t *ATARI_AudioBuffer(void)
{
	extern UBYTE *LIBATARI800_Sound_array;

	if (sound_array_fill == 0) {
		return 0;
	}
	return (const uint8_t *)LIBATARI800_Sound_array;
}

unsigned ATARI_AudioBytes(void)
{
	return sound_array_fill;
}

unsigned ATARI_SoundSampleRate(void)
{
	unsigned rate = Sound_out.freq;
	return rate != 0 ? rate : 44100;
}

unsigned ATARI_SoundChannels(void)
{
	unsigned channels = Sound_out.channels;
	return channels != 0 ? channels : 1;
}

/* ===========================================================================
 * Machine control
 * ======================================================================== */

int ATARI_PowerCycle(const atari_config_t *cfg)
{
	if (!s_initialised) {
		SetError("the emulator is not running");
		return 0;
	}

	/*
	 * A power cycle is a new machine, not a reset.
	 *
	 * Atari800_Coldstart() is the CPU's RESET line: it resets PIA, ANTIC and the
	 * CPU, and the core says in so many words that POKEY and GTIA have no reset
	 * pin.  GTIA's console-key mask is therefore one of the things that
	 * survives -- software that writes $D01F can mask a console key out, and a
	 * masked key reads as *pressed*, so the OS boots as though Option were held:
	 * no BASIC, and BOOT ERROR when there is no disk to boot from.
	 *
	 * So the core is torn down and built again from CFG.  Atari800_Exit() and
	 * not ATARI_Exit(): the latter also closes the audio device, and reopening
	 * the HDMI sink hangs this hardware.  PLATFORM_SoundSetup() reuses the open
	 * device when the stream shape is unchanged, which it is here.
	 */
	ATARI_RuntimeLog("power cycle: tearing down");

	Atari800_Exit(FALSE);
	s_initialised = 0;

	/*
	 * Zero the 64K the machine sees.  RAM survives a reset -- which is what
	 * "some software partly survives a cold reset" looks like from the outside
	 * -- and at power-on it holds nothing.  The 130XE's extra banks belong to
	 * the core and are not reachable from here; they are cleared when the RAM
	 * size changes.
	 */
	memset(MEMORY_mem, 0, 65536);

	if (!ATARI_Init(cfg)) {
		SetError("the emulator could not be restarted: %s", ATARI_LastError());
		return 0;
	}

	ATARI_RuntimeLog("power cycle: done, PC 0x%04x", (unsigned)CPU_regPC);

	return 1;
}

void ATARI_ColdReset(void)
{
	if (!s_initialised) {
		return;
	}

	/*
	 * Logged on both sides: a cold start runs the emulated OS from its reset
	 * vector, so if that is where a hang happens, "starting" with no "done" is
	 * the proof, and it distinguishes a hang in the reset path from one in the
	 * frame loop that follows.
	 */
	ATARI_RuntimeLog("cold reset: starting");
	Atari800_Coldstart();
	ATARI_RuntimeLog("cold reset: done, PC 0x%04x", (unsigned)CPU_regPC);

	/*
	 * Watch the reads the machine makes while booting, so a boot that stalls
	 * shows which sector it last asked for.
	 */
	ATARI_VFS_BeginLoadTrace();

	/*
	 * Restart the frame trace.
	 *
	 * The budget is per boot, not per session.  It used to be spent once and
	 * never replenished, so after the first boot no frame lines were written at
	 * all -- which made a machine that stopped after a reset look as though
	 * nothing had happened at any point.  That was my instrumentation hiding the
	 * very failure it was added to find.
	 */
	s_diag_frame_logs = 0;

	/* Arm the boot watchdog; it starts timing on the next frame. */
	s_boot_log_armed = 1;

	s_next_frame_time = 0;
	/* The restarted machine gets the same grace period as a cold boot. */
	s_frames_since_reset = 0;
	s_blank_frames = 0;
}

void ATARI_WarmReset(void)
{
	if (!s_initialised) {
		return;
	}
	Atari800_Warmstart();
	s_frames_since_reset = 0;
	s_blank_frames = 0;
}

void ATARI_SetVideoStandard(atari_video_standard_t video)
{
	if (video == s_config.video) {
		return;
	}
	s_config.video = video;

	if (!s_initialised) {
		return;
	}

	/*
	 * Atari800_SetTVMode() refreshes the palette and re-runs the artifacting
	 * setup.  It also re-initialises POKEY's sound generator with timing
	 * constants for the new standard, but it does not reopen the audio device,
	 * so the sound configuration has to be re-applied afterwards.  The frame
	 * pacing changes too, so restart it.
	 */
	Atari800_SetTVMode(CoreTvMode(video));

	/*
	 * Re-apply the sound configuration through the one guarded path.
	 *
	 * This used to call Sound_Setup() directly, which re-opened the audio device
	 * even when it was already running -- and did so without the "already open"
	 * check that ApplySoundConfiguration() performs.  That is what produced two
	 * sink-open attempts at startup, and almost certainly what makes the first
	 * video-standard switch feel slow: it is reopening a device that never
	 * needed reopening.
	 */
	if (s_config.sound_enabled) {
		ApplySoundConfiguration();
	}

	ATARI_SetArtifact(s_config.artifact);
	s_next_frame_time = 0;
	s_frame_phase = 0;
}

void ATARI_SetArtifact(atari_artifact_t artifact)
{
	ARTIFACT_Set(CoreArtifact(s_config.video, artifact));
}

void ATARI_SetVolume(int volume_0_100)
{
	if (volume_0_100 < 0) {
		volume_0_100 = 0;
	}
	if (volume_0_100 > 100) {
		volume_0_100 = 100;
	}
	s_config.sound_volume = volume_0_100;

	if (!s_initialised) {
		return;
	}
	POKEYSND_SetVolume(s_config.sound_volume);

	/*
	 * The mixer has to be re-initialised for the new volume to be heard.
	 *
	 * POKEY's mixer copies POKEYSND_volume into its own gain tables when it is
	 * initialised (mzpokeysnd.c: "volume.s16 = POKEYSND_volume * 0xffff / 256.0"),
	 * so setting the variable alone changes nothing on the mixer that is already
	 * running.  That is why changing the volume in the menu used to do nothing
	 * until the sound was switched off and on again -- switching it re-ran the
	 * whole set-up and re-initialised the mixer as a side effect.
	 *
	 * POKEYSND_DoInit() repeats POKEY's initialisation with the parameters it
	 * already holds.  It touches no device, which matters here: reopening the
	 * audio sink hangs on this hardware, so re-initialising the mixer is the only
	 * way to make a volume change audible without a restart.
	 */
	POKEYSND_DoInit();
}

void ATARI_SetSoundEnabled(int enabled)
{
	int wanted = enabled ? 1 : 0;

	if (wanted == s_config.sound_enabled) {
		return;
	}
	s_config.sound_enabled = wanted;

	if (s_initialised) {
		ApplySoundConfiguration();
	}
}

/*
 * Whether BRK runs as an instruction or stops the frame for the log.
 *
 * The switch lives inside the core's LIBATARI800 build, where a BRK is treated
 * as "the program ended" unless told otherwise.  That is the wrong default for
 * an emulator: BRK is how a debugger takes control.  See the declaration in
 * atari_port.h and docs/using.md.
 */
void ATARI_SetContinueOnBrk(int bContinue)
{
	libatari800_continue_emulation_on_brk(bContinue ? TRUE : FALSE);
}

void ATARI_SetStereo(int enabled)
{
	int wanted = enabled ? 1 : 0;

	if (wanted == s_config.sound_stereo) {
		return;
	}
	s_config.sound_stereo = wanted;

	if (s_initialised && s_config.sound_enabled) {
		ApplySoundConfiguration();
	}
}

atari_ram_kb_t ATARI_GetRamSize(void)
{
	return s_config.ram_kb;
}

atari_machine_t ATARI_GetMachine(void)
{
	return s_config.machine;
}

atari_video_standard_t ATARI_GetVideoStandard(void)
{
	return s_config.video;
}

/*
 * Rebuilds the emulated machine after a change to its RAM size or personality.
 */
static int RebuildMachine(void)
{
	if (!s_initialised) {
		return 1;
	}

	MEMORY_ram_size = CoreRamSize(s_config.ram_kb);
	Atari800_SetMachineType(CoreMachineType(s_config.machine));
	ApplyMachinePersonality(s_config.machine);

	/*
	 * Changing the RAM size or the machine type only takes effect when the
	 * machine is rebuilt, and the rebuilt machine starts from cold.
	 */
	if (!Atari800_InitialiseMachine()) {
		SetError("the emulated machine could not be rebuilt: no usable OS ROM");
		return 0;
	}
	Atari800_Coldstart();

	s_next_frame_time = 0;
	s_frame_phase = 0;
	/* A rebuilt machine starts from cold, so it needs its own grace period. */
	s_frames_since_reset = 0;
	s_blank_frames = 0;
	return 1;
}

int ATARI_SetRamSize(atari_ram_kb_t ram_kb)
{
	if (ram_kb == s_config.ram_kb) {
		return 1;
	}
	s_config.ram_kb = ram_kb;
	return RebuildMachine();
}

int ATARI_SetMachine(atari_machine_t machine)
{
	if (machine == s_config.machine) {
		return 1;
	}
	s_config.machine = machine;
	return RebuildMachine();
}

/* ===========================================================================
 * Media
 * ======================================================================== */

/*
 * Reports whether a disk image is in one of the compressed formats the core
 * would try to decompress.
 *
 * Those formats are refused deliberately, and the reason is worth recording
 * because refusing a file that "should" work looks like an oversight.
 *
 * To read a gzipped or DCM image, the core decompresses it to a temporary file
 * and then opens that.  It has no temporary-file support here, so its
 * Util_tmpopen() falls back to a path that calls snprintf() with a NULL buffer
 * -- a store to address zero, which is not a graceful failure.  Reaching that
 * from a file on the SD card would be a crash on startup with no explanation.
 *
 * There is also nowhere sensible for a temporary file to go.  This is a
 * bare-metal FAT volume with no notion of a working directory and no unlink()
 * in the file layer, so a decompressed image could not be removed afterwards.
 *
 * So the image is identified up front and refused with a message that says what
 * to do instead: decompress it on a PC.  Without this check the user would get a
 * crash, or a baffling "cannot mount".
 *
 * \return A description of the format if it is one the core would decompress,
 *	   or NULL if the file is one this layer should hand to the core.
 */
static const char *CompressedImageFormat(const char *path)
{
	static const unsigned char s_gzipMagic[2] = { 0x1f, 0x8b };
	static const unsigned char s_dcmMagic[4] = { 'D', 'C', 'M', 0x01 };
	unsigned char header[4] = { 0, 0, 0, 0 };
	FILE *file;
	size_t got;

	file = ATARI_VFS_Open(path, "rb");
	if (file == 0) {
		/* Not readable.  Let the core report that, in its own words. */
		return 0;
	}

	got = ATARI_VFS_Read(header, 1, sizeof header, file);
	ATARI_VFS_Close(file);

	if (got >= sizeof s_gzipMagic
	    && memcmp(header, s_gzipMagic, sizeof s_gzipMagic) == 0) {
		return "gzip compressed";
	}

	if (got >= sizeof s_dcmMagic
	    && memcmp(header, s_dcmMagic, sizeof s_dcmMagic) == 0) {
		return "DCM compressed";
	}

	return 0;
}

int ATARI_MountDisk(int drive, const char *path, int read_only)
{
	if (!s_initialised) {
		SetError("the emulator is not running");
		return 0;
	}
	if (drive < 1 || drive > 8) {
		SetError("drive %d is out of range (1-8)", drive);
		return 0;
	}

	if (path == 0 || path[0] == '\0') {
		return ATARI_UnmountDisk(drive);
	}

	{
		const char *format = CompressedImageFormat(path);

		if (format != 0) {
			SetError("%s is %s; decompress it to a plain ATR or XFD first",
				 path, format);
			return 0;
		}
	}

	if (SIO_Mount(drive, path, read_only)) {
		unsigned nUsed = 0, nTotal = 0;

		/*
		 * Report what the drive really is, not what was asked for.
		 *
		 * With ignore_header_writeprotect set (see ATARI_Init) the only way a
		 * read/write request comes back read-only is that the file itself could
		 * not be opened for writing -- a read-only file attribute, or a locked
		 * card.  Silently dropping the writes is the one outcome that must not
		 * happen unnoticed: the emulated 1050 reports success to the machine
		 * whatever the file layer does.
		 */
		if (!read_only && SIO_drive_status[drive - 1] != SIO_READ_WRITE) {
			ATARI_RuntimeLog("disk %d: %s cannot be opened for writing; "
			                 "the drive is read only", drive, path);
		}

		ATARI_VFS_GetHandleStats(&nUsed, &nTotal);

		ATARI_RuntimeLog("disk %d: mounted %s (%s) -- file handles %u of %u used",
		                 drive, path,
		                 SIO_drive_status[drive - 1] == SIO_READ_WRITE
		                 ? "read/write" : "read only",
		                 nUsed, nTotal);

		/*
		 * The canary that used to be filled here was never once damaged across
		 * several runs, so a buffer overrun adjacent to it is ruled out and the
		 * check has been removed rather than left to imply something it cannot.
		 */

		/* Follow this image through the file layer as the machine boots. */
		ATARI_VFS_BeginFileTrace();

		return 1;
	}

	ATARI_RuntimeLog("disk %d: FAILED to mount %s", drive, path);
	SetError("cannot mount %s in drive %d", path, drive);
	return 0;
}

/*
 * Crash marker.
 *
 * Sits beside the settings file.  Holds a single character: '1' while a crash
 * is recorded, '0' otherwise.  A one-byte rewrite is used to clear it rather
 * than a delete, because the VFS has no remove and adding one for this would be
 * more risk than the marker is worth.
 */
#define CRASH_MARKER_PATH	"SD:/ATARI/crashed.flag"

void ATARI_MarkCrashed(void)
{
	FILE *pFile = ATARI_VFS_Open(CRASH_MARKER_PATH, "wb");

	if (pFile == 0) {
		return;
	}

	(void) ATARI_VFS_Write("1", 1, 1, pFile);
	(void) ATARI_VFS_Close(pFile);
}

int ATARI_CrashMarkerConsume(void)
{
	FILE *pFile = ATARI_VFS_Open(CRASH_MARKER_PATH, "rb");
	char c = '0';
	int bCrashed = 0;

	if (pFile == 0) {
		return 0;
	}

	if (ATARI_VFS_Read(&c, 1, 1, pFile) == 1 && c == '1') {
		bCrashed = 1;
	}
	(void) ATARI_VFS_Close(pFile);

	/* Clear whatever was there, so a second boot does not repeat the reset. */
	pFile = ATARI_VFS_Open(CRASH_MARKER_PATH, "wb");
	if (pFile != 0) {
		(void) ATARI_VFS_Write("0", 1, 1, pFile);
		(void) ATARI_VFS_Close(pFile);
	}

	return bCrashed;
}

void ATARI_LogCrashState(void)
{
	ATARI_RuntimeLog("    6502 PC 0x%04x  A 0x%02x  ANTIC DMACTL 0x%02x  "
			 "dlist 0x%04x  xpos %d  ypos %d",
			 (unsigned) CPU_regPC, (unsigned) CPU_regA,
			 (unsigned) ANTIC_DMACTL, (unsigned) ANTIC_dlist,
			 ANTIC_xpos, ANTIC_ypos);

	ATARI_RuntimeLog("    machine %d, %d kB, drive 1 status %d, "
			 "last sector %d",
			 (int) s_config.machine, (int) s_config.ram_kb,
			 (int) SIO_drive_status[0], SIO_last_sector);

	{
		unsigned nUsed = 0, nTotal = 0;

		ATARI_VFS_GetHandleStats(&nUsed, &nTotal);
		ATARI_RuntimeLog("    file handles %u of %u used", nUsed, nTotal);
	}
}

int ATARI_UnmountDisk(int drive)
{
	if (drive < 1 || drive > SIO_MAX_DRIVES) {
		return 0;
	}

	ATARI_RuntimeLog("disk %d: unmounting", drive);
	SIO_Dismount(drive);
	ATARI_RuntimeLog("disk %d: unmounted", drive);
	return 1;
}

/*
 * A cartridge takes effect on the next reset, and that reset is deferred until
 * the menu closes.
 *
 * Doing it inside the menu ran the emulated machine while the OSD still owned
 * the display and, more importantly, while the storage layer was mid-session
 * with another image already open.  Selecting a disk and then a cartridge hung
 * there; deferring the reset means it happens once, at a point where the menu
 * has released both.
 */
static int s_bColdStartPending;

void ATARI_ClearPendingColdStart(void)
{
	s_bColdStartPending = 0;
}

void ATARI_ApplyPendingColdStart(void)
{
	if (!s_bColdStartPending) {
		return;
	}

	s_bColdStartPending = 0;
	ATARI_RuntimeLog("cartridge: applying the deferred cold start");
	ATARI_ColdReset();
}

int ATARI_MountCartridge(const char *path)
{
	if (!s_initialised) {
		SetError("the emulator is not running");
		return 0;
	}

	if (path == 0 || path[0] == '\0') {
		ATARI_RuntimeLog("cartridge: removed");
		CARTRIDGE_Remove();
		return 1;
	}

	ATARI_RuntimeLog("cartridge: inserting %s", path);

	/*
	 * CARTRIDGE_Insert() returns 0 on success.
	 *
	 * The value is not a boolean and not a size.  CARTRIDGE_ReadImage() returns:
	 *   0                        success -- the type was identified unambiguously
	 *   positive                 the size matches more than one cartridge type;
	 *                            the core expects the user to choose one
	 *   CARTRIDGE_BAD_CHECKSUM   read, but the CART checksum failed
	 *   other negative           the file could not be opened, read, or parsed
	 *
	 * InsertCartridge() re-initialises the cartridge on 0 and on a bad checksum,
	 * which is the step that actually maps it into the machine -- so 0 is the
	 * success case, not an empty result.
	 */
	{
		int nResult = CARTRIDGE_Insert(path);

		if (nResult == 0 || nResult == CARTRIDGE_BAD_CHECKSUM) {
			ATARI_RuntimeLog("cartridge: mounted %s%s",
					 path,
					 nResult == CARTRIDGE_BAD_CHECKSUM
					 ? " (bad checksum, still usable)" : "");

			/*
			 * The machine is not reset here.  This call arrives from inside the
			 * menu, and running the emulated CPU while the OSD owns the display
			 * and another image is open on the same storage is what hung after
			 * selecting a disk and then a cartridge.  The reset is applied when
			 * the menu closes, so the user still sees the new cartridge at once.
			 */
			s_bColdStartPending = 1;
			ATARI_RuntimeLog("cartridge: cold start deferred until the menu "
					 "closes");
			return 1;
		}

		if (nResult > 0) {
			/*
			 * Several cartridge types share this size.  Desktop Atari800 asks
			 * the user which one; this port has no such prompt, so the image is
			 * left unloaded rather than guessing -- a wrong type maps the wrong
			 * banks and the machine runs garbage.
			 */
			ATARI_RuntimeLog("cartridge: %s matches %d kB of more than one "
					 "type; cannot choose, not mounted",
					 path, nResult);
			SetError("%s needs a cartridge type selected; not mounted", path);
			return 0;
		}

		ATARI_RuntimeLog("cartridge: FAILED to mount %s (error %d)",
				 path, nResult);
		SetError("cannot mount cartridge %s (error %d)", path, nResult);
		return 0;
	}
}


int ATARI_SetCassetteWriteProtect(int read_only)
{
	int want = read_only ? 1 : 0;

	if (!s_initialised) {
		SetError("the emulator is not running");
		return 0;
	}
	if (CASSETTE_status != CASSETTE_STATUS_READ_WRITE) {
		SetError("no cassette is inserted");
		return 0;
	}
	if (CASSETTE_write_protect == want) {
		return 1;
	}
	if (!CASSETTE_ToggleWriteProtect()) {
		SetError("the cassette cannot be toggled");
		return 0;
	}

	return CASSETTE_write_protect == want;
}

int ATARI_MountCassette(const char *path, int read_only)
{
	if (!s_initialised) {
		SetError("the emulator is not running");
		return 0;
	}

	if (path == 0 || path[0] == '\0') {
		ATARI_RuntimeLog("cassette: removed");
		s_bCassetteMounted = 0;
		s_cassette_type = CASSETTE_TYPE_UNKNOWN;
		CASSETTE_Remove();
		return 1;
	}

	ATARI_RuntimeLog("cassette: inserting %s%s", path,
	                 read_only ? " (read only)" : "");

	/*
	 * Classified before the insert, and kept afterwards: what the tape needs is
	 * compared against the mode the machine was built with, and a mismatch is
	 * resolved by rebuilding it -- see ATARI_CassetteNeedsRebuild().
	 */
	ATARI_ClassifyCassette(path);

	if (CASSETTE_Insert(path)) {
		/*
		 * The core keeps the write protect state, not the caller, and exposes
		 * it as a toggle whose return value says whether it toggled -- not what
		 * the state became.  So the state itself is compared, not the result.
		 */
		ATARI_SetCassetteWriteProtect(read_only);

		s_bCassetteMounted = 1;

		ATARI_RuntimeLog("cassette: mounted %s, write protect %s", path,
		                 CASSETTE_write_protect ? "on" : "off");

		if (ATARI_CassetteNeedsRebuild()) {
			ATARI_RuntimeLog("cassette: the running machine is in the other "
			                 "SIO mode; a power cycle is required");
		}

		return 1;
	}

	s_cassette_type = CASSETTE_TYPE_UNKNOWN;
	s_bCassetteMounted = 0;
	ATARI_RuntimeLog("cassette: FAILED to mount %s", path);
	SetError("cannot mount cassette %s", path);
	return 0;
}

int ATARI_BootFile(const char *path){
	int file_type;

	if (!s_initialised || path == 0 || path[0] == '\0') {
		return 0;
	}
	/*
	 * AFILE_OpenFile() decides what the file is (executable, disk image,
	 * cartridge, cassette, ...) and runs it.  A cold start is what makes the
	 * loaded program actually take effect, which is what upstream's own
	 * "reboot with file" path does.
	 */
	file_type = AFILE_OpenFile(path, FALSE, 1, FALSE);
	if (file_type != AFILE_ERROR) {
		Atari800_Coldstart();
		return 1;
	}

	SetError("cannot load %s", path);
	return 0;
}

/* ===========================================================================
 * Snapshots
 * ======================================================================== */

/*
 * A snapshot is produced and consumed through the core's in-memory save
 * backend.  The buffer has to be large enough for the whole emulated machine:
 * for a 320 kB Atari that is roughly 450 kB, which is why the build raises
 * STATESAV_MAX_SIZE above the upstream default of 210 kB.
 *
 * The buffer is allocated once, on first use, and kept: the Circle heap may
 * leak blocks larger than 512 kB when they are freed, so a long-lived buffer
 * is both cheaper and safer than allocating per snapshot.
 */
static emulator_state_t *s_snapshot_buffer;

static emulator_state_t *SnapshotBuffer(void)
{
	if (s_snapshot_buffer == 0) {
		s_snapshot_buffer = (emulator_state_t *)Util_malloc(sizeof(emulator_state_t));
		if (s_snapshot_buffer == 0) {
			SetError("out of memory for a snapshot buffer");
		}
	}
	return s_snapshot_buffer;
}

int ATARI_SaveSnapshot(const char *path)
{
	emulator_state_t *state;
	FILE *file;
	uint32_t length;

	if (!s_initialised || path == 0 || path[0] == '\0') {
		return 0;
	}

	state = SnapshotBuffer();
	if (state == 0) {
		return 0;
	}

	memset(state, 0, sizeof *state);

	/*
	 * The core serialises the whole machine -- RAM, the extended memory
	 * banks, the chip state and the CPU registers -- into state->state, and
	 * records the byte offsets of each block in state->tags.
	 */
	LIBATARI800_StateSave(state->state, &state->tags);
	state->flags.selftest_enabled = MEMORY_selftest_enabled;
	state->flags.nframes = (ULONG)Atari800_nframes;
	state->flags.sample_residual = (ULONG)(0xffffffff * sample_residual);

	length = state->tags.size;
	if (length == 0) {
		SetError("the emulation core produced an empty snapshot");
		return 0;
	}
	if (length > (uint32_t)STATESAV_MAX_SIZE) {
		SetError("the snapshot does not fit in the state buffer");
		return 0;
	}

	file = ATARI_VFS_Open(path, "wb");
	if (file == 0) {
		SetError("cannot create %s", path);
		return 0;
	}

	if (ATARI_VFS_Write(&state->tags, 1, sizeof state->tags, file) != sizeof state->tags
	    || ATARI_VFS_Write(&state->flags, 1, sizeof state->flags, file) != sizeof state->flags
	    || ATARI_VFS_Write(state->state, 1, length, file) != length) {
		SetError("writing the snapshot to %s failed", path);
		ATARI_VFS_Close(file);
		return 0;
	}

	if (ATARI_VFS_Close(file) != 0) {
		SetError("closing %s failed", path);
		return 0;
	}
	return 1;
}

int ATARI_LoadSnapshot(const char *path)
{
	emulator_state_t *state;
	FILE *file;
	uint32_t length;

	if (!s_initialised || path == 0 || path[0] == '\0') {
		return 0;
	}

	state = SnapshotBuffer();
	if (state == 0) {
		return 0;
	}

	memset(state, 0, sizeof *state);

	file = ATARI_VFS_Open(path, "rb");
	if (file == 0) {
		SetError("cannot open %s", path);
		return 0;
	}

	if (ATARI_VFS_Read(&state->tags, 1, sizeof state->tags, file) != sizeof state->tags
	    || ATARI_VFS_Read(&state->flags, 1, sizeof state->flags, file) != sizeof state->flags) {
		SetError("%s is not a snapshot file", path);
		ATARI_VFS_Close(file);
		return 0;
	}

	length = state->tags.size;
	if (length == 0 || length > (uint32_t)STATESAV_MAX_SIZE) {
		SetError("%s is not a valid snapshot", path);
		ATARI_VFS_Close(file);
		return 0;
	}

	if (ATARI_VFS_Read(state->state, 1, length, file) != length) {
		SetError("%s is truncated", path);
		ATARI_VFS_Close(file);
		return 0;
	}
	ATARI_VFS_Close(file);

	/*
	 * Check the signature before loading.
	 *
	 * The core's StateSav_ReadAtariState() rejects a buffer that does not begin
	 * with "ATARI800", but the library entry point this uses wraps it and
	 * returns nothing, so that rejection cannot be observed from here.  Without
	 * this check a bogus or unrelated file would be reported to the user as
	 * having loaded successfully, while the machine silently kept its previous
	 * state -- the worst kind of failure to diagnose.
	 */
	if (length < 8 || memcmp(state->state, "ATARI800", 8) != 0) {
		SetError("%s is not an Atari800 snapshot", path);
		return 0;
	}

	LIBATARI800_StateLoad(state->state);
	MEMORY_selftest_enabled = state->flags.selftest_enabled;
	Atari800_nframes = (int)state->flags.nframes;
	sample_residual = (double)state->flags.sample_residual / (double)0xffffffff;

	return 1;
}
