//
// kernel.cpp
//
// Circle - A C++ bare metal environment for Raspberry Pi
// Copyright (C) 2014-2020  R. Stange <rsta2@o2online.de>
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include "kernel.h"

#include <emmc.h>			// addon/SDCard
#include "ff.h"
#include <assert.h>

static const char FromKernel[] = "kernel";

/*
 * Where Circle's log goes: the screen, as the sample does, and GAMEPAD.LOG.
 *
 * Circle timestamps every line itself ("00:00:03.79"), so routing its messages
 * through here gives the file timestamps without this program formatting any.
 */

/*
 * Messages waiting to be logged.
 *
 * The status handler runs in the USB interrupt, and logging from there now means
 * writing to FatFs as well as the screen -- FatFs is not reentrant and performs
 * disk I/O, so it cannot be called from an interrupt.  The handler deposits a line
 * here and Run() writes it, where both targets are safe.
 */
class CLogTeeDevice : public CDevice
{
public:
	CLogTeeDevice (CScreenDevice *pScreen, FIL *pFile, const volatile boolean *pbOpen)
	:	m_pScreen (pScreen), m_pFile (pFile), m_pbOpen (pbOpen)
	{
	}

	int Write (const void *pBuffer, size_t nCount) override
	{
		if (m_pScreen != 0)
		{
			m_pScreen->Write (pBuffer, nCount);
		}

		if (m_pFile != 0 && m_pbOpen != 0 && *m_pbOpen)
		{
			UINT nWritten = 0;
			f_write (m_pFile, pBuffer, (UINT) nCount, &nWritten);
			f_sync (m_pFile);
		}

		return (int) nCount;
	}

private:
	CScreenDevice		*m_pScreen;
	FIL			*m_pFile;
	const volatile boolean	*m_pbOpen;
};

static FATFS	s_FileSystem;
static FIL	s_LogFile;
static boolean	s_bLogOpen = FALSE;


CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_USBHCI (&m_Interrupt, &m_Timer, TRUE)			// TRUE: enable plug-and-play
{
	for (unsigned i = 0; i < MAX_GAMEPADS; i++)
	{
		m_pGamePad[i] = 0;
	}

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
		bOK = m_Serial.Initialize (115200);
	}

	if (bOK)
	{
		CDevice *pTarget = m_DeviceNameService.GetDevice (m_Options.GetLogDevice (), FALSE);
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

	/*
	 * The card, then the logger's tee.
	 *
	 * Placed before the USB controller is started, so that Circle's enumeration
	 * messages -- device IDs, product names, transfer errors -- are written to
	 * GAMEPAD.LOG as they happen.  A fresh file each boot: FA_CREATE_ALWAYS.
	 */
	if (bOK)
	{
		static CEMMCDevice EMMC (&m_Interrupt, &m_Timer, &m_ActLED);

		if (   EMMC.Initialize ()
		    && f_mount (&s_FileSystem, "SD:", 1) == FR_OK
		    && f_open (&s_LogFile, "GAMEPAD.LOG",
			       FA_WRITE | FA_CREATE_ALWAYS) == FR_OK)
		{
			s_bLogOpen = TRUE;
		}

		static CLogTeeDevice Tee (&m_Screen, &s_LogFile, &s_bLogOpen);
		m_Logger.Initialize (&Tee);
	}

	if (bOK)
	{
		bOK = m_USBHCI.Initialize ();
	}

	return bOK;
}

TShutdownMode CKernel::Run (void)
{
	m_Logger.Write (FromKernel, LogNotice, "Compile time: " __DATE__ " " __TIME__);

	m_Logger.Write (FromKernel, LogNotice, "Please attach an USB gamepad, if not already done!");

	for (unsigned nCount = 0; 1; nCount++)
	{
		// This must be called from TASK_LEVEL to update the tree of connected USB devices.
		boolean bUpdated = m_USBHCI.UpdatePlugAndPlay ();

		for (unsigned nDevice = 1; bUpdated && nDevice <= MAX_GAMEPADS; nDevice++)
		{
			if (m_pGamePad[nDevice-1] != 0)
			{
				continue;
			}

			m_pGamePad[nDevice-1] = (CUSBGamePadDevice *)
				m_DeviceNameService.GetDevice ("upad", nDevice, FALSE);
			if (m_pGamePad[nDevice-1] == 0)
			{
				continue;
			}

			const TGamePadState *pState = m_pGamePad[nDevice-1]->GetInitialState ();
			assert (pState != 0);

			m_Logger.Write (FromKernel, LogNotice, "Gamepad %u: %d Button(s) %d Hat(s)",
					nDevice, pState->nbuttons, pState->nhats);

			for (int i = 0; i < pState->naxes; i++)
			{
				m_Logger.Write (FromKernel, LogNotice,
						"Gamepad %u: Axis %d: Minimum %d Maximum %d",
						nDevice, i+1, pState->axes[i].minimum,
						pState->axes[i].maximum);
			}

			m_pGamePad[nDevice-1]->RegisterRemovedHandler (GamePadRemovedHandler, this);
			m_pGamePad[nDevice-1]->RegisterStatusHandler (GamePadStatusHandler);

			m_Logger.Write (FromKernel, LogNotice, "Use your gamepad controls!");
		}


		m_Screen.Rotor (0, nCount);
		m_Timer.MsDelay (100);
	}

	return ShutdownHalt;
}

CString CKernel::m_LastMsg;

void CKernel::GamePadStatusHandler (unsigned nDeviceIndex, const TGamePadState *pState)
{
	CString Msg;
	Msg.Format ("Gamepad %u: Buttons 0x%X", nDeviceIndex+1, pState->buttons);

	CString Value;

	if (pState->naxes > 0)
	{
		Msg.Append (" Axes");

		for (int i = 0; i < pState->naxes; i++)
		{
			Value.Format (" %d", pState->axes[i].value);
			Msg.Append (Value);
		}
	}

	if (pState->nhats > 0)
	{
		Msg.Append (" Hats");

		for (int i = 0; i < pState->nhats; i++)
		{
			Value.Format (" %d", pState->hats[i]);
			Msg.Append (Value);
		}
	}

	/*
	 * Only what changed.
	 *
	 * The handler runs for every report the pad sends -- dozens a second while a
	 * stick is held -- and most reports repeat the previous state exactly.  The
	 * timestamp is added by the logger, so comparing this text compares the
	 * content alone, which is what "differs" means here: a line that differs only
	 * by its time is the same line and is not printed again.
	 */

	int nCompare = m_LastMsg.GetLength() > 0 ? m_LastMsg.Compare (Msg) : 1;
	m_LastMsg = Msg;

	if (nCompare == 0)
	{
		return;
	}

	CLogger::Get ()->Write (FromKernel, LogNotice, Msg);
}

void CKernel::GamePadRemovedHandler (CDevice *pDevice, void *pContext)
{
	CKernel *pThis = (CKernel *) pContext;
	assert (pThis != 0);

	for (unsigned i = 0; i < MAX_GAMEPADS; i++)
	{
		if (pThis->m_pGamePad[i] == (CUSBGamePadDevice *) pDevice)
		{
			CLogger::Get ()->Write (FromKernel, LogDebug, "Gamepad %u removed", i+1);

			pThis->m_pGamePad[i] = 0;

			break;
		}
	}
}
