/*
 * updater.cpp -- install kernel images from a USB drive.
 *
 * See updater.h for the contract.  The copy is staged before any destination is
 * replaced; there is deliberately no backup and no board detection.
 */

#include <stdio.h>
#include <string.h>

#include "port/vfs.h"
#include "update/updater.h"

#define UPDATE_MARKER		"USB:/update.emu"
#define UPDATE_DONE_MARKER	"USB:/update.done"
#define UPDATE_COPY_CHUNK	4096
#define UPDATE_COMPARE_CHUNK	512
#define UPDATE_SPACE_MARGIN	(64 * 1024)

static const char *const UpdateNames[] =
{
	"kernel8.img",
	"kernel8-rpi4.img",
	"kernel_2712.img",
	"armstub8-rpi4.bin"
};

static const unsigned UpdateNameCount =
	(unsigned) (sizeof UpdateNames / sizeof UpdateNames[0]);

static boolean IsSpace (char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static char *Trim (char *pText)
{
	while (*pText != '\0' && IsSpace (*pText))
	{
		pText++;
	}

	char *pEnd = pText + strlen (pText);
	while (pEnd > pText && IsSpace (pEnd[-1]))
	{
		pEnd--;
	}
	*pEnd = '\0';

	return pText;
}

static boolean ReadMarker (char *pCode, unsigned nSize)
{
	if (pCode == 0 || nSize < 2)
	{
		return FALSE;
	}

	FILE *pFile = ATARI_VFS_Open (UPDATE_MARKER, "rb");
	if (pFile == 0)
	{
		return FALSE;
	}

	size_t nRead = ATARI_VFS_Read (pCode, 1, nSize - 1, pFile);
	boolean bError = ATARI_VFS_Error (pFile) != 0;

	ATARI_VFS_Close (pFile);

	if (bError)
	{
		return FALSE;
	}

	pCode[nRead] = '\0';

	return TRUE;
}

static boolean ValidSize (const char *pName, long nSize)
{
	if (nSize <= 0)
	{
		return FALSE;
	}

	if (strcmp (pName, "armstub8-rpi4.bin") == 0)
	{
		return nSize >= 256 && nSize <= 64 * 1024;
	}

	return nSize >= 64 * 1024 && nSize <= 2 * 1024 * 1024;
}

CAtariUpdater::CAtariUpdater (void)
:	m_nFiles (0)
{
	for (unsigned i = 0; i < sizeof m_File / sizeof m_File[0]; i++)
	{
		m_File[i].Name[0] = '\0';
		m_File[i].Size = 0;
	}
}

void CAtariUpdater::Clear (void)
{
	m_nFiles = 0;
}

const char *CAtariUpdater::GetFileName (unsigned nIndex) const
{
	if (nIndex >= m_nFiles)
	{
		return "";
	}

	return m_File[nIndex].Name;
}

boolean CAtariUpdater::Scan (CAtariStorage &Storage, char *pMessage,
			     unsigned nMessageSize)
{
	Clear ();

	if (pMessage == 0 || nMessageSize == 0)
	{
		return FALSE;
	}

	if (!Storage.IsUserVolumeAvailable ())
	{
		snprintf (pMessage, nMessageSize,
			  "USB drive not found; insert it before power-on");
		return FALSE;
	}

	if (!CAtariStorage::FileExists (UPDATE_MARKER))
	{
		snprintf (pMessage, nMessageSize, "No update.emu on the USB drive");
		return FALSE;
	}

	char Code[64];
	if (!ReadMarker (Code, sizeof Code))
	{
		snprintf (pMessage, nMessageSize, "Cannot read update.emu");
		return FALSE;
	}

	if (strcmp (Trim (Code), ATARI_UPDATE_CODE) != 0)
	{
		snprintf (pMessage, nMessageSize,
			  "Update rejected: version code mismatch");
		return FALSE;
	}

	unsigned long long nTotal = 0;

	for (unsigned i = 0; i < UpdateNameCount; i++)
	{
		char Path[STORAGE_PATH_MAX];
		snprintf (Path, sizeof Path, "USB:/%s", UpdateNames[i]);

		if (!CAtariStorage::FileExists (Path))
		{
			continue;
		}

		long nSize = CAtariStorage::FileSize (Path);
		if (!ValidSize (UpdateNames[i], nSize))
		{
			snprintf (pMessage, nMessageSize,
				  "Update rejected: bad size for %s",
				  UpdateNames[i]);
			Clear ();
			return FALSE;
		}

		if (m_nFiles >= sizeof m_File / sizeof m_File[0])
		{
			Clear ();
			snprintf (pMessage, nMessageSize, "Too many update files");
			return FALSE;
		}

		strncpy (m_File[m_nFiles].Name, UpdateNames[i],
			 sizeof m_File[m_nFiles].Name - 1);
		m_File[m_nFiles].Name[sizeof m_File[m_nFiles].Name - 1] = '\0';
		m_File[m_nFiles].Size = nSize;
		m_nFiles++;
		nTotal += (unsigned long long) nSize;
	}

	if (m_nFiles == 0)
	{
		snprintf (pMessage, nMessageSize, "No update files in the USB root");
		return FALSE;
	}

	long nFree = CAtariStorage::GetFreeSpace (STORAGE_VOLUME_SYSTEM);
	if (nFree < 0
	    || (unsigned long long) nFree < nTotal + UPDATE_SPACE_MARGIN)
	{
		Clear ();
		snprintf (pMessage, nMessageSize, "Not enough free space on SD:");
		return FALSE;
	}

	snprintf (pMessage, nMessageSize, "Found %u update file(s)", m_nFiles);

	return TRUE;
}

boolean CAtariUpdater::CopyFile (const char *pSource, const char *pDestination)
{
	FILE *pIn = ATARI_VFS_Open (pSource, "rb");
	if (pIn == 0)
	{
		return FALSE;
	}

	FILE *pOut = ATARI_VFS_Open (pDestination, "wb");
	if (pOut == 0)
	{
		ATARI_VFS_Close (pIn);
		return FALSE;
	}

	unsigned char Buffer[UPDATE_COPY_CHUNK];
	boolean bOK = TRUE;

	for (;;)
	{
		size_t nRead = ATARI_VFS_Read (Buffer, 1, sizeof Buffer, pIn);

		if (nRead > 0)
		{
			if (ATARI_VFS_Write (Buffer, 1, nRead, pOut) != nRead)
			{
				bOK = FALSE;
				break;
			}
		}

		if (nRead < sizeof Buffer)
		{
			if (ATARI_VFS_Error (pIn) != 0 || ATARI_VFS_Eof (pIn) == 0)
			{
				bOK = FALSE;
			}
			break;
		}
	}

	if (ATARI_VFS_Close (pOut) != 0)
	{
		bOK = FALSE;
	}
	if (ATARI_VFS_Close (pIn) != 0)
	{
		bOK = FALSE;
	}

	if (!bOK)
	{
		return FALSE;
	}

	return SameContents (pSource, pDestination);
}

boolean CAtariUpdater::SameContents (const char *pA, const char *pB)
{
	FILE *pFileA = ATARI_VFS_Open (pA, "rb");
	if (pFileA == 0)
	{
		return FALSE;
	}

	FILE *pFileB = ATARI_VFS_Open (pB, "rb");
	if (pFileB == 0)
	{
		ATARI_VFS_Close (pFileA);
		return FALSE;
	}

	unsigned char BufferA[UPDATE_COMPARE_CHUNK];
	unsigned char BufferB[UPDATE_COMPARE_CHUNK];
	boolean bSame = TRUE;

	for (;;)
	{
		size_t nA = ATARI_VFS_Read (BufferA, 1, sizeof BufferA, pFileA);
		size_t nB = ATARI_VFS_Read (BufferB, 1, sizeof BufferB, pFileB);

		if (nA != nB || memcmp (BufferA, BufferB, nA) != 0)
		{
			bSame = FALSE;
			break;
		}

		if (nA == 0)
		{
			if (ATARI_VFS_Error (pFileA) != 0
			    || ATARI_VFS_Error (pFileB) != 0)
			{
				bSame = FALSE;
			}
			break;
		}
	}

	ATARI_VFS_Close (pFileA);
	ATARI_VFS_Close (pFileB);

	return bSame;
}

boolean CAtariUpdater::Apply (CAtariStorage &Storage, char *pMessage,
			      unsigned nMessageSize)
{
	if (pMessage == 0 || nMessageSize == 0 || m_nFiles == 0)
	{
		return FALSE;
	}

	char Source[STORAGE_PATH_MAX];
	char Destination[STORAGE_PATH_MAX];
	char Staged[STORAGE_PATH_MAX];
	char Temp[STORAGE_PATH_MAX];

	/*
	 * Stage every file first.  The destination kernel files remain untouched
	 * until every copy has been verified, so a read or write error cannot leave
	 * a half-written kernel in the boot path.
	 */
	for (unsigned i = 0; i < m_nFiles; i++)
	{
		snprintf (Source, sizeof Source, "USB:/%s", m_File[i].Name);
		snprintf (Destination, sizeof Destination, "SD:/%s", m_File[i].Name);
		snprintf (Staged, sizeof Staged, "%s.new", Destination);

		if (CAtariStorage::FileExists (Staged)
		    && !CAtariStorage::Remove (Staged))
		{
			for (unsigned j = 0; j < i; j++)
			{
				snprintf (Temp, sizeof Temp, "SD:/%s.new",
					  m_File[j].Name);
				if (CAtariStorage::FileExists (Temp))
				{
					CAtariStorage::Remove (Temp);
				}
			}

			snprintf (pMessage, nMessageSize,
				  "Cannot remove old staging file");
			return FALSE;
		}

		if (!CopyFile (Source, Staged))
		{
			for (unsigned j = 0; j <= i; j++)
			{
				snprintf (Temp, sizeof Temp, "SD:/%s.new",
					  m_File[j].Name);
				if (CAtariStorage::FileExists (Temp))
				{
					CAtariStorage::Remove (Temp);
				}
			}

			snprintf (pMessage, nMessageSize,
				  "Copy failed for %s", m_File[i].Name);
			return FALSE;
		}
	}

	/*
	 * Replace only after all staging completed.  There is no backup: a power
	 * loss here is recovered from the original release ZIP on a PC.
	 */
	for (unsigned i = 0; i < m_nFiles; i++)
	{
		snprintf (Destination, sizeof Destination, "SD:/%s", m_File[i].Name);
		snprintf (Staged, sizeof Staged, "%s.new", Destination);

		if (CAtariStorage::FileExists (Destination)
		    && !CAtariStorage::Remove (Destination))
		{
			snprintf (pMessage, nMessageSize,
				  "Cannot replace %s", m_File[i].Name);
			return FALSE;
		}

		if (!CAtariStorage::Rename (Staged, Destination))
		{
			snprintf (pMessage, nMessageSize,
				  "Cannot install %s", m_File[i].Name);
			return FALSE;
		}
	}

	/*
	 * Mark the package as installed by removing the marker.  If removal is
	 * refused, try renaming it to update.done instead.  Both failures are
	 * ignored: the update itself is already complete.
	 */
	if (CAtariStorage::FileExists (UPDATE_DONE_MARKER))
	{
		CAtariStorage::Remove (UPDATE_DONE_MARKER);
	}
	if (!CAtariStorage::Remove (UPDATE_MARKER))
	{
		CAtariStorage::Rename (UPDATE_MARKER, UPDATE_DONE_MARKER);
	}

	Clear ();
	snprintf (pMessage, nMessageSize, "Update complete; restarting");

	return TRUE;
}
