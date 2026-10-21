/*
 * bootlog.h -- start-up diagnostics that survive both a hang and a failure.
 *
 * The problem this solves
 * -----------------------
 * Circle's logger writes to CScreenDevice, which is the firmware's text
 * console.  This emulator then creates its own CBcmFramebuffer, which is a
 * *different* firmware framebuffer, and the console's content is lost -- the new
 * buffer starts black.
 *
 * So anything that goes wrong after that point is reported into a screen
 * nothing is showing: the machine stops with a black display and no explanation.
 * On a board where something is genuinely wrong, that is the worst possible
 * outcome, because the information needed to diagnose it has been thrown away.
 *
 * What this does instead
 * ----------------------
 * Four things, in increasing order of how much they need to already work:
 *
 *   1. Messages are kept in a small fixed buffer, so they survive the change of
 *      framebuffer and can be painted on screen at any time.
 *
 *   2. Each start-up stage paints a full-screen colour as it begins.  A hang is
 *      otherwise indistinguishable from a failure, and a black screen says
 *      nothing; a colour says exactly how far it got.
 *
 *   3. If the file system came up, everything is also written to
 *      SD:/ATARI/bootlog.txt and flushed line by line -- so the log is complete
 *      even if the machine hangs immediately afterwards or the power is cut.
 *      That file can be read on a PC, which is what makes it useful when the
 *      screen is the thing that is broken.
 *
 *   4. The previous session's log is kept and shown with the current one.  This
 *      exists for the awkward case where the machine dies *before* it can write
 *      anything: a corrupted card, or a failure in the SD interface itself.  The
 *      screen then still has something to show.
 *
 * Nothing here allocates.  It has to work when the heap or a subsystem has
 * already failed, and it is written to during bring-up.
 */

#ifndef ATARI_PI_BOOTLOG_H
#define ATARI_PI_BOOTLOG_H

#include <circle/logger.h>
#include <circle/types.h>

#include <stdio.h>

#include "video/video_out.h"

/*
 * Start-up stages, in the order they happen.
 *
 * The values are also the order, so a stage can be compared against another.
 * Each has a colour (see BootLogStageColor) chosen to be unmistakable at a
 * glance and distinct from the others.
 */
enum TBootStage
{
	BootStagePowerOn = 0,		/* the kernel is running at all */
	/*
	 * Circle's own initialisation is done: the serial port, logger, interrupt
	 * system and timer are up and the boot log can be recorded with timestamps.
	 *
	 * This was called "text console" and reported as such.  There is no text
	 * console any more -- it allocated a framebuffer of its own and was replaced
	 * by the emulator's, so it was removed -- and a stage label that names
	 * something which no longer exists sends whoever is investigating a slow boot
	 * looking in the wrong place.  The enumerator keeps its name so that the
	 * colours and the order stay untouched.
	 */
	BootStageScreen,
	BootStageUsb,			/* USB host controller up */
	BootStageSdCard,		/* SD card interface up */
	BootStageFilesystems,		/* SD and USB mounted */
	BootStageSettings,		/* settings read */
	BootStageVideo,			/* framebuffer created */
	BootStageAudio,			/* sound device opened */
	BootStageCore,			/* emulation core started */
	BootStageInput,			/* keyboard and controllers up */
	BootStageRunning,		/* the first frame has been drawn */

	BootStageCount
};

/// A colour identifying STAGE, for the full-screen diagnostic fill.
TAtariColor BootLogStageColor (TBootStage Stage);

/// A short name for STAGE, for the log and the failure screen.
const char *BootLogStageName (TBootStage Stage);

class CAtariStorage;

/*
 * The name of the log file, within the system directory.
 *
 * Two files exist: this one for the running session, and PreviousFileName for
 * the one before it.  See OpenLogFile().
 */
#define BOOTLOG_FILE_NAME	"bootlog.txt"
#define BOOTLOG_PREV_FILE_NAME	"bootlog.prev.txt"

/*
 * Errors that happen while the emulated machine is already running.
 *
 * Kept in a file of its own rather than appended to the boot log, because the
 * two answer different questions.  The boot log describes start-up and is
 * written once; this one grows while the machine runs, and its value is that it
 * survives a hang or a reset.  Mixing them would bury a runtime failure under
 * the start-up record, and would make the boot log's rotation throw away the
 * part someone actually wants to read.
 *
 * Truncated at start-up: it describes the session that just began, and an
 * earlier session's errors would read as if they were this one's.
 */
#define RUNTIME_LOG_FILE_NAME	"runtimelog.txt"

class CBootLog
{
public:
	/*
	 * Buffer sizes.
	 *
	 * Public and declared first because the helper below takes a pointer to an
	 * array of lines, and C++ needs the length at the point of declaration.
	 *
	 * Sized for the messages bring-up produces -- a few dozen -- because the file
	 * is the durable record and this is only what the screen can show.
	 */
	static const unsigned MaxLineLength = 128;
	static const unsigned MaxLines = 48;

	/*
	 * How much of the previous session's log is kept in memory.
	 *
	 * A separate, smaller ring because it is only ever read from a file once and
	 * shown on the failure screen; keeping all of it would waste memory the
	 * emulator needs.
	 */
	static const unsigned MaxPreviousLines = 12;

	CBootLog (void);
	~CBootLog (void);

	/*
	 * Records a message.
	 *
	 * Safe to call at any time from anywhere in bring-up, including before any
	 * display or file system exists: the message is buffered and nothing else
	 * is attempted.
	 */
	void Record (const char *pMessage);

	/// Records a formatted message.
	void Recordf (const char *pFormat, ...);

	/*
	 * Records an error that happened while the machine was running.
	 *
	 * Goes to RUNTIME_LOG_FILE_NAME and to Circle's logger, and does NOT go to
	 * the in-memory boot ring: the ring is start-up history, and a program
	 * failing in a loop would flush it out within a second.  This is the record
	 * that has to survive a hang, a reset, or a power cycle, which is why it is
	 * written and flushed immediately.
	 *
	 * Safe to call before the file exists: it is buffered nowhere and simply
	 * does nothing, so a failure during bring-up cannot recurse into another.
	 */
	void RecordRuntime (const char *pMessage);

	/// Records a formatted runtime error.
	void RecordRuntimef (const char *pFormat, ...);

	/*
	 * Marks the start of STAGE.
	 *
	 * Records the stage, writes it to the log file if one is open, and -- while
	 * painting is on -- fills the screen with the stage's colour, so that a hang
	 * inside the stage leaves that colour visible.
	 *
	 * The fill stops at BootStageRunning, because by then the emulator is drawing
	 * real frames and filling over them would destroy the picture.
	 */
	void Stage (TBootStage Stage);

	/*
	 * Attaches the display, so that stage colours and failure reports can be
	 * drawn.
	 *
	 * \param pVideo The framebuffer wrapper, or NULL to record only.
	 * \param pLogger Where messages also go, or NULL.
	 */
	void Attach (CAtariVideoOut *pVideo, CLogger *pLogger);

	/*
	 * Opens the log file, and rotates the previous session's log.
	 *
	 * Called as soon as the SD card is mounted -- which is the earliest point a
	 * file can be written, and deliberately before the framebuffer exists, so
	 * that a failure in between is still recorded.
	 *
	 * The rotation is what makes a failure *before this call* diagnosable: the
	 * previous session's log is moved aside rather than overwritten, so if this
	 * session dies before writing anything, the last one is still on the card.
	 *
	 * From here on every line is flushed as it is written, so a hang or a power
	 * cut loses at most the line in progress.
	 *
	 * \return TRUE if the log file is open for writing.
	 */
	boolean OpenLogFile (const CAtariStorage &Storage);

	/*
	 * Reads the tail of the previous session's log, so that it can be shown
	 * alongside this one.
	 *
	 * Needed because the on-screen report is often the only thing a user can
	 * get at: if this session failed before writing anything, the screen would
	 * otherwise have nothing to show.
	 */
	void ReadPreviousLog (const CAtariStorage &Storage);

	/// Turns the full-screen stage fill off, leaving only recording.
	/// \note Called when the first real frame is drawn, so that the diagnostic
	///	  cannot overwrite the emulator's output.
	void StopPainting (void);

	/*
	 * Draws the recorded messages over the current frame, with a heading naming
	 * the stage that was reached and, if it was read, the previous session's
	 * log.
	 *
	 * This is what a failed start-up calls: it replaces a black screen with the
	 * reason for it.
	 */
	void PaintReport (const char *pTitle);

	/// Writes a final message and closes the log file, flushing everything.
	/// \note Called on the way out of a failed start-up, so that the file
	///	  ends with the reason rather than simply stopping.
	void CloseLogFile (const char *pFinalMessage);

	/// \return The last stage passed to Stage().
	TBootStage GetStage (void) const	{ return m_Stage; }

	/// \return The path of the open log file, or "" if none is open.
	const char *GetLogPath (void) const	{ return m_LogPath; }

	/// \return How many lines of the previous session's log were kept.
	unsigned GetPreviousLineCount (void) const	{ return m_nPreviousCount; }

	/*
	 * The single instance, created on first use.
	 *
	 * A function-local static rather than a global object, and deliberately not
	 * a member of the kernel class either.  It holds about 8 KB of buffers, and
	 * the kernel object is a local in main() -- so a member would put those 8 KB
	 * on Circle's 128 KB stack, which the emulation core's call chains also need.
	 * A function-local static lives in .bss instead.
	 *
	 * Every module can reach it, including ones brought up before the kernel has
	 * finished constructing.
	 */
	static CBootLog *Get (void);

private:
	/*
	 * Adds one already-formatted line to a ring of lines.
	 *
	 * \param pRing The ring, as a pointer to its first line.  The type carries
	 *		the line length, which is what makes pRing[Index] address
	 *		whole lines rather than bytes.
	 * \param nLines How many lines the ring holds.
	 *
	 * Static because it is pure bookkeeping with no state of its own.
	 */
	static void AppendToRing (char (*pRing)[MaxLineLength], unsigned nLines,
				  unsigned *pCount, unsigned *pNext, const char *pLine);

	/* Writes one line to the log file, if open.  Flushes it. */
	void WriteToFile (const char *pLine);

	/* Writes one line to the runtime log file, if open.  Flushes it. */
	void WriteToRuntimeFile (const char *pLine);

	/*
	 * The current session's messages: a fixed ring, no allocation.
	 */
	char		m_Lines[MaxLines][MaxLineLength];
	unsigned	m_nCount;
	unsigned	m_nNext;

	/*
	 * The previous session's last few lines.
	 */
	char		m_Previous[MaxPreviousLines][MaxLineLength];
	unsigned	m_nPreviousCount;
	unsigned	m_nPreviousNext;

	TBootStage	m_Stage;
	boolean		m_bPainting;
	boolean		m_bHaveDisplay;
	boolean		m_bHaveFile;

	/*
	 * Whether Circle's timer exists yet.
	 *
	 * Messages are timestamped from it, but the timer is not up until part-way
	 * through CKernel::Initialize(), and this is recorded from before that --
	 * the very first line is written before the timer exists.  CTimer::Get()
	 * asserts when there is no timer, so asking for the time too early would
	 * abort the kernel at start-up, which is the exact failure this whole
	 * mechanism exists to report.  Set by Attach().
	 */
	boolean		m_bHaveTimer;

	FILE	       *m_pFile;
	char		m_LogPath[160];

	/*
	 * The runtime log: errors raised while the emulated machine is running.
	 *
	 * Separate handle and path from the boot log, for the reasons given at
	 * RUNTIME_LOG_FILE_NAME.
	 */
	FILE	       *m_pRuntimeFile;
	char		m_RuntimePath[160];

	CAtariVideoOut	*m_pVideo;
	CLogger		*m_pLogger;

	static CBootLog *s_pThis;
};

#endif
