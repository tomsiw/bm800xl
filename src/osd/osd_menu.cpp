/*
 * osd_menu.cpp -- the on-screen menu.  See osd_menu.h for what it is for and
 * why it is written as a state machine rather than a table.
 *
 * The layout is a simple list: a title bar, a column of "Label   Value" rows
 * with the selection highlighted, and a line of key hints at the bottom.  The
 * picture keeps running behind a dimmed panel so that the user can see what
 * they are changing.
 */

#include "osd_menu.h"

#include <circle/logger.h>

#include "bootlog.h"

#include <stdio.h>
#include <string.h>

// How the menu is laid out.  These are pixels in the framebuffer.
//
// The menu is drawn over the *picture*, not over the framebuffer.  The picture
// is a 4:3 rectangle inside a wider display -- 960x720 centred in 1280x720 --
// so it starts at x=160, and margins measured from the framebuffer's corner put
// the menu in the black bar beside the picture.  These values are the default
// position for a display where the picture fills the framebuffer; the real
// position is computed from the video geometry in Initialize(), which is why
// they are variables rather than constants.
static unsigned MenuMarginX		= 40;
static unsigned MenuMarginY		= 30;
static const unsigned MenuPanelWidth	= 900;
static const unsigned MenuPanelHeight	= 480;
static const unsigned MenuIndent	= 16;
static const unsigned MenuLabelColumn	= 450;	// x offset of the value column

// Longest text the menu builds on the stack.
#define MENU_TEXT_MAX 128

// How many frames a transient message stays on screen (about two seconds).
#define MESSAGE_FRAMES 120

// Numbers of entries on each screen.  All five switch statements keyed on
// m_Screen have to agree with these, so they are named rather than repeated.
static const unsigned MainItemCount		= 12;
static const unsigned DiskItemCount		= SETTINGS_MAX_DRIVES * 2;	// insert and eject
static const unsigned CartridgeItemCount	= 2;	// insert, eject
static const unsigned CassetteItemCount		= 2;	// insert, eject
static const unsigned JoystickItemCount		= 2;	// interface, swap ports
static const unsigned MachineItemCount		= 3;	// machine, RAM, restore defaults
static const unsigned VideoItemCount		= 4;	// standard, artifacting, resolution, aspect
static const unsigned SoundItemCount		= 3;	// enable, volume, stereo
static const unsigned SnapshotItemCount		= 4;	// save, save new, load, erase

static const char FromMenu[] = "menu";

/*
 * Copies SOURCE into DESTINATION, always terminating and never overflowing.
 *
 * strncpy is not used because it neither terminates when it truncates nor
 * tolerates a source that fills the buffer exactly, and the compiler is right
 * to warn about it here.
 */
static void CopyText (char *pDestination, const char *pSource, unsigned nSize)
{
	if (pDestination == 0 || nSize == 0)
	{
		return;
	}

	unsigned nLength = 0;
	if (pSource != 0)
	{
		nLength = strlen (pSource);
		if (nLength > nSize - 1)
		{
			nLength = nSize - 1;
		}
		memcpy (pDestination, pSource, nLength);
	}
	pDestination[nLength] = '\0';
}

CAtariMenu::CAtariMenu (void)
:	m_pVideo (0),
	m_pStorage (0),
	m_pHost (0),
	m_pSettings (0),
	m_bOpen (FALSE),
	m_Screen (ScreenMain),
	m_nSelected (0),
	m_BrowseTarget (BrowseNone),
	m_nBrowseTop (0),
	m_nBrowserSelected (0),
	m_nEraseFrames (0),
	m_nMessageFrames (0),
	m_nLineHeight (16),
	m_nRowPitch (26),
	m_nHeaderHeight (40),
	m_nFooterHeight (34),
	m_nItemTop (60),
	m_nStatusOffset (60),
	m_nMessageOffset (78),
	m_nVisibleRows (20)
{
	m_SettingsPath[0] = '\0';
	m_BrowseDirectory[0] = '\0';
	m_BrowseFilter[0] = '\0';
	m_ErasePath[0] = '\0';
	m_Message[0] = '\0';
}

CAtariMenu::~CAtariMenu (void)
{
}

void CAtariMenu::Initialize (CAtariVideoOut *pVideo, CAtariStorage *pStorage,
			     CAtariMenuHost *pHost, TAtariSettings *pSettings,
			     const char *pSettingsPath)
{
	m_pVideo = pVideo;
	m_pStorage = pStorage;
	m_pHost = pHost;
	m_pSettings = pSettings;

	if (pSettingsPath != 0)
	{
		CopyText (m_SettingsPath, pSettingsPath, sizeof m_SettingsPath);
	}

	/*
	 * The font decides the vertical rhythm: one line plus a gap is a row, the
	 * header and footer bands hold a line with room around it, and the status
	 * and message lines sit above the footer.  Deriving these from the line
	 * height is what lets the OSD use a different font -- Circle's 8x8 instead
	 * of 8x16 -- without the panel looking empty or the text overlapping.
	 */
	unsigned nLine = pVideo != 0 ? pVideo->GetTextHeight () : 16;

	m_nLineHeight    = nLine;
	m_nRowPitch      = nLine + 8;
	m_nHeaderHeight  = nLine + 18;
	m_nFooterHeight  = nLine + 16;
	m_nItemTop       = m_nHeaderHeight + 8;
	m_nStatusOffset  = m_nFooterHeight + 16;
	m_nMessageOffset = m_nStatusOffset + 14;

	/*
	 * How many list rows fit between the header and the message line.  This
	 * used to be a constant (20), which was only true while the font was small:
	 * with the old 16 pixel font twenty rows ran straight past the bottom of the
	 * panel, and with a taller font they would climb into the status line.  The
	 * limit is the message line, because a transient message is drawn over the
	 * last row if the list reaches it.
	 */
	unsigned nAvailable = MenuPanelHeight > m_nItemTop + m_nMessageOffset
			    ? MenuPanelHeight - m_nItemTop - m_nMessageOffset : 0;

	m_nVisibleRows = nAvailable / m_nRowPitch;
	if (m_nVisibleRows == 0)
	{
		m_nVisibleRows = 1;		// never a listing that cannot show a row
	}

	/*
	 * Centre the menu on the picture.
	 *
	 * The picture does not fill the framebuffer: it is a 4:3 rectangle centred
	 * inside it, so on a 1280x720 display it occupies x=160..1120.  Margins
	 * measured from the framebuffer's corner therefore draw the menu over the
	 * left black bar, which is what "moved to the left top corner" was.
	 *
	 * Clamped at the plain margins so that a display where the picture does fill
	 * the framebuffer -- or a panel too small for the menu -- still looks right.
	 */
	Layout ();
}

/*
 * Positions the panel from the picture rectangle.
 *
 * Called at start-up and whenever the picture changes shape -- the aspect ratio
 * is applied live, and an integer mode makes the picture narrower than the panel,
 * so a stale position would leave the menu hanging over the black border.
 */
void CAtariMenu::Layout (void)
{
	if (m_pVideo != 0)
	{
		unsigned nPictureLeft = m_pVideo->GetPictureLeft ();
		unsigned nPictureWidth = m_pVideo->GetPictureWidth ();
		unsigned nPictureTop = m_pVideo->GetPictureTop ();
		unsigned nPictureHeight = m_pVideo->GetPictureHeight ();

		/*
		 * Centre the panel in the picture, and put it against the picture's
		 * edge when it is too big to be centred.
		 *
		 * The margin used to be compared against MenuMarginX, which is an
		 * absolute position rather than a margin: at 720p the picture is 960
		 * wide, a 900-wide panel needs a 30-pixel margin, 30 is less than the
		 * default 40, and the comparison fell through to "no margin at all" --
		 * leaving the panel flush with the left edge of the picture and 30
		 * pixels short on the right.  That is the offset that looked like a
		 * panel leaning left.  1080p has a wide enough margin that the fault
		 * never showed there.
		 *
		 * A panel WIDER than the picture is centred on the picture's centre, not
		 * pushed against its left edge: an integer scale makes the picture
		 * narrower than 900 pixels, and aligning to the left edge moved the whole
		 * menu to the right of the screen.  The picture is centred in the
		 * framebuffer, so its centre is the framebuffer's, and the panel lands
		 * where it does in every other mode.  The margin is computed signed for
		 * that, and floored at zero so it cannot step off the left edge.
		 */
		int nMarginX = (int) nPictureLeft
			     + ((int) nPictureWidth - (int) MenuPanelWidth) / 2;
		if (nMarginX < 0)
		{
			nMarginX = 0;
		}
		MenuMarginX = (unsigned) nMarginX;

		int nMarginY = (int) nPictureTop
			     + ((int) nPictureHeight - (int) MenuPanelHeight) / 2;
		if (nMarginY < 0)
		{
			nMarginY = 0;
		}
		MenuMarginY = (unsigned) nMarginY;
	}
}

void CAtariMenu::Toggle (void)
{
	if (m_bOpen)
	{
		ExitMenu (ExitClose);
		return;
	}

	m_bOpen = TRUE;
	OpenScreen (ScreenMain);

	/*
	 * The OSD's presence changes how the frame is paced -- while it is open the
	 * picture is redrawn on a reduced cadence, and with STATIC_OSD it is not
	 * redrawn at all -- so the log says when it opened.  Closing is logged in
	 * Close(), which every route out of the menu goes through.
	 */
	CBootLog::Get ()->RecordRuntime ("OSD: opened");
}

void CAtariMenu::ExitMenu (TMenuExit Action)
{
	/*
	 * A tape that needs the other SIO mode is resolved here, whatever the user
	 * asked for: see the declaration.  The rebuild replaces the requested
	 * action, because none of them can change the mode.
	 */
	if (m_pHost != 0 && m_pHost->CassetteModeMismatch ())
	{
		Action = ExitPowerCycle;
	}

	switch (Action)
	{
	case ExitPowerCycle:
		/*
		 * The host logs the rebuild and why; see ATARI_PowerCycle().  Its
		 * outcome has to be reported before the menu closes, or a failure would
		 * leave a closed menu over a machine that did not restart.
		 */
		if (m_pHost != 0 && !m_pHost->PowerCycle ())
		{
			snprintf (m_Message, sizeof m_Message, "Could not restart: %s",
				  m_pHost->GetLastError ());
			m_nMessageFrames = MESSAGE_FRAMES;

			return;		// stay in the menu
		}
		break;

	case ExitColdReset:
		if (m_pHost != 0)
		{
			m_pHost->ColdReset ();
		}
		break;

	case ExitWarmReset:
		if (m_pHost != 0)
		{
			m_pHost->WarmReset ();
		}
		break;

	case ExitClose:
	default:
		break;
	}

	Close ();
}

void CAtariMenu::Close (void)
{
	m_bOpen = FALSE;
	m_Screen = ScreenMain;
	m_nSelected = 0;
	m_BrowseTarget = BrowseNone;
	ClearPendingErase (FALSE);

	/*
	 * The panel is drawn over the black margins when the picture is narrower than
	 * it is -- which every integer scale makes it -- and those margins are
	 * otherwise written once and forgotten.  Asking for them again is what stops
	 * the strips where the panel was from staying on screen after the menu goes.
	 */
	if (m_pVideo != 0)
	{
		m_pVideo->InvalidatePadding ();
	}

	/*
	 * Logged here rather than in each exit: F12, Escape, Close menu, the three
	 * reset items and the fallback key path all end up in this one function, so
	 * this is the only place that can say the OSD closed whichever way it was
	 * left.  Nothing calls Close() during start-up, so a closed line always has
	 * its opened line before it.
	 */
	CBootLog::Get ()->RecordRuntime ("OSD: closed");
}

void CAtariMenu::OpenScreen (TMenuScreen Screen)
{
	m_Screen = Screen;
	m_nSelected = 0;
	m_nBrowseTop = 0;
	m_nBrowserSelected = 0;
}

/* ---------------------------------------------------------------------------
 * Item descriptions
 * ------------------------------------------------------------------------- */

unsigned CAtariMenu::GetItemCount (void) const
{
	switch (m_Screen)
	{
	case ScreenMain:
		return MainItemCount;
	case ScreenDisks:
		return DiskItemCount;
	case ScreenCartridge:
		return CartridgeItemCount;
	case ScreenCassette:
		return CassetteItemCount;
	case ScreenJoysticks:
		return JoystickItemCount;
	case ScreenMachine:
		return MachineItemCount;
	case ScreenVideo:
		return VideoItemCount;
	case ScreenSound:
		return SoundItemCount;
	case ScreenSnapshots:
		return SnapshotItemCount;
	default:
		return 0;
	}
}

unsigned CAtariMenu::TextColumns (unsigned nPixels) const
{
	unsigned nCharWidth = m_pVideo != 0 ? m_pVideo->GetTextWidth () : 8;

	if (nCharWidth == 0)
	{
		nCharWidth = 8;
	}

	return nPixels / nCharWidth;
}

void CAtariMenu::Truncate (char *pText, unsigned nMaxChars)
{
	if (pText == 0)
	{
		return;
	}

	unsigned nLength = strlen (pText);

	if (nLength > nMaxChars)
	{
		pText[nMaxChars] = '\0';
	}
}

boolean CAtariMenu::SamePath (const char *pA, const char *pB)
{
	if (pA == 0 || pB == 0)
	{
		return FALSE;
	}

	for (unsigned i = 0; ; i++)
	{
		char a = pA[i];
		char b = pB[i];

		if (a >= 'a' && a <= 'z')
		{
			a = (char) (a - 'a' + 'A');
		}
		if (b >= 'a' && b <= 'z')
		{
			b = (char) (b - 'a' + 'A');
		}

		if (a != b)
		{
			return FALSE;
		}
		if (a == '\0')
		{
			return TRUE;
		}
	}
}

boolean CAtariMenu::IsWithinUserBase (const char *pPath) const
{
	if (m_pStorage == 0 || pPath == 0)
	{
		return FALSE;
	}

	const char *pBase = m_pStorage->GetUserBase ();
	unsigned nBase = strlen (pBase);

	if (strlen (pPath) < nBase)
	{
		return FALSE;
	}

	char Prefix[SETTINGS_PATH_MAX];
	CopyText (Prefix, pPath, nBase + 1);

	if (!SamePath (Prefix, pBase))
	{
		return FALSE;
	}

	// The base itself, or something below it -- not a sibling whose name merely
	// begins with the same characters, such as "SD:/USERDATA2".
	return pPath[nBase] == '\0' || pPath[nBase] == '/';
}

const char *CAtariMenu::GetItemLabel (unsigned nIndex) const
{
	switch (m_Screen)
	{
	case ScreenMain:
		switch (nIndex)
		{
		case 0: return "Disk drives";
		case 1: return "Cartridges";
		case 2: return "Cassettes";
		case 3: return "Joysticks";
		case 4: return "Machine and memory";
		case 5: return "Screen";
		case 6: return "Sound";
		case 7: return "Save states";
		case 8: return "Cold reset";
		case 9: return "Warm reset";
		case 10: return "Power cycle";
		case 11: return "Close menu";
		}
		break;

	case ScreenDisks:
		{
			// The rows are interleaved: an insert row and an eject row for
			// each drive, so ejecting never means loading another image
			// first to get a row to press Delete on.
			static const char *Names[SETTINGS_MAX_DRIVES] =
				{ "Drive 1", "Drive 2", "Drive 3", "Drive 4" };
			static const char *EjectNames[SETTINGS_MAX_DRIVES] =
				{ "Eject drive 1", "Eject drive 2",
				  "Eject drive 3", "Eject drive 4" };

			unsigned nDrive = nIndex / 2;
			if (nDrive >= SETTINGS_MAX_DRIVES)
			{
				break;
			}

			return (nIndex % 2) == 0 ? Names[nDrive] : EjectNames[nDrive];
		}
		break;

	case ScreenCartridge:
		if (nIndex == 0)
		{
			return "Cartridge";
		}
		return "Eject cartridge";

	case ScreenCassette:
		if (nIndex == 0)
		{
			return "Cassette";
		}
		return "Eject cassette";

	case ScreenJoysticks:
		switch (nIndex)
		{
		case 0: return "Interface";
		case 1: return "Swap ports";
		}
		break;

	case ScreenMachine:
		switch (nIndex)
		{
		case 0: return "Machine";
		case 1: return "Memory";
		case 2: return "Restore defaults";
		}
		break;

	case ScreenVideo:
		switch (nIndex)
		{
		case 0: return "Video standard";
		case 1: return "Artifacting";
		case 2: return "Resolution";
		case 3: return "Aspect ratio";
		}
		break;

	case ScreenSound:
		switch (nIndex)
		{
		case 0: return "Enabled";
		case 1: return "Volume";
		case 2: return "Stereo (two POKEYs)";
		}
		break;

	case ScreenSnapshots:
		switch (nIndex)
		{
		case 0: return "Save snapshot";
		case 1: return "Save as new snapshot";
		case 2: return "Load snapshot";
		case 3: return "Erase snapshot";
		}
		break;

	default:
		break;
	}

	return "";
}

const char *CAtariMenu::MachineName (int nMachine)
{
	switch (nMachine)
	{
	case ATARI_MACHINE_XL:   return "Atari XL (600XL/800XL)";
	case ATARI_MACHINE_XE:   return "Atari XE (65XE/130XE)";
	case ATARI_MACHINE_XEGS: return "Atari XE Game System";
	default:                 return "?";
	}
}

const char *CAtariMenu::RamName (int nRamKB)
{
	switch (nRamKB)
	{
	case ATARI_RAM_KB_64:   return "64 kB";
	case ATARI_RAM_KB_128:  return "128 kB";
	case ATARI_RAM_KB_192:  return "192 kB";
	case ATARI_RAM_KB_320:  return "320 kB (RAMBO XL)";
	case ATARI_RAM_KB_320C: return "320 kB (Compy Shop)";
	default:                return "?";
	}
}

const char *CAtariMenu::VideoName (int nStandard)
{
	return nStandard == ATARI_VIDEO_PAL ? "PAL" : "NTSC";
}

const char *CAtariMenu::ArtifactName (int nArtifact)
{
	switch (nArtifact)
	{
	case ATARI_ARTIFACT_NONE:       return "Off";
	case ATARI_ARTIFACT_NTSC_OLD:   return "NTSC artifacting (old)";
	case ATARI_ARTIFACT_NTSC_NEW:   return "NTSC artifacting (new)";
	case ATARI_ARTIFACT_PAL_SIMPLE: return "PAL blending (simple)";
	case ATARI_ARTIFACT_PAL_BLEND:  return "PAL blending (accurate)";
	default:                        return "?";
	}
}

const char *CAtariMenu::KeyboardName (int nMode)
{
	switch (nMode)
	{
	case AtariKeyboardModeUSB:  return "USB keyboard";
	case AtariKeyboardModeGPIO: return "GPIO keyboard";
	default:                    return "?";
	}
}

const char *CAtariMenu::JoystickInterfaceName (int nInterface)
{
	switch (nInterface)
	{
	case AtariJoystickInterfaceUSB:  return "USB gamepads";
	case AtariJoystickInterfaceGPIO: return "9-pin GPIO ports";
	case AtariJoystickInterfaceKeyboard: return "USB keyboard keys";
	default:                         return "?";
	}
}

/*
 * Whether this scale fits the framebuffer that was actually created.
 *
 * Asked of the video layer rather than of the RESOLUTION setting, because a
 * display that refuses 1080p falls back to 720p, and 4x would not fit there.
 */
boolean CAtariMenu::FitsAspect (int nAspect) const
{
	if (m_pVideo == 0 || nAspect < 2)
	{
		return TRUE;		/* the 4:3 fit always fits */
	}

	return    m_pVideo->GetWidth ()  >= (unsigned) (336 * nAspect)
	       && m_pVideo->GetHeight () >= (unsigned) (240 * nAspect);
}
const char *CAtariMenu::AspectName (int nAspect)
{
	switch (nAspect)
	{
	case AtariAspect2x: return "2x";
	case AtariAspect3x: return "3x";
	case AtariAspect4x: return "4x";
	default:            return "4:3";
	}
}

const char *CAtariMenu::ResolutionName (int nResolution)
{
	switch (nResolution)
	{
	case AtariResolution720p:  return "1280x720";
	case AtariResolution1080p: return "1920x1080";
	default:                   return "?";
	}
}

/* The last path component, for showing a file name instead of a full path. */
const char *CAtariMenu::BaseName (const char *pPath)
{
	const char *pSlash;

	if (pPath == 0 || pPath[0] == '\0')
	{
		return "(none)";
	}

	pSlash = strrchr (pPath, '/');
	return pSlash != 0 ? pSlash + 1 : pPath;
}

const char *CAtariMenu::GetItemValue (unsigned nIndex) const
{
	static char Text[MENU_TEXT_MAX];
	const TAtariSettings &S = *m_pSettings;

	switch (m_Screen)
	{
	case ScreenMain:
		switch (nIndex)
		{
		case 0:
			{
				unsigned nMounted = 0;
				for (int i = 0; i < SETTINGS_MAX_DRIVES; i++)
				{
					if (S.DiskPath[i][0] != '\0')
					{
						nMounted++;
					}
				}
				snprintf (Text, sizeof Text, "%u of %d loaded",
					  nMounted, SETTINGS_MAX_DRIVES);
				return Text;
			}
		case 1:
			return BaseName (S.CartridgePath);
		case 2:
			return BaseName (S.CassettePath);
		case 3:
			snprintf (Text, sizeof Text, "%s%s",
				  JoystickInterfaceName (S.nJoystickInterface),
				  S.bSwapJoystickPorts ? ", swapped" : "");
			return Text;
		case 4:
			snprintf (Text, sizeof Text, "%s, %s",
				  MachineName (S.nMachine), RamName (S.nRamKB));
			return Text;
		case 5:
			snprintf (Text, sizeof Text, "%s, %s, %s",
				  VideoName (S.nVideoStandard),
				  ResolutionName (S.nResolution),
				  AspectName (S.nAspect));
			return Text;
		case 6:
			if (!S.bSoundEnabled)
			{
				return "Off";
			}
			snprintf (Text, sizeof Text, "%d%%%s", S.nSoundVolume,
				  S.bSoundStereo ? ", stereo" : "");
			return Text;
		case 7:
			return "Save / load / erase";
		}
		break;

	case ScreenDisks:
		{
			unsigned nDrive = nIndex / 2;
			if (nDrive >= SETTINGS_MAX_DRIVES)
			{
				break;
			}

			// Only the insert row shows what is mounted; the eject row acts
			// on the same drive, so repeating the name would be noise.
			if ((nIndex % 2) != 0)
			{
				return "";
			}

			if (S.DiskPath[nDrive][0] == '\0')
			{
				return "(empty)";
			}
			snprintf (Text, sizeof Text, "%s%s",
				  BaseName (S.DiskPath[nDrive]),
				  S.DiskReadOnly[nDrive] ? "  [ro]" : "");
			return Text;
		}
		break;

	case ScreenCartridge:
		if (nIndex == 0)
		{
			return BaseName (S.CartridgePath);
		}
		return "";

	case ScreenCassette:
		// Shown like the disks: the name, and the protection state, because
		// Left/Right changes it and the row is the only place that says so.
		if (nIndex != 0)
		{
			return "";
		}
		if (S.CassettePath[0] == '\0')
		{
			return "(empty)";
		}
		snprintf (Text, sizeof Text, "%s%s",
			  BaseName (S.CassettePath),
			  S.bCassetteReadOnly ? "  [ro]" : "");
		return Text;

	case ScreenJoysticks:
		switch (nIndex)
		{
		case 0: return JoystickInterfaceName (S.nJoystickInterface);
		case 1: return S.bSwapJoystickPorts ? "On" : "Off";
		}
		break;

	case ScreenMachine:
		switch (nIndex)
		{
		case 0: return MachineName (S.nMachine);
		case 1: return RamName (S.nRamKB);
		case 2: return "press to reset";
		}
		break;

	case ScreenVideo:
		switch (nIndex)
		{
		case 0: return VideoName (S.nVideoStandard);
		case 1: return ArtifactName (S.nArtifact);
		case 2: return ResolutionName (S.nResolution);
		case 3: return AspectName (S.nAspect);
		}
		break;

	case ScreenSound:
		switch (nIndex)
		{
		case 0: return S.bSoundEnabled ? "On" : "Off";
		case 1:
			snprintf (Text, sizeof Text, "%d%%", S.nSoundVolume);
			return Text;
		case 2: return S.bSoundStereo ? "On" : "Off";
		}
		break;

	case ScreenSnapshots:
		/*
		 * The snapshot screens carry no value column: every entry either acts
		 * immediately or opens the browser, so there is nothing to show.
		 */
		return "";

	default:
		break;
	}

	return "";
}

/* ---------------------------------------------------------------------------
 * Navigation
 * ------------------------------------------------------------------------- */

void CAtariMenu::MoveSelection (int nDelta)
{
	unsigned nCount = GetItemCount ();

	if (nCount == 0)
	{
		return;
	}

	int nSelected = (int) m_nSelected + nDelta;

	// Wrap, so the user never gets stuck at an end of the list.
	while (nSelected < 0)
	{
		nSelected += (int) nCount;
	}
	while (nSelected >= (int) nCount)
	{
		nSelected -= (int) nCount;
	}

	m_nSelected = (unsigned) nSelected;
}

/*
 * Left and right change the value of the selected item, where the item has a
 * value that can be stepped.  Actions ignore this.
 */
/*
 * Steps the selected row.
 *
 * NDELTA is -1 for Left and +1 for Right, and every row that offers more than
 * two values has to step by it: Right goes to the next value, Left to the
 * previous one.  A switch of fixed cases can only ever go one way, which is the
 * fault that made Left advance instead of retreat on the aspect ratio and the
 * joystick interface -- so those rows walk an array and step the index, and any
 * row added later should do the same.
 *
 * Two-valued rows -- video standard, resolution, sound on and off, stereo,
 * write protection, port swap -- are toggles, where next and previous are the
 * same value and the direction has no meaning.
 */
void CAtariMenu::AdjustSetting (int nDelta)
{
	TAtariSettings &S = *m_pSettings;
	boolean bChanged = TRUE;

	switch (m_Screen)
	{
	case ScreenMachine:
		if (m_nSelected == 0)
		{
			int nValue = S.nMachine + nDelta;
			if (nValue < 0) nValue = 2;
			if (nValue > 2) nValue = 0;
			S.nMachine = nValue;
		}
		else if (m_nSelected == 1)
		{
			// Only the sizes the emulation core actually implements.  There
			// is deliberately no 256 kB: the core has no such configuration.
			static const int Sizes[] =
				{ ATARI_RAM_KB_64, ATARI_RAM_KB_128, ATARI_RAM_KB_192,
				  ATARI_RAM_KB_320, ATARI_RAM_KB_320C };
			const int nCount = (int) (sizeof Sizes / sizeof Sizes[0]);

			int nIndex = 0;
			for (int i = 0; i < nCount; i++)
			{
				if (Sizes[i] == S.nRamKB)
				{
					nIndex = i;
					break;
				}
			}
			nIndex = (nIndex + nDelta + nCount) % nCount;
			S.nRamKB = Sizes[nIndex];
		}
		else
		{
			// "Restore defaults" is an action, not a value to step.
			bChanged = FALSE;
		}
		break;

	case ScreenVideo:
		if (m_nSelected == 0)
		{
			S.nVideoStandard = S.nVideoStandard == ATARI_VIDEO_PAL
					 ? ATARI_VIDEO_NTSC : ATARI_VIDEO_PAL;
		}
		else if (m_nSelected == 1)
		{
			int nValue = S.nArtifact + nDelta;
			if (nValue < ATARI_ARTIFACT_NONE) nValue = ATARI_ARTIFACT_PAL_BLEND;
			if (nValue > ATARI_ARTIFACT_PAL_BLEND) nValue = ATARI_ARTIFACT_NONE;
			S.nArtifact = nValue;
		}
		else if (m_nSelected == 2)
		{
			/*
			 * The output mode is chosen when the framebuffer is created, which
			 * is before the emulator is built -- changing it means tearing the
			 * framebuffer down and building another, with the display
			 * renegotiating its mode underneath a running machine.  So the
			 * setting is written here and applied by the next start, and the
			 * menu says as much instead of leaving the row looking as if it
			 * had taken effect.
			 */
			S.nResolution = S.nResolution == AtariResolution720p
					? AtariResolution1080p : AtariResolution720p;

			snprintf (m_Message, sizeof m_Message,
				  "%s: switch the Pi off and on",
				  ResolutionName (S.nResolution));
			m_nMessageFrames = MESSAGE_FRAMES;
		}
		else
		{
			/*
			 * The aspect ratio is live: the same framebuffer, drawn in a
			 * different rectangle, so the geometry is recomputed once and the
			 * next frame uses it.  The panel is re-laid out because an integer
			 * mode makes the picture narrower than the panel is.
			 */
			/*
			 * Walked as a list rather than switched, so that Right goes forward
			 * and Left goes back; a switch of cases can only ever go one way.
			 *
			 * 4x is 1344x960, so it exists only on a display that can hold it --
			 * in practice 1080p.  It is left OUT of the list rather than offered
			 * and reduced, because a row that reads 4x while the picture is 3x is
			 * worse than a row that does not offer it.
			 */
			static const int Aspects[] =
				{ AtariAspect43, AtariAspect2x, AtariAspect3x, AtariAspect4x };
			const int nAspectCount = FitsAspect (AtariAspect4x) ? 4 : 3;

			int nIndex = 0;
			for (int i = 0; i < nAspectCount; i++)
			{
				if (Aspects[i] == S.nAspect)
				{
					nIndex = i;
					break;
				}
			}
			if (nIndex >= nAspectCount)
			{
				nIndex = 0;
			}

			nIndex = (nIndex + nDelta + nAspectCount) % nAspectCount;
			S.nAspect = Aspects[nIndex];

			if (m_pVideo != 0)
			{
				/*
				 * No re-layout: the picture is centred in the framebuffer in
				 * every mode, so the panel's position does not depend on the
				 * aspect.  Moving the panel about when nothing moved would be
				 * visible for no reason.
				 */
				m_pVideo->SetAspect (S.nAspect);

				/*
				 * A factor the framebuffer cannot hold is reduced, and the row
				 * would then show a mode that is not the one in use -- so say
				 * which one it is.
				 */
				if (m_pVideo->GetPixelScale () != (unsigned) S.nAspect)
				{
					snprintf (m_Message, sizeof m_Message,
						  "%s does not fit; using %ux",
						  AspectName (S.nAspect),
						  m_pVideo->GetPixelScale ());
					m_nMessageFrames = MESSAGE_FRAMES;
				}
			}
		}
		break;

	case ScreenSound:
		if (m_nSelected == 0)
		{
			S.bSoundEnabled = S.bSoundEnabled ? FALSE : TRUE;
		}
		else if (m_nSelected == 1)
		{
			int nValue = S.nSoundVolume + nDelta * 5;
			if (nValue < 0) nValue = 0;
			if (nValue > 100) nValue = 100;
			S.nSoundVolume = nValue;
		}
		else
		{
			S.bSoundStereo = S.bSoundStereo ? FALSE : TRUE;
		}
		break;

	case ScreenDisks:
		{
			/*
			 * Left/Right toggles the read-only flag of the drive the
			 * selection is on.  Only the insert rows carry that flag; the
			 * eject rows are actions, and there is nothing to step.
			 */
			if ((m_nSelected % 2) != 0)
			{
				bChanged = FALSE;
				break;
			}

			unsigned nDrive = m_nSelected / 2;
			if (nDrive >= SETTINGS_MAX_DRIVES
			    || S.DiskPath[nDrive][0] == '\0')
			{
				bChanged = FALSE;
				break;
			}
			S.DiskReadOnly[nDrive] = S.DiskReadOnly[nDrive] ? FALSE : TRUE;

			/*
			 * Apply it now.  Read-only is decided when the image file is
			 * opened, so an open drive can never be changed: it has to be
			 * re-opened for the new mode to reach the machine.  Without this
			 * the row said one thing and the drive did another, until the disk
			 * happened to be inserted again.
			 */
			if (   m_pHost == 0
			    || !m_pHost->MountDisk (nDrive + 1, S.DiskPath[nDrive],
						    S.DiskReadOnly[nDrive]))
			{
				snprintf (m_Message, sizeof m_Message, "Could not re-open: %s",
					  m_pHost != 0 ? m_pHost->GetLastError ()
						       : "the emulator is not available");
				m_nMessageFrames = MESSAGE_FRAMES;

				// The drive did not change, so neither should the row.
				S.DiskReadOnly[nDrive] = S.DiskReadOnly[nDrive] ? FALSE : TRUE;
				bChanged = FALSE;
			}
		}
		break;

	case ScreenCartridge:
		/*
		 * A cartridge has no flag -- it is ROM -- so the row reports that
		 * rather than silently doing nothing.
		 */
		bChanged = FALSE;
		if (m_nSelected == 0)
		{
			snprintf (m_Message, sizeof m_Message,
				  "A cartridge is read-only by nature");
			m_nMessageFrames = MESSAGE_FRAMES;
		}
		break;

	case ScreenCassette:
		// Write protection is a flag of the cassette, so it toggles on the
		// insert row; the eject row is an action.
		if (m_nSelected == 0)
		{
			S.bCassetteReadOnly = S.bCassetteReadOnly ? FALSE : TRUE;

			/*
			 * Re-mounting would rewind the tape, so the port changes the
			 * protection in place; the tape keeps its position.
			 */
			if (   m_pHost == 0
			    || !m_pHost->SetCassetteWriteProtect (S.bCassetteReadOnly))
			{
				snprintf (m_Message, sizeof m_Message, "Could not apply: %s",
					  m_pHost != 0 ? m_pHost->GetLastError ()
						       : "the emulator is not available");
				m_nMessageFrames = MESSAGE_FRAMES;

				S.bCassetteReadOnly = S.bCassetteReadOnly ? FALSE : TRUE;
				bChanged = FALSE;
			}
		}
		else
		{
			bChanged = FALSE;
		}
		break;

	case ScreenJoysticks:
		if (m_nSelected == 0)
		{
			/*
			 * The interface is fixed when the controllers are created, which is
			 * before the emulator runs: the USB drivers are cached in the factory
			 * and the GPIO ports claim header pins.  So the choice is stored and
			 * the next start applies it, and the menu says so rather than leaving
			 * the row looking as if it had taken effect.
			 */
			static const int Interfaces[] =
			{
				AtariJoystickInterfaceUSB,
				AtariJoystickInterfaceGPIO,
				AtariJoystickInterfaceKeyboard
			};
			const int nInterfaceCount =
				(int) (sizeof Interfaces / sizeof Interfaces[0]);

			int nIndex = 0;
			for (int i = 0; i < nInterfaceCount; i++)
			{
				if (Interfaces[i] == S.nJoystickInterface)
				{
					nIndex = i;
					break;
				}
			}

			nIndex = (nIndex + nDelta + nInterfaceCount) % nInterfaceCount;
			S.nJoystickInterface = Interfaces[nIndex];

			snprintf (m_Message, sizeof m_Message,
				  "%s: switch the Pi off and on",
				  JoystickInterfaceName (S.nJoystickInterface));
			m_nMessageFrames = MESSAGE_FRAMES;
		}
		else
		{
			/*
			 * Swapping is applied at once: it is only a question of which
			 * controller the platform layer finds in which port, so the two are
			 * re-bound rather than recreated.
			 */
			S.bSwapJoystickPorts = S.bSwapJoystickPorts ? FALSE : TRUE;

			if (m_pHost != 0)
			{
				m_pHost->SwapJoystickPorts ();
			}

			snprintf (m_Message, sizeof m_Message, "Ports %s",
				  S.bSwapJoystickPorts ? "swapped" : "back to normal");
			m_nMessageFrames = MESSAGE_FRAMES;
		}
		break;

	default:
		bChanged = FALSE;
		break;
	}

	if (bChanged)
	{
		if (m_pHost != 0 && !m_pHost->ApplySettings (S))
		{
			snprintf (m_Message, sizeof m_Message, "Could not apply: %s",
				  m_pHost->GetLastError ());
			m_nMessageFrames = MESSAGE_FRAMES;
		}
		SaveSettings ();
	}
}

void CAtariMenu::SaveNewSnapshot (void)
{
	if (m_pHost == 0 || m_pStorage == 0)
	{
		return;
	}

	/*
	 * Saves under the first unused SNAPSHOTn.SAV.
	 *
	 * The browser can only offer files that already exist, so on a fresh card
	 * it has nothing to offer and the first snapshot could never be created.
	 * This item is the way out of that: a name is chosen here rather than typed,
	 * because there is no text entry in the menu and adding one would need a
	 * full key-entry widget for a once-per-card operation.
	 */
	char Name[STORAGE_NAME_MAX];
	char Path[SETTINGS_PATH_MAX];

	for (unsigned n = 1; n <= 99; n++)
	{
		snprintf (Name, sizeof Name, "SNAPSHOT%u.SAV", n);
		m_pStorage->MakeSystemPath (Path, sizeof Path, Name);

		if (!m_pStorage->FileExists (Path))
		{
			if (m_pHost->SaveSnapshot (Path))
			{
				snprintf (m_Message, sizeof m_Message, "Saved as %s", Name);
			}
			else
			{
				snprintf (m_Message, sizeof m_Message, "Could not save: %s",
					  m_pHost->GetLastError ());
			}
			m_nMessageFrames = MESSAGE_FRAMES;
			return;
		}
	}

	snprintf (m_Message, sizeof m_Message,
		  "SNAPSHOT1.SAV to SNAPSHOT99.SAV are all in use");
	m_nMessageFrames = MESSAGE_FRAMES;
}

void CAtariMenu::EjectSelection (void)
{
	if (m_pHost == 0 || m_pSettings == 0)
	{
		return;
	}

	TAtariSettings &S = *m_pSettings;
	const char *pWhat = 0;

	switch (m_Screen)
	{
	case ScreenDisks:
		{
			// Both rows of a drive pair name the same drive, so Delete on
			// either of them ejects it.
			unsigned nDrive = m_nSelected / 2;
			if (nDrive >= SETTINGS_MAX_DRIVES)
			{
				return;
			}

			if (S.DiskPath[nDrive][0] == '\0')
			{
				pWhat = "That drive is already empty";
				break;
			}

			// An empty path is what the host reads as "eject this drive".
			m_pHost->MountDisk (nDrive + 1, "",
					    S.DiskReadOnly[nDrive]);
			S.DiskPath[nDrive][0] = '\0';

			/*
			 * The read-only choice stays with the drive, not with the image:
			 * ejecting and inserting the same disk again keeps the mode it was
			 * given, and inserting a different one starts the way the drive was
			 * left.  It is shown on the row, and Left/Right changes it.
			 */
			pWhat = "Drive ejected";
		}
		break;

	case ScreenCartridge:
		if (S.CartridgePath[0] == '\0')
		{
			pWhat = "No cartridge is loaded";
			break;
		}
		m_pHost->MountCartridge ("");
		S.CartridgePath[0] = '\0';
		pWhat = "Cartridge removed";
		break;

	case ScreenCassette:
		if (S.CassettePath[0] == '\0')
		{
			pWhat = "No cassette is loaded";
			break;
		}
		m_pHost->MountCassette ("", S.bCassetteReadOnly);
		S.CassettePath[0] = '\0';
		pWhat = "Cassette ejected";
		break;

	default:
		return;
	}

	SaveSettings ();

	snprintf (m_Message, sizeof m_Message, "%s", pWhat);
	m_nMessageFrames = MESSAGE_FRAMES;
}

void CAtariMenu::ActivateSelection (void)
{
	TAtariSettings &S = *m_pSettings;

	switch (m_Screen)
	{
	case ScreenMain:
		switch (m_nSelected)
		{
		case 0: OpenScreen (ScreenDisks); break;
		case 1: OpenScreen (ScreenCartridge); break;
		case 2: OpenScreen (ScreenCassette); break;
		case 3: OpenScreen (ScreenJoysticks); break;
		case 4: OpenScreen (ScreenMachine); break;
		case 5: OpenScreen (ScreenVideo); break;
		case 6: OpenScreen (ScreenSound); break;
		case 7: OpenScreen (ScreenSnapshots); break;
		case 8: ExitMenu (ExitColdReset); break;
		case 9: ExitMenu (ExitWarmReset); break;
		case 10: ExitMenu (ExitPowerCycle); break;
		case 11: ExitMenu (ExitClose); break;
		}
		break;

	case ScreenDisks:
		if ((m_nSelected % 2) == 0)
		{
			OpenBrowser ((TBrowseTarget) (BrowseDisk1 + m_nSelected / 2));
		}
		else
		{
			EjectSelection ();
		}
		break;

	case ScreenCartridge:
		if (m_nSelected == 0)
		{
			OpenBrowser (BrowseCartridge);
		}
		else
		{
			EjectSelection ();
		}
		break;

	case ScreenCassette:
		if (m_nSelected == 0)
		{
			OpenBrowser (BrowseCassette);
		}
		else
		{
			EjectSelection ();
		}
		break;

	case ScreenMachine:
		if (m_nSelected == 2)
		{
			// "Restore defaults" only resets the machine settings, not the
			// user's mounted media, which they would not expect to lose.
			S.nMachine = ATARI_MACHINE_XL;
			S.nRamKB = ATARI_RAM_KB_64;
			S.nVideoStandard = ATARI_VIDEO_PAL;
			S.nArtifact = ATARI_ARTIFACT_PAL_SIMPLE;

			if (m_pHost != 0)
			{
				m_pHost->ApplySettings (S);
			}
			SaveSettings ();
		}
		break;

	case ScreenSnapshots:
		switch (m_nSelected)
		{
		case 0: OpenBrowser (BrowseSnapshotSave); break;
		case 1: SaveNewSnapshot (); break;
		case 2: OpenBrowser (BrowseSnapshotLoad); break;
		case 3: OpenBrowser (BrowseSnapshotErase); break;
		}
		break;

	default:
		break;
	}
}

/* ---------------------------------------------------------------------------
 * File browser
 * ------------------------------------------------------------------------- */

CAtariMenu::TMenuScreen CAtariMenu::BrowseOrigin (TBrowseTarget Target) const
{
	switch (Target)
	{
	case BrowseCartridge:
		return ScreenCartridge;
	case BrowseCassette:
		return ScreenCassette;
	case BrowseSnapshotLoad:
	case BrowseSnapshotSave:
	case BrowseSnapshotErase:
		return ScreenSnapshots;
	case BrowseDisk1:
	case BrowseDisk2:
	case BrowseDisk3:
	case BrowseDisk4:
	default:
		return ScreenDisks;
	}
}

void CAtariMenu::OpenBrowser (TBrowseTarget Target)
{
	const char *pExtensions = 0;

	m_BrowseTarget = Target;

	switch (Target)
	{
	case BrowseDisk1:
	case BrowseDisk2:
	case BrowseDisk3:
	case BrowseDisk4:
		pExtensions = "ATR;XFD";
		break;
	case BrowseCartridge:
		pExtensions = "CAR;ROM";
		break;
	case BrowseCassette:
		pExtensions = "CAS";
		break;
	case BrowseSnapshotLoad:
	case BrowseSnapshotSave:
	case BrowseSnapshotErase:
		pExtensions = "SAV";
		break;
	default:
		break;
	}

	CopyText (m_BrowseFilter, pExtensions != 0 ? pExtensions : "",
		  sizeof m_BrowseFilter);

	if (m_pStorage == 0)
	{
		return;
	}

	// Reopen where the user was last time, if that is where this kind of file
	// lives; otherwise start at the volume root.
	if (   Target == BrowseSnapshotLoad
	    || Target == BrowseSnapshotSave
	    || Target == BrowseSnapshotErase)
	{
		CopyText (m_BrowseDirectory, m_pStorage->GetSystemDirectory (),
			  sizeof m_BrowseDirectory);
	}
	else
	{
		/*
		 * Media: reopen where the user was last time, but only inside the user
		 * volume.  A path left in the settings from the other volume -- the
		 * usual thing after switching between USB and SD by hand -- is outside
		 * it, and the browser starts at the base instead.
		 */
		const char *pStart = m_pSettings->UserDirectory;

		if (!IsWithinUserBase (pStart))
		{
			pStart = m_pStorage->GetUserBase ();
		}

		CopyText (m_BrowseDirectory, pStart, sizeof m_BrowseDirectory);
	}

	m_pStorage->RefreshUserVolume ();

	ListBrowserDirectory ();

	ClearPendingErase (FALSE);

	m_nBrowseTop = 0;
	m_nBrowserSelected = 0;
	m_Screen = ScreenBrowse;
	m_nSelected = 0;
}

/*
 * Lists m_BrowseDirectory into the storage layer.
 *
 * The directory and its volume have to be handed over separately: a listing
 * wants the volume on its own plus the directory within it.  Passing the whole
 * path as both builds "USB:USB:/disks" and opens nothing, which presents itself
 * as an empty browser rather than as an error.
 */
void CAtariMenu::ListBrowserDirectory (void)
{
	char Volume[STORAGE_VOLUME_MAX];
	char Directory[SETTINGS_PATH_MAX];

	CAtariStorage::SplitPath (m_BrowseDirectory, Volume, sizeof Volume,
				  Directory, sizeof Directory);

	// The listing shows names, not paths, so a volume root needs no leading
	// separator; everywhere else the directory is passed as-is.
	m_pStorage->ListWithDirectories (Volume, Directory, m_BrowseFilter);
}

void CAtariMenu::BrowserEnterDirectory (void)
{
	if (m_pStorage == 0)
	{
		return;
	}

	const char *pEntry = m_pStorage->GetEntry (m_nBrowserSelected);
	if (pEntry == 0 || pEntry[0] != '/')
	{
		return;
	}

	// The listing marks directories with a leading '/', which is stripped
	// again here to build the child path.
	char Name[STORAGE_NAME_MAX];
	char NewDirectory[SETTINGS_PATH_MAX];

	CopyText (Name, pEntry + 1, sizeof Name);

	CAtariStorage::MakePath (NewDirectory, sizeof NewDirectory,
				 m_BrowseDirectory, Name);

	CopyText (m_BrowseDirectory, NewDirectory, sizeof m_BrowseDirectory);

	ListBrowserDirectory ();
	ClearPendingErase (TRUE);
	m_nBrowseTop = 0;
	m_nBrowserSelected = 0;
}

/*
 * Moves the browser up one directory, or reports that it could not.
 *
 * Going up is not always possible: a volume root such as "USB:" has no parent.
 * When there is nowhere to go the caller leaves the browser rather than staying
 * put, so that Escape always does something visible.
 *
 * \return TRUE if the listing now shows the parent directory
 */
boolean CAtariMenu::BrowserGoUp (void)
{
	if (m_pStorage == 0)
	{
		return FALSE;
	}

	/*
	 * The browser never leaves the user volume.  With the SD card chosen the
	 * tree starts at SD:/USERDATA, and one step above it is the emulator's own
	 * part of the card -- settings, snapshots, ROMs -- so "up" from there has
	 * nowhere to go and Escape leaves the browser, exactly as it does from a
	 * volume root.
	 */
	if (   m_BrowseTarget == BrowseDisk1 || m_BrowseTarget == BrowseDisk2
	    || m_BrowseTarget == BrowseDisk3 || m_BrowseTarget == BrowseDisk4
	    || m_BrowseTarget == BrowseCartridge
	    || m_BrowseTarget == BrowseCassette)
	{
		if (SamePath (m_BrowseDirectory, m_pStorage->GetUserBase ()))
		{
			return FALSE;
		}
	}

	if (!CAtariStorage::TruncateToParent (m_BrowseDirectory))
	{
		return FALSE;
	}

	/*
	 * A directory that lists nothing is not somewhere the user can be: it is
	 * either a volume that failed to mount, or a directory that has since been
	 * removed.  Walk up until the listing is non-empty or the root is reached,
	 * with a bound so that a volume which never lists cannot spin here.
	 */
	for (unsigned nAttempt = 0; nAttempt < 16; nAttempt++)
	{
		ListBrowserDirectory ();
		if (m_pStorage->GetEntryCount () > 0)
		{
			break;
		}
		if (!CAtariStorage::TruncateToParent (m_BrowseDirectory))
		{
			break;
		}
	}

	ClearPendingErase (TRUE);
	m_nBrowseTop = 0;
	m_nBrowserSelected = 0;

	return TRUE;
}

void CAtariMenu::BrowserChoose (void)
{
	if (m_pStorage == 0)
	{
		return;
	}

	const char *pEntry = m_pStorage->GetEntry (m_nBrowserSelected);
	if (pEntry == 0)
	{
		return;
	}

	/*
	 * The erase screen is the one place where choosing a file does not mount or
	 * load it, so it is handled before the rest: the confirmation it needs is
	 * the same state the Delete key uses, and BrowserErase() owns it.
	 */
	if (   m_BrowseTarget == BrowseSnapshotErase
	    && pEntry[0] != '/')
	{
		BrowserErase ();

		return;
	}

	if (m_pHost == 0)
	{
		return;
	}

	ClearPendingErase (TRUE);

	if (pEntry[0] == '/')
	{
		BrowserEnterDirectory ();
		return;
	}

	char Path[SETTINGS_PATH_MAX];
	CAtariStorage::MakePath (Path, sizeof Path, m_BrowseDirectory, pEntry);

	boolean bOK = FALSE;

	switch (m_BrowseTarget)
	{
	case BrowseDisk1:
	case BrowseDisk2:
	case BrowseDisk3:
	case BrowseDisk4:
		{
			int nDrive = m_BrowseTarget - BrowseDisk1;
			bOK = m_pHost->MountDisk (nDrive + 1, Path,
						  m_pSettings->DiskReadOnly[nDrive]);
			if (bOK)
			{
				CopyText (m_pSettings->DiskPath[nDrive], Path, SETTINGS_PATH_MAX);
			}
		}
		break;

	case BrowseCartridge:
		bOK = m_pHost->MountCartridge (Path);
		if (bOK)
		{
			CopyText (m_pSettings->CartridgePath, Path, SETTINGS_PATH_MAX);
		}
		break;

	case BrowseCassette:
		bOK = m_pHost->MountCassette (Path, m_pSettings->bCassetteReadOnly);
		if (bOK)
		{
			CopyText (m_pSettings->CassettePath, Path, SETTINGS_PATH_MAX);
		}
		break;

	case BrowseSnapshotLoad:
		bOK = m_pHost->LoadSnapshot (Path);
		break;

	case BrowseSnapshotSave:
		bOK = m_pHost->SaveSnapshot (Path);
		break;

	default:
		break;
	}

	// Remember where the user was browsing, for next time.
	if (   m_BrowseTarget != BrowseSnapshotLoad
	    && m_BrowseTarget != BrowseSnapshotSave
	    && m_BrowseTarget != BrowseSnapshotErase)
	{
		CopyText (m_pSettings->UserDirectory, m_BrowseDirectory, SETTINGS_PATH_MAX);
	}

	if (bOK)
	{
		/*
		 * A tape that needs the other SIO mode replaces "Loaded" with what has to
		 * happen next: the mode is fixed when the machine is built, so a power
		 * cycle is the only way to load it.  Leaving the menu performs that cycle
		 * -- see ExitMenu().
		 */
		const char *pNotice = m_pHost != 0
				    ? m_pHost->GetCassetteModeMessage () : 0;

		if (pNotice != 0)
		{
			snprintf (m_Message, sizeof m_Message, "%s", pNotice);
		}
		else
		{
			snprintf (m_Message, sizeof m_Message, "Loaded %s", pEntry);
		}

		m_nMessageFrames = MESSAGE_FRAMES;
		SaveSettings ();
		OpenScreen (BrowseOrigin (m_BrowseTarget));
	}
	else
	{
		snprintf (m_Message, sizeof m_Message, "Failed: %s",
			  m_pHost->GetLastError ());
		m_nMessageFrames = MESSAGE_FRAMES;
	}
}

void CAtariMenu::ClearPendingErase (boolean bHideMessage)
{
	if (m_ErasePath[0] == '\0' && m_nEraseFrames == 0)
	{
		return;
	}

	m_ErasePath[0] = '\0';
	m_nEraseFrames = 0;

	/*
	 * While an erasure is pending the message is the question, so hiding it is
	 * what "cancelled" has to look like: leaving it on screen after the key
	 * that cancelled it would leave the user staring at a prompt whose action
	 * has already been abandoned.
	 */
	if (bHideMessage)
	{
		m_Message[0] = '\0';
		m_nMessageFrames = 0;
	}
}

/*
 * Erases the highlighted snapshot.
 *
 * The first press only asks; the second removes the file.  Return and Delete
 * both mean "this one" on this screen, so either can answer the question, and
 * the wording does not name a key.  The question is kept open for one message
 * lifetime, so a press much later in the same place starts again rather than
 * removing something the user no longer has in mind.
 */
void CAtariMenu::BrowserErase (void)
{
	if (m_pStorage == 0)
	{
		return;
	}

	const char *pEntry = m_pStorage->GetEntry (m_nBrowserSelected);
	if (pEntry == 0 || pEntry[0] == '/')
	{
		/* Nothing to remove, or a directory: leave the question closed. */
		ClearPendingErase (TRUE);

		return;
	}

	/*
	 * Copied because the listing is rebuilt below, which overwrites the name
	 * the entry pointer points into.
	 */
	char Name[STORAGE_NAME_MAX];
	CopyText (Name, pEntry, sizeof Name);

	char Path[SETTINGS_PATH_MAX];
	CAtariStorage::MakePath (Path, sizeof Path, m_BrowseDirectory, Name);

	if (strcmp (Path, m_ErasePath) != 0)
	{
		CopyText (m_ErasePath, Path, sizeof m_ErasePath);
		m_nEraseFrames = MESSAGE_FRAMES;

		snprintf (m_Message, sizeof m_Message,
			  "Erase %s? Press again to confirm", Name);
		m_nMessageFrames = MESSAGE_FRAMES;

		return;
	}

	ClearPendingErase (FALSE);

	if (!CAtariStorage::Remove (Path))
	{
		snprintf (m_Message, sizeof m_Message, "Could not erase %s", Name);
		m_nMessageFrames = MESSAGE_FRAMES;

		return;
	}

	ListBrowserDirectory ();

	/*
	 * The listing shrank by one, so the highlight may now be past the end.
	 * Leaving it where it is puts it on the row that followed the erased file,
	 * which is where the user is looking.
	 */
	unsigned nCount = m_pStorage->GetEntryCount ();
	if (m_nBrowserSelected >= nCount)
	{
		m_nBrowserSelected = nCount > 0 ? nCount - 1 : 0;
	}
	if (m_nBrowseTop > m_nBrowserSelected)
	{
		m_nBrowseTop = m_nBrowserSelected;
	}

	snprintf (m_Message, sizeof m_Message, "Erased %s", Name);
	m_nMessageFrames = MESSAGE_FRAMES;
}

/* ---------------------------------------------------------------------------
 * Settings persistence
 * ------------------------------------------------------------------------- */

void CAtariMenu::SaveSettings (void)
{
	if (m_pStorage == 0 || m_pSettings == 0 || m_SettingsPath[0] == '\0')
	{
		return;
	}

	if (!Settings_Save (*m_pStorage, m_SettingsPath, *m_pSettings))
	{
		// The path can be longer than the message buffer, so it is not
		// included; the log has it.
		CopyText (m_Message, "Could not save the settings file; see the log",
			  sizeof m_Message);
		m_nMessageFrames = MESSAGE_FRAMES;
	}
}

/* ---------------------------------------------------------------------------
 * Key handling
 * ------------------------------------------------------------------------- */

boolean CAtariMenu::HandleKey (int nKey, boolean bShift, boolean bControl)
{
	(void) bControl;

	if (!m_bOpen)
	{
		return FALSE;
	}

	if (nKey == ATARI_KEY_NONE)
	{
		return FALSE;
	}

	if (m_Screen == ScreenBrowse)
	{
		unsigned nCount = m_pStorage != 0 ? m_pStorage->GetEntryCount () : 0;

		// Enough rows to fill the panel; see m_nVisibleRows.
		unsigned nVisibleRows = m_nVisibleRows;

		switch (nKey)
		{
		case ATARI_KEY_UP:
			ClearPendingErase (TRUE);
			if (nCount > 0)
			{
				if (m_nBrowserSelected > 0)
				{
					m_nBrowserSelected--;
					if (m_nBrowserSelected < m_nBrowseTop)
					{
						m_nBrowseTop = m_nBrowserSelected;
					}
				}
			}
			return TRUE;

		case ATARI_KEY_DOWN:
			ClearPendingErase (TRUE);
			if (nCount > 0 && m_nBrowserSelected + 1 < nCount)
			{
				m_nBrowserSelected++;
				if (m_nBrowserSelected >= m_nBrowseTop + nVisibleRows)
				{
					m_nBrowseTop = m_nBrowserSelected - nVisibleRows + 1;
				}
			}
			return TRUE;

		case ATARI_KEY_RETURN:
			BrowserChoose ();
			return TRUE;

		case ATARI_KEY_DELETE_CHAR:
			/*
			 * A second way to answer the erase screen's question.
			 *
			 * The key does nothing in the other browsers: the save and load
			 * screens have no business removing files, and neither do the
			 * media browsers, so a file can only be erased from the screen
			 * that exists for it.
			 */
			if (m_BrowseTarget == BrowseSnapshotErase)
			{
				BrowserErase ();
			}
			return TRUE;

		case ATARI_KEY_ESCAPE:
			/*
			 * Escape goes up a directory, and leaves the browser only from a
			 * volume root -- which is the point where there is nothing above
			 * to go to, so the key always does something visible rather than
			 * appearing to be ignored.
			 */
			ClearPendingErase (TRUE);
			if (!BrowserGoUp ())
			{
				OpenScreen (BrowseOrigin (m_BrowseTarget));
			}
			return TRUE;

		default:
			// Anything else closes the menu, so the user is never trapped.
			Close ();
			return TRUE;
		}
	}

	switch (nKey)
	{
	case ATARI_KEY_UP:
		MoveSelection (-1);
		return TRUE;

	case ATARI_KEY_DOWN:
		MoveSelection (1);
		return TRUE;

	case ATARI_KEY_LEFT:
		AdjustSetting (-1);
		return TRUE;

	case ATARI_KEY_RIGHT:
		AdjustSetting (1);
		return TRUE;

	case ATARI_KEY_RETURN:
	case ATARI_KEY_SPACE:
		ActivateSelection ();
		return TRUE;

	case ATARI_KEY_DELETE_CHAR:
		/*
		 * Ejects whatever the media screens have selected.
		 *
		 * Without this there is no way to remove an image once loaded: the
		 * browser can only ever supply a path, so a drive would keep its image
		 * until the machine was power-cycled.  The Atari's own DELETE key is
		 * used because it means "remove", and because on a USB keyboard it is
		 * the Delete key in the expected place.
		 */
		EjectSelection ();
		return TRUE;

	case ATARI_KEY_ESCAPE:
		if (m_Screen == ScreenMain)
		{
			ExitMenu (ExitClose);
		}
		else
		{
			OpenScreen (ScreenMain);
		}
		return TRUE;

	default:
		return FALSE;
	}
}

/* ---------------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------------- */

void CAtariMenu::DrawHeader (const char *pTitle)
{
	unsigned nPanelX = MenuMarginX;
	unsigned nPanelY = MenuMarginY;

	m_pVideo->DrawRect (nPanelX, nPanelY, MenuPanelWidth, m_nHeaderHeight,
			       ATARI_COLOR_SELECT);
	m_pVideo->DrawText (nPanelX + MenuIndent,
			    nPanelY + (m_nHeaderHeight - m_nLineHeight) / 2,
			    ATARI_COLOR_WHITE, pTitle);
}

void CAtariMenu::DrawFooter (const char *pHint)
{
	unsigned nPanelY = MenuMarginY + MenuPanelHeight - m_nFooterHeight;

	m_pVideo->DrawRect (MenuMarginX, nPanelY, MenuPanelWidth, m_nFooterHeight,
			       ATARI_COLOR_SELECT);
	m_pVideo->DrawText (MenuMarginX + MenuIndent,
			    nPanelY + (m_nFooterHeight - m_nLineHeight) / 2,
			    ATARI_COLOR_WHITE, pHint);
}

void CAtariMenu::DrawItem (unsigned nY, boolean bSelected, const char *pLabel,
			   const char *pValue)
{
	unsigned nPanelX = MenuMarginX;
	unsigned nRowX = nPanelX + MenuIndent;
	unsigned nRowWidth = MenuPanelWidth - MenuIndent * 2;

	if (bSelected)
	{
		m_pVideo->DrawRect (nPanelX + 4, nY - 2, MenuPanelWidth - 8,
				    m_nRowPitch - 4, ATARI_COLOR_SELECT);
	}

	TAtariColor Color = bSelected ? ATARI_COLOR_WHITE : ATARI_COLOR_PANEL;
	// Unselected text is drawn bright enough to read against the panel; the
	// panel colour is too dark for body text, so use grey.
	if (!bSelected)
	{
		Color = ATARI_COLOR_GREY;
	}

	m_pVideo->DrawText (nRowX, nY, Color, pLabel);
	if (pValue != 0 && pValue[0] != '\0')
	{
		{
		char Value[64];
		CopyText (Value, pValue, sizeof Value);
		Truncate (Value, TextColumns (MenuPanelWidth - MenuLabelColumn - 8));

		m_pVideo->DrawText (nPanelX + MenuLabelColumn, nY, Color, Value);
	}
	}

	(void) nRowWidth;
}

/*
 * The status line shows what the emulator is doing, so that the user can tell
 * at a glance whether the machine is running at the right speed.
 */
void CAtariMenu::DrawStatusLine (void)
{
	char Text[MENU_TEXT_MAX];

	snprintf (Text, sizeof Text, "%s  %s  %s  %.1f fps",
		  MachineName (m_pSettings->nMachine),
		  RamName (m_pSettings->nRamKB),
		  VideoName (m_pSettings->nVideoStandard),
		  m_pHost != 0 ? m_pHost->GetFps () : 0.0);

	Truncate (Text, TextColumns (MenuPanelWidth - 8));

	m_pVideo->DrawText (MenuMarginX + 4,
			    MenuMarginY + MenuPanelHeight - m_nStatusOffset,
			    ATARI_COLOR_TITLE, Text);
}

void CAtariMenu::DrawMainScreen (void)
{
	DrawHeader ("Atari XL/XE");

	unsigned nY = MenuMarginY + m_nItemTop;
	unsigned nCount = GetItemCount ();

	for (unsigned i = 0; i < nCount; i++)
	{
		DrawItem (nY, i == m_nSelected, GetItemLabel (i), GetItemValue (i));
		nY += m_nRowPitch;
	}

	DrawFooter ("Up/Down  Left/Right  Return  Esc");
}

void CAtariMenu::DrawBrowser (void)
{
	char Title[MENU_TEXT_MAX];

	// The browser's title is the directory being listed, which can be longer
	// than the title bar; CopyText truncates it to the buffer, and the second
	// cut is to the panel, which is what the user can see.
	CopyText (Title, m_BrowseDirectory, sizeof Title);
	Truncate (Title, TextColumns (MenuPanelWidth - 2 * MenuIndent));
	DrawHeader (Title);

	unsigned nCount = m_pStorage != 0 ? m_pStorage->GetEntryCount () : 0;
	unsigned nVisibleRows = m_nVisibleRows;

	if (nCount == 0)
	{
		// Two different explanations: an empty directory, or a volume that is
		// not there at all.  Which volume the user expects depends on the
		// setting, so the message does too.
		const char *pMessage = "No matching files here";

		if (m_pStorage == 0 || !m_pStorage->IsUserVolumeAvailable ())
		{
			pMessage = m_pStorage != 0 && m_pStorage->IsUserVolumeSD ()
				 ? "The SD card is not available"
				 : "No USB drive detected";
		}

		m_pVideo->DrawText (MenuMarginX + MenuIndent, MenuMarginY + m_nItemTop,
				    ATARI_COLOR_TITLE, pMessage);
	}
	else
	{
		unsigned nY = MenuMarginY + m_nItemTop;

		for (unsigned nRow = 0; nRow < nVisibleRows; nRow++)
		{
			unsigned nIndex = m_nBrowseTop + nRow;
			if (nIndex >= nCount)
			{
				break;
			}

			const char *pEntry = m_pStorage->GetEntry (nIndex);

			// The leading '/' that marks a directory is not part of the
			// name; show the directory with a trailing slash instead, which
			// is how users expect to see it.
			char Name[STORAGE_NAME_MAX + 2];
			if (pEntry != 0 && pEntry[0] == '/')
			{
				snprintf (Name, sizeof Name, "%s/", pEntry + 1);
			}
			else
			{
				snprintf (Name, sizeof Name, "%s", pEntry != 0 ? pEntry : "");
			}

			Truncate (Name, TextColumns (MenuPanelWidth - 2 * MenuIndent));

			DrawItem (nY, nIndex == m_nBrowserSelected, Name, "");
			nY += m_nRowPitch;
		}
	}

	DrawFooter (   m_BrowseTarget == BrowseSnapshotErase
		    ? "Up/Down  Return/Del erase  Esc"
		    : "Up/Down  Return  Esc");
}

void CAtariMenu::Draw (void)
{
	if (!m_bOpen || m_pVideo == 0 || m_pSettings == 0)
	{
		return;
	}

	unsigned nPanelX = MenuMarginX;
	unsigned nPanelY = MenuMarginY;

	/*
	 * The panel's background.
	 *
	 * Normally the picture behind it is dimmed, so the menu reads clearly without
	 * hiding what the machine is doing.  With STATIC_OSD the picture is not
	 * repainted while the menu is open, so a dimmed copy of it would be a stale
	 * one: the panel gets an opaque background instead, which is also cheaper --
	 * a write, rather than a read-modify-write over 0.9 MB.
	 */
	if (m_pSettings->bStaticOSD)
	{
		m_pVideo->DrawRect (nPanelX, nPanelY, MenuPanelWidth, MenuPanelHeight,
				    ATARI_COLOR_DARK);
	}
	else
	{
		m_pVideo->DimRect (nPanelX, nPanelY, MenuPanelWidth, MenuPanelHeight, 65);
	}

	m_pVideo->DrawRectOutline (nPanelX, nPanelY, MenuPanelWidth, MenuPanelHeight,
				   ATARI_COLOR_WHITE);

	if (m_Screen == ScreenBrowse)
	{
		DrawBrowser ();
	}
	else
	{
		DrawMainScreen ();
	}

	DrawStatusLine ();

	// A transient message reports the outcome of the last action.  It can carry
	// a file name, so it is cut to the panel like everything else.
	if (m_nMessageFrames > 0)
	{
		char Message[sizeof m_Message];
		CopyText (Message, m_Message, sizeof Message);
		Truncate (Message, TextColumns (MenuPanelWidth - 8));

		m_pVideo->DrawText (MenuMarginX + 4,
				    MenuMarginY + MenuPanelHeight - m_nMessageOffset,
				    ATARI_COLOR_WHITE, Message);
		m_nMessageFrames--;
	}

	/*
	 * An erasure is only confirmed while its question is still on screen; after
	 * that the next press asks again rather than removing a file the user has
	 * stopped thinking about.  The message is not touched: it is about to be
	 * zero, which hides it in the same frame.
	 */
	if (m_nEraseFrames > 0 && --m_nEraseFrames == 0)
	{
		m_ErasePath[0] = '\0';
	}
}
