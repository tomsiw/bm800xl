/*
 * gpio_keyboard.cpp -- an Atari XL/XE keyboard on the GPIO header.
 *
 * The wiring, the electrical arrangement and the pin assignment are described
 * in gpio_keyboard.h, which is the user manual.  This file is the driver: it
 * drives the eight row lines one at a time, reads the eight column lines,
 * debounces what it finds, and turns it into the Atari keyboard state, the
 * Shift + Help menu request and the menu key presses.
 *
 * Why it is written the way it is:
 *
 *   * the scan is a tight loop with no timer calls, because it runs inside a
 *     16.7 ms frame and has to stay out of the way;
 *   * the pins are CGPIOPin members assigned in Initialize() rather than
 *     objects with static storage, so that a driver that is never initialized
 *     never touches the GPIO block at all;
 *   * the debouncing counts scans, not microseconds, because the caller polls
 *     once per frame and a counter is cheaper and more predictable than a
 *     timestamp per key.
 */
#include <circle/gpiopin.h>
#include <circle/timer.h>
#include <circle/types.h>

#if RASPPI >= 5
	#include <circle/southbridge.h>
#endif

#include "gpio_keyboard.h"

#include "atari_port.h"

/* ===========================================================================
 * Pin assignment
 *
 * These are Broadcom (BCM) GPIO numbers, which is what CGPIOPin takes -- not
 * physical header pin numbers.  The physical pin is in the comment so the two
 * cannot be mixed up while wiring; gpio_keyboard.h has the same table for the
 * manual.
 * ======================================================================== */

#define GPIO_KEYBOARD_BCM_ROW0		5	/* physical  29 */
#define GPIO_KEYBOARD_BCM_ROW1		6	/* physical  31 */
#define GPIO_KEYBOARD_BCM_ROW2		13	/* physical  33 */
#define GPIO_KEYBOARD_BCM_ROW3		19	/* physical  35 */
#define GPIO_KEYBOARD_BCM_ROW4		26	/* physical  37 */
#define GPIO_KEYBOARD_BCM_ROW5		20	/* physical  38 */
#define GPIO_KEYBOARD_BCM_ROW6		21	/* physical  40 */
#define GPIO_KEYBOARD_BCM_ROW7		16	/* physical  36 */

#define GPIO_KEYBOARD_BCM_COL0		12	/* physical  32 */
#define GPIO_KEYBOARD_BCM_COL1		25	/* physical  22 */
#define GPIO_KEYBOARD_BCM_COL2		24	/* physical  18 */
#define GPIO_KEYBOARD_BCM_COL3		23	/* physical  16 */
#define GPIO_KEYBOARD_BCM_COL4		22	/* physical  15 */
#define GPIO_KEYBOARD_BCM_COL5		27	/* physical  13 */
#define GPIO_KEYBOARD_BCM_COL6		17	/* physical  11 */
#define GPIO_KEYBOARD_BCM_COL7		18	/* physical  12 */

#define GPIO_KEYBOARD_BCM_START		4	/* physical   7 */
#define GPIO_KEYBOARD_BCM_SELECT	8	/* physical  24 */
#define GPIO_KEYBOARD_BCM_OPTION	9	/* physical  21 */
#define GPIO_KEYBOARD_BCM_RESET		10	/* physical  19 */

/*
 * Index of each console key in the pin list and in the state bit masks.  The
 * order also decides the order the pins are read in, which is the order they
 * would appear in a scope trace.
 */
#define GPIO_KEYBOARD_CONSOL_START	0
#define GPIO_KEYBOARD_CONSOL_SELECT	1
#define GPIO_KEYBOARD_CONSOL_OPTION	2
#define GPIO_KEYBOARD_CONSOL_RESET	3

static const unsigned s_RowPin[GPIO_KEYBOARD_ROWS] =
{
	GPIO_KEYBOARD_BCM_ROW0,
	GPIO_KEYBOARD_BCM_ROW1,
	GPIO_KEYBOARD_BCM_ROW2,
	GPIO_KEYBOARD_BCM_ROW3,
	GPIO_KEYBOARD_BCM_ROW4,
	GPIO_KEYBOARD_BCM_ROW5,
	GPIO_KEYBOARD_BCM_ROW6,
	GPIO_KEYBOARD_BCM_ROW7
};

static const unsigned s_ColPin[GPIO_KEYBOARD_COLS] =
{
	GPIO_KEYBOARD_BCM_COL0,
	GPIO_KEYBOARD_BCM_COL1,
	GPIO_KEYBOARD_BCM_COL2,
	GPIO_KEYBOARD_BCM_COL3,
	GPIO_KEYBOARD_BCM_COL4,
	GPIO_KEYBOARD_BCM_COL5,
	GPIO_KEYBOARD_BCM_COL6,
	GPIO_KEYBOARD_BCM_COL7
};

static const unsigned s_ConsolPin[GPIO_KEYBOARD_CONSOL_KEYS] =
{
	GPIO_KEYBOARD_BCM_START,
	GPIO_KEYBOARD_BCM_SELECT,
	GPIO_KEYBOARD_BCM_OPTION,
	GPIO_KEYBOARD_BCM_RESET
};

/* ===========================================================================
 * Tunables
 * ======================================================================== */

/*
 * How many consecutive scans have to agree before a key's state is reported.
 *
 * Two, which is one more than "trust the sample we just took": a single bad
 * sample -- a bounce, or a column that is still charging through its pull-up
 * after a contact opened -- is thrown away, and the press is reported on the
 * next scan instead.
 *
 * The scans are at least GPIO_KEYBOARD_SCAN_INTERVAL_US apart, so two of them
 * mean at least 10 ms of unchanged state; at the usual one scan per emulated
 * frame that is 17 ms (NTSC) to 20 ms (PAL) of unchanged state.  Both are far
 * longer than the 1..5 ms a keyboard switch bounces for, and reporting a change
 * one scan late adds at most one frame of latency, which is what the emulated
 * machine's own keyboard handling costs anyway.
 */
#define GPIO_KEYBOARD_DEBOUNCE_SCANS	2

/*
 * Minimum time between two scans, in microseconds.
 *
 * keyboard.h polls the driver once per emulated frame, but ConsumeMenuRequest()
 * and ConsumeMenuKey() are polled from the same loop and would otherwise scan
 * again microseconds after GetState() did.  Spacing the scans keeps the debounce
 * count meaningful, and it is also what makes it harmless for the kernel to
 * call the menu entry points any number of times per frame.
 *
 * 10 ms is chosen so that it is longer than any switch bounce: a change that has
 * survived GPIO_KEYBOARD_DEBOUNCE_SCANS scans has then been stable for at least
 * 10 ms no matter how the caller polls.
 */
#define GPIO_KEYBOARD_SCAN_INTERVAL_US	10000

/*
 * Iterations of the settling loop after a row line is driven LOW (see Settle()
 * below).  About a microsecond at 1 GHz, which is ample for a contact that
 * closes in well under 20 ns and is paid for eight times per scan.
 */
#define GPIO_KEYBOARD_SETTLE_LOOPS	256

/*
 * The key code the RESET switch reports.
 *
 * src/port/atari_port.h has no ATARI_KEY_* define for it, so the value is
 * repeated here with the symbol it comes from: AKEY_WARMSTART (-2) in
 * external/atari800/src/akey.h.  That is what upstream's own ports send for the
 * Reset key (src/sdl/input.c returns AKEY_COLDSTART only with Shift held), and
 * it is what the emulation core acts on: src/input.c watches for the negative
 * codes and does not put them into KBCODE.  AKEY_COLDSTART (-3) would power
 * cycle the emulated machine instead, which the Atari's RESET key does not do.
 */
static const int ATARI_RESET_KEY_CODE = -2;	/* AKEY_WARMSTART, akey.h */

/*
 * Matrix positions that have no key on a real Atari.  Six intersections are
 * free -- R1C1, R3C1, R4C4, R5C1, R6C1 and R7C3, i.e. KBCODE 0x09, 0x19, 0x24,
 * 0x29, 0x31 and 0x3b -- and three of them carry the signals that leave a real
 * keyboard outside the matrix (see gpio_keyboard.h):
 *
 *	SHIFT	R3C1	KBCODE 0x19	ROW3 (GPIO 19) - COL1 (GPIO 25)
 *	CONTROL	R4C4	KBCODE 0x24	ROW4 (GPIO 26) - COL4 (GPIO 22)
 *	BREAK	R7C3	KBCODE 0x3b	ROW7 (GPIO 16) - COL3 (GPIO 23)
 *
 * The three were picked because they share neither a row nor a column, so a
 * wire on the wrong pin shows up immediately, and R1C1, R5C1 and R6C1 stay free
 * for later use.
 */
#define GPIO_KEYBOARD_SHIFT_ROW		3
#define GPIO_KEYBOARD_SHIFT_COL		1
#define GPIO_KEYBOARD_CONTROL_ROW	4
#define GPIO_KEYBOARD_CONTROL_COL	4
#define GPIO_KEYBOARD_BREAK_ROW		7
#define GPIO_KEYBOARD_BREAK_COL		3

/* Position of HELP, which together with SHIFT opens the menu. */
#define GPIO_KEYBOARD_HELP_ROW		2
#define GPIO_KEYBOARD_HELP_COL		1

/*
 * Two matrix positions have no ATARI_KEY_* define in src/port/atari_port.h, so
 * they are named here after the akey.h symbols they come from:
 *
 *   0x36 is AKEY_LESS, the "<" key.  The port header calls the same value
 *        ATARI_KEY_CLEAR, because Shift + "<" is Clear on an Atari; that name
 *        would be wrong for the unshifted key, which is what this cell is.
 *   0x37 is AKEY_GREATER, the ">" key.
 *
 * Both are (row << 3) | column like every other KBCODE: 0x36 is ROW6/COL6 and
 * 0x37 is ROW6/COL7.
 */
static const int ATARI_KEY_CODE_LESS = 0x36;		/* AKEY_LESS, akey.h */
static const int ATARI_KEY_CODE_GREATER = 0x37;		/* AKEY_GREATER, akey.h */

/* ===========================================================================
 * The key matrix
 * ======================================================================== */

/*
 * Each entry is the Atari key at (row, column); ATARI_KEY_NONE means the
 * intersection has no key.  Letters are stored as the character itself and are
 * turned into their KBCODE by KeyCodeForCell() below, so the letter mapping
 * exists only in ATARI_KeyCodeForLetter().
 *
 * The KBCODE of a key is (row << 3) | column.  Two cells are worth a second
 * look, because the table reads as if it had more empty intersections than it
 * has:
 *
 *   * ROW1/COL6 is the Atari "-" key, KBCODE 0x0e.  It only looks empty because
 *     "-" is also the character this table uses for "no key".
 *   * ROW7/COL4 is CAPS, and the CAPS key's KBCODE is 0x3c, i.e.
 *     ATARI_KEY_CAPSTOGGLE.  ATARI_KEY_CAPSLOCK (0x7c) is 0x3c with the Shift
 *     bit set, which is what a real Atari sends for Shift + CAPS.
 *
 * The reserved SHIFT, CONTROL and BREAK positions are ATARI_KEY_NONE here: they
 * are not keys but the three extra switches described in gpio_keyboard.h.
 */
static const int s_KeyMatrix[GPIO_KEYBOARD_ROWS][GPIO_KEYBOARD_COLS] =
{
	/* ROW0: L J ; F1 F2 K + * */
	{
		'L', 'J', ATARI_KEY_SEMICOLON, ATARI_KEY_F1,
		ATARI_KEY_F2, 'K', ATARI_KEY_PLUS, ATARI_KEY_ASTERISK
	},
	/* ROW1: O - P U Return I - = (COL1 unused, COL6 is the Atari "-") */
	{
		'O', ATARI_KEY_NONE, 'P', 'U',
		ATARI_KEY_RETURN, 'I', ATARI_KEY_MINUS, ATARI_KEY_EQUAL
	},
	/* ROW2: V Help C F3 F4 B X Z */
	{
		'V', ATARI_KEY_HELP, 'C', ATARI_KEY_F3,
		ATARI_KEY_F4, 'B', 'X', 'Z'
	},
	/* ROW3: 4 SHIFT 3 6 Esc 5 2 1 (COL1 is the reserved SHIFT position) */
	{
		ATARI_KEY_4, ATARI_KEY_NONE, ATARI_KEY_3, ATARI_KEY_6,
		ATARI_KEY_ESCAPE, ATARI_KEY_5, ATARI_KEY_2, ATARI_KEY_1
	},
	/* ROW4: , Space . N CONTROL M / Atari (COL4 is reserved CONTROL) */
	{
		ATARI_KEY_COMMA, ATARI_KEY_SPACE, ATARI_KEY_FULLSTOP, 'N',
		ATARI_KEY_NONE, 'M', ATARI_KEY_SLASH, ATARI_KEY_ATARI
	},
	/* ROW5: R - E Y Tab T W Q (COL1 unused) */
	{
		'R', ATARI_KEY_NONE, 'E', 'Y',
		ATARI_KEY_TAB, 'T', 'W', 'Q'
	},
	/* ROW6: 9 - 0 7 Backspace 8 < > (COL1 unused) */
	{
		ATARI_KEY_9, ATARI_KEY_NONE, ATARI_KEY_0, ATARI_KEY_7,
		ATARI_KEY_BACKSPACE, ATARI_KEY_8, ATARI_KEY_CODE_LESS,
		ATARI_KEY_CODE_GREATER
	},
	/* ROW7: F H D BREAK Caps G S A (COL3 is the reserved BREAK position) */
	{
		'F', 'H', 'D', ATARI_KEY_NONE,
		ATARI_KEY_CAPSTOGGLE, 'G', 'S', 'A'
	}
};

/* ===========================================================================
 * Helpers
 * ======================================================================== */

/*
 * Turns a matrix cell into an Atari key code.  A cell that holds a letter is
 * resolved through ATARI_KeyCodeForLetter(); the codes that come from
 * atari_port.h are all <= 0x3f, well below 'A', so the two kinds of cell cannot
 * be confused.
 */
static int KeyCodeForCell (int nCell)
{
	if (nCell >= 'A' && nCell <= 'Z')
	{
		return (int) ATARI_KeyCodeForLetter ((char) nCell);
	}

	return nCell;
}

/*
 * The key code a position contributes to the reported state.
 *
 * The modifier positions return ATARI_KEY_NONE so that they can never take
 * key_code -- they are reported as the shift and control flags instead -- and
 * BREAK is substituted, because it is a special code rather than a matrix key.
 */
static int KeyCodeForPosition (unsigned nRow, unsigned nCol)
{
	if (nRow == GPIO_KEYBOARD_BREAK_ROW && nCol == GPIO_KEYBOARD_BREAK_COL)
	{
		return ATARI_KEY_BREAK;
	}

	return KeyCodeForCell (s_KeyMatrix[nRow][nCol]);
}

/*
 * The menu key a position produces, or ATARI_KEY_NONE if the menu has no use
 * for it.  A press is classified by position, not by the reported key code,
 * because only one key can be reported per frame.
 *
 * The cursor keys are the Atari's own.  An XL keyboard wires its arrow keys in
 * parallel with the + * - = intersections and pulls the keyboard's CONTROL line
 * low while one of them is pressed, which is why the Atari cursor codes are
 * Control plus those keys: AKEY_LEFT 0x86 is CTRL|PLUS, AKEY_RIGHT 0x87 is
 * CTRL|ASTERISK, AKEY_UP 0x8e is CTRL|MINUS and AKEY_DOWN 0x8f is CTRL|EQUAL.
 * The menu treats those four intersections as the cursor keys whether or not the
 * CONTROL line is wired, so the menu is usable with a keyboard that only has the
 * matrix brought out.
 *
 * ESCAPE comes from the real Esc key (ROW3/COL4, KBCODE 0x1c) and from the
 * reserved BREAK switch, which is the closest thing an Atari keyboard has to
 * "leave this menu".
 */
static int MenuKeyForPosition (unsigned nRow, unsigned nCol)
{
	if (nRow == GPIO_KEYBOARD_BREAK_ROW && nCol == GPIO_KEYBOARD_BREAK_COL)
	{
		return ATARI_KEY_ESCAPE;
	}

	switch (s_KeyMatrix[nRow][nCol])
	{
	case ATARI_KEY_RETURN:
		return ATARI_KEY_RETURN;

	case ATARI_KEY_SPACE:
		return ATARI_KEY_SPACE;

	case ATARI_KEY_ESCAPE:
		return ATARI_KEY_ESCAPE;

	case ATARI_KEY_PLUS:
		return ATARI_KEY_LEFT;

	case ATARI_KEY_ASTERISK:
		return ATARI_KEY_RIGHT;

	case ATARI_KEY_MINUS:
		return ATARI_KEY_UP;

	case ATARI_KEY_EQUAL:
		return ATARI_KEY_DOWN;

	/*
	 * The Atari keyboard's DELETE function is Control+Back Space, and the menu
	 * cannot see modifiers here, so the plain key stands in for it: while the
	 * menu is open it is the menu's "remove", and while it is closed the key
	 * still types a back space into the machine.
	 */
	case ATARI_KEY_BACKSPACE:
		return ATARI_KEY_DELETE_CHAR;

	default:
		return ATARI_KEY_NONE;
	}
}

/*
 * Settling time after a row line has been driven LOW.
 *
 * A contact that closes pulls its column down through the contact resistance
 * (47R..120R on a real Atari keyboard) into a few tens of picofarads, which is
 * a time constant below 20 ns: closing a switch needs no waiting at all.  The
 * slow direction is a contact that opens -- the pull-up then has to charge that
 * capacitance through 50k, which takes some 25 us -- so a column can still read
 * LOW for the rest of the scan in which its key was released and look like a
 * phantom press elsewhere in that column for that one scan.
 *
 * Waiting long enough for that would cost about 30 us per row, 240 us per scan,
 * and buy nothing: a change is only reported once
 * GPIO_KEYBOARD_DEBOUNCE_SCANS scans agree, and those are milliseconds apart, so
 * a one-scan transient is exactly what the debouncing exists to discard.  A few
 * hundred iterations of an empty loop is about a microsecond at 1 GHz, which
 * covers the fast direction with plenty of margin and costs nothing.
 */
static void Settle (void)
{
	for (volatile unsigned nLoop = 0; nLoop < GPIO_KEYBOARD_SETTLE_LOOPS; nLoop++)
	{
	}
}

/* ===========================================================================
 * Construction
 * ======================================================================== */

CGPIOAtariKeyboard::CGPIOAtariKeyboard (void)
:	m_bInitialized (FALSE),
	m_RawConsol (0),
	m_nLastScanTicks (0),
	m_bMenuRequest (FALSE),
	m_bMenuComboLatch (FALSE),
	m_bMenuActive (FALSE),
	m_nMenuQueueHead (0),
	m_nMenuQueueCount (0)
{
	for (unsigned nRow = 0; nRow < GPIO_KEYBOARD_ROWS; nRow++)
	{
		m_RawMatrix[nRow] = 0;

		for (unsigned nCol = 0; nCol < GPIO_KEYBOARD_COLS; nCol++)
		{
			m_ChangeCount[nRow][nCol] = 0;
			m_bKeyPressed[nRow][nCol] = FALSE;
		}
	}

	for (unsigned nKey = 0; nKey < GPIO_KEYBOARD_CONSOL_KEYS; nKey++)
	{
		m_ConsolChangeCount[nKey] = 0;
		m_bConsolPressed[nKey] = FALSE;
	}

	for (unsigned nSlot = 0; nSlot < GPIO_KEYBOARD_MENU_QUEUE_SIZE; nSlot++)
	{
		m_MenuQueue[nSlot] = ATARI_KEY_NONE;
	}
}

CGPIOAtariKeyboard::~CGPIOAtariKeyboard (void)
{
}

/* ===========================================================================
 * Initialization
 * ======================================================================== */

boolean CGPIOAtariKeyboard::Initialize (void)
{
	if (m_bInitialized)
	{
		return TRUE;
	}

#if RASPPI >= 5
	/*
	 * On the Pi 5 the 40-pin header is not on the SoC: GPIO0..GPIO27 live
	 * behind the RP1 south bridge, and a pin cannot be configured until that
	 * bridge is up -- CGPIOPin::SetMode() asserts it.  Checking here turns a
	 * hang in a release build into a failure the kernel can report.
	 */
	if (!CSouthbridge::IsInitialized ())
	{
		return FALSE;
	}
#endif

	/*
	 * The rows are outputs.  CGPIOPin::SetMode() drives a fresh output LOW, so
	 * they are all briefly LOW at once, which is harmless (all the drivers
	 * agree) but not a state to scan in: each one is driven HIGH as it is set
	 * up, and the first scan only ever pulls one of them down again.
	 */
	for (unsigned nRow = 0; nRow < GPIO_KEYBOARD_ROWS; nRow++)
	{
		m_RowPin[nRow].AssignPin (s_RowPin[nRow]);
		m_RowPin[nRow].SetMode (GPIOModeOutput);
		m_RowPin[nRow].Write (HIGH);
	}

	/*
	 * The columns are inputs with the internal pull-up.  A column therefore
	 * reads HIGH when nothing is pressed, LOW when the switch at the selected
	 * row is closed, and never floats.
	 */
	for (unsigned nCol = 0; nCol < GPIO_KEYBOARD_COLS; nCol++)
	{
		m_ColPin[nCol].AssignPin (s_ColPin[nCol]);
		m_ColPin[nCol].SetMode (GPIOModeInputPullUp);
	}

	/*
	 * The console keys are switches to ground, so they need a pull-up as well:
	 * pressed reads LOW.
	 */
	for (unsigned nKey = 0; nKey < GPIO_KEYBOARD_CONSOL_KEYS; nKey++)
	{
		m_ConsolPin[nKey].AssignPin (s_ConsolPin[nKey]);
		m_ConsolPin[nKey].SetMode (GPIOModeInputPullUp);
	}

	ClearMenuQueue ();

	m_bInitialized = TRUE;

	return TRUE;
}

/* ===========================================================================
 * Scanning
 * ======================================================================== */

void CGPIOAtariKeyboard::Poll (void)
{
	if (!m_bInitialized)
	{
		return;
	}

	unsigned nNow = CTimer::GetClockTicks ();

	/*
	 * At most one scan per GPIO_KEYBOARD_SCAN_INTERVAL_US, so that the entry
	 * points the kernel polls in the same frame share one scan and the debounce
	 * count keeps meaning "scans this far apart".  The elapsed time is compared
	 * as a signed difference, which stays correct when the 1 MHz tick counter
	 * wraps (every ~71 minutes); the first call always scans.
	 */
	if (   m_nLastScanTicks != 0
	    && (int) (nNow - m_nLastScanTicks) < (int) GPIO_KEYBOARD_SCAN_INTERVAL_US)
	{
		return;
	}

	m_nLastScanTicks = nNow;

	ScanMatrix ();
	ScanConsole ();
	Debounce ();
	UpdateMenuRequest ();
}

void CGPIOAtariKeyboard::ScanMatrix (void)
{
	for (unsigned nRow = 0; nRow < GPIO_KEYBOARD_ROWS; nRow++)
	{
		/*
		 * One row LOW, the other seven HIGH -- they were left HIGH by the
		 * previous iteration, or by Initialize().  This is the whole point of
		 * the scan order: two LOW rows could be shorted together by two closed
		 * switches in the same column, and the diode-less matrix has nothing to
		 * stop the current that would flow.
		 */
		m_RowPin[nRow].Write (LOW);
		Settle ();

		u8 nMask = 0;
		for (unsigned nCol = 0; nCol < GPIO_KEYBOARD_COLS; nCol++)
		{
			if (m_ColPin[nCol].Read () == LOW)
			{
				nMask |= (u8) (1 << nCol);
			}
		}

		m_RowPin[nRow].Write (HIGH);

		m_RawMatrix[nRow] = nMask;
	}
}

void CGPIOAtariKeyboard::ScanConsole (void)
{
	u8 nMask = 0;

	for (unsigned nKey = 0; nKey < GPIO_KEYBOARD_CONSOL_KEYS; nKey++)
	{
		if (m_ConsolPin[nKey].Read () == LOW)
		{
			nMask |= (u8) (1 << nKey);
		}
	}

	m_RawConsol = nMask;
}

/* ===========================================================================
 * Debouncing
 * ======================================================================== */

/*
 * A key changes state only after GPIO_KEYBOARD_DEBOUNCE_SCANS consecutive scans
 * have disagreed with the state being reported; any agreeing scan resets its
 * counter.  The counters are per key, so one noisy switch cannot delay or
 * retrigger any other.
 */
void CGPIOAtariKeyboard::Debounce (void)
{
	for (unsigned nRow = 0; nRow < GPIO_KEYBOARD_ROWS; nRow++)
	{
		for (unsigned nCol = 0; nCol < GPIO_KEYBOARD_COLS; nCol++)
		{
			boolean bRaw = (m_RawMatrix[nRow] & (1 << nCol)) != 0;

			if (bRaw == m_bKeyPressed[nRow][nCol])
			{
				m_ChangeCount[nRow][nCol] = 0;
				continue;
			}

			if (++m_ChangeCount[nRow][nCol] < GPIO_KEYBOARD_DEBOUNCE_SCANS)
			{
				continue;
			}

			m_ChangeCount[nRow][nCol] = 0;
			m_bKeyPressed[nRow][nCol] = bRaw;

			if (bRaw)
			{
				/*
				 * A press is reported to the menu once, when it is first
				 * accepted.  The menu is edge triggered, so holding a key down
				 * must not repeat.
				 */
				int nMenuKey = MenuKeyForPosition (nRow, nCol);
				if (nMenuKey != ATARI_KEY_NONE)
				{
					PushMenuKey (nMenuKey);
				}
			}
		}
	}

	for (unsigned nKey = 0; nKey < GPIO_KEYBOARD_CONSOL_KEYS; nKey++)
	{
		boolean bRaw = (m_RawConsol & (1 << nKey)) != 0;

		if (bRaw == m_bConsolPressed[nKey])
		{
			m_ConsolChangeCount[nKey] = 0;
			continue;
		}

		if (++m_ConsolChangeCount[nKey] < GPIO_KEYBOARD_DEBOUNCE_SCANS)
		{
			continue;
		}

		m_ConsolChangeCount[nKey] = 0;
		m_bConsolPressed[nKey] = bRaw;
	}
}

void CGPIOAtariKeyboard::UpdateMenuRequest (void)
{
	boolean bCombo =   m_bKeyPressed[GPIO_KEYBOARD_SHIFT_ROW][GPIO_KEYBOARD_SHIFT_COL]
			&& m_bKeyPressed[GPIO_KEYBOARD_HELP_ROW][GPIO_KEYBOARD_HELP_COL];

	if (!bCombo)
	{
		/*
		 * Both keys are up again, so the combination is armed for the next
		 * time.  This is what makes the request edge triggered: holding Shift
		 * and Help down fires it once, and opening the menu twice needs the
		 * combination to be released in between.
		 */
		m_bMenuComboLatch = FALSE;
		return;
	}

	if (!m_bMenuComboLatch)
	{
		m_bMenuRequest = TRUE;
		m_bMenuComboLatch = TRUE;
	}
}

/* ===========================================================================
 * State reporting
 * ======================================================================== */

void CGPIOAtariKeyboard::GetState (atari_keyboard_state_t *pState)
{
	Poll ();

	/* The caller zeroed the struct, so only what is pressed is set here. */

	if (m_bConsolPressed[GPIO_KEYBOARD_CONSOL_START])
	{
		pState->start = 1;
	}
	if (m_bConsolPressed[GPIO_KEYBOARD_CONSOL_SELECT])
	{
		pState->select = 1;
	}
	if (m_bConsolPressed[GPIO_KEYBOARD_CONSOL_OPTION])
	{
		pState->option = 1;
	}

	/*
	 * Shift and Control are reported as flags and never take key_code, so a
	 * letter held with Shift arrives as that letter with shift set; applying
	 * the modifier is the caller's job (ATARI_ApplyModifiers() in
	 * src/port/atari_port.h exists for exactly that).
	 *
	 * If nothing but a modifier is held there is no base key to report and
	 * key_code is left alone.  A real Atari puts 0x40 (or 0x80) in KBCODE in
	 * that case; the flags carry the same information through INPUT_key_shift.
	 */
	if (m_bKeyPressed[GPIO_KEYBOARD_SHIFT_ROW][GPIO_KEYBOARD_SHIFT_COL])
	{
		pState->shift = 1;
	}
	if (m_bKeyPressed[GPIO_KEYBOARD_CONTROL_ROW][GPIO_KEYBOARD_CONTROL_COL])
	{
		pState->control = 1;
	}

	/*
	 * Only one key code fits in the state, so the precedence is fixed and
	 * documented:
	 *
	 *   1. RESET, because a held key must never be able to hide a reset;
	 *   2. BREAK, which means "stop what you are doing";
	 *   3. the first pressed intersection in scan order -- row 0 first, then
	 *      left to right -- skipping the keys an open menu has taken over.
	 *
	 * The order does not depend on which key was pressed last, so the same set
	 * of held keys always yields the same key code.  That matters because the
	 * emulation core compares each frame with the previous code to decide
	 * whether a key event happened (external/atari800/src/input.c).
	 */
	if (m_bConsolPressed[GPIO_KEYBOARD_CONSOL_RESET])
	{
		pState->key_code = ATARI_RESET_KEY_CODE;
		return;
	}

	if (   m_bKeyPressed[GPIO_KEYBOARD_BREAK_ROW][GPIO_KEYBOARD_BREAK_COL]
	    && !m_bMenuActive)
	{
		pState->key_code = ATARI_KEY_BREAK;
		return;
	}

	for (unsigned nRow = 0; nRow < GPIO_KEYBOARD_ROWS; nRow++)
	{
		for (unsigned nCol = 0; nCol < GPIO_KEYBOARD_COLS; nCol++)
		{
			if (!m_bKeyPressed[nRow][nCol])
			{
				continue;
			}

			int nKeyCode = KeyCodeForPosition (nRow, nCol);
			if (nKeyCode == ATARI_KEY_NONE)
			{
				continue;
			}

			/*
			 * While the menu is open its keys drive the menu, not the emulated
			 * machine; otherwise the cursor keys would move the menu selection
			 * and the emulated cursor at the same time.
			 */
			if (   m_bMenuActive
			    && MenuKeyForPosition (nRow, nCol) != ATARI_KEY_NONE)
			{
				continue;
			}

			pState->key_code = nKeyCode;

			return;
		}
	}
}

/* ===========================================================================
 * Menu requests
 * ======================================================================== */

boolean CGPIOAtariKeyboard::ConsumeMenuRequest (void)
{
	Poll ();

	if (!m_bMenuRequest)
	{
		return FALSE;
	}

	/* Consuming it is what stops a held Shift + Help from repeating. */
	m_bMenuRequest = FALSE;

	return TRUE;
}

int CGPIOAtariKeyboard::ConsumeMenuKey (void)
{
	Poll ();

	/*
	 * Presses are accumulated whatever the menu is doing, but only the open
	 * menu is given them: while it is closed these keys belong to the emulated
	 * machine.  SetMenuActive() empties the queue when the menu opens, so a
	 * press aimed at the Atari cannot come back as a menu command.
	 */
	if (!m_bMenuActive)
	{
		return ATARI_KEY_NONE;
	}

	return PopMenuKey ();
}

void CGPIOAtariKeyboard::SetMenuActive (boolean bActive)
{
	if (bActive && !m_bMenuActive)
	{
		ClearMenuQueue ();
	}

	m_bMenuActive = bActive;
}

void CGPIOAtariKeyboard::PushMenuKey (int nKeyCode)
{
	if (m_nMenuQueueCount >= GPIO_KEYBOARD_MENU_QUEUE_SIZE)
	{
		/*
		 * The kernel polls once per frame, so this needs more presses between
		 * two polls than a person can make.  Dropping the newest press keeps
		 * the older ones in order, which is what a menu wants.
		 */
		return;
	}

	unsigned nTail = (m_nMenuQueueHead + m_nMenuQueueCount)
			 % GPIO_KEYBOARD_MENU_QUEUE_SIZE;

	m_MenuQueue[nTail] = nKeyCode;
	m_nMenuQueueCount++;
}

int CGPIOAtariKeyboard::PopMenuKey (void)
{
	if (m_nMenuQueueCount == 0)
	{
		return ATARI_KEY_NONE;
	}

	int nKeyCode = m_MenuQueue[m_nMenuQueueHead];

	m_nMenuQueueHead = (m_nMenuQueueHead + 1) % GPIO_KEYBOARD_MENU_QUEUE_SIZE;
	m_nMenuQueueCount--;

	return nKeyCode;
}

void CGPIOAtariKeyboard::ClearMenuQueue (void)
{
	m_nMenuQueueHead = 0;
	m_nMenuQueueCount = 0;
}

/* ===========================================================================
 * The factory the kernel calls
 * ======================================================================== */

/*
 * The object lives in function-local static storage: nothing is allocated, so
 * nothing can fail, the pointer stays valid for the life of the program -- which
 * is what the kernel expects, since it never deletes a keyboard -- and a build
 * that does not select the GPIO keyboard never runs the constructor, so it never
 * touches the GPIO block either.  Circle supplies the __cxa_guard_*() functions
 * that the compiler emits for a function-local static (lib/cxa_guard.cpp).
 *
 * NULL is deliberately never returned.  The kernel treats a NULL keyboard as
 * fatal but a FALSE from Initialize() as a warning, so a Pi whose GPIO block
 * cannot be configured still gets an object that simply reports no input and
 * lets the emulator start.
 *
 * extern "C" matches the declaration in keyboard.h: the kernel calls this by
 * name, so the symbol must not be mangled.
 */
extern "C" CAtariKeyboard *ATARI_CreateGPIOKeyboard (void)
{
	static CGPIOAtariKeyboard Keyboard;

	return &Keyboard;
}
