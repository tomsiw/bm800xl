/*
 * usb_keymap.cpp -- parses usbkeys.cfg and resolves USB strokes to Atari
 * functions.  See usb_keymap.h for the model and docs/usbkeys.md for the file.
 *
 * The tables below are the whole of the emulator's built-in keyboard mapping,
 * as far as the remapping file is concerned: the USB key names it may use, and
 * the Atari function names with the stroke each one is bound to by default.
 * The default stroke is what rule 4 removes when a function is moved, so it has
 * to be here even though the built-in resolution itself lives in the keyboard
 * driver.
 */

#include "usb_keymap.h"

#include "atari_port.h"

#include <circle/logger.h>

#include <stdio.h>
#include <string.h>

static const char FromKeymap[] = "usbkeys";

/* ---------------------------------------------------------------------------
 * Small string helpers
 *
 * The C library here is newlib with no locale, so case folding is done by hand
 * rather than with toupper(): the file is ASCII and that is all that matters.
 * ------------------------------------------------------------------------- */

static char Upper (char c)
{
	return (c >= 'a' && c <= 'z') ? (char) (c - 'a' + 'A') : c;
}

static boolean EqualsIgnoreCase (const char *pA, const char *pB)
{
	while (*pA != '\0' && *pB != '\0')
	{
		if (Upper (*pA) != Upper (*pB))
		{
			return FALSE;
		}
		pA++;
		pB++;
	}

	return *pA == '\0' && *pB == '\0';
}

static boolean StartsWithIgnoreCase (const char *pText, const char *pPrefix)
{
	while (*pPrefix != '\0')
	{
		if (Upper (*pText) != Upper (*pPrefix))
		{
			return FALSE;
		}
		pText++;
		pPrefix++;
	}

	return TRUE;
}

static const char *SkipSpace (const char *p)
{
	while (*p == ' ' || *p == '\t')
	{
		p++;
	}

	return p;
}

/* Trims spaces, tabs and a CR from both ends, in place. */
static void Trim (char *p)
{
	char *pStart = p;
	while (*pStart == ' ' || *pStart == '\t')
	{
		pStart++;
	}

	if (pStart != p)
	{
		memmove (p, pStart, strlen (pStart) + 1);
	}

	char *pEnd = p + strlen (p);
	while (pEnd > p && (pEnd[-1] == ' ' || pEnd[-1] == '\t' || pEnd[-1] == '\r'))
	{
		pEnd--;
		*pEnd = '\0';
	}
}

/* ---------------------------------------------------------------------------
 * USB key names
 * ------------------------------------------------------------------------- */

struct TUsbName
{
	const char	*pName;
	u8		Usage;
};

static const TUsbName s_UsbNames[] =
{
	{ "SPACE",	0x2c },
	{ "RETURN",	0x28 },
	{ "ESCAPE",	0x29 },
	{ "BACKSPACE",	0x2a },
	{ "TAB",	0x2b },
	{ "CAPSLOCK",	0x39 },
	{ "NUMLOCK",	0x53 },
	{ "SCROLLLOCK",	0x47 },
	{ "INSERT",	0x49 },
	{ "DELETE",	0x4c },
	{ "HOME",	0x4a },
	{ "END",	0x4d },
	{ "PAGEUP",	0x4b },
	{ "PAGEDOWN",	0x4e },
	{ "UP",		0x52 },
	{ "DOWN",	0x51 },
	{ "LEFT",	0x50 },
	{ "RIGHT",	0x4f },
	{ "PRINTSCREEN", 0x46 },
	{ "PAUSE",	0x48 },
	{ "MENU",	0x65 },

	/* The function keys: F1 is 0x3a and they run in order to F12. */
	{ "F1",		0x3a },
	{ "F2",		0x3b },
	{ "F3",		0x3c },
	{ "F4",		0x3d },
	{ "F5",		0x3e },
	{ "F6",		0x3f },
	{ "F7",		0x40 },
	{ "F8",		0x41 },
	{ "F9",		0x42 },
	{ "F10",	0x43 },
	{ "F11",	0x44 },
	{ "F12",	0x45 },

	/* The keys that arrive in the modifier byte, not in the key array. */
	{ "CTRL",	USB_KEYMAP_KEY_CTRL },
	{ "SHIFT",	USB_KEYMAP_KEY_SHIFT },
	{ "ALT",	USB_KEYMAP_KEY_ALT },

	/* The keypad. */
	{ "NUM_0",	0x62 },
	{ "NUM_1",	0x59 },
	{ "NUM_2",	0x5a },
	{ "NUM_3",	0x5b },
	{ "NUM_4",	0x5c },
	{ "NUM_5",	0x5d },
	{ "NUM_6",	0x5e },
	{ "NUM_7",	0x5f },
	{ "NUM_8",	0x60 },
	{ "NUM_9",	0x61 },
	{ "NUM_DOT",	0x63 },
	{ "NUM_ENTER",	0x58 },
	{ "NUM_SLASH",	0x54 },
	{ "NUM_STAR",	0x55 },
	{ "NUM_MINUS",	0x56 },
	{ "NUM_PLUS",	0x57 },
};

/* \return The HID usage code for PNAME, or -1 if it is not a USB key name. */
static int UsbUsageForName (const char *pName)
{
	if (pName[0] == '\0')
	{
		return -1;
	}

	if (pName[1] == '\0')
	{
		char c = Upper (pName[0]);

		if (c >= 'A' && c <= 'Z')
		{
			return 0x04 + (c - 'A');
		}
		if (c >= '1' && c <= '9')
		{
			return 0x1e + (c - '1');
		}
		if (c == '0')
		{
			return 0x27;
		}

		/* The punctuation keys with an unshifted legend of their own. */
		switch (c)
		{
		case '\'':	return 0x34;
		case ',':	return 0x36;
		case '.':	return 0x37;
		case '/':	return 0x38;
		case ';':	return 0x33;
		case '[':	return 0x2f;
		case ']':	return 0x30;
		case '\\':	return 0x31;
		case '-':	return 0x2d;
		case '=':	return 0x2e;
		case '`':	return 0x35;

		default:	break;
		}

		return -1;
	}

	for (unsigned i = 0; i < sizeof s_UsbNames / sizeof s_UsbNames[0]; i++)
	{
		if (EqualsIgnoreCase (pName, s_UsbNames[i].pName))
		{
			return s_UsbNames[i].Usage;
		}
	}

	return -1;
}

/* ---------------------------------------------------------------------------
 * Atari function names
 * ------------------------------------------------------------------------- */

struct TFunction
{
	u8	Action;		/* TUsbKeyAction */
	int	Code;		/* UsbKeyActionKey */
	u8	DefUsage;	/* default stroke; 0 = none */
	u8	DefMods;
};

struct TFunctionName
{
	const char	*pName;
	u8		Action;
	int		Code;
	u8		DefUsage;
	u8		DefMods;
};

/*
 * Every named function.  Letters and digits are handled by code above, and the
 * Ctrl forms are derived, so they are not listed here.
 */
static const TFunctionName s_AtariNames[] =
{
	{ "SPACE",	UsbKeyActionKey, ATARI_KEY_SPACE,		0x2c, 0 },
	{ "RETURN",	UsbKeyActionKey, ATARI_KEY_RETURN,		0x28, 0 },
	{ "ESCAPE",	UsbKeyActionKey, ATARI_KEY_ESCAPE,		0x29, 0 },
	{ "BACKSPACE",	UsbKeyActionKey, ATARI_KEY_BACKSPACE,		0x2a, 0 },
	{ "TAB",	UsbKeyActionKey, ATARI_KEY_TAB,			0x2b, 0 },
	{ "HELP",	UsbKeyActionKey, ATARI_KEY_HELP,		0x3e, 0 },
	{ "START",	UsbKeyActionStart, 0,				0x3f, 0 },
	{ "SELECT",	UsbKeyActionSelect, 0,				0x40, 0 },
	{ "OPTION",	UsbKeyActionOption, 0,				0x41, 0 },
	{ "RESET",	UsbKeyActionKey, ATARI_KEY_WARMSTART,		0x43, 0 },
	{ "BREAK",	UsbKeyActionKey, ATARI_KEY_BREAK,		0x42, 0 },
	{ "INVERSE",	UsbKeyActionKey, ATARI_KEY_ATARI,		0x53, 0 },
	{ "CAPS",	UsbKeyActionKey, ATARI_KEY_CAPSLOCK,		0x39, 0 },
	{ "CAPSTOGGLE",	UsbKeyActionKey, ATARI_KEY_CAPSTOGGLE,		0,    0 },
	{ "F1",		UsbKeyActionKey, ATARI_KEY_F1,			0x3a, 0 },
	{ "F2",		UsbKeyActionKey, ATARI_KEY_F2,			0x3b, 0 },
	{ "F3",		UsbKeyActionKey, ATARI_KEY_F3,			0x3c, 0 },
	{ "F4",		UsbKeyActionKey, ATARI_KEY_F4,			0x3d, 0 },
	{ "UP",		UsbKeyActionKey, ATARI_KEY_UP,			0x52, 0 },
	{ "DOWN",	UsbKeyActionKey, ATARI_KEY_DOWN,		0x51, 0 },
	{ "LEFT",	UsbKeyActionKey, ATARI_KEY_LEFT,		0x50, 0 },
	{ "RIGHT",	UsbKeyActionKey, ATARI_KEY_RIGHT,		0x4f, 0 },
	{ "INSERT",	UsbKeyActionKey, ATARI_KEY_INSERT_CHAR,		0x49, 0 },
	{ "INSERTLINE",	UsbKeyActionKey, ATARI_KEY_INSERT_LINE,		0x49, USB_KEYMAP_SHIFT },
	{ "DELETE",	UsbKeyActionKey, ATARI_KEY_DELETE_CHAR,		0x4c, 0 },
	{ "DELETELINE",	UsbKeyActionKey, ATARI_KEY_DELETE_LINE,		0x4c, USB_KEYMAP_SHIFT },
	{ "CLEAR",	UsbKeyActionKey, ATARI_KEY_CLEAR,		0x29, USB_KEYMAP_SHIFT },
	{ "SETTAB",	UsbKeyActionKey, ATARI_KEY_SETTAB,		0x2b, USB_KEYMAP_SHIFT },
	{ "CLEARTAB",	UsbKeyActionKey, ATARI_KEY_CLRTAB,		0x2b, USB_KEYMAP_CTRL },
	{ "SHIFT",	UsbKeyActionShiftLine, 0,			USB_KEYMAP_KEY_SHIFT, 0 },
	{ "CTRL",	UsbKeyActionCtrlLine, 0,			USB_KEYMAP_KEY_CTRL, 0 },
	{ "NONE",	UsbKeyActionNone, 0,				0,    0 },

	/*
	 * The joystick actions.
	 *
	 * Nothing is bound to these by default and nothing can be: a direction works
	 * only when usbkeys.cfg names it, which is the point -- a keyboard that is
	 * also a joystick should not start steering the moment the interface is
	 * selected.  The keyboard driver ignores them, so a stroke bound to one
	 * types nothing; the keyboard joystick driver reads them from the same
	 * reports.  Code is (port << 8) | bit.
	 */
	{ "JOY1_UP",	UsbKeyActionJoystick, USB_KEYMAP_JOY_UP,			0, 0 },
	{ "JOY1_DOWN",	UsbKeyActionJoystick, USB_KEYMAP_JOY_DOWN,		0, 0 },
	{ "JOY1_LEFT",	UsbKeyActionJoystick, USB_KEYMAP_JOY_LEFT,		0, 0 },
	{ "JOY1_RIGHT",	UsbKeyActionJoystick, USB_KEYMAP_JOY_RIGHT,		0, 0 },
	{ "JOY1_FIRE",	UsbKeyActionJoystick, USB_KEYMAP_JOY_FIRE,		0, 0 },
	{ "JOY2_UP",	UsbKeyActionJoystick, (1 << USB_KEYMAP_JOY_PORT_SHIFT) | USB_KEYMAP_JOY_UP,	0, 0 },
	{ "JOY2_DOWN",	UsbKeyActionJoystick, (1 << USB_KEYMAP_JOY_PORT_SHIFT) | USB_KEYMAP_JOY_DOWN,	0, 0 },
	{ "JOY2_LEFT",	UsbKeyActionJoystick, (1 << USB_KEYMAP_JOY_PORT_SHIFT) | USB_KEYMAP_JOY_LEFT,	0, 0 },
	{ "JOY2_RIGHT",	UsbKeyActionJoystick, (1 << USB_KEYMAP_JOY_PORT_SHIFT) | USB_KEYMAP_JOY_RIGHT,	0, 0 },
	{ "JOY2_FIRE",	UsbKeyActionJoystick, (1 << USB_KEYMAP_JOY_PORT_SHIFT) | USB_KEYMAP_JOY_FIRE,	0, 0 },

	/* Punctuation with a position of its own. */
	{ ",",		UsbKeyActionKey, ATARI_KEY_COMMA,		0x36, 0 },
	{ ".",		UsbKeyActionKey, ATARI_KEY_FULLSTOP,		0x37, 0 },
	{ "/",		UsbKeyActionKey, ATARI_KEY_SLASH,		0x38, 0 },
	{ ";",		UsbKeyActionKey, ATARI_KEY_SEMICOLON,		0x33, 0 },
	{ "-",		UsbKeyActionKey, ATARI_KEY_MINUS,		0x2d, 0 },
	{ "=",		UsbKeyActionKey, ATARI_KEY_EQUAL,		0x2e, 0 },
	{ "*",		UsbKeyActionKey, ATARI_KEY_ASTERISK,		0x34, 0 },
	{ "+",		UsbKeyActionKey, ATARI_KEY_PLUS,		0x57, 0 },
	{ "<",		UsbKeyActionKey, ATARI_KEY_LESS,		0x36, USB_KEYMAP_SHIFT },
	{ ">",		UsbKeyActionKey, ATARI_KEY_GREATER,		0x37, USB_KEYMAP_SHIFT },
	{ "[",		UsbKeyActionKey, ATARI_KEY_BRACKETLEFT,		0x2f, 0 },
	{ "]",		UsbKeyActionKey, ATARI_KEY_BRACKETRIGHT,	0x30, 0 },
	{ "\\",		UsbKeyActionKey, ATARI_KEY_BACKSLASH,		0x31, 0 },

	/* Shifted characters. */
	{ "!",		UsbKeyActionKey, ATARI_KEY_EXCLAMATION,		0x1e, USB_KEYMAP_SHIFT },
	{ "\"",		UsbKeyActionKey, ATARI_KEY_DBLQUOTE,		0x1f, USB_KEYMAP_SHIFT },
	{ "#",		UsbKeyActionKey, ATARI_KEY_HASH,		0x20, USB_KEYMAP_SHIFT },
	{ "$",		UsbKeyActionKey, ATARI_KEY_DOLLAR,		0x21, USB_KEYMAP_SHIFT },
	{ "%",		UsbKeyActionKey, ATARI_KEY_PERCENT,		0x22, USB_KEYMAP_SHIFT },
	{ "&",		UsbKeyActionKey, ATARI_KEY_AMPERSAND,		0x23, USB_KEYMAP_SHIFT },
	{ "'",		UsbKeyActionKey, ATARI_KEY_QUOTE,		0x24, USB_KEYMAP_SHIFT },
	{ "@",		UsbKeyActionKey, ATARI_KEY_AT,			0x25, USB_KEYMAP_SHIFT },
	{ "_",		UsbKeyActionKey, ATARI_KEY_UNDERSCORE,		0x2d, USB_KEYMAP_SHIFT },
	{ ":",		UsbKeyActionKey, ATARI_KEY_COLON,		0x33, USB_KEYMAP_SHIFT },
	{ "?",		UsbKeyActionKey, ATARI_KEY_QUESTION,		0x38, USB_KEYMAP_SHIFT },
	{ "^",		UsbKeyActionKey, ATARI_KEY_CIRCUMFLEX,		0x34, USB_KEYMAP_SHIFT },
	{ "|",		UsbKeyActionKey, ATARI_KEY_BAR,			0x2e, USB_KEYMAP_SHIFT },
	{ "LPAREN",	UsbKeyActionKey, ATARI_KEY_PARENLEFT,		0x26, USB_KEYMAP_SHIFT },
	{ "RPAREN",	UsbKeyActionKey, ATARI_KEY_PARENRIGHT,		0x27, USB_KEYMAP_SHIFT },
};

/* The Atari KBCODE of the digit keys, which are not sequential either. */
static const u16 s_DigitCodes[10] =
{
	0x32, 0x1f, 0x1e, 0x1a, 0x18, 0x1d, 0x1b, 0x33, 0x35, 0x30
};

/*
 * Resolves one Atari function name.
 *
 * PB_LETTER is set when the name is a bare letter, because a letter owns both
 * of its cases: rule 4 then removes its shifted stroke as well.
 */
static boolean ResolveAtariName (const char *pName, TFunction *pFunction,
				 boolean *pbLetter)
{
	if (pbLetter != 0)
	{
		*pbLetter = FALSE;
	}

	if (pName[0] == '\0' || pFunction == 0)
	{
		return FALSE;
	}

	/* A(CTRL_x): the Control function of anything else that names a key. */
	if (StartsWithIgnoreCase (pName, "CTRL_") && pName[5] != '\0')
	{
		TFunction Base;

		if (!ResolveAtariName (pName + 5, &Base, 0)
		    || Base.Action != UsbKeyActionKey)
		{
			return FALSE;
		}

		pFunction->Action = UsbKeyActionKey;
		pFunction->Code = Base.Code | 0x80;
		pFunction->DefUsage = Base.DefUsage;
		pFunction->DefMods = Base.DefMods | USB_KEYMAP_CTRL;

		return TRUE;
	}

	if (pName[1] == '\0')
	{
		char c = Upper (pName[0]);

		if (c >= 'A' && c <= 'Z')
		{
			pFunction->Action = UsbKeyActionKey;
			pFunction->Code = (int) ATARI_KeyCodeForLetter (c);
			pFunction->DefUsage = (u8) (0x04 + (c - 'A'));
			pFunction->DefMods = 0;

			if (pbLetter != 0)
			{
				*pbLetter = TRUE;
			}

			return TRUE;
		}

		if (c >= '0' && c <= '9')
		{
			int nDigit = c - '0';

			pFunction->Action = UsbKeyActionKey;
			pFunction->Code = s_DigitCodes[nDigit];
			pFunction->DefUsage =
				(u8) (nDigit == 0 ? 0x27 : 0x1e + (nDigit - 1));
			pFunction->DefMods = 0;

			return TRUE;
		}
	}

	for (unsigned i = 0; i < sizeof s_AtariNames / sizeof s_AtariNames[0]; i++)
	{
		if (EqualsIgnoreCase (pName, s_AtariNames[i].pName))
		{
			pFunction->Action = s_AtariNames[i].Action;
			pFunction->Code = s_AtariNames[i].Code;
			pFunction->DefUsage = s_AtariNames[i].DefUsage;
			pFunction->DefMods = s_AtariNames[i].DefMods;

			return TRUE;
		}
	}

	return FALSE;
}

/* ---------------------------------------------------------------------------
 * The table
 * ------------------------------------------------------------------------- */

CUSBKeyMap::CUSBKeyMap (void)
{
	Reset ();
}

CUSBKeyMap &CUSBKeyMap::Get (void)
{
	/*
	 * One map, in static storage, constructed on first use.
	 *
	 * It has to be shared: usbkeys.cfg is one file describing strokes, and both
	 * the keyboard and the keyboard joystick resolve strokes through it.  It is
	 * written once by Load() at start-up and only read afterwards, so a
	 * function-local static costs nothing at run time.
	 */
	static CUSBKeyMap Instance;

	return Instance;
}

unsigned CUSBKeyMap::CountJoystickMappings (int nPort) const
{
	unsigned nCount = 0;

	for (unsigned i = 0; i < m_nBindings; i++)
	{
		if (m_Binding[i].Action != UsbKeyActionJoystick)
		{
			continue;
		}

		if ((m_Binding[i].Code >> USB_KEYMAP_JOY_PORT_SHIFT) == nPort)
		{
			nCount++;
		}
	}

	return nCount;
}

void CUSBKeyMap::Reset (void)
{
	m_nBindings = 0;
	m_nRemoved = 0;
}

unsigned CUSBKeyMap::FindBinding (unsigned nUsage, unsigned nMods) const
{
	for (unsigned i = 0; i < m_nBindings; i++)
	{
		if (   m_Binding[i].Usage == nUsage
		    && m_Binding[i].Mods == nMods)
		{
			return m_Binding[i].nLine;
		}
	}

	return 0;
}

void CUSBKeyMap::AddBinding (unsigned nUsage, unsigned nMods, u8 Action,
			     int Code, unsigned nLine)
{
	for (unsigned i = 0; i < m_nBindings; i++)
	{
		if (   m_Binding[i].Usage == nUsage
		    && m_Binding[i].Mods == nMods)
		{
			m_Binding[i].Action = Action;
			m_Binding[i].Code = Code;
			m_Binding[i].nLine = nLine;

			return;
		}
	}

	if (m_nBindings >= USB_KEYMAP_MAX_BINDINGS)
	{
		CLogger::Get ()->Write (FromKeymap, LogWarning,
					"line %u: no room for more than %u mappings "
					"-- line ignored",
					nLine, USB_KEYMAP_MAX_BINDINGS);
		return;
	}

	m_Binding[m_nBindings].Usage = (u8) nUsage;
	m_Binding[m_nBindings].Mods = (u8) nMods;
	m_Binding[m_nBindings].Action = Action;
	m_Binding[m_nBindings].Code = Code;
	m_Binding[m_nBindings].nLine = nLine;
	m_nBindings++;
}

void CUSBKeyMap::RemoveStroke (unsigned nUsage, unsigned nMods)
{
	for (unsigned i = 0; i < m_nRemoved; i++)
	{
		if (   m_Removed[i].Usage == nUsage
		    && m_Removed[i].Mods == nMods)
		{
			return;			/* already removed */
		}
	}

	if (m_nRemoved >= USB_KEYMAP_MAX_REMOVED)
	{
		return;				/* the table is full; keep the binding */
	}

	m_Removed[m_nRemoved].Usage = (u8) nUsage;
	m_Removed[m_nRemoved].Mods = (u8) nMods;
	m_nRemoved++;
}

boolean CUSBKeyMap::Resolve (unsigned nUsage, unsigned nMods,
			     TUsbKeyResult *pResult) const
{
	if (pResult == 0)
	{
		return FALSE;
	}

	/* An exact binding, which is also the one a modifier combination makes. */
	for (unsigned i = 0; i < m_nBindings; i++)
	{
		if (   m_Binding[i].Usage == nUsage
		    && m_Binding[i].Mods == nMods)
		{
			pResult->Action = m_Binding[i].Action;
			pResult->Code = m_Binding[i].Code;
			pResult->bExact = TRUE;

			return TRUE;
		}
	}

	/*
	 * A plain-key binding applies to the key with modifiers held too, which
	 * is what makes U(A) = A(B) swap the letters in both cases.  The held
	 * modifiers are added by the caller, hence bExact = FALSE.
	 */
	if (nMods != 0)
	{
		for (unsigned i = 0; i < m_nBindings; i++)
		{
			if (m_Binding[i].Usage == nUsage && m_Binding[i].Mods == 0)
			{
				pResult->Action = m_Binding[i].Action;
				pResult->Code = m_Binding[i].Code;
				pResult->bExact = FALSE;

				return TRUE;
			}
		}
	}

	/* A stroke whose built-in binding rule 4 took away produces nothing. */
	for (unsigned i = 0; i < m_nRemoved; i++)
	{
		if (   m_Removed[i].Usage == nUsage
		    && m_Removed[i].Mods == nMods)
		{
			pResult->Action = UsbKeyActionNone;
			pResult->Code = 0;
			pResult->bExact = TRUE;

			return TRUE;
		}
	}

	return FALSE;
}

/* ---------------------------------------------------------------------------
 * Parsing
 * ------------------------------------------------------------------------- */

/* Reads a name up to the closing ')' and trims it.  Advances *ppPast. */
static boolean ReadName (const char **ppPast, char *pName, unsigned nSize)
{
	const char *p = *ppPast;
	const char *pEnd = strchr (p, ')');
	unsigned nLength;

	if (pEnd == 0)
	{
		return FALSE;
	}

	nLength = (unsigned) (pEnd - p);
	if (nLength >= nSize)
	{
		nLength = nSize - 1;
	}

	memcpy (pName, p, nLength);
	pName[nLength] = '\0';
	Trim (pName);

	*ppPast = pEnd + 1;

	return TRUE;
}

/*
 * Parses one line.
 *
 * Grammar, with the spaces the format allows:
 *
 *     U ( [modifier + ...] key )  =  A ( function )
 *
 * A line that does not parse is reported with its number and skipped; the rest
 * of the file still applies.
 */
boolean CUSBKeyMap::ParseLine (char *pLine, unsigned nLine)
{
	/* Drop a comment, but not a '#' inside U(...) or A(...). */
	int nDepth = 0;
	for (char *p = pLine; *p != '\0'; p++)
	{
		if (*p == '(')
		{
			nDepth++;
		}
		else if (*p == ')')
		{
			if (nDepth > 0)
			{
				nDepth--;
			}
		}
		else if (*p == '#' && nDepth == 0)
		{
			*p = '\0';
			break;
		}
	}

	Trim (pLine);

	if (pLine[0] == '\0')
	{
		return FALSE;
	}

	const char *p = SkipSpace (pLine);

	if (Upper (*p) != 'U')
	{
		CLogger::Get ()->Write (FromKeymap, LogWarning,
					"line %u: expected U(...) on the left "
					"-- line ignored", nLine);
		return FALSE;
	}

	p = SkipSpace (p + 1);
	if (*p != '(')
	{
		CLogger::Get ()->Write (FromKeymap, LogWarning,
					"line %u: expected U(...) on the left "
					"-- line ignored", nLine);
		return FALSE;
	}

	char Left[64];
	p++;					/* skip the '(' */
	if (!ReadName (&p, Left, sizeof Left) || Left[0] == '\0')
	{
		CLogger::Get ()->Write (FromKeymap, LogWarning,
					"line %u: U(...) is not closed -- line ignored",
					nLine);
		return FALSE;
	}

	p = SkipSpace (p);
	if (*p != '=')
	{
		CLogger::Get ()->Write (FromKeymap, LogWarning,
					"line %u: expected '=' after U(...) "
					"-- line ignored", nLine);
		return FALSE;
	}

	p = SkipSpace (p + 1);
	if (Upper (*p) != 'A')
	{
		CLogger::Get ()->Write (FromKeymap, LogWarning,
					"line %u: expected A(...) on the right "
					"-- line ignored", nLine);
		return FALSE;
	}

	p = SkipSpace (p + 1);
	if (*p != '(')
	{
		CLogger::Get ()->Write (FromKeymap, LogWarning,
					"line %u: expected A(...) on the right "
					"-- line ignored", nLine);
		return FALSE;
	}

	char Right[64];
	p++;					/* skip the '(' */
	if (!ReadName (&p, Right, sizeof Right) || Right[0] == '\0')
	{
		CLogger::Get ()->Write (FromKeymap, LogWarning,
					"line %u: A(...) is not closed -- line ignored",
					nLine);
		return FALSE;
	}

	p = SkipSpace (p);
	if (*p != '\0')
	{
		CLogger::Get ()->Write (FromKeymap, LogWarning,
					"line %u: text after the mapping -- line ignored",
					nLine);
		return FALSE;
	}

	/* Split the left side into modifiers and the key. */
	unsigned nMods = 0;
	char *pToken = Left;
	char *pPlus;

	while ((pPlus = strchr (pToken, '+')) != 0)
	{
		*pPlus = '\0';
		Trim (pToken);

		unsigned nBit = 0;
		if (EqualsIgnoreCase (pToken, "SHIFT"))
		{
			nBit = USB_KEYMAP_SHIFT;
		}
		else if (EqualsIgnoreCase (pToken, "CTRL"))
		{
			nBit = USB_KEYMAP_CTRL;
		}
		else if (   EqualsIgnoreCase (pToken, "ALT")
			 || EqualsIgnoreCase (pToken, "ALTGR"))
		{
			CLogger::Get ()->Write (FromKeymap, LogWarning,
						"line %u: Alt is not a combination "
						"modifier -- line ignored", nLine);
			return FALSE;
		}
		else
		{
			CLogger::Get ()->Write (FromKeymap, LogWarning,
						"line %u: unknown modifier \"%s\" "
						"-- line ignored", nLine, pToken);
			return FALSE;
		}

		if ((nMods & nBit) != 0)
		{
			CLogger::Get ()->Write (FromKeymap, LogWarning,
						"line %u: modifier used twice "
						"-- line ignored", nLine);
			return FALSE;
		}

		nMods |= nBit;
		pToken = pPlus + 1;
	}

	Trim (pToken);

	int nUsage = UsbUsageForName (pToken);
	if (nUsage < 0)
	{
		CLogger::Get ()->Write (FromKeymap, LogWarning,
					"line %u: unknown USB key \"U(%s)\" "
					"-- line ignored", nLine, pToken);
		return FALSE;
	}

	if (nUsage == 0x45)			/* F12 */
	{
		CLogger::Get ()->Write (FromKeymap, LogWarning,
					"line %u: U(F12) is the menu key and cannot "
					"be remapped -- line ignored", nLine);
		return FALSE;
	}

	/*
	 * Shift, Ctrl and Alt arrive in the modifier byte and are resolved as
	 * strokes of their own; a combination on one of them could never match.
	 */
	if (nUsage >= 0xE0 && nMods != 0)
	{
		CLogger::Get ()->Write (FromKeymap, LogWarning,
					"line %u: a modifier key cannot be part of a "
					"combination -- line ignored", nLine);
		return FALSE;
	}

	TFunction Function;
	boolean bLetter = FALSE;

	if (!ResolveAtariName (Right, &Function, &bLetter))
	{
		CLogger::Get ()->Write (FromKeymap, LogWarning,
					"line %u: unknown Atari function \"A(%s)\" "
					"-- line ignored", nLine, Right);
		return FALSE;
	}

	unsigned nPrevious = FindBinding ((unsigned) nUsage, nMods);
	if (nPrevious != 0)
	{
		CLogger::Get ()->Write (FromKeymap, LogWarning,
					"line %u: U(%s) was already bound on line %u "
					"-- the later line wins",
					nLine, pToken, nPrevious);
	}

	AddBinding ((unsigned) nUsage, nMods, Function.Action, Function.Code,
		    nLine);

	/*
	 * Rule 4: the function has one home, and it has just moved.  A letter
	 * owns both of its cases; every other function has one stroke.
	 */
	if (Function.Action != UsbKeyActionNone && Function.DefUsage != 0)
	{
		RemoveStroke (Function.DefUsage, Function.DefMods);

		if (bLetter)
		{
			RemoveStroke (Function.DefUsage, USB_KEYMAP_SHIFT);
		}
	}

	return TRUE;
}

unsigned CUSBKeyMap::Load (const char *pPath, const char *pText)
{
	Reset ();

	if (pText == 0)
	{
		return 0;
	}

	unsigned nLines = 0;
	unsigned nApplied = 0;

	const char *p = pText;

	while (*p != '\0')
	{
		const char *pEnd = p;
		while (*pEnd != '\0' && *pEnd != '\n')
		{
			pEnd++;
		}

		char Line[128];
		unsigned nLength = (unsigned) (pEnd - p);

		if (nLength >= sizeof Line)
		{
			nLength = sizeof Line - 1;
		}

		memcpy (Line, p, nLength);
		Line[nLength] = '\0';

		p = (*pEnd == '\n') ? pEnd + 1 : pEnd;

		nLines++;

		if (ParseLine (Line, nLines))
		{
			nApplied++;
		}
	}

	CLogger::Get ()->Write (FromKeymap, LogWarning,
				"%s: %u lines, %u mappings",
				pPath != 0 ? pPath : "usbkeys.cfg",
				nLines, nApplied);

	return nApplied;
}
