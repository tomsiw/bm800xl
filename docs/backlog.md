# Backlog

Planned work that is not implemented yet. Each item records enough design to
resume it without reconstructing the discussion.

---

## GPIO selector sharing: Atari keyboard + two joysticks + console keys

**Status:** waiting for a real Atari keyboard to test. Not implemented.

**Goal:** run the original Atari keyboard matrix, both 9-pin joystick ports and
the four console keys at the same time, using only GPIO pins and no I2C/SPI
expander.

### Why an intermediate layer seemed necessary

The current direct GPIO keyboard uses 20 pins: eight rows, eight columns and
four console keys. Two joystick ports use ten more, some overlapping the
keyboard's old pins. With GPIO 0/1 reserved for the HAT ID EEPROM, only 16
GPIOs remain after giving the joystick ports their requested pins:

```text
5, 6, 7, 8, 9, 10, 11, 12, 13, 16, 19, 20, 21, 24, 25, 26
```

An 8x8 matrix consumes all 16, leaving nothing for the four console keys.

### Mechanism borrowed from BMC64

BMC64's "real keyboard + real joysticks" configuration does not use an
expander. The joystick direction/fire pins are also keyboard-matrix lines, and
each joystick's common (DB9 pin 8) goes to a **selector GPIO** instead of
ground:

- While scanning the keyboard, the selectors are inputs with pull-ups, so the
  joystick commons float and the joystick switches do not disturb the matrix.
- To read a joystick, its selector is driven LOW, making it the common for that
  joystick; the shared matrix lines are read as inputs.
- The selector is then returned to input/pull-up.

BMC64 uses GPIO 7 and GPIO 21 as the two selectors. It has no separate console
keys, so the Atari adaptation below extends the idea to those four keys.

References:

- <https://github.com/randyrossi/bmc64>
- <https://github.com/randyrossi/bmc64/blob/master/src/kernel.cpp>
- <https://github.com/randyrossi/bmc64/blob/master/src/viceapp.h>
- <https://github.com/randyrossi/bmc64/blob/master/docs/USER_GUIDE.md>

### Proposed Atari pin assignment

Port 1's five pins become five matrix rows; Port 2's five pins become five
matrix columns; three free pins complete each group. Two further free pins are
the selectors.

**Rows**

| Matrix line | GPIO | Physical pin |
|---|---|---|
| ROW0 | GPIO 2 | 3 |
| ROW1 | GPIO 3 | 5 |
| ROW2 | GPIO 4 | 7 |
| ROW3 | GPIO 14 | 8 |
| ROW4 | GPIO 15 | 10 |
| ROW5 | GPIO 5 | 29 |
| ROW6 | GPIO 6 | 31 |
| ROW7 | GPIO 13 | 33 |

**Columns**

| Matrix line | GPIO | Physical pin |
|---|---|---|
| COL0 | GPIO 17 | 11 |
| COL1 | GPIO 27 | 13 |
| COL2 | GPIO 22 | 15 |
| COL3 | GPIO 18 | 12 |
| COL4 | GPIO 23 | 16 |
| COL5 | GPIO 19 | 35 |
| COL6 | GPIO 26 | 37 |
| COL7 | GPIO 20 | 38 |

**Selectors**

| Function | GPIO | Physical pin |
|---|---|---|
| J1 / console selector | GPIO 7 | 26 |
| J2 selector | GPIO 21 | 40 |

This uses exactly 18 GPIOs. GPIO 0/1 stay reserved. Free afterwards:
GPIO 8, 9, 10, 11, 12, 16, 24, 25.

### Joystick wiring

Port 1:

| Atari pin | Signal | GPIO |
|---|---|---|
| 1 | Up | 2 |
| 2 | Down | 3 |
| 3 | Left | 4 |
| 4 | Right | 14 |
| 6 | Fire | 15 |
| 8 | Common | GPIO 7 selector |

Port 2:

| Atari pin | Signal | GPIO |
|---|---|---|
| 1 | Up | 17 |
| 2 | Down | 27 |
| 3 | Left | 22 |
| 4 | Right | 18 |
| 6 | Fire | 23 |
| 8 | Common | GPIO 21 selector |

**The joystick commons must not be connected to Pi ground.** Atari pin 7 (+5 V)
also stays unconnected.

### Console keys

The four console switches are wired between a matrix line and the J1 selector,
not to ground:

| Console key | Matrix line | GPIO |
|---|---|---|
| START | ROW5 | 5 |
| SELECT | ROW6 | 6 |
| OPTION | ROW7 | 13 |
| RESET | COL5 | 19 |

The other side of each switch goes to GPIO 7. During the J1/console read GPIO 7
is driven LOW; during keyboard scanning it is input/pull-up, so the console
switches do not affect the matrix. None of these four lines is one of Port 1's
joystick direction pins.

### SHIFT / CONTROL / BREAK

These remain switches across matrix intersections:

| Signal | Position | Row GPIO | Column GPIO |
|---|---|---|---|
| SHIFT | R3C1 | 14 | 27 |
| CONTROL | R4C4 | 15 | 23 |
| BREAK | R7C3 | 13 | 18 |

### Software plan

1. Give the GPIO keyboard and joystick drivers a shared owner for the matrix
   pins and the two selectors, so they cannot change pin direction
   independently.
2. Keyboard scan:
   - both selectors input/pull-up;
   - rows outputs, one LOW at a time;
   - columns inputs;
   - update the key matrix as now.
3. J1/console read:
   - all matrix pins inputs with pull-ups;
   - GPIO 7 output LOW;
   - read Port 1's five pins and the four console lines;
   - GPIO 7 back to input/pull-up.
4. J2 read:
   - all matrix pins inputs with pull-ups;
   - GPIO 21 output LOW;
   - read Port 2's five pins;
   - GPIO 21 back to input/pull-up.
5. Keep a short settle time after changing a selector, as BMC64 does, rather
   than reading immediately.
6. Remove the current "GPIO keyboard and joysticks are mutually exclusive"
   refusal; the shared matrix makes both possible.
7. Update `docs/keyboard-gpio.md`, `docs/joystick.md` and `docs/using.md` with
   the new wiring and the pin-sharing rule; the existing joystick pin tables in
   `docs/joystick.md` already match the row/column split above.

### Acceptance criteria

- All Atari keyboard keys, SHIFT, CONTROL and BREAK work.
- START, SELECT, OPTION and RESET work.
- Both joystick ports work independently and at the same time.
- Typing and moving a joystick at the same time does not produce false keys.
- No use of GPIO 0/1; no connection to Atari pin 7 (+5 V).
- No intermediate IC.

### Open questions to resolve with hardware

- Selector settle time and whether a short delay is needed after switching.
- Ghosting or interaction when a console key and a keyboard key in the same
  electrical group are pressed together.
- Whether the J1/console extension behaves with the keyboard scan's row outputs
  and the console switches sharing the same lines.
- Real Atari keyboard ribbon pinout and switch behaviour versus the documented
  matrix.
