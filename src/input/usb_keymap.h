/*
 * usb_keymap.h -- the USB keyboard remapping: usbkeys.cfg.
 *
 * The emulator's USB keyboard (Mode 1) is mapped by default from HID usage
 * codes onto the Atari keyboard.  This module lets a file on the SD card change
 * that mapping.
 *
 * The unit of a mapping is a **stroke**: one USB key plus the Shift and/or
 * Control state it is pressed with.  A stroke produces one **function** of the
 * Atari keyboard, which is a KBCODE with its own Shift/Control state -- the
 * `*` key produces `*`, `^` shifted and cursor right with Control, and each of
 * the three is a function that can be moved on its own.  docs/usbkeys.md is the
 * full description, and it is the contract this implementation follows.
 *
 * The map is written once, at start-up, and read from the USB interrupt
 * afterwards, so nothing here allocates or locks: the table is fixed size and
 * read-only once Load() returns.
 */

#ifndef ATARI_PI_USB_KEYMAP_H
#define ATARI_PI_USB_KEYMAP_H

#include <circle/types.h>

/* Most of the file that is read.  A mapping line is short; this is generous. */
#define USB_KEYMAP_MAX_FILE	4096

/* How many bindings and removed strokes the table holds. */
#define USB_KEYMAP_MAX_BINDINGS	64
#define USB_KEYMAP_MAX_REMOVED	64

/* The modifier bits of a stroke. */
#define USB_KEYMAP_SHIFT	0x01
#define USB_KEYMAP_CTRL		0x02

/*
 * Pseudo usage codes for the keys that arrive in the HID modifier byte rather
 * than in the six-key array.  They are above every real HID usage, so they can
 * never collide, and the modifier strokes are resolved through the same table
 * as ordinary keys -- that is what lets U(Shift) = A(A) and A(Shift) work.
 */
#define USB_KEYMAP_KEY_CTRL	0xE0
#define USB_KEYMAP_KEY_SHIFT	0xE1
#define USB_KEYMAP_KEY_ALT	0xE2

/* What a stroke produces. */
enum TUsbKeyAction
{
	UsbKeyActionNone = 0,		/* the stroke produces nothing */
	UsbKeyActionKey,		/* an Atari key code, in Code */
	UsbKeyActionStart,
	UsbKeyActionSelect,
	UsbKeyActionOption,
	UsbKeyActionShiftLine,
	UsbKeyActionCtrlLine
};

struct TUsbKeyResult
{
	u8	Action;			/* TUsbKeyAction */
	int	Code;			/* UsbKeyActionKey: KBCODE, modifiers included */
	/*
	 * TRUE when the binding names its stroke exactly, so the modifiers that
	 * selected it must not be added to Code again.  FALSE for a plain-key
	 * binding, where the held modifiers are still to be folded in.
	 */
	boolean	bExact;
};

class CUSBKeyMap
{
public:
	CUSBKeyMap (void);

	/* Forgets everything, so that the built-in mapping applies again. */
	void Reset (void);

	/*
	 * Parses TEXT and installs it.  PATH names the file in the log only.
	 * \return Number of mappings applied; problems are logged line by line.
	 */
	unsigned Load (const char *pPath, const char *pText);

	/* \return TRUE if the map can decide anything at all. */
	boolean IsActive (void) const	{ return m_nBindings > 0 || m_nRemoved > 0; }

	/*
	 * Resolves one stroke.
	 *
	 * \param nUsage HID usage code, or one of the USB_KEYMAP_KEY_* pseudo
	 *		 codes for Shift, Control and Alt.
	 * \param nMods  USB_KEYMAP_SHIFT / USB_KEYMAP_CTRL bits held.
	 * \param pResult Filled in when the answer is TRUE.
	 * \return TRUE if the map decided this stroke -- including "produces
	 *	   nothing"; FALSE when the built-in mapping applies.
	 */
	boolean Resolve (unsigned nUsage, unsigned nMods, TUsbKeyResult *pResult) const;

private:
	struct TBinding
	{
		u8		Usage;
		u8		Mods;
		u8		Action;
		int		Code;
		unsigned	nLine;		/* for the duplicate message */
	};

	struct TStroke
	{
		u8	Usage;
		u8	Mods;
	};

	/* Adds a binding, replacing one for the same stroke. */
	void AddBinding (unsigned nUsage, unsigned nMods, u8 Action, int Code,
			 unsigned nLine);
	/* The line an existing binding for this stroke came from, or 0. */
	unsigned FindBinding (unsigned nUsage, unsigned nMods) const;

	/* Marks one of the built-in bindings as removed by rule 4. */
	void RemoveStroke (unsigned nUsage, unsigned nMods);

	/* Parses one line.  \return TRUE when a binding was applied. */
	boolean ParseLine (char *pLine, unsigned nLine);

	TBinding	m_Binding[USB_KEYMAP_MAX_BINDINGS];
	unsigned	m_nBindings;

	TStroke		m_Removed[USB_KEYMAP_MAX_REMOVED];
	unsigned	m_nRemoved;
};

#endif
