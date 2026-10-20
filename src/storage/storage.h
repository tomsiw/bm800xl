/*
 * storage.h -- file system access for the emulator.
 *
 * The emulator keeps two kinds of file in two places:
 *
 *   * Its own settings and save states live on the SD card, in the directory
 *     the kernel image was booted from, so that they follow the card rather
 *     than the removable drive.
 *
 *   * Everything the emulated machine uses -- disk, cartridge and cassette
 *     images -- lives on the USB drive, because those images are large, they
 *     change, and they are the user's own collection.
 *
 * Both are FAT file systems, both are reached through FatFs, and both are
 * addressed by a FatFs path with a volume: "SD:/ROMS/ATARIXL.ROM" and
 * "USB:/disks/BOUNTYBOB.ATR".  This module owns the volume names and the
 * "USER:" alias, so that nothing else in the kernel has to know that the
 * external drive is really called "USB1:".
 *
 * Volumes may be absent: the emulator has to start, and stay usable, with no
 * USB drive plugged in.  Every call therefore reports failure rather than
 * assuming a medium.
 */

#ifndef ATARI_PI_STORAGE_H
#define ATARI_PI_STORAGE_H

#include <circle/types.h>

#include "ff.h"

/*
 * The volume names the rest of the emulator uses.
 *
 * STORAGE_VOLUME_SYSTEM is where settings, snapshots and the ROM images live;
 * STORAGE_VOLUME_USER is where media images live, and is the first USB mass
 * storage device that has a usable file system.
 */
#define STORAGE_VOLUME_SYSTEM	"SD:"
#define STORAGE_VOLUME_USER	"USB:"

/*
 * Longest volume prefix, including the colon and the terminating NUL.
 *
 * The longest string FatFs is configured with is "NVME", so "NVME:" is five
 * characters plus the terminator.  Sized here rather than derived, because the
 * volume list lives in the FatFs configuration and this must not grow with it
 * silently.
 */
#define STORAGE_VOLUME_MAX	16

/* Longest path this module handles, including the terminating NUL. */
#define STORAGE_PATH_MAX	256

/* Longest file name in a directory listing, including the terminating NUL. */
#define STORAGE_NAME_MAX	(FF_LFN_BUF + 1)

/* Directory listing size.  A media directory with more entries than this is
 * still usable: the listing is only ever shown one page at a time. */
#define STORAGE_MAX_ENTRIES	192

/* Longest file extension filter, e.g. "ATR". */
#define STORAGE_MAX_FILTER	16

class CAtariStorage
{
public:
	CAtariStorage (void);
	~CAtariStorage (void);

	/*
	 * Prepares the file systems.
	 *
	 * The SD card is mounted and, if the ROMS directory is missing, created.
	 * The USB drive is optional: this returns TRUE even when none is
	 * attached, because the emulator must still run.
	 *
	 * \param pSystemDirectory Directory on the SD card for settings and save
	 *			   states; created if it does not exist.
	 * \return FALSE only if the SD card itself cannot be mounted.
	 */
	boolean Initialize (const char *pSystemDirectory = "/ATARI");

	/* \return TRUE if a USB drive with a usable file system is mounted. */
	boolean IsUserVolumeAvailable (void) const	{ return m_bUserVolume; }

	/* \return TRUE if the SD card is mounted. */
	boolean IsSystemVolumeAvailable (void) const	{ return m_bSystemVolume; }

	/*
	 * Tries to mount the USB drive, or to notice that it has been removed.
	 * Call this when the user asks for a file list, so that plugging the drive
	 * in after boot works.
	 *
	 * \return TRUE if the state changed.
	 */
	boolean RefreshUserVolume (void);

	/* The directory settings and save states are kept in, e.g. "SD:/ATARI". */
	const char *GetSystemDirectory (void) const	{ return m_SystemDirectory; }

	/* The directory media images are read from, e.g. "USB:". */
	const char *GetUserDirectory (void) const;

	/*
	 * Builds a path in PATH.  NAME may be absolute (it already contains a
	 * volume) in which case it is copied unchanged, or a bare name which is
	 * resolved against the given directory.
	 */
	static void MakePath (char *pPath, unsigned nSize, const char *pDirectory,
			      const char *pName);

	/*
	 * Splits an absolute path into its volume prefix and the directory within
	 * that volume.
	 *
	 * The two are needed separately because FatFs wants them separately: a
	 * directory is opened by a path that already carries its volume, while the
	 * file browser starts from the volume on its own.  Passing a whole path in
	 * both places builds "USB:USB:/disks" and opens nothing at all.
	 *
	 * A path with no colon is treated as being on the system volume.
	 */
	static void SplitPath (const char *pPath, char *pVolume, unsigned nVolumeSize,
			       char *pDirectory, unsigned nDirectorySize);

	/* \return The last component of a path, or the path itself if it has none. */
	static const char *BaseName (const char *pPath);

	/*
	 * Removes the last component of PPATH in place, leaving the parent path.
	 *
	 * Cuts at the last '/' rather than using BaseName(), and tolerates a
	 * trailing slash so that both "USB:/disks" and "USB:/disks/" reduce to
	 * "USB:".
	 *
	 * \return FALSE if there is nowhere above PPATH -- that is, if PPATH is a
	 *	   volume root such as "USB:" or "USB:/", which has no parent.
	 */
	static boolean TruncateToParent (char *pPath);

	/* Builds a path on the system volume, e.g. "SD:/ATARI/settings.cfg". */
	void MakeSystemPath (char *pPath, unsigned nSize, const char *pName) const;

	/*
	 * Directory listing
	 *
	 * Extensions is a string of semicolon separated extensions without dots,
	 * e.g. "ATR;XFD".  A NULL or empty filter lists every regular file.
	 */

	/*
	 * Fills the internal listing from DIRECTORY in the given volume.
	 *
	 * \param pVolume   STORAGE_VOLUME_SYSTEM or STORAGE_VOLUME_USER
	 * \param pDirectory Directory to list, or NULL for the volume root
	 * \param pExtensions Extension filter as described above
	 * \return Number of entries found
	 */
	unsigned List (const char *pVolume, const char *pDirectory, const char *pExtensions);

	/*
	 * As List(), but also includes sub-directories so that the user can walk
	 * into them.  Names that are directories are prefixed with "/".  "." and
	 * ".." are not returned.
	 */
	unsigned ListWithDirectories (const char *pVolume, const char *pDirectory,
				      const char *pExtensions);

	/* \return Number of entries from the most recent List call. */
	unsigned GetEntryCount (void) const		{ return m_nEntryCount; }

	/* \return Name of entry INDEX, including the "/" prefix for a directory,
	 *	    or NULL if INDEX is out of range. */
	const char *GetEntry (unsigned nIndex) const;

	/* \return TRUE if entry INDEX is a sub-directory. */
	boolean IsEntryDirectory (unsigned nIndex) const;

	/* \return TRUE if PATH names an existing file. */
	static boolean FileExists (const char *pPath);

	/* \return Size of PATH in bytes, or -1 if it cannot be read. */
	static long FileSize (const char *pPath);

	/* Creates DIRECTORY (one level) if it does not already exist. */
	static boolean EnsureDirectory (const char *pDirectory);

private:
	void ResetListing (void);
	unsigned ListInternal (const char *pVolume, const char *pDirectory,
			       const char *pExtensions, boolean bIncludeDirectories);

private:
	FATFS m_SystemFileSystem;
	FATFS m_UserFileSystem;

	boolean m_bSystemVolume;
	boolean m_bUserVolume;

	char m_SystemDirectory[STORAGE_PATH_MAX];

	char m_EntryNames[STORAGE_MAX_ENTRIES][STORAGE_NAME_MAX];
	boolean m_EntryIsDirectory[STORAGE_MAX_ENTRIES];
	unsigned m_nEntryCount;
};

#endif
