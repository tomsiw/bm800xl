/*
 * joystick.cpp -- the base class that bridges the C++ drivers to the C
 * platform layer.
 *
 * The only interesting thing here is the function table.  A driver is a C++
 * object with virtual methods; the platform layer is C and can only call
 * function pointers.  Each entry in the table is a small trampoline that casts
 * the handle back to the object and calls the virtual.
 *
 * The cast is safe because atari_joystick_t is the first base of
 * CAtariJoystick, so the handle and the object have the same address.  That is
 * also why the C interface must stay the first base: reordering it would break
 * every controller at once, silently, by making these casts read the wrong
 * memory.
 */

#include "joystick.h"

CAtariJoystick::CAtariJoystick (void)
{
	/*
	 * The table is filled in here rather than by each subclass, so that a new
	 * driver cannot forget one of the entries and leave a null pointer for the
	 * emulation loop to call sixty times a second.
	 */
	initialize = TrampolineInitialize;
	get_directions = TrampolineDirections;
	get_trigger = TrampolineTrigger;
	is_present = TrampolinePresent;
	get_name = TrampolineName;
}

int CAtariJoystick::TrampolineInitialize (atari_joystick_t *self)
{
	return static_cast<CAtariJoystick *> (self)->Initialize () ? 1 : 0;
}

unsigned CAtariJoystick::TrampolineDirections (atari_joystick_t *self)
{
	return static_cast<CAtariJoystick *> (self)->GetDirections ();
}

int CAtariJoystick::TrampolineTrigger (atari_joystick_t *self)
{
	return static_cast<CAtariJoystick *> (self)->GetTrigger ();
}

int CAtariJoystick::TrampolinePresent (atari_joystick_t *self)
{
	return static_cast<CAtariJoystick *> (self)->IsPresent () ? 1 : 0;
}

const char *CAtariJoystick::TrampolineName (atari_joystick_t *self)
{
	return static_cast<CAtariJoystick *> (self)->GetName ();
}
