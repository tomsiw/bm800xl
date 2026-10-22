# Joysticks and controllers

The emulator supports two kinds of Atari controller, and both can be used at the
same time because they share no hardware:

- a **USB gamepad**, plugged into any USB port;
- a **real 9-pin Atari joystick**, wired to the Raspberry Pi's GPIO header.

There are four controller inputs, which is what an Atari 800 had: each of its two
ports carried a second controller's lines alongside the first, for four-player
games. So a four-player game works with four USB gamepads, and a two-player game
works with two 9-pin joysticks in the two physical ports.

---

## USB gamepads

Plug one in at any time, before or after power-on. Up to four are watched, and a
controller attached later is noticed without restarting. Unplugging one releases
whatever it was holding, and a different controller plugged into the same slot is
mapped afresh — the two are not assumed to be the same device.

### What gets mapped

| Controller | Atari |
|---|---|
| D-pad, and the left stick if it has one | The four directions |
| **A** / Cross, **B** / Circle | Fire |
| **X** / Square, **Y** / Triangle | Fire |
| **L1** / LB, **R1** / RB | Fire |
| **L2** / LT, **R2** / RT | Fire |
| **Start** | Fire |

Every button that could reasonably mean "fire" is accepted, because the Atari has
exactly one button and there is nothing else to map them to. A player should not
have to discover which of four buttons the emulator happens to want.

That table is what a **controller Circle has a driver for** gets. There the button
numbers are the ones Circle's own enum defines, so they mean specific physical
buttons. A controller Circle does *not* recognise — a generic "USB joystick"
adapter, for instance — reports its buttons in whatever order its HID descriptor
lists them, and then a number says nothing about which button it is. Those are
matched one at a time, from the measurements recorded in
[project progress](progress.md#8-usb-gamepads--where-this-stands): on such a pad
the four directions and the two buttons that were measured work, and an
unidentified button does nothing rather than firing by accident.

On such a controller the D-pad and the left stick are read **together**: either
one steers, so a player can move between them without lifting off. A hat also
reports its diagonals, and a diagonal pushes both of the directions it lies
between.

### Analog sticks and the dead zone

An analog stick is read as a direction once it passes a quarter of its travel
from the centre. That is deliberately generous. A stick at rest is never exactly
centred and a controller's range is rarely symmetric, so a tighter threshold
would make the emulated joystick drift on its own — much worse than needing a
deliberate push.

### When a controller goes quiet

If no report arrives for a second, the controller is treated as released. Without
that, unplugging a controller mid-press would leave the emulated joystick stuck
in that direction, which is a state the player cannot correct without a reset.

### Unrecognised controllers

Circle understands the common controller families and normalises them. For a
controller it does not recognise, the raw buttons, axes and hat are still
reported, and this driver reads whichever of them the pad's own mapping says
carries each direction — which is why an unusual pad usually works even though
its name is unknown.

The log says which mapping a controller was given, once, when it is attached:

```
usbjoy: USB gamepad 1 attached (ven57e-2009): 21 buttons, 0 hats, 4 axes, map buttons (Circle driver)
```

### What the log says while you use it

Each change of direction or fire is written to the log once, at most one line per
tenth of a second, so a controller that is attached but does nothing can be told
from one that is not attached at all:

```
usbjoy: USB gamepad 1: up
usbjoy: USB gamepad 1: fire pressed (buttons 0x200)
usbjoy: USB gamepad 1: centred
usbjoy: USB gamepad 1 removed
```

The attachment line names the controller's USB ID and the rule that was chosen
for it. The button mask on the fire line is what the pad actually sent, which is
the quickest way to tell a mapping mistake from a dead controller.

---

## 9-pin joysticks on GPIO

An actual Atari joystick — or a Competition Pro, or a Tac-2 — wired to the
40-pin header. This is what the machine originally had, including the feel of a
real microswitched stick.

> **Read [the pin budget](#the-pin-budget) before wiring.** These two ports and
> the GPIO keyboard need the same pins, so they cannot both be used.

### How the port is wired

The Atari 9-pin controller port is about as simple as an interface gets. Inside
the joystick there are five switches — up, down, left, right and fire — and each
one, when closed, connects its pin on the port to the common ground on pin 8.
There is no matrix, no multiplexing and no power: the port is six wires, one per
switch plus ground.

| Atari 9-pin pin | Signal |
|---|---|
| 1 | Up |
| 2 | Down |
| 3 | Left |
| 4 | Right |
| 6 | Fire |
| 8 | Ground |
| 5 | Paddle potentiometer — **not connected** |
| 7 | +5 volts — **not connected** |
| 9 | Not connected |

> **Do not connect pin 7 to anything.** It carries five volts on the real
> machine, and a GPIO pin will not tolerate that. Only pins 1, 2, 3, 4, 6 and 8
> are used here.

Each switch goes between its GPIO pin and a ground pin. **No resistors are
needed**: the GPIO inputs use their internal pull-ups, so an open switch reads
HIGH and a closed one reads LOW.

### Pin assignment

These are the GPIO keyboard's pins, deliberately reused. The kernel refuses to
run a GPIO keyboard and these ports together, so a pin can only ever be claimed
by one of them.

**Port 1**

| Signal | BCM GPIO | Physical pin | Atari 9-pin pin |
|---|---|---|---|
| Up | GPIO 4 | 7 | 1 |
| Down | GPIO 5 | 29 | 2 |
| Left | GPIO 6 | 31 | 3 |
| Right | GPIO 8 | 24 | 4 |
| Fire | GPIO 9 | 21 | 6 |

**Port 2**

| Signal | BCM GPIO | Physical pin | Atari 9-pin pin |
|---|---|---|---|
| Up | GPIO 10 | 19 | 1 |
| Down | GPIO 11 | 17 | 2 |
| Left | GPIO 12 | 32 | 3 |
| Right | GPIO 13 | 33 | 4 |
| Fire | GPIO 16 | 36 | 6 |

**Ground** — any of pins 6, 9, 14, 20, 25, 30, 34, 39. Both ports' pin 8 go to
one of these.

### The pin budget

This is the constraint that shapes the whole design, so it is worth stating
plainly.

A 40-pin header does not have many usable GPIO pins: some are power and ground,
and GPIO 0 and 1 are reserved by the firmware. That leaves about 26. A GPIO
keyboard needs sixteen of them, plus four for the console switches — twenty in
all. Two 9-pin joystick ports need ten more.

**There are not enough pins for both.** So the two are mutually exclusive:

| Keyboard | GPIO joysticks | USB gamepads |
|---|---|---|
| USB keyboard (`KEYBOARD=0`) | **yes**, both ports | yes |
| GPIO keyboard (`KEYBOARD=1`) | no | yes |

The keyboard wins when it is selected, and the kernel logs why:

```
GPIO joysticks are off while the GPIO keyboard is selected: they need the same header pins
```

The joystick pins then follow the keyboard's own wiring, which is deliberate: one
ribbon can serve either a keyboard or a pair of joysticks, and there is nothing
to re-wire if you change your mind.

The check is enforced, not merely documented. Each port asks the kernel which
pins are already claimed before it configures anything, and a port whose pins are
taken fails to initialise with a message naming the pin — rather than a keyboard
reading nonsense and a joystick that does not work, with nothing to explain it.

### Both kinds at once

A USB gamepad and the GPIO ports can be used together, and when both are present
the USB gamepad takes the slot: it is the more informative of the two, and a port
silently overridden by a stick nobody is holding would be worse.

### What a GPIO joystick cannot do

There is no way to detect whether anything is plugged in. A joystick is a set of
switches with no electronics at all, so "present" can only mean "configured and
ready to be read". A port with nothing attached reads as centred with the button
open, which is exactly what an Atari with no joystick does — so an empty port
behaves correctly rather than needing to be detected.

There is also no debouncing, and none is wanted. The emulation core reads a
controller port once per frame and does its own edge detection on top, and a
joystick is read by a person's hand rather than by a fast scan. Debouncing here
would only make the controller feel unresponsive.

### Pi 5

On the Pi 5 the GPIO block lives behind the RP1 southbridge rather than being
part of the main SoC. The kernel checks that RP1 is up before configuring
anything; if it is not, each port reports that it did not initialise and the
emulator carries on without it. Every GPIO access on the Pi 5 is a round trip
over PCIe, but a controller read is five reads per frame, which is nothing.

---

## If a controller does not work

See [Troubleshooting](troubleshooting.md#a-joystick-or-gamepad-does-not-work).
