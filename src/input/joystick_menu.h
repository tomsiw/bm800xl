/*
 * joystick_menu.h -- joystick and gamepad input for the OSD.
 *
 * The emulated machine and the OSD need the same two controllers for different
 * purposes.  While the OSD is open the controllers navigate the menu; while it
 * is closed they belong to the emulated machine.  This class is the OSD side:
 * it polls the controller handles the kernel already owns and turns direction
 * and fire edges into the same TMenuKeyEvent the keyboard produces.
 *
 * It deliberately knows nothing about USB, GPIO or CAtariJoystick.  The handles
 * are the C function table from joystick_types.h, so the same code works for a
 * USB gamepad and a 9-pin joystick.
 */

#ifndef ATARI_PI_JOYSTICK_MENU_H
#define ATARI_PI_JOYSTICK_MENU_H

#include <circle/types.h>

#include "joystick_types.h"
#include "keyboard.h"

class CAtariMenuJoystick
{
public:
	CAtariMenuJoystick (void);

	/*
	 * Sets the two controller handles.  bEnabled is FALSE for the keyboard
	 * joystick interface: the keyboard already produces menu keys, and feeding
	 * the same strokes in twice would double every press.
	 */
	void SetHandles (atari_joystick_t *pPort1, atari_joystick_t *pPort2,
			 boolean bEnabled);

	/*
	 * Called once when the OSD opens or closes.  Opening clears the queue and
	 * arms both controllers only after they have been neutral, so a control
	 * held while opening cannot select something by itself.
	 */
	void SetActive (boolean bActive);

	/* Polls both controllers once per frame.  Call only while active. */
	void Poll (void);

	/* Pops one queued menu event.  FALSE when the queue is empty. */
	boolean ConsumeEvent (TMenuKeyEvent *pEvent);

private:
	enum
	{
		PortCount = ATARI_JOYSTICK_COUNT,
		QueueSize = 8
	};

	struct TPortState
	{
		boolean  bArmed;
		unsigned nDirection;
		boolean  bFire;
	};

	void Reset (void);
	unsigned PickDirection (unsigned nRaw, unsigned nCurrent) const;
	void QueueKey (int nKey, boolean bPressed);
	void ReleaseOwner (void);

private:
	atari_joystick_t *m_pHandle[PortCount];
	boolean		 m_bEnabled;
	boolean		 m_bActive;

	TPortState	 m_Port[PortCount];
	int		 m_nOwner;	/* -1 until one controller is chosen */

	TMenuKeyEvent	 m_Queue[QueueSize];
	unsigned	 m_nHead;
	unsigned	 m_nTail;
};

#endif
