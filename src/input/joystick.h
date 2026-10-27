/*
 * joystick.h -- the C++ side of the controller interface.
 *
 * The emulator supports two kinds of Atari controller:
 *
 *   USB gamepads       a modern USB controller, with its buttons and stick
 *                      mapped onto the Atari's joystick;
 *   GPIO joysticks     a real 9-pin Atari joystick wired straight to the
 *                      Raspberry Pi's GPIO header, which is what the machine
 *                      actually had.
 *
 * Both are read as "which direction, and is the button pressed", so the rest of
 * the emulator does not care which is in use.
 *
 * CAtariJoystick derives from atari_joystick_t and fills in its function table,
 * so a controller can be handed to the C platform layer as a plain handle.  See
 * joystick_types.h for why the interface is shaped that way and for the note on
 * polarity.
 */

#ifndef ATARI_PI_JOYSTICK_H
#define ATARI_PI_JOYSTICK_H

#include <circle/types.h>

#include "joystick_types.h"

/*
 * A controller source.
 *
 * The base class supplies the C function table, so a subclass only implements
 * the five operations below.
 */
class CAtariJoystick : public atari_joystick_t
{
public:
	CAtariJoystick (void);
	virtual ~CAtariJoystick (void) { }

	/* Brings the controller up.  FALSE leaves it unusable but harmlessly so. */
	virtual boolean Initialize (void) = 0;

	/* \return Bit mask of the directions currently held (ATARI_JOY_*). */
	virtual unsigned GetDirections (void) = 0;

	/* \return ATARI_JOY_TRIG_FIRED while the fire button is pressed. */
	virtual int GetTrigger (void) = 0;

	/* \return Is this controller connected? */
	virtual boolean IsPresent (void) = 0;

	/* \return A short name for the log, e.g. "USB gamepad 1". */
	virtual const char *GetName (void) = 0;

	/*
	 * The handle to hand to ATARI_SetJoystick().
	 *
	 * Because atari_joystick_t is the first base, this is the same address as
	 * the object itself; the accessor exists so that callers never have to rely
	 * on that.
	 */
	atari_joystick_t *Handle (void) { return this; }

private:
	/* The C function table, filled with trampolines onto the virtuals. */
	static int TrampolineInitialize (atari_joystick_t *self);
	static unsigned TrampolineDirections (atari_joystick_t *self);
	static int TrampolineTrigger (atari_joystick_t *self);
	static int TrampolinePresent (atari_joystick_t *self);
	static const char *TrampolineName (atari_joystick_t *self);
};

/*
 * Factories.
 *
 * Each returns NULL if the object could not be created at all; whether a
 * controller is attached is reported by IsPresent().  The returned pointer is
 * owned by the factory and must not be deleted by the caller.
 */

class CUSBHCIDevice;

/*
 * \param nIndex 1-based controller number; USB gamepads are matched to indices
 *		 by the order the USB stack reports them in.
 */
CAtariJoystick *ATARI_CreateUSBJoystick (CUSBHCIDevice *pUSBHCI, unsigned nIndex);

/*
 * \param nPort 1 or 2, selecting one of the two 9-pin port pin assignments.
 */
CAtariJoystick *ATARI_CreateGPIOJoystick (unsigned nPort);

/*
 * A joystick whose directions and fire button are keys on the USB keyboard,
 * bound in usbkeys.cfg as Joy1_Up, Joy2_Fire and so on.
 *
 * \param nIndex 1 or 2 -- which Atari joystick the keys drive.
 * \note Independent of the keyboard driver: it reads the same key reports
 *	 through CUSBKeyReports, so the keyboard keeps working, and a key bound to
 *	 a joystick action types nothing.
 */
CAtariJoystick *ATARI_CreateKeyboardJoystick (unsigned nIndex);

#endif

