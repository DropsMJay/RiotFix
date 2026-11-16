# Controller support — what the binaries still hold

Findings from the Xbox build (`Default.xbe`, 3035136 bytes, from the USA disc) read against both PC binaries.
Everything under "measured" was read out of the files; everything under "inferred" is a reading of that data that has
not been tested in play.

**Superseded in part by `Docs/XboxInput.md`**, which reads the Xbox controller code itself. Corrections from there
are marked below; the rest of this file still stands.

## Summary

The PC build removed the controller **code**, not the controller **data**. Both PC binaries still carry the whole
action vocabulary, the console's Controller Settings screens, the button artwork, the rumble curves and the
weapon code. (Corrected: the five layouts' button tables are in the Xbox executable, at `0x2CDAA0`, and the
"aim-assist routine" is code both builds run; see `Docs/XboxInput.md`.)

So RiotFix does not need the Xbox ids. It needs the PC's own, and those are in the `.rfl`.

## The action vocabulary (measured)

Twenty-two names, in this order, identical in all three binaries:

```
ScreenAdjust  GUIDelete  GUIDevice  GUICanNoBack  GUICancel  GUISelNoStart  GUISelect  GUIUnpause
Flashlight  Reload  Map  Inventory  Insanity  Health  FirstPerson  Use  Throw  Jump  Attack  Look
LadderMove  Move
```

These are the placeholders the games expand per device to build help text (`<GUISelect>` and friends). Where they
live:

| | strings | table |
|---|---|---|
| Xbox `Default.xbe` | `.rdata` `0x00294F88` | `.data` `0x002BE158` |
| The Suffering `Suffering.rfl` | `0x1014B538` | `0x10167438` |
| Ties That Bind `SufferingTTB.rfl` | `0x01A36BB4`-ish | `0x01A36C60` |

## Table 1 — action to input id (measured)

**Xbox and The Suffering** use the same shape: rows of 8 bytes, `(pointer to the action name, input id)`, in
reverse order of the string table.

**Ties That Bind** stores the same information differently: rows of 8 bytes, `(input id, index into the action
list)`, with no pointers at all. A pointer search finds nothing in TTB for that reason, which cost an hour.

Xbox holds 30 rows; both PC builds hold 26. The four rows PC dropped are the `Move` rows — Xbox ids `0x402`,
`0x403`, `0x408`, `0x409` — which is the left stick. On PC, movement is keys, so those rows went away.

Row for row, same actions, same order, renumbered:

| action | Xbox | PIH | TTB |
|---|---|---|---|
| GUIUnpause | `0x3EA` | `0x3EA` | `0x3EA` |
| ScreenAdjust ×4 | `0x3EC`–`0x3EF` | `0x3EC`–`0x3EF` | `0x3EC`, `0x3ED`, … |
| GUISelNoStart | `0x3FA` | `0x3FA` | `0x3F9` |
| GUICanNoBack | `0x3FB` | `0x3FB` | `0x3FB` |
| GUISelect | `0x3FC` | `0x3FC` | `0x3F9`-group |
| GUIDelete | `0x3FD` | `0x3FD` | — |
| GUIDevice | `0x3FE` | `0x3FE` | `0x3FE` |
| Map | `0x3FF` | `0x3FF` | `0x3FF` |
| GUICancel | `0x401` | `0x403` | `0x401` |
| LadderMove ×2 | `0x402`, `0x403` | `0x409`, `0x40A` | |
| Look ×4 | `0x404`, `0x405`, `0x415`, `0x416` | `0x40B`, `0x40C`, `0x41C`, `0x41D` | |
| Reload | `0x41D` | `0x425` | |
| Flashlight | `0x41E` | `0x426` | |
| Attack | `0x41F` | `0x428` | |
| Throw | `0x420` | `0x429` | |
| Jump | `0x421` | `0x42A` | |
| Use | `0x422` | `0x42B` | |
| FirstPerson | `0x423` | `0x42C` | |
| Insanity | `0x424` | `0x42D` | |
| Inventory | `0x425` | `0x42E` | |
| Health | `0x426` | `0x430` | |

The pattern: **the menu ids below `0x400` did not move between Xbox and PC; the gameplay ids above it rose by 7 to
10.** The natural reading is that the PC build inserted keyboard-specific ids into the middle of the enumeration
and pushed everything above them up, leaving the lower range alone [inferred]. TTB renumbered again and added ids
of its own (`0x44A`, `0x44B`, `0x44C`).

## Table 2 — analog and button slots, Xbox only (measured)

(Corrected: this is the first of the five layout tables, and the slots are the pad's inputs, proven by its poll
code: 0 Start, 1 Back, 2–5 left stick, 6–9 right stick, 10–13 d-pad, 14–17 Y X B A. See `Docs/XboxInput.md`.)

Directly after table 1, at `0x002BE260`, 24 records of 20 bytes: `(input id, flags, slot index, minimum, maximum)`.
Every record measured so far has flags 0, minimum −1.0 and maximum +1.0.

```
0x3EA  slot 0            0x400  slots 17,16,15,14,0,1
0x3FF  slot 1            0x3EC  slots 2,10
0x427  slots 17,16,15,14 0x3ED  slots 3,11
0x3F0  slot 10           0x3EE  slots 4,12
0x3F1  slot 11           0x3EF  slots 5,13
0x3F2  slot 6            0x3F3  slot 7
```

The slots pair up in two groups of four — 2/3/4/5 against 10/11/12/13, and a separate 14/15/16/17 — and the
`ScreenAdjust` ids take one slot from each of the first two groups.

**Inferred, not proven:** slot 0 is Start and slot 1 is Back (they carry `GUIUnpause` and `Map`); 2–5 are the d-pad;
10–13 are one stick's four directions; 14–17 are the other stick's. Ids `0x400` and `0x427` do not appear in table 1
at all, so they are analog-only actions. None of this has been checked against a running game.

## What is still missing

(Corrected: both are found. The layouts are five tables picked by index, and the ids are read by the same action
query the PC uses. See `Docs/XboxInput.md`. The text below is kept as it was written.)

**The per-layout button tables.** The data has the five layout names (`5:400`–`5:404`: Double Stick (Default),
Throw/Jump Swap, Single Stick, Double Stick+Flashlight, Jump On Top, plus Custom at `5:406`), the Controller
Settings templates (#855 front end, #858 in game, class `GUI Controller Settings` — the class name is in the XBE at
`0x002978AC`), the action-name labels and the controller pictures (textures `5:340`, `5:341`, and `5:889`, `5:890`
for some layouts). It does not have, anywhere found so far, the mapping of button to action per layout. The
reimplementation's notes reach the same conclusion from the PC side. It may be computed in code rather than stored.

**What consumes the input ids.** Tables 1 and 2 tie an action name to a number. Nothing here shows what reads those
numbers. Until that path is traced, "RiotFix writes these ids and the game responds" is a guess, and the whole
plan below rests on it.

## Other controller-era data still in the PC build

From the reimplementation's own survey of the retail data, confirmed to be about the PC files:

- **68 rumble curves** in `COMMON/Resources/Resources.wdu`, package 4, as `.wdu` type `0x60` entries whose header
  kind is 0: `u16` motors, `u16` bits, `u32` rate (10/15/20/30 Hz), `u32` 0, `u32` n, then n bytes of interleaved
  motor levels. Referenced only from sound headers at `+0x28` — 127 sounds, all into package 4: weapon fire, melee
  swings and hits, detonations, creature attacks, and System sounds 2:148–175.
- **Console strings**: `5:169` is the controller-removed notice, with `%d` and `<GUISelect>` placeholders; `5:196`
  is "no pad"; System 46–74 are **PS2** button names ("START button", "L1 button", "left analog stick up & down").
  So the PC data carries PlayStation button names while texture `5_014c` carries Xbox face-button art — traces of
  both consoles survived.
- **Console rows on the Gameplay page**: Auto Pitch Correction (`5:362`), Auto Targeting (`5:355`), Controller
  Vibration (`5:370`).
- **How a hint names an input**: either a word from the string table or the input's own code written into the
  text as a character for the font to draw as a button icon, with a hash set of codes deciding which. Measured
  in the expander; see `Docs/PadPort.md` section 7. The PS2 build does both in one line ("Use R1 button to
  attack", "Use <cross icon> to open lockers"); the PC build only ever shows words.
- **Two of the cheats are colour filters**: Murnau Mode (black and white) and Tripping the Light Fantastic.
  Their codes are controller button sequences and this build never had a controller, so they may have been
  unreachable here since release. Tried in play with RiotFix's pad: the sequence did nothing, so the reader
  either does not watch this device or wants something else. Not investigated yet. Whatever Murnau Mode
  switches is a way into the engine's own colour path, which is where a colour effect of ours should attach.
- **A cone and line-of-sight test** in `Suffering.rfl` at `0x10127FED`–`0x10128083`, inside the ranged-weapon fire
  routine at `0x10127B30`. Once taken for aim assist; the Xbox runs the same code (`0x135A09`), so it is not a
  console feature.
- Neither PC exe imports anything but `DirectInput8Create`; `GUID_SysKeyboard` and `GUID_SysMouse` are present
  (PIH `0x59784C`, `0x59785C`), `GUID_Joystick` is not. There is no joystick, gamepad, XInput or analog-axis string
  anywhere in either exe or `.rfl`.

## Where this points

(Replaced.) RiotFix 0.2.x answers the engine's action query with the stick values and turns the buttons into the
keys the player has bound, in the five PS2 layouts. `Docs/XboxInput.md` sets out what the Xbox code adds on top of
that and the two ways to port it.
