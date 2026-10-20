/*
 * osd_menu.h -- the on-screen menu.
 *
 * The menu is the only user interface the emulator has, so everything the user
 * can change lives here: which disk, cartridge and cassette images are loaded,
 * how much RAM the emulated machine has, whether it is an XL or an XE, PAL or
 * NTSC, the sound volume, and saving and loading a snapshot of the machine.
 *
 * It is opened with F12 on a USB keyboard, or Shift+Help on an Atari keyboard
 * wired to the GPIO header, and it is drawn straight into the framebuffer over
 * the emulated picture.
 *
 * The menu owns its own state machine rather than a table of descriptions, for
 * one reason: a menu action here is not a value to store but something to do
 * (mount this file, reboot the machine, write a snapshot), and the arguments
 * differ from item to item.  Keeping the actions as code makes that obvious and
 * keeps the awkward cases -- the file browser, the volume slider, the settings
 * that need a confirmation -- readable.
 *
 * Settings are written to the SD card as soon as they change, so the machine
 * comes back the way it was left even if the power is cut.
 */

#ifndef ATARI_PI_OSD_MENU_H
#define ATARI_PI_OSD_MENU_H

#include <circle/types.h>

#include "atari_port.h"
#include "settings.h"
#include "storage.h"
#include "video_out.h"

// The menu needs to be able to apply the settings it edits, and to ask for a
// reset.  The kernel implements this; the menu deliberately knows nothing about
// how the emulator is built.
class CAtariMenuHost
{
public:
	virtual ~CAtariMenuHost (void) { }

	// Applies everything in SETTINGS to the running emulator.  Called after
	// the menu changes something.  Returns FALSE if a change could not be
	// applied, in which case the caller reports it.
	virtual boolean ApplySettings (const TAtariSettings &Settings) = 0;

	// Starts, or continues, the game or program on the given image: mounts it
	// and presses the emulated Reset button, which is what an Atari does when
	// a cartridge is inserted or a disk is swapped.
	virtual boolean MountDisk (int nDrive, const char *pPath, boolean bReadOnly) = 0;
	virtual boolean MountCartridge (const char *pPath) = 0;
	virtual boolean MountCassette (const char *pPath, boolean bReadOnly) = 0;

	virtual boolean SaveSnapshot (const char *pPath) = 0;
	virtual boolean LoadSnapshot (const char *pPath) = 0;

	virtual void ColdReset (void) = 0;
	virtual void WarmReset (void) = 0;

	// The message to show when the last operation failed.
	virtual const char *GetLastError (void) = 0;

	// Frames emulated per second, for the status display.
	virtual double GetFps (void) = 0;
};

class CAtariMenu
{
public:
	CAtariMenu (void);
	~CAtariMenu (void);

	void Initialize (CAtariVideoOut *pVideo, CAtariStorage *pStorage,
			 CAtariMenuHost *pHost, TAtariSettings *pSettings,
			 const char *pSettingsPath);

	/// \return Is the menu currently on screen?
	boolean IsOpen (void) const		{ return m_bOpen; }

	/// Opens or closes the menu.  Closing commits any pending setting.
	void Toggle (void);

	/// Closes the menu without applying pending changes.
	void Close (void);

	/*
	 * Routes one Atari key press to the menu.
	 *
	 * NKey is an ATARI_KEY_* code, or ATARI_KEY_NONE.  Shift and Control are
	 * passed separately.  The key must be a fresh press, not a held key: the
	 * caller is responsible for edge detection, because only it knows how its
	 * keyboard reports state.
	 *
	 * \return TRUE if the key was used by the menu and must not reach the
	 *	   emulated machine.
	 */
	boolean HandleKey (int nKey, boolean bShift, boolean bControl);

	/*
	 * Draws the menu over whatever is currently in the framebuffer, and
	 * presents the result.  Does nothing when the menu is closed.
	 */
	void Draw (void);

private:
	// What the menu is currently showing
	enum TMenuScreen
	{
		ScreenMain = 0,
		ScreenDisks,
		ScreenCartridge,
		ScreenCassette,
		ScreenMachine,
		ScreenVideo,
		ScreenSound,
		ScreenSnapshots,
		ScreenBrowse,
	};

	// A file browser that can be entered from any of the file settings.
	enum TBrowseTarget
	{
		BrowseNone = 0,
		BrowseDisk1,
		BrowseDisk2,
		BrowseDisk3,
		BrowseDisk4,
		BrowseCartridge,
		BrowseCassette,
		BrowseSnapshotLoad,
		BrowseSnapshotSave,
	};

	void OpenScreen (TMenuScreen Screen);
	void MoveSelection (int nDelta);
	void AdjustSetting (int nDelta);
	void ActivateSelection (void);

	/*
	 * Saves a snapshot under the first unused SNAPSHOTn.SAV.
	 *
	 * Needed because the file browser can only list files that already exist,
	 * so on a fresh card there is nothing for it to save to and the first
	 * snapshot could never be made.
	 */
	void SaveNewSnapshot (void);

	/*
	 * Removes the image loaded in whatever the current screen has selected.
	 *
	 * Ejecting clears the stored path as well as telling the host, so the two
	 * cannot disagree: a drive shown as loaded but with nothing mounted is a
	 * state the user has no way to understand or correct.
	 *
	 * Does nothing on screens that have nothing to eject.
	 */
	void EjectSelection (void);

	void OpenBrowser (TBrowseTarget Target);

	/*
	 * The screen a browser belongs to.
	 *
	 * The browser is entered from several screens, so leaving it has to
	 * return where the user started rather than to a fixed one; otherwise a
	 * cartridge chosen from the main screen would drop the user into the
	 * disk screen afterwards.
	 */
	TMenuScreen BrowseOrigin (TBrowseTarget Target) const;

	/*
	 * Lists m_BrowseDirectory into the storage layer.
	 *
	 * The directory and its volume are passed to the storage layer separately,
	 * because that is how FatFs takes them; handing over the whole path as both
	 * builds "USB:USB:/disks" and lists nothing.
	 */
	void ListBrowserDirectory (const char *pExtensions);

	void BrowserEnterDirectory (void);

	/* \return TRUE if the listing now shows the parent directory */
	boolean BrowserGoUp (void);
	void BrowserChoose (void);

	void SaveSettings (void);

	// Drawing helpers, all of them in terms of the video output's primitives
	void DrawMainScreen (void);
	void DrawBrowser (void);
	void DrawHeader (const char *pTitle);
	void DrawFooter (const char *pHint);
	void DrawItem (unsigned nY, boolean bSelected, const char *pLabel,
		       const char *pValue);
	void DrawStatusLine (void);

	unsigned GetItemCount (void) const;
	const char *GetItemLabel (unsigned nIndex) const;
	const char *GetItemValue (unsigned nIndex) const;

	// Names shown for enumerated settings
	static const char *MachineName (int nMachine);
	static const char *RamName (int nRamKB);
	static const char *VideoName (int nStandard);
	static const char *ArtifactName (int nArtifact);
	static const char *KeyboardName (int nMode);
	static const char *ResolutionName (int nResolution);
	static const char *BaseName (const char *pPath);

private:
	CAtariVideoOut *m_pVideo;
	CAtariStorage *m_pStorage;
	CAtariMenuHost *m_pHost;
	TAtariSettings *m_pSettings;

	char m_SettingsPath[SETTINGS_PATH_MAX];

	boolean m_bOpen;
	TMenuScreen m_Screen;
	unsigned m_nSelected;

	// File browser state
	TBrowseTarget m_BrowseTarget;
	char m_BrowseDirectory[SETTINGS_PATH_MAX];
	unsigned m_nBrowseTop;		// first visible entry
	unsigned m_nBrowserSelected;

	// Transient message shown at the bottom, and how many frames are left
	char m_Message[80];
	unsigned m_nMessageFrames;
};

#endif
