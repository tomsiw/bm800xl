/*
 * storage.cpp -- file system access for the emulator.  See storage.h.
 *
 * FatFs is used for everything, with two logical drives:
 *
 *   "SD:"   the SD card, mounted as m_SystemFileSystem, holding settings,
 *           save states and the OS ROM images;
 *   "USB:"  the first USB mass storage device that carries a file system,
 *           mounted as m_UserFileSystem, holding disk, cartridge and cassette
 *           images.
 *
 * The USB side is optional throughout.  Every function that touches it checks
 * that it is actually present, because a user is free to run the emulator with
 * no USB drive attached, and a drive can be unplugged at any moment.
 */

#include "storage.h"

#include <circle/logger.h>
#include <circle/string.h>
#include <circle/util.h>

#include <string.h>

static const char FromStorage[] = "storage";

CAtariStorage::CAtariStorage (void)
:	m_pSystemFileSystem (new FATFS),
	m_pUserFileSystem (new FATFS),
	m_bSystemVolume (FALSE),
	m_bUserVolume (FALSE),
	m_nEntryCount (0)
{
	m_SystemDirectory[0] = '\0';
	ResetListing ();
}

CAtariStorage::~CAtariStorage (void)
{
	if (m_bUserVolume)
	{
		f_mount (0, STORAGE_VOLUME_USER, 0);
	}
	if (m_bSystemVolume)
	{
		f_mount (0, STORAGE_VOLUME_SYSTEM, 0);
	}
}

boolean CAtariStorage::Initialize (const char *pSystemDirectory)
{
	// The SD card carries the emulator's own files, so it must work.
	if (f_mount (m_pSystemFileSystem, STORAGE_VOLUME_SYSTEM, 1) != FR_OK)
	{
		CLogger::Get ()->Write (FromStorage, LogError,
					"Cannot mount the SD card as %s",
					STORAGE_VOLUME_SYSTEM);
		return FALSE;
	}
	m_bSystemVolume = TRUE;

	// Settings live in a directory of their own so that the card's root stays
	// tidy and the user can find them easily.
	if (pSystemDirectory != 0 && pSystemDirectory[0] != '\0')
	{
		if (pSystemDirectory[0] == '/')
		{
			// A path relative to the volume root, e.g. "/ATARI".
			if (strlen (STORAGE_VOLUME_SYSTEM) + strlen (pSystemDirectory) + 1
			    > sizeof m_SystemDirectory)
			{
				return FALSE;
			}
			strcpy (m_SystemDirectory, STORAGE_VOLUME_SYSTEM);
			strcat (m_SystemDirectory, pSystemDirectory);
		}
		else
		{
			MakePath (m_SystemDirectory, sizeof m_SystemDirectory,
				  STORAGE_VOLUME_SYSTEM, pSystemDirectory);
		}

		EnsureDirectory (m_SystemDirectory);
	}
	else
	{
		strcpy (m_SystemDirectory, STORAGE_VOLUME_SYSTEM);
	}

	// The USB drive is a bonus; not having one is not an error.
	RefreshUserVolume ();

	return TRUE;
}

boolean CAtariStorage::RefreshUserVolume (void)
{
	boolean bWasAvailable = m_bUserVolume;

	/*
	 * Never re-mount a volume that is already mounted.
	 *
	 * f_mount() with opt = 1 re-initialises the volume, and FatFs invalidates
	 * every open file on it when that happens.  This function is called when the
	 * menu opens, so re-mounting there silently destroyed the handle of any disk
	 * image already loaded: every later seek returned FR_INVALID_OBJECT, the
	 * emulated machine read whatever was at position zero instead of the sector
	 * it asked for, and it ran off into the weeds.
	 *
	 * That is why mounting an image and then opening the menu broke the machine,
	 * and why a power cycle -- where nothing re-mounts -- worked.
	 *
	 * Mounting is only needed to discover a drive that has not been seen yet, so
	 * it is attempted only in that case.  The cost is that a drive removed and
	 * reinserted without a reboot is not noticed; a read on the stale volume
	 * fails cleanly rather than corrupting a live image.
	 */
	if (!m_bUserVolume)
	{
		m_bUserVolume = f_mount (m_pUserFileSystem,
					 STORAGE_VOLUME_USER, 1) == FR_OK;
	}

	return m_bUserVolume != bWasAvailable;
}

const char *CAtariStorage::GetUserDirectory (void) const
{
	return m_bUserVolume ? STORAGE_VOLUME_USER : 0;
}

void CAtariStorage::MakePath (char *pPath, unsigned nSize, const char *pDirectory,
			      const char *pName)
{
	if (pPath == 0 || nSize == 0)
	{
		return;
	}
	pPath[0] = '\0';
	if (pName == 0)
	{
		return;
	}

	// A name that already carries a volume is absolute and must not be
	// prefixed; that is how the OSD menu hands back a chosen file.
	const char *pColon = strchr (pName, ':');
	if (pColon != 0)
	{
		unsigned nLen = strlen (pName);
		if (nLen + 1 > nSize)
		{
			return;
		}
		memcpy (pPath, pName, nLen + 1);
		return;
	}

	if (pName[0] == '/')
	{
		// Absolute within a volume, so only the volume is needed.
		CString Path;
		Path.Format ("%s%s", pDirectory != 0 ? pDirectory : "", pName);
		if (Path.GetLength () + 1 > nSize)
		{
			return;
		}
		memcpy (pPath, (const char *) Path, Path.GetLength () + 1);
		return;
	}

	{
		CString Path;
		const char *pDir = pDirectory != 0 ? pDirectory : "";
		unsigned nDirLen = strlen (pDir);

		/*
		 * A trailing ':' is a VOLUME, not a separator.
		 *
		 * "USB:" names the volume; the file inside it is still "USB:/NAME".
		 * Treating the colon as a separator produced "USB:NAME", which FatFs
		 * happens to accept but is not a path this project builds anywhere else
		 * -- the system volume is always "SD:/ATARI/...".  Anything that later
		 * splits or compares such a path sees a different shape, so the
		 * inconsistency was worth removing rather than relying on the parser
		 * being forgiving.
		 */
		boolean bEndsWithSeparator = nDirLen > 0 && pDir[nDirLen - 1] == '/';
		boolean bNeedSeparator = nDirLen > 0 && !bEndsWithSeparator;

		Path.Format ("%s%s%s", pDir, bNeedSeparator ? "/" : "", pName);
		if (Path.GetLength () + 1 > nSize)
		{
			return;
		}
		memcpy (pPath, (const char *) Path, Path.GetLength () + 1);
	}
}

void CAtariStorage::SplitPath (const char *pPath, char *pVolume, unsigned nVolumeSize,
			       char *pDirectory, unsigned nDirectorySize)
{
	if (pVolume != 0 && nVolumeSize > 0)
	{
		pVolume[0] = '\0';
	}
	if (pDirectory != 0 && nDirectorySize > 0)
	{
		pDirectory[0] = '\0';
	}

	if (pPath == 0)
	{
		return;
	}

	// A colon is what marks a volume.  Without one the path is relative to the
	// system volume, which is where the emulator's own files live.
	const char *pColon = strchr (pPath, ':');
	if (pColon == 0)
	{
		if (pVolume != 0 && nVolumeSize > 0)
		{
			strncpy (pVolume, STORAGE_VOLUME_SYSTEM, nVolumeSize - 1);
			pVolume[nVolumeSize - 1] = '\0';
		}
		if (pDirectory != 0 && nDirectorySize > 0)
		{
			strncpy (pDirectory, pPath, nDirectorySize - 1);
			pDirectory[nDirectorySize - 1] = '\0';
		}
		return;
	}

	// The volume includes the colon: FatFs's volume strings are "SD:", "USB:".
	unsigned nVolumeLength = (unsigned) (pColon - pPath) + 1;
	if (pVolume != 0 && nVolumeSize > 0)
	{
		if (nVolumeLength + 1 > nVolumeSize)
		{
			return;
		}
		memcpy (pVolume, pPath, nVolumeLength);
		pVolume[nVolumeLength] = '\0';
	}

	if (pDirectory != 0 && nDirectorySize > 0)
	{
		strncpy (pDirectory, pColon + 1, nDirectorySize - 1);
		pDirectory[nDirectorySize - 1] = '\0';
	}
}

const char *CAtariStorage::BaseName (const char *pPath)
{
	if (pPath == 0)
	{
		return "";
	}

	const char *pSlash = strrchr (pPath, '/');
	if (pSlash != 0 && pSlash[1] != '\0')
	{
		return pSlash + 1;
	}

	// No directory part, so the whole thing is the name -- including "USB:",
	// which is what the browser should show at a volume root.
	return pPath;
}

boolean CAtariStorage::TruncateToParent (char *pPath)
{
	if (pPath == 0)
	{
		return FALSE;
	}

	unsigned nLength = strlen (pPath);

	// Ignore one trailing separator, so "USB:/disks/" behaves like "USB:/disks".
	if (nLength > 0 && pPath[nLength - 1] == '/')
	{
		pPath[--nLength] = '\0';
	}

	char *pSlash = strrchr (pPath, '/');

	// No separator at all means the path is a bare volume such as "USB:", which
	// is the root of that volume and has no parent.
	if (pSlash == 0)
	{
		return FALSE;
	}

	// "USB:/" is the same place as "USB:", stated differently, so there is
	// still nowhere above it.
	if (pSlash == pPath + 1 && pPath[0] != '\0' && pPath[0] != '/')
	{
		return FALSE;
	}

	// A leading separator with no volume: keep the root as "/" rather than
	// letting it become an empty string, which would name nothing.
	if (pSlash == pPath)
	{
		pPath[1] = '\0';
		return pPath[1] != '\0';
	}

	*pSlash = '\0';

	return TRUE;
}

void CAtariStorage::MakeSystemPath (char *pPath, unsigned nSize, const char *pName) const
{
	MakePath (pPath, nSize, m_SystemDirectory, pName);
}

boolean CAtariStorage::FileExists (const char *pPath)
{
	FILINFO Info;

	if (pPath == 0 || pPath[0] == '\0')
	{
		return FALSE;
	}

	// f_stat() also succeeds for directories, which is fine: both are things
	// the caller asked whether they exist.
	return f_stat (pPath, &Info) == FR_OK;
}

long CAtariStorage::FileSize (const char *pPath)
{
	FILINFO Info;

	if (pPath == 0 || pPath[0] == '\0')
	{
		return -1;
	}
	if (f_stat (pPath, &Info) != FR_OK)
	{
		return -1;
	}
	if (Info.fattrib & AM_DIR)
	{
		return -1;
	}

	return (long) Info.fsize;
}

boolean CAtariStorage::EnsureDirectory (const char *pDirectory)
{
	FRESULT Result;

	if (pDirectory == 0 || pDirectory[0] == '\0')
	{
		return FALSE;
	}

	// Already there?  f_mkdir() would fail, so ask first.
	{
		FILINFO Info;
		if (f_stat (pDirectory, &Info) == FR_OK && (Info.fattrib & AM_DIR))
		{
			return TRUE;
		}
	}

	Result = f_mkdir (pDirectory);
	if (Result != FR_OK)
	{
		CLogger::Get ()->Write (FromStorage, LogWarning,
					"Cannot create directory %s (error %d)",
					pDirectory, (int) Result);
		return FALSE;
	}
	return TRUE;
}

void CAtariStorage::ResetListing (void)
{
	m_nEntryCount = 0;
}

/* ---------------------------------------------------------------------------
 * Directory listing
 * ------------------------------------------------------------------------- */

/*
 * Case-insensitive comparison of exactly N characters.  Implemented here rather
 * than using the C library's strncasecmp, which is not part of standard C and
 * is not guaranteed to be present in this newlib configuration.
 */
static boolean EqualsIgnoreCase (const char *pA, const char *pB, unsigned nLength)
{
	for (unsigned i = 0; i < nLength; i++)
	{
		char a = pA[i];
		char b = pB[i];

		if (a >= 'a' && a <= 'z') a = (char) (a - 'a' + 'A');
		if (b >= 'a' && b <= 'z') b = (char) (b - 'a' + 'A');

		if (a != b)
		{
			return FALSE;
		}
	}
	return TRUE;
}

/*
 * Returns TRUE if NAME ends in one of the extensions in FILTER, which is a
 * semicolon separated list.  A NULL or empty filter accepts every name.
 */
static boolean MatchesFilter (const char *pName, const char *pFilter)
{
	if (pFilter == 0 || pFilter[0] == '\0')
	{
		return TRUE;
	}

	const char *pDot = strrchr (pName, '.');
	if (pDot == 0)
	{
		return FALSE;
	}
	pDot++;				// skip the dot

	unsigned nExtensionLength = strlen (pDot);

	const char *p = pFilter;
	while (*p != '\0')
	{
		const char *pEnd = strchr (p, ';');
		unsigned nLen = pEnd != 0 ? (unsigned) (pEnd - p) : strlen (p);

		if (nLen > 0 && nExtensionLength == nLen
		    && EqualsIgnoreCase (pDot, p, nLen))
		{
			return TRUE;
		}
		if (pEnd == 0)
		{
			break;
		}
		p = pEnd + 1;
	}
	return FALSE;
}

unsigned CAtariStorage::List (const char *pVolume, const char *pDirectory,
			      const char *pExtensions)
{
	return ListInternal (pVolume, pDirectory, pExtensions, FALSE);
}

unsigned CAtariStorage::ListWithDirectories (const char *pVolume, const char *pDirectory,
					     const char *pExtensions)
{
	return ListInternal (pVolume, pDirectory, pExtensions, TRUE);
}

unsigned CAtariStorage::ListInternal (const char *pVolume, const char *pDirectory,
				      const char *pExtensions, boolean bIncludeDirectories)
{
	DIR Directory;
	FILINFO FileInfo;
	FRESULT Result;
	CString Path;

	ResetListing ();

	if (pVolume == 0)
	{
		return 0;
	}

	Path.Format ("%s%s", pVolume,
		     pDirectory != 0 && pDirectory[0] != '\0' ? pDirectory : "/");

	Result = f_opendir (&Directory, (const char *) Path);
	if (Result != FR_OK)
	{
		return 0;
	}

	for (;;)
	{
		Result = f_readdir (&Directory, &FileInfo);
		if (Result != FR_OK || FileInfo.fname[0] == '\0')
		{
			break;			// end of directory or error
		}

		// A leading dot means a hidden entry on FAT, and the volume label
		// also arrives as a name starting with a dot.  Neither is a file the
		// user wants to see, and FatFs's hidden/system attribute constants are
		// not all defined in this configuration.
		if (FileInfo.fname[0] == '.')
		{
			continue;
		}

		boolean bIsDirectory = (FileInfo.fattrib & AM_DIR) != 0;

		if (bIsDirectory)
		{
			if (!bIncludeDirectories)
			{
				continue;
			}
		}
		else if (!MatchesFilter (FileInfo.fname, pExtensions))
		{
			continue;
		}

		if (m_nEntryCount >= STORAGE_MAX_ENTRIES)
		{
			break;			// listing is full
		}

		// Directories are marked with a leading '/' so that the menu can
		// tell them apart from files without keeping a parallel array.
		if (bIsDirectory)
		{
			if (strlen (FileInfo.fname) + 2 > STORAGE_NAME_MAX)
			{
				continue;
			}
			m_EntryNames[m_nEntryCount][0] = '/';
			strcpy (&m_EntryNames[m_nEntryCount][1], FileInfo.fname);
		}
		else
		{
			if (strlen (FileInfo.fname) + 1 > STORAGE_NAME_MAX)
			{
				continue;
			}
			strcpy (m_EntryNames[m_nEntryCount], FileInfo.fname);
		}
		m_EntryIsDirectory[m_nEntryCount] = bIsDirectory;
		m_nEntryCount++;
	}

	f_closedir (&Directory);

	return m_nEntryCount;
}

const char *CAtariStorage::GetEntry (unsigned nIndex) const
{
	if (nIndex >= m_nEntryCount)
	{
		return 0;
	}
	return m_EntryNames[nIndex];
}

boolean CAtariStorage::IsEntryDirectory (unsigned nIndex) const
{
	if (nIndex >= m_nEntryCount)
	{
		return FALSE;
	}
	return m_EntryIsDirectory[nIndex];
}
