/*
 * gpio_keyboard.h -- an Atari XL/XE keyboard wired to the Raspberry Pi GPIO
 *                    header, as a CAtariKeyboard.
 *
 * This is "Mode 2" of src/input/keyboard.h: instead of a USB keyboard, the
 * keyboard of a real Atari is connected to the 40-pin header as a scan matrix
 * and read once per emulated frame.  It produces the same
 * atari_keyboard_state_t and the same OSD menu requests as the USB source, so
 * nothing above it needs to know which keyboard is attached.
 *
 * The electrical arrangement
 * ==========================
 *
 * An Atari keyboard is an 8x8 matrix: eight select (row) lines, eight return
 * (column) lines and one switch at each intersection, with no diodes.  A closed
 * switch connects its row line to its column line.
 *
 *	ROW lines	outputs.  Exactly one is driven LOW at a time, the other
 *			seven are held HIGH, and the scanned row goes back to
 *			HIGH before the next one is selected.  Never driving two
 *			rows LOW at once is what makes a diode-less matrix safe:
 *			with keys closed in two different rows but the same
 *			column, two output drivers would otherwise pull against
 *			each other through the pair of closed switches.
 *
 *	COLUMN lines	inputs with the internal pull-up enabled.  A column reads
 *			LOW only while the selected row is LOW and the switch at
 *			(row, column) is closed; with every row HIGH the pull-up
 *			holds every column HIGH, so nothing is ever floating.
 *
 * Ghosting -- a third key making a fourth intersection look closed -- is
 * inherent to a keyboard without diodes and is not corrected here.  It is not a
 * deviation from the emulated machine either: a real Atari keyboard is wired
 * the same way, and its ROM lives with the same limitation.
 *
 * Pin assignment
 * ==============
 *
 * All on the 40-pin header, avoiding the pins the emulator needs for SD, UART
 * and audio:
 *
 *	Signal	BCM	Physical	Signal	BCM	Physical
 *	ROW 0	 5	 29		COL 0	12	 32
 *	ROW 1	 6	 31		COL 1	25	 22
 *	ROW 2	13	 33		COL 2	24	 18
 *	ROW 3	19	 35		COL 3	23	 16
 *	ROW 4	26	 37		COL 4	22	 15
 *	ROW 5	20	 38		COL 5	27	 13
 *	ROW 6	21	 40		COL 6	17	 11
 *	ROW 7	16	 36		COL 7	18	 12
 *
 *	START	 4	  7
 *	SELECT	 8	 24
 *	OPTION	 9	 21
 *	RESET	10	 19
 *	GND	--	6, 9, 14, 20, 25, 30, 34, 39 (any of them)
 *
 * The four console keys are not part of the matrix on a real Atari either.
 * They are wired as plain switches to ground on their own pins, configured as
 * inputs with pull-ups and therefore active LOW:
 *
 *	START		reported as state.start  = 1
 *	SELECT		reported as state.select = 1
 *	OPTION		reported as state.option = 1
 *	RESET		reported as the reset key code in state.key_code
 *
 * On an XL/XE, HELP and START are the same physical key; here they are separate
 * switches, so wire whichever one the keyboard has.  The USB driver maps F5/F6
 * onto the same two Atari keys, for consistency.
 *
 * SHIFT, CONTROL and BREAK
 * ========================
 *
 * On a real Atari, SHIFT and CONTROL do not leave the keyboard through the
 * matrix: they have their own two lines (called S and X) which POKEY turns into
 * bits 6 and 7 of KBCODE, and BREAK is a separate switch as well.  This driver
 * has only the sixteen matrix pins, so those three signals are wired to three
 * of the six intersections that the matrix leaves unused.  The three chosen
 * here share neither a row nor a column, so a mis-wired line is easy to spot,
 * and the remaining unused intersections (R1C1, R5C1, R6C1) stay free:
 *
 *	signal	position	KBCODE of that position	GPIO pins
 *	SHIFT	R3C1		0x19			ROW3 (19) - COL1 (25)
 *	CONTROL	R4C4		0x24			ROW4 (26) - COL4 (22)
 *	BREAK	R7C3		0x3b			ROW7 (16) - COL3 (23)
 *
 * Each is a switch between its row pin and its column pin, exactly like a
 * matrix key -- not a switch to ground, which would short a row driver.  A
 * keyboard whose SHIFT/CONTROL/BREAK lines are wired to the keyboard's common
 * ground has to be re-connected so that the switch shorts the two header pins.
 *
 * "Shift + Help" opens the OSD menu (this is the Atari counterpart of the USB
 * driver's F12).  HELP is matrix R2C1; SHIFT is the reserved position above.
 * The request is edge triggered -- the combination has to be released before it
 * can open the menu a second time.
 *
 * Debouncing and scan cost
 * ========================
 *
 * A mechanical switch bounces for a few milliseconds.  A key's raw state has to
 * be seen unchanged by GPIO_KEYBOARD_DEBOUNCE_SCANS consecutive scans before a
 * change is reported (gpio_keyboard.cpp), and scans are at least
 * GPIO_KEYBOARD_SCAN_INTERVAL_US apart, so a reported change has been stable
 * for at least 10 ms and usually for a whole emulated frame (16.7 ms on NTSC,
 * 20 ms on PAL).  A scan costs a few microseconds on a Pi 3 and, in the worst
 * case, on the order of a hundred microseconds on a Pi 5, where every GPIO
 * access is a round trip to the RP1 -- both are far below a frame.
 *
 * Raspberry Pi 5 / RP1
 * ====================
 *
 * * Every pin used here is GPIO0..GPIO27, which is the bank the 40-pin header
 *   lives in on the Pi 5 as well (RP1 bank 0).  Pins 28..53 are in other banks
 *   and are not wired to the header.
 * * The RP1 must be initialised before a pin can be configured; Initialize()
 *   checks that and returns FALSE instead of tripping the library's assert.
 * * Pull-up configuration goes through the RP1 pad registers and read latency
 *   is a PCIe round trip, so a scan is much slower than on a Pi 3 -- still
 *   nothing next to a frame.
 */
#ifndef ATARI_PI_GPIO_KEYBOARD_H
#define ATARI_PI_GPIO_KEYBOARD_H

#include <circle/gpiopin.h>
#include <circle/types.h>

#include "keyboard.h"

/* Sizes of the matrix and of the console key list. */
#define GPIO_KEYBOARD_ROWS		8
#define GPIO_KEYBOARD_COLS		8
#define GPIO_KEYBOARD_CONSOL_KEYS	4

/*
 * How many menu key presses can be buffered between two polls.  The kernel
 * polls once per frame, so this is far more than a person can produce.
 */
#define GPIO_KEYBOARD_MENU_QUEUE_SIZE	8

class CGPIOAtariKeyboard : public CAtariKeyboard
{
public:
	CGPIOAtariKeyboard (void);
	~CGPIOAtariKeyboard (void);

	/// Configures the row, column and console key pins.
	/// \return FALSE if the GPIO block cannot be used (see the Pi 5 notes
	///	    above); TRUE otherwise, including on a repeated call.
	/// \note Returns with every row line driven HIGH, so an unpowered or
	///	  disconnected keyboard leaves the columns pulled high.
	boolean Initialize (void);

	/* CAtariKeyboard */
	void GetState (atari_keyboard_state_t *pState);
	boolean ConsumeMenuRequest (void);
	int ConsumeMenuKey (void);
	void SetMenuActive (boolean bActive);
	boolean IsPresent (void)	{ return m_bInitialized; }

private:
	/// One hardware scan, unless the last one is younger than the minimum
	/// scan interval.
	void Poll (void);

	/// Drives the row lines one at a time and records which columns read LOW.
	void ScanMatrix (void);

	/// Reads the four console key pins into m_RawConsol.
	void ScanConsole (void);

	/// Advances the per-key stability counters and reports the changes.
	void Debounce (void);

	/// Re-arms and fires the Shift + Help menu request.
	void UpdateMenuRequest (void);

	void PushMenuKey (int nKeyCode);
	int PopMenuKey (void);
	void ClearMenuQueue (void);

private:
	boolean m_bInitialized;

	CGPIOPin m_RowPin[GPIO_KEYBOARD_ROWS];
	CGPIOPin m_ColPin[GPIO_KEYBOARD_COLS];
	CGPIOPin m_ConsolPin[GPIO_KEYBOARD_CONSOL_KEYS];

	// What the last scan saw: one bit per column, LOW means closed.
	u8 m_RawMatrix[GPIO_KEYBOARD_ROWS];
	u8 m_RawConsol;

	// Scans in which a key has disagreed with the reported state.
	u8 m_ChangeCount[GPIO_KEYBOARD_ROWS][GPIO_KEYBOARD_COLS];
	u8 m_ConsolChangeCount[GPIO_KEYBOARD_CONSOL_KEYS];

	// The debounced state, which is what is reported.
	boolean m_bKeyPressed[GPIO_KEYBOARD_ROWS][GPIO_KEYBOARD_COLS];
	boolean m_bConsolPressed[GPIO_KEYBOARD_CONSOL_KEYS];

	// Clock tick of the last scan, for the scan interval.
	unsigned m_nLastScanTicks;

	// "Shift + Help" request: pending flag and the latch that makes it edge
	// triggered.
	boolean m_bMenuRequest;
	boolean m_bMenuComboLatch;

	// Key presses the menu has not consumed yet.
	boolean m_bMenuActive;
	int m_MenuQueue[GPIO_KEYBOARD_MENU_QUEUE_SIZE];
	unsigned m_nMenuQueueHead;
	unsigned m_nMenuQueueCount;
};

#endif
