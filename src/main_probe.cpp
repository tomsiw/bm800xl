/*
 * main_probe.cpp -- a deliberately minimal replacement for main.cpp.
 *
 * WHY THIS EXISTS
 * ---------------
 * The full emulator starts and dies with a black screen, before it can print
 * anything, while Circle's own sample program runs perfectly on the same card,
 * config, firmware and armstub.  Every explanation that could be checked from
 * the build -- load address, image size, the size guard in sysinit(), static
 * constructors, the boot chain -- has been ruled out.
 *
 * What remains is a difference between how this project links and how the
 * working sample links.  The most obvious one is that this project links
 * newlib's libc and libm alongside Circle, while the sample links only Circle.
 * Putting newlib in front of Circle means its strlen/memset/memcpy are used in
 * preference to Circle's, in a program Circle's own code also runs in.
 *
 * This file tests that, and nothing else.  It is built by the same makefile,
 * with the same libraries in the same order, linked by the same toolchain, and
 * produces a kernel whose only action is to print two lines to the screen.
 *
 * It uses no part of the emulator, and it does not initialise USB, the SD card,
 * the framebuffer, or anything else.  If this prints, then linking newlib the
 * way this project does is sound, and the fault is in the emulator's own
 * start-up; if it does not print, the fault is in the linkage itself.
 */

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

#include <circle/startup.h>

#include <stdio.h>
#include <string.h>

/* Named for the probe, so it cannot collide with the emulator's own enum of
 * the same shape in kernel.h. */
enum TProbeShutdownMode
{
	ProbeShutdownNone,
	ProbeShutdownHalt,
	ProbeShutdownReboot
};

/*
 * The same member order as the emulator's CKernel base, because that order is
 * part of what is being tested: several of these need the ones before them.
 */
class CProbeKernel
{
public:
	CProbeKernel (void);
	~CProbeKernel (void);

	boolean Initialize (void);

	TProbeShutdownMode Run (void);

private:
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CScreenDevice		m_Screen;
	CSerialDevice		m_Serial;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogger			m_Logger;
};

/*
 * The same member order and the same initialiser list as the emulator's CKernel,
 * because that is part of what is being tested: the screen size comes from the
 * kernel options, the timer and serial device need the interrupt system, and the
 * logger needs the timer.
 */
CProbeKernel::CProbeKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	m_Serial (&m_Interrupt, 0),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer)
{
	m_ActLED.Blink (5);		/* prove the kernel is alive */
}

CProbeKernel::~CProbeKernel (void)
{
}

boolean CProbeKernel::Initialize (void)
{
	boolean bOK = TRUE;

	if (bOK)
	{
		bOK = m_Screen.Initialize ();
	}

	if (bOK)
	{
		/* Deliberately before anything else can fail, and straight to the
		 * console: this is the earliest a kernel can say anything. */
		m_Screen.Write ("\x1b[2J\x1b[H", 7);
		m_Screen.Write ("PROBE A: kernel running, screen up.\r\n", 37);
	}

	if (bOK)
	{
		bOK = m_Serial.Initialize (115200);
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
		m_Screen.Write ("PROBE B: logger up.\r\n", 22);
		m_Logger.Write ("probe", LogNotice, "PROBE C: Logger.Write works");
	}

	if (bOK)
	{
		bOK = m_Interrupt.Initialize ();
		m_Screen.Write ("PROBE D: interrupt system up.\r\n", 32);
	}

	if (bOK)
	{
		bOK = m_Timer.Initialize ();
		m_Screen.Write ("PROBE E: timer up.\r\n", 21);
	}

	if (bOK)
	{
		/*
		 * The newlib calls this project uses throughout.
		 *
		 * Skipped when PROBE_NO_NEWLIB is defined, which is how the Circle-only
		 * variant of the probe is built: that variant exists to test the linkage
		 * rather than the C library, and Circle's own library has no snprintf, so
		 * including this would only make that variant fail to link.
		 */
#ifndef PROBE_NO_NEWLIB
		char Buffer[64];

		snprintf (Buffer, sizeof Buffer, "PROBE F: printf-style %d %s\r\n", 42, "works");
		m_Screen.Write (Buffer, strlen (Buffer));
#else
		m_Screen.Write ("PROBE F: skipped (Circle-only linkage)\r\n", 40);
#endif

		m_Screen.Write ("PROBE G: all probes passed.\r\n", 30);
	}

	return bOK;
}

TProbeShutdownMode CProbeKernel::Run (void)
{
	m_Screen.Write ("PROBE H: running.\r\n", 19);

	for (;;)
	{
		m_Timer.MsDelay (500);
		m_ActLED.Blink (2);		/* a slow heartbeat, so a hang is visible */
	}

	return ProbeShutdownNone;
}

int main (void)
{
	CProbeKernel *pKernel = new CProbeKernel;

	if (!pKernel->Initialize ())
	{
		pKernel->Run ();
		halt ();
		return EXIT_HALT;
	}

	TProbeShutdownMode Mode = pKernel->Run ();

	delete pKernel;

	if (Mode == ProbeShutdownReboot)
	{
		reboot ();
		return EXIT_REBOOT;
	}

	halt ();
	return EXIT_HALT;
}
