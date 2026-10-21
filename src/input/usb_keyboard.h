/*
 * usb_keyboard.h -- the Mode 1 keyboard: a USB keyboard attached to the Pi.
 *
 * A real Atari XL/XE keyboard is a scan matrix, and the emulation core wants
 * exactly what that matrix produces: a 6-bit KBCODE for the key that is down,
 * plus separate flags for Shift/Control and for the console keys.  This class
 * translates a USB HID keyboard into that form.
 *
 * Circle's USB stack hands us the raw HID boot report -- six key usage codes
 * plus a modifier byte -- from an interrupt handler.  Those usage codes are
 * what we map, not the cooked characters Circle's keyboard driver would produce
 * instead, because a stream of characters cannot say where a key is or when it
 * is released.
 *
 * The mapping deliberately mimics a real Atari keyboard: a USB Shift or Control
 * sets the corresponding field of atari_keyboard_state_t and the *base* key is
 * reported, leaving it to the emulation core -- and the emulated OS -- to turn
 * that into a character.  See usb_keyboard.cpp for the whole translation table.
 *
 * The class also owns the OSD menu's share of the keyboard: F12 opens it, and
 * while it is open the cursor keys, Return, Escape and Space are handed to the
 * menu instead of the emulated machine.
 */

#ifndef ATARI_PI_USB_KEYBOARD_H
#define ATARI_PI_USB_KEYBOARD_H

#include <circle/types.h>
#include <circle/spinlock.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/usb/usbhid.h>

#include "keyboard.h"

/*
 * "Reset" is not a key of the Atari keyboard matrix.  A real machine resets
 * when the Reset line is pulled, which the core models with the special
 * (negative) AKEY codes that can never collide with a real KBCODE.
 *
 * The core acts on both of them when it sees one in INPUT_key_code -- see the
 * switch at the top of Atari800_Frame() in atari800/src/atari.c.  F8 produces a
 * *warm* start, the Reset key of a real machine; a cold start is left to the
 * kernel, which can call ATARI_ColdReset() directly.
 *
 * ATARI_KEY_WARMSTART is the core's AKEY_WARMSTART.  The port's atari_port.h
 * publishes only AKEY_BREAK of the special codes, so it is repeated here.
 */
#define ATARI_KEY_WARMSTART	(-2)

/*
 * How many USB keyboards are tracked at once.
 *
 * More than one may be plugged in.  Only the last one to send a report drives
 * the emulated machine, but every one of them is tracked so that a report from
 * an idle keyboard cannot clear a key held on another.  Each keyboard costs one
 * 32-byte bit map.
 */
#define USB_KEYBOARD_MAX_DEVICES	4

class CUSBAtariKeyboard : public CAtariKeyboard
{
public:
	/*
	 * PUSBHCI is the host controller the kernel already owns and has
	 * initialised; CUSBHCIDevice is Circle's alias for the class that fits
	 * the model being built for.  It must outlive this object, which is
	 * normally a member of CKernel too.
	 *
	 * N_TIMEOUT_MS bounds how long Initialize() waits for a keyboard to turn
	 * up.  It is zero, so start-up never waits at all.
	 *
	 * A keyboard is already enumerated by the time this runs -- the USB stack does
	 * that during its own initialisation -- and Update() keeps looking every frame
	 * afterwards, so one plugged in later is picked up regardless.  Waiting here
	 * bought nothing and cost a black screen whenever no keyboard was present: on
	 * a Zero 2 W with nothing attached the machine stopped at this stage, after
	 * the "keyboard and controllers" marker and before the first frame, with
	 * nothing logged because nothing had failed.
	 */
	CUSBAtariKeyboard (CUSBHCIDevice *pUSBHCI, unsigned nTimeoutMs = 0);
	~CUSBAtariKeyboard (void);

	/*
	 * Waits for a keyboard to appear and starts listening to it.
	 *
	 * Returns TRUE as soon as one keyboard is usable.  Returns FALSE if the
	 * host controller is missing or was never initialised, or if no keyboard
	 * turned up within the timeout -- which is not fatal: the kernel carries
	 * on without a keyboard and keeps calling Update(), so one plugged in
	 * later is picked up.
	 */
	boolean Initialize (void);

	/*
	 * Picks up keyboards attached or removed since the last call.
	 *
	 * Hot plugging is asynchronous, so this has to be called from TASK_LEVEL
	 * regularly -- once per emulated frame is plenty.  It is cheap and safe
	 * to call whether or not a keyboard is attached.
	 */
	void Update (void);

	/* --- CAtariKeyboard ------------------------------------------------ */

	/* Fills PSTATE with the Atari keys currently held. */
	void GetState (atari_keyboard_state_t *pState) override;

	/*
	 * TRUE once per F12 press: this is what opens the OSD menu.  The caller
	 * consumes the request, so holding F12 down does not re-open the menu.
	 */
	boolean ConsumeMenuRequest (void) override;

	/*
	 * The next cursor/Return/Escape/Space press for the menu, or
	 * ATARI_KEY_NONE when there is nothing pending.
	 *
	 * Each press is handed out exactly once.  While the menu is active those
	 * keys are held back from GetState() as well, so the arrow keys move the
	 * menu selection instead of the emulated cursor.
	 */
	int ConsumeMenuKey (void) override;

	boolean IsPresent (void) override;

	/* Makes the keypad's 2/4/6/8 arrows act as the Atari cursor keys. */
	void SetNumpadAsCursor (boolean bEnabled) override;

	/* Tells the driver whether the OSD menu is open; see ConsumeMenuKey(). */
	void SetMenuActive (boolean bActive) override;

	/*
	 * Entry point for one raw HID report.
	 *
	 * Circle's raw handler carries no context, so the adapters in
	 * usb_keyboard.cpp call this with the keyboard they belong to.  It runs
	 * in interrupt context and only records what the report says.  Public
	 * because the adapter is not part of this class.
	 */
	static void KeyReport (CUSBKeyboardDevice *pKeyboard,
			       unsigned char ucModifiers,
			       const unsigned char RawKeys[6]);

private:
	/* HID usage codes fit in one byte, so one bit each. */
	static const unsigned BitmapWords = 4;

	struct TBitmap
	{
		u64	Words[BitmapWords];

		void Clear (void);
		void Set (unsigned nUsage);
		boolean IsSet (unsigned nUsage) const;
		/* ORs in the key list of an 8-byte HID report. */
		void AddReportKeys (const unsigned char RawKeys[6]);
	};

	/*
	 * What one attached keyboard last told us.
	 *
	 * The bit map is written only by the interrupt handler, but a keyboard
	 * can be unplugged from the main thread at any moment -- that is how the
	 * name service calls our removed handler -- so all of it is under
	 * CUSBAtariKeyboard's spin lock.
	 */
	struct TReport
	{
		CUSBKeyboardDevice	*pKeyboard;	/* 0 when the slot is free */
		boolean			 bActive;
		boolean			 bF12Down;	/* F12 was down last report */
		TBitmap			 Held;		/* keys down last report */
	};

	/* One keyboard's contribution to the Atari state. */
	struct TEventState
	{
		boolean	 bActive;
		int	 nKeyCode;	/* base ATARI_KEY_* of the key down */
		boolean	 bShift;
		boolean	 bControl;
		boolean	 bStart;
		boolean	 bSelect;
		boolean	 bOption;
	};

	/* Lowest set bit, or ATARI_KEY_NONE when the map is empty. */
	static int FirstSetBit (const TBitmap &rBitmap);

	/* HID usage code to Atari key, or ATARI_KEY_NONE.  The whole table. */
	static int AtariKeyForUsage (unsigned nUsage);

	/* Iff the usage code is a console key, sets its flag. */
	static void ConsoleKeysForUsage (unsigned nUsage, boolean *pbStart,
					 boolean *pbSelect, boolean *pbOption);

	/*
	 * The key the OSD menu uses for this HID usage code, or ATARI_KEY_NONE
	 * if the menu ignores it.  This is the one place the menu's keys are
	 * named, so the queue and GetState() cannot disagree about them.
	 *
	 * Free of the object because the interrupt handler needs it and has no
	 * object; the two settings it depends on are passed in.
	 */
	static int MenuKeyForUsage (unsigned nUsage, boolean bNumpadAsCursor,
				    boolean bMenuActive);

	/* Turns one report into Atari state.  Caller holds the spin lock. */
	static void HandleReport (unsigned nEvent, unsigned char ucModifiers,
				  const unsigned char RawKeys[6]);

	/* Called by the name service when a keyboard goes away. */
	static void KeyboardRemovedHandler (CDevice *pDevice, void *pContext);

	/* Registers every "ukbd<N>" we are not listening to yet. */
	void ScanDevices (void);

	/*
	 * The state behind the spin lock.  Static, because Circle's raw key
	 * handler is a plain function pointer: each keyboard registers a
	 * callback carrying only a pointer to itself, so the handler has to
	 * find the shared state without an object to hang it on.
	 */
	static CSpinLock	s_SpinLock;
	static TReport		s_Report[USB_KEYBOARD_MAX_DEVICES];
	static TEventState	s_Event[USB_KEYBOARD_MAX_DEVICES];

	/*
	 * Modified from interrupt context, so volatile: a reader may see it
	 * change between two statements, but never a torn value, because it is
	 * word sized and only ever written whole.
	 */
	static volatile unsigned s_nLastEvent;

	/*
	 * The menu's pending presses, one ring per keyboard.  Written only by
	 * the interrupt handler, drained only by ConsumeMenuKey().
	 */
	static const unsigned	MenuQueueSize = 8;
	static int		s_MenuQueue[USB_KEYBOARD_MAX_DEVICES][MenuQueueSize];
	static unsigned		s_nMenuHead[USB_KEYBOARD_MAX_DEVICES];
	static unsigned		s_nMenuTail[USB_KEYBOARD_MAX_DEVICES];

	/* Latched by the interrupt handler, taken by ConsumeMenuRequest(). */
	static boolean		s_bMenuRequest;

	/*
	 * Mirrors of the two settings below, for the interrupt handler, which
	 * has no object to read them from.  Written only by the main thread.
	 */
	static boolean		s_bNumpadAsCursor;
	static boolean		s_bMenuActive;

	/* Only the main thread touches these two, so they need no lock. */
	boolean	m_bNumpadAsCursor;
	boolean	m_bMenuActive;

	CUSBHCIDevice	*m_pUSBHCI;
	unsigned	 m_nTimeoutMs;
};

#endif
