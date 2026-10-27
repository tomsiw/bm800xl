/*
 * keyboard_joystick.h -- the Atari joystick driven by keys on a USB keyboard.
 *
 * This driver is a peer of the two others, not a customer of the Atari keyboard
 * driver: it listens to the USB key reports itself, through
 * CUSBKeyReports (which owns the keyboard device's single raw handler), and it
 * resolves strokes through usbkeys.cfg itself.  It never calls into
 * CUSBAtariKeyboard, and that driver is not aware that it exists -- a stroke
 * bound to `Joy1_Up` produces nothing at all there.
 *
 * Nothing is mapped by default.  A direction works only when usbkeys.cfg binds
 * some key to `Joy1_Up`, `Joy2_Fire` and so on, which is deliberate: a keyboard
 * that is also a joystick should not start steering because the interface was
 * selected.
 */

#ifndef ATARI_PI_KEYBOARD_JOYSTICK_H
#define ATARI_PI_KEYBOARD_JOYSTICK_H

#include <circle/types.h>

#include "joystick.h"
#include "usb_key_reports.h"

/* Ports that keys can drive.  Matches the two Atari ports. */
#define ATARI_KEYBOARD_JOYSTICK_PORTS	2

class CKeyboardAtariJoystick : public CAtariJoystick
{
public:
	/// \param nIndex 1 or 2 -- which Atari joystick the keys drive
	CKeyboardAtariJoystick (unsigned nIndex);
	~CKeyboardAtariJoystick (void) { Deactivate (); }

	/// \return FALSE if NIndex is out of range or the listener could not be added
	boolean Initialize (void);

	/*
	 * Stops listening.
	 *
	 * Only a keyboard joystick that is actually driving a port should be on the
	 * report list: the objects are created only when this interface is the
	 * selected one, and this is how the class lets go if it ever is not.
	 */
	void Deactivate (void);

	unsigned GetDirections (void);
	int GetTrigger (void);

	/* TRUE once the key reports are being read, i.e. a keyboard is present. */
	boolean IsPresent (void);

	const char *GetName (void);

private:
	/* Called from the USB interrupt for every report from every keyboard. */
	static void ReportListener (void *pContext, unsigned nSlot,
				    unsigned char ucModifiers,
				    const unsigned char RawKeys[6]);

	unsigned	m_nIndex;		/* 1-based port */
	unsigned	m_nPort;		/* m_nIndex - 1 */
	boolean		m_bListening;

	/*
	 * The directions and fire currently held, as USB_KEYMAP_JOY_* bits.
	 *
	 * Written from the USB interrupt and read from the frame loop, so volatile;
	 * it is one byte, which an ARM writes and reads whole, and a stale bit can
	 * only last until the next report.
	 */
	volatile unsigned char	m_Bits;

	/* For the log: how many strokes the file binds to this port. */
	unsigned	m_nMappings;
};

#endif
