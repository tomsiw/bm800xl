//
// kernel.h
//
// The emulator's main application class.
//
// Circle calls Initialize() once and then Run(), which is where the machine
// spends the rest of its life.  Everything this class owns is either a Circle
// subsystem (timer, USB, SD card) or one of the emulator's own modules, and its
// job is to bring them up in the right order, translate settings into the
// emulation configuration, and drive the frame loop.
//
#ifndef _atari_kernel_h
#define _atari_kernel_h

#include <circle/actled.h>
#include <circle/devicenameservice.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/screen.h>
#include <circle/serial.h>
#include <circle/timer.h>
#include <circle/types.h>
#include <circle/usb/usbhcidevice.h>

#include <SDCard/emmc.h>

#include <stdint.h>

#include "atari_port.h"
#include "audio_out.h"
#include "bootlog.h"
#include "config/settings.h"
#include "input/keyboard.h"
#include "osd/osd_menu.h"
#include "storage/storage.h"
#include "video/video_out.h"

// Circle does not ship a kernel class: every application defines its own, which
// is why this one does too.
enum TShutdownMode
{
	ShutdownNone,
	ShutdownHalt,
	ShutdownReboot
};

/*
 * An exception handler that writes to the runtime log as well as the serial port.
 *
 * Circle's own handler dumps the exception to the text console and halts.  On a
 * machine with no serial console that dump exists only on the screen, so a fault
 * that leaves the picture frozen leaves no record of itself anywhere -- which is
 * exactly the situation this project kept hitting.  Overriding Throw() puts the
 * same information in RUNTIMELOG.TXT, where it survives the machine being turned
 * off.
 *
 * With the text console removed, the runtime log is the only place a fault can be
 * recorded at all: the base implementation's dump no longer has a screen to go to.
 */
class CAtariExceptionHandler : public CExceptionHandler
{
public:
	void Throw (unsigned nException, TAbortFrame *pFrame) override;
};

class CKernel
{
public:
	CKernel (void);
	~CKernel (void);

	boolean Initialize (void);

	TShutdownMode Run (void);

protected:
	// Do not change this order: each of these needs the ones before it.
	// Protected rather than private so that the emulator subclass can use them.
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	/*
	 * No CScreenDevice here.  The text console allocated its own framebuffer,
	 * cleared it, displayed a few lines of start-up text, and was then replaced by
	 * the emulator's own framebuffer -- a visible text flash and a second display
	 * mode switch on the way to the picture.  Everything it was used for is now
	 * buffered in the boot log (which is written to the SD card as soon as one is
	 * mounted) and, if one is attached, sent to the serial port.
	 */
	CSerialDevice		m_Serial;
	CAtariExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogger			m_Logger;
};

class CAtariKernel : public CKernel, public CAtariMenuHost
{
public:
	CAtariKernel (void);
	~CAtariKernel (void);

	/// \return Initialization successful?
	boolean Initialize (void);

	/// \return Reason for the emulator to stop, if it ever does
	TShutdownMode Run (void);

	// --- CAtariMenuHost ---------------------------------------------------
	boolean ApplySettings (const TAtariSettings &Settings);
	boolean MountDisk (int nDrive, const char *pPath, boolean bReadOnly);
	boolean MountCartridge (const char *pPath);
	boolean MountCassette (const char *pPath, boolean bReadOnly);
	boolean SaveSnapshot (const char *pPath);
	boolean LoadSnapshot (const char *pPath);
	void ColdReset (void);
	void WarmReset (void);
	const char *GetLastError (void);
	double GetFps (void);

private:
	// Builds the configuration the emulation core needs from the settings.
	// Not const: it fills in m_EmulatorConfig, which the core holds a pointer
	// to for the duration of ATARI_Init().
	void MakeEmulatorConfig (const atari_config_t *&pConfig);

	// Mounts everything the settings say should be mounted.
	void MountMediaFromSettings (void);

	// Brings up the audio device and hands it to the emulation layer.
	boolean InitializeAudio (void);

	// Creates the keyboard the settings ask for.
	boolean InitializeKeyboard (void);

	/*
	 * Reports a failed start-up on screen and holds it there.
	 *
	 * Anything that stops the emulator before it can run must go through this,
	 * so that the failure is visible without a serial cable.  Returns after a
	 * delay; the caller is expected to return FALSE so that main() halts.
	 */
	void ReportStartupFailure (const char *pWhat);

	/*
	 * Brings up the controllers: USB gamepads, and the two 9-pin ports on GPIO.
	 *
	 * Never fatal.  A machine with no controller is a normal thing to have, and
	 * one plugged in later is noticed by the drivers.
	 */
	void InitializeJoysticks (void);

	// Runs one frame: keyboard, emulation, screen, sound, menu.
	void RunFrame (void);

	// Reports keyboard presence once it is a fact rather than a guess about
	// enumeration timing.  Called at the top of every frame.
	void UpdateKeyboardStatus (void);

	// The host clock and idle functions the emulation core calls.  Their
	// signatures are fixed by src/port/atari_port.h.
	static uint64_t GetHostClock (void *pContext);
	static void IdleUntil (void *pContext, uint32_t nMicroseconds);

private:
	/*
	 * Start-up progress, and the report shown if start-up fails.
	 *
	 * A pointer, not a member: the log holds about 8 KB of buffers and this
	 * object is a local in main(), so a member would put them on the stack.  See
	 * CBootLog::Get().
	 *
	 * Declared first because it is initialised first, and because it is the
	 * thing that describes everything below it.  Set in Initialize(), not in the
	 * constructor, so that it cannot be created before Circle's timer exists.
	 */
	CBootLog	       *m_pBootLog;

	// The USB controller and the SD card interface need the interrupt system
	// and the timer, both of which come from CKernel.
	CUSBHCIDevice		m_USBHCI;
	CEMMCDevice		m_EMMC;

	// Emulator modules
	CAtariStorage		m_Storage;
	CAtariVideoOut		m_Video;
	CAtariMenu		m_Menu;
	CAtariKeyboard	       *m_pKeyboard;
	audio_sink_t	       *m_pAudioSink;

	// Configuration in force
	TAtariSettings		m_Settings;
	atari_config_t		m_EmulatorConfig;

	char			m_SettingsPath[SETTINGS_PATH_MAX];

	boolean			m_bInitialized;
	boolean			m_bAudioOpen;

	/*
	 * Whether the USB host controller started.  Enumeration is driven from the
	 * frame loop, and asking a controller that failed to start would only
	 * assert.
	 */
	boolean			m_bUSBReady;

	// Set once the first frame has been drawn, so that a hang during start-up
	// can be told from a hang after the emulator was running.
	boolean			m_bFirstFrameDrawn;

	/*
	 * Frame errors are reported once per distinct reason rather than every
	 * frame, and a CPU crash is the only one that resets the machine.  See
	 * RunFrame().
	 */
	boolean			m_bFrameErrorReported;
	char			m_LastFrameError[96];

	/*
	 * Keyboard presence, reported when it is a fact rather than a guess.
	 *
	 * Start-up scans the USB bus before Circle has registered a keyboard, so a
	 * message there would appear on a machine whose keyboard works -- which is
	 * what it used to do, and why it was worth ignoring.  The frame loop checks
	 * instead: a notice when a keyboard appears, and a warning only once enough
	 * frames have gone by that its absence is real.
	 */
	boolean			m_bKeyboardSeen;
	boolean			m_bKeyboardWarned;
	unsigned		m_nKeyboardFrames;
};

#endif
