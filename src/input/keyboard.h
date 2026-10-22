/*
 * keyboard.h -- the interface every keyboard source implements.
 *
 * The emulator supports two ways of attaching an Atari XL/XE keyboard:
 *
 *   Mode 1  a USB keyboard plugged into any Raspberry Pi USB port, with the
 *           Atari's console keys and cursor keys mapped onto it;
 *   Mode 2  a real Atari keyboard wired to the GPIO header as a scan matrix.
 *
 * Both produce the same two things:
 *
 *   * the state of the Atari keyboard for the current frame, which goes
 *     straight to the emulation core, and
 *   * the key presses the OSD menu consumes, which must not reach the emulated
 *     machine.
 *
 * Keeping both in one interface means the rest of the emulator never has to
 * know which keyboard is in use.
 *
 * Neither driver does any translation beyond identifying which Atari key is
 * down: deciding what Shift and Control do to a key is the emulation core's
 * job.
 */

#ifndef ATARI_PI_KEYBOARD_H
#define ATARI_PI_KEYBOARD_H

#include <circle/types.h>

#include "atari_port.h"

/*
 * A keyboard source.
 *
 * Implementations are polled once per emulated frame.  They must be safe to
 * call from the main thread, must not block, and must not allocate.
 */
class CAtariKeyboard
{
public:
	virtual ~CAtariKeyboard (void) { }

	/*
	 * Brings the keyboard up.
	 *
	 * For a USB keyboard this creates the device and waits, briefly, for one
	 * to be attached.  For a GPIO keyboard it configures the pins.  Returning
	 * FALSE is not fatal: the emulator still runs, it simply cannot be typed
	 * into, and the user is told so.
	 */
	virtual boolean Initialize (void) = 0;

	/*
	 * Fills STATE with the keys currently held down.
	 *
	 * STATE is zeroed by the caller before the call, so an implementation only
	 * has to set what is pressed.  In particular `key_code` starts at
	 * ATARI_KEY_NONE, and the console key fields start at "not pressed".
	 */
	virtual void GetState (atari_keyboard_state_t *pState) = 0;

	/*
	 * Reports whether the user has asked for the OSD menu.
	 *
	 * Returns TRUE when the menu was requested and marks the request as
	 * consumed, so a single key press opens the menu exactly once.  This is
	 * edge triggered on purpose: the menu is toggled by a press, not by a key
	 * being held down.
	 *
	 * The keys that do this are F12 on a USB keyboard, and Shift+Help on an
	 * Atari keyboard.
	 */
	virtual boolean ConsumeMenuRequest (void) = 0;

	/*
	 * Takes the next key press that the OSD menu should handle while it is
	 * open, or returns ATARI_KEY_NONE when there is nothing new.
	 *
	 * Only presses that the menu uses are reported here -- the cursor keys,
	 * Return, Escape and Space.  Everything else is left for the emulated
	 * machine, so a key pressed while the menu is closed still reaches the
	 * Atari.
	 *
	 * Each press is reported exactly once, and while the menu is open these
	 * keys should not also be reported through GetState(), so that arrow keys
	 * move the menu selection instead of the emulated cursor.
	 */
	virtual int ConsumeMenuKey (void) = 0;

	/*
	 * Polls for a keyboard that has appeared since the last call.
	 *
	 * Called once per frame from the kernel.  A source whose keyboards cannot
	 * be attached later has nothing to do.  This is not where USB enumeration
	 * is driven -- the kernel owns that -- it only notices what enumeration
	 * has produced.
	 */
	virtual void Update (void) { }

	/* \return Is this keyboard attached and able to produce input? */
	virtual boolean IsPresent (void) = 0;

	/*
	 * Settings applied from the OSD menu.
	 */

	/*
	 * Hands the driver its remapping file.
	 *
	 * P_PATH is where the file came from, for the log; P_TEXT is its
	 * contents, or 0 when there is no file and the built-in mapping applies.
	 * Called once, before Initialize(), so that nothing has to be locked
	 * afterwards: the map is read-only while reports are arriving.
	 */
	virtual void SetKeyMap (const char *pPath, const char *pText)
	{
		(void) pPath;
		(void) pText;
	}

	/* Makes the numeric keypad's arrows act as cursor keys. */
	virtual void SetNumpadAsCursor (boolean bEnabled)	{ (void) bEnabled; }

	/*
	 * Enables or disables reporting keys to the menu.
	 *
	 * The kernel turns this on while the menu is open, so that the cursor
	 * keys drive the menu rather than the emulated machine.  Presses are
	 * accumulated either way; this only decides whether
	 * ConsumeMenuKey() hands them out.
	 */
	virtual void SetMenuActive (boolean bActive)		{ (void) bActive; }
};

/*
 * Factories for the two keyboards.
 *
 * Each returns NULL if the object itself could not be created; whether the
 * keyboard is actually usable is reported by Initialize().  They have C linkage
 * so that the symbol names do not depend on which translation unit defines
 * them.
 *
 * This header is C++ only, so no __cplusplus guard is needed here, unlike
 * src/port/audio_out.h which the emulation layer also includes from C.
 */
class CUSBHCIDevice;

extern "C" {
CAtariKeyboard *ATARI_CreateUSBKeyboard (CUSBHCIDevice *pUSBHCI);
CAtariKeyboard *ATARI_CreateGPIOKeyboard (void);
}

#endif
