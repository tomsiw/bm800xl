/*
 * usb_joystick.cpp -- a USB gamepad used as an Atari joystick.
 *
 * A modern controller has far more than an Atari joystick does, so this maps the
 * generous input onto the sparse one:
 *
 *   - the D-pad (hat) if the controller has one, otherwise the left stick;
 *   - the first action button (A / Cross) and the second (B / Circle) as the
 *     single fire button, because either is the natural thing to press;
 *   - the left shoulder button as fire as well, which is what most people reach
 *     for on a controller shaped like a console pad.
 *
 * The Atari has exactly one button, so anything beyond that is mapped onto the
 * same button rather than being ignored: a player should not have to discover
 * which of four buttons the emulator happens to accept.
 *
 * Circle already understands the common controller families and normalises them
 * into TGamePadState, so this does not need to know one controller from another.
 * For an unrecognised controller the raw buttons and hat are still reported, and
 * this driver falls back to reading those directly.
 */

#include "joystick.h"

#include <circle/logger.h>
#include <circle/usb/usbgamepad.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/devicenameservice.h>
#include <circle/synchronize.h>
#include <circle/timer.h>

#include <stdio.h>

static const char FromJoystick[] = "usbjoy";

/*
 * How many gamepads are watched.
 *
 * This matches the emulation core's four controller inputs rather than any
 * limit of Circle's: the sample code defines its own USB_JOYSTICK_COUNT of 2, so the
 * value is not available from a header and is stated here.  Four USB gamepads
 * can be attached at once, which is one more than the Atari's four controller
 * inputs can express in the two-player case and exactly enough for four-player
 * games.
 */
#define USB_JOYSTICK_COUNT 4

/*
 * A controller that has stopped sending reports is treated as released after
 * this long.  Without it, unplugging a controller mid-press would leave the
 * emulated joystick stuck in that direction, which is far more annoying than
 * the alternative.
 */
#define USB_JOYSTICK_TIMEOUT_MS 1000

/*
 * An axis is read as a direction when it is beyond one of these fractions of
 * its range from the centre.
 *
 * A quarter is deliberately generous.  An analog stick at rest is never exactly
 * centred, and a controller's range is rarely symmetric; a tighter threshold
 * would make the joystick drift on its own, which is much worse than needing a
 * deliberate push.  The dead zone matters more than precision here, because the
 * Atari only sees four directions.
 */
#define USB_JOYSTICK_AXIS_DIVISOR 4

class CUSBAtariJoystick : public CAtariJoystick
{
public:
	CUSBAtariJoystick (CUSBHCIDevice *pUSBHCI, unsigned nIndex);
	~CUSBAtariJoystick (void);

	boolean Initialize (void);

	unsigned GetDirections (void);
	int GetTrigger (void);
	boolean IsPresent (void);
	const char *GetName (void);

	/*
	 * The USB stack calls this from interrupt context when a report arrives.
	 *
	 * It does nothing but copy the state and note the time.  Decoding is left to
	 * GetDirections(), in the main loop, so that no reporting happens from an
	 * interrupt and the main loop always sees one coherent state rather than a
	 * half-updated one.
	 */
	static void StatusHandler (unsigned nDeviceIndex, const TGamePadState *pState);

	/* Called when the controller is unplugged. */
	static void RemovedHandler (CDevice *pDevice, void *pContext);

private:
	CUSBHCIDevice *m_pUSBHCI;
	unsigned m_nIndex;			/* 1-based */

	CUSBGamePadDevice *m_pDevice;

	/*
	 * A snapshot of the most recent report, plus when it arrived.
	 *
	 * Marked volatile because the interrupt handler writes it and the main loop
	 * reads it; a lock would be worse than useless here, since nothing is
	 * computed from it, only copied.  The fields are read in an order that
	 * cannot mix two reports: the sequence number is written last and checked
	 * first.
	 */
	volatile boolean m_bHaveState;
	volatile unsigned m_nButtons;
	volatile int m_nHats[MAX_HATS];
	volatile int m_nNaxes;
	volatile int m_nAxis[MAX_AXIS];
	volatile int m_nAxisMin[MAX_AXIS];
	volatile int m_nAxisMax[MAX_AXIS];
	volatile unsigned m_nSequence;
	unsigned m_nLastSeen;

	/* Reports are delivered per device index, so the handler needs to find the
	 * object that owns this index. */
	static CUSBAtariJoystick *s_pInstance[USB_JOYSTICK_COUNT];
};

CUSBAtariJoystick *CUSBAtariJoystick::s_pInstance[USB_JOYSTICK_COUNT] = { 0, 0, 0, 0 };

CUSBAtariJoystick::CUSBAtariJoystick (CUSBHCIDevice *pUSBHCI, unsigned nIndex)
:	m_pUSBHCI (pUSBHCI),
	m_nIndex (nIndex),
	m_pDevice (0),
	m_bHaveState (FALSE),
	m_nButtons (0),
	m_nNaxes (0),
	m_nSequence (0),
	m_nLastSeen (0)
{
	for (unsigned i = 0; i < MAX_HATS; i++)
	{
		m_nHats[i] = -1;			/* centred */
	}
	for (unsigned i = 0; i < MAX_AXIS; i++)
	{
		m_nAxis[i] = 0;
		m_nAxisMin[i] = 0;
		m_nAxisMax[i] = 255;
	}

	if (m_nIndex >= 1 && m_nIndex <= USB_JOYSTICK_COUNT)
	{
		s_pInstance[m_nIndex - 1] = this;
	}
}

CUSBAtariJoystick::~CUSBAtariJoystick (void)
{
	if (m_nIndex >= 1 && m_nIndex <= USB_JOYSTICK_COUNT)
	{
		s_pInstance[m_nIndex - 1] = 0;
	}
}

boolean CUSBAtariJoystick::Initialize (void)
{
	if (m_pUSBHCI == 0 || m_nIndex < 1 || m_nIndex > USB_JOYSTICK_COUNT)
	{
		return FALSE;
	}

	/*
	 * The controller is looked up by the name the USB stack registers it under.
	 * It may not be there yet: a controller plugged in after boot appears later,
	 * and IsPresent() reports that honestly rather than this being a failure.
	 */
	m_pDevice = (CUSBGamePadDevice *) CDeviceNameService::Get ()
					 ->GetDevice ("upad", m_nIndex, FALSE);
	if (m_pDevice == 0)
	{
		return TRUE;				/* nothing attached yet */
	}

	m_pDevice->RegisterStatusHandler (StatusHandler);
	m_pDevice->RegisterRemovedHandler (RemovedHandler, this);

	const TGamePadState *pState = m_pDevice->GetInitialState ();
	if (pState != 0)
	{
		StatusHandler (m_nIndex, pState);
	}

	CLogger::Get ()->Write (FromJoystick, LogNotice,
				"%s attached: %d buttons, %d hats, %d axes",
				GetName (), pState != 0 ? pState->nbuttons : 0,
				pState != 0 ? pState->nhats : 0,
				pState != 0 ? pState->naxes : 0);

	return TRUE;
}

void CUSBAtariJoystick::StatusHandler (unsigned nDeviceIndex, const TGamePadState *pState)
{
	if (pState == 0 || nDeviceIndex < 1 || nDeviceIndex > USB_JOYSTICK_COUNT)
	{
		return;
	}

	CUSBAtariJoystick *pThis = s_pInstance[nDeviceIndex - 1];
	if (pThis == 0)
	{
		return;
	}

	pThis->m_nButtons = pState->buttons;

	pThis->m_nNaxes = pState->naxes > MAX_AXIS ? MAX_AXIS : pState->naxes;
	for (int i = 0; i < pThis->m_nNaxes; i++)
	{
		pThis->m_nAxis[i] = pState->axes[i].value;
		pThis->m_nAxisMin[i] = pState->axes[i].minimum;
		pThis->m_nAxisMax[i] = pState->axes[i].maximum;
	}

	int nHats = pState->nhats > MAX_HATS ? MAX_HATS : pState->nhats;
	for (int i = 0; i < nHats; i++)
	{
		pThis->m_nHats[i] = pState->hats[i];
	}

	/*
	 * The time is taken last and the sequence number after it, so a reader that
	 * checks the sequence number before and after copying knows it saw one
	 * whole report.
	 */
	pThis->m_nLastSeen = CTimer::Get ()->GetTicks ();
	pThis->m_bHaveState = TRUE;
	pThis->m_nSequence++;
}

void CUSBAtariJoystick::RemovedHandler (CDevice *pDevice, void *pContext)
{
	CUSBAtariJoystick *pThis = (CUSBAtariJoystick *) pContext;
	if (pThis == 0)
	{
		return;
	}

	if ((CDevice *) pThis->m_pDevice == pDevice)
	{
		pThis->m_pDevice = 0;
		pThis->m_bHaveState = FALSE;
		pThis->m_nButtons = 0;
		pThis->m_nSequence++;

		CLogger::Get ()->Write (FromJoystick, LogNotice, "%s removed", pThis->GetName ());
	}
}

/*
 * Reads one axis as a direction.
 *
 * \return -1 for negative, +1 for positive, 0 for "within the dead zone".
 */
static int AxisDirection (int nValue, int nMin, int nMax)
{
	if (nMax <= nMin)
	{
		return 0;
	}

	int nCentre = (nMin + nMax) / 2;
	int nSpan = nMax - nMin;
	int nDead = nSpan / USB_JOYSTICK_AXIS_DIVISOR;

	if (nValue > nCentre + nDead)
	{
		return 1;
	}
	if (nValue < nCentre - nDead)
	{
		return -1;
	}

	return 0;
}

unsigned CUSBAtariJoystick::GetDirections (void)
{
	if (!m_bHaveState || m_pDevice == 0)
	{
		return 0;
	}

	/*
	 * A controller that has gone quiet is treated as released, so an unplugged
	 * or crashed controller cannot leave a direction held down.
	 */
	if (CTimer::Get ()->GetTicks () - m_nLastSeen > USB_JOYSTICK_TIMEOUT_MS)
	{
		return 0;
	}

	/* Copy the report, checking that it did not change underneath us. */
	unsigned nSequence;
	unsigned nButtons;
	int nHats[MAX_HATS];
	int nNaxes;
	int nAxis[MAX_AXIS];
	int nAxisMin[MAX_AXIS];
	int nAxisMax[MAX_AXIS];

	do
	{
		nSequence = m_nSequence;
		nButtons = m_nButtons;
		nNaxes = m_nNaxes;
		for (int i = 0; i < nNaxes && i < MAX_AXIS; i++)
		{
			nAxis[i] = m_nAxis[i];
			nAxisMin[i] = m_nAxisMin[i];
			nAxisMax[i] = m_nAxisMax[i];
		}
		for (int i = 0; i < MAX_HATS; i++)
		{
			nHats[i] = m_nHats[i];
		}
	}
	while (nSequence != m_nSequence);

	unsigned nDirections = 0;

	/*
	 * The D-pad first, if there is one.  A controller with both a D-pad and a
	 * stick is played with the D-pad when a game wants four directions, so
	 * preferring it matches what the player is doing.
	 */
	boolean bUsedHat = FALSE;
	if (nHats[0] >= 0)
	{
		/*
		 * Circle reports a hat as an angle in degrees, 0 being up and
		 * increasing clockwise, or -1 when centred.  Dividing by 45 gives the
		 * eight compass positions.
		 */
		unsigned nAngle = (unsigned) nHats[0];
		switch (nAngle / 45)
		{
		case 0:					/* 0: up */
			nDirections |= ATARI_JOY_UP;
			bUsedHat = TRUE;
			break;
		case 1:					/* 45: up and right */
			nDirections |= ATARI_JOY_UP | ATARI_JOY_RIGHT;
			bUsedHat = TRUE;
			break;
		case 2:					/* 90: right */
			nDirections |= ATARI_JOY_RIGHT;
			bUsedHat = TRUE;
			break;
		case 3:					/* 135: down and right */
			nDirections |= ATARI_JOY_DOWN | ATARI_JOY_RIGHT;
			bUsedHat = TRUE;
			break;
		case 4:					/* 180: down */
			nDirections |= ATARI_JOY_DOWN;
			bUsedHat = TRUE;
			break;
		case 5:					/* 225: down and left */
			nDirections |= ATARI_JOY_DOWN | ATARI_JOY_LEFT;
			bUsedHat = TRUE;
			break;
		case 6:					/* 270: left */
			nDirections |= ATARI_JOY_LEFT;
			bUsedHat = TRUE;
			break;
		case 7:					/* 315: up and left */
			nDirections |= ATARI_JOY_UP | ATARI_JOY_LEFT;
			bUsedHat = TRUE;
			break;
		default:
			break;
		}
	}

	/*
	 * Otherwise the directional buttons, then the left stick.  Some controllers
	 * report their D-pad as four buttons rather than as a hat, and those are
	 * named by Circle.
	 */
	if (!bUsedHat)
	{
		if (nButtons & GamePadButtonUp)		nDirections |= ATARI_JOY_UP;
		if (nButtons & GamePadButtonDown)	nDirections |= ATARI_JOY_DOWN;
		if (nButtons & GamePadButtonLeft)	nDirections |= ATARI_JOY_LEFT;
		if (nButtons & GamePadButtonRight)	nDirections |= ATARI_JOY_RIGHT;
	}

	if (nNaxes > GamePadAxisLeftX && nNaxes > GamePadAxisLeftY)
	{
		int nX = AxisDirection (nAxis[GamePadAxisLeftX],
					nAxisMin[GamePadAxisLeftX], nAxisMax[GamePadAxisLeftX]);
		int nY = AxisDirection (nAxis[GamePadAxisLeftY],
					nAxisMin[GamePadAxisLeftY], nAxisMax[GamePadAxisLeftY]);

		if (nX < 0)	nDirections |= ATARI_JOY_LEFT;
		if (nX > 0)	nDirections |= ATARI_JOY_RIGHT;
		if (nY < 0)	nDirections |= ATARI_JOY_UP;
		if (nY > 0)	nDirections |= ATARI_JOY_DOWN;
	}

	return nDirections;
}

int CUSBAtariJoystick::GetTrigger (void)
{
	if (!m_bHaveState || m_pDevice == 0)
	{
		return ATARI_JOY_TRIG_OPEN;
	}

	if (CTimer::Get ()->GetTicks () - m_nLastSeen > USB_JOYSTICK_TIMEOUT_MS)
	{
		return ATARI_JOY_TRIG_OPEN;
	}

	/*
	 * Every button that would reasonably mean "fire" is accepted.  A player
	 * should not have to work out which one the emulator happens to want, and
	 * the Atari has only one button, so mapping several onto it costs nothing.
	 */
	static const unsigned FireButtons =
		GamePadButtonA | GamePadButtonB | GamePadButtonX | GamePadButtonY
		| GamePadButtonLB | GamePadButtonRB
		| GamePadButtonLT | GamePadButtonRT
		| GamePadButtonStart;

	return (m_nButtons & FireButtons) != 0 ? ATARI_JOY_TRIG_FIRED : ATARI_JOY_TRIG_OPEN;
}

boolean CUSBAtariJoystick::IsPresent (void)
{
	/*
	 * Looked up again rather than remembered, so that a controller plugged in
	 * after start-up is noticed.  The name service lookup is a short list walk
	 * and this is called once per frame.
	 */
	if (m_pDevice == 0 && m_pUSBHCI != 0)
	{
		m_pDevice = (CUSBGamePadDevice *) CDeviceNameService::Get ()
						 ->GetDevice ("upad", m_nIndex, FALSE);
		if (m_pDevice != 0)
		{
			m_pDevice->RegisterStatusHandler (StatusHandler);
			m_pDevice->RegisterRemovedHandler (RemovedHandler, this);
		}
	}

	return m_pDevice != 0 && m_bHaveState;
}

const char *CUSBAtariJoystick::GetName (void)
{
	static char Name[24];

	snprintf (Name, sizeof Name, "USB gamepad %u", m_nIndex);

	return Name;
}

/* ---------------------------------------------------------------------------
 * Factory
 * ------------------------------------------------------------------------- */

CAtariJoystick *ATARI_CreateUSBJoystick (CUSBHCIDevice *pUSBHCI, unsigned nIndex)
{
	if (pUSBHCI == 0 || nIndex < 1 || nIndex > USB_JOYSTICK_COUNT)
	{
		return 0;
	}

	static CUSBAtariJoystick *s_pJoystick[USB_JOYSTICK_COUNT] = { 0, 0, 0, 0 };

	if (s_pJoystick[nIndex - 1] == 0)
	{
		s_pJoystick[nIndex - 1] = new CUSBAtariJoystick (pUSBHCI, nIndex);
	}

	return s_pJoystick[nIndex - 1];
}
