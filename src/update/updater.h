/*
 * updater.h -- install a kernel image from a USB drive.
 *
 * The update is deliberately small and explicit: the user puts update.emu and
 * one or more kernel files in the USB root, asks the OSD to check, confirms,
 * and the Pi restarts.  There is no hot-plug support, no rollback and no
 * target-board detection; see docs/using.md for the procedure and its limits.
 */

#ifndef ATARI_PI_UPDATER_H
#define ATARI_PI_UPDATER_H

#include <circle/types.h>

#include "storage/storage.h"

/*
 * Compatibility marker this build accepts.  Change it when an update would
 * need a different boot layout, firmware or armstub.  The value is compared
 * with the trimmed contents of USB:/update.emu.
 */
#define ATARI_UPDATE_CODE	"Noal"

class CAtariUpdater
{
public:
	CAtariUpdater (void);

	/*
	 * Looks for update.emu and the accepted files in the USB root.
	 *
	 * On success m_File[] holds the files to install and MSG describes what
	 * was found.  On failure nothing is changed and MSG says why.
	 */
	boolean Scan (CAtariStorage &Storage, char *pMessage, unsigned nMessageSize);

	/*
	 * Copies every scanned file to a staging name on SD:, verifies it, then
	 * replaces the destination.  The marker is renamed to update.done after all
	 * replacements succeed; if the rename fails it is deleted instead.  Either
	 * failure is ignored and the update still counts as complete.
	 *
	 * \return TRUE if every destination was replaced.
	 */
	boolean Apply (CAtariStorage &Storage, char *pMessage, unsigned nMessageSize);

	void Clear (void);

	boolean HasFiles (void) const			{ return m_nFiles != 0; }
	unsigned GetFileCount (void) const		{ return m_nFiles; }
	const char *GetFileName (unsigned nIndex) const;

private:
	struct TFile
	{
		char	Name[24];
		long	Size;
	};

	static boolean CopyFile (const char *pSource, const char *pDestination);
	static boolean SameContents (const char *pA, const char *pB);

private:
	TFile		m_File[4];
	unsigned	m_nFiles;
};

#endif
