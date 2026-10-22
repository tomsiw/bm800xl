/*
 * usb_keyboard.cpp -- Mode 1 keyboard: a USB keyboard mapped onto the Atari.
 *
 * The heart of this file is AtariKeyForUsage(): a USB HID usage code goes in,
 * the Atari key in the same place on the keyboard comes out.  The Atari side of
 * that table is the set of ATARI_KEY_* key codes -- the KBCODE the emulated
 * keyboard matrix would produce -- plus the console keys, which on an XL/XE are
 * not part of the matrix at all and have their own fields in the state.
 *
 * Two things are deliberately *not* done here:
 *
 *   * Shift and Control are not folded into the key code.  They are reported as
 *     the shift/control fields, exactly as a real keyboard does: the matrix
 *     reports one key and a separate shift line, and the OS looks the shifted
 *     character up.  A USB Shift therefore shifts every key the way the Atari's
 *     own Shift does.  Because the Atari's shifted number row is not the PC's,
 *     some shifted symbols come out on a different key than on a PC.
 *
 *   * Nothing that runs in interrupt context allocates, logs or blocks.  The
 *     raw report handler only touches a bit map and a small ring buffer, and
 *     everything it shares with the main thread is under one spin lock.
 *
 * Every attached keyboard is tracked separately.  A report can only add to or
 * clear its own keyboard's keys, so an idle second keyboard cannot make a key
 * held on the first one stick or vanish; the machine follows whichever keyboard
 * last sent a report, which is what a user expects when they pick up another
 * keyboard.
 */

#include "usb_keyboard.h"

#include <circle/timer.h>
#include <circle/devicenameservice.h>
#include <circle/device.h>

/* How long Initialize() waits between two looks for a keyboard. */
#define USB_KEYBOARD_POLL_MS	10

/* How many "ukbd<N>" names to try.  Circle numbers keyboards from 1. */
#define USB_KEYBOARD_MAX_INDEX	16

/* Key codes in HID report: six of them, with the modifiers in a separate byte. */
#define USB_KEYBOARD_REPORT_KEYS	6

/*
 * HID usage codes this file refers to by name.
 *
 * The console keys are on F6/F7/F8, which is where an Atari XL/XE has them:
 *
 *   F5  Help
 *   F6  Start
 *   F7  Select
 *   F8  Option
 *   F10 Reset          (handled in platform.c, not as a console key)
 *
 * These positions are not a matter of preference.  Option decides whether a cold
 * boot reaches BASIC or the built-in diagnostic, and Start reaches SELF TEST, so
 * the keys that change what the machine does at power-on must not be adjacent to
 * a key that does something else.
 */
#define USB_HID_START		0x3f	/* F6; Start console key */
#define USB_HID_SELECT		0x40	/* F7; Select console key */
#define USB_HID_OPTION		0x41	/* F8; Option console key */
#define USB_HID_F12		0x45	/* the OSD menu key */

/*
 * The keypad digits that double as cursor keys, plus the console keys: three
 * of the few usages that are checked by number rather than through the table.
 */
#define USB_HID_KP_2		0x5a
#define USB_HID_KP_4		0x5c
#define USB_HID_KP_6		0x5e
#define USB_HID_KP_8		0x60

/* ---------------------------------------------------------------------------
 * State shared with the interrupt handler
 *
 * All of it is static because Circle's raw key handler is a plain function
 * pointer: a keyboard registers a callback that carries nothing but a pointer
 * to itself, so the handler has to reach the shared state without an object to
 * hang it on.
 * ------------------------------------------------------------------------- */

CSpinLock CUSBAtariKeyboard::s_SpinLock;

volatile unsigned CUSBAtariKeyboard::s_nLastEvent = (unsigned) -1;

CUSBAtariKeyboard::TEventState CUSBAtariKeyboard::s_Event[USB_KEYBOARD_MAX_DEVICES];

CUSBAtariKeyboard::TReport CUSBAtariKeyboard::s_Report[USB_KEYBOARD_MAX_DEVICES];

int CUSBAtariKeyboard::s_MenuQueue[USB_KEYBOARD_MAX_DEVICES][CUSBAtariKeyboard::MenuQueueSize];

unsigned CUSBAtariKeyboard::s_nMenuHead[USB_KEYBOARD_MAX_DEVICES];

unsigned CUSBAtariKeyboard::s_nMenuTail[USB_KEYBOARD_MAX_DEVICES];

boolean CUSBAtariKeyboard::s_bMenuRequest = FALSE;

/*
 * The two OSD settings, mirrored here because the interrupt handler has no
 * object to read them from.  They are written only by the main thread.
 */
boolean CUSBAtariKeyboard::s_bNumpadAsCursor = FALSE;

boolean CUSBAtariKeyboard::s_bMenuActive = FALSE;

/* ---------------------------------------------------------------------------
 * Bit maps over HID usage codes
 *
 * The raw handler gets the six key codes of one report.  A bit map makes "is
 * this key down?" a single test, and comparing one report against the previous
 * one is how a press is told apart from a key that was already down -- which is
 * what makes the menu key edge triggered.
 * ------------------------------------------------------------------------- */

void CUSBAtariKeyboard::TBitmap::Clear (void)
{
	for (unsigned i = 0; i < BitmapWords; i++)
	{
		Words[i] = 0;
	}
}

void CUSBAtariKeyboard::TBitmap::Set (unsigned nUsage)
{
	Words[nUsage >> 6] |= (u64) 1 << (nUsage & 63);
}

boolean CUSBAtariKeyboard::TBitmap::IsSet (unsigned nUsage) const
{
	return (Words[nUsage >> 6] & ((u64) 1 << (nUsage & 63))) != 0 ? TRUE : FALSE;
}

/* ORs the non-empty slots of an 8-byte HID report into the bit map. */
void CUSBAtariKeyboard::TBitmap::AddReportKeys (const unsigned char RawKeys[6])
{
	for (unsigned i = 0; i < USB_KEYBOARD_REPORT_KEYS; i++)
	{
		/*
		 * Zero means "this slot is empty", so it must not set bit 0.
		 * The error and roll-over codes (0x01..0x03) do set bits, but
		 * they mean nothing to the Atari and the table drops them.
		 */
		if (RawKeys[i] != 0)
		{
			Set (RawKeys[i]);
		}
	}
}

int CUSBAtariKeyboard::FirstSetBit (const TBitmap &rBitmap)
{
	for (unsigned i = 0; i < BitmapWords; i++)
	{
		u64 ullWord = rBitmap.Words[i];
		if (ullWord == 0)
		{
			continue;
		}

		unsigned nBit = 0;
		while ((ullWord & 1) == 0)
		{
			ullWord >>= 1;
			nBit++;
		}

		return (int) (i * 64 + nBit);
	}

	return (int) ATARI_KEY_NONE;
}

/* ---------------------------------------------------------------------------
 * The key table
 * ------------------------------------------------------------------------- */

/*
 * Maps a HID usage code to the Atari key in the same keyboard position, or to
 * ATARI_KEY_NONE when the Atari has no key there.
 *
 * Where a USB key has an unshifted and a shifted legend, the *unshifted* key is
 * returned and the shift field does the rest.  Letters go through
 * ATARI_KeyCodeForLetter() so the 26-entry KBCODE table lives in one place, and
 * digits use ATARI_KEY_1 .. ATARI_KEY_0 so that the number row shifts the way
 * the Atari's does rather than the way a PC's does.
 *
 * A few keys are marked "keyed by character": they have no Atari matrix
 * position of their own, so they stand for the character printed on the key
 * cap.  On a machine whose keyboard layout cannot be changed that is the useful
 * behaviour.
 */
int CUSBAtariKeyboard::AtariKeyForUsage (unsigned nUsage)
{
	/* Letters: the base code; Shift is reported separately. */
	if (nUsage >= 0x04 && nUsage <= 0x1d)
	{
		return (int) ATARI_KeyCodeForLetter ((char) ('A' + (nUsage - 0x04)));
	}

	switch (nUsage)
	{
	/*
	 * The Atari's shifted number row is its own row rather than the PC's,
	 * so the digits are reported as digits and the Atari's OS decides what
	 * Shift makes of them.
	 */
	case 0x1e: return ATARI_KEY_1;
	case 0x1f: return ATARI_KEY_2;
	case 0x20: return ATARI_KEY_3;
	case 0x21: return ATARI_KEY_4;
	case 0x22: return ATARI_KEY_5;
	case 0x23: return ATARI_KEY_6;
	case 0x24: return ATARI_KEY_7;
	case 0x25: return ATARI_KEY_8;
	case 0x26: return ATARI_KEY_9;
	case 0x27: return ATARI_KEY_0;

	case 0x28: return ATARI_KEY_RETURN;

	case 0x29: return ATARI_KEY_ESCAPE;
	case 0x2a: return ATARI_KEY_BACKSPACE;
	case 0x2b: return ATARI_KEY_TAB;
	case 0x2c: return ATARI_KEY_SPACE;
	case 0x2d: return ATARI_KEY_MINUS;
	case 0x2e: return ATARI_KEY_EQUAL;
	case 0x2f: return ATARI_KEY_BRACKETLEFT;
	case 0x30: return ATARI_KEY_BRACKETRIGHT;
	case 0x31: return ATARI_KEY_BACKSLASH;

	/* Keyed by character: the PC has no key in the Atari's position. */
	case 0x33: return ATARI_KEY_SEMICOLON;

	/*
	 * The Atari key that carries '*' unshifted has '^' above it, so it is
	 * AKEY_ASTERISK (0x07) with Shift applied by the OS to reach AKEY_CIRCUMFLEX
	 * (0x47 = 0x07 | SHFT).
	 *
	 * Reporting CIRCUMFLEX here instead -- which is what this did -- set the
	 * shift bit in the key code itself, so the key typed '^' whatever the Shift
	 * key was doing, and '*' could not be typed from the main keyboard at all.
	 */
	case 0x34: return ATARI_KEY_ASTERISK;	/* '*' ; Shift gives '^' */

	case 0x36: return ATARI_KEY_COMMA;
	case 0x37: return ATARI_KEY_FULLSTOP;
	case 0x38: return ATARI_KEY_SLASH;

	/*
	 * Caps Lock and Num Lock are matrix keys on an Atari: Caps is the
	 * Caps/Control key, and the Atari key is Inverse.  On a PC they are
	 * toggle locks, but holding one down is the closest a USB keyboard
	 * comes to holding the Atari key, and it is what a report shows.
	 */
	case 0x39: return ATARI_KEY_CAPSLOCK;

	/* F1..F4 are fitted only on a 1200XL; elsewhere they do nothing. */
	case 0x3a: return ATARI_KEY_F1;
	case 0x3b: return ATARI_KEY_F2;
	case 0x3c: return ATARI_KEY_F3;
	case 0x3d: return ATARI_KEY_F4;

	/*
	 * The console keys, on the function keys:
	 *
	 *   F5  Help      (a matrix key, so it stays a key code)
	 *   F6  Start     \
	 *   F7  Select     |  console keys, reported through their own fields
	 *   F8  Option    /   (see ConsoleKeysForUsage())
	 *   F10 Reset
	 *
	 * Help and Start share a physical key on the real XL/XE, so F5 and F6 differ
	 * only in the Atari key they stand for.
	 *
	 * These positions are fixed by the hardware rather than chosen: Option
	 * decides whether a cold boot reaches BASIC or the built-in diagnostic, and
	 * Start reaches SELF TEST, so the two keys that change what the machine does
	 * at power-on have to be exactly where they are.
	 */
	case 0x3e: return ATARI_KEY_HELP;	/* F5 */
	case 0x3f: return ATARI_KEY_START;	/* F6 -- also a console key */

	/*
	 * F7 is Select, which on an XL/XE is a console key -- it shares the PIA
	 * port with Start and Option -- so it is reported through pbSelect rather
	 * than as a matrix key code, and returns nothing from here.  Consuming the
	 * key here as well would make it double as an unassigned Atari key.
	 */
	case 0x40: return ATARI_KEY_NONE;	/* F7 -- Select, see ConsoleKeysForUsage() */

	case 0x41: return ATARI_KEY_NONE;	/* F8 -- Option, see ConsoleKeysForUsage() */

	/* Reset is not a matrix key on the Atari; see usb_keyboard.h. */
	case 0x43: return ATARI_KEY_WARMSTART;	/* F10 */

	/*
	 * Break is not a matrix key either.
	 *
	 * On a real XL/XE, Break is a separate line that pulls the keyboard
	 * hardware into an NMI, which is why it has its own negative code rather
	 * than a KBCODE.  F9 is the obvious free function key for it: F1..F4 are
	 * the Atari's own function keys, F5..F8 and F10 are the console keys, and
	 * F12 opens the menu.
	 */
	case 0x42: return ATARI_KEY_BREAK;	/* F9 */

	/* 0x44 F11 is unused. */
	/* 0x45 F12 is the menu key; see MenuKeyForUsage(). */

	case 0x49: return ATARI_KEY_INSERT_CHAR;
	case 0x4c: return ATARI_KEY_DELETE_CHAR;

	/* Cursor keys. */
	case 0x4f: return ATARI_KEY_RIGHT;
	case 0x50: return ATARI_KEY_LEFT;
	case 0x51: return ATARI_KEY_DOWN;
	case 0x52: return ATARI_KEY_UP;

	case 0x53: return ATARI_KEY_ATARI;	/* Num Lock -> Inverse */

	/*
	 * The keypad.  0x5a/0x5c/0x5e/0x60 also serve as the cursor keys.
	 *
	 * The digit assignments run 0x59..0x61 for 1..9, which is correct for the
	 * HID usage table.  Keypad Enter is 0x58, keypad 0 is 0x62 and keypad '.' is
	 * 0x63; this block previously treated 0x58 as "keypad 0", which made Enter
	 * type a zero and left the real 0 and '.' keys producing a full stop and
	 * nothing at all.
	 */
	case 0x54: return ATARI_KEY_SLASH;
	case 0x55: return ATARI_KEY_ASTERISK;
	case 0x56: return ATARI_KEY_MINUS;
	case 0x57: return ATARI_KEY_PLUS;
	case 0x58: return ATARI_KEY_RETURN;	/* Keypad Enter */
	case 0x59: return ATARI_KEY_1;
	case 0x5a: return ATARI_KEY_2;
	case 0x5b: return ATARI_KEY_3;
	case 0x5c: return ATARI_KEY_4;
	case 0x5d: return ATARI_KEY_5;
	case 0x5e: return ATARI_KEY_6;
	case 0x5f: return ATARI_KEY_7;
	case 0x60: return ATARI_KEY_8;
	case 0x61: return ATARI_KEY_9;
	case 0x62: return ATARI_KEY_0;		/* Keypad 0 */
	case 0x63: return ATARI_KEY_FULLSTOP;	/* Keypad . */
	case 0x64: return ATARI_KEY_BACKSLASH;

	default:
		break;
	}

	/*
	 * Keys with no Atari equivalent, reported as nothing at all:
	 *
	 *   0x46 Print Screen, 0x47 Scroll Lock, 0x48 Pause
	 *   0x4a Home, 0x4b Page Up, 0x4d End, 0x4e Page Down
	 *   0x63 Keypad =, 0x65 Applications, 0x66 Power
	 *   0x67 Keypad =, 0x68..0x73 the F13..F24 block
	 *   0x74 Execute, 0x75 Help (the PC one), 0x76 Menu
	 *   0x77 Select, 0x78 Stop, 0x79 Again, 0x7a Undo, 0x7b Cut
	 *   0x7c Copy, 0x7d Paste, 0x7e Find, 0x7f Mute
	 *   0x80 Volume Up, 0x81 Volume Down, 0x85..0xa4 international keys
	 *   0xe0..0xe7 the modifiers, which come from the modifier byte
	 *
	 * These Atari keys have no matrix position a PC key maps to directly.  Each
	 * is reached through a changed Shift combination applied in
	 * ATARI_ApplyModifiers() -- see platform.c for the reasoning:
	 *
	 *   ATARI_KEY_LESS               Shift+Comma       '<'
	 *   ATARI_KEY_GREATER            Shift+Fullstop    '>'
	 *   ATARI_KEY_BAR                Shift+Backslash   '|'
	 *   ATARI_KEY_CLEAR              Shift+Escape      Clear
	 *   ATARI_KEY_DELETE_LINE        Shift+Delete
	 *   ATARI_KEY_INSERT_LINE        Shift+Insert
	 *
	 * Everything else with a shifted legend is handled by the Atari OS from the
	 * unshifted key plus the shift line, and comes out as the Atari's own
	 * character rather than the PC's.  That is deliberate: it means the number
	 * row types the Atari's symbols, not the PC's.
	 */
	return (int) ATARI_KEY_NONE;
}

void CUSBAtariKeyboard::ConsoleKeysForUsage (unsigned nUsage, boolean *pbStart,
					     boolean *pbSelect, boolean *pbOption)
{
	/*
	 * Start, Select and Option are console keys on an XL/XE: they are not
	 * in the keyboard matrix and have their own fields in the state, which
	 * the platform layer inverts (a clear bit means pressed).
	 */
	switch (nUsage)
	{
	case USB_HID_START:  *pbStart = TRUE; break;
	case USB_HID_SELECT: *pbSelect = TRUE; break;
	case USB_HID_OPTION: *pbOption = TRUE; break;

	default:
		break;
	}

	/*
	 * Select is reachable through the OSD menu instead of a console key.
	 *
	 * The parameter exists so that it can be given a key in one place later
	 * without touching the rest of the path; until then it is deliberately left
	 * clear.  See the note on USB_HID_START for why it does not simply take F5.
	 */
	(void) pbSelect;
}

/*
 * The key menu handling gives this usage code, or ATARI_KEY_NONE if it does not
 * handle it.  Free of the object on purpose: the interrupt handler needs it and
 * has no object, so the two settings it depends on are passed in.
 */
int CUSBAtariKeyboard::MenuKeyForUsage (unsigned nUsage, boolean bNumpadAsCursor,
					boolean bMenuActive)
{
	/*
	 * The keypad's arrows are the cursor keys once SetNumpadAsCursor() has
	 * been called, whether the menu is open or not.
	 */
	if (bNumpadAsCursor)
	{
		switch (nUsage)
		{
		case USB_HID_KP_2: return ATARI_KEY_DOWN;
		case USB_HID_KP_4: return ATARI_KEY_LEFT;
		case USB_HID_KP_6: return ATARI_KEY_RIGHT;
		case USB_HID_KP_8: return ATARI_KEY_UP;

		default:
			break;
		}
	}

	/*
	 * Only the keys the menu acts on are handed to it, and only while the
	 * menu is open.  Everything else -- and everything while the menu is
	 * closed -- is left for the emulated machine.
	 */
	if (!bMenuActive)
	{
		return (int) ATARI_KEY_NONE;
	}

	int nKeyCode = AtariKeyForUsage (nUsage);

	switch (nKeyCode)
	{
	case ATARI_KEY_UP:
	case ATARI_KEY_DOWN:
	case ATARI_KEY_LEFT:
	case ATARI_KEY_RIGHT:
	case ATARI_KEY_RETURN:
	case ATARI_KEY_ESCAPE:
	case ATARI_KEY_SPACE:
		return nKeyCode;

	default:
		break;
	}

	return (int) ATARI_KEY_NONE;
}

/* ---------------------------------------------------------------------------
 * Recording one report (interrupt context)
 * ------------------------------------------------------------------------- */

/*
 * Circle's raw handler carries no context of its own, and a member function
 * cannot be handed to it, so one adapter per keyboard is made here: it holds
 * the keyboard it belongs to and passes it on when the report arrives.  Each
 * keyboard then gets its own callback, which is what lets several of them be
 * told apart -- a single shared callback would misattribute every report.
 *
 * The adapters are named by slot, but the slot is only what the report is
 * looked up by, so a stale adapter (`s_bUsed` already cleared) is harmless: its
 * keyboard's slot is gone and the report is dropped.
 */
class CUSBKeyboardRawAdapter
{
public:
	CUSBKeyboardRawAdapter (void)
	:	m_pKeyboard (0),
		m_bUsed (FALSE)
	{
	}

	void Attach (CUSBKeyboardDevice *pKeyboard)
	{
		m_pKeyboard = pKeyboard;
		m_bUsed = TRUE;
	}

	void Detach (void)
	{
		m_bUsed = FALSE;
	}

	void Invoke (unsigned char ucModifiers, const unsigned char RawKeys[6]) const
	{
		if (m_bUsed)
		{
			CUSBAtariKeyboard::KeyReport (m_pKeyboard, ucModifiers, RawKeys);
		}
	}

	/* The plain function pointer to hand to RegisterKeyStatusHandlerRaw(). */
	static TKeyStatusHandlerRaw *Handler (unsigned nSlot);

private:
	CUSBKeyboardDevice	*m_pKeyboard;
	boolean			 m_bUsed;
};

static CUSBKeyboardRawAdapter s_RawAdapter[USB_KEYBOARD_MAX_DEVICES];

TKeyStatusHandlerRaw *CUSBKeyboardRawAdapter::Handler (unsigned nSlot)
{
	/*
	 * One trampoline per slot, because the callback has no context to say
	 * which slot it came from.
	 */
	switch (nSlot)
	{
	case 0: return [] (unsigned char ucModifiers, const unsigned char RawKeys[6])
			{ s_RawAdapter[0].Invoke (ucModifiers, RawKeys); };
	case 1: return [] (unsigned char ucModifiers, const unsigned char RawKeys[6])
			{ s_RawAdapter[1].Invoke (ucModifiers, RawKeys); };
	case 2: return [] (unsigned char ucModifiers, const unsigned char RawKeys[6])
			{ s_RawAdapter[2].Invoke (ucModifiers, RawKeys); };
	case 3: return [] (unsigned char ucModifiers, const unsigned char RawKeys[6])
			{ s_RawAdapter[3].Invoke (ucModifiers, RawKeys); };

	default:
		return 0;
	}
}

void CUSBAtariKeyboard::KeyReport (CUSBKeyboardDevice *pKeyboard,
				   unsigned char ucModifiers,
				   const unsigned char RawKeys[6])
{
	/*
	 * Interrupt context: no logging, no allocation, no blocking, and no
	 * more work than the report needs.  Everything shared with the main
	 * thread is behind the spin lock.
	 */
	s_SpinLock.Acquire ();

	/*
	 * The callback carries only the keyboard pointer, so that is what the
	 * slot is looked up by.  A keyboard unplugged at this instant has
	 * already had its slot cleared, and then this report is simply dropped.
	 */
	unsigned nEvent = (unsigned) -1;
	for (unsigned i = 0; i < USB_KEYBOARD_MAX_DEVICES; i++)
	{
		if (s_Report[i].bActive && s_Report[i].pKeyboard == pKeyboard)
		{
			nEvent = i;
			break;
		}
	}

	if (nEvent != (unsigned) -1)
	{
		HandleReport (nEvent, ucModifiers, RawKeys);
	}

	s_SpinLock.Release ();
}

/*
 * Turns one report into Atari state and queues any menu press in it.  The
 * caller holds the spin lock.
 */
void CUSBAtariKeyboard::HandleReport (unsigned nEvent, unsigned char ucModifiers,
				      const unsigned char RawKeys[6])
{
	TReport &rReport = s_Report[nEvent];
	TEventState &rEvent = s_Event[nEvent];

	/*
	 * The interrupt handler is free of the object, so the two settings the
	 * menu depends on are read here and passed down.  Both are written by
	 * the main thread as it opens and closes the menu, so either value is
	 * consistent with what GetState() will report for this frame.
	 */
	boolean bNumpadAsCursor = s_bNumpadAsCursor;
	boolean bMenuActive = s_bMenuActive;

	TBitmap Now;
	Now.Clear ();
	Now.AddReportKeys (RawKeys);

	/* Keys that are down now but were not in the previous report. */
	TBitmap Newly;
	Newly.Clear ();
	for (unsigned i = 0; i < BitmapWords; i++)
	{
		Newly.Words[i] = Now.Words[i] & ~rReport.Held.Words[i];
	}

	/*
	 * F12 is edge triggered.  A keyboard repeats its whole report while a
	 * key is held, so only the report in which F12 first appears may ask
	 * for the menu.
	 */
	boolean bF12Down = Now.IsSet (USB_HID_F12) ? TRUE : FALSE;
	if (bF12Down && !rReport.bF12Down)
	{
		s_bMenuRequest = TRUE;
	}
	rReport.bF12Down = bF12Down;

	/* Queue a menu press for every key that just went down. */
	int nUsage;
	while ((nUsage = FirstSetBit (Newly)) != (int) ATARI_KEY_NONE)
	{
		/* Take this key out of the set so the loop makes progress. */
		Newly.Words[nUsage >> 6] &= ~((u64) 1 << (nUsage & 63));

		int nMenuKey = MenuKeyForUsage ((unsigned) nUsage, bNumpadAsCursor,
						bMenuActive);
		if (nMenuKey == ATARI_KEY_NONE)
		{
			continue;
		}

		unsigned &rnHead = s_nMenuHead[nEvent];
		unsigned &rnTail = s_nMenuTail[nEvent];

		/* One report can add several presses; drop any that overflow. */
		unsigned nNext = (rnTail + 1) % MenuQueueSize;
		if (nNext != rnHead)
		{
			s_MenuQueue[nEvent][rnTail] = nMenuKey;
			rnTail = nNext;
		}
	}

	/*
	 * The modifiers are lines of their own, not matrix keys, so they are
	 * read from the modifier byte, exactly as a real keyboard's shift and
	 * control lines would be.  Alt and the GUI keys have no Atari meaning.
	 */
	rEvent.bActive = TRUE;
	rEvent.bShift = ((ucModifiers & (LSHIFT | RSHIFT)) != 0) ? TRUE : FALSE;
	rEvent.bControl = ((ucModifiers & (LCTRL | RCTRL)) != 0) ? TRUE : FALSE;

	/* The console keys have their own lines on an XL/XE as well. */
	rEvent.bStart = FALSE;
	rEvent.bSelect = FALSE;
	rEvent.bOption = FALSE;

	/*
	 * Only the keys that can be console keys are tested, so this stays a
	 * couple of tests in interrupt context rather than a scan of the map.
	 */
	static const unsigned ConsoleUsage[] = {
		USB_HID_START, USB_HID_SELECT, USB_HID_OPTION
	};
	for (unsigned i = 0; i < sizeof ConsoleUsage / sizeof ConsoleUsage[0]; i++)
	{
		if (Now.IsSet (ConsoleUsage[i]))
		{
			ConsoleKeysForUsage (ConsoleUsage[i], &rEvent.bStart,
					     &rEvent.bSelect, &rEvent.bOption);
		}
	}

	/*
	 * One key code at a time, like the real matrix.  Which key wins when
	 * several are down follows the usage code order because the bit map is
	 * scanned from bit 0 upwards, so it does not depend on the order in
	 * which the keyboard happened to list the six slots of its report.
	 */
	rEvent.nKeyCode = ATARI_KEY_NONE;

	int nScan = FirstSetBit (Now);
	while (nScan != (int) ATARI_KEY_NONE)
	{
		/* Drop this key so the next iteration looks at the next one. */
		Now.Words[nScan >> 6] &= ~((u64) 1 << (nScan & 63));

		if (MenuKeyForUsage ((unsigned) nScan, bNumpadAsCursor, bMenuActive)
		    != ATARI_KEY_NONE)
		{
			/* The menu has this one, so the machine must not. */
			nScan = FirstSetBit (Now);

			continue;
		}

		ConsoleKeysForUsage ((unsigned) nScan, &rEvent.bStart,
				     &rEvent.bSelect, &rEvent.bOption);

		int nKeyCode = AtariKeyForUsage ((unsigned) nScan);
		if (nKeyCode == ATARI_KEY_NONE)
		{
			nScan = FirstSetBit (Now);

			continue;
		}

		/*
		 * Start, Select and Option are console keys, not key codes: they
		 * are already in their own fields, and must not also be reported
		 * as the key that is down.
		 */
		if (   nKeyCode == ATARI_KEY_START
		    || nKeyCode == ATARI_KEY_OPTION
		    || nKeyCode == ATARI_KEY_SELECT)
		{
			nScan = FirstSetBit (Now);

			continue;
		}

		rEvent.nKeyCode = nKeyCode;

		break;
	}

	rReport.Held = Now;

	/*
	 * The machine follows the keyboard that last sent a report.  This is a
	 * single word, written only from here, so reading it needs no lock.
	 */
	s_nLastEvent = nEvent;
}

/* ---------------------------------------------------------------------------
 * Construction and start-up
 * ------------------------------------------------------------------------- */

CUSBAtariKeyboard::CUSBAtariKeyboard (CUSBHCIDevice *pUSBHCI, unsigned nTimeoutMs)
:	m_bNumpadAsCursor (FALSE),
	m_bMenuActive (FALSE),
	m_pUSBHCI (pUSBHCI),
	m_nTimeoutMs (nTimeoutMs)
{
	for (unsigned i = 0; i < USB_KEYBOARD_MAX_DEVICES; i++)
	{
		s_Report[i].bActive = FALSE;
		s_Report[i].pKeyboard = 0;
		s_Report[i].bF12Down = FALSE;
		s_Report[i].Held.Clear ();

		s_Event[i].bActive = FALSE;
		s_Event[i].nKeyCode = ATARI_KEY_NONE;
		s_Event[i].bShift = FALSE;
		s_Event[i].bControl = FALSE;
		s_Event[i].bStart = FALSE;
		s_Event[i].bSelect = FALSE;
		s_Event[i].bOption = FALSE;

		s_nMenuHead[i] = 0;
		s_nMenuTail[i] = 0;
	}
}

CUSBAtariKeyboard::~CUSBAtariKeyboard (void)
{
	/*
	 * The keyboard devices belong to the USB stack, not to us, so there is
	 * nothing to delete here: dropping our slots is all that is needed.
	 */
	s_SpinLock.Acquire ();

	for (unsigned i = 0; i < USB_KEYBOARD_MAX_DEVICES; i++)
	{
		s_Report[i].bActive = FALSE;
		s_Report[i].pKeyboard = 0;
		s_Report[i].Held.Clear ();
		s_Event[i].bActive = FALSE;
		s_RawAdapter[i].Detach ();
	}

	s_SpinLock.Release ();
}

boolean CUSBAtariKeyboard::Initialize (void)
{
	if (m_pUSBHCI == 0)
	{
		return FALSE;
	}

	CTimer *pTimer = CTimer::Get ();

	/*
	 * CLOCKHZ ticks make one second, so the elapsed time in milliseconds is
	 * (ticks * 1000) / CLOCKHZ.
	 *
	 * This used to divide by "CTimer::GetClockTicks()", which is not a frequency
	 * at all but the free-running microsecond counter.  On a machine with a
	 * keyboard it never mattered, because the check below returns at once; on one
	 * without, the divisor grew with the machine's uptime and the elapsed time
	 * advanced a few milliseconds per second, so the wait never ended.  Booting a
	 * Zero 2 W with no keyboard attached therefore stopped here, after the
	 * "keyboard and controllers" stage and before the first frame -- a black
	 * screen with nothing logged, because nothing had failed.
	 */
	const unsigned nTicksPerSecond = CLOCKHZ;

	unsigned nStart = pTimer != 0 ? pTimer->GetTicks () : 0;
	unsigned nElapsed = 0;

	do
	{
		/*
		 * Scanned, not driven.
		 *
		 * Update() also calls CUSBHCIDevice::UpdatePlugAndPlay(), which drives
		 * enumeration.  During bring-up that is dangerous: on a Zero 2 W with a
		 * hub attached, Circle found a device it could not read a descriptor from
		 * ("usbdev0-1: Cannot get device descriptor (short)") and retried it
		 * without ever returning, so the machine stopped here -- after the
		 * "keyboard and controllers" stage and before the first frame, with
		 * nothing logged because nothing had failed.
		 *
		 * The USB stack has already enumerated what is attached by the time this
		 * runs, so scanning finds a keyboard without driving anything.  Driving
		 * plug-and-play is left to Update() from the frame loop, where a device
		 * that misbehaves can at worst cost frames rather than the boot.
		 */
		ScanDevices ();

		if (IsPresent ())
		{
			return TRUE;
		}

		if (pTimer == 0 || nTicksPerSecond == 0 || m_nTimeoutMs == 0)
		{
			break;
		}

		CTimer::SimpleMsDelay (USB_KEYBOARD_POLL_MS);

		/*
		 * The tick count wraps like the clock it counts, so the difference is
		 * taken as an unsigned subtraction.
		 */
		nElapsed = (pTimer->GetTicks () - nStart) * 1000 / nTicksPerSecond;
	}
	while (nElapsed < m_nTimeoutMs);

	/*
	 * No keyboard.  This is not an error: the emulator runs without one, and
	 * Update() keeps looking, so one plugged in later is picked up.
	 */
	return FALSE;
}

void CUSBAtariKeyboard::Update (void)
{
	if (m_pUSBHCI == 0)
	{
		return;
	}

	/*
	 * Only the scan happens here.  USB enumeration is driven once per frame by
	 * the kernel, so that it happens whether or not a USB keyboard is the
	 * selected keyboard -- with a GPIO keyboard selected, a joystick plugged in
	 * later still has to be noticed.
	 */
	ScanDevices ();
}

void CUSBAtariKeyboard::ScanDevices (void)
{
	for (unsigned nIndex = 1; nIndex <= USB_KEYBOARD_MAX_INDEX; nIndex++)
	{
		CDevice *pDevice =
			CDeviceNameService::Get ()->GetDevice ("ukbd", nIndex, FALSE);
		if (pDevice == 0)
		{
			continue;
		}

		CUSBKeyboardDevice *pKeyboard = (CUSBKeyboardDevice *) pDevice;

		/* Nothing to do if we are already listening to this one. */
		boolean bKnown = FALSE;

		s_SpinLock.Acquire ();
		for (unsigned i = 0; i < USB_KEYBOARD_MAX_DEVICES; i++)
		{
			if (s_Report[i].bActive && s_Report[i].pKeyboard == pKeyboard)
			{
				bKnown = TRUE;
				break;
			}
		}

		unsigned nFree = (unsigned) -1;
		if (!bKnown)
		{
			/*
			 * Claim a slot.  With more keyboards attached than there
			 * are slots the extra ones are ignored rather than
			 * mishandled: an untracked keyboard contributes no keys.
			 */
			for (unsigned i = 0; i < USB_KEYBOARD_MAX_DEVICES; i++)
			{
				if (!s_Report[i].bActive)
				{
					nFree = i;
					break;
				}
			}

			if (nFree != (unsigned) -1)
			{
				s_Report[nFree].bActive = TRUE;
				s_Report[nFree].pKeyboard = pKeyboard;
				s_Report[nFree].bF12Down = FALSE;
				s_Report[nFree].Held.Clear ();
			}
		}
		s_SpinLock.Release ();

		if (bKnown || nFree == (unsigned) -1)
		{
			continue;
		}

		/*
		 * A keyboard can be unplugged at any time, and the handler has
		 * to find our slot again: it gets the index, because the slot
		 * array is fixed and never moves.
		 */
		pKeyboard->RegisterRemovedHandler (KeyboardRemovedHandler,
						   (void *) (uintptr) nFree);

		/*
		 * Raw mode, not cooked: HID usage codes are needed to tell one
		 * physical key from another, and a report arrives on every
		 * change instead of as characters.  The adapter holds the
		 * keyboard so that its report can be attributed to it.
		 */
		TKeyStatusHandlerRaw *pHandler = CUSBKeyboardRawAdapter::Handler (nFree);
		if (pHandler == 0)
		{
			KeyboardRemovedHandler (pKeyboard, (void *) (uintptr) nFree);

			continue;
		}

		s_RawAdapter[nFree].Attach (pKeyboard);

		pKeyboard->RegisterKeyStatusHandlerRaw (pHandler, FALSE);
	}
}

void CUSBAtariKeyboard::KeyboardRemovedHandler (CDevice *pDevice, void *pContext)
{
	(void) pDevice;

	unsigned nEvent = (unsigned) (uintptr) pContext;
	if (nEvent >= USB_KEYBOARD_MAX_DEVICES)
	{
		return;
	}

	s_SpinLock.Acquire ();

	/*
	 * Release everything this keyboard was holding.  If it was the last one
	 * to report, the machine now sees a keyboard with no keys down, which
	 * is what unplugging one should look like.
	 */
	s_Report[nEvent].bActive = FALSE;
	s_Report[nEvent].pKeyboard = 0;
	s_Report[nEvent].bF12Down = FALSE;
	s_Report[nEvent].Held.Clear ();

	/*
	 * Stop the adapter too: the keyboard may still complete one report on
	 * its way out, and that report must not be attributed to whoever takes
	 * the slot next.
	 */
	s_RawAdapter[nEvent].Detach ();

	s_Event[nEvent].bActive = FALSE;
	s_Event[nEvent].nKeyCode = ATARI_KEY_NONE;
	s_Event[nEvent].bShift = FALSE;
	s_Event[nEvent].bControl = FALSE;
	s_Event[nEvent].bStart = FALSE;
	s_Event[nEvent].bSelect = FALSE;
	s_Event[nEvent].bOption = FALSE;

	s_nMenuHead[nEvent] = 0;
	s_nMenuTail[nEvent] = 0;

	if (s_nLastEvent == nEvent)
	{
		s_nLastEvent = (unsigned) -1;
	}

	s_SpinLock.Release ();
}

/* ---------------------------------------------------------------------------
 * The Atari keyboard state
 * ------------------------------------------------------------------------- */

void CUSBAtariKeyboard::GetState (atari_keyboard_state_t *pState)
{
	/*
	 * While the OSD menu is open, the cursor keys, Return, Escape and Space
	 * belong to the menu; reporting them here as well would move the
	 * emulated cursor at the same time as the menu selection.  With the
	 * menu open no key reaches the emulated machine at all, which also
	 * stops a stray press from being typed into the program running behind
	 * the menu.
	 */
	if (m_bMenuActive)
	{
		return;
	}

	int nKeyCode = ATARI_KEY_NONE;
	boolean bShift = FALSE;
	boolean bControl = FALSE;
	boolean bStart = FALSE;
	boolean bSelect = FALSE;
	boolean bOption = FALSE;

	s_SpinLock.Acquire ();

	unsigned nEvent = s_nLastEvent;
	if (nEvent < USB_KEYBOARD_MAX_DEVICES && s_Event[nEvent].bActive)
	{
		nKeyCode = s_Event[nEvent].nKeyCode;
		bShift = s_Event[nEvent].bShift;
		bControl = s_Event[nEvent].bControl;
		bStart = s_Event[nEvent].bStart;
		bSelect = s_Event[nEvent].bSelect;
		bOption = s_Event[nEvent].bOption;
	}

	s_SpinLock.Release ();

	pState->key_code = nKeyCode;
	pState->shift = bShift ? 1 : 0;
	pState->control = bControl ? 1 : 0;
	pState->start = bStart ? 1 : 0;
	pState->select = bSelect ? 1 : 0;
	pState->option = bOption ? 1 : 0;
}

boolean CUSBAtariKeyboard::IsPresent (void)
{
	boolean bPresent = FALSE;

	s_SpinLock.Acquire ();

	unsigned nEvent = s_nLastEvent;
	if (nEvent < USB_KEYBOARD_MAX_DEVICES && s_Report[nEvent].bActive)
	{
		bPresent = TRUE;
	}

	s_SpinLock.Release ();

	return bPresent;
}

/* ---------------------------------------------------------------------------
 * The OSD menu
 * ------------------------------------------------------------------------- */

boolean CUSBAtariKeyboard::ConsumeMenuRequest (void)
{
	/*
	 * The request is a latch set by the interrupt handler, so taking it
	 * under the lock means a request arriving while this runs is either
	 * taken now or left for the next call -- never lost, and never handed
	 * out twice.
	 */
	boolean bRequested = FALSE;

	s_SpinLock.Acquire ();

	if (s_bMenuRequest)
	{
		s_bMenuRequest = FALSE;
		bRequested = TRUE;
	}

	s_SpinLock.Release ();

	return bRequested;
}

int CUSBAtariKeyboard::ConsumeMenuKey (void)
{
	int nKeyCode = ATARI_KEY_NONE;

	s_SpinLock.Acquire ();

	/*
	 * The queue is per keyboard, and drained from the one that last
	 * reported: while the menu is open that is the one the user is typing
	 * on.
	 */
	unsigned nEvent = s_nLastEvent;
	if (nEvent < USB_KEYBOARD_MAX_DEVICES)
	{
		unsigned &rnHead = s_nMenuHead[nEvent];
		unsigned nTail = s_nMenuTail[nEvent];

		if (rnHead != nTail)
		{
			nKeyCode = s_MenuQueue[nEvent][rnHead];
			rnHead = (rnHead + 1) % MenuQueueSize;
		}
	}

	s_SpinLock.Release ();

	return nKeyCode;
}

/* ---------------------------------------------------------------------------
 * Settings from the OSD
 * ------------------------------------------------------------------------- */

void CUSBAtariKeyboard::SetNumpadAsCursor (boolean bEnabled)
{
	/* Read while a report is being handled; either value is harmless. */
	m_bNumpadAsCursor = bEnabled;
	s_bNumpadAsCursor = bEnabled;
}

void CUSBAtariKeyboard::SetMenuActive (boolean bActive)
{
	m_bMenuActive = bActive;

	/*
	 * GetState() reads the member; the interrupt handler, which has no
	 * object, reads the static mirror of it.
	 */
	s_bMenuActive = bActive;

	if (!bActive)
	{
		/*
		 * Anything the menu did not take is stale.  Dropping it means a
		 * key pressed just as the menu closed is not replayed into the
		 * emulated machine afterwards.
		 */
		s_SpinLock.Acquire ();

		for (unsigned i = 0; i < USB_KEYBOARD_MAX_DEVICES; i++)
		{
			s_nMenuHead[i] = 0;
			s_nMenuTail[i] = 0;
		}

		s_SpinLock.Release ();
	}
}

/* ---------------------------------------------------------------------------
 * The factory the kernel calls
 * ------------------------------------------------------------------------- */

/*
 * The object lives in function-local static storage, so there is no
 * allocation that could fail and the pointer stays valid for the life of the
 * program -- which is what the kernel expects, since it never deletes a
 * keyboard.
 *
 * A keyboard that turns up later is not a reason to return NULL: the kernel
 * treats NULL as fatal but a FALSE from Initialize() as a warning, so an
 * unusable or absent controller still gets an object that simply reports that
 * no keyboard is attached.
 *
 * The definition is wrapped in extern "C" to match the declaration in
 * keyboard.h, which is what the kernel calls through: the name must not be
 * mangled, or the link fails.
 */
extern "C" CAtariKeyboard *ATARI_CreateUSBKeyboard (CUSBHCIDevice *pUSBHCI)
{
	static CUSBAtariKeyboard Keyboard (pUSBHCI);

	return &Keyboard;
}
