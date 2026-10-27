/*
 * joystick_types.h -- the controller interface, expressed so that both C and
 * C++ can use it.
 *
 * The platform layer that answers the emulation core is C, compiled as part of
 * the core; the controller drivers are C++ and need Circle.  The two have to
 * meet somewhere, and this is that place.
 *
 * The meeting point is a small table of C function pointers.  A driver embeds an
 * atari_joystick_t as the first member of its object and fills in the table, so
 * the same address serves as both "the driver's object" and "the C handle".  The
 * platform layer only ever holds the handle and calls through the table, so it
 * needs to know nothing about the driver.
 *
 * The alternative -- declaring the C++ class in the C header -- does not work,
 * and neither does letting the C compiler see a class.  A function-pointer table
 * is the plainest thing both languages agree on exactly.
 *
 * ---------------------------------------------------------------------------
 * Polarity
 * ---------------------------------------------------------------------------
 * The Atari reads its controller port as **active low**.  A direction bit that
 * is 0 means that direction is held, and a trigger of 0 means the button is
 * pressed, because on the real machine each joystick switch connects its line
 * to ground.  The emulation core expects this, so these values follow it.
 */

#ifndef ATARI_PI_JOYSTICK_TYPES_H
#define ATARI_PI_JOYSTICK_TYPES_H

/* Held directions, as the bits of a controller port. */
#define ATARI_JOY_UP		(1 << 0)
#define ATARI_JOY_DOWN		(1 << 1)
#define ATARI_JOY_LEFT		(1 << 2)
#define ATARI_JOY_RIGHT		(1 << 3)

/* Nothing held, and the button released: the idle state of a port. */
#define ATARI_JOY_CENTRE	0x0f
#define ATARI_JOY_TRIG_OPEN	1
#define ATARI_JOY_TRIG_FIRED	0

/*
 * How many controller inputs this project fills.
 *
 * An Atari 800 had four ports, and each of its two pairs carried a second
 * controller's lines alongside the first, so the core asks for four.  An XL/XE
 * has two physical ports, and this project drives those two -- STICK(0) and
 * STICK(1).  The remaining two inputs are left unbound, and the platform layer
 * answers them as centred, so a game that reads STICK(2) sees a released
 * joystick rather than a fault.
 */
#define ATARI_JOYSTICK_COUNT	2

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A controller, as the platform layer sees it.
 *
 * Every function must be safe to call once per frame, must not block, and must
 * not allocate: the emulation loop calls them on the critical path.
 */
typedef struct atari_joystick {
	/*
	 * Brings the controller up.  Returns 0 on failure, which leaves it
	 * unusable but harmlessly so.  May be called more than once.
	 */
	int (*initialize)(struct atari_joystick *self);

	/* Directions currently held, as a mask of the ATARI_JOY_* bits. */
	unsigned (*get_directions)(struct atari_joystick *self);

	/* ATARI_JOY_TRIG_FIRED while the button is pressed. */
	int (*get_trigger)(struct atari_joystick *self);

	/* Whether the controller is usable at all, and connected. */
	int (*is_present)(struct atari_joystick *self);

	/* A short name for the log, e.g. "USB gamepad 1". */
	const char *(*get_name)(struct atari_joystick *self);
} atari_joystick_t;

#ifdef __cplusplus
}
#endif

#endif
