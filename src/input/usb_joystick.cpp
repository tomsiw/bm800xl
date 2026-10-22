/*
 * usb_joystick.cpp -- a USB gamepad used as an Atari joystick.
 *
 * A modern controller has far more than an Atari joystick does, and its controls
 * are not in standard places.  Circle understands the common controller families
 * and normalises them into TGamePadState, but a pad it does not recognise -- the
 * cheap "USB joystick" adapters, for instance -- reports its buttons and axes in
 * whatever order its HID descriptor happens to use, so a button bit means a
 * position in that descriptor rather than a named button.
 *
 * So the mapping from pad to Atari is **resolved once per controller** into a
 * TMapping: for each of the four directions and for fire, a description of where
 * it comes from -- a button bit, an axis with a threshold and a side, or a hat
 * position.  Resolution is keyed by the USB vendor/product ID, which is the only
 * thing that tells a pad Circle has a driver for from one it has not, and by what
 * the pad reports it has, because a pad with no hat cannot use the hat rule.
 *
 * The resolver is the one proven in probe/gamepad against three controllers;
 * docs/progress.md section 8 records the measurements behind each rule.
 *
 * Two consequences worth stating, because they look like omissions:
 *
 *   - the resolver is consulted, never re-derived, while a controller stays
 *     attached: the mapping cannot change under a held button;
 *   - the interrupt handler only copies a report.  Deciding what the report means
 *     happens in GetDirections(), in the main loop, which is what keeps the log
 *     and FatFs out of the USB interrupt.  Logging a report from there once
 *     produced an eMMC transfer error (docs/progress.md section 5).
 */

#include "joystick.h"

#include <circle/logger.h>
#include <circle/string.h>
#include <circle/usb/usbdevice.h>
#include <circle/usb/usbgamepad.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/devicenameservice.h>
#include <circle/synchronize.h>
#include <circle/timer.h>

#include <stdio.h>
#include <string.h>

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
 * An analog axis is read as a direction when it is beyond this fraction of its
 * range from the centre.
 *
 * A quarter is deliberately generous.  An analog stick at rest is never exactly
 * centred, and a controller's range is rarely symmetric; a tighter threshold
 * would make the joystick drift on its own, which is much worse than needing a
 * deliberate push.
 *
 * Only the left stick of a pad Circle knows about is read this way.  The
 * direction axes of a generic pad are read against measured absolute thresholds
 * instead, which is what the resolver describes.
 */
#define USB_JOYSTICK_AXIS_DIVISOR 4

/*
 * The shortest interval between two action lines in the log.
 *
 * Every line of the runtime log is written and flushed to the card, so an
 * unthrottled log would turn a fast-moving stick into a stream of card writes
 * in the middle of the frame loop -- the very cost being measured.  A tenth of
 * a second is slow enough to be cheap and fast enough to follow a player's
 * hands.
 */
#define USB_JOYSTICK_LOG_INTERVAL_US 100000

/* ---------------------------------------------------------------------------
 * The mapping
 *
 * A bare mask table cannot describe the generic pads: on ven79-11 (measured) the
 * vertical D-pad arrives on an axis, not on a button bit, so "up" has to be
 * looked for in a different place.  Describing where each control comes from is
 * therefore the only shape that works for both kinds.
 * ------------------------------------------------------------------------- */

enum TDirectionSource
{
	SourceNone,
	SourceButton,
	SourceAxis,
	SourceHat
};

struct TDirection
{
	TDirectionSource	Source;
	unsigned		nBit;		/* SourceButton */
	int			nAxis;		/* SourceAxis */
	int			nThreshold;	/* SourceAxis: the value at the active end */
	boolean			bHighActive;	/* SourceAxis: TRUE = active above it */
	int			nHatMask;	/* SourceHat: raw hat values that count */
};

struct TMapping
{
	const char	*pName;		/* what the log calls this mapping */
	TDirection	Up, Down, Left, Right;
	TDirection	Fire;		/* the pad's first action button */
	TDirection	Fire2;		/* and a second one, if it reports one */
	unsigned	nMoreFire;	/* further button bits that mean fire */
	boolean		bLeftStick;	/* read the left stick as well */
};

static TDirection DirBtn (unsigned nBit)
{
	TDirection d;
	d.Source = SourceButton; d.nBit = nBit;
	d.nAxis = -1; d.nThreshold = 0; d.bHighActive = FALSE; d.nHatMask = 0;
	return d;
}

static TDirection DirAx (int nAxis, int nThreshold, boolean bHighActive)
{
	TDirection d;
	d.Source = SourceAxis; d.nBit = 0;
	d.nAxis = nAxis; d.nThreshold = nThreshold; d.bHighActive = bHighActive;
	d.nHatMask = 0;
	return d;
}

static TDirection DirHat (unsigned nMask)
{
	TDirection d;
	d.Source = SourceHat; d.nBit = 0;
	d.nAxis = -1; d.nThreshold = 0; d.bHighActive = FALSE; d.nHatMask = nMask;
	return d;
}

static TDirection DirNothing (void)
{
	TDirection d;
	d.Source = SourceNone; d.nBit = 0;
	d.nAxis = -1; d.nThreshold = 0; d.bHighActive = FALSE; d.nHatMask = 0;
	return d;
}

/* A mapping in which nothing is held, for "no controller" and for start-up. */
static TMapping EmptyMapping (void)
{
	TMapping m;
	m.pName = "none";
	m.Up = m.Down = m.Left = m.Right = DirNothing ();
	m.Fire = m.Fire2 = DirNothing ();
	m.nMoreFire = 0;
	m.bLeftStick = FALSE;
	return m;
}

/*
 * Whether one mapped control is held, according to a report.
 *
 * Used both for the four directions and for fire, so there is exactly one place
 * where "where does this control live" becomes "is it pressed".
 */
static boolean IsActive (const TDirection &Dir, const TGamePadState *pState)
{
	switch (Dir.Source)
	{
	case SourceButton:
		return (pState->buttons & Dir.nBit) != 0;

	case SourceAxis:
		if (Dir.nAxis < 0 || Dir.nAxis >= pState->naxes)
		{
			return FALSE;
		}
		{
			int v = pState->axes[Dir.nAxis].value;
			return Dir.bHighActive ? v >= Dir.nThreshold : v <= Dir.nThreshold;
		}

	case SourceHat:
		if (pState->nhats < 1)
		{
			return FALSE;
		}
		{
			int nHat = pState->hats[0];

			/*
			 * A hat value is the raw HID one: 0..7 clockwise from up, and
			 * out of that range when the hat is centred (a standard pad
			 * reports its logical maximum, a DualShock 4 reports 8).  A
			 * mask, rather than a single value, is what lets a diagonal
			 * count for both of the directions it lies between.
			 */
			return nHat >= 0 && nHat < 8
			    && (Dir.nHatMask & (1u << nHat)) != 0;
		}

	default:
		return FALSE;
	}
}

/*
 * The pads Circle has a driver for.
 *
 * These are the gamepad entries of lib/usb/usbdevicefactory.cpp, and the point of
 * listing them is that each of those drivers fills TGamePadState with the button
 * bits of Circle's own TGamePadButton enum -- so a bit means a named button
 * rather than a position in a HID descriptor.  The 8BitDo entries qualify too:
 * their driver maps raw buttons through a table onto the same enum.
 *
 * Only the pads in Circle's gamepad section are listed: 8BitDo's idle
 * receivers, for instance, are not gamepads at all and never reach this code.  A
 * pad that is not listed falls through to the measured rules below.
 */
static boolean IsKnownPad (const char *pId)
{
	static const char *Ids[] =
	{
		"ven57e-2009",					/* Switch Pro */

		"ven54c-268",					/* PS3 */
		"ven54c-5c4", "ven54c-9cc",			/* PS4 */

		"ven45e-28e", "ven45e-28f",			/* Xbox 360 */
		"ven45e-719", "ven45e-2a9",			/* Xbox 360 wireless receiver */
		"ven45e-2d1", "ven45e-2dd",			/* Xbox One / One (2015 fw) */
		"ven45e-2e3", "ven45e-2ea",			/* Xbox One Elite / One S */
		"ven45e-b12",					/* Xbox Series X */

		"ven2dc8-310b",					/* 8BitDo Pro 3, Ultimate 2.4G */
		"ven2dc8-3106",					/* 8BitDo Ultimate C, adapter 2 */
	};

	for (unsigned i = 0; i < sizeof Ids / sizeof Ids[0]; i++)
	{
		if (strcmp (pId, Ids[i]) == 0)
		{
			return TRUE;
		}
	}

	return FALSE;
}

/*
 * Works out where this controller's controls are.
 *
 * Order of preference, each step backed by something observed:
 *
 *   1. a known vendor/product ID -- Circle's specific driver then fills
 *      TGamePadButton exactly, and ven57e-2009 (Switch Pro) confirmed every
 *      value used below;
 *   2. a hat -- directions come from hats[0], which is the raw HID value
 *      0..7 clockwise from up;
 *   3. five or more axes -- directions come from axes, against measured
 *      thresholds;
 *   4. anything else -- the bits Circle's own drivers use, so the pad still
 *      partly works.
 *
 * The Microntek entry has to precede the hat rule: that pad reports a hat it
 * never uses for direction.
 */
static TMapping ResolveMapping (const char *pId, const TGamePadState *pState)
{
	TMapping m = EmptyMapping ();

	const char *p = pId != 0 ? pId : "";

	/* 1. Pads Circle has a driver for: the enum values are authoritative. */
	if (IsKnownPad (p))
	{
		m.pName = "buttons (Circle driver)";
		m.Up    = DirBtn (GamePadButtonUp);
		m.Down  = DirBtn (GamePadButtonDown);
		m.Left  = DirBtn (GamePadButtonLeft);
		m.Right = DirBtn (GamePadButtonRight);
		m.Fire  = DirBtn (GamePadButtonA);
		m.Fire2 = DirBtn (GamePadButtonB);

		/*
		 * Because the bit values are meaningful for these pads, every button
		 * that could reasonably mean "fire" is accepted: there is only one
		 * button on an Atari and a player should not have to discover which
		 * of them the emulator wants.  The direction bits are excluded --
		 * they would make the joystick fire when it is steered -- as are
		 * SELECT, GUIDE, L3 and R3, which are menus rather than actions.
		 */
		m.nMoreFire = GamePadButtonX | GamePadButtonY
			    | GamePadButtonLB | GamePadButtonRB
			    | GamePadButtonLT | GamePadButtonRT
			    | GamePadButtonStart;

		/*
		 * Circle's driver also normalises the left stick into axes 0 and 1
		 * for these pads, so the stick steers as well as the D-pad.  This
		 * is safe only where the axis order is known: on a generic pad the
		 * order is the descriptor's, which is exactly what made the old
		 * fixed mapping report a constant direction.
		 */
		m.bLeftStick = TRUE;

		return m;
	}

	/*
	 * Microntek USB joystick (ven79-6), measured: left/right on axis 0 and
	 * up/down on axis 1, fire on the pad's first two buttons.  Checked before
	 * the hat rule because this pad also reports one hat, which it does not
	 * use for direction.
	 */
	if (strcmp (p, "ven79-6") == 0)
	{
		m.pName = "axes (Microntek)";
		m.Left  = DirAx (0, 64, FALSE);		/* left  -> 0   */
		m.Right = DirAx (0, 192, TRUE);		/* right -> 255 */
		m.Up    = DirAx (1, 64, FALSE);		/* up    -> 0   */
		m.Down  = DirAx (1, 192, TRUE);		/* down  -> 255 */
		m.Fire  = DirBtn (0x00002);
		m.Fire2 = DirBtn (0x00004);

		return m;
	}

	/*
	 * 2. A hat: directions come from hats[0], which is the raw HID value --
	 * 0 is up and it increases clockwise, the odd values being the diagonals.
	 * It is not an angle in degrees, which an earlier version of this driver
	 * assumed; dividing it by 45 made every hat direction read as "up".
	 *
	 * Each direction accepts the three values it spans, so a diagonal pushes
	 * two directions and a game still steers properly.
	 */
	if (pState != 0 && pState->nhats > 0)
	{
		m.pName = "hat";
		m.Up    = DirHat (0x83);		/* 7, 0, 1 */
		m.Right = DirHat (0x0E);		/* 1, 2, 3 */
		m.Down  = DirHat (0x38);		/* 3, 4, 5 */
		m.Left  = DirHat (0xE0);		/* 5, 6, 7 */
		m.Fire  = DirBtn (0x00002);		/* its second declared button */

		return m;
	}

	/*
	 * 3. Axes.  Observed on ven79-11: pressing left and right moved axis 3
	 * between 0 and 255 while no button bit changed, and up and down moved
	 * axis 4 the same way.  Those indices come from the pad's descriptor
	 * order, which is what makes a per-device mapping necessary at all.
	 *
	 * Each end of an axis is a different direction, so each direction carries
	 * its own threshold and side.
	 *
	 * The guard is five axes because the rule reads index 4.  The probe tested
	 * for four and then used the fifth; on a pad with only four it produced a
	 * map whose up and down could never activate.
	 */
	if (pState != 0 && pState->naxes >= 5)
	{
		m.pName = "axes";
		m.Left  = DirAx (3, 64, FALSE);		/* left  -> 0   */
		m.Right = DirAx (3, 192, TRUE);		/* right -> 255 */
		m.Up    = DirAx (4, 64, FALSE);		/* up    -> 0   */
		m.Down  = DirAx (4, 192, TRUE);		/* down  -> 255 */
		m.Fire  = DirBtn (0x00002);
		m.Fire2 = DirBtn (0x00004);

		return m;
	}

	/* 4. Unknown: the bits most Circle drivers use, so the pad still partly works. */
	m.pName = "fallback bits";
	m.Up    = DirBtn (0x08000);
	m.Down  = DirBtn (0x20000);
	m.Left  = DirBtn (0x40000);
	m.Right = DirBtn (0x10000);
	m.Fire  = DirBtn (0x00200);

	return m;
}

/*
 * Reads one axis of the left stick as a direction.
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

/*
 * Names a direction mask for the log: "up", "up+left", or "centred" for none.
 */
static void FormatDirections (unsigned nDirections, char *pBuffer, unsigned nSize)
{
	static const struct
	{
		unsigned	nBit;
		const char	*pName;
	} Names[] =
	{
		{ ATARI_JOY_UP,    "up" },
		{ ATARI_JOY_DOWN,  "down" },
		{ ATARI_JOY_LEFT,  "left" },
		{ ATARI_JOY_RIGHT, "right" },
	};

	unsigned nUsed = 0;
	pBuffer[0] = '\0';

	for (unsigned i = 0; i < sizeof Names / sizeof Names[0]; i++)
	{
		if ((nDirections & Names[i].nBit) == 0)
		{
			continue;
		}

		int n = snprintf (pBuffer + nUsed, nSize - nUsed, "%s%s",
				  nUsed > 0 ? "+" : "", Names[i].pName);
		if (n <= 0 || (unsigned) n >= nSize - nUsed)
		{
			break;			/* truncated; the names still say something */
		}
		nUsed += (unsigned) n;
	}

	if (pBuffer[0] == '\0')
	{
		snprintf (pBuffer, nSize, "centred");
	}
}

/*
 * Whether enough time has passed to write another action line.
 *
 * The timestamp of the last line is passed in and updated when the answer is
 * yes, so a caller that is throttled simply tries again on a later frame.
 */
static boolean MayLogNow (unsigned &nLastTicks)
{
	unsigned nNow = CTimer::GetClockTicks ();

	if (nLastTicks != 0 && nNow - nLastTicks < USB_JOYSTICK_LOG_INTERVAL_US)
	{
		return FALSE;
	}

	nLastTicks = nNow;

	return TRUE;
}

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
	/* Takes over a controller that has just been found, and resolves its map. */
	void Attach (CUSBGamePadDevice *pDevice);

	CUSBHCIDevice *m_pUSBHCI;
	unsigned m_nIndex;			/* 1-based */

	CUSBGamePadDevice *m_pDevice;

	/* Where this controller's controls are.  Valid while m_pDevice is set. */
	TMapping m_Map;

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
	volatile int m_nNhats;
	volatile int m_nNaxes;
	volatile int m_nAxis[MAX_AXIS];
	volatile int m_nAxisMin[MAX_AXIS];
	volatile int m_nAxisMax[MAX_AXIS];
	volatile unsigned m_nSequence;
	unsigned m_nLastSeen;

	/*
	 * An unplug is noticed by the device's destructor, which is not a context
	 * this file gets to choose -- so only a flag is set here and the message is
	 * written from IsPresent(), in the main loop.  Writing to the log means
	 * writing to FatFs, which must never happen below task level.
	 */
	volatile boolean m_bWasRemoved;

	/*
	 * What the log has already been told.
	 *
	 * The comparison is against the *logged* state, not against the previous
	 * frame, so a change that a throttle step skipped is still reported on one
	 * of the following frames rather than being lost.
	 */
	unsigned m_nLoggedDirections;
	unsigned m_nDirectionsLogTicks;
	boolean m_bLoggedFire;
	unsigned m_nFireLogTicks;

	/* Writes the state to the log if it differs from what was last logged. */
	void ReportDirections (unsigned nDirections);
	void ReportFire (boolean bFired, unsigned nButtons);

	/* Reports are delivered per device index, so the handler needs to find the
	 * object that owns this index.  The index is 0-based: Circle passes
	 * m_nDeviceNumber - 1. */
	static CUSBAtariJoystick *s_pInstance[USB_JOYSTICK_COUNT];
};

CUSBAtariJoystick *CUSBAtariJoystick::s_pInstance[USB_JOYSTICK_COUNT] = { 0, 0, 0, 0 };

CUSBAtariJoystick::CUSBAtariJoystick (CUSBHCIDevice *pUSBHCI, unsigned nIndex)
:	m_pUSBHCI (pUSBHCI),
	m_nIndex (nIndex),
	m_pDevice (0),
	m_bHaveState (FALSE),
	m_nButtons (0),
	m_nNhats (0),
	m_nNaxes (0),
	m_nSequence (0),
	m_nLastSeen (0),
	m_bWasRemoved (FALSE),
	m_nLoggedDirections (0),
	m_nDirectionsLogTicks (0),
	m_bLoggedFire (FALSE),
	m_nFireLogTicks (0)
{
	/* Nothing is mapped until a controller is found. */
	m_Map = EmptyMapping ();

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

void CUSBAtariJoystick::Attach (CUSBGamePadDevice *pDevice)
{
	m_pDevice = pDevice;

	m_pDevice->RegisterStatusHandler (StatusHandler);
	m_pDevice->RegisterRemovedHandler (RemovedHandler, this);

	/*
	 * This is the one moment the mapping can be worked out: the controller has
	 * just appeared, and GetInitialState() describes what it has -- its button,
	 * hat and axis counts -- before anything has been pressed.
	 */
	const TGamePadState *pState = m_pDevice->GetInitialState ();
	if (pState != 0)
	{
		StatusHandler (m_nIndex - 1, pState);
	}

	/*
	 * The vendor/product ID comes from the *device*, not from the gamepad
	 * object: CUSBGamePadDevice is a function, and asking the function for its
	 * name returns uninitialised storage (docs/progress.md section 5).
	 */
	char Id[32] = "?";
	CUSBDevice *pUSBDevice = m_pDevice->GetDevice ();
	if (pUSBDevice != 0)
	{
		CString *pId = pUSBDevice->GetName (DeviceNameVendor);
		if (pId != 0)
		{
			snprintf (Id, sizeof Id, "%s", (const char *) *pId);
			delete pId;
		}
	}

	m_Map = ResolveMapping (Id, pState);

	/*
	 * LogWarning, not LogNotice: this kernel's logger runs at LogWarning and
	 * discards everything below it, so a LogNotice here would be invisible.
	 * Written from the main loop, never from the interrupt.
	 */
	CLogger::Get ()->Write (FromJoystick, LogWarning,
				"%s attached (%s): %d buttons, %d hats, %d axes, map %s",
				GetName (), Id,
				pState != 0 ? pState->nbuttons : 0,
				pState != 0 ? pState->nhats : 0,
				pState != 0 ? pState->naxes : 0,
				m_Map.pName);
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
	CUSBGamePadDevice *pDevice = (CUSBGamePadDevice *) CDeviceNameService::Get ()
						->GetDevice ("upad", m_nIndex, FALSE);
	if (pDevice != 0)
	{
		Attach (pDevice);
	}

	return TRUE;
}

void CUSBAtariJoystick::StatusHandler (unsigned nDeviceIndex, const TGamePadState *pState)
{
	/*
	 * Circle passes the device number minus one, so the index is 0-based.  A
	 * guard against 1 used to discard every call to this handler.
	 */
	if (pState == 0 || nDeviceIndex >= USB_JOYSTICK_COUNT)
	{
		return;
	}

	CUSBAtariJoystick *pThis = s_pInstance[nDeviceIndex];
	if (pThis == 0)
	{
		return;
	}

	/*
	 * Copy, and nothing else.  This runs in the USB interrupt: no logging, no
	 * file system, no allocation (docs/progress.md section 5).
	 */
	pThis->m_nButtons = pState->buttons;
	pThis->m_nNaxes = pState->naxes > MAX_AXIS ? MAX_AXIS : pState->naxes;
	pThis->m_nNhats = pState->nhats > MAX_HATS ? MAX_HATS : pState->nhats;

	for (int i = 0; i < pThis->m_nNaxes; i++)
	{
		pThis->m_nAxis[i] = pState->axes[i].value;
		pThis->m_nAxisMin[i] = pState->axes[i].minimum;
		pThis->m_nAxisMax[i] = pState->axes[i].maximum;
	}

	for (int i = 0; i < pThis->m_nNhats; i++)
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

		/*
		 * The controller is gone, so its mapping is no longer this adapter's.
		 * A different pad plugged in later is resolved from scratch by Attach().
		 */
		pThis->m_Map = EmptyMapping ();

		/* What the log was told belongs to the controller that has just left. */
		pThis->m_nLoggedDirections = 0;
		pThis->m_bLoggedFire = FALSE;

		/* Reported from IsPresent(), which is known to run in the main loop. */
		pThis->m_bWasRemoved = TRUE;
	}
}

unsigned CUSBAtariJoystick::GetDirections (void)
{
	/*
	 * Ask whether a controller has appeared before using the state.
	 *
	 * This is the only entry point the emulation core calls, so it is also the
	 * only place a controller plugged in after start-up can be noticed.  IsPresent()
	 * performs that lookup and registers the report handlers; without this call
	 * the adapter sat permanently unattached, reporting centre and no fire, with
	 * nothing in the log to say why.
	 */
	(void) IsPresent ();

	if (!m_bHaveState || m_pDevice == 0)
	{
		ReportDirections (0);

		return 0;
	}

	/*
	 * A controller that has gone quiet is treated as released, so an unplugged
	 * or crashed controller cannot leave a direction held down.
	 */
	if (CTimer::Get ()->GetTicks () - m_nLastSeen > USB_JOYSTICK_TIMEOUT_MS)
	{
		ReportDirections (0);

		return 0;
	}

	/*
	 * Copy the report, checking that it did not change underneath us, into a
	 * form the resolver can be asked about.
	 */
	TGamePadState State;
	memset (&State, 0, sizeof State);

	unsigned nSequence;
	do
	{
		nSequence = m_nSequence;

		State.nbuttons = (int) m_nButtons;
		State.buttons = m_nButtons;
		State.naxes = m_nNaxes;
		for (int i = 0; i < State.naxes && i < MAX_AXIS; i++)
		{
			State.axes[i].value = m_nAxis[i];
			State.axes[i].minimum = m_nAxisMin[i];
			State.axes[i].maximum = m_nAxisMax[i];
		}
		State.nhats = m_nNhats;
		for (int i = 0; i < State.nhats && i < MAX_HATS; i++)
		{
			State.hats[i] = m_nHats[i];
		}
	}
	while (nSequence != m_nSequence);

	unsigned nDirections = 0;

	if (IsActive (m_Map.Up, &State))	nDirections |= ATARI_JOY_UP;
	if (IsActive (m_Map.Down, &State))	nDirections |= ATARI_JOY_DOWN;
	if (IsActive (m_Map.Left, &State))	nDirections |= ATARI_JOY_LEFT;
	if (IsActive (m_Map.Right, &State))	nDirections |= ATARI_JOY_RIGHT;

	/*
	 * On a pad Circle knows, the left stick steers as well as the D-pad.  The
	 * two cannot conflict: a direction is held if either source says so, which
	 * is what a player moving from one to the other expects.
	 */
	if (m_Map.bLeftStick && State.naxes > GamePadAxisLeftY)
	{
		int nX = AxisDirection (State.axes[GamePadAxisLeftX].value,
					State.axes[GamePadAxisLeftX].minimum,
					State.axes[GamePadAxisLeftX].maximum);
		int nY = AxisDirection (State.axes[GamePadAxisLeftY].value,
					State.axes[GamePadAxisLeftY].minimum,
					State.axes[GamePadAxisLeftY].maximum);

		if (nX < 0)	nDirections |= ATARI_JOY_LEFT;
		if (nX > 0)	nDirections |= ATARI_JOY_RIGHT;
		if (nY < 0)	nDirections |= ATARI_JOY_UP;
		if (nY > 0)	nDirections |= ATARI_JOY_DOWN;
	}

	ReportDirections (nDirections);

	return nDirections;
}

/*
 * Writes a direction change to the log.
 *
 * Only changes are written, and at most one line per interval: the log is a
 * file on the card, flushed line by line, and this runs in the frame loop.  A
 * change that the interval suppresses is not lost -- the comparison is against
 * what was logged, so the next frame that is allowed to write reports the
 * current state.
 */
void CUSBAtariJoystick::ReportDirections (unsigned nDirections)
{
	if (nDirections == m_nLoggedDirections || !MayLogNow (m_nDirectionsLogTicks))
	{
		return;
	}

	m_nLoggedDirections = nDirections;

	char Text[32];
	FormatDirections (nDirections, Text, sizeof Text);

	CLogger::Get ()->Write (FromJoystick, LogWarning, "%s: %s", GetName (), Text);
}

/*
 * Writes a fire change to the log, with the buttons that produced it -- the
 * only way to tell "the pad reports nothing" from "the mapping is wrong".
 */
void CUSBAtariJoystick::ReportFire (boolean bFired, unsigned nButtons)
{
	if (bFired == m_bLoggedFire || !MayLogNow (m_nFireLogTicks))
	{
		return;
	}

	m_bLoggedFire = bFired;

	CLogger::Get ()->Write (FromJoystick, LogWarning, "%s: fire %s (buttons 0x%X)",
				GetName (), bFired ? "pressed" : "released", nButtons);
}

int CUSBAtariJoystick::GetTrigger (void)
{
	if (!m_bHaveState || m_pDevice == 0)
	{
		ReportFire (FALSE, 0);

		return ATARI_JOY_TRIG_OPEN;
	}

	if (CTimer::Get ()->GetTicks () - m_nLastSeen > USB_JOYSTICK_TIMEOUT_MS)
	{
		ReportFire (FALSE, 0);

		return ATARI_JOY_TRIG_OPEN;
	}

	/*
	 * The buttons are one word, and are copied under the same sequence check the
	 * directions use, so fire cannot be read from two different reports.
	 */
	TGamePadState State;
	memset (&State, 0, sizeof State);

	unsigned nSequence;
	do
	{
		nSequence = m_nSequence;

		State.nbuttons = (int) m_nButtons;
		State.buttons = m_nButtons;
	}
	while (nSequence != m_nSequence);

	/*
	 * The resolver names the two buttons that were measured as fire on this
	 * controller.  On a pad Circle knows, nMoreFire widens that to every button
	 * that could reasonably mean "fire"; on an unrecognised pad a bit number
	 * says nothing about which physical button it is, so only the measured two
	 * are used and the rest cannot fire by accident.
	 */
	boolean bFired = IsActive (m_Map.Fire, &State) || IsActive (m_Map.Fire2, &State);
	if (!bFired && m_Map.nMoreFire != 0)
	{
		bFired = (State.buttons & m_Map.nMoreFire) != 0;
	}

	ReportFire (bFired, State.buttons);

	return bFired ? ATARI_JOY_TRIG_FIRED : ATARI_JOY_TRIG_OPEN;
}

boolean CUSBAtariJoystick::IsPresent (void)
{
	/* An unplug is reported here, where writing to the log is safe. */
	if (m_bWasRemoved)
	{
		m_bWasRemoved = FALSE;

		CLogger::Get ()->Write (FromJoystick, LogWarning, "%s removed",
					GetName ());
	}

	/*
	 * Looked up again rather than remembered, so that a controller plugged in
	 * after start-up is noticed.  The name service lookup is a short list walk
	 * and this is called once per frame.
	 */
	if (m_pDevice == 0 && m_pUSBHCI != 0)
	{
		CUSBGamePadDevice *pDevice = (CUSBGamePadDevice *) CDeviceNameService::Get ()
						->GetDevice ("upad", m_nIndex, FALSE);
		if (pDevice != 0)
		{
			Attach (pDevice);
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
