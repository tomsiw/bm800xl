/*
 * settings.cpp -- the emulator's persistent configuration.  See settings.h.
 *
 * The file format is one "KEY=VALUE" per line with '#' marking a comment.  Keys
 * are matched exactly; a line whose key is not recognised is ignored, which is
 * what makes the file forward and backward compatible and lets a user leave
 * notes in it.
 *
 * The whole file is read into memory and then parsed.  That keeps this file
 * dependent on only the four basic file operations (open, read, write, close),
 * and a settings file is far too small for streaming to be worth the extra
 * complexity.
 *
 * Values are read with bounded conversions rather than sscanf() so that a
 * corrupt or truncated file cannot run off the end of a buffer or produce a
 * surprising value.
 */

#include "settings.h"

#include "vfs.h"

#include <circle/logger.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char FromSettings[] = "settings";

/* The largest settings file worth reading.  Anything beyond this is ignored. */
#define SETTINGS_MAX_FILE 4096

/* The file header, so the file explains itself to whoever opens it. */
static const char FileHeader[] =
	"# Atari XL/XE emulator settings\n"
	"# Written automatically whenever a setting is changed in the OSD menu.\n"
	"# It is safe to edit by hand; unknown lines are ignored.\n"
	"\n";

void Settings_SetDefaults (TAtariSettings *pSettings)
{
	if (pSettings == 0)
	{
		return;
	}

	memset (pSettings, 0, sizeof *pSettings);

	pSettings->nMachine = ATARI_MACHINE_XL;
	pSettings->nRamKB = ATARI_RAM_KB_64;
	pSettings->nVideoStandard = ATARI_VIDEO_PAL;
	pSettings->nArtifact = ATARI_ARTIFACT_PAL_SIMPLE;

	pSettings->bSoundEnabled = TRUE;
	pSettings->nSoundVolume = 80;
	pSettings->bSoundStereo = FALSE;

	pSettings->nResolution = AtariResolution720p;

	pSettings->nKeyboardMode = AtariKeyboardModeUSB;
	pSettings->bNumpadAsCursor = TRUE;

	/*
	 * BRK follows the machine until asked otherwise: it is a normal
	 * instruction, and the one a debugger uses to take control.
	 */
	pSettings->bContinueOnBrk = TRUE;

	/*
	 * USB gamepads by default.
	 *
	 * One interface, not two: which one is feeding the two Atari ports has to be
	 * a decision, because the GPIO ports need the same header pins as the GPIO
	 * keyboard and because a machine cannot meaningfully have both kinds wired to
	 * the same two ports.  A USB gamepad plugged in later still works, which is
	 * why USB is the default.
	 */
	pSettings->nAspect = AtariAspect43;

	pSettings->nJoystickInterface = AtariJoystickInterfaceUSB;
	pSettings->bSwapJoystickPorts = FALSE;

	strcpy (pSettings->UserDirectory, STORAGE_VOLUME_USER);
	strcpy (pSettings->SystemDirectory, STORAGE_VOLUME_SYSTEM);
}

/* ---------------------------------------------------------------------------
 * Parsing helpers
 * ------------------------------------------------------------------------- */

/*
 * Reads a non-negative decimal integer from P, storing it in *pValue.
 * Returns TRUE if the whole remaining text was a number.
 */
static boolean ParseInt (const char *p, int *pValue)
{
	char *pEnd = 0;
	long nValue;

	if (p == 0 || p[0] == '\0')
	{
		return FALSE;
	}

	nValue = strtol (p, &pEnd, 10);
	if (pEnd == p || (pEnd != 0 && *pEnd != '\0'))
	{
		return FALSE;
	}
	// Keep the value inside a range every consumer can cope with; the exact
	// limits are checked where the value is used.
	if (nValue < -1000000 || nValue > 1000000)
	{
		return FALSE;
	}

	*pValue = (int) nValue;
	return TRUE;
}

/*
 * Compares a settings value with a word, ignoring case.
 *
 * Written out rather than using strcasecmp because that function is not
 * declared by the C library this builds against unless the feature macros are
 * set, and a hand-rolled comparison of two short ASCII words is not worth
 * another configuration knob.
 */
static boolean EqualsIgnoreCase (const char *pA, const char *pB)
{
	while (*pA != '\0' && *pB != '\0')
	{
		char a = *pA++;
		char b = *pB++;

		if (a >= 'A' && a <= 'Z')
		{
			a += 'a' - 'A';
		}
		if (b >= 'A' && b <= 'Z')
		{
			b += 'a' - 'A';
		}

		if (a != b)
		{
			return FALSE;
		}
	}

	return *pA == '\0' && *pB == '\0';
}

/*
 * Reads a boolean written as "0"/"1", "yes"/"no", "true"/"false" or
 * "on"/"off", in any case.
 *
 * The words are read as words, not by their first letter.  Deciding on the
 * first letter alone would make "off" true -- it starts with 'o' -- which is
 * exactly the kind of silent inversion that turns a hand-edited settings file
 * into a puzzle.
 *
 * The value is only assigned on success, so a caller that provides a default
 * keeps it when this returns FALSE.
 */
static boolean ParseBool (const char *p, boolean *pValue)
{
	if (p == 0 || p[0] == '\0')
	{
		return FALSE;
	}

	// A single digit is the common case in a settings file.
	if (p[1] == '\0')
	{
		if (p[0] == '0')
		{
			*pValue = FALSE;
			return TRUE;
		}
		if (p[0] == '1')
		{
			*pValue = TRUE;
			return TRUE;
		}
		return FALSE;
	}

	static const struct
	{
		const char *pWord;
		boolean bValue;
	} Words[] = {
		{ "no",    FALSE },
		{ "off",   FALSE },
		{ "false", FALSE },
		{ "yes",   TRUE  },
		{ "on",    TRUE  },
		{ "true",  TRUE  },
	};

	for (unsigned i = 0; i < sizeof Words / sizeof Words[0]; i++)
	{
		if (EqualsIgnoreCase (p, Words[i].pWord))
		{
			*pValue = Words[i].bValue;
			return TRUE;
		}
	}

	return FALSE;
}

/* Copies P into TARGET, truncating rather than overflowing. */
static void ParseString (const char *p, char *pTarget, unsigned nSize)
{
	unsigned nLength;

	if (nSize == 0)
	{
		return;
	}
	if (p == 0)
	{
		pTarget[0] = '\0';
		return;
	}

	nLength = strlen (p);
	if (nLength >= nSize)
	{
		nLength = nSize - 1;
	}
	memcpy (pTarget, p, nLength);
	pTarget[nLength] = '\0';
}

/* ---------------------------------------------------------------------------
 * Loading
 * ------------------------------------------------------------------------- */

/*
 * Applies one key/value pair.  Unknown keys are silently ignored.
 */
static void ApplySetting (TAtariSettings *pSettings, const char *pKey, char *pValue)
{
	int nIndex;
	int nValue;
	boolean bValue;

	// Drive settings carry their index, e.g. "DISK1_PATH".
	if (strncmp (pKey, "DISK", 4) == 0
	    && pKey[4] >= '1' && pKey[4] <= '1' + SETTINGS_MAX_DRIVES - 1)
	{
		nIndex = pKey[4] - '1';
		if (strcmp (pKey + 5, "_PATH") == 0)
		{
			ParseString (pValue, pSettings->DiskPath[nIndex], SETTINGS_PATH_MAX);
			return;
		}
		if (strcmp (pKey + 5, "_READONLY") == 0)
		{
			if (ParseBool (pValue, &bValue))
			{
				pSettings->DiskReadOnly[nIndex] = bValue;
			}
			return;
		}
		return;
	}

	if (strcmp (pKey, "MACHINE") == 0)
	{
		if (ParseInt (pValue, &nValue) && nValue >= 0 && nValue < 3)
		{
			pSettings->nMachine = nValue;
		}
	}
	else if (strcmp (pKey, "RAM_KB") == 0)
	{
		if (ParseInt (pValue, &nValue))
		{
			// Validated against the core's supported sizes when applied.
			pSettings->nRamKB = nValue;
		}
	}
	else if (strcmp (pKey, "VIDEO") == 0)
	{
		if (ParseInt (pValue, &nValue) && nValue >= 0 && nValue <= 1)
		{
			pSettings->nVideoStandard = nValue;
		}
	}
	else if (strcmp (pKey, "ARTIFACT") == 0)
	{
		if (ParseInt (pValue, &nValue) && nValue >= 0 && nValue <= 4)
		{
			pSettings->nArtifact = nValue;
		}
	}
	else if (strcmp (pKey, "SOUND") == 0)
	{
		if (ParseBool (pValue, &bValue))
		{
			pSettings->bSoundEnabled = bValue;
		}
	}
	else if (strcmp (pKey, "VOLUME") == 0)
	{
		if (ParseInt (pValue, &nValue) && nValue >= 0 && nValue <= 100)
		{
			pSettings->nSoundVolume = nValue;
		}
	}
	else if (strcmp (pKey, "STEREO") == 0)
	{
		if (ParseBool (pValue, &bValue))
		{
			pSettings->bSoundStereo = bValue;
		}
	}
	else if (strcmp (pKey, "RESOLUTION") == 0)
	{
		if (ParseInt (pValue, &nValue) && nValue >= 0 && nValue <= 1)
		{
			pSettings->nResolution = nValue;
		}
	}
	else if (strcmp (pKey, "KEYBOARD") == 0)
	{
		if (ParseInt (pValue, &nValue) && nValue >= 0 && nValue <= 1)
		{
			pSettings->nKeyboardMode = nValue;
		}
	}
	else if (strcmp (pKey, "NUMPAD_CURSOR") == 0)
	{
		if (ParseBool (pValue, &bValue))
		{
			pSettings->bNumpadAsCursor = bValue;
		}
	}
	else if (strcmp (pKey, "CONTINUE_ON_BRK") == 0)
	{
		if (ParseBool (pValue, &bValue))
		{
			pSettings->bContinueOnBrk = bValue;
		}
	}
	else if (strcmp (pKey, "STATIC_OSD") == 0)
	{
		if (ParseBool (pValue, &bValue))
		{
			pSettings->bStaticOSD = bValue;
		}
	}
	else if (strcmp (pKey, "JOY_INTERFACE") == 0)
	{
		if (EqualsIgnoreCase (pValue, "USB") || strcmp (pValue, "0") == 0)
		{
			pSettings->nAspect = AtariAspect43;

	pSettings->nJoystickInterface = AtariJoystickInterfaceUSB;
		}
		else if (EqualsIgnoreCase (pValue, "GPIO") || strcmp (pValue, "1") == 0)
		{
			pSettings->nJoystickInterface = AtariJoystickInterfaceGPIO;
		}
		else if (EqualsIgnoreCase (pValue, "KEYBOARD") || strcmp (pValue, "2") == 0)
		{
			pSettings->nJoystickInterface = AtariJoystickInterfaceKeyboard;
		}
	}
	else if (strcmp (pKey, "ASPECT") == 0)
	{
		if (EqualsIgnoreCase (pValue, "4:3") || strcmp (pValue, "0") == 0)
		{
			pSettings->nAspect = AtariAspect43;
		}
		else if (EqualsIgnoreCase (pValue, "2X") || strcmp (pValue, "2") == 0)
		{
			pSettings->nAspect = AtariAspect2x;
		}
		else if (EqualsIgnoreCase (pValue, "3X") || strcmp (pValue, "3") == 0)
		{
			pSettings->nAspect = AtariAspect3x;
		}
	}
	else if (strcmp (pKey, "JOY_SWAP") == 0)
	{
		if (ParseBool (pValue, &bValue))
		{
			pSettings->bSwapJoystickPorts = bValue;
		}
	}
	/*
	 * USB_JOYSTICKS and GPIO_JOYSTICKS are deliberately not read, now that one
	 * interface is chosen instead of two independent switches.
	 *
	 * Every settings file written before this change carries both, set to 1, in
	 * that order.  Honouring them would be worse than ignoring them: a rule that
	 * let the second key win would silently put existing users -- all of whom
	 * have only ever run USB controllers, because the 9-pin path was never
	 * reachable -- onto the GPIO interface, which for them means no joystick at
	 * all.  Ignoring both leaves the default, which is USB.
	 */
	else if (strcmp (pKey, "CARTRIDGE") == 0)
	{
		ParseString (pValue, pSettings->CartridgePath, SETTINGS_PATH_MAX);
	}
	else if (strcmp (pKey, "CASSETTE") == 0)
	{
		ParseString (pValue, pSettings->CassettePath, SETTINGS_PATH_MAX);
	}
	else if (strcmp (pKey, "CASSETTE_READONLY") == 0)
	{
		if (ParseBool (pValue, &bValue))
		{
			pSettings->bCassetteReadOnly = bValue;
		}
	}
	else if (strcmp (pKey, "USER_DIR") == 0)
	{
		ParseString (pValue, pSettings->UserDirectory, SETTINGS_PATH_MAX);
	}
	else if (strcmp (pKey, "SYSTEM_DIR") == 0)
	{
		ParseString (pValue, pSettings->SystemDirectory, SETTINGS_PATH_MAX);
	}
}

/*
 * Parses the in-memory contents of a settings file, modifying it in place.
 * Returns the number of settings that were recognised.
 */
static unsigned ParseBuffer (TAtariSettings *pSettings, char *pBuffer)
{
	char *pLine = pBuffer;
	unsigned nApplied = 0;

	while (pLine != 0 && *pLine != '\0')
	{
		char *pNext = strchr (pLine, '\n');
		char *pEquals;
		char *pKey;

		if (pNext != 0)
		{
			*pNext = '\0';
			pNext++;
		}

		// A stray CR from a file written on another system is not part of
		// the value.
		{
			char *pCR = strchr (pLine, '\r');
			if (pCR != 0)
			{
				*pCR = '\0';
			}
		}

		pKey = pLine;
		while (*pKey == ' ' || *pKey == '\t')
		{
			pKey++;
		}

		if (*pKey != '\0' && *pKey != '#')
		{
			pEquals = strchr (pKey, '=');
			if (pEquals != 0)
			{
				*pEquals = '\0';
				ApplySetting (pSettings, pKey, pEquals + 1);
				nApplied++;
			}
		}

		pLine = pNext;
	}

	return nApplied;
}

boolean Settings_Load (const CAtariStorage &Storage, const char *pPath,
		       TAtariSettings *pSettings)
{
	FILE *pFile;
	static char Buffer[SETTINGS_MAX_FILE + 1];
	size_t nRead;
	unsigned nApplied;

	(void) Storage;

	if (pPath == 0 || pSettings == 0)
	{
		return FALSE;
	}

	// Start from the defaults so that a file which only mentions a few
	// settings still leaves the machine in a consistent state.
	Settings_SetDefaults (pSettings);

	pFile = ATARI_VFS_Open (pPath, "rb");
	if (pFile == 0)
	{
		return FALSE;		// no file yet: the defaults are the answer
	}

	nRead = ATARI_VFS_Read (Buffer, 1, SETTINGS_MAX_FILE, pFile);
	ATARI_VFS_Close (pFile);

	Buffer[nRead] = '\0';

	nApplied = ParseBuffer (pSettings, Buffer);
	if (nApplied == 0)
	{
		CLogger::Get ()->Write (FromSettings, LogWarning,
					"%s contains no usable settings", pPath);
		return FALSE;
	}

	return TRUE;
}

/* ---------------------------------------------------------------------------
 * Saving
 * ------------------------------------------------------------------------- */

/*
 * Appends one already-formatted "KEY=VALUE" line to BUFFER.
 *
 * The buffer is fixed size, so a line that would not fit is dropped rather than
 * corrupting memory.  The caller does not need to check: a line is only skipped
 * when the file has grown beyond SETTINGS_MAX_FILE, which the settings shown in
 * the menu cannot reach.
 */
static void AppendLine (char *pBuffer, unsigned nSize, unsigned *pUsed,
			const char *pFormat, ...)
{
	va_list Args;
	int nWritten;
	unsigned nLeft;

	if (*pUsed >= nSize)
	{
		return;
	}
	nLeft = nSize - *pUsed;

	va_start (Args, pFormat);
	nWritten = vsnprintf (pBuffer + *pUsed, nLeft, pFormat, Args);
	va_end (Args);

	if (nWritten <= 0 || (unsigned) nWritten >= nLeft)
	{
		// Truncated or failed: leave the buffer as a valid string.
		pBuffer[*pUsed] = '\0';
		return;
	}
	*pUsed += (unsigned) nWritten;
}

boolean Settings_Save (const CAtariStorage &Storage, const char *pPath,
		       const TAtariSettings &Settings)
{
	FILE *pFile;
	static char Buffer[SETTINGS_MAX_FILE];
	unsigned nUsed = 0;

	(void) Storage;

	if (pPath == 0)
	{
		return FALSE;
	}

	// The header explains the file to anyone who opens it in a text editor.
	memcpy (Buffer, FileHeader, sizeof FileHeader - 1);
	nUsed = sizeof FileHeader - 1;

	AppendLine (Buffer, sizeof Buffer, &nUsed, "MACHINE=%d\n", Settings.nMachine);
	AppendLine (Buffer, sizeof Buffer, &nUsed, "RAM_KB=%d\n", Settings.nRamKB);
	AppendLine (Buffer, sizeof Buffer, &nUsed, "VIDEO=%d\n", Settings.nVideoStandard);
	AppendLine (Buffer, sizeof Buffer, &nUsed, "ARTIFACT=%d\n", Settings.nArtifact);

	AppendLine (Buffer, sizeof Buffer, &nUsed, "SOUND=%d\n", Settings.bSoundEnabled ? 1 : 0);
	AppendLine (Buffer, sizeof Buffer, &nUsed, "VOLUME=%d\n", Settings.nSoundVolume);
	AppendLine (Buffer, sizeof Buffer, &nUsed, "STEREO=%d\n", Settings.bSoundStereo ? 1 : 0);

	AppendLine (Buffer, sizeof Buffer, &nUsed, "RESOLUTION=%d\n", Settings.nResolution);

	AppendLine (Buffer, sizeof Buffer, &nUsed, "KEYBOARD=%d\n", Settings.nKeyboardMode);
	AppendLine (Buffer, sizeof Buffer, &nUsed, "NUMPAD_CURSOR=%d\n",
		    Settings.bNumpadAsCursor ? 1 : 0);
	AppendLine (Buffer, sizeof Buffer, &nUsed, "CONTINUE_ON_BRK=%d\n",
		    Settings.bContinueOnBrk ? 1 : 0);
	AppendLine (Buffer, sizeof Buffer, &nUsed, "STATIC_OSD=%d\n",
		    Settings.bStaticOSD ? 1 : 0);
	AppendLine (Buffer, sizeof Buffer, &nUsed, "ASPECT=%s\n",
		    Settings.nAspect == AtariAspect2x ? "2X"
		    : Settings.nAspect == AtariAspect3x ? "3X"
		    : "4:3");
	AppendLine (Buffer, sizeof Buffer, &nUsed, "JOY_INTERFACE=%s\n",
		    Settings.nJoystickInterface == AtariJoystickInterfaceGPIO
			? "GPIO"
		    : Settings.nJoystickInterface == AtariJoystickInterfaceKeyboard
			? "KEYBOARD" : "USB");
	AppendLine (Buffer, sizeof Buffer, &nUsed, "JOY_SWAP=%d\n",
		    Settings.bSwapJoystickPorts ? 1 : 0);

	for (int i = 0; i < SETTINGS_MAX_DRIVES; i++)
	{
		AppendLine (Buffer, sizeof Buffer, &nUsed, "DISK%d_PATH=%s\n",
			    i + 1, Settings.DiskPath[i]);
		AppendLine (Buffer, sizeof Buffer, &nUsed, "DISK%d_READONLY=%d\n",
			    i + 1, Settings.DiskReadOnly[i] ? 1 : 0);
	}

	AppendLine (Buffer, sizeof Buffer, &nUsed, "CARTRIDGE=%s\n", Settings.CartridgePath);
	AppendLine (Buffer, sizeof Buffer, &nUsed, "CASSETTE=%s\n", Settings.CassettePath);
	AppendLine (Buffer, sizeof Buffer, &nUsed, "CASSETTE_READONLY=%d\n",
		    Settings.bCassetteReadOnly ? 1 : 0);

	AppendLine (Buffer, sizeof Buffer, &nUsed, "USER_DIR=%s\n", Settings.UserDirectory);
	AppendLine (Buffer, sizeof Buffer, &nUsed, "SYSTEM_DIR=%s\n", Settings.SystemDirectory);

	pFile = ATARI_VFS_Open (pPath, "wb");
	if (pFile == 0)
	{
		CLogger::Get ()->Write (FromSettings, LogError,
					"Cannot write settings to %s", pPath);
		return FALSE;
	}

	if (ATARI_VFS_Write (Buffer, 1, nUsed, pFile) != nUsed)
	{
		CLogger::Get ()->Write (FromSettings, LogError,
					"Settings were not written completely to %s", pPath);
		ATARI_VFS_Close (pFile);
		return FALSE;
	}

	if (ATARI_VFS_Close (pFile) != 0)
	{
		CLogger::Get ()->Write (FromSettings, LogError,
					"Settings file %s did not close cleanly", pPath);
		return FALSE;
	}

	return TRUE;
}
