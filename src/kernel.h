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
 * An exception handler that writes to the runtime log as well as the screen.
 *
 * Circle's own handler dumps the exception to the text console and halts.  On a
 * machine with no serial console that dump exists only on the screen, so a fault
 * that leaves the picture frozen leaves no record of itself anywhere -- which is
 * exactly the situation this project kept hitting.  Overriding Throw() puts the
 * same information in RUNTIMELOG.TXT, where it survives the machine being turned
 * off.
 *
 * The base implementation is still called, so the on-screen dump is unchanged.
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
	CScreenDevice		m_Screen;
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
};

#endif
