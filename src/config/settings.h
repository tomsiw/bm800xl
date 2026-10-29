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

/*
 * How the emulated picture is scaled up to the framebuffer.
 *
 * The values are the scale factor, so they can be handed to the video layer as
 * they are; 0 is the aspect-correct fit rather than an integer block.
 */
enum TAtariAspect
{
	AtariAspect43 = 0,	///< fit a 4:3 picture to the framebuffer height
	AtariAspect2x = 2,	///< every pixel a 2x2 block
	AtariAspect3x = 3	///< every pixel a 3x3 block
};

/* Where the two Atari joystick ports are fed from. */
enum TAtariJoystickInterface
{
	AtariJoystickInterfaceUSB = 0,	///< two USB gamepads
	AtariJoystickInterfaceGPIO,	///< the two 9-pin ports on the GPIO header
	AtariJoystickInterfaceKeyboard,	///< keys on the USB keyboard, bound in usbkeys.cfg
	AtariJoystickInterfaceUnknown
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
	int		nResolution;
	int		nAspect;		///< TAtariAspect

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
	 * Whether the OSD is drawn on a static picture.
	 *
	 * With this off, every OSD redraw also repaints the emulated picture behind
	 * it, and the panel sits on a dimmed copy of that picture.  With it on, the
	 * picture is left exactly as it was -- the OSD is drawn over the frame that
	 * is already there, and the panel gets a solid background instead of a
	 * dimmed picture.
	 *
	 * Off by default, because the picture behind the menu is part of how the
	 * OSD looks.  On the slower boards it is the difference between a readable
	 * menu and a stuttering machine: repainting 960x720 pixels costs about
	 * 18 ms of a 20 ms frame there, so a redraw cannot fit alongside the
	 * emulation.
	 */
	boolean		bStaticOSD;

	/*
	 * Controllers.
	 *
	 * One interface at a time, not both: the two Atari ports are fed from either
	 * two USB gamepads or the two 9-pin ports on the GPIO header, and mixing the
	 * two would mean deciding case by case which source owns a port.  The
	 * interface is chosen when the controllers are created, which is before the
	 * emulator runs, so changing it needs the Pi switched off and on.
	 *
	 * Swap exchanges the two ports, and unlike the interface it is applied
	 * immediately: it is only a question of which handle the platform layer finds
	 * in which slot.
	 */
	int		nJoystickInterface;	///< TAtariJoystickInterface
	boolean		bSwapJoystickPorts;

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
