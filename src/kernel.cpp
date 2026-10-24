//
// kernel.cpp
//
// The emulator's main application class.  See kernel.h.
//
// Start-up order matters and is worth stating once:
//
//   1. The screen and logger come up first, so that anything that goes wrong
//      afterwards can be seen.
//   2. The interrupt system, timer and USB follow.  USB needs the timer and the
//      interrupt system; the SD card needs the same.
//   3. The SD card is mounted, because the settings and the OS ROM images live
//      there.  A failure here is fatal: without the ROMs there is no machine.
//   4. Settings are read, then applied to the emulation core, which is started
//      with them.  The audio device is opened before the core because the core
//      negotiates the sample rate with it.
//   5. Finally the menu is given its pointers and the frame loop starts.
//
#include "kernel.h"

#include <circle/logger.h>
#include <circle/memory.h>
#include <circle/string.h>
#include <circle/util.h>

#include "host_services.h"
#include "audio_out.h"

#include <errno.h>

/*
 * NOTE: routing newlib's reentrant allocator (_malloc_r and friends) directly
 * onto Circle's was tried here and REVERTED.  Circle's CHeapAllocator is not
 * reentrant, and newlib's internals allocate from contexts it never expected --
 * the scheduler's tasks and the USB driver task -- which produced 2-3 fps, a
 * 45-second USB timeout and a frozen OSD.
 *
 * The narrower approach below is used instead: newlib keeps its own allocator
 * and its own bookkeeping, but the memory it grows into is carved from Circle's
 * heap.  Only the occasional _sbrk call touches Circle's allocator, so the
 * reentrancy exposure is rare rather than on every allocation.
 */

/*
 * newlib's heap, carved from Circle's.
 *
 * Without this, _sbrk comes from libnosys and can never return memory, so every
 * allocation made inside newlib -- strdup, asprintf, setvbuf, the float
 * formatter for large conversions -- fails with NULL.  Code that does not check
 * writes through that NULL, and which address gets corrupted then depends only
 * on the link layout: the exact shape of the fault this port has been chasing.
 *
 * A single block is taken from Circle's heap on first use and handed to newlib
 * in the linear fashion _sbrk requires.  Once newlib owns it, its own allocator
 * manages all the small allocations, which is why the non-reentrant Circle heap
 * is touched at most a few times per boot.
 */
#define NEWLIB_HEAP_BYTES	(256 * 1024)

extern "C" void *malloc (size_t);

static u8 *s_pNewlibHeap = 0;
static size_t s_nNewlibHeapUsed = 0;

extern "C" void *_sbrk (ptrdiff_t nBytes)
{
	if (s_pNewlibHeap == 0) {
		s_pNewlibHeap = (u8 *) malloc (NEWLIB_HEAP_BYTES);
		if (s_pNewlibHeap == 0) {
			errno = ENOMEM;
			return (void *) -1;
		}
	}

	if (nBytes < 0 || s_nNewlibHeapUsed + (size_t) nBytes > NEWLIB_HEAP_BYTES) {
		errno = ENOMEM;
		return (void *) -1;
	}

	u8 *pResult = s_pNewlibHeap + s_nNewlibHeapUsed;
	s_nNewlibHeapUsed += (size_t) nBytes;

	return pResult;
}

#include "input/joystick.h"
#include "input/gpio_joystick.h"
#include "input/usb_keymap.h"
#include "osd/atari_font.h"

#include "vfs.h"

#include <string.h>

static const char FromKernel[] = "kernel";

// Where settings, snapshots and the ROM images live on the SD card.
static const char SystemDirectory[] = "/ATARI";

// Runs at 1 MHz, so this counts microseconds.  See GetHostClock().
static CAtariKernel *s_pKernelForClock = 0;

/* ---------------------------------------------------------------------------
 * CKernel -- the Circle boilerplate every application needs
 *
 * Circle has no base kernel class; each application brings its own, and this is
 * the standard shape: construct the devices, bring them up in dependency order,
 * and hand control to Run().
 * ------------------------------------------------------------------------- */

// The initializer order below matches the member declaration order in kernel.h,
// which is also the order Circle requires.
/*
 * Runtime log sink: the C++ side of the function pointer the platform layer
 * calls.  extern "C" so that its type matches atari_runtime_log_fn exactly
 * rather than relying on the two languages agreeing about linkage.
 */
extern "C" void RuntimeLogSink (const char *pMessage)
{
	CBootLog::Get ()->RecordRuntime (pMessage);
}

/*
 * Exception reporting.
 *
 * The screen dump is the only witness Circle normally leaves, and it is lost the
 * moment the machine is powered off.  These write the same thing to the runtime
 * log first, flushed line by line, and then hand over to the base handler so the
 * on-screen behaviour is exactly as it was.
 */
/*
 * A log target that writes Circle's own messages into our runtime log.
 *
 * Circle's drivers report through CLogger -- the HDMI sound device, for one,
 * explains exactly why it refused to start -- but the default target is the
 * framebuffer console, which paints text over the Atari picture and is lost as
 * soon as the machine is powered off.  Raising the screen threshold to silence
 * them also silenced the explanations.
 *
 * Installing this as the logger's target keeps the screen clean and puts those
 * messages in runtimelog.txt, where they can be read afterwards.  Lines are
 * forwarded as they arrive and prefixed so they are distinguishable from ours.
 */
class CRuntimeLogTarget : public CDevice
{
public:
	int Write (const void *pBuffer, size_t nCount) override
	{
		const char *pText = (const char *) pBuffer;
		size_t nStart = 0;

		for (size_t i = 0; i < nCount; i++)
		{
			if (pText[i] != '\n')
			{
				continue;
			}

			if (i > nStart)
			{
				Forward (pText + nStart, i - nStart);
			}

			nStart = i + 1;
		}

		// A final chunk without a newline still deserves to be recorded.
		if (nStart < nCount)
		{
			Forward (pText + nStart, nCount - nStart);
		}

		return (int) nCount;
	}

private:
	void Forward (const char *pText, size_t nLength)
	{
		// The runtime log writes whole lines of bounded length.
		static const size_t MaxLength = 160;
		char Line[MaxLength + 1];

		if (nLength > MaxLength)
		{
			nLength = MaxLength;
		}

		memcpy (Line, pText, nLength);
		Line[nLength] = '\0';

		if (!s_bRuntimeLogReady)
		{
			// Nothing to write to yet; the log file opens later than the first
			// messages, and those are boot noise we can afford to lose.
			return;
		}

		CBootLog::Get ()->RecordRuntimef ("circle: %s", Line);
	}

public:
	static void SetReady (boolean bReady) { s_bRuntimeLogReady = bReady; }

private:
	static boolean s_bRuntimeLogReady;
};

boolean CRuntimeLogTarget::s_bRuntimeLogReady = FALSE;

static CRuntimeLogTarget s_RuntimeLogTarget;

/*
 * Accumulated per-phase frame timings, in microseconds.
 *
 * Reported as averages because a single frame is too noisy to read, and because
 * the point is to find which phase dominates the budget, not to chase spikes.
 */
struct TFrameTimings
{
	u64 nEmulation;
	u64 nFill;
	u64 nMenu;
	u64 nPresent;
	unsigned nFrames;
};

static TFrameTimings s_Timings;

void CAtariExceptionHandler::Throw (unsigned nException, TAbortFrame *pFrame)
{
	CBootLog::Get ()->RecordRuntimef ("*** EXCEPTION %u -- the machine has "
					 "halted; this is the last line it wrote",
					 nException);

	/*
	 * Leave a note for the next boot.
	 *
	 * This is deliberately the first thing done: one byte written to a file
	 * beside the settings.  The next boot reads it, ejects all media, restores
	 * the machine defaults and saves them, so a disk image that crashes the
	 * emulator cannot trap the user in a crash loop -- the image is re-mounted
	 * from the settings before any menu can be reached.
	 *
	 * Nothing more is attempted here on purpose.  Writing settings would mean
	 * trusting the FAT layer while the CPU is in an unknown state; one byte is
	 * the smallest footprint that still carries the message.
	 */
	ATARI_MarkCrashed ();

	/*
	 * What the emulated machine was doing.  The host fault address says where
	 * our CPU jumped; this says where the Atari's CPU was, which is the thing
	 * that identifies the ROM routine the crash is hiding behind.
	 */
	ATARI_LogCrashState ();

	/*
	 * The faulting address, which is the only thing that identifies the culprit.
	 *
	 * An undefined instruction says the CPU jumped somewhere that is not code,
	 * but not where or why.  ELR_EL1 is the address of that instruction, and
	 * resolving it against the symbol table names the function outright -- a
	 * virtual dispatch through a corrupted object, or a return address that a
	 * buffer overran, both become obvious once the address is known.
	 *
	 * SP_EL1 is included because a stack pointer near the bottom of the kernel
	 * stack means the fault is a stack overrun rather than a stray pointer, and
	 * the two need looking for in completely different places.
	 */
	if (pFrame != 0)
	{
		CBootLog::Get ()->RecordRuntimef (
			"    fault PC 0x%08lx  ESR 0x%08lx  FAR 0x%08lx",
			(unsigned long) pFrame->elr_el1,
			(unsigned long) pFrame->esr_el1,
			(unsigned long) pFrame->far_el1);

		CBootLog::Get ()->RecordRuntimef (
			"    SP_EL1  0x%08lx  SP_EL0 0x%08lx  SPSR 0x%08lx",
			(unsigned long) pFrame->sp_el1,
			(unsigned long) pFrame->sp_el0,
			(unsigned long) pFrame->spsr_el1);
	}

	CExceptionHandler::Throw (nException, pFrame);
}

CKernel::CKernel (void)
:	m_Serial (&m_Interrupt, 0),
	m_Timer (&m_Interrupt),
	/*
	 * Errors and panics only.
	 *
	 * At the time this was written Circle's logger drew to the text console, so
	 * at its default level every notice and warning from the framework and from
	 * this kernel appeared over the emulated machine's picture -- including the
	 * version banner and the USB enumeration chatter.  The console has since been
	 * removed and the logger now targets the serial port, where the chatter costs
	 * nothing; the level is kept as it is because a real Atari shows its own
	 * screen and nothing else, and because it also governs how much the file log
	 * keeps.
	 *
	 * LogError is deliberately not LogNone: a failure that the user cannot see
	 * is worse than a line of text, and the whole boot sequence reported itself
	 * silently before the emulator reached the point of being able to say
	 * anything at all.  Everything below LogError still reaches the log file.
	 */
	/*
	 * The threshold is on what Circle is allowed to *generate*, not on where it
	 * goes: the check happens before any target is consulted, so a logger built
	 * with LogPanic silently discarded every error the drivers reported -- which
	 * is why the HDMI sound device's own explanation never appeared anywhere.
	 *
	 * The target is our runtime-log device, not the screen, so the picture stays
	 * clean regardless of this level.  Warnings are the useful floor: they
	 * include the reasons drivers refuse to start, without the per-frame noise
	 * that debug output would add.
	 */
	m_Logger (LogWarning, &m_Timer)
{
	m_ActLED.Blink (5);	// show we are alive
}

CKernel::~CKernel (void)
{
}

boolean CKernel::Initialize (void)
{
	boolean bOK = TRUE;

	/*
	 * No text console is brought up.
	 *
	 * It used to be initialized here, which allocated a framebuffer of its own,
	 * cleared it and showed a few lines of start-up text -- all of which was
	 * then thrown away when the emulator's framebuffer replaced it.  Removing it
	 * saves that allocation and clear, removes the visible text flash, and
	 * leaves the display untouched until the emulated machine's own picture
	 * appears.
	 *
	 * The serial port stays: it costs almost nothing, works without a card or a
	 * display, and is the only output available if the SD card cannot be mounted.
	 */

	if (bOK)
	{
		// Only used for the log; a board with no serial cable attached must
		// still boot.
		m_Serial.Initialize (115200);
	}

	if (bOK)
	{
		/*
		 * The logger targets the serial port.
		 *
		 * It used to fall back to the screen when the configured log device was
		 * not found, which is no longer possible: with no console there is
		 * nothing to fall back to.  m_Serial is a CDevice and is always present
		 * (writes to an unconnected port are harmless), so the logger always has
		 * somewhere valid to go, and this project installs its own file target
		 * later anyway.
		 */
		CDevice *pTarget =
			CDeviceNameService::Get ()->GetDevice (m_Options.GetLogDevice (), FALSE);
		if (pTarget == 0)
		{
			pTarget = &m_Serial;
		}
		bOK = m_Logger.Initialize (pTarget);
	}

	if (bOK)
	{
		bOK = m_Interrupt.Initialize ();
	}

	if (bOK)
	{
		bOK = m_Timer.Initialize ();
	}

	return bOK;
}

// The Circle members are constructed by the CKernel base class, in its order.
CAtariKernel::CAtariKernel (void)
:	m_pBootLog (0),
	m_USBHCI (&m_Interrupt, &m_Timer, TRUE),
	m_EMMC (&m_Interrupt, &m_Timer, &m_ActLED),
	m_pKeyboard (0),
	m_pAudioSink (0),
	m_bInitialized (FALSE),
	m_bAudioOpen (FALSE),
	m_bUSBReady (FALSE),
	m_bFirstFrameDrawn (FALSE),
	m_bFrameErrorReported (FALSE),
	m_bKeyboardSeen (FALSE),
	m_bKeyboardWarned (FALSE),
	m_nKeyboardFrames (0)
{
	m_SettingsPath[0] = '\0';
	m_LastFrameError[0] = '\0';
	memset (&m_EmulatorConfig, 0, sizeof m_EmulatorConfig);

	s_pKernelForClock = this;
}

CAtariKernel::~CAtariKernel (void)
{
	// The emulation core must be shut down before the modules it calls into
	// go away, and before the audio device it writes to.
	if (m_bInitialized)
	{
		ATARI_Exit ();
	}

	// It is the host's job to release the audio sink that was handed over.
	delete m_pAudioSink;
	m_pAudioSink = 0;

	delete m_pKeyboard;
	m_pKeyboard = 0;

	s_pKernelForClock = 0;
}

/*
 * Reading the ARM generic timer directly.
 *
 * Circle's CTimer is not available until CKernel::Initialize() has brought up the
 * interrupt system and the timer itself, and the whole point here is to measure
 * what happened *before* that -- the firmware's own start-up and Circle's system
 * init.  The generic counter is free-running from power-on and readable at any
 * time, so it can measure the interval that CTimer cannot.
 *
 * CNTFRQ_EL0 reports the frequency; on this SoC it is 54 MHz or 19.2 MHz
 * depending on the model, so it must be read rather than assumed.
 */
static u64 ReadBootCounter (void)
{
	u64 nValue;
	__asm volatile ("mrs %0, cntpct_el0" : "=r" (nValue));
	return nValue;
}

static u64 ReadBootCounterHz (void)
{
	u64 nValue;
	__asm volatile ("mrs %0, cntfrq_el0" : "=r" (nValue));
	return nValue != 0 ? nValue : 1;
}

boolean CAtariKernel::Initialize (void)
{
	/*
	 * Taken as early as this project has any control at all: the difference
	 * between this reading and the one after CKernel::Initialize() is the
	 * firmware plus Circle's system init, and it is the largest unexplained block
	 * in the boot.
	 */
	const u64 nEntryTicks = ReadBootCounter ();
	const u64 nEntryHz = ReadBootCounterHz ();
	/*
	 * The boot log is created here rather than in the constructor.
	 *
	 * It holds about 8 KB of buffers, and the kernel object is a local in
	 * main() -- on the stack -- so the log lives in .bss behind Get().  Get()
	 * must not be called until CKernel's members exist, because it may ask the
	 * timer for the time and CTimer::Get() asserts when there is no timer.
	 * Calling it from the initialiser list would run before the timer.
	 */
	m_pBootLog = CBootLog::Get ();

	/*
	 * From here to the first frame, every step is reported into the message
	 * buffer.  There is no text console any more, so nothing is shown until the
	 * emulator's own framebuffer is up.  If anything below fails, the buffered
	 * messages are painted on screen and written to a file on the SD card, so the
	 * machine explains itself instead of showing nothing.
	 */
	m_pBootLog->Stage (BootStagePowerOn);

	// Bring up the serial port, logger, interrupt system and timer first, so that
	// anything that goes wrong afterwards has somewhere to be reported.
	if (!CKernel::Initialize ())
	{
		// Not even Circle's own initialisation completed.  There is nowhere to
		// report to, but the caller still gets an honest FALSE.
		return FALSE;
	}

	/*
	 * Now that the timer exists, report how the first seconds were spent.
	 *
	 * "since power-on" is everything before this project ran: firmware, load of
	 * the kernel image, and Circle's system init.  "Circle init" is the text
	 * console, serial, logger, interrupt system and timer.  Both figures are in
	 * milliseconds from the free-running counter, so they do not depend on the
	 * boot log's own clock.
	 */
	{
		u64 nNow = ReadBootCounter ();
		u64 nSincePowerOnMs = (nNow * 1000) / nEntryHz;
		u64 nCircleMs = ((nNow - nEntryTicks) * 1000) / nEntryHz;

		m_pBootLog->Recordf ("boot timing: %lu ms since power-on, of which "
				     "%lu ms is Circle's own init",
				     (unsigned long) nSincePowerOnMs,
				     (unsigned long) nCircleMs);
	}

	/*
	 * Take over Circle's logging once the kernel has set it up.
	 *
	 * This must come after CKernel::Initialize(), not before: that call ends in
	 * CLogger::Initialize(), which assigns the target to the screen device.
	 * Installing ours earlier was simply overwritten -- which is why no driver
	 * message ever reached the log, not even the ones explaining why the audio
	 * device refused to start.
	 *
	 * With this target in place, nothing a driver reports can paint over the
	 * picture.  Messages are discarded until the runtime log file exists (see
	 * CRuntimeLogTarget::SetReady), then recorded.
	 */
	CLogger::Get ()->SetNewTarget (&s_RuntimeLogTarget);

	/*
	 * No logger is attached to the boot log in the released build.
	 *
	 * At the time this was written Circle's logger drew to the text console, so
	 * attaching it left the start-up messages displayed over the emulated
	 * machine's picture.  The console is gone and the logger now targets the
	 * serial port, but the rule stands: a real Atari shows its own screen and
	 * nothing else.  The boot log still records everything to BOOTLOG.TXT on the
	 * SD card, which is where it is actually useful.
	 *
	 * Pass &m_Logger here to also send the messages to the serial port when
	 * bringing this up on new hardware -- that is the only change needed.
	 */
	m_pBootLog->Attach (0, 0);	/* file only */
	m_pBootLog->Stage (BootStageScreen);

	/*
	 * Nothing is drawn yet: there is no text console any more and the framebuffer
	 * comes later.  Until then the screen shows whatever the firmware left, which
	 * is preferable to a text console appearing and being replaced.
	 */
	m_pBootLog->Record ("Atari XL/XE emulator for Raspberry Pi");
	m_pBootLog->Recordf ("Build: %s %s", __DATE__, __TIME__);

	boolean bOK = TRUE;

	// USB carries both the keyboard and the mass storage device, so a failure
	// here is only fatal if the settings ask for a USB keyboard; that is
	// checked below.
	boolean bUSBOK = FALSE;
	m_pBootLog->Stage (BootStageUsb);
	if (bOK)
	{
		bUSBOK = m_USBHCI.Initialize ();
		m_bUSBReady = bUSBOK;
		if (!bUSBOK)
		{
			m_pBootLog->Record ("USB could not be started; "
					  "the USB keyboard and drive will not work");
		}
		else
		{
			m_pBootLog->Record ("USB host controller started");
		}
	}

	m_pBootLog->Stage (BootStageSdCard);
	if (bOK)
	{
		bOK = m_EMMC.Initialize ();
		if (!bOK)
		{
			m_pBootLog->Record ("PANIC: cannot start the SD card interface");
			ReportStartupFailure ("The SD card interface could not be started");
		}
		else
		{
			m_pBootLog->Record ("SD card interface started");
		}
	}

	if (!bOK)
	{
		return FALSE;
	}

	// -----------------------------------------------------------------------
	// File systems
	// -----------------------------------------------------------------------
	m_pBootLog->Stage (BootStageFilesystems);

	if (!m_Storage.Initialize (SystemDirectory))
	{
		/*
		 * Recorded in the boot log and reported through the serial port, which is
		 * the only output left: there is no text console, the log file cannot be
		 * opened without a card, and the framebuffer does not exist yet so the
		 * stage colour cannot be painted either.
		 */
		m_pBootLog->Record ("PANIC: cannot mount the SD card");

		ReportStartupFailure ("The SD card could not be mounted");
		return FALSE;
	}
	m_pBootLog->Recordf ("SD card mounted, system directory is %s",
			   m_Storage.GetSystemDirectory ());

	/*
	 * The log file is opened here, before the framebuffer exists, because this
	 * is the earliest moment a file can be written -- and the messages between
	 * here and the display coming up are exactly the ones worth having if
	 * something in between fails.
	 *
	 * The previous session's log is read back at the same time, so that it can
	 * be shown on screen.  It matters when this session dies before writing
	 * anything: the screen would otherwise have no history at all.
	 */
	if (m_pBootLog->OpenLogFile (m_Storage))
	{
		m_pBootLog->Recordf ("Boot log open at %s", m_pBootLog->GetLogPath ());
		m_pBootLog->ReadPreviousLog (m_Storage);

		/*
		 * Install the runtime log sink now that the file exists.
		 *
		 * The platform layer is C and the log is a C++ class, so it reaches the
		 * file through this function pointer.  Until it is installed, runtime
		 * messages are dropped rather than buffered: there is nowhere useful to
		 * put them, and a failure during bring-up must not recurse.
		 */
		ATARI_SetRuntimeLog (RuntimeLogSink);

		if (m_pBootLog->GetPreviousLineCount () > 0)
		{
			m_pBootLog->Recordf ("Previous session's log read (%u lines kept)",
					   m_pBootLog->GetPreviousLineCount ());
		}
	}
	else
	{
		m_pBootLog->Record ("Could not open a boot log on the SD card; "
				  "progress is only kept in memory");
	}

	if (m_Storage.IsUserVolumeAvailable ())
	{
		m_pBootLog->Record ("USB drive mounted");
	}
	else
	{
		m_pBootLog->Record ("No USB drive found; disk, cartridge and cassette "
				  "images will not be available until one is plugged in");
	}

	m_Storage.MakeSystemPath (m_SettingsPath, sizeof m_SettingsPath, "settings.cfg");

	// -----------------------------------------------------------------------
	// Display
	// -----------------------------------------------------------------------
	m_pBootLog->Stage (BootStageSettings);

	if (!Settings_Load (m_Storage, m_SettingsPath, &m_Settings))
	{
		m_pBootLog->Record ("No usable settings file; using the defaults");
	}
	else
	{
		m_pBootLog->Recordf ("Settings loaded from %s", m_SettingsPath);
	}

	/*
	 * Where the user's media live is a setting, not a build option: the volume
	 * named by USER_DIR is the one the file browser shows.  "USB:" (the default)
	 * is the USB drive; "SD:" puts them in SD:/USERDATA and keeps the browser
	 * from wandering into the rest of the card, which holds the emulator's own
	 * files.
	 */
	{
		boolean bUserOnSD = CAtariStorage::OnSystemVolume (m_Settings.UserDirectory);

		m_Storage.SetUserVolume (bUserOnSD);

		m_pBootLog->Recordf ("Media: user files on %s (%s)",
				     bUserOnSD ? "the SD card" : "the USB drive",
				     m_Storage.GetUserBase ());
	}

	/*
	 * Report the settings that are not otherwise visible at startup.
	 *
	 * The machine type, RAM size and video standard are logged by the platform
	 * once the core is up, because those are the ones that can be silently
	 * overridden.  Everything else only appears in the settings file, which is
	 * not on the machine when the log is being read -- so a wrong resolution or
	 * an unexpected keyboard mode costs an experiment to confirm.  These two
	 * lines make the whole configuration readable from the log alone.
	 */
	CBootLog::Get ()->RecordRuntimef (
		"display configured: %s",
		m_Settings.nResolution == AtariResolution1080p ? "1080p" : "720p");

	CBootLog::Get ()->RecordRuntimef (
		"input configured: %s keyboard, numpad as cursor %s, "
		"USB joysticks %s, GPIO joysticks %s, cassette %s",
		m_Settings.nKeyboardMode == AtariKeyboardModeUSB ? "USB" : "GPIO",
		m_Settings.bNumpadAsCursor ? "on" : "off",
		m_Settings.bUSBJoysticks ? "on" : "off",
		m_Settings.bGPIOJoysticks ? "on" : "off",
		m_Settings.bCassetteReadOnly ? "read only" : "read/write");

	/*
	 * If the previous run died, start from a clean machine.
	 *
	 * A disk image that crashes the emulator would otherwise crash it again on
	 * every boot, with no way back into the menu, because the image is re-mounted
	 * from the settings before the user can act.  Ejecting everything and
	 * restoring the machine defaults once, and saving that, breaks the loop.
	 *
	 * The decision is taken here rather than in the exception handler: at crash
	 * time the disk layer must not be asked to do more than write one byte.
	 */
	if (ATARI_CrashMarkerConsume ())
	{
		m_pBootLog->Record ("Previous run crashed; ejecting all media and "
				    "restoring machine defaults");

		for (int i = 0; i < SETTINGS_MAX_DRIVES; i++)
		{
			m_Settings.DiskPath[i][0] = '\0';
		}
		m_Settings.CartridgePath[0] = '\0';
		m_Settings.CassettePath[0] = '\0';

		m_Settings.nMachine = ATARI_MACHINE_XL;
		m_Settings.nRamKB = ATARI_RAM_KB_64;
		m_Settings.nVideoStandard = ATARI_VIDEO_PAL;
		m_Settings.nArtifact = ATARI_ARTIFACT_PAL_SIMPLE;

		Settings_Save (m_Storage, m_SettingsPath, m_Settings);
	}

	m_pBootLog->Stage (BootStageVideo);

	if (!m_Video.Initialize (m_Settings.nResolution == AtariResolution1080p
				 ? AtariDisplayMode1080p : AtariDisplayMode720p))
	{
		m_pBootLog->Record ("PANIC: no framebuffer could be created");
		ReportStartupFailure ("No framebuffer, so there is no way to show a picture. "
				      "Is the HDMI display connected and switched on?");
		return FALSE;
	}

	/*
	 * The framebuffer now exists, so the boot log can be seen.  From here the
	 * stage colours are painted on the screen the emulator will use, which is
	 * what makes a hang identifiable.
	 */
	m_pBootLog->Attach (&m_Video, &m_Logger);

	m_pBootLog->Recordf ("Display: %ux%u, picture %ux%u%s",
			   m_Video.GetWidth (), m_Video.GetHeight (),
			   m_Video.GetPictureWidth (), m_Video.GetHeight (),
			   m_Video.IsDoubleBuffered () ? "" : " (single buffered)");

	// -----------------------------------------------------------------------
	// Audio and the emulation core
	// -----------------------------------------------------------------------
	//
	// Audio does not depend on USB.  The PWM device needs the interrupt system
	// and the timer, both of which are up by now; gating it on USB success
	// would silently disable sound on a machine with no USB device attached,
	// which is exactly when the user is most likely to be looking for sound.
	m_pBootLog->Stage (BootStageAudio);

	/*
	 * The log file exists from here on, so Circle's messages can be recorded.
	 * The target was installed at power-on to keep the screen clean.
	 */
	CRuntimeLogTarget::SetReady (TRUE);

	if (m_Settings.bSoundEnabled)
	{
		if (!InitializeAudio ())
		{
			m_pBootLog->Record ("Audio could not be started; "
					  "the emulator will run silently");
		}
		else
		{
			m_pBootLog->Record ("Sound device started");
		}
	}
	else
	{
		m_pBootLog->Record ("Sound is switched off in the settings");
	}

	ATARI_SetHostServices (GetHostClock, IdleUntil, this);

	m_pBootLog->Stage (BootStageCore);

	/*
	 * Build the core's configuration from the settings before handing it over.
	 *
	 * This was missing: ATARI_Init() was being given m_EmulatorConfig directly,
	 * which nothing had filled, so the core started with a zeroed configuration
	 * and ignored every saved setting -- machine type, RAM size and video
	 * standard alike.  The menu still showed what had been chosen, which made it
	 * look as though the setting was lost rather than never passed.
	 */
	const atari_config_t *pConfig = 0;
	MakeEmulatorConfig (pConfig);

	/*
	 * The tape in the settings decides the SIO mode, and it has to be known
	 * before the machine is built: the OS ROM is patched during the build.
	 * A turbo tape then boots with real timing from the first frame, instead of
	 * needing a power cycle to get there.
	 */
	ATARI_ClassifyCassette (m_Settings.CassettePath);

	if (!ATARI_Init (pConfig))
	{
		m_pBootLog->Recordf ("PANIC: the emulation core failed to start: %s",
				   ATARI_LastError ());
		ReportStartupFailure ("The emulation core could not be started");
		return FALSE;
	}
	m_pBootLog->Recordf ("Emulation core started: %s", ATARI_CoreVersion ());

	/*
	 * The OSD's font is the Atari character set, doubled, compiled into the
	 * kernel: the shapes an Atari would draw rather than a PC console font, and
	 * no ROM to read at run time.  See src/osd/atari_font.cpp.
	 */
	m_Video.SetFont (AtariFont);

	/*
	 * BRK follows the machine unless the settings file asks for the diagnostic.
	 * Applied here, after the core is up, and again whenever the menu applies
	 * settings -- so that "restore defaults" puts it back to following the
	 * machine.
	 */
	ATARI_SetContinueOnBrk (m_Settings.bContinueOnBrk);

	if (!m_Settings.bContinueOnBrk)
	{
		m_pBootLog->Record ("BRK: stopping on BRK (CONTINUE_ON_BRK=0)");
	}

	// -----------------------------------------------------------------------
	// Keyboard and controllers
	// -----------------------------------------------------------------------
	m_pBootLog->Stage (BootStageInput);

	/*
	 * Each of these is recorded separately.
	 *
	 * A Zero 2 W with no keyboard attached stopped inside this stage and wrote
	 * nothing further: the last marker was the stage itself, so the whole of
	 * input, media and menu set-up was one unexplained block.  One line per step
	 * makes the next stop name itself.
	 */
	m_pBootLog->Record ("input: creating the keyboard");

	if (!InitializeKeyboard ())
	{
		ReportStartupFailure ("No keyboard could be set up");
		return FALSE;
	}

	m_pBootLog->Record ("input: keyboard created");

	// Controllers are not essential: a machine with none still runs, and one
	// plugged in later is picked up.  Failures are logged, never fatal.
	InitializeJoysticks ();

	m_pBootLog->Record ("input: joysticks created");

	MountMediaFromSettings ();

	m_pBootLog->Record ("input: media mounted");

	// -----------------------------------------------------------------------
	// Menu
	// -----------------------------------------------------------------------
	m_Menu.Initialize (&m_Video, &m_Storage, this, &m_Settings, m_SettingsPath);

	m_pBootLog->Record ("input: menu created");

	// Persist whatever was loaded, so that a first run leaves a settings file
	// the user can find and edit.
	Settings_Save (m_Storage, m_SettingsPath, m_Settings);

	m_pBootLog->Record ("input: settings saved");

	m_bInitialized = TRUE;

	return TRUE;
}

/*
 * Shown when start-up cannot continue.
 *
 * The point is that the machine says why, on screen, rather than leaving a black
 * display.  The messages the boot log has collected are the whole story of what
 * did work, so they are painted above the failure itself.
 *
 * It waits before returning, because main() halts immediately afterwards and a
 * message that flashes past is no better than no message.  The delay is
 * interruptible by nothing -- the emulator is not running yet -- so it is a plain
 * busy wait on the timer.
 */
void CAtariKernel::ReportStartupFailure (const char *pWhat)
{
	m_pBootLog->Recordf ("FAILED: %s", pWhat);

	/*
	 * Close the log with the reason in it.  This is the line that makes the file
	 * worth having: the account ends with what went wrong rather than simply
	 * stopping, so a reader on a PC does not have to guess which of the last few
	 * lines was fatal.
	 */
	m_pBootLog->CloseLogFile (pWhat);

	m_pBootLog->PaintReport (pWhat);

	// Leave it on screen long enough to be read and photographed.  Not forever:
	// a watchdog reboot or a power cycle should still work.
	CTimer::Get ()->MsDelay (60000);
}

boolean CAtariKernel::InitializeAudio (void)
{
	m_pAudioSink = ATARI_CreateAudioSink (&m_Interrupt);
	if (m_pAudioSink == 0)
	{
		return FALSE;
	}

	ATARI_SetHostAudioSink (m_pAudioSink);
	m_bAudioOpen = TRUE;

	return TRUE;
}

/*
 * Reads usbkeys.cfg into the keyboard driver.
 *
 * Small and read once, so it is read whole rather than streamed.  A missing
 * file is not an error: the driver is told there is none and keeps the built-in
 * mapping, which is also what happens on a machine whose card has no ATARI
 * directory yet.
 */
void CAtariKernel::LoadKeyboardMap (void)
{
	char Path[SETTINGS_PATH_MAX];
	m_Storage.MakeSystemPath (Path, sizeof Path, "usbkeys.cfg");

	FILE *pFile = ATARI_VFS_Open (Path, "rb");
	if (pFile == 0)
	{
		m_pKeyboard->SetKeyMap (Path, 0);

		return;
	}

	static char Buffer[USB_KEYMAP_MAX_FILE + 1];
	size_t nRead = ATARI_VFS_Read (Buffer, 1, USB_KEYMAP_MAX_FILE, pFile);
	ATARI_VFS_Close (pFile);

	Buffer[nRead] = '\0';

	if (nRead == USB_KEYMAP_MAX_FILE)
	{
		m_Logger.Write (FromKernel, LogWarning,
				"%s is larger than %u bytes; the rest is ignored",
				Path, (unsigned) USB_KEYMAP_MAX_FILE);
	}

	m_pKeyboard->SetKeyMap (Path, Buffer);
}

boolean CAtariKernel::InitializeKeyboard (void)
{
	// The two keyboards are created by the platform-specific factories.  Only
	// one is built, because a user has either plugged in a USB keyboard or
	// wired up an Atari one.
	if (m_Settings.nKeyboardMode == AtariKeyboardModeGPIO)
	{
		m_pKeyboard = ATARI_CreateGPIOKeyboard ();
		if (m_pKeyboard == 0)
		{
			m_Logger.Write (FromKernel, LogPanic,
					"The GPIO keyboard could not be created");
			return FALSE;
		}

		// The emulator must still run if the keyboard was not wired up, so a
		// failure to initialize is reported but not fatal.
		if (!m_pKeyboard->Initialize ())
		{
			m_Logger.Write (FromKernel, LogWarning,
					"The GPIO keyboard did not initialise; check the wiring");
		}
	}
	else
	{
		m_pKeyboard = ATARI_CreateUSBKeyboard (&m_USBHCI);
		if (m_pKeyboard == 0)
		{
			m_Logger.Write (FromKernel, LogPanic,
					"The USB keyboard could not be created");
			return FALSE;
		}

		/*
		 * The remapping file, read here rather than in the driver: the
		 * system directory and the file system are the kernel's business.
		 * It has to be installed before Initialize(), which is what
		 * registers the drivers that reports arrive through.
		 */
		LoadKeyboardMap ();

		if (!m_pKeyboard->Initialize ())
		{
			/*
			 * Deliberately silent.
			 *
			 * Start-up scans the USB bus before Circle has registered a
			 * keyboard, so anything said here would also appear on a machine
			 * whose keyboard works perfectly a frame later -- which is what the
			 * old message did, and why it was not worth reading.  The frame loop
			 * reports the real situation instead; see UpdateKeyboardStatus().
			 */
		}
	}

	m_pKeyboard->SetNumpadAsCursor (m_Settings.bNumpadAsCursor);

	// The keyboard source is polled once per frame by the emulation core.
	ATARI_SetKeyboardSource (
		[](void *pContext, atari_keyboard_state_t *pState)
		{
			static_cast<CAtariKeyboard *> (pContext)->GetState (pState);
		},
		m_pKeyboard);

	return TRUE;
}

/*
 * Brings up the controllers.
 *
 * There are two kinds, and they are not alternatives to each other in the way
 * the two keyboards are: USB gamepads and the two 9-pin GPIO ports can be used
 * at the same time, because they share no hardware.
 *
 * What they cannot do is share with a GPIO keyboard.  That keyboard takes
 * sixteen header pins as outputs and inputs, and the two joystick ports need
 * pins from the same set -- the assignment deliberately follows the keyboard's
 * own wiring, because there are not enough spare pins for both.  So a GPIO
 * keyboard and GPIO joysticks are mutually exclusive, and the keyboard wins:
 * the user has explicitly asked for it, and a keyboard with no joysticks is
 * more useful than the other way round.
 *
 * Nothing here is fatal.  A machine with no controller at all is a normal thing
 * to have, and one plugged in later is picked up by the drivers.
 */
void CAtariKernel::InitializeJoysticks (void)
{
	boolean bGPIOKeyboard = m_Settings.nKeyboardMode == AtariKeyboardModeGPIO;

	/*
	 * A start-up record of the controller configuration.  The driver's own log
	 * says nothing unless a controller is attached, so this line is what tells a
	 * log without controller lines apart from a log written by a build in which
	 * joysticks were never created.
	 */
	CBootLog::Get ()->RecordRuntimef ("joysticks: initialising, usb %s, gpio %s",
					 m_Settings.bUSBJoysticks ? "on" : "off",
					 bGPIOKeyboard ? "on" : "off");

	// --- USB gamepads -------------------------------------------------------
	//
	// Four are supported, matching the core's four controller inputs.  Each is
	// looked up by index, and a controller that is not attached simply reports
	// itself absent until one is.
	if (m_Settings.bUSBJoysticks)
	{
		unsigned nUSBFound = 0;
		unsigned nUSBAttached = 0;
		for (unsigned n = 1; n <= ATARI_JOYSTICK_COUNT; n++)
		{
			CAtariJoystick *pJoystick = ATARI_CreateUSBJoystick (&m_USBHCI, n);
			if (pJoystick == 0)
			{
				continue;
			}

			pJoystick->Initialize ();

			if (pJoystick->IsPresent ())
			{
				nUSBAttached++;
			}

			/*
			 * The slot is filled even when nothing is attached: a USB gamepad
			 * plugged in later is then noticed by the driver's own IsPresent(),
			 * without the kernel having to poll for new devices.
			 */
			ATARI_SetJoystick ((int) n - 1, pJoystick->Handle ());
			nUSBFound++;
		}

		/*
		 * LogWarning, because this kernel's logger discards LogNotice -- this
		 * line was written for years without ever reaching the log.  It says the
		 * driver is running, which is not the same thing as a controller being
		 * attached, so the two are reported separately.
		 */
		if (nUSBFound > 0)
		{
			m_Logger.Write (FromKernel, LogWarning,
					"Watching %u USB gamepad input%s; plug one in at any time",
					nUSBFound, nUSBFound == 1 ? "" : "s");
		}

		if (nUSBAttached == 0)
		{
			m_Logger.Write (FromKernel, LogWarning,
					"No USB gamepad is attached yet; one plugged in later "
					"is noticed without restarting");
		}
	}

	// --- 9-pin ports on GPIO ------------------------------------------------
	if (!m_Settings.bGPIOJoysticks)
	{
		return;
	}

	if (bGPIOKeyboard)
	{
		m_Logger.Write (FromKernel, LogWarning,
				"GPIO joysticks are off while the GPIO keyboard is selected: "
				"they need the same header pins");
		return;
	}

	for (unsigned nPort = 1; nPort <= ATARI_GPIO_JOYSTICK_PORTS; nPort++)
	{
		CAtariJoystick *pJoystick = ATARI_CreateGPIOJoystick (nPort);
		if (pJoystick == 0)
		{
			continue;
		}

		/*
		 * A USB gamepad has already claimed this port if one was found, and a
		 * USB gamepad is the more informative of the two.  The GPIO port is
		 * then left unclaimed rather than silently overriding it.
		 */
		if (ATARI_GetJoystick ((int) nPort - 1) != 0)
		{
			continue;
		}

		if (!pJoystick->Initialize ())
		{
			m_Logger.Write (FromKernel, LogWarning,
					"GPIO joystick port %u did not initialise", nPort);
			continue;
		}

		ATARI_SetJoystick ((int) nPort - 1, pJoystick->Handle ());
	}
}

void CAtariKernel::MountMediaFromSettings (void)
{
	/*
	 * Media is mounted from the saved paths at start-up.  A failure is reported
	 * and the machine carries on: the USB drive may simply not be plugged in
	 * yet, and the user can load the image from the menu once it is.
	 */
	for (int i = 0; i < SETTINGS_MAX_DRIVES; i++)
	{
		if (m_Settings.DiskPath[i][0] == '\0')
		{
			continue;
		}
		if (!ATARI_MountDisk (i + 1, m_Settings.DiskPath[i], m_Settings.DiskReadOnly[i]))
		{
			m_Logger.Write (FromKernel, LogWarning, "Cannot mount %s in drive %d: %s",
					m_Settings.DiskPath[i], i + 1, ATARI_LastError ());
		}
	}

	if (m_Settings.CartridgePath[0] != '\0')
	{
		if (!ATARI_MountCartridge (m_Settings.CartridgePath))
		{
			m_Logger.Write (FromKernel, LogWarning, "Cannot mount cartridge %s: %s",
					m_Settings.CartridgePath, ATARI_LastError ());
		}
	}

	if (m_Settings.CassettePath[0] != '\0')
	{
		if (!ATARI_MountCassette (m_Settings.CassettePath, m_Settings.bCassetteReadOnly))
		{
			m_Logger.Write (FromKernel, LogWarning, "Cannot mount cassette %s: %s",
					m_Settings.CassettePath, ATARI_LastError ());
		}
	}

	/*
	 * Mounting the saved cartridge above asked for a deferred cold start, and
	 * this is where that request has to be dropped.
	 *
	 * ATARI_MountCartridge() always defers the reset, which is right when a user
	 * inserts a cartridge from the menu -- the emulated CPU must not start running
	 * while the OSD owns the display.  During bring-up it is wrong: the machine
	 * has just been initialised and cold-started, and the cartridge is mapped by
	 * CARTRIDGE_Insert() as it is read.  Nothing clears the request otherwise, so
	 * it stayed pending until the first time the menu was closed, and the machine
	 * then reset itself -- boot with a cartridge in settings, open the OSD, close
	 * it, and the cartridge started over.
	 *
	 * Clearing it here rather than in ATARI_Init() is deliberate: this mounting
	 * runs *after* initialisation, so clearing it any earlier is undone by the
	 * mount that follows.
	 */
	ATARI_ClearPendingColdStart ();
}

TShutdownMode CAtariKernel::Run (void)
{
	if (!m_bInitialized)
	{
		return ShutdownHalt;
	}

	for (;;)
	{
		RunFrame ();
	}

	return ShutdownNone;
}

/*
 * Reports the keyboard's presence when that is a fact.
 *
 * Nothing is said at start-up because the answer there is meaningless: the scan
 * runs before Circle has registered a keyboard, so it reads "absent" on machines
 * where one works a frame later.
 *
 * Called once per frame.  A keyboard that turns up gets a notice, and one that
 * stays away for two seconds gets a warning -- two seconds being long enough that
 * its absence is a property of the machine rather than of enumeration timing.
 * Each is said once; the frame loop runs fifty times a second and the log is a
 * file on the SD card.
 */
void CAtariKernel::UpdateKeyboardStatus (void)
{
	if (m_pKeyboard == 0 || (m_bKeyboardSeen && m_bKeyboardWarned))
	{
		return;
	}

	if (m_pKeyboard->IsPresent ())
	{
		if (!m_bKeyboardSeen)
		{
			m_bKeyboardSeen = TRUE;

			if (m_nKeyboardFrames > 0)
			{
				// It was not there when the machine started, and now it is.
				m_Logger.Write (FromKernel, LogWarning, "Keyboard connected");
			}

			if (!m_bKeyboardWarned)
			{
				m_bKeyboardWarned = TRUE;	// nothing to warn about
			}
		}

		return;
	}

	m_nKeyboardFrames++;

	// Half a second at fifty frames: the frame count is only a fallback for a
	// machine where the pacer is not running yet.
	if (!m_bKeyboardWarned && m_nKeyboardFrames == 100)
	{
		m_bKeyboardWarned = TRUE;

		m_Logger.Write (FromKernel, LogWarning,
				"No keyboard has appeared; the machine runs but cannot "
				"be typed on until one is connected");
	}
}

void CAtariKernel::RunFrame (void)
{
	/*
	 * USB enumeration is driven here, from the frame loop, and nowhere else.
	 *
	 * This call is what makes a device plugged in after start-up appear at all:
	 * Circle queues a port-status event in its interrupt and enumerates the
	 * device only when the event is taken at task level.  Until this call
	 * existed, a controller plugged in later was never even seen, and -- worse
	 * -- a device whose first configuration attempt failed was never retried,
	 * because the same call is what runs the root port's recovery retry.  The
	 * Switch Pro controller is exactly that case: its driver exchanges a
	 * handshake with the pad while configuring, and a failed handshake at
	 * power-on left the port disabled for the whole session.
	 *
	 * It happens after the machine is running rather than during start-up, so a
	 * device being configured costs frames but never the boot -- the trade
	 * recorded in docs/progress.md.  When nothing has happened it takes the
	 * empty event queue and returns.
	 */
	if (m_bUSBReady)
	{
		if (m_USBHCI.UpdatePlugAndPlay ())
		{
			/*
			 * Worth one line: it means something was attached, removed, or
			 * that a failed enumeration is being retried.  Without it a
			 * controller that never appears leaves no trace at all, which is
			 * exactly how the Switch Pro failure stayed unexplained.
			 */
			m_Logger.Write (FromKernel, LogWarning,
					"USB device tree changed: something was attached, "
					"removed, or enumeration was retried");
		}
	}

	/*
	 * The keyboard driver re-scans as well, so a keyboard plugged in later is
	 * picked up.  Scanning is its own job; enumeration above is not.
	 */
	if (m_pKeyboard != 0)
	{
		m_pKeyboard->Update ();
	}

	UpdateKeyboardStatus ();

	// Set when the menu acted on a key this frame.  The OSD is drawn on a
	// reduced cadence while it is open (see the draw section below), and an
	// interaction must be visible immediately rather than on the next tick.
	boolean bMenuKeyHandled = FALSE;

	// The menu takes precedence over the emulated machine while it is open:
	// the same arrow keys move the selection rather than the Atari cursor.
	if (m_pKeyboard != 0 && m_pKeyboard->ConsumeMenuRequest ())
	{
		m_Menu.Toggle ();
		bMenuKeyHandled = TRUE;
	}

	if (m_Menu.IsOpen () && m_pKeyboard != 0)
	{
		// Drain the menu's own key queue.
		//
		// Which keys the emulated machine still sees while the menu is open
		// depends on the keyboard driver, and the two differ deliberately: the
		// USB driver withholds every key while the menu is active, so that the
		// arrows that move the selection do not also move the emulated cursor;
		// the GPIO driver withholds only the keys the menu uses, so typing
		// carries on.  See docs/keyboard-usb.md and docs/keyboard-gpio.md.
		for (int nGuard = 0; nGuard < 8; nGuard++)
		{
			int nKey = m_pKeyboard->ConsumeMenuKey ();
			if (nKey == ATARI_KEY_NONE)
			{
				break;
			}
			m_Menu.HandleKey (nKey, FALSE, FALSE);
			bMenuKeyHandled = TRUE;
		}
	}

	/*
	 * Tell the keyboard where the menu ended up, and do it AFTER the keys have
	 * been handled.
	 *
	 * The menu can close itself while handling a key -- Escape, or an item that
	 * performs an action and then closes.  Synchronising before that, as this
	 * used to, missed exactly those closes: the driver was left believing the
	 * menu was still open, and its GetState() then withheld *every* key for the
	 * rest of the session, console keys included.  The machine looked frozen
	 * after nothing more than opening and closing the menu once.
	 *
	 * Checking the final state instead catches every route out of the menu,
	 * whether it was Toggle() or the menu closing itself.
	 */
	/*
	 * Apply a deferred cold start when the menu closes.
	 *
	 * Inserting a cartridge asks for a reset, but it cannot be performed from
	 * inside the menu: that ran the emulated machine while the OSD owned the
	 * display and another image was open on the same storage, which is where the
	 * machine hung after selecting a disk and then a cartridge.  This transition
	 * -- menu open last frame, closed this frame -- is the first safe moment, and
	 * it catches every route out, including the menu closing itself.
	 */
	{
		static boolean bMenuWasOpen = FALSE;
		boolean bMenuOpen = m_Menu.IsOpen ();

		if (bMenuWasOpen && !bMenuOpen)
		{
			ATARI_ApplyPendingColdStart ();
		}

		bMenuWasOpen = bMenuOpen;
	}

	if (m_pKeyboard != 0)
	{
		m_pKeyboard->SetMenuActive (m_Menu.IsOpen ());
	}

	/*
	 * While the menu is open the output is silenced, but nothing else changes:
	 * the emulated machine keeps producing samples and the sink keeps accepting
	 * them, so the sound returns at the moment the menu closes rather than
	 * stuttering back to life from an empty queue.
	 */
	if (m_pAudioSink != 0)
	{
		ATARI_AudioSinkSetMuted (m_pAudioSink, m_Menu.IsOpen ());
	}

	/*
	 * Frame timing.
	 *
	 * The emulation itself costs only a few milliseconds, so the frame budget is
	 * being spent somewhere in this function.  Measuring each phase separately
	 * is the difference between optimising the right thing and guessing: the
	 * numbers go into the periodic report below.
	 */
	u64 nStart = CTimer::GetClockTicks64 ();

	// Emulate one frame.  The emulation core pulls the keyboard state itself,
	// through the callback installed in InitializeKeyboard().
	if (!ATARI_RunFrame ())
	{
		/*
		 * The frame could not be completed.  The core has already described
		 * why, in ATARI_LastError().
		 *
		 * This is reported once per distinct failure rather than acted on every
		 * frame.  It used to warm-reset the machine here, which turned any
		 * persistent fault into a loop: the machine restarted, hit the same
		 * condition, and restarted again, so the screen flickered with an
		 * unreadable repeat of the same message and the cause could never be
		 * seen.  Worse, the reset cleared the boot log's buffer, so the evidence
		 * was destroyed by the handling of it.
		 *
		 * A CPU crash still resets, because that genuinely leaves the machine in
		 * a state it cannot continue from.  Everything else is left running and
		 * reported, so that the machine has a chance to recover and the log
		 * keeps a record either way.
		 */
		const char *pReason = ATARI_LastError ();

		if (!m_bFrameErrorReported || strcmp (pReason, m_LastFrameError) != 0)
		{
			m_bFrameErrorReported = TRUE;
			strncpy (m_LastFrameError, pReason, sizeof m_LastFrameError - 1);
			m_LastFrameError[sizeof m_LastFrameError - 1] = '\0';

			m_pBootLog->RecordRuntimef ("Frame error: %s", pReason);
		}

		if (ATARI_LastErrorIsCpuCrash ())
		{
			m_pBootLog->Record ("Resetting the machine after a CPU crash");
			ATARI_WarmReset ();
		}
	}

	u64 nAfterEmulation = CTimer::GetClockTicks64 ();

	/*
	 * Draw the frame, on a reduced cadence while the menu is open.
	 *
	 * The emulated machine keeps running at full speed either way -- audio
	 * depends on that, because the emulator must produce 44,100 samples per
	 * second regardless of what is being drawn.  Only the drawing is throttled.
	 *
	 * With the menu open, every frame used to redraw the whole Atari picture and
	 * then re-dim the whole menu panel: two full-screen passes, plus the text.
	 * That measured at 25-52 ms per frame and is what made the OSD flicker, since
	 * at that cost the frame misses its slot.  Redrawing a few times a second
	 * keeps the picture visible behind the panel at a fraction of the cost, and
	 * an interaction is drawn immediately so the menu still feels responsive.
	 */
	static const unsigned MenuRedrawInterval = 10;	// frames between redraws
	/*
	 * A key must be seen at once, but not on every frame: key repeat delivers a
	 * key each frame while an arrow is held, and each redraw costs far more than
	 * a frame's budget.  Redrawing every frame therefore overruns continuously,
	 * which is what the flicker was.  A short floor keeps the menu responsive
	 * while giving the frame pacer room to recover between redraws.
	 */
	static const unsigned MenuKeyRedrawFloor = 4;

	static unsigned nMenuCountdown = 0;
	static unsigned nSinceRedraw = MenuKeyRedrawFloor;

	boolean bMenuOpen = m_Menu.IsOpen ();
	boolean bRedraw = TRUE;

	if (bMenuOpen)
	{
		bRedraw = nMenuCountdown == 0
			  || (bMenuKeyHandled && nSinceRedraw >= MenuKeyRedrawFloor);
		nMenuCountdown = bRedraw ? MenuRedrawInterval - 1 : nMenuCountdown - 1;
	}
	else
	{
		nMenuCountdown = 0;
	}

	nSinceRedraw = bRedraw ? 0 : nSinceRedraw + 1;

	if (bRedraw)
	{
		/*
		 * With STATIC_OSD the emulated picture is not repainted while the menu
		 * is up: the OSD is drawn over the frame already in the buffer, and the
		 * panel covers it.  Repainting 960x720 pixels costs about 18 ms of a
		 * 20 ms frame on a Zero 2 W, so a redraw cannot fit beside the
		 * emulation there -- the machine loses its frame deadline and games that
		 * depend on it crash.  Only the OSD redraws skip the picture; closing
		 * the menu repaints on the next frame as usual.
		 */
		if (!bMenuOpen || !m_Settings.bStaticOSD)
		{
			const u8 *pScreen = ATARI_ScreenIndices ();
			if (pScreen != 0)
			{
				m_Video.FillScreen (pScreen, (const u32 *) ATARI_Palette ());
			}
		}

		if (bMenuOpen)
		{
			m_Menu.Draw ();
		}

		m_Video.Present ();
	}

	u64 nEnd = CTimer::GetClockTicks64 ();

	s_Timings.nEmulation += nAfterEmulation - nStart;
	s_Timings.nFill      += nEnd - nAfterEmulation;
	s_Timings.nMenu      += 0;
	s_Timings.nPresent   += 0;
	s_Timings.nFrames++;

	/*
	 * Report every few hundred frames rather than every frame: the numbers are
	 * averages, and a log line per frame would cost more than it measures.
	 */
	if (s_Timings.nFrames >= 300)
	{
		unsigned n = s_Timings.nFrames;

		m_pBootLog->RecordRuntimef (
			"frame cost (us): emulate %llu, fill %llu, menu %llu, "
			"present %llu, total %llu",
			s_Timings.nEmulation / n, s_Timings.nFill / n,
			s_Timings.nMenu / n, s_Timings.nPresent / n,
			(s_Timings.nEmulation + s_Timings.nFill + s_Timings.nMenu
			 + s_Timings.nPresent) / n);

		s_Timings.nEmulation = s_Timings.nFill = 0;
		s_Timings.nMenu = s_Timings.nPresent = 0;
		s_Timings.nFrames = 0;
	}

	/*
	 * The first completed frame is the end of start-up.
	 *
	 * Everything after this point is the emulator running normally, so the
	 * stage colours stop here -- painting over a working picture to say "the
	 * framebuffer works" would be self-defeating.  Recording that the frame
	 * happened also lets a later hang be told from a start-up failure.
	 */
	if (!m_bFirstFrameDrawn)
	{
		m_bFirstFrameDrawn = TRUE;
		m_pBootLog->Stage (BootStageRunning);
		m_pBootLog->StopPainting ();
	}

	/*
	 * The emulated machine reports its own state once, a second or so in, and
	 * it cannot reach the log file itself -- see ATARI_TakeStateSnapshot().
	 * Collected here, on a frame boundary, so that the record of a machine that
	 * never produces a picture says why rather than simply stopping.
	 */
	{
		atari_state_snapshot_t Snapshot;

		if (ATARI_TakeStateSnapshot (&Snapshot))
		{
			m_pBootLog->Record ("=== emulated machine state ===");
			m_pBootLog->Recordf ("  OS version     : %d  (-1 = no OS ROM chosen)",
					     Snapshot.os_version);
			m_pBootLog->Recordf ("  ROM images     : %u accepted",
					     Snapshot.rom_files);
			m_pBootLog->Recordf ("  BASIC ROM      : %s",
					     Snapshot.has_basic ? "present" : "absent");
			m_pBootLog->Recordf ("  machine type   : %d", Snapshot.machine_type);
			m_pBootLog->Recordf ("  RAM            : %d kB", Snapshot.ram_kb);
			m_pBootLog->Recordf ("  ANTIC dlist    : 0x%04x  (0 = never programmed)",
					     Snapshot.antic_dlist);
			m_pBootLog->Recordf ("  ANTIC DMACTL   : 0x%02x  (0x20 enables the list)",
					     Snapshot.antic_dmactl);
			m_pBootLog->Recordf ("  ANTIC scan/xpos: line %u, cycle %u",
					     Snapshot.antic_scan, Snapshot.antic_mode);
			m_pBootLog->Recordf ("  PIA PORTB      : 0x%02x  (OS/bank select)",
					     Snapshot.portb);
			m_pBootLog->Recordf ("  CPU PC         : 0x%04x  A=0x%02x",
					     Snapshot.cpu_pc, Snapshot.cpu_a);
			m_pBootLog->Recordf ("  frames emulated: %u", Snapshot.frames);
		}
	}
}

/* ---------------------------------------------------------------------------
 * Host services for the emulation core
 * ------------------------------------------------------------------------- */

u64 CAtariKernel::GetHostClock (void *pContext)
{
	(void) pContext;

	// Circle's timer counts at 1 MHz, so its ticks are microseconds, which is
	// exactly the unit the emulation core's platform layer expects.
	return CTimer::GetClockTicks64 ();
}

void CAtariKernel::IdleUntil (void *pContext, uint32_t nMicroseconds)
{
	(void) pContext;

	if (nMicroseconds == 0)
	{
		return;
	}

	// Do not sleep for a whole frame's worth of time in one go: the USB stack
	// and the audio queue are serviced by interrupts, but keeping the wait
	// short means the loop stays responsive to them and the emulation does not
	// overshoot its frame time.
	const uint32_t nSlice = 500;
	while (nMicroseconds > 0)
	{
		uint32_t nThis = nMicroseconds > nSlice ? nSlice : nMicroseconds;
		CTimer::SimpleusDelay (nThis);
		nMicroseconds -= nThis;
	}
}

/* ---------------------------------------------------------------------------
 * CAtariMenuHost
 * ------------------------------------------------------------------------- */

void CAtariKernel::MakeEmulatorConfig (const atari_config_t *&pConfig)
{
	// The emulation core keeps its own copy of the strings it is given for the
	// duration of the call, so pointing at the settings text is safe here.
	m_EmulatorConfig.machine = (atari_machine_t) m_Settings.nMachine;
	m_EmulatorConfig.video = (atari_video_standard_t) m_Settings.nVideoStandard;
	m_EmulatorConfig.ram_kb = (atari_ram_kb_t) m_Settings.nRamKB;
	m_EmulatorConfig.artifact = (atari_artifact_t) m_Settings.nArtifact;
	m_EmulatorConfig.sound_enabled = m_Settings.bSoundEnabled && m_bAudioOpen;
	m_EmulatorConfig.sound_volume = m_Settings.nSoundVolume;
	m_EmulatorConfig.sound_stereo = m_Settings.bSoundStereo;
	m_EmulatorConfig.sound_sample_rate = 44100;
	m_EmulatorConfig.rom_directory = "SD:/ROMS";

	pConfig = &m_EmulatorConfig;
}

boolean CAtariKernel::ApplySettings (const TAtariSettings &Settings)
{
	m_Settings = Settings;

	/*
	 * Traced step by step because changing any setting halts the whole system,
	 * with no exception and with the timer heartbeat stopping too.  A hang with
	 * interrupts dead is not a fault we can catch afterwards, so each step says
	 * which one it reached: the last line before silence names the culprit.
	 */
	CBootLog::Get ()->RecordRuntime ("apply: start");

	// The emulation core applies each of these directly; none of them requires
	// the machine to be restarted, which is why changing the video standard or
	// the volume in the menu takes effect at once.
	ATARI_SetVideoStandard ((atari_video_standard_t) Settings.nVideoStandard);
	CBootLog::Get ()->RecordRuntime ("apply: video standard set");

	ATARI_SetArtifact ((atari_artifact_t) Settings.nArtifact);
	CBootLog::Get ()->RecordRuntime ("apply: artifact set");

	ATARI_SetSoundEnabled (Settings.bSoundEnabled);
	CBootLog::Get ()->RecordRuntime ("apply: sound enabled set");

	ATARI_SetVolume (Settings.nSoundVolume);
	CBootLog::Get ()->RecordRuntime ("apply: volume set");

	ATARI_SetStereo (Settings.bSoundStereo);
	CBootLog::Get ()->RecordRuntime ("apply: stereo set");

	// Changing the amount of RAM or the machine personality does restart the
	// emulated machine; the core does that itself.
	CBootLog::Get ()->RecordRuntime ("apply: setting ram size");
	if (!ATARI_SetRamSize ((atari_ram_kb_t) Settings.nRamKB))
	{
		return FALSE;
	}
	CBootLog::Get ()->RecordRuntime ("apply: ram size done");

	CBootLog::Get ()->RecordRuntime ("apply: setting machine");
	if (!ATARI_SetMachine ((atari_machine_t) Settings.nMachine))
	{
		return FALSE;
	}
	CBootLog::Get ()->RecordRuntime ("apply: machine done");

	if (m_pKeyboard != 0)
	{
		m_pKeyboard->SetNumpadAsCursor (Settings.bNumpadAsCursor);
	}

	/*
	 * Not a menu item: this one is set in the settings file, because it is a
	 * diagnostic rather than something to switch while playing.
	 */
	ATARI_SetContinueOnBrk (Settings.bContinueOnBrk);

	CBootLog::Get ()->RecordRuntime ("apply: complete");

	return TRUE;
}

boolean CAtariKernel::MountDisk (int nDrive, const char *pPath, boolean bReadOnly)
{
	return ATARI_MountDisk (nDrive, pPath, bReadOnly) ? TRUE : FALSE;
}

boolean CAtariKernel::MountCartridge (const char *pPath)
{
	return ATARI_MountCartridge (pPath) ? TRUE : FALSE;
}

boolean CAtariKernel::MountCassette (const char *pPath, boolean bReadOnly)
{
	return ATARI_MountCassette (pPath, bReadOnly) ? TRUE : FALSE;
}

boolean CAtariKernel::SetCassetteWriteProtect (boolean bReadOnly)
{
	return ATARI_SetCassetteWriteProtect (bReadOnly) ? TRUE : FALSE;
}

boolean CAtariKernel::CassetteModeMismatch (void)
{
	return ATARI_CassetteNeedsRebuild () ? TRUE : FALSE;
}

const char *CAtariKernel::GetCassetteModeMessage (void)
{
	return ATARI_CassetteModeMessage ();
}

boolean CAtariKernel::SaveSnapshot (const char *pPath)
{
	return ATARI_SaveSnapshot (pPath) ? TRUE : FALSE;
}

boolean CAtariKernel::LoadSnapshot (const char *pPath)
{
	return ATARI_LoadSnapshot (pPath) ? TRUE : FALSE;
}

void CAtariKernel::ColdReset (void)
{
	/*
	 * Logged on both sides of the call.
	 *
	 * A reset requested from the OSD runs this from inside the menu's key
	 * handler, and a fault in it leaves the machine apparently running but deaf
	 * -- no keys, no menu, and nothing further in the log.  "requested" without
	 * "returned" says the reset itself is the problem rather than what follows
	 * it.
	 */
	CBootLog::Get ()->RecordRuntime ("OSD: cold reset requested");
	ATARI_ColdReset ();
	CBootLog::Get ()->RecordRuntime ("OSD: cold reset returned");
}

void CAtariKernel::WarmReset (void)
{
	CBootLog::Get ()->RecordRuntime ("OSD: warm reset requested");
	ATARI_WarmReset ();
	CBootLog::Get ()->RecordRuntime ("OSD: warm reset returned");
}

boolean CAtariKernel::PowerCycle (void)
{
	/*
	 * The configuration is rebuilt from the settings first: the core is about to
	 * be built again, so machine type, RAM, video standard and the sound settings
	 * all come from what the user has chosen, not from whatever the previous
	 * machine was told.
	 */
	const atari_config_t *pConfig = 0;
	MakeEmulatorConfig (pConfig);

	CBootLog::Get ()->RecordRuntime ("OSD: power cycle requested");

	if (!ATARI_PowerCycle (pConfig))
	{
		CBootLog::Get ()->RecordRuntime ("OSD: power cycle failed");

		return FALSE;
	}

	/*
	 * The new machine has no media in it: put back what the settings say was
	 * inserted.  A power cycle does not empty the drives -- the images are on the
	 * card -- so they are mounted again from the same paths.
	 */
	MountMediaFromSettings ();

	/* Not part of the core's configuration: it is a policy of this port. */
	ATARI_SetContinueOnBrk (m_Settings.bContinueOnBrk);

	CBootLog::Get ()->RecordRuntime ("OSD: power cycle returned");

	return TRUE;
}

const char *CAtariKernel::GetLastError (void)
{
	return ATARI_LastError ();
}

double CAtariKernel::GetFps (void)
{
	return ATARI_Fps ();
}
