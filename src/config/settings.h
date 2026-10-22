/*
 * settings.h -- the emulator's persistent configuration.
 *
 * Settings live in a small text file on the SD card, next to the emulator
 * itself, so that they follow the card rather than the removable drive the
 * media images are on.  The format is deliberately plain text with one
 * "KEY=VALUE" pair per line:
 *
 *   * it can be read and edited by hand when something goes wrong;
 *   * a partially written file still yields the settings that were intact,
 *     because unknown keys and unparsable values are skipped rather than
 *     treated as fatal;
 *   * no parser beyond string comparison is needed.
 *
 * Anything the user changes in the OSD menu is written back here, so the
 * emulator comes up the way it was left.
 */

#ifndef ATARI_PI_SETTINGS_H
#define ATARI_PI_SETTINGS_H

#include <circle/types.h>

#include "storage.h"
#include "atari_port.h"

/* Number of disk drives the emulated machine can have. */
#define SETTINGS_MAX_DRIVES 4

/* Longest path stored for a media image, including the terminating NUL. */
#define SETTINGS_PATH_MAX 192

/* Which keyboard source to use. */
enum TAtariKeyboardMode
{
	AtariKeyboardModeUSB = 0,	///< USB keyboard on a Pi USB port
	AtariKeyboardModeGPIO,		///< Atari keyboard wired to the GPIO header
	AtariKeyboardModeUnknown
};

/* Output resolution preference. */
enum TAtariResolution
{
	AtariResolution720p = 0,
	AtariResolution1080p,
	AtariResolutionUnknown
};

struct TAtariSettings
{
	// Emulated machine
	int		nMachine;		///< atari_machine_t
	int		nRamKB;			///< atari_ram_kb_t
	int		nVideoStandard;		///< atari_video_standard_t
	int		nArtifact;		///< atari_artifact_t

	// Audio
	boolean		bSoundEnabled;
	int		nSoundVolume;		///< 0..100
	boolean		bSoundStereo;

	// Display
	int		nResolution;		///< TAtariResolution

	// Input
	int		nKeyboardMode;		///< TAtariKeyboardMode
	boolean		bNumpadAsCursor;

	/*
	 * Whether BRK runs as the 6502 instruction it is, or stops the frame so
	 * that the log can report it.
	 *
	 * BRK is how a debugger takes control -- MAC/65's DDT, for example --
	 * so following the machine is the default.  The other behaviour is a
	 * diagnostic for a program that has crashed into a BRK.
	 */
	boolean		bContinueOnBrk;

	/*
	 * Controllers.
	 *
	 * The two kinds are independent rather than one setting with three values,
	 * because they are independent in reality: they share no hardware, so "both"
	 * is a state worth being able to ask for.  The one thing that cannot happen
	 * is GPIO joysticks alongside a GPIO keyboard, which needs no setting --
	 * they need the same pins, so the kernel refuses and says so.
	 */
	boolean		bUSBJoysticks;
	boolean		bGPIOJoysticks;

	// Media.  Empty means nothing loaded.
	char		DiskPath[SETTINGS_MAX_DRIVES][SETTINGS_PATH_MAX];
	boolean		DiskReadOnly[SETTINGS_MAX_DRIVES];
	char		CartridgePath[SETTINGS_PATH_MAX];
	char		CassettePath[SETTINGS_PATH_MAX];
	boolean		bCassetteReadOnly;

	// Where the user was last browsing, so the file browser reopens there.
	char		UserDirectory[SETTINGS_PATH_MAX];
	char		SystemDirectory[SETTINGS_PATH_MAX];
};

/* Fills SETTINGS with the defaults the emulator ships with. */
void Settings_SetDefaults (TAtariSettings *pSettings);

/*
 * Reads settings from PATH.
 *
 * A missing file is not an error: SETTINGS keeps its defaults and the function
 * returns FALSE so the caller can write a file out.
 */
boolean Settings_Load (const CAtariStorage &Storage, const char *pPath,
		       TAtariSettings *pSettings);

/* Writes settings to PATH, creating or truncating it. */
boolean Settings_Save (const CAtariStorage &Storage, const char *pPath,
		       const TAtariSettings &Settings);

#endif
