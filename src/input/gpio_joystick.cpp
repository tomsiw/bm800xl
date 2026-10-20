/*
 * gpio_joystick.cpp -- a real Atari 9-pin joystick on the GPIO header.
 *
 * See gpio_joystick.h for why these pins are used and how the port is wired.
 *
 * Two things shape the implementation:
 *
 *   1. A CGPIOPin must be configured once and then only read.  Its constructor
 *      and SetMode() reprogram the pin's function select and pull-up, so they
 *      are not something to do per frame.  The five pins are therefore held as
 *      members and configured in Initialize().
 *
 *   2. There is no debouncing.  The emulation core reads a controller port once
 *      per frame and does its own edge detection on top, and a joystick is read
 *      by a person's hand rather than by a fast scan.  Debouncing here would
 *      only make the controller feel unresponsive.
 */

#include "gpio_joystick.h"

#include <circle/logger.h>

static const char FromJoystick[] = "gpiojoy";

/* ---------------------------------------------------------------------------
 * Pin assignment
 *
 * These are the GPIO keyboard's pins, deliberately reused.  The kernel refuses
 * to run a GPIO keyboard and these ports together, so a pin can only ever be
 * claimed by one of them.
 *
 * The Atari 9-pin connector is: pin 1 up, 2 down, 3 left, 4 right, 6 fire,
 * 8 ground.  Pin 5 (the paddle potentiometer) and pin 7 (+5V) are not
 * connected -- pin 7 especially must not be, because it carries five volts and
 * a GPIO pin will not tolerate that.
 * ------------------------------------------------------------------------- */

static const unsigned s_PortPins[ATARI_GPIO_JOYSTICK_PORTS][ATARI_JOY_PINS_PER_PORT] =
{
	/* Up, Down, Left, Right, Fire -- port 1 */
	{ 4, 5, 6, 8, 9 },
	/* Up, Down, Left, Right, Fire -- port 2 */
	{ 10, 11, 12, 13, 16 }
};

/* Names for the positions in a port's pin list, so the code below reads. */
#define PIN_UP		0
#define PIN_DOWN	1
#define PIN_LEFT	2
#define PIN_RIGHT	3
#define PIN_FIRE	4

/*
 * Pins the kernel has already claimed.  A static table rather than an instance
 * member because reservation happens before any port object exists, and because
 * it describes the machine rather than one controller.
 */
static boolean s_bPinReserved[GPIO_PINS];

void CGPIOAtariJoystick::ReservePin (unsigned nPin)
{
	if (nPin < GPIO_PINS)
	{
		s_bPinReserved[nPin] = TRUE;
	}
}

void CGPIOAtariJoystick::ClearReservedPins (void)
{
	for (unsigned nPin = 0; nPin < GPIO_PINS; nPin++)
	{
		s_bPinReserved[nPin] = FALSE;
	}
}

CGPIOAtariJoystick::CGPIOAtariJoystick (unsigned nPort)
:	m_nPort (nPort),
	m_bInitialised (FALSE)
{
	/* The pins are assigned and configured in Initialize(). */
}

CGPIOAtariJoystick::~CGPIOAtariJoystick (void)
{
	/*
	 * The pins are left as they are.  Releasing them would only matter if
	 * something else were about to claim them, and nothing is: this object
	 * lives until the machine is switched off.
	 */
}

boolean CGPIOAtariJoystick::Initialize (void)
{
	if (m_nPort < 1 || m_nPort > ATARI_GPIO_JOYSTICK_PORTS)
	{
		CLogger::Get ()->Write (FromJoystick, LogError,
					"Port %u is out of range (1-%u)",
					m_nPort, ATARI_GPIO_JOYSTICK_PORTS);
		return FALSE;
	}

	unsigned nPortIndex = m_nPort - 1;

	/*
	 * Refuse to take a pin that is already spoken for.  Without this check a
	 * GPIO keyboard and a joystick port would both configure the same pin --
	 * one as an output, the other as an input -- and the result would be a
	 * keyboard reading nonsense and a joystick that does not work, with
	 * nothing in the log to explain why.
	 */
	for (unsigned i = 0; i < ATARI_JOY_PINS_PER_PORT; i++)
	{
		unsigned nPin = s_PortPins[nPortIndex][i];

		if (s_bPinReserved[nPin])
		{
			CLogger::Get ()->Write (FromJoystick, LogError,
						"Port %u needs GPIO %u, which is already in use",
						m_nPort, nPin);
			return FALSE;
		}
	}

	/*
	 * A joystick switch connects its pin to ground, so an open switch has to
	 * read HIGH.  The internal pull-up provides that, and it is what makes
	 * external resistors unnecessary.
	 *
	 * On the Pi 5 the GPIO block lives behind RP1 and Circle sets the pull-up
	 * through RP1's pad registers, so the same call is correct on every model.
	 * RP1 has to be up by now, which it is: the kernel checks that before
	 * reaching here, because Circle asserts inside the GPIO driver otherwise.
	 */
	for (unsigned i = 0; i < ATARI_JOY_PINS_PER_PORT; i++)
	{
		m_Pin[i].AssignPin (s_PortPins[nPortIndex][i]);
		m_Pin[i].SetMode (GPIOModeInputPullUp);
	}

	m_bInitialised = TRUE;

	CLogger::Get ()->Write (FromJoystick, LogNotice,
				"%s ready on GPIO %u, %u, %u, %u and %u",
				GetName (),
				s_PortPins[nPortIndex][PIN_UP],
				s_PortPins[nPortIndex][PIN_DOWN],
				s_PortPins[nPortIndex][PIN_LEFT],
				s_PortPins[nPortIndex][PIN_RIGHT],
				s_PortPins[nPortIndex][PIN_FIRE]);

	return TRUE;
}

unsigned CGPIOAtariJoystick::GetDirections (void)
{
	unsigned nDirections = 0;

	if (!m_bInitialised)
	{
		return 0;
	}

	/*
	 * The port is active low: a switch that is closed reads LOW.  ATARI_JOY_UP
	 * and friends are the bits a port reading CLEARS when the direction is
	 * held, so a closed switch sets its bit.
	 */
	if (m_Pin[PIN_UP].Read () == LOW)
	{
		nDirections |= ATARI_JOY_UP;
	}
	if (m_Pin[PIN_DOWN].Read () == LOW)
	{
		nDirections |= ATARI_JOY_DOWN;
	}
	if (m_Pin[PIN_LEFT].Read () == LOW)
	{
		nDirections |= ATARI_JOY_LEFT;
	}
	if (m_Pin[PIN_RIGHT].Read () == LOW)
	{
		nDirections |= ATARI_JOY_RIGHT;
	}

	return nDirections;
}

int CGPIOAtariJoystick::GetTrigger (void)
{
	if (!m_bInitialised)
	{
		return ATARI_JOY_TRIG_OPEN;
	}

	return m_Pin[PIN_FIRE].Read () == LOW ? ATARI_JOY_TRIG_FIRED : ATARI_JOY_TRIG_OPEN;
}

boolean CGPIOAtariJoystick::IsPresent (void)
{
	/*
	 * A GPIO joystick cannot announce itself: it is a set of switches with no
	 * electronics at all.  "Present" can therefore only mean "configured and
	 * ready to be read".  A port with nothing plugged in reads as centred with
	 * the button open, which is exactly what an Atari with no joystick does,
	 * so an empty port behaves correctly rather than needing to be detected.
	 */
	return m_bInitialised;
}

const char *CGPIOAtariJoystick::GetName (void)
{
	return m_nPort == 1 ? "GPIO joystick port 1" : "GPIO joystick port 2";
}

/* ---------------------------------------------------------------------------
 * Factory
 * ------------------------------------------------------------------------- */

CAtariJoystick *ATARI_CreateGPIOJoystick (unsigned nPort)
{
	if (nPort < 1 || nPort > ATARI_GPIO_JOYSTICK_PORTS)
	{
		return 0;
	}

	/*
	 * One object per port, created on first use and kept forever.  These are
	 * allocated with new rather than as function-local statics because
	 * CGPIOPin has a non-trivial constructor, and the object has to outlive
	 * every caller.  A failure here is reported as a null return, which the
	 * kernel treats as "no joystick" rather than an error: a missing
	 * controller must never stop the emulator running.
	 */
	static CGPIOAtariJoystick *s_pPort[ATARI_GPIO_JOYSTICK_PORTS] = { 0, 0 };

	unsigned nIndex = nPort - 1;

	if (s_pPort[nIndex] == 0)
	{
		s_pPort[nIndex] = new CGPIOAtariJoystick (nPort);
	}

	return s_pPort[nIndex];
}
