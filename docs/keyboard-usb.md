# Keyboard: USB (Mode 1)

A USB keyboard plugged into any Raspberry Pi USB port acts as the Atari XL/XE
keyboard. This is the default (`KEYBOARD=0` in the settings file).

The driver reads the keyboard's **raw HID reports** rather than the translated
characters a normal OS would give you. That is what makes it possible to map
every key position exactly, and to notice Shift and Control as separate states
rather than as part of the character produced.

The mapping below is the built-in one. Any key can be moved with
[`usbkeys.cfg`](usbkeys.md), which also lists every Atari key that may be named.

## Special keys

| USB key | Atari key |
|---|---|
| **F5** | HELP |
| **F6** | START |
| **F7** | SELECT |
| **F8** | OPTION |
| **F9** | BREAK |
| **F10** | RESET (warm reset) |
| **F12** | **Opens the OSD menu** |
| Arrow keys | Atari cursor keys |
| Left Ctrl / Right Ctrl | CONTROL |
| Left Shift / Right Shift | SHIFT |
| Caps Lock | CAPS (the Atari `Caps` key) |
| Num Lock | INVERSE (the Atari key that turns text inverse video) |

F12 is deliberately *not* mapped to an Atari key: it belongs to the emulator.
Pressing and holding it opens the menu once, not repeatedly.

`RESET` and `BREAK` are not keys on a real Atari keyboard — they are buttons and
lines on the machine itself. The emulator treats F10 as a press of the RESET
button, which is a warm reset: the emulated machine resets but keeps its memory.
Use the menu for a cold reset. F9 raises BREAK, which most software reads as
"stop"; in BASIC it interrupts a running program.

Holding **F8** (OPTION) while the machine starts-up skips the disk boot and
BASIC, which is how you reach the built-in SELF TEST. With nothing held and no
disk in a drive, the machine starts in BASIC — as a real one does.

## Full mapping

Shift and Control are reported separately, exactly as on a real keyboard, so
Shift+A produces a capital A through the Atari's own keyboard handling rather
than being a different key code.

| USB key | Atari key |
|---|---|
| A – Z | The matching Atari letter |
| 1 2 3 4 5 6 7 8 9 0 | Atari `1`…`9`, `0` |
| Enter | RETURN |
| Escape | ESCAPE |
| Backspace | BACKSPACE |
| Tab | TAB |
| Space | SPACE |
| `-` `_` | MINUS |
| `=` `+` | EQUAL |
| `[` `{` | BRACKET LEFT |
| `]` `}` | BRACKET RIGHT |
| `\` `\|` | BACKSLASH |
| `;` `:` | SEMICOLON |
| `'` | ASTERISK; Shift gives **`^`** — the Atari's `*` key has no PC position |
| `,` | COMMA; Shift gives **`<`** |
| `.` | FULL STOP; Shift gives **`>`** |
| `/` `?` | SLASH |
| Caps Lock | CAPS |
| F1 – F4 | Atari F1 – F4 (1200XL only) |
| Insert | INSERT CHAR; Shift gives **INSERT LINE** |
| Delete | DELETE CHAR; Shift gives **DELETE LINE** |
| Keypad `/` `*` `-` `+` | SLASH, ASTERISK, MINUS, PLUS |
| Keypad 0 – 9 | Atari `0`…`9` |
| Keypad `.` | FULL STOP |
| Keypad Enter | RETURN |

With the numeric keypad mapped to the cursor keys (the default), keypad
8/2/4/6 also produce the Atari cursor keys. Turn that off in the settings file
with `NUMPAD_CURSOR=0`.

The digit and punctuation rows are mapped **by position**, not by the symbol
printed on the key. Pressing `2` sends the Atari `2` key and lets the Atari's
own Shift handling decide whether that produces `2` or `"`. This is what makes
the layout feel like the real machine rather than like a PC keyboard with
letters in the wrong places.

`<` and `>` are the exceptions, and they are deliberate. On the Atari those two
characters have matrix positions of their own, which no PC key maps to — and
without the exception there was **no way to type them at all**. Since a PC
keyboard prints `<` and `>` on Shift+`,` and Shift+`.`, those two combinations
produce `<` and `>` here. Nothing is lost: `[` and `]` keep their own dedicated
keys.

## Keys with no Atari equivalent

These USB keys are ignored, and report as nothing rather than as a wrong key:
`` ` `` and `~`, F11, Print Screen, Scroll Lock, Pause, Home, Page Up, End,
Page Down, keypad `=`, the Windows/Apps keys, Power, F13 – F24, and the media
and international keys.

## Atari keys you cannot reach

Only one Atari key code has no source on a PC keyboard:

`CAPS TOGGLE` — not a physical key on the machine either. It is an internal
action code, and the CAPS key itself is on Caps Lock.

Everything else is reachable. The less obvious ones:

| Atari function | USB keys |
|---|---|
| `<` `>` | Shift+`,` and Shift+`.` |
| `\|` | Shift+`=` (the Atari's own `=` key), or Shift+`\` |
| `_` `@` `:` `^` `?` | Shift+`-` `8` `;` `'` `/` — the Atari's own shifted row |
| `! " # $ % & ' ( )` | Shift+`1` … Shift+`0` |
| CLEAR | Shift+Escape |
| SET TAB / CLEAR TAB | Shift+Tab / Ctrl+Tab |
| INSERT LINE | Shift+Insert |
| DELETE LINE | Shift+Backspace, or Shift+Delete |
| INSERT CHAR / DELETE CHAR | Insert / Delete |
| INVERSE | Num Lock |
| HELP | F5 |
| BREAK | F9 |
| SELECT / START / OPTION | F7 / F6 / F8 |

## Hot plugging

The keyboard may be plugged in before or after power-on, and a keyboard plugged
in later is picked up. Start-up no longer waits for one: it scans the devices the
USB stack has already enumerated and then gets on with booting, so a missing
keyboard cannot delay the emulator reaching its first frame.

Up to four USB keyboards can be attached at once, and unplugging one releases
whatever keys it was holding — so an unplugged keyboard cannot leave a key stuck
down.

### A USB hub may not work on a Pi Zero 2 W

A keyboard connected **directly** works. Behind a USB hub on a Pi Zero 2 W it
may not be enumerated at all, and in that case it is simply invisible to the
emulator: no keys, and the log reports no keyboard found.

Circle's own `sample/08-usbkeyboard`, with none of this project's code involved,
behaves the same way on that board, so this is a limit of Circle's USB stack on
BCM2837 rather than anything the emulator does. Connect the keyboard directly, or
try a different hub.

Earlier builds could additionally **stop before the first frame** when such a hub
was attached, because USB enumeration was driven during start-up and a device that
failed to return a descriptor was retried forever. Bring-up now only scans; the
enumeration that can stall is left to the frame loop, where it costs frames
instead of the boot.

One practical consequence: **while the OSD menu is open, the USB keyboard sends
nothing to the emulated machine.** This is deliberate — without it, the arrow
keys used to move around the menu would also move the emulated cursor. Keys
pressed while the menu is open are therefore not seen by the Atari.

Only the keys the menu acts on are handed to it: the arrows, Return, Escape,
Space, and **Delete**, which ejects the selected image and answers the question
on the *Erase snapshot* screen. Everything else is ignored while the menu is
open.

## If it does not work

See [Troubleshooting](troubleshooting.md#the-usb-keyboard-does-not-work).
