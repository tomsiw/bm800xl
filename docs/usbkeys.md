# Remapping the USB keyboard: `usbkeys.cfg`

This document describes how the emulator's USB keyboard (Mode 1) can be
re-mapped, which keys may be named, and what the default mapping is.

> **Status:** this is the specification. The file is not read by the emulator
> yet; until it is, the mapping in [section 7](#7-the-default-mapping) is what
> the emulator does. Once implemented, removing the file restores exactly that
> behaviour.

---

## 1. Where the file goes

```
<system directory>/usbkeys.cfg
```

The system directory is `SYSTEM_DIR` from `settings.cfg`, normally:

```
SD:/ATARI/usbkeys.cfg
```

It is read **once, at start-up**, by the USB keyboard driver, after the SD card
is mounted and before the first frame. A mapping cannot be changed while the
machine runs: edit the file on a PC and restart the Pi.

The file affects every USB keyboard attached at any time, including one plugged
in later. It does **not** affect the GPIO keyboard (Mode 2), which is a scan
matrix of a real Atari keyboard and has nothing to remap.

The card must be FAT32, the same as the rest of `SD:/ATARI`. The file may use
LF or CRLF line endings; the emulator does not care which.

---

## 2. What the file does

Most Atari keys do more than one thing. The key marked `*` also produces `^`
with Shift held, and moves the cursor right with Control held — on the machine
that is one physical key with three *functions*, and the Atari OS tells them
apart by the Shift and Control lines. The same is true of the whole matrix: the
`-` key is `_` shifted and cursor up with Control, the `=` key is `|` shifted and
cursor down with Control, and so on. Section 6 has the whole picture.

`usbkeys.cfg` assigns **functions**, not keys. That is what makes it possible to
give, say, the USB arrow keys the three functions of the `*` key without
disturbing where `*` and `^` already are:

```
U(Right)       = A(*)
U(Shift+Right) = A(^)
U(Ctrl+Right)  = A(Right)
```

The rules:

1. **No file** — the built-in mapping is used, exactly as before.
2. **File present** — every line in it replaces or adds one binding. Lines for
   things you do not care about are not needed.
3. **A subset is enough** — anything not mentioned keeps its built-in binding.
4. **A function moves** — when a line gives an Atari function a new USB
   key or key combination, its built-in binding is removed. Only that function:
   the other functions of the same Atari key, and the other functions of the same
   USB key, stay exactly where they were.

Rule 4 is the point of the function-level view. `U(F1) = A(*)` takes `*` off the
apostrophe key; `^` is still on `U(Shift+')` and cursor right is still on
`U(Ctrl+')`, because those are different functions with their own bindings.

Two consequences worth stating because they are easy to get wrong:

- **A USB stroke has at most one function.** A stroke is a USB key plus the
  Shift/Control state it is pressed with — `U(Right)`, `U(Shift+Right)`,
  `U(Ctrl+Right)` are three strokes. A line replaces the function of exactly one
  stroke. The Atari function that used to live there is left without a binding
  unless another line gives it one.
- **The old stroke is left empty.** Moving `A(A)` to another key does not leave
  `U(A)` producing anything: the function has one binding and it has moved.

---

## 3. File format

One mapping per line:

```
U([<modifiers>+]<usb key>) = A(<atari function>)
```

- Whitespace around the names and around `=` is ignored.
- A `#` starts a comment that runs to the end of the line, unless the `#` is
  inside `U(...)` or `A(...)` — `#` is a character name on the Atari side
  (`A(#)`), so that has to keep working. Blank lines are ignored.
- Names are **case-insensitive**: `U(f5)`, `U(F5)`, `U(shift+A)` and `U(Shift+A)`
  are the same, and `A(a)` is the same key as `A(A)`.
- A line that cannot be understood is **skipped and reported in the boot log**;
  the rest of the file still applies. A broken line never stops the emulator.
- If the same stroke is bound more than once, the **last** line wins, and the
  earlier one is reported.

### Example

```
# My layout: letters swapped, Start and Select swapped, and the three
# functions of the Atari '*' key spread over the PC arrow keys.

U(A) = A(B)
U(B) = A(A)

U(F6)  = A(Select)      # Select used to be on F7
U(F7)  = A(Start)       # Start used to be on F6

U(Right)       = A(*)
U(Shift+Right) = A(^)
U(Ctrl+Right)  = A(Right)
```

---

## 4. USB key names — `U(...)`

The name inside `U(...)` identifies a physical key on the PC keyboard, not a
character, optionally preceded by the modifiers it is pressed with.

### Letters and digits

| Name | Key |
|---|---|
| `U(A)` … `U(Z)` | the letter keys |
| `U(0)` … `U(9)` | the digits on the main row (not the keypad) |

The keypad has its own names; see [the numpad](#the-numpad) below.

### Punctuation

| Name | Key |
|---|---|
| `U(')` | apostrophe / quote |
| `U(,)` | comma |
| `U(.)` | full stop |
| `U(/)` | slash |
| `U(;)` | semicolon |
| `U([)` | left bracket |
| `U(])` | right bracket |
| `U(\)` | backslash |
| `U(-)` | minus |
| `U(=)` | equals |
| `` U(`) `` | grave / backtick |

> **Names follow the legends on a US keyboard.** The emulator reads HID usage
> codes, so a name means the **key in that position**; on another layout the cap
> may be printed differently. Only unshifted legends are names: `!`, `@`, `(` and
> so on are Shift+digit on a US keyboard and are named by the digit key.

Several keys have no name and therefore no way to be mapped: the `#` key of
non-US layouts, the keypad `=`, the media keys and the Windows keys. Others have
a name but no default action — `Home`, `End`, `PageUp`, `PageDown`,
`PrintScreen`, `Pause`, `Menu`, `ScrollLock`, `Alt` and `F11` — so they do
nothing until a line gives them something to do.

### The numpad

| Name | Key |
|---|---|
| `U(NUM_0)` … `U(NUM_9)` | keypad digits |
| `U(NUM_DOT)` | keypad `.` |
| `U(NUM_ENTER)` | keypad Enter |
| `U(NUM_SLASH)` | keypad `/` |
| `U(NUM_STAR)` | keypad `*` |
| `U(NUM_MINUS)` | keypad `-` |
| `U(NUM_PLUS)` | keypad `+` |

`U(1)` and `U(NUM_1)` are different keys even though both produce the Atari `1`
by default. The `NUM_` form always means the keypad; without it, the main block.

### Named keys

| Name | Key |
|---|---|
| `U(Space)` | space bar |
| `U(Return)` | Return / Enter (main block) |
| `U(Escape)` | Escape |
| `U(Backspace)` | Backspace |
| `U(Tab)` | Tab |
| `U(CapsLock)` | Caps Lock |
| `U(NumLock)` | Num Lock |
| `U(ScrollLock)` | Scroll Lock |
| `U(Insert)` | Insert |
| `U(Delete)` | Delete |
| `U(Home)` | Home |
| `U(End)` | End |
| `U(PageUp)` | Page Up |
| `U(PageDown)` | Page Down |
| `U(Up)` `U(Down)` `U(Left)` `U(Right)` | the arrow keys |
| `U(F1)` … `U(F12)` | the function keys |
| `U(Shift)` | Shift, either one |
| `U(Ctrl)` | Control, either one |
| `U(Alt)` | Alt, either one |
| `U(PrintScreen)` `U(Pause)` `U(Menu)` | as named |

### Modifier combinations

A stroke may name **Shift** and **Ctrl** before the key, joined with `+`:

```
U(Shift+A)        U(Ctrl+1)        U(Shift+Ctrl+A)        U(Ctrl+Right)
```

- The order does not matter: `U(Ctrl+Shift+A)` is the same stroke.
- Only Shift and Control may appear here. `Alt`, the GUI keys and the media keys
  have no Atari lines and are not combination modifiers.
- A `+` is never a key name on its own — the keypad plus is `NUM_PLUS` — so the
  separator is unambiguous: `U(Shift+=)` is Shift and the `=` key, `U(Ctrl+-)`
  is Control and the minus key.
- A combination is an **exact** selector: `U(Shift+A)` fires only while Shift is
  held and Control is not, and it produces **exactly** the function on the right:
  the modifiers choose the line and are not added to the result again. This is
  what lets one USB key mean three different things.
- A line for a **plain** key — `U(A)` — is different: it rebinds the key, and the
  modifiers you hold are added to the function as usual, so `A(A)` bound to
  `U(B)` still types `B` with Shift down. See [section 9](#9-how-a-stroke-is-resolved).

### Keys that are reserved

`U(F12)` cannot be used: F12 belongs to the emulator (it opens the OSD menu) and
is never given to the Atari. A line naming it is rejected with a message.

`U(Shift)` and `U(Ctrl)` are ordinary names on the left of a mapping, but they
mean more than an ordinary key: while held they are the Atari Shift and Control
*lines*, not strokes. Remapping them is allowed — see
[section 5.4](#54-lines) — but it changes how the whole keyboard behaves.

`U(Alt)` is only a key: the Atari has no Alt line, so on its own it does nothing,
and it is useful only as a spare key to assign a function to.

---

## 5. Atari function names — `A(...)`

The name inside `A(...)` identifies one **function** of the Atari keyboard: the
code the machine receives, including the Shift or Control state that code
carries. The same Atari key has several functions, and each has its own name
(where it has a name at all):

| On the Atari | Name | What it is |
|---|---|---|
| `*` | `A(*)` | the `*` key, unmodified |
| `^` | `A(^)` | the same key with Shift |
| cursor right | `A(Right)` | the same key with Control |

Three names, one key, three bindings that move independently.

### 5.1 Keys

| Name | What it is |
|---|---|
| `A(A)` … `A(Z)` | the letter keys |
| `A(0)` … `A(9)` | the digits, unmodified |
| `A(Space)` | space |
| `A(Return)` | Return |
| `A(Escape)` | Escape |
| `A(Backspace)` | Backspace |
| `A(Tab)` | Tab |
| `A(,)` `A(.)` `A(/)` `A(;)` `A(-)` `A(=)` | the punctuation keys that have a position of their own |
| `A(*)` `A(+)` | the Atari `*` and `+` keys |
| `A(<)` `A(>)` | the Atari `<` and `>` keys |
| `A(Help)` | Help |
| `A(Start)` `A(Select)` `A(Option)` | the three console keys |
| `A(Reset)` | the Reset line: a warm start |
| `A(Break)` | Break |
| `A(Inverse)` | the ATARI / inverse-video key |
| `A(Caps)` | the Caps key |
| `A(CapsToggle)` | the internal Caps-toggle action |
| `A(F1)` … `A(F4)` | the 1200XL function keys |
| `A(Insert)` `A(Delete)` | Insert and Delete |
| `A(InsertLine)` `A(DeleteLine)` | Insert line and Delete line |
| `A(Clear)` | the Clear key |
| `A(SetTab)` `A(ClearTab)` | Set tab and Clear tab |

A **letter** name means the letter key: it types `a`, `A` with Shift, and
Control+letter is the separate `A(CTRL_A)` function. Every other name above is
one specific function, even where the machine reaches it with Shift or Control —
several of these keys are the shifted or Control function of another key, which
is what [section 6](#6-one-key-several-functions) is about.

`A(Reset)` and `A(Break)` are commands rather than key codes: the emulation core
acts on them directly, resetting the machine or raising the Break NMI.

### 5.2 Shifted characters

| Name | Character | On the Atari this is |
|---|---|---|
| `A(!)` | `!` | Shift+`1` |
| `A(")` | `"` | Shift+`2` |
| `A(#)` | `#` | Shift+`3` |
| `A($)` | `$` | Shift+`4` |
| `A(%)` | `%` | Shift+`5` |
| `A(&)` | `&` | Shift+`6` |
| `A(')` | `'` | Shift+`7` |
| `A(@)` | `@` | Shift+`8` |
| `A(LParen)` | `(` | Shift+`9` |
| `A(RParen)` | `)` | Shift+`0` |
| `A(_)` | `_` | Shift+`-` |
| `A(:)` | `:` | Shift+`;` |
| `A(?)` | `?` | Shift+`/` |
| `A(^)` | `^` | Shift+`*` |
| `A(|)` | `|` | Shift+`=` |
| `A([)` | `[` | Shift+`,` |
| `A(])` | `]` | Shift+`.` |
| `A(\)` | `\` | Shift+`+` |

These are functions in their own right: the Shift is part of the name. A key
bound to `A(')` types `'` whether or not Shift is held.

Parentheses have word names on this side because everything up to the first `)`
is the name: `A(LParen)` and `A(RParen)`.

### 5.3 Control functions

| Name | What it is |
|---|---|
| `A(CTRL_A)` … `A(CTRL_Z)` | Control+letter |
| `A(CTRL_0)` … `A(CTRL_9)` | Control+digit |
| `A(CTRL_<key>)` | Control+any other key, e.g. `A(CTRL_Slash)`, `A(CTRL_.)` |
| `A(Up)` `A(Down)` `A(Left)` `A(Right)` | the cursor keys |

`A(CTRL_A)` means **the keystroke Ctrl+A**, not "the A key, with Ctrl handled
elsewhere": the Control is part of the function, so a key bound to it types
control-A on its own.

The cursor keys are Control functions too, and this is the clearest example of
the Atari's multi-purpose keys: **Ctrl+`*` is cursor right, Ctrl+`+` is cursor
left, Ctrl+`-` is cursor up and Ctrl+`=` is cursor down**. The emulator binds them
to the PC arrow keys as well, which is a convenience; both routes exist.

So the default `U(Right) = A(Right)` means the PC right arrow sends
**Control+`*`** — the cursor-right code — not `*`. Giving `A(*)` or `A(^)` a new
key does not touch it, because they are different functions.

A letter's Control function is separate from the letter key: `A(A)` is the key,
`A(CTRL_A)` is Ctrl+A. Giving one a new key does not move the other.

### 5.4 Lines

`A(Shift)` and `A(Ctrl)` are the Atari Shift and Control **lines**, not
functions of a key. By default they are driven by `U(Shift)` and `U(Ctrl)`.

Two things follow from rule 4:

- Giving `A(Shift)` a different USB key means the USB Shift key no longer
  shifts. That is occasionally what you want (moving Shift to the Caps Lock key,
  say), but a mapping that removes Shift without providing a new one leaves the
  Atari unable to type capitals.
- They are lines rather than functions: a key mapped to `A(Shift)` sets the line
  while it is held, so at least two keys have to be down for it to matter.

### 5.5 `A(None)`

`A(None)` is not an Atari function. It means "this stroke does nothing", and it
is the way to take a stroke out of service without giving its function somewhere
else to live:

```
U(F11) = A(None)     # F11 was unassigned anyway; now it is explicit
U(Shift+1) = A(None) # Shift+1 no longer types !
```

---

## 6. One key, several functions

The Atari matrix gives most keys three functions: unmodified, shifted, and
Control. The emulator's built-in bindings follow the machine, with the PC arrow
keys and Ins/Del added as convenient homes for functions that have no obvious PC
key. This table is the map of that structure; the default bindings themselves are
in [section 7](#7-the-default-mapping).

| Atari key | Unmodified | Shifted | Control |
|---|---|---|---|
| `1` … `0` | `1` … `0` | `! " # $ % & ' @ ( )` | — |
| `-` | `-` | `_` | **cursor up** |
| `=` | `=` | `|` | **cursor down** |
| `+` | `+` | `\` | **cursor left** |
| `*` | `*` | `^` | **cursor right** |
| `;` | `;` | `:` | — |
| `/` | `/` | `?` | — |
| `,` | `,` | `[` | — |
| `.` | `.` | `]` | — |
| `<` | `<` | Clear | — |
| `>` | `>` | Insert line | Insert |
| Backspace | Backspace | Delete line | Delete |
| Tab | Tab | Set tab | Clear tab |
| letter | `a` | `A` | Ctrl+letter |

Read a row like this: the `*` key produces `*`; with Shift down the same key
produces `^`; with Control down it moves the cursor right. The three are
independent functions with independent bindings, and that is exactly what lets
`U(Right) = A(*)`, `U(Shift+Right) = A(^)` and `U(Ctrl+Right) = A(Right)` coexist.

The Shifted column also shows why some names look like keys of their own: `[` is
not a key on the Atari, it is Shift+`,`, and `\` is Shift+`+`. A PC key that
produces one of them — the PC `[` key, for instance — is bound straight to that
shifted function, which is a convenience of the emulator, not a position in the
Atari matrix.

`—` means the machine produces a code there that no Atari software uses; it can
still be named and bound (`A(CTRL_x)`), but there is no default for it.

---

## 7. The default mapping

This is the mapping the emulator uses when `usbkeys.cfg` is absent, and the
suggested starting point for a file of your own. The **default binding** column is
written in the same `U(...)` form a line uses — `U(Shift+1)` is the stroke *Shift
and `1`* — and it is also what rule 4 removes: it is each function's home.

The `code` column is the Atari KBCODE, in hexadecimal, for the log and for
comparison with the emulation core. Console keys, Reset, Break and the two lines
have no KBCODE and are marked `—`.

### Letters and digits

| Function | Code | Default binding |
|---|---|---|
| `A(A)` | `3f` | `U(A)` |
| `A(B)` | `15` | `U(B)` |
| `A(C)` | `12` | `U(C)` |
| `A(D)` | `3a` | `U(D)` |
| `A(E)` | `2a` | `U(E)` |
| `A(F)` | `38` | `U(F)` |
| `A(G)` | `3d` | `U(G)` |
| `A(H)` | `39` | `U(H)` |
| `A(I)` | `0d` | `U(I)` |
| `A(J)` | `01` | `U(J)` |
| `A(K)` | `05` | `U(K)` |
| `A(L)` | `00` | `U(L)` |
| `A(M)` | `25` | `U(M)` |
| `A(N)` | `23` | `U(N)` |
| `A(O)` | `08` | `U(O)` |
| `A(P)` | `0a` | `U(P)` |
| `A(Q)` | `2f` | `U(Q)` |
| `A(R)` | `28` | `U(R)` |
| `A(S)` | `3e` | `U(S)` |
| `A(T)` | `2d` | `U(T)` |
| `A(U)` | `0b` | `U(U)` |
| `A(V)` | `10` | `U(V)` |
| `A(W)` | `2e` | `U(W)` |
| `A(X)` | `16` | `U(X)` |
| `A(Y)` | `2b` | `U(Y)` |
| `A(Z)` | `17` | `U(Z)` |
| `A(0)` | `32` | `U(0)` |
| `A(1)` | `1f` | `U(1)` |
| `A(2)` | `1e` | `U(2)` |
| `A(3)` | `1a` | `U(3)` |
| `A(4)` | `18` | `U(4)` |
| `A(5)` | `1d` | `U(5)` |
| `A(6)` | `1b` | `U(6)` |
| `A(7)` | `33` | `U(7)` |
| `A(8)` | `35` | `U(8)` |
| `A(9)` | `30` | `U(9)` |

A letter is a key: shifting it gives the capital, and that is not a separate
binding. A digit is a function of its own, which is why `A(!)` can be moved
without touching `A(1)`.

### Shifted characters

| Function | Code | Default binding |
|---|---|---|
| `A(!)` | `5f` | `U(Shift+1)` |
| `A(")` | `5e` | `U(Shift+2)` |
| `A(#)` | `5a` | `U(Shift+3)` |
| `A($)` | `58` | `U(Shift+4)` |
| `A(%)` | `5d` | `U(Shift+5)` |
| `A(&)` | `5b` | `U(Shift+6)` |
| `A(')` | `73` | `U(Shift+7)` |
| `A(@)` | `75` | `U(Shift+8)` |
| `A(LParen)` | `70` | `U(Shift+9)` |
| `A(RParen)` | `72` | `U(Shift+0)` |
| `A(_)` | `4e` | `U(Shift+-)` |
| `A(:)` | `42` | `U(Shift+;)` |
| `A(?)` | `66` | `U(Shift+/)` |
| `A(^)` | `47` | `U(Shift+')` |
| `A(|)` | `4f` | `U(Shift+=)` |
| `A([)` | `60` | `U([)` |
| `A(])` | `62` | `U(])` |
| `A(\)` | `46` | `U(\)` |

### Control functions

| Function | Code | Default binding |
|---|---|---|
| `A(CTRL_A)` … `A(CTRL_Z)` | `bf` … `97` | `U(Ctrl+A)` … `U(Ctrl+Z)` |
| `A(CTRL_0)` … `A(CTRL_9)` | `b2` … `b0` | `U(Ctrl+0)` … `U(Ctrl+9)` |
| `A(Up)` | `8e` | `U(Up)`, and `U(Ctrl+-)` / `U(Ctrl+NUM_MINUS)` |
| `A(Down)` | `8f` | `U(Down)`, and `U(Ctrl+=)` |
| `A(Left)` | `86` | `U(Left)`, and `U(Ctrl+NUM_PLUS)` |
| `A(Right)` | `87` | `U(Right)`, and `U(Ctrl+')` / `U(Ctrl+NUM_STAR)` |
| `A(Insert)` | `b7` | `U(Insert)` |
| `A(Delete)` | `b4` | `U(Delete)`, and `U(Ctrl+Backspace)` |

The PC key is each function's **home**: it is what rule 4 removes. The `Ctrl+`
route belongs to the key it is on (the `*` key with Control, say), so it keeps
working when the function is moved — bind that stroke yourself if you want it to
go as well. Not every machine route is reachable from a PC keyboard: `Insert` is
Ctrl+`>` on the machine, but the PC has no `>` key, only Shift+`.`, and that
stroke is already the Atari `>` function.

### Named keys and commands

| Function | Code | Default binding |
|---|---|---|
| `A(Space)` | `21` | `U(Space)` |
| `A(Return)` | `0c` | `U(Return)` |
| `A(Escape)` | `1c` | `U(Escape)` |
| `A(Backspace)` | `34` | `U(Backspace)` |
| `A(Tab)` | `2c` | `U(Tab)` |
| `A(,)` | `20` | `U(,)` |
| `A(.)` | `22` | `U(.)` |
| `A(/)` | `26` | `U(/)` |
| `A(;)` | `02` | `U(;)` |
| `A(-)` | `0e` | `U(-)` |
| `A(=)` | `0f` | `U(=)` |
| `A(*)` | `07` | `U(')` |
| `A(+)` | `06` | `U(NUM_PLUS)` |
| `A(<)` | `36` | `U(Shift+,)` |
| `A(>)` | `37` | `U(Shift+.)` |
| `A(Clear)` | `76` | `U(Shift+Escape)` |
| `A(InsertLine)` | `77` | `U(Shift+Insert)` |
| `A(DeleteLine)` | `74` | `U(Shift+Delete)` |
| `A(SetTab)` | `6c` | `U(Shift+Tab)` |
| `A(ClearTab)` | `ac` | `U(Ctrl+Tab)` |
| `A(Help)` | `11` | `U(F5)` |
| `A(Start)` | — | `U(F6)` |
| `A(Select)` | — | `U(F7)` |
| `A(Option)` | — | `U(F8)` |
| `A(Break)` | — | `U(F9)` |
| `A(Reset)` | — | `U(F10)` |
| `A(Inverse)` | `27` | `U(NumLock)` |
| `A(Caps)` | `7c` | `U(CapsLock)` |
| `A(F1)` | `03` | `U(F1)` |
| `A(F2)` | `04` | `U(F2)` |
| `A(F3)` | `13` | `U(F3)` |
| `A(F4)` | `14` | `U(F4)` |
| `A(Shift)` | — | `U(Shift)` |
| `A(Ctrl)` | — | `U(Ctrl)` |
| `A(CapsToggle)` | `3c` | *none* |

### Remarks on the defaults

- **Letters and digits are mapped by position, not by legend.** The Atari's
  shifted number row is its own row (`! " # $ % & ' @ ( )`), so Shift+`1` on the
  PC types the Atari's `!`, not the PC's.
- **`A(*)` is on `U(')`.** The Atari has a `*`/`^` key with no PC counterpart in
  the same place, and the PC apostrophe key is the natural spare; `*` and `^` are
  otherwise unreachable from the main block. The Atari's own `'` is Shift+`7`,
  which is where a PC keyboard has `7` — so `'` is typed at the Atari position
  and `*` at the spare one.
- **`A(+)` is on the keypad.** Shift+`=` types the Atari's `|`, which is what the
  `=` key carries, and the Atari `+` key has no PC position, so the keypad `+` is
  used.
- **Shift+`,` and Shift+`.` are the Atari's own `[` and `]` keys** in the
  machine's wiring, but the emulator gives those strokes to `A(<)` and `A(>)`,
  because a PC keyboard prints `<` and `>` there. `[` and `]` keep their own PC
  keys, so nothing is lost.
- **Shift+Escape, Shift+Insert, Shift+Delete** reach Clear, Insert line and
  Delete line because those functions have no PC key of their own. The machine
  reaches the same three from Shift+`<`, Shift+`>` and Shift+Backspace.
- **The keypad repeats some keys.** `U(NUM_0)` … `U(NUM_9)`, `U(NUM_DOT)`,
  `U(NUM_SLASH)`, `U(NUM_MINUS)`, `U(NUM_STAR)` and `U(NUM_ENTER)` produce the
  same Atari functions as their main-block counterparts. They are aliases, not
  bindings: remapping `A(1)` moves `U(1)` and leaves `U(NUM_1)` alone.
  `U(NUM_PLUS)` is the home of `A(+)`.

### 7.1 Keypad arrows

When `NUMPAD_CURSOR` is on in `settings.cfg` — the default — the keypad `8`, `2`,
`4` and `6` also produce the cursor functions. This is a setting, not a binding:
it applies on top of whatever `usbkeys.cfg` says, except that a stroke the file
binds explicitly does its bound job instead.

Remapping `A(Up)` therefore does not disturb the keypad arrows, and binding
`U(NUM_8)` takes that key away from them.

---

## 8. Rule 4 in detail

Rule 4 says a function has one binding. In practice:

| Target | What is removed |
|---|---|
| `A(A)` … `A(Z)` | the letter **key**: its unmodified and shifted strokes. Control+letter is a different function and stays |
| every other name | the one stroke listed as its default binding |
| `A(Shift)`, `A(Ctrl)` | `U(Shift)` / `U(Ctrl)` |
| `A(None)` | nothing |

Two consequences that the table makes explicit:

- **Moving one function does not touch the others of the same key.**
  `U(F1) = A(')` removes `U(Shift+7)`; `7` is still on `U(7)`, `&` on `U(Shift+6)`
  and `^` on `U(Shift+')`. Likewise `U(F2) = A(^)` removes only `U(Shift+')`.
- **A function with a machine route keeps it.** `A(Right)` has two producers:
  `U(Right)` (its home) and `U(Ctrl+')`. Rule 4 removes the home. Ctrl+`*` is the
  `*` key with the Control line, not a binding of `A(Right)`, so it keeps moving
  the cursor until you bind `U(Ctrl+')` yourself.

Removed bindings are not reused: a stroke whose binding was removed does nothing
until a line gives it something.

**A function is not a key.** Naming `A(Right)` moves the cursor-right function;
the Atari `*` key, with its own `*` and `^` functions, is untouched. That is the
whole point of the function-level rules.

---

## 9. How a stroke is resolved

For a USB key pressed with Shift and/or Control held, in this order:

1. **An exact binding.** If a line names this stroke, `U(Shift+Right)` say, its
   function is produced exactly as written. The modifiers chose the line and are
   not added again.
2. **A binding for the plain key.** If a line names the key without modifiers,
   its function is produced with the modifiers you are holding added — so a key
   bound to `A(A)` still types `A` with Shift, and one bound to `A(1)` still types
   `!`.
3. **A removed binding.** If this stroke's default binding was removed by rule 4,
   nothing is produced.
4. **The built-in mapping.** Otherwise the default function for this stroke is
   produced: the Atari key the PC key maps to, with Shift and Control folded in
   as the machine does.

Only one Atari key is delivered at a time, as on the real matrix. When several
USB keys are held, the one with the lowest HID usage code wins — this is the
existing behaviour and the file does not change it.

While the OSD menu is open, the keys the menu uses (arrows, Return, Escape,
Space) go to the menu regardless of any mapping, exactly as before. F12 always
opens the menu.

---

## 10. What the log says

The file is read at start-up and its result is reported, because a mapping that
silently does nothing is the hardest kind of problem to see:

```
usbkeys: no SD:/ATARI/usbkeys.cfg; using the built-in mapping
```

or, when present:

```
usbkeys: SD:/ATARI/usbkeys.cfg: 18 lines, 7 mappings
usbkeys: line 12: unknown USB key "U(Qqq)" -- line ignored
usbkeys: line 15: unknown Atari function "A(Clr)" -- line ignored
usbkeys: line 18: U(F5) was already bound on line 9 -- the later line wins
usbkeys: line 21: U(F12) is the menu key and cannot be remapped -- line ignored
usbkeys: line 24: Alt is not a combination modifier -- line ignored
```

A line with an unknown name is skipped; everything else in the file still
applies. Nothing about the file is fatal: a typo costs that line, not the boot.

---

## 11. Examples

**Swap two letters.** Both cases are covered, because a letter is a key.

```
U(A) = A(B)
U(B) = A(A)
```

**Move one function off a key, keeping the others.** `*` leaves the apostrophe
key; `^` and cursor right stay on it.

```
U(F1) = A(*)
```

**The three functions of the Atari `*` key on the PC arrow keys.** This is the
example that needs combination selectors: the plain stroke, Shift and Control are
three strokes of one USB key.

```
U(Right)       = A(*)
U(Shift+Right) = A(^)
U(Ctrl+Right)  = A(Right)
```

**Give a function a second home.** Rule 4 removes the old binding, so the old
stroke has to be restated.

```
U(')     = A(*)      # the * already lives here: restate it
U(NUM_8) = A(*)      # and add the keypad 8
```

**Type `'` from the apostrophe key.** `'` moves off Shift+`7`, and the `*` that
the apostrophe key used to produce moves to the otherwise unused grave key, so
nothing is lost.

```
U(')  = A(')
U(`)  = A(*)
```

**A dedicated control key.** One key that types Ctrl+A, useful in BASIC where
Ctrl+A is a common editing command.

```
U(F11) = A(CTRL_A)
```

**Move Start, Select and Option.** The old function keys stop doing those jobs,
which is rule 4.

```
U(F1) = A(Option)      # was U(F8)
U(F2) = A(Select)      # was U(F7)
U(F3) = A(Start)       # was U(F6)
```

**Disable strokes.** A stroke bound to `A(None)` does nothing.

```
U(NUM_9)     = A(None)
U(Shift+1)   = A(None)   # Shift+1 no longer types !
U(ScrollLock) = A(None)
```

---

## 12. Limits

- **Read at start-up only.** There is no reload command; restart the Pi.
- **One function per stroke.** A stroke can mean one thing, and only Shift and
  Control can be part of it. There is no `U(Alt+A)`.
- **F12 belongs to the emulator** and cannot be mapped.
- **The GPIO keyboard is not affected.**
- **The menu's keys are not affected** while the menu is open.
- **Keys not listed in section 4 cannot be named.** The media keys, the Windows
  keys, the keypad `=` and the international keys have no names and no default
  action.
- **A letter is a key; everything else is a function.** There is no name for the
  capital `A` on its own, so a letter moves with both its cases.
