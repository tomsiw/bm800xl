/*
 * keyboard_joystick.cpp -- see keyboard_joystick.h.
 *
 * Two details are worth knowing about the reports this reads.
 *
 * First, a report lists the keys that are held *now*, all six slots of them, and
 * arrives whenever that set changes.  So the bits are rebuilt from scratch for
 * every report instead of being set and cleared: release needs no special case,
 * a direction held together with fire works because both are in the same report,
 * and an unplugged keyboard is released by the empty report that
 * CUSBKeyReports sends for a disappearing device.
 *
 * Second, only the Atari joystick's four directions and its one button exist, so
 * a key bound to a direction is not "pressed" in any keyboard sense -- it types
 * nothing, because the keyboard driver ignores this whole class of action.
 */

#include "keyboard_joystick.h"

#include <circle/logger.h>
#include <circle/usb/usbhid.h>

#include "usb_keymap.h"

static const char FromKeyboardJoystick[] = "kbjoy";

CKeyboardAtariJoystick::CKeyboardAtariJoystick (unsigned nIndex)
:	m_nIndex (nIndex),
	m_nPort (nIndex - 1),
	m_bListening (FALSE),
	m_Bits (0),
	m_nMappings (0)
{
}

boolean CKeyboardAtariJoystick::Initialize (void)
{
	if (m_nIndex < 1 || m_nIndex > ATARI_KEYBOARD_JOYSTICK_PORTS)
	{
		CLogger::Get ()->Write (FromKeyboardJoystick, LogWarning,
					"keyboard joystick %u: no such port", m_nIndex);

		return FALSE;
	}

	/*
	 * The listener is added once.  A keyboard that appears later is picked up by
	 * CUSBKeyReports, so there is nothing to wait for here -- which is why this
	 * succeeds even with nothing plugged in.
	 */
	if (!m_bListening)
	{
		m_bListening = CUSBKeyReports::Get ()->AddListener (
			ReportListener, this, GetName ());
	}

	m_nMappings = CUSBKeyMap::Get ().CountJoystickMappings ((int) m_nPort);

	/*
	 * Said out loud, because "the joystick does nothing" has two very different
	 * causes: no keys bound for this port, or keys bound and not being read.
	 */
	if (m_nMappings == 0)
	{
		CLogger::Get ()->Write (FromKeyboardJoystick, LogWarning,
					"%s: no keys are mapped to it in usbkeys.cfg",
					GetName ());
	}

	return m_bListening;
}

void CKeyboardAtariJoystick::Deactivate (void)
{
	if (!m_bListening)
	{
		return;
	}

	/*
	 * Off the list, so that a joystick which is not driving a port costs nothing
	 * and cannot be moved by a key press.  Both entries go: this class registers a
	 * report listener and a device listener, and the removal matches on context.
	 */
	CUSBKeyReports *pReports = CUSBKeyReports::Get ();

	pReports->RemoveListener (this);
	pReports->RemoveDeviceListener (this);

	m_bListening = FALSE;
	m_Bits = 0;
}

void CKeyboardAtariJoystick::ReportListener (void *pContext, unsigned nSlot,
					     unsigned char ucModifiers,
					     const unsigned char RawKeys[6])
{
	(void) nSlot;		/* every keyboard drives the same ports */

	CKeyboardAtariJoystick *pThis = (CKeyboardAtariJoystick *) pContext;
	if (pThis == 0)
	{
		return;
	}

	/*
	 * The modifiers are part of a stroke, so a file can bind U(Shift+NUM_8) to
	 * a direction and have it kept apart from U(NUM_8).
	 */
	unsigned nMods = 0;
	if ((ucModifiers & (LSHIFT | RSHIFT)) != 0)
	{
		nMods |= USB_KEYMAP_SHIFT;
	}
	if ((ucModifiers & (LCTRL | RCTRL)) != 0)
	{
		nMods |= USB_KEYMAP_CTRL;
	}

	unsigned nBits = 0;

	for (unsigned i = 0; i < 6; i++)
	{
		if (RawKeys[i] == 0)
		{
			continue;
		}

		TUsbKeyResult Result;
		if (!CUSBKeyMap::Get ().Resolve (RawKeys[i], nMods, &Result))
		{
			continue;
		}

		if (Result.Action != UsbKeyActionJoystick)
		{
			continue;
		}

		if ((unsigned) (Result.Code >> USB_KEYMAP_JOY_PORT_SHIFT) != pThis->m_nPort)
		{
			continue;
		}

		nBits |= (unsigned) Result.Code & USB_KEYMAP_JOY_MASK;
	}

	pThis->m_Bits = (unsigned char) nBits;
}

unsigned CKeyboardAtariJoystick::GetDirections (void)
{
	unsigned nBits = m_Bits;
	unsigned nDirections = 0;

	if (nBits & USB_KEYMAP_JOY_UP)
	{
		nDirections |= ATARI_JOY_UP;
	}
	if (nBits & USB_KEYMAP_JOY_DOWN)
	{
		nDirections |= ATARI_JOY_DOWN;
	}
	if (nBits & USB_KEYMAP_JOY_LEFT)
	{
		nDirections |= ATARI_JOY_LEFT;
	}
	if (nBits & USB_KEYMAP_JOY_RIGHT)
	{
		nDirections |= ATARI_JOY_RIGHT;
	}

	return nDirections;
}

int CKeyboardAtariJoystick::GetTrigger (void)
{
	return (m_Bits & USB_KEYMAP_JOY_FIRE) != 0
		? ATARI_JOY_TRIG_FIRED : ATARI_JOY_TRIG_OPEN;
}

boolean CKeyboardAtariJoystick::IsPresent (void)
{
	/*
	 * Present means "a keyboard is being read", not "a key is held": the port
	 * exists as soon as a keyboard does, and the emulated joystick simply reads
	 * as centred until a mapped key is pressed.
	 */
	return m_bListening && CUSBKeyReports::Get ()->GetDeviceCount () > 0;
}

const char *CKeyboardAtariJoystick::GetName (void)
{
	return m_nIndex == 1 ? "keyboard joystick 1" : "keyboard joystick 2";
}

/*
 * The factory.
 *
 * Static storage, like the other two drivers, so that the handle handed to the
 * platform layer outlives the call.  Two objects exist at most.
 */
CAtariJoystick *ATARI_CreateKeyboardJoystick (unsigned nIndex)
{
	static CKeyboardAtariJoystick *s_pJoystick[ATARI_KEYBOARD_JOYSTICK_PORTS] = { 0, 0 };

	if (nIndex < 1 || nIndex > ATARI_KEYBOARD_JOYSTICK_PORTS)
	{
		return 0;
	}

	if (s_pJoystick[nIndex - 1] == 0)
	{
		s_pJoystick[nIndex - 1] = new CKeyboardAtariJoystick (nIndex);
	}

	return s_pJoystick[nIndex - 1];
}
