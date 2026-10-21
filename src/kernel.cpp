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
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	m_Serial (&m_Interrupt, 0),
	m_Timer (&m_Interrupt),
	/*
	 * Errors and panics only.
	 *
	 * Circle's logger writes to the text console, which is the screen, so at its
	 * default level every notice and warning from the framework and from this
	 * kernel appeared over the emulated machine's picture -- including the
	 * version banner and the USB enumeration chatter.  A real Atari shows its
	 * own screen and nothing else.
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

	if (bOK)
	{
		bOK = m_Screen.Initialize ();
	}

	if (bOK)
	{
		// Only used for the log; a board with no serial cable attached must
		// still boot.
		m_Serial.Initialize (115200);
	}

	if (bOK)
	{
		CDevice *pTarget =
			CDeviceNameService::Get ()->GetDevice (m_Options.GetLogDevice (), FALSE);
		if (pTarget == 0)
		{
			pTarget = &m_Screen;
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
	m_bFirstFrameDrawn (FALSE),
	m_bFrameErrorReported (FALSE)
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

boolean CAtariKernel::Initialize (void)
{
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
	 * From here to the first frame, every step is reported twice: into the
	 * message buffer and to the screen, which does not survive -- the
	 * framebuffer created later replaces the text console.  If anything below
	 * fails, the buffered messages are painted on screen and written to a file
	 * on the SD card, so the machine explains itself instead of showing nothing.
	 */
	m_pBootLog->Stage (BootStagePowerOn);

	// Bring up the screen, logger, interrupt system and timer first, so that
	// anything that goes wrong afterwards can be seen.
	if (!CKernel::Initialize ())
	{
		// Not even the text console came up.  There is nowhere to report to,
		// but the caller still gets an honest FALSE.
		return FALSE;
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
	 * Circle's logger writes to the text console, which is on screen, so
	 * attaching it left the start-up messages displayed over the emulated
	 * machine's picture.  A real Atari shows its own screen and nothing else.
	 * The boot log still records everything to BOOTLOG.TXT on the SD card, which
	 * is where it is actually useful.
	 *
	 * Pass &m_Logger here to put the messages back on screen when bringing this
	 * up on new hardware -- that is the only change needed.
	 */
	m_pBootLog->Attach (0, 0);	/* no on-screen output; file only */
	m_pBootLog->Stage (BootStageScreen);

	/*
	 * Clear the text console.
	 *
	 * With the logger silenced nothing more is written to it, so whatever the
	 * firmware or Circle left behind would otherwise stay on screen until the
	 * framebuffer appears.  Clearing it makes the screen black from the start,
	 * which is what the emulated machine's own screen should replace.
	 */
	m_Screen.Write ("\x1b[2J\x1b[H", 7);	/* clear, home; no text */

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
		 * Reported on the text console as well as in the log: the log file
		 * cannot be opened without a card, and the framebuffer does not exist
		 * yet, so the stage colour cannot be painted either.  Circle's console
		 * is the only output left at this point.
		 */
		m_pBootLog->Record ("PANIC: cannot mount the SD card");
		m_Screen.Write ("The SD card could not be mounted.\r\n", 33);

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

	if (!ATARI_Init (pConfig))
	{
		m_pBootLog->Recordf ("PANIC: the emulation core failed to start: %s",
				   ATARI_LastError ());
		ReportStartupFailure ("The emulation core could not be started");
		return FALSE;
	}
	m_pBootLog->Recordf ("Emulation core started: %s", ATARI_CoreVersion ());

	// -----------------------------------------------------------------------
	// Keyboard and controllers
	// -----------------------------------------------------------------------
	m_pBootLog->Stage (BootStageInput);

	if (!InitializeKeyboard ())
	{
		ReportStartupFailure ("No keyboard could be set up");
		return FALSE;
	}

	// Controllers are not essential: a machine with none still runs, and one
	// plugged in later is picked up.  Failures are logged, never fatal.
	InitializeJoysticks ();

	MountMediaFromSettings ();

	// -----------------------------------------------------------------------
	// Menu
	// -----------------------------------------------------------------------
	m_Menu.Initialize (&m_Video, &m_Storage, this, &m_Settings, m_SettingsPath);

	// Persist whatever was loaded, so that a first run leaves a settings file
	// the user can find and edit.
	Settings_Save (m_Storage, m_SettingsPath, m_Settings);

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

		if (!m_pKeyboard->Initialize ())
		{
			m_Logger.Write (FromKernel, LogWarning,
					"No USB keyboard attached; the emulator runs but cannot be used");
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

	// --- USB gamepads -------------------------------------------------------
	//
	// Four are supported, matching the core's four controller inputs.  Each is
	// looked up by index, and a controller that is not attached simply reports
	// itself absent until one is.
	if (m_Settings.bUSBJoysticks)
	{
		unsigned nUSBFound = 0;
		for (unsigned n = 1; n <= ATARI_JOYSTICK_COUNT; n++)
		{
			CAtariJoystick *pJoystick = ATARI_CreateUSBJoystick (&m_USBHCI, n);
			if (pJoystick == 0)
			{
				continue;
			}

			pJoystick->Initialize ();

			/*
			 * The slot is filled even when nothing is attached: a USB gamepad
			 * plugged in later is then noticed by the driver's own IsPresent(),
			 * without the kernel having to poll for new devices.
			 */
			ATARI_SetJoystick ((int) n - 1, pJoystick->Handle ());
			nUSBFound++;
		}

		if (nUSBFound > 0)
		{
			m_Logger.Write (FromKernel, LogNotice,
					"Watching %u USB gamepad input%s; plug one in at any time",
					nUSBFound, nUSBFound == 1 ? "" : "s");
		}
	}

	// --- 9-pin ports on GPIO ------------------------------------------------
	if (!m_Settings.bGPIOJoysticks)
	{
		return;
	}

	if (bGPIOKeyboard)
	{
		m_Logger.Write (FromKernel, LogNotice,
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

void CAtariKernel::RunFrame (void)
{
	// The menu takes precedence over the emulated machine while it is open:
	// the same arrow keys move the selection rather than the Atari cursor.
	if (m_pKeyboard != 0 && m_pKeyboard->ConsumeMenuRequest ())
	{
		m_Menu.Toggle ();
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

	// Scale the emulated screen into the framebuffer, then let the menu draw
	// over it.
	const u8 *pScreen = ATARI_ScreenIndices ();
	if (pScreen != 0)
	{
		m_Video.FillScreen (pScreen, (const u32 *) ATARI_Palette ());
	}

	if (m_Menu.IsOpen ())
	{
		m_Menu.Draw ();
	}

	m_Video.Present ();

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

const char *CAtariKernel::GetLastError (void)
{
	return ATARI_LastError ();
}

double CAtariKernel::GetFps (void)
{
	return ATARI_Fps ();
}
