# The Xbox build's controller code, read against the PC build

Read from the Xbox `Default.xbe` (USA disc) and the GOG `SUFFERING.EXE` / `Suffering.rfl`. Everything marked
**measured** was read out of the code or data; **inferred** is a reading not yet proven by code or by a test.
Nothing here has been tested in play yet.

## The short version

- **The input layer is the same engine code on both machines** (measured). The PC removed one class, the pad device,
  and kept everything around it: the device array, the device interface, the binding tables, the action query and
  even the rumble call. A pad on PC is one missing class, not a missing system.
- **The five console layouts are in the Xbox executable** (measured), as five binding tables in the same format as the
  PC's own control table, picked by layout index. `Docs/Controller.md` said they had never been found: they had not
  been looked for in the right place.
- **Walking with the stick halfway is the engine's own code, and the PC still has it** (measured). Speed is the length
  of the move vector, capped at 1. The PC only skips it because its default bindings give that same slot a key.
- **The Xbox layouts are not the PS2 ones** RiotFix uses today: the Xbox has White and Black buttons and puts Jump on
  A, Use on X.

## 1. The device layer (measured)

Both builds keep an array of input devices, 0x28 bytes per entry, the first field a pointer to the device object:

| | array | entries | devices |
|---|---|---|---|
| PC `SUFFERING.EXE` | `0x5B5418`–`0x5B5468` | 2 | keyboard `0x5CAA20` (vtable `0x58CCB0`), mouse `0x5CAF58` (vtable `0x58D298`) |
| Xbox `Default.xbe` | `0x2D15E8`–`0x2D1688` | 4 | four pads `0x300FC0`, `0x300DC0`, `0x3010C0`, `0x300EC0` (vtable `0x2A7F30`) |

The loops over that array are byte for byte the same code (PC `0x40C080`, Xbox `0x14E0B0`). A binding record's device
number picks the entry: PC `manager+0x78 + record.Device`; the Xbox goes through one more table (`manager+0x6C`,
indexed by `manager+0x80 + record.Device`) that maps a player to a pad port.

All three device classes share one 20-slot interface. Side by side:

| slot | keyboard | mouse | Xbox pad | what the pad's does |
|---|---|---|---|---|
| 0 | `0x410050` | `0x4103C0` | `0x1B0EE0` | destructor |
| 1 | `0x40FE00` | `0x4103E0` | `0x1B06C0` | start: `XInitDevices` once, then slot 6 |
| 2 | `0x101` | `0x10` | `0x1F` (31) | number of inputs |
| 3 | `0x100` | `0x0E` | `0x18` (24) | number of inputs offered for binding [inferred] |
| 4 | `0x40FB50` | `0x4104C0` | `0x1B0740` | close (`XInputClose`) |
| 5, 6 | shared | shared | `0x1B0760`, `0x1B0990` | is open / open (`XInputOpen`) |
| 7 | `0x40FE90` | `0x4100C0` | `0x1B09A0` | **poll**: fills the value array |
| 8 | `+0x110` | `0x410340` | `+0x38` | the value array, one float per input |
| 9 | 0.0 | 0.0 | `0x1B0770` | per-input press threshold |
| 10–12 | 1 | 1 | 1 | |
| 15 | `0x58CD00` | `0x58D2F0` | `0x2A7E20` | input names: 6-byte records `(0xE000 + input, string id, 0)` |
| 19 | `0x40FD50` | same | `0x1B0F00` | **rumble**: one of the engine's growable arrays, by value (`ret 0xC`), holding two levels - level 0 to XINPUT_FEEDBACK's left motor at `+0x42`, level 1 to its right at `+0x44`, both x 45874.5 and skipped while the last feedback is still pending (`0x3E5`). The PC devices discard the lot |

## 2. The pad's poll (measured, `0x1B09A0`)

It reads `XInputGetState` and writes 27 floats, 0 to 1:

| input | source | | input | source |
|---|---|---|---|---|
| 0 | Start | | 14 | Y (analog byte / 255) |
| 1 | Back | | 15 | X |
| 2, 3 | left stick up, down | | 16 | B |
| 4, 5 | left stick left, right | | 17 | A |
| 6, 7 | right stick up, down | | 18, 19 | left, right trigger |
| 8, 9 | right stick left, right | | 20, 21 | left, right stick click |
| 10–13 | d-pad up, down, left, right | | 22, 23 | Black, White |
| | | | 24, 25, 26 | largest of the left stick's / right stick's / d-pad's four |

**Stick dead zone:** each axis is divided by 32767; an axis under 0.3 is zeroed; then the stick's length is taken,
and if it is 0.3 or less both axes are 0, otherwise both are scaled by `(length - 0.3) x 1.4286 / length`. That is
a radial dead zone of 0.3 with the rest rescaled to 0..1 (1.4286 = 1 / 0.7). RiotFix's current `DeadZone` is a plain
per-axis cut with no rescale.

**Press thresholds (slot 9):** 0.2 for buttons, d-pad and clicks; 0.55 for the triggers; 0 for the stick halves.

These input numbers are the "slots" `Docs/Controller.md` guessed at. That guess is now proven by code.

## 3. The five layouts (measured)

The controller settings object calls the bindings setter (`0x14E1F0`, which writes `manager+0x24`) with
`LayoutTables[layout]`, a table of five pointers at `0x2CDAA0` (code at `0x16B60`). Each table has the PC
control table's own record format (action, device, input, -1.0, +1.0) and ends with an action of -1:

| table | address | records |
|---|---|---|
| 0 | `0x2BE260` | 82 |
| 1 | `0x2BE8E0` | 83 |
| 2 | `0x2BEF70` | 82 |
| 3 | `0x2BF5F0` | 82 |
| 4 | `0x2BFC70` | 87 |

Table 0 is also installed at start-up as the default (`0x16DA0`).

Gameplay rows, in Xbox buttons:

| action | 0 | 1 | 2 | 3 | 4 |
|---|---|---|---|---|---|
| Attack | RT | RT | RT | RT | X |
| Jump | A | A | LT | White | A |
| Throw | LT | LT | X | LT | B |
| Use | X | X | A | A | Y |
| First person | White | White | White | X | White |
| Health | Black | Black | Black | B | L3 |
| Insanity | Y | Y | Y | Y | Black |
| Reload | B | B | B | Black | R3 |
| Flashlight | – | R3 | – | – | – |
| Move | left stick | left stick | left stick | left stick | left stick up/down, triggers strafe |
| Look | right stick | right stick | right stick | right stick | both sticks turn, right stick pitches |
| Speed (see 4) | – | – | – | – | largest of left stick, LT, RT |

Which name goes with which table is **inferred** from the content: 0 Double Stick (Default), 1 Double Stick +
Flashlight (the only one with Flashlight), 2 Throw/Jump Swap, 3 Jump On Top, 4 Single Stick (the only one with
triggers on Move). Table 2 moves Jump to Throw's trigger, as the PS2's Throw/Jump Swap moves Jump to Throw's L1.
The code does not hold the names: the controller screen (`0x643C0`) labels layout *i* with template property
`0x17 + i` (measured), so the name of each index is in the screen's template data (`#855`, `#858`), which can prove
it.

Menu rows are the same in all five: Start pause, Back map, A select, B or Back cancel, stick and d-pad move,
d-pad cycles weapons and thrown weapons, any d-pad direction opens the inventory.

## 4. Walking and running (measured)

The same function in both builds (PC `Suffering.rfl` `0x10045DE3`, Xbox `0x56741`) decides the player's speed:

- **If the speed action is bound** (PC `0x411`, Xbox `0x40A`): PC takes `1 - value`, so a held key slows the
  player; the Xbox takes the value itself.
- **If it is not bound:** both builds call the same function (PC `0x100A10B0`, Xbox `0xAEB20`): the length of the
  move vector, capped at 1.

On the Xbox, only Single Stick binds the speed action. The other four leave it unbound, so the stick halfway walks.
The PC's default table binds `0x411` to LShift, so the engine never reaches the stick-length path.

**What that means for RiotFix:** if the engine is told that `0x411` is not bound while the pad is in use (the
"is bound?" question is slot 37 of the same object whose slot 38 RiotFix already hooks, `[edx+0x94]` at
`0x10045DEF`), the PC runs the console's own speed code, with nothing invented. It also needs the move actions to
carry the stick's real value, which RiotFix already sends. Not tested.

## 5. Rumble (measured)

The PC still runs the engine's rumble mixer: a function at `SUFFERING.EXE` `0x40F7A0`, called through a pointer at
`0x58C5BC`, works out two motor levels, compares them with each device's slot 18 value, and calls slot 19 on every
device in the array (`0x40FAC1`). It is the same function as the Xbox's `0x18B9B0`. On the PC that call reaches the
keyboard and the mouse, which discard it. A pad device in the array would get the engine's own rumble with nothing
else changed. What feeds the mixer (the rumble curves in the data) has not been read.

## 6. Action ids, Xbox to PC

Measured by laying the Xbox table 0 next to the PC control table: the rows come in the same order. **Inferred**
for rows whose meaning is not yet named in code:

| Xbox | PC | | Xbox | PC |
|---|---|---|---|---|
| `0x3EA`–`0x3FF` | same | | `0x40A` speed | `0x411` (measured, section 4) |
| `0x401` cancel | `0x403` | | `0x40B`–`0x410` | `0x412`–`0x417` (a second move context) |
| `0x402`, `0x403` move up, down | `0x409`, `0x40A` | | `0x411`–`0x414` | `0x418`–`0x41B` |
| `0x404`, `0x405` look left, right | `0x40B`, `0x40C` | | `0x415`, `0x416` look up, down | `0x41C`, `0x41D` |
| `0x406`, `0x407` (Y, A) | `0x40D`, `0x40E` (mouse wheel) | | `0x419`–`0x41C` d-pad | `0x420`–`0x423` |
| `0x408`, `0x409` move left, right | `0x40F`, `0x410` | | `0x41D`–`0x426` gameplay | `0x425`–`0x430` (table above) |
| `0x425` inventory | `0x42E` | | `0x437` struggle | `0x443` (Space) |

Read in the code:

- **`0x437` / `0x443`, struggle:** both builds count the presses of this action and take them off a meter (Xbox
  `0x8D9DB` and `0x91A42`, PC `0x1012EBB0`): mashing to break free. Xbox binds every face button and trigger to it,
  the PC binds Space.
- **`0x40B`–`0x414` / `0x412`–`0x41B`:** one function in each build (Xbox `0xBC630`, PC `0x100AE99C`) reads them as
  three move axes and a look pair for something other than the player. On the PC its look comes from the mouse.
  What it moves is not read yet.
- **`0x406`, `0x407` / `0x40D`, `0x40E`:** only read by a function that polls every move and look action at once
  (Xbox `0x51F20`, PC `0x10040D97`), as far as found.
- **`0x400`, `0x427`:** "press" queries in menus and sequences (Xbox `0x13C55A` after the sequence manager); their
  PC counterparts are not proven.

## 7. Options that are not the controller's

The Xbox options block applied at `0x16B60` holds the layout at `+0`. Its `+4` and `+8` go to the display object
(`0x2A4240`); its `+0x18` and `+0x1C` go to a DirectSound class (vtable `0x2A7D40`). So none of those is Auto
Targeting, Auto Pitch or Vibration. Where those three are read was not found.

The block in `Suffering.rfl` `0x10127FED` that `Docs/Controller.md` called the aim assist is the same code on the
Xbox (`0x135A09`): a cone and line-of-sight test in the weapon code, run on both machines. It is not a console
feature the PC lost.

## 8. Ties That Bind (measured)

The same layer: device array at `SufferingTTB.exe` `0x6156E8`, two devices (keyboard `0x63E410`, mouse `0x63E948`).
The speed decision is at `SufferingTTB.rfl` `0x18CDEAD`: the speed action is `0x400`, "is bound?" is slot 38 (`0x98`)
and its value slot 39 (`0x9C`), one slot further on than in the first game, as RiotFix's action-query hook already
has it. When the action is unbound it falls back to the move vector's length, as in the first game. No Xbox build
of Ties That Bind was read.

## 9. How far this goes for a port

There are two ways, and the second is what the Xbox actually does:

1. **Keep today's approach** (pad values answered to the action query, buttons turned into keys) and port pieces:
   the radial dead zone, the trigger threshold, analog walking (section 4), the Xbox layouts next to the PS2 ones.
   Small and safe. Rumble stays impossible: nothing in this path hears the engine's rumble call.
2. **Port the pad device itself.** A C class with the 20-slot interface above, its poll filled from SDL instead of
   `XInputGetState`, added to the device array as a third device, with control-table records for device 2 taken
   from the five Xbox tables (renumbered with section 6). The engine would then do everything itself: presses,
   releases, holds, thresholds, menus, the help text's button names, analog speed when the speed action is unbound,
   and **rumble**, through slot 19 and the 68 rumble curves still in the PC data. The cost: the PC array has room
   for two devices, so it has to be moved to a bigger one (17 references to `0x5B5418` in the exe, more to the fields and the end of the
   array, all operand changes), and the control table needs the pad's rows added. Bigger, and each step has to be proven in play.

## Still open

- Which layout name goes with which table index: in the template data (section 3).
- What feeds the rumble mixer, and whether the PC data's curves reach it (section 5).
- Where Auto Targeting, Auto Pitch and Vibration are read (section 7).
- What the second move context drives (section 6).
- What the engine does with the input names (slot 15) and slot 16 (`0x1B0870`).
