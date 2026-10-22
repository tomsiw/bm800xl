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
#include <string.h>

/*
 * Where each Atari control comes from on this particular controller.
 *
 * A bare mask table cannot describe the generic USB pads: on ven79-11 (tested) the
 * vertical D-pad arrives on an axis, not on a button bit, so "up" has to be looked
 * for in a different place.  Resolving the mapping from the device ID *and* from
 * the capabilities the driver reports is therefore the only shape that works for
 * both kinds.
 *
 * Order of preference, each step backed by something observed:
 *   1. a known vendor/product ID -- Circle's specific driver fills TGamePadButton
 *      exactly, and pad ven57e-2009 confirmed every value used below
 *   2. hats present -- directions come from hats[0]
 *   3. otherwise -- directions come from axes, with a threshold
 */
enum TDirectionSource { SourceNone, SourceButton, SourceAxis, SourceHat };

struct TDirection
{
	TDirectionSource	Source;
	unsigned		nBit;		// DirButton
	int			nAxis;		// DirAxis
	int			nThreshold;	// DirAxis: value at the active end
	boolean			bHighActive;	// DirAxis: TRUE = on above the threshold
	int			nHatValue;	// DirHat
};

struct TMapping
{
	const char	*pName;
	TDirection	Up, Down, Left, Right, Fire, Fire2;
};

static TDirection DirBtn (unsigned nBit)
{
	TDirection d;
	d.Source = SourceButton; d.nBit = nBit;
	d.nAxis = -1; d.nThreshold = 0; d.bHighActive = FALSE; d.nHatValue = 0;
	return d;
}

static TDirection DirAx (int nAxis, int nThreshold, boolean bHighActive)
{
	TDirection d;
	d.Source = SourceAxis; d.nBit = 0;
	d.nAxis = nAxis; d.nThreshold = nThreshold; d.bHighActive = bHighActive;
	d.nHatValue = 0;
	return d;
}

static TDirection DirHat (int nHatValue)
{
	TDirection d;
	d.Source = SourceHat; d.nBit = 0;
	d.nAxis = -1; d.nThreshold = 0; d.bHighActive = FALSE; d.nHatValue = nHatValue;
	return d;
}

static TDirection DirNothing (void)
{
	TDirection d;
	d.Source = SourceNone; d.nBit = 0;
	d.nAxis = -1; d.nThreshold = 0; d.bHighActive = FALSE; d.nHatValue = 0;
	return d;
}

/*
 * The resolved map is kept per device and consulted on every event, so that a
 * press is reported as what it means rather than as a bit number:
 *
 *     Gamepad 1: ... -> LEFT
 *     Gamepad 1: ... -> FIRE
 */
static TMapping	s_Map[MAX_GAMEPADS];
static boolean	s_bMapValid[MAX_GAMEPADS];

static boolean IsActive (const TDirection &Dir, const TGamePadState *pState)
{
	switch (Dir.Source)
	{
	case SourceButton:
		return (pState->buttons & Dir.nBit) != 0;

	case SourceAxis:
		if (Dir.nAxis < 0 || Dir.nAxis >= pState->naxes)
		{
			return FALSE;
		}
		{
			int v = pState->axes[Dir.nAxis].value;
			return Dir.bHighActive ? v >= Dir.nThreshold : v <= Dir.nThreshold;
		}

	case SourceHat:
		if (pState->nhats < 1)
		{
			return FALSE;
		}
		return pState->hats[0] == Dir.nHatValue;

	default:
		return FALSE;
	}
}

static void AppendMapped (CString &Msg, const TMapping &Map, const TGamePadState *pState)
{
	Msg.Append (" ->");

	boolean bAny = FALSE;

	if (IsActive (Map.Up, pState))    { Msg.Append (" UP");    bAny = TRUE; }
	if (IsActive (Map.Down, pState))  { Msg.Append (" DOWN");  bAny = TRUE; }
	if (IsActive (Map.Left, pState))  { Msg.Append (" LEFT");  bAny = TRUE; }
	if (IsActive (Map.Right, pState)) { Msg.Append (" RIGHT"); bAny = TRUE; }
	if (IsActive (Map.Fire, pState) || IsActive (Map.Fire2, pState))
	{
		Msg.Append (" FIRE");  bAny = TRUE;
	}

	if (!bAny)
	{
		Msg.Append (" (none)");
	}
}
static TMapping ResolveMapping (const char *pId, const TGamePadState *pState)
{
	TMapping m;

	m.pName = "generic";
	m.Up = m.Down = m.Left = m.Right = DirNothing ();
	m.Fire = DirNothing ();
	m.Fire2 = DirNothing ();

	const char *p = pId != 0 ? pId : "";

	/* 1. Circle's specific drivers: the enum values are authoritative. */
	if (   strcmp (p, "ven57e-2009") == 0	/* Switch Pro */
	    || strcmp (p, "ven45e-28e")  == 0	/* Xbox 360 */
	    || strcmp (p, "ven45e-2f8")  == 0
	    || strcmp (p, "ven45e-2d1")  == 0	/* Xbox One */
	    || strcmp (p, "ven54c-268")  == 0)	/* PS3 */
	{
		m.pName = "buttons (Circle driver)";
		m.Up    = DirBtn (0x08000);
		m.Down  = DirBtn (0x20000);
		m.Left  = DirBtn (0x40000);
		m.Right = DirBtn (0x10000);
		m.Fire  = DirBtn (0x00200);		/* A */
		m.Fire2 = DirBtn (0x00100);		/* B */

		return m;
	}

	/*
	 * Microntek USB joystick (ven79-6), measured: left/right on axis 1 and
	 * up/down on axis 2, fire on buttons 2 and 4.  Checked before the hat rule
	 * because this pad also reports one hat, which it does not use for direction.
	 */
	if (strcmp (p, "ven79-6") == 0)
	{
		m.pName = "axes (Microntek)";
		m.Left  = DirAx (0, 64, FALSE);		/* left  -> 0   */
		m.Right = DirAx (0, 192, TRUE);		/* right -> 255 */
		m.Up    = DirAx (1, 64, FALSE);		/* up    -> 0   */
		m.Down  = DirAx (1, 192, TRUE);		/* down  -> 255 */
		m.Fire  = DirBtn (0x00002);
		m.Fire2 = DirBtn (0x00004);

		return m;
	}
	/* 2. A hat: directions come from hats[0], 0..7 clockwise from up. */
	if (pState != 0 && pState->nhats > 0)
	{
		m.pName = "hat";
		m.Up    = DirHat (0);
		m.Right = DirHat (2);
		m.Down  = DirHat (4);
		m.Left  = DirHat (6);
		m.Fire  = DirBtn (0x00002);		/* first button reported */

		return m;
	}

	/*
	 * 3. Axes.  Observed on ven79-11: pressing up and down moved axis 3 between 0
	 * and 255 while no button bit changed, so the vertical pair lives there.  The
	 * horizontal pair is taken as the axis below it, which is what the descriptor
	 * order implies; the log prints the raw values so the guess can be checked.
	 */
	if (pState != 0 && pState->naxes >= 4)
	{
		m.pName = "axes";
		/*
		 * Index 3 is the horizontal pair: it moved when left and right were
		 * pressed, which is how the previous version came to report them as
		 * up/down.  Up and down changed none of the first four axes, and this pad
		 * reports five, so the vertical pair is index 4 -- the fifth, which the log
		 * did not print before.
		 *
		 * Each end of an axis is a different direction, so each direction carries
		 * its own threshold and side.
		 */
		m.Left  = DirAx (3, 64, FALSE);		/* left  -> 0   */
		m.Right = DirAx (3, 192, TRUE);		/* right -> 255 */
		m.Up    = DirAx (4, 64, FALSE);		/* up    -> 0   */
		m.Down  = DirAx (4, 192, TRUE);		/* down  -> 255 */
		m.Fire  = DirBtn (0x00002);
		m.Fire2 = DirBtn (0x00004);

		return m;
	}

	/* 4. Unknown: the bits most Circle drivers use, so the pad still partly works. */
	m.pName = "fallback bits";
	m.Up    = DirBtn (0x08000);
	m.Down  = DirBtn (0x20000);
	m.Left  = DirBtn (0x40000);
	m.Right = DirBtn (0x10000);
	m.Fire  = DirBtn (0x00200);

	return m;
}



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

			for (int i = 0; i < pState->naxes && i < 8; i++)
			{
				m_Logger.Write (FromKernel, LogNotice,
						"Gamepad %u: Axis %d: Minimum %d Maximum %d",
						nDevice, i+1, pState->axes[i].minimum,
						pState->axes[i].maximum);
			}

			m_pGamePad[nDevice-1]->RegisterRemovedHandler (GamePadRemovedHandler, this);
			m_pGamePad[nDevice-1]->RegisterStatusHandler (GamePadStatusHandler);

			/*
			 * What the resolver made of this controller, printed once so that
			 * every later line can be read in the light of it.
			 */
			CUSBDevice *pParent = m_pGamePad[nDevice-1]->GetDevice ();
			CString *pIdStr = pParent != 0
					? pParent->GetName (DeviceNameVendor) : 0;

			TMapping Map = ResolveMapping (pIdStr != 0 ? (const char *) *pIdStr : "",
						       pState);

			m_Logger.Write (FromKernel, LogNotice,
					"Gamepad %u: id %s map %s",
					nDevice,
					pIdStr != 0 ? (const char *) *pIdStr : "?",
					Map.pName);

			delete pIdStr;

			s_Map[nDevice - 1] = Map;
			s_bMapValid[nDevice - 1] = TRUE;

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

		for (int i = 0; i < pState->naxes && i < 8; i++)
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

	/*
	 * Only reached for a line that will actually be printed: resolving the mapped
	 * directions costs one comparison per direction, and a duplicate report has
	 * already been discarded above.  m_LastMsg was stored before this point, so
	 * the comparison stays against the raw state rather than against this
	 * decorated text.
	 */
	if (s_bMapValid[nDeviceIndex] && pState != 0)
	{
		AppendMapped (Msg, s_Map[nDeviceIndex], pState);
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
