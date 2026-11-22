# Porting the console's pad as a real device

The plan for native controller support: not keys faked for the game, but a third input device in the engine's own
device array, so the engine answers presses, releases, holds, analog values, menus and rumble itself, exactly as it
did on the Xbox. `Docs/XboxInput.md` holds what the Xbox build does; this file holds what RiotFix will do and why,
with the measurements the design rests on and the order of the work.

**measured** = read out of the binaries. **inferred** = a reading not yet proven. **to measure** = a question stage 0
answers in play. Nothing here has been tested in play yet.

## 1. What the engine already does for a device (measured, The Suffering)

The input manager keeps an array of devices at `0x5B5418`, two entries of 0x28 bytes, ending at `0x5B5468`:

| field | what it holds |
|---|---|
| +0 | the device object |
| +4 | its state: 0, 1 or 2 (slot 1 returns 2 when the device is ready; 2 is the state the per-frame update polls) |
| +8 | its number of inputs, the stride of one row of samples |
| +0xC | another buffer the manager keeps |
| +0x10 | the samples: rows of one float per input, read as `[+0x10] + 4 * (inputs * row + input)` |
| +0x18 | how many rows are kept |
| +0x20 | when it was last polled (`timeGetTime`) |

Each entry's device is reached through a 20-slot interface. Of those, 11 slots are the same function for the keyboard
and the mouse, so they are the base class's; only 9 differ:

| slot | keyboard | mouse | what a pad must do |
|---|---|---|---|
| 0 | `0x410050` | `0x4103C0` | destructor: nothing (the object lives as long as the game) |
| 1 | `0x40FE00` | `0x4103E0` | become ready; **returns 2** on success, -1 on failure |
| 2 | 0x101 | 0x10 | number of inputs: the pad's 27 |
| 3 | 0x100 | 0x0E | how many of them may be bound: 24 |
| 4 | `0x40FB50` | `0x4104C0` | close: nothing |
| 5, 6 | shared | shared | is it open / open it; **slot 6 must answer 2**, see the state machine below |
| 7 | `0x40FE90` | `0x4100C0` | poll: write one float per input |
| 8 | `this+0x110` | `this+0x34` | where those floats are |
| 13 | `ret 4` | `0x410500` | keyboard does nothing; the pad does the same |
| 15 | `0x58CD00` | `0x58D2F0` | the input names: 6-byte rows of `(code, 32-bit string id)`, see below |

The shared ones: 5 and 6 (is open / open, through a handle at +0xC), 9 (**0.0** for both, the press threshold per
input — the pad's own thresholds go here), 10, 11 and 12 (all return 1), 14 (names the input, through slots 15 and 2),
16 (nothing), 17 (**1.0**), 18 (**0.05**, the floor the rumble mixer compares against), 19 (rumble, thrown away by
both PC devices; what arrives is described below).

**A name row is not three words (measured).** The row slot 15 hands out is six bytes, but only the first two are the
input's code: the reader at `0x40C690` takes a **dword** from the row's second byte (`mov eax, [eax+2]`) and treats
**-1** as "this input has no name". So the last four bytes are one 32-bit string id, which is why the keyboard's own
nameless row 0 (`0x58CD00`) carries 0xFFFF in both of its last words and every named row carries a small id and a
zero. An id that is neither -1 nor a real entry is not refused anywhere: the game looks it up, takes what the lookup
leaves behind and calls a method on it. The mouse's table is worth the same note - 14 rows for the 16 inputs slot 2
claims, while slot 14 bounds-checks against slot 2 - so a device must never be asked about an input it cannot bind.

**Slot 19's argument (measured).** The mixer (`0x40F7D0`) works out the frame's two motor levels, zeroes either one
below slot 18's floor, copies them into one of the engine's growable arrays and hands that array whole to slot 19 on
every device (`0x40FAC1`). The array is three words - `{ the floats, how many fit, how many there are }`, the shape
`0x4D1600` fills - and it goes on the stack by value, which is the 12 bytes the slot clears on return. The Xbox
build's own rumble slot (`0x1B0F00`) reads it the same way and settles the rest: **level 0 is XINPUT_FEEDBACK's left
motor and level 1 its right**, which are SDL's low and high frequency ones, and both are scaled by the float at
`0x2A7F80`, **45874.5** rather than 65535 - the console holds its motors to seven tenths of what they can do.

**Values are plain:** the keyboard writes 1.0 for a pressed key and 0.0 for a loose one. There is no dead zone and no
scaling in a device; all of that belongs to the binding record.

**The state machine is the whole of it.** The manager keeps a state per device in the array and polls only a device
whose state is 2 (`0x40C9E6`):

- state 0: it calls slot 5, and anything but zero moves the device to state 1;
- state 1: it calls slot 6 and **makes the answer the new state**, so an answer of 1 leaves it at state 1 for ever;
- state 2: it polls the device twice a frame.

Answering 1 from slot 6 is what made the pad work in the menus and nowhere else: slot 1 answers 2, so the pad began
at state 2 and was read; loading a level put the devices back to state 0, and from there the pad could climb to
state 1 and no further, so the engine stopped reading it. The log said it plainly once it was asked to count - "the
engine read the pad 0 times" while the pad itself was reading 1.000 - and the code said why.

## 2. What a binding record is (measured)

The lookup (`0x40C3D0`) walks the manager's table at +0x28, then the one at +0x2C, in rows of 20 bytes, to the first
row of the asked action; the query (`0x40D1A0`) then walks on while the rows keep that action. A row is:

`{ Action, Device, Code, Low, High }` — Device being the array index, Code the device's input number.

For each row of the action the query works out, with the raw value `v` of that input (`0x40C470`, by mode: 0 the
newest sample, 1 the largest of the kept rows, 5 their average):

```
if v <= Low            ->  0
else                   ->  v = (v - Low) / (1 - Low)
if |High| != 1         ->  v = sign(v) * |v| ^ |High|
if High < 0            ->  v = 1 - v
```

and returns the largest result of the action's rows. So the record carries the range, the curve and the inversion:
**an inverted axis is a record with a negative High**, and no code of ours is needed for any of it.

The table in `Suffering.rfl` at `0x10167520` is not this live table: it holds the defaults, and its rows read
Low = -1.0, High = 1.0, which would make a loose key read 0.5. **Measured in play**: every live record reads
Low = 0.0, High = 1.0, so a pad record is `{ action, 2, input, 0.0, 1.0 }`, or High = -1.0 for an inverted one.

**Measured in play** too, both tables are in use, so neither is free: +0x28 holds the player's 99 bindings and lives
on the heap, +0x2C holds 25 rows of the engine's own debug keys and lives in the executable. Our rows therefore go
into a copy of the first table with the pad's rows appended, installed at +0x28. That pointer is the game's to
change - it rebuilds the table when the bindings are loaded or edited - so RiotFix checks each frame that +0x28 is
still its own table and rebuilds it when the game has replaced it.

## 3. The design

**A pad device of our own, in the array.** A C struct with the 20-slot interface of section 1 and its own float
array, filled from SDL in slot 7 with the Xbox's own numbers (section 2 of `Docs/XboxInput.md`): 27 inputs, a radial
dead zone of 0.3 rescaled to 0..1, thresholds of 0.2 for buttons and 0.55 for triggers given out by slot 9.

**The array has to move.** It has room for two entries, and the memory right after it is in use (`0x5B5468` onwards
holds other data). So RiotFix puts a three-entry array in its own memory, copies the two static entries into it, puts
the pad in the third, and repoints the executable's 30 references. Every one of those is an instruction operand — an
address inside a `mov` or a `cmp` — so this is a table of immediates, not written assembly. It happens at attach,
before any game code runs, and the engine then initialises all three entries itself.

Two kinds of reference, and the difference is what makes or breaks it: 24 **fields**, which name the array or one
field of it (+0, +4, +8, +0xC) and simply move, and 6 **bounds**, the addresses loops stop at, which are a starting
field plus one entry per device and therefore have to grow by a whole entry. The first build of this stage missed
one bound — the end of the loop at `0x40BC50`, which is the loop that gives every device its state, its input count
and its buffers — so that loop ran for the keyboard alone and left the mouse with none of them, and the game hung.
RiotFix now checks every site holds the address it should before writing anything, and sweeps the game's code
afterwards: a single four-byte value still pointing into the old array and the whole change is put back.

**The buttons become records.** RiotFix builds a table from the live one with the chosen layout's rows (Device = 2)
appended, and installs it at +0x28, rebuilding it whenever the game replaces that table with one of its own.

**Layouts:** ten of them, the five Xbox tables renumbered to PC action ids (`Docs/XboxInput.md` section 6) and the
five PS2 ones RiotFix already uses, chosen by `[Controller] Layout`.

**Rumble** needs nothing but the pad: the engine's mixer already calls slot 19 on every device in the array
(`0x40FAC1`), so our slot 19 passes the two motor levels to `SDL_RumbleGamepad`, in the console's order and with the
console's ceiling. The mixer asks every frame with the levels it wants playing *now*, not with events, so RiotFix
sends them on only when they change, and asks for 250 ms: longer than a frame, so an ordinary frame refreshes an
effect rather than restarting it, and short enough that a stall lets the motors fall quiet on their own.

**Walking with the stick** also needs nothing but a record. The engine's own speed code (`Suffering.rfl`
`0x10045DE3`) takes `1 - value` of the speed action when it is bound. If the pad exposes an input that reads
`1 - magnitude` while the stick is pushed and **0 while it rests**, then a record binding the speed action to it
gives: stick resting → 0 → run (nothing is moving anyway); stick at half → 0.5 → half speed; stick full → 0 → run.
LShift keeps working, because the query takes the largest result of the action's rows. The resting value is what makes
this work: an input that read 1.0 at rest would hold the player at a walk for ever.

**What goes away:** the key injection and the stick-answering hook of 0.2.x, once stage 3 proves out. Until then both
paths exist and `[Controller] Native` chooses.

## 4. Order of work, with the test that closes each stage

0. **Measure in play** (`[Debug] LogBindings`): log the live records of a few actions with their Low and High, the
   three fields the manager fills in each array entry, and which of +0x28 / +0x2C is free. Read-only; nothing to
   break. *Closes when the log answers all three questions.*
1. **Move the array** to three entries, the third holding a pad with 27 inputs filled from SDL, with no record
   pointing at it. *Closes when the game plays exactly as before and the log shows the manager filling in entry 2's
   stride, samples and state, with its values moving as the sticks and buttons do.*
2. ~~Fill the values from SDL~~ — done with stage 1, since reading SDL was already proven by the 0.2.x controller.
3. ~~**Install the records** of one layout (Double Stick) and turn the old path off.~~ **Closed in play**: the pad
   moves, looks, fights and works the menus through the engine. Three bugs were found and fixed there, all three in
   section 1's state machine or in the record groups: slot 6 answering 1 instead of 2, the first-group test looking
   before the row instead of before the group, and two menu actions never bound - going into an item (`0x3FC`, the
   keyboard's Enter) and scrolling a long list (`0x3F2`/`0x3F3`, the mouse wheel). Closing a menu is `0x403`, the
   keyboard's Escape; the console that puts it on triangle is the PS2, whose table RiotFix cannot read, so that one
   button rests on a player's report and not on a measurement.
### The crash of the native pad: cause and fix

A save loaded into one place crashed every time the pad was a device of the engine, always identically: a jump to
an address equal to one of our own input codes, from the engine's screen walk. It is **the game's own fault**,
uncovered rather than caused, and it is fixed by two bytes and a re-aimed branch. Found in `SUFFERING.EXE`, not in
the `.rfl` where the stack pointed.

**The cause.** A hint carries markers - `To turn crank handles, press and hold <Use>, then use <LadderMove> to
rotate crank forward and backward` - and the expander at `0x40DEA0` replaces each with the keys bound to it. For
every record of the action it appends that record's code to a list kept **per device**, and the array of those
lists is built a few instructions earlier with a count of two:

```
0040E059  push 2                      ; two devices: the keyboard and the mouse
0040E05B  push 0xC                    ; twelve bytes each
0040E0B8  mov  eax, [edi+4]           ; the record's device
0040E0BB  cmp  eax, -1 / je 0x40E10A  ; the only device it refuses
0040E0C0  lea  esi, [eax+eax*2]       ; device * 3 dwords
0040E0F9  mov  ecx, [esp+eax*4+0x80]  ; that device's list - read past the end for device 2
0040E100  mov  [ecx+ebp*4], edx       ; and the record's code written through it
```

The pad is device 2. Reading one element past a two-element array hands the expander whatever the frame holds
there, and the code of the record is written through it. `<LadderMove>` covers 0x409 and 0x40A, so the loop runs
over two actions and the second pass reaches the frame's return address - which is why the jump target was always
one of our input codes (3 on the left stick, 7 on the right), and why one action alone never crashed. A player
could reach the same fault without any patch by binding three keys to walking forwards and three to backwards.

**The fix.** The test at `0x40E0BB` already exists to refuse a device; it just refuses the wrong set. `cmp eax, 2`
with an unsigned `jae` refuses device 2 and above and still refuses -1. The branch must also be re-aimed from
`0x40E10A` to `0x40E107`: `0x40E10A` is **past** the `add edi, 0x14` that steps to the next record, so refusing a
record and landing there reads it again forever. That cost one build and a hang that took the task manager -
harmless in the original, where the test never matched, and an endless loop the moment it matches something real.
Three byte values change, no instruction moves, no assembly is written. Hints name the keyboard, as they did
before the pad existed. `[Controller] FixHintDeviceList=0` puts the fault back.

**What it took, and what it refused.** Seven explanations were tried and refused by test before this one: the
number of records; records moving when ours are inserted; a text buffer overrun; the forty-by-three hint matrix
(hooked, and the crash survived); nameless rows (`NameStringId` gave every input a real string and the crash
stayed); the input itself (`ForceCode`, `SwapSticks`); and one picture drawn for a stick pair (`SplitWalkSticks` -
the interface atlas, texture 5_014c, does hold a stick icon, but splitting the pair across two sticks changed
nothing). What found it was comparing with the Xbox build, which has a pad and therefore a working version of the
same path - and that comparison is what showed the expander was in the executable all along.

4. ~~**Rumble** through slot 19.~~ **Closed in play.** That diagnostic earned its keep: the first run showed the
   engine reading the pad 240 times in two seconds and asking for rumble **not once**, which pointed away from
   our slot and at the mixer. The mixer shakes only the devices named in a two-slot list inline in the input
   manager (`+0x6C`, length at `+0x74`), its constructor fills both slots with -1, and the one write to that
   length in the whole executable is that constructor - so **the PC build has no rumble at all**, patch or no
   patch. The pad now lists itself there each frame (the engine empties the list when it resets). Measured
   afterwards: the list held one slot, both entries -1, and the mixer then arrived 120 times in two seconds
   asking for levels of its own, 0.315 and 0.543 on the two motors at one hit, 0.378 and 0.614 at another.
5. ~~**Walking** with the speed record of section 3.~~ **Closed in play.** The engine does the work; we only had
   to answer. At `Suffering.rfl+0x10045DD0` the player's speed is read as

   ```
   10045de3  push 0x411                   ; the speed action
   10045def  call [edx + 0x94]            ; is it bound to anything?
   10045dfc  je   0x10045e1b              ; no -> the other path
   10045e07  push 0x411                   ; yes: read its value, mode 1
   10045e0d  call [eax + 0x98]
   10045e13  fsubr dword ptr [0x1014b628] ; speed = 1.0 - value    (the constant reads 1.0)
   ```

   so the pad reports **1 minus how far the move stick is pushed** and the subtraction hands the push back as the
   speed: at the edge 1 and runs, half way 0.5 and walks. The push is the radial length of the stick halves, with
   the dead zone already taken out, which is the console's measure. One trap, and the reason the input reports
   **0 at rest and not 1**: the engine answers an action with the *largest* value among its records, so a resting
   stick reporting 1 would pin the speed at zero for the keyboard too, and walking on foot would break the moment
   a pad was plugged in. Reporting 0 at rest leaves the keyboard's own walk key deciding. This is also why the
   walk value is an input of its own (`PadWalkAmount`, index 27) rather than a reuse of the stick-magnitude
   input, which must keep reporting the magnitude itself. `[Controller] AnalogWalk=0` goes back to always running.
6. **The other nine layouts**, the input names for the game's own help text, `[Controller]` options revisited.
   *Closes when each layout plays as its console screen says.* See section 7 for what blocks the names today.

## 7. Why the pad is not named in a hint, and what has to change (measured)

A hint that names an action - "Use Mouse4, Enter or E to open lockers." - is built by the expander at
`SUFFERING.EXE+0x40DEA6`. The chain, read in the executable:

```
0040e0b8  eax = record.Device
0040e0bb  cmp eax, -1                  ; RiotFix patches this to "cmp eax,2 / jae 0x40E107"
0040e0c0  esi = Device * 3
0040e0c3  ebp = [esp + esi*4 + 0x88]   ; the device's list: Count
0040e0ca  ecx = [esp + esi*4 + 0x84]   ;                    Capacity
0040e0d1  esi =  esp + esi*4 + 0x80    ;                    {Data, Capacity, Count}
0040e0e4  call 0x4D1600                ; grows the list when it is full
0040e100  Data[Count] = record.Code
...
0040e244  call [device_vtable + 0x38]  ; row = name table + input*6   (slot 14, 0x40E500)
0040e24b  si = *(word *)row            ; the row's first word: the input's code
0040e25b  bucket = (code * 3) & 0x7F   ; a hash set of codes that are drawn, not spelled
0040e26c  in the set  -> 0x40E29D: write the code into the text as a character, two bytes wide
0040e280  not in it   -> id = *(dword *)(row + 2), then the string manager
```

Two things follow.

**The pad is skipped on purpose, and that is ours.** The lists are growable, so their size was never the limit;
what is limited is how many of them there are. `[Controller] FixHintDeviceList` buys safety by refusing device
2 and above, so the pad is never collected and never named.

The frame, read from the prologue at `0x40DE80` (`push -1` / `push 0x57E03B` / `push fs:0` / `mov fs:0,esp` /
`sub esp,0x88`, then `push ebx,esi,edi,ebp`). Offsets are from `esp` in the body, and the epilogue agrees:
`add esp,0x94` after four pops, and `ret 8`.

```
esp+0x00..0x0F   saved ebp, edi, esi, ebx
esp+0x10..0x7F   locals
esp+0x80..0x8B   lists[0]  {Data, Capacity, Count}
esp+0x8C..0x97   lists[1]
esp+0x98         saved fs:0, the previous SEH frame      <- lists[2].Data would land here
esp+0x9C         the SEH handler, 0x57E03B               <- lists[2].Capacity
esp+0xA0         the __try level                         <- lists[2].Count
esp+0xA4         return address
esp+0xA8         first argument                          (0x40DECB reads it, which confirms the layout)
```

An earlier note here said `esp+0xA0` was "a local the function clears at `0x40DF1A`". That is wrong. The eight
writes to `esp+0xA0` - `0`, `1`, `2`, `3` and `0xFFFFFFFF` at `0x40DF1A`, `0x40DF7E`, `0x40E06E`, `0x40E175`,
`0x40E3EF`, `0x40E41E`, `0x40E446` and `0x40E4A2` - are the compiler tracking which `__try` block is live. So a
third device does not graze a local: **it writes its three fields over the entire exception record**, handler
pointer included. That is why the crash was immediate and total rather than a wrong name on screen.

**Two is designed in, in five places, and all five are immediates.** The array is not a variable-length thing
that happens to have two entries:

| Where | Bytes | What it says |
|---|---|---|
| `0x40E059` | `6A 02` | `push 2` - the element count given to the array constructor at `0x57CD76`, with element size `0xC` and the pair `0x40D8E0` / `0x40D8F0` |
| `0x40E401` | `6A 02` | the same count given to the destructor at `0x57CE48` |
| `0x40E149` | `83 FE 50` | `cmp esi,0x50` - the consumer loop walks the device table at `0x5B5418` in steps of `0x28`, so `0x50` is exactly two devices |
| `0x40E1C1` | `83 FE 02` | a second loop over the same two |
| `0x40E1C6`, `0x40E3C3` | `83 FD 02` | and the two bounds after it |

**The fix, now fully specified.** Point the array at a buffer of ours and raise the five counts. Nothing new
has to be assembled - every site is a displacement or an immediate, and each replacement fits the space it
has:

- the four indexed accesses, `0x40E0C3`, `0x40E0CA`, `0x40E0D1` and `0x40E0F9`, are `base esp + index*4 +
  disp32`, seven bytes each; dropping the base gives `index*4 + disp32`, also seven bytes (`8B 2C B5`,
  `8B 0C B5`, `8D 34 B5`, `8B 0C 85` and the address)
- the constructor's `lea ecx,[esp+0x90]` and the destructor's `lea edx,[esp+0x8C]` are seven bytes; `lea
  reg,[disp32]` is six, so each takes one `nop`
- the loop at `0x40E122` takes `lea edi,[esp+0x88]`, and `0x40E17F` `lea edx,[esp+0x88]`, the same way
- the five counts are one byte each (`02` to the number of devices) and `0x50` becomes `devices * 0x28`

**A correction, and the record of how it happened.** This section once carried a long passage arguing that
moving the list array would not be enough, because the engine-wide device table at `0x5B5418` holds two entries
and seven loops bound it there. The table part was read correctly from the retail executable and was useless:
**RiotFix already moves that table.** `RiotPad.c` relocates it into `MovedArray` with room for eight, rewriting
24 field references and 6 loop bounds, and the log says so on every run - "the device array moved from 0x5b5418
to ... with room for 3 devices: 30 references, none left over". Six of the seven loops were already ours.

The lesson is worth more than the finding: that analysis was done against the untouched exe, and what RiotFix
already patches was never checked. Read the source before reading the binary.

**What is actually left**, and it is only this function's own arithmetic:

- the list array on the stack, the four indexed accesses at `0x40E0C3`, `0x40E0CA`, `0x40E0D1` and `0x40E0F9`,
  and the constructor's and destructor's `lea` at `0x40E05D` and `0x40E405`, each replaceable in its own length
- the counts those two calls pass, `6A 02` at `0x40E059` and `0x40E401`
- the four bounds this function keeps for itself, which `PihBounds` does not cover: `cmp esi,0x50` at `0x40E149`,
  and `cmp esi,2` / `cmp ebp,2` at `0x40E1C1`, `0x40E1C6` and `0x40E3C3`

One thing is still unproven and decides the shape of the buffer: **does this function recurse, or run on more
than one thread?** A static buffer is only safe if it does not, and nothing has checked.

## 8. Ties That Bind

The same, with its own numbers (array `0x6156E8`, speed action `0x400`, slots one further on).

Each stage is a build you test; none of the later ones is written before the earlier one is proven in play.
