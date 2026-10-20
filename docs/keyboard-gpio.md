# Keyboard: a real Atari keyboard on GPIO (Mode 2)

An actual Atari XL/XE keyboard can be wired to the Raspberry Pi's 40-pin header,
giving every key the machine ever had — including SELECT, the 1200XL function
keys and the inverse-video key.

Set `KEYBOARD=1` in the settings file to use it.

> **This mode and the GPIO joystick ports cannot be used together.** They need
> the same header pins, and only one of the two can have them. Selecting this
> keyboard therefore disables the two 9-pin ports; see
> [Joysticks](joystick.md) for the pin budget and for which combination to
> choose. USB gamepads continue to work either way.
>
> It also conflicts with PWM audio on machines with no 3.5 mm jack, because
> GPIO 12 and 13 are a column and a row line here. See the note at the end.

## How the Atari keyboard works

The Atari keyboard is an 8×8 matrix with no diodes:

- the Pi drives **8 row lines** as outputs, one at a time, holding the selected
  row LOW and the other seven HIGH;
- it reads **8 column lines** configured as inputs with their internal pull-ups
  enabled;
- a LOW reading means the switch at that (row, column) intersection is closed.

Only ever one row is driven LOW. With no diodes in the matrix, driving two rows
at once would short two output drivers together through two closed switches.

The driver reads the whole matrix about every 10 ms, and requires a change to be
stable for two consecutive scans (about 10–20 ms) before believing it. That is
longer than any switch bounce and well within the 16.7 ms of a frame, so a key
press is reported at most one frame late.

## Pin assignment

All on the 40-pin header. The rows are grouped together and the columns are
grouped together so the wiring is easy to follow.

### Row lines (Pi outputs)

| Signal | BCM GPIO | Physical pin |
|---|---|---|
| ROW 0 | GPIO 5 | 29 |
| ROW 1 | GPIO 6 | 31 |
| ROW 2 | GPIO 13 | 33 |
| ROW 3 | GPIO 19 | 35 |
| ROW 4 | GPIO 26 | 37 |
| ROW 5 | GPIO 20 | 38 |
| ROW 6 | GPIO 21 | 40 |
| ROW 7 | GPIO 16 | 36 |

### Column lines (Pi inputs, pull-ups enabled)

| Signal | BCM GPIO | Physical pin |
|---|---|---|
| COL 0 | GPIO 12 | 32 |
| COL 1 | GPIO 25 | 22 |
| COL 2 | GPIO 24 | 18 |
| COL 3 | GPIO 23 | 16 |
| COL 4 | GPIO 22 | 15 |
| COL 5 | GPIO 27 | 13 |
| COL 6 | GPIO 17 | 11 |
| COL 7 | GPIO 18 | 12 |

### Console keys (separate switches to ground)

These four are not part of the matrix. Each is one side of a switch to **ground**,
configured as an input with a pull-up and active LOW.

| Signal | BCM GPIO | Physical pin | Reported as |
|---|---|---|---|
| START | GPIO 4 | 7 | Start |
| SELECT | GPIO 8 | 24 | Select |
| OPTION | GPIO 9 | 21 | Option |
| RESET | GPIO 10 | 19 | Warm reset |

### Ground

Any of pins 6, 9, 14, 20, 25, 30, 34, 39. The four console switches need it; the
matrix does not.

## The key matrix

Each cell is the Atari key at that row and column intersection. `-` means the
intersection carries no key on a standard Atari keyboard — six of them are used
for SHIFT, CONTROL and BREAK (see below).

```
        COL0    COL1    COL2    COL3    COL4      COL5      COL6    COL7
ROW0    L       J       ;       F1      F2        K         +       *
ROW1    O       -       P       U       RETURN    I         MINUS   =
ROW2    V       HELP    C       F3      F4        B         X       Z
ROW3    4       -       3       6       ESCAPE    5         2       1
ROW4    ,       SPACE   .       N       -         M         /       ATARI
ROW5    R       -       E       Y       TAB       T         W       Q
ROW6    9       -       0       7       BACKSPACE 8         <       >
ROW7    F       H       D       -       CAPS      G         S       A
```

`ATARI` is the inverse-video key. `CAPS` is the Atari Caps key, which is
`CAPSTOGGLE` in the emulation core — the key that produces `CAPSLOCK` is Shift
plus that key, exactly as on the machine.

### The three extra signals on unused intersections

SHIFT, CONTROL and BREAK are not matrix keys on real Atari hardware: the
keyboard brings them out on their own lines, and POKEY folds them into bits 6
and 7 of the key code. Since those lines are not part of the 8×8 matrix, each of
these three switches is wired **between the two header pins of a reserved
intersection** — not to ground.

| Signal | Matrix position | Row pin | Column pin | Physical pins | Reported as |
|---|---|---|---|---|---|
| SHIFT | R3C1 | GPIO 19 | GPIO 25 | 35 + 22 | Shift held |
| CONTROL | R4C4 | GPIO 26 | GPIO 22 | 37 + 15 | Control held |
| BREAK | R7C3 | GPIO 16 | GPIO 23 | 36 + 16 | BREAK (`ESCAPE` in the menu) |

Three intersections remain free for anything else you want to add: R1C1
(GPIO 6 + 25), R5C1 (GPIO 20 + 25) and R6C1 (GPIO 21 + 25).

> **Important if you are re-wiring a real Atari keyboard.** A real keyboard's
> SHIFT, CONTROL and BREAK switches are already connected to the keyboard's own
> connector pins, not to ground. To use them here they must be re-wired to short
> the two header pins of their reserved intersection. Do not connect them both
> to the existing wiring and to a reserved position.

## Opening the OSD menu

Hold **SHIFT and HELP** together. The menu opens on the transition, once, and
you must release both before it can open again.

Inside the menu, the cursor keys are the Atari keys the emulator is watching:

| Atari key | Matrix position | Menu action |
|---|---|---|
| `-` | ROW1/COL6 | Up |
| `=` | ROW1/COL7 | Down |
| `+` | ROW0/COL6 | Left |
| `*` | ROW0/COL7 | Right |
| RETURN | ROW1/COL4 | Select |
| BREAK | R7C3 (reserved) | Escape / back |
| SPACE | ROW4/COL1 | Select |

That strange-looking set is not arbitrary. On an Atari XL keyboard the cursor
arrows are wired in parallel with exactly those four intersections, which is why
the Atari's own cursor key codes are `CTRL` combined with them: `AKEY_UP` is
CTRL+MINUS, `AKEY_DOWN` is CTRL+EQUAL, `AKEY_LEFT` is CTRL+PLUS and `AKEY_RIGHT`
is CTRL+ASTERISK. The emulator asks for the cursor keys, so those are the keys
that produce them.

Unlike the USB driver, this one withholds **only** the keys the menu uses, so
you can still type on the emulated machine while the menu is on screen.

## When several keys are held

A real keyboard reports one key at a time, so the driver has to choose. The
order is fixed, which matters because the emulation core compares consecutive
frames to detect a new press:

1. **RESET**, so a held key can never hide a reset;
2. **BREAK**, which means "stop what you are doing";
3. the **first key found in scan order** — row 0 first, then left to right.

SHIFT and CONTROL are never chosen as the key: they only set the shift and
control state, so SHIFT+A is reported as `A` with shift held, exactly as the
hardware does.

## A note on GPIO 12 and 13

This mapping uses GPIO 12 (pin 32) as a column line and GPIO 13 (pin 33) as a
row line. Those are also the pins Circle's PWM audio output uses, so **audio and
a GPIO keyboard cannot be used together on a machine whose audio comes from the
PWM pins** — which is every model without a 3.5 mm jack (Pi Zero, Pi Zero 2 W,
Pi 3).

If you need both, change the two pin constants at the top of
`src/input/gpio_keyboard.cpp` to free pins and update this table. The two arrays
are named `s_RowPins` and `s_ColumnPins` and the console pins are alongside them.

## Pi 5 differences

On the Pi 5 the GPIO header is behind the RP1 southbridge rather than being part
of the main SoC. The driver checks that RP1 is up before configuring anything; if
it is not, `Initialize()` reports failure and the emulator continues without a
keyboard rather than misbehaving. Every GPIO access on the Pi 5 is a round trip
over PCIe, so a full matrix scan costs tens of microseconds rather than a few —
still a tiny fraction of a frame.

## If it does not work

See [Troubleshooting](troubleshooting.md#the-gpio-keyboard-does-not-work).
