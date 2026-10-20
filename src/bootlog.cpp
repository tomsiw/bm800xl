/*
 * bootlog.cpp -- see bootlog.h for what this is for and why it exists.
 */

#include "bootlog.h"

#include <circle/logger.h>
#include <circle/timer.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "port/vfs.h"
#include "storage/storage.h"

static const char FromBootLog[] = "boot";

CBootLog *CBootLog::s_pThis = 0;

/*
 * The one instance, created on first use.
 *
 * A function-local static, so its buffers live in .bss rather than on main()'s
 * stack -- see the note in the header.  Concurrent initialisation is not a
 * concern: the first call happens during single-threaded start-up.
 */
CBootLog *CBootLog::Get (void)
{
	static CBootLog Instance;

	return &Instance;
}

/*
 * One colour per start-up stage.
 *
 * These are the diagnostic.  If the machine stops responding, the colour left on
 * screen names the stage it died in, which turns "black screen, no idea" into a
 * specific place to look.  They are therefore chosen to be unmistakable and easy
 * to tell apart, and deliberately not the colours the emulator itself draws -- an
 * Atari screen is mostly blue, so blue means the emulator is running rather than
 * that start-up stopped.
 *
 * Read them as a traffic light that got further: red, then orange, then yellows
 * and greens, then white at the end.
 */
TAtariColor BootLogStageColor (TBootStage Stage)
{
	(void) Stage;

	/*
	 * Black, for every stage.
	 *
	 * These used to be a traffic light of bright colours, one per start-up
	 * stage, so that a machine that stopped could be diagnosed from the colour
	 * left on screen.  That was useful while this was being brought up on
	 * hardware, and it is not what a console should do: a real Atari shows its
	 * own screen and nothing else.
	 *
	 * Painting black rather than not painting at all is deliberate.  The stage
	 * paint is what clears whatever the firmware left in the framebuffer, so a
	 * turned-off screen is guaranteed rather than merely likely.  The stage
	 * names and timings still go to the log file, which is where they are
	 * actually useful now.
	 *
	 * Error reports are unaffected: PaintReport() uses its own colours and is
	 * still shown, because a failure the user cannot see is worse than a
	 * colourful screen.
	 */
	return ATARI_COLOR_RGB (0x00, 0x00, 0x00);
}

const char *BootLogStageName (TBootStage Stage)
{
	switch (Stage)
	{
	case BootStagePowerOn:		return "power-on (Circle started)";
	case BootStageScreen:		return "text console";
	case BootStageUsb:		return "USB host controller";
	case BootStageSdCard:		return "SD card interface";
	case BootStageFilesystems:	return "file systems mounted";
	case BootStageSettings:		return "settings loaded";
	case BootStageVideo:		return "framebuffer created";
	case BootStageAudio:		return "sound device";
	case BootStageCore:		return "emulation core started";
	case BootStageInput:		return "keyboard and controllers";
	case BootStageRunning:		return "running (first frame drawn)";
	default:			return "unknown";
	}
}

CBootLog::CBootLog (void)
:	m_nCount (0),
	m_nNext (0),
	m_nPreviousCount (0),
	m_nPreviousNext (0),
	m_Stage (BootStagePowerOn),
	m_bPainting (TRUE),
	m_bHaveDisplay (FALSE),
	m_bHaveFile (FALSE),
	m_bHaveTimer (FALSE),
	m_pFile (0),
	m_pRuntimeFile (0),
	m_pVideo (0),
	m_pLogger (0)
{
	s_pThis = this;

	memset (m_Lines, 0, sizeof m_Lines);
	memset (m_Previous, 0, sizeof m_Previous);
	memset (m_LogPath, 0, sizeof m_LogPath);
	memset (m_RuntimePath, 0, sizeof m_RuntimePath);
}

CBootLog::~CBootLog (void)
{
	CloseLogFile ("emulator shut down");
}

void CBootLog::Attach (CAtariVideoOut *pVideo, CLogger *pLogger)
{
	m_pVideo = pVideo;
	m_pLogger = pLogger;
	m_bHaveDisplay = (pVideo != 0);

	/*
	 * Attach() is called once Circle is up, so the timer exists from here on and
	 * messages can be timestamped.  It is called twice -- once without a display,
	 * just to mark the timer as available, and once with it.
	 */
	m_bHaveTimer = (CTimer::Get () != 0);

	if (m_bHaveDisplay)
	{
		/* The display is about to be usable, so the colour left on screen from
		 * here on means something. */
		m_pVideo->FillSolid (BootLogStageColor (m_Stage));
	}
}

void CBootLog::AppendToRing (char (*pRing)[MaxLineLength], unsigned nLines,
			     unsigned *pCount, unsigned *pNext, const char *pLine)
{
	/*
	 * pRing is a pointer to an array of lines, so pRing[*pNext] is a whole line.
	 * Indexing a plain char * here would write into the middle of a line, which
	 * is why the parameter carries its length in its type.
	 */
	char *pTarget = pRing[*pNext];

	strncpy (pTarget, pLine, MaxLineLength - 1);
	pTarget[MaxLineLength - 1] = '\0';

	*pNext = (*pNext + 1) % nLines;
	if (*pCount < nLines)
	{
		(*pCount)++;
	}
}

void CBootLog::WriteToFile (const char *pLine)
{
	if (!m_bHaveFile || m_pFile == 0)
	{
		return;
	}

	/*
	 * Every line is flushed as it is written.
	 *
	 * That is the entire point of the file: it has to be complete when the
	 * machine hangs a moment later.  Buffering it would mean the last and most
	 * interesting lines were still in memory when everything stopped.
	 *
	 * The cost is a card write per line during bring-up, which is a few dozen
	 * writes once, at start-up.  That is not worth optimising.
	 */
	ATARI_VFS_Write (pLine, 1, strlen (pLine), m_pFile);
	ATARI_VFS_Write ("\r\n", 1, 2, m_pFile);
	ATARI_VFS_Flush (m_pFile);
}

void CBootLog::WriteToRuntimeFile (const char *pLine)
{
	if (m_pRuntimeFile == 0 || pLine == 0)
	{
		return;
	}

	/*
	 * Written and flushed one line at a time, for the same reason as the boot
	 * log: the interesting case is the machine hanging immediately afterwards,
	 * and a buffered line would never reach the card.
	 */
	ATARI_VFS_Write (pLine, 1, strlen (pLine), m_pRuntimeFile);
	ATARI_VFS_Write ("\r\n", 1, 2, m_pRuntimeFile);
	ATARI_VFS_Flush (m_pRuntimeFile);
}

void CBootLog::RecordRuntime (const char *pMessage)
{
	if (pMessage == 0)
	{
		return;
	}

	char Line[MaxLineLength];

	if (m_bHaveTimer)
	{
		snprintf (Line, sizeof Line, "[%8lu ms] %s",
			  (unsigned long) (CTimer::Get ()->GetClockTicks64 () / 1000),
			  pMessage);
	}
	else
	{
		snprintf (Line, sizeof Line, "[        ] %s", pMessage);
	}

	WriteToRuntimeFile (Line);

	/*
	 * The file only.  Nothing here goes to the screen.
	 *
	 * This used to forward to Circle's logger as well, on the theory that a
	 * failing machine showing nothing was worse than one showing why.  That was
	 * wrong for a log that carries a line per frame boundary or per disk read:
	 * the logger draws to the text console, which is on screen, so hundreds of
	 * diagnostic lines were painted over the emulated machine's picture.  The
	 * result was a screen flickering between the Atari and a wall of text --
	 * worse than either, and it hid the machine completely.
	 *
	 * Diagnostic information belongs in the file.  Only a failure that stops
	 * start-up is shown on screen, and that goes through PaintReport() instead,
	 * which paints over the picture deliberately and once.
	 */
}

void CBootLog::RecordRuntimef (const char *pFormat, ...)
{
	if (pFormat == 0)
	{
		return;
	}

	char Buffer[MaxLineLength];
	va_list args;

	va_start (args, pFormat);
	vsnprintf (Buffer, sizeof Buffer, pFormat, args);
	va_end (args);

	RecordRuntime (Buffer);
}

void CBootLog::Record (const char *pMessage)
{
	if (pMessage == 0)
	{
		return;
	}

	if (m_pLogger != 0)
	{
		/*
		 * Only when a logger was attached on purpose.
		 *
		 * In the released build nothing is attached, because Circle's logger
		 * draws to the text console and those messages then sit on screen over
		 * the emulated machine's picture.  A real Atari shows its own screen and
		 * nothing else, so the boot log goes to its file on the SD card only.
		 *
		 * Attaching a logger is still supported for bring-up on new hardware,
		 * which is why this is a parameter rather than being removed.
		 */
		m_pLogger->Write (FromBootLog, LogNotice, "%s", pMessage);
	}

	/*
	 * Timestamped, but only once Circle's timer exists.
	 *
	 * The earliest messages are recorded before CKernel::Initialize() has
	 * started the timer, and CTimer::Get() asserts when there is none -- so
	 * asking for the time too early would abort the kernel at start-up.  That
	 * would be this diagnostic causing the very failure it exists to report, so
	 * the check is not optional.
	 */
	char Line[MaxLineLength];

	if (m_bHaveTimer)
	{
		/* Microseconds since boot, from a 64-bit counter that does not wrap in
		 * any realistic session. */
		snprintf (Line, sizeof Line, "[%8lu ms] %s",
			  (unsigned long) (CTimer::Get ()->GetClockTicks64 () / 1000),
			  pMessage);
	}
	else
	{
		snprintf (Line, sizeof Line, "[   boot ] %s", pMessage);
	}

	AppendToRing (m_Lines, MaxLines, &m_nCount, &m_nNext, Line);
	WriteToFile (Line);
}

void CBootLog::Recordf (const char *pFormat, ...)
{
	char Buffer[MaxLineLength];

	va_list Arguments;
	va_start (Arguments, pFormat);
	vsnprintf (Buffer, sizeof Buffer, pFormat, Arguments);
	va_end (Arguments);

	Record (Buffer);
}

void CBootLog::Stage (TBootStage Stage)
{
	m_Stage = Stage;

	{
		char Line[MaxLineLength];
		snprintf (Line, sizeof Line, "--- stage: %s ---", BootLogStageName (Stage));
		AppendToRing (m_Lines, MaxLines, &m_nCount, &m_nNext, Line);
		WriteToFile (Line);
	}

	if (!m_bHaveDisplay || !m_bPainting || m_pVideo == 0)
	{
		return;
	}

	/* BootStageRunning means real frames are being drawn; painting over them
	 * would be the diagnostic destroying the thing it is diagnosing. */
	if (Stage >= BootStageRunning)
	{
		m_bPainting = FALSE;
		return;
	}

	m_pVideo->FillSolid (BootLogStageColor (Stage));
}

/*
 * Opens the log file, rotating the previous session's aside first.
 *
 * The rotation is the part that matters.  If this session dies before it can
 * write anything -- a failure in the SD interface, or the very next thing to
 * happen -- then the file left on the card is the *previous* session's, which
 * still says how far that one got.  Without it, opening the log would truncate
 * the only evidence there was.
 */
boolean CBootLog::OpenLogFile (const CAtariStorage &Storage)
{
	char Path[160];
	char PreviousPath[160];

	if (m_bHaveFile)
	{
		return TRUE;
	}

	Storage.MakeSystemPath (Path, sizeof Path, BOOTLOG_FILE_NAME);
	Storage.MakeSystemPath (PreviousPath, sizeof PreviousPath, BOOTLOG_PREV_FILE_NAME);

	if (Path[0] == '\0')
	{
		return FALSE;
	}

	/*
	 * Rotate.  There is no rename in this file layer, so the previous log is
	 * copied to the ".prev" name and then truncated.
	 *
	 * Copying rather than moving is what keeps it correct: a failed copy leaves
	 * the original in place, so the worst case is a stale ".prev" rather than a
	 * lost log.
	 */
	{
		FILE *pIn = ATARI_VFS_Open (Path, "rb");

		if (pIn != 0)
		{
			FILE *pOut = ATARI_VFS_Open (PreviousPath, "wb");

			if (pOut != 0)
			{
				char Copy[256];
				size_t nRead;

				/* Bounded, so a huge or corrupt file cannot stall start-up.
				 * The tail is what matters, and the tail is what is read back. */
				while ((nRead = ATARI_VFS_Read (Copy, 1, sizeof Copy, pIn)) > 0)
				{
					ATARI_VFS_Write (Copy, 1, nRead, pOut);
				}
				ATARI_VFS_Close (pOut);
			}
			ATARI_VFS_Close (pIn);
		}
	}

	strncpy (m_LogPath, Path, sizeof m_LogPath - 1);
	m_LogPath[sizeof m_LogPath - 1] = '\0';

	m_pFile = ATARI_VFS_Open (Path, "wb");
	if (m_pFile == 0)
	{
		m_LogPath[0] = '\0';
		return FALSE;
	}

	/*
	 * The runtime log is opened here too, because this is the one place that
	 * has the storage object needed to build its path.  Failing to open it is
	 * not fatal: it only means runtime errors go unrecorded, and refusing to
	 * start the emulator over that would be a poor trade.
	 */
	{
		char RuntimePath[160];

		Storage.MakeSystemPath (RuntimePath, sizeof RuntimePath,
					RUNTIME_LOG_FILE_NAME);

		if (RuntimePath[0] != '\0')
		{
			m_pRuntimeFile = ATARI_VFS_Open (RuntimePath, "wb");

			if (m_pRuntimeFile != 0)
			{
				strncpy (m_RuntimePath, RuntimePath,
					 sizeof m_RuntimePath - 1);
				m_RuntimePath[sizeof m_RuntimePath - 1] = '\0';
			}
		}
	}

	m_bHaveFile = TRUE;

	/* Everything buffered before the card was mounted, in order.  These are the
	 * messages that would otherwise be lost -- they happened before there was
	 * anywhere to write them. */
	{
		unsigned nFirst = (m_nCount < MaxLines) ? 0 : m_nNext;

		WriteToFile ("=== boot log (entries before this point were buffered "
			     "until the SD card was mounted) ===");

		for (unsigned i = 0; i < m_nCount; i++)
		{
			WriteToFile (m_Lines[(nFirst + i) % MaxLines]);
		}
	}

	return TRUE;
}

void CBootLog::ReadPreviousLog (const CAtariStorage &Storage)
{
	char PreviousPath[160];

	Storage.MakeSystemPath (PreviousPath, sizeof PreviousPath, BOOTLOG_PREV_FILE_NAME);
	if (PreviousPath[0] == '\0')
	{
		return;
	}

	FILE *pFile = ATARI_VFS_Open (PreviousPath, "rb");
	if (pFile == 0)
	{
		return;
	}

	m_nPreviousCount = 0;
	m_nPreviousNext = 0;

	/*
	 * Read the file in chunks and split it into lines, so that the ring ends up
	 * holding the *last* MaxPreviousLines lines rather than the first.  The end
	 * of a log is where the reason is.
	 *
	 * There is no character-read function in the file layer, so a partial line
	 * is carried across reads.  A line longer than the buffer is truncated
	 * rather than split, which is acceptable for a diagnostic.
	 */
	{
		char Chunk[256];
		char Line[MaxLineLength];
		unsigned nUsed = 0;
		size_t nRead;

		while ((nRead = ATARI_VFS_Read (Chunk, 1, sizeof Chunk, pFile)) > 0)
		{
			for (size_t i = 0; i < nRead; i++)
			{
				char Ch = Chunk[i];

				if (Ch == '\n')
				{
					Line[nUsed] = '\0';

					/* Drop the carriage return from the CRLF the writer used. */
					if (nUsed > 0 && Line[nUsed - 1] == '\r')
					{
						Line[nUsed - 1] = '\0';
					}

					if (Line[0] != '\0')
					{
						AppendToRing (m_Previous, MaxPreviousLines,
							      &m_nPreviousCount,
							      &m_nPreviousNext, Line);
					}
					nUsed = 0;
					continue;
				}

				if (nUsed < MaxLineLength - 1)
				{
					Line[nUsed++] = Ch;
				}
			}
		}

		/* A file that does not end in a newline still has a last line. */
		if (nUsed > 0)
		{
			Line[nUsed] = '\0';
			if (nUsed > 0 && Line[nUsed - 1] == '\r')
			{
				Line[nUsed - 1] = '\0';
			}
			if (Line[0] != '\0')
			{
				AppendToRing (m_Previous, MaxPreviousLines,
					      &m_nPreviousCount, &m_nPreviousNext, Line);
			}
		}
	}

	ATARI_VFS_Close (pFile);
}

void CBootLog::StopPainting (void)
{
	m_bPainting = FALSE;
}

void CBootLog::CloseLogFile (const char *pFinalMessage)
{
	/*
	 * The two files are closed independently: the runtime log can be open when
	 * the boot log is not (the boot log path is built first and can fail on its
	 * own), and leaving a handle open would lose its last buffered bytes on a
	 * card that is still mounted.
	 */
	if (m_pRuntimeFile != 0)
	{
		ATARI_VFS_Close (m_pRuntimeFile);
		m_pRuntimeFile = 0;
		m_RuntimePath[0] = '\0';
	}

	if (!m_bHaveFile || m_pFile == 0)
	{
		return;
	}

	if (pFinalMessage != 0)
	{
		/* Bypasses Record(): this goes to the file only, because the buffer is
		 * about to be shown on screen and this line is already the reason. */
		WriteToFile (pFinalMessage);
	}

	WriteToFile ("=== end of boot log ===");

	ATARI_VFS_Close (m_pFile);
	m_pFile = 0;
	m_bHaveFile = FALSE;
}

void CBootLog::PaintReport (const char *pTitle)
{
	if (!m_bHaveDisplay || m_pVideo == 0)
	{
		/* No display: nothing can be shown.  The messages are in the log file
		 * and in the buffer for a serial console or a debugger. */
		return;
	}

	const TAtariColor Background = ATARI_COLOR_RGB (0x10, 0x10, 0x18);
	const TAtariColor Heading    = ATARI_COLOR_RGB (0xff, 0x60, 0x60);
	const TAtariColor Text       = ATARI_COLOR_RGB (0xd0, 0xd0, 0xd0);
	const TAtariColor Dim        = ATARI_COLOR_RGB (0x80, 0x80, 0x90);

	/* Start from a clean screen: whatever is there is not to be trusted. */
	m_pVideo->FillSolid (Background);

	const unsigned nLineHeight = m_pVideo->GetTextHeight ();
	const unsigned nMarginX = 8;
	unsigned nY = 8;

	if (pTitle != 0)
	{
		m_pVideo->DrawText (nMarginX, nY, Heading, pTitle, TRUE);
		nY += nLineHeight * 3;
	}

	/* The stage reached is the single most useful fact, so it leads. */
	{
		char Line[MaxLineLength];
		snprintf (Line, sizeof Line, "Reached: %s", BootLogStageName (m_Stage));
		m_pVideo->DrawText (nMarginX, nY, Heading, Line);
		nY += nLineHeight * 2;
	}

	/* Where the log file is, so it can be read on a PC.  This is the most
	 * actionable line on the screen when the screen itself is the problem. */
	if (m_LogPath[0] != '\0')
	{
		char Line[MaxLineLength];
		snprintf (Line, sizeof Line, "Full log: %s", m_LogPath);
		m_pVideo->DrawText (nMarginX, nY, Dim, Line);
		nY += nLineHeight;
	}
	else
	{
		m_pVideo->DrawText (nMarginX, nY, Dim,
				    "No log file: the SD card was not mounted");
		nY += nLineHeight;
	}
	nY += nLineHeight / 2;

	/* The previous session's tail, first, because if this session failed very
	 * early that is the only history there is. */
	if (m_nPreviousCount > 0)
	{
		m_pVideo->DrawText (nMarginX, nY, Dim,
				    "--- previous session (end of its log) ---");
		nY += nLineHeight;

		unsigned nFirst = (m_nPreviousCount < MaxPreviousLines)
				  ? 0 : m_nPreviousNext;

		for (unsigned i = 0; i < m_nPreviousCount; i++)
		{
			if (nY + nLineHeight > m_pVideo->GetHeight ())
			{
				return;
			}

			m_pVideo->DrawText (nMarginX, nY, Dim,
					    m_Previous[(nFirst + i) % MaxPreviousLines]);
			nY += nLineHeight;
		}

		nY += nLineHeight / 2;
	}

	/* Then this session's messages, oldest first, in the order they happened. */
	m_pVideo->DrawText (nMarginX, nY, Dim, "--- this session ---");
	nY += nLineHeight;

	unsigned nFirst = (m_nCount < MaxLines) ? 0 : m_nNext;

	for (unsigned i = 0; i < m_nCount; i++)
	{
		const char *pMessage = m_Lines[(nFirst + i) % MaxLines];

		if (pMessage[0] == '\0')
		{
			continue;
		}

		/* Stop before running off the bottom of the screen; a partial list is
		 * better than text drawn outside the picture. */
		if (nY + nLineHeight > m_pVideo->GetHeight ())
		{
			m_pVideo->DrawText (nMarginX, nY, Dim, "... (see the log file)");
			break;
		}

		m_pVideo->DrawText (nMarginX, nY, Text, pMessage);
		nY += nLineHeight;
	}
}
