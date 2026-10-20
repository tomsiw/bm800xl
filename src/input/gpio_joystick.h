/*
 * gpio_joystick.h
 *
 * A real Atari 9-pin joystick wired to the Raspberry Pi's GPIO header.
 *
 * ---------------------------------------------------------------------------
 * Why these pins
 * ---------------------------------------------------------------------------
 * The Atari 9-pin controller port is the simplest possible interface.  Inside
 * the joystick there are five switches -- up, down, left, right and fire -- and
 * each one, when closed, connects its pin on the port to the common ground on
 * pin 8.  There is no matrix, no multiplexing and no power: the port is six
 * wires, one per switch plus ground.
 *
 * That means the Pi only has to read five inputs per port, and a direction is
 * held when its input reads LOW, because the switch is pulling it to ground.
 *
 * The challenge is pins, not protocol.  The emulator's GPIO keyboard already
 * takes sixteen header pins, and there are not enough left for two ports'
 * worth of ten.  But the two are never used at the same time in the same way:
 * when the keyboard is a USB keyboard, **no** GPIO pin is being used for input
 * at all.  So these two ports deliberately **reuse the GPIO keyboard's pins**,
 * and the kernel refuses to start both at once.
 *
 * The assignment is not arbitrary: it follows the keyboard's own wiring, so that
 * a single ribbon can serve either a keyboard or a pair of joysticks, and the
 * pin numbers are documented in docs/joystick-gpio.md.
 *
 * ---------------------------------------------------------------------------
 * Wiring
 * ---------------------------------------------------------------------------
 * Each switch goes between its GPIO pin and a ground pin (6, 9, 14, 20, 25, 30,
 * 34 or 39).  No resistors are needed: the GPIO inputs use their internal
 * pull-ups, so an open switch reads HIGH and a closed one reads LOW.
 *
 * Read the documentation before wiring.  Connecting a 9-pin joystick's +5V pin
 * (pin 7 on the Atari port) to a GPIO pin would damage the Pi; the joystick's
 * pin 7 is left unconnected here, and only pins 1, 2, 3, 4, 6 and 8 are used.
 */

#ifndef ATARI_PI_GPIO_JOYSTICK_H
#define ATARI_PI_GPIO_JOYSTICK_H

#include <circle/gpiopin.h>
#include <circle/types.h>

#include "joystick.h"

/* Number of 9-pin ports the emulator can serve from GPIO. */
#define ATARI_GPIO_JOYSTICK_PORTS 2

/* Pins each port needs: the four directions and the fire button. */
#define ATARI_JOY_PINS_PER_PORT 5

class CGPIOAtariJoystick : public CAtariJoystick
{
public:
	/// \param nPort 1 or 2 -- selects the pin assignment
	CGPIOAtariJoystick (unsigned nPort);
	~CGPIOAtariJoystick (void);

	/// \return Configuration successful?
	/// \note Fails if NPort is out of range, or if any of the pins it needs is
	///	  one the kernel has already reserved (see ReservePin()).
	boolean Initialize (void);

	unsigned GetDirections (void);
	int GetTrigger (void);
	boolean IsPresent (void);
	const char *GetName (void);

	/*
	 * Reserved pins
	 *
	 * The kernel tells this driver which GPIO pins are already in use for
	 * something else, so that a port cannot silently drive a pin the keyboard
	 * is using.  Reserved pins are recorded before Initialize() is called; a
	 * port whose pins are reserved fails to initialise rather than fighting
	 * over them.
	 */
	static void ReservePin (unsigned nPin);
	static void ClearReservedPins (void);

private:
	unsigned m_nPort;

	/*
	 * The port's pins, held rather than created per read.
	 *
	 * A CGPIOPin reprograms the pin's function select and pull-up when it is
	 * constructed or when SetMode() is called, so a temporary object per read
	 * would reconfigure the hardware on every frame.  These are assigned once
	 * in Initialize() and only ever read afterwards.
	 */
	CGPIOPin m_Pin[ATARI_JOY_PINS_PER_PORT];

	boolean m_bInitialised;
};

#endif
