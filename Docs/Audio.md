# Audio in The Suffering — what was wrong, and how it was found

Written after the fixes landed, from the logs that proved them. Every number here is measured in play unless it is
marked as an estimate or a hypothesis. Addresses are The Suffering (PIH) unless stated; Ties That Bind differs
where noted.

## The framing

Almost none of this was a bug in 2004. The game was written for a PC that existed then: a sound card that mixed in
hardware, DirectSound 3D with hardware acceleration, and a voice budget that matched what a SoundBlaster Live! gave
you. Windows removed that floor in Vista. What presents today as "the audio is broken" is, for the most part,
correct 2004 code running on a system that changed underneath it.

Two of the five findings below are genuine engine mistakes. The other three are the game asking for a machine that
no longer exists.

## A sound has to pass four gates

A play request starts at the engine's own sound system and only reaches the sound card if it survives all four.
Three of them were closed. They stack, which is why fixing any one of them alone changed nothing audible — the
sound simply died at the next gate. That is the single most important thing to understand about this
investigation.

### Gate 1 — a hardware buffer that cannot exist

Every 3D sound is created with `DSBCAPS_LOCHARDWARE`: "give me a buffer on the card". Windows has had no hardware
audio mixing since Vista — DirectSound became an emulation layer over WASAPI — so those creations simply failed and
the sound never got a buffer at all.

**Measured: 4406 buffer-creation failures in one session. With `[Audio] SoftwareBuffers=1`, zero.**

Fix: clear `DSBCAPS_LOCHARDWARE` as the buffer is created.

### Gate 2 — twenty voices

The engine reserves 20 voices in all and 8 for sounds placed in 3D. It does know how to raise that: it reads how
many voices the card reports in hardware and lifts its own limit to match. No card has reported hardware voices
since Vista, so it is permanently stuck at 20 and 8.

The refusal happens **inside the engine**, before DirectSound is asked for anything, so it leaves no trace in any
log on the Windows side.

**Measured: 3765 of 4032 real play requests refused for want of a voice — 93.4%. With 64 and 32, 6 of 279 — 2.2%.
Raising it further (128/64) gained nothing: 6 of 296.**

This was the big one. Nine of every ten sounds in the game died here.

Fix: `[Audio] Voices` and `ThreeDimensionalVoices` write the two counts (`VoiceCount`,
`ThreeDimensionalVoiceCount` in the per-game address table) before the sound system reads them.

### Gate 3 — the audible-distance limit

Before anything else, the engine compares the sound's distance against a limit of its own and refuses outright if
it is past it, unless the sound carries the always-audible flag (`test bl,0x10`). The limit is a float at
`0x5C6DF8`, compared with `fcomp`, and comes out at about 102400 units in play. It is not written as a constant
anywhere in either binary.

**Measured in one place, same sounds asked for the same number of times: 5 refusals of 14 with the game's limit,
0 of 15 with the limit at 1000000.**

Fix: `[Audio] AudibleDistance` raises the float only for the length of the call that tests it, and restores it
after, so nothing else in the engine ever sees a different world.

### Gate 4 — interface sounds created in 3D and never placed

The subtlest of the four, and the one that produced the strangest symptom.

The engine gives every sound it positions a minimum **and** maximum distance of 1e9. That pair switches
DirectSound's own distance attenuation off and leaves the volume the engine worked out alone — the engine does its
own attenuation and writes the result with `SetVolume`.

Interface sounds — a page turning in the archive, picking up a map or a note — are created **with** 3D control but
are never given a position or a distance, because a menu sound is not in the world. From the engine's side that is
reasonable. But the buffer then keeps DirectSound's factory defaults: the origin, and a minimum distance of 1 unit.

The levels put the player hundreds of thousands to millions of units from the origin. At a minimum distance of 1,
that attenuates the sound by something like 123 dB. It plays, DirectSound reports success, and nobody hears it.

**Measured: in one session, of 61 sounds played in 3D, exactly one was in this state — and it was the sound
reported missing. In a longer session, 24 of them, all from `Interface.sdu`: `5_001c` (page turn, 10274 bytes),
`5_001e` (map pickup, 9992 bytes), and four others of 10944 to 14144 bytes, all 22050 Hz mono, 226–320 ms.**

This is what made the symptom look like madness: whether you hear an interface sound depends on how far you are
standing from the level origin. Near it, audible. Away from it, silent. Same sound, same action, different place.

Fix: `[Audio] FixUnplacedSounds` turns 3D off (`DS3DMODE_DISABLE`) for a sound still at DirectSound's defaults at
the moment it is told to play. Turning it off is right rather than inventing a distance: a sound with no position
is not meant to be anywhere, and a large minimum distance would leave 3D on and pan it towards an origin it was
never meant to sit at.

**The mode belongs to the buffer, not to the sound.** The engine pools buffers and hands the same one to a
different sound later — that is what the voice counts are counting. The first build wrote the mode once and left
it there, which fixed the cutscene sound and took the position away from sounds that reused that buffer. The mode
is now derived from the buffer's state at every play and never left behind: off for a sound with no position, back
on for a sound that has one.

*Falsified hypothesis, recorded because the measurement was worth having:* buffer reuse was expected to show up in
the counter for modes turned back on. Over a full session it stayed at 0. So in practice the engine does not hand a
disabled buffer to a positioned sound — but the guard costs nothing and the leak it prevents is real.

## Gate 5, off to the side — reverb

Not a gate, but part of the same picture.

Ties That Bind asks DirectSound for the I3DL2 reverb its levels choose. Windows 11 no longer registers that effect
class, while the other effects in the same `dsdmo.dll` still are. The Suffering is worse: it *clears* the flag that
opens its own reverb path, so it runs dry by its own code's decision.

Fix: `[Audio] Reverb` hands the game the Waves reverb instead and carries the environment over by the two values
both effects share — how loud the reverb is and how long it takes to die away. Close, not identical.
`[Audio] EngineReverb` can set the flag PIH clears, which makes the game's own path run and succeed on every call,
but it does not sound good; it is off on purpose.

## Why it took so long to see

Gates 2 and 3 refuse the sound inside the engine, before DirectSound is called. No audio profiler, no system log,
nothing on the Windows side observes it happening. From outside, the only symptom is "that sound sometimes doesn't
play".

The investigation only became possible after hooking the engine's own play request:

- Sound-system vtable `0x58FC10`.
- Slot 40 (`+0xA0`) = `0x441410`, **4 stack arguments**, `ret 0x10`, ends at `0x4416EF`.
- Slots 41 and 42 (`+0xA4`, `+0xA8`) = `0x441B00` and `0x441A80`, 6 arguments each, `ret 0x18`.

`0x441410` has **seven** exits to `xor eax,eax` at `0x4416E6` — seven distinct ways of saying no: sound system
null; `[system+0x2C]` zero; sound id argument zero; `[this+0x666]` set; `[this+0x446]` at or below zero; no free
voice after the search loop; and the distance test. A return of 0 is a refusal; anything else is a voice.

Only after logging which exit fired was it possible to separate "out of voices" from "too far away" from "never
placed". Counting what the engine answered, rather than what the sound card did, is the whole method here.

**Method note, learned the hard way:** the argument counts above were measured with `Scripts/Args.py`, which finds
a function's real end (the `ret` after which no branch of the function points ahead). An earlier attempt derived
"6 arguments" for `0x441410` by linear disassembly to the first `ret`, which crosses into the next function. The
resulting hook crashed the game. Linear disassembly does not find function boundaries.

## Identifying a sound from a log

`.sdu` packages decode as: magic `SRSC`, then records of a 0x40-byte header followed by decoded PCM, each record
aligned to 0x20. In the header, `+0x2C` holds channels in the low word and bits in the high word, `+0x30` the
sample rate, `+0x38` the data size. The word at `+0x0C` is **not** a format tag — one package holds eleven distinct
values there. Streamed packages (levels, World Common, NPCs) keep a variable-size block between records, so a
reader has to resync on the header signature rather than walk by size.

Package ids come from the `id` line of each `.db`: System 2, FX 3, Resources 4, Interface 5, World Common 7,
NPCs 10, 1 PrisonDeathRow 12. Resource ids are `pkg<<16 | index`, but the index is **not** the record's position in
the `.sdu`.

`Scripts/NameSounds.py` builds a (bytes, rate, channels) table from every package in the game folder and annotates
a `RiotFix.log` with the package and record of each sound, so a log can be read by sound instead of by byte count.
On a sample log: 1958 sounds across 7 packages, 79 named, 10 ambiguous, 4 missing.

SurrealEd remains the fastest way to find which sound a script actually plays — faster than working back from a
log. `5_001c` and `5_001e` were found that way after a wrong identification from exported `.wav` sizes.

## Two bugs that were RiotFix's own, not the game's

Both are recorded here because they cost a full evening and both masqueraded as audio problems.

**`Voices` silently never applied.** An earlier edit left the `Voices`, `ThreeDimensionalVoices` and
`EngineReverb` blocks *inside* the loop that parses `[Debug] WatchSound`. With `WatchSound` empty the loop body
never runs, so the three options were never read however they were set in the `.ini`, and the engine kept its 20/8
voices. It went unnoticed because the diagnostic sessions had `WatchSound` filled in, which made the options apply
by accident; the moment `WatchSound` was cleared, the voice fix vanished with it. Any measurement from a session
without `WatchSound` set was taken on a game running at 20/8.

The log now prints the counts read back from memory, before and after the write, so an option that fails to apply
shows up in one line rather than in a refusal rate three sessions later.

**Nothing that waits may run in `DllMain`.** Twice, at both ends:

- `LoadLibrary` for `SDL3.dll` from `DLL_PROCESS_ATTACH` — recursive loader lock, the game hung at startup. Fixed
  by deferring the load to the first frame.
- `SDL_Quit` and `FreeLibrary` from `DLL_PROCESS_DETACH` — `SDL_Quit` waits for the joystick and audio threads,
  and those threads need the loader lock `DllMain` is holding. The process stayed alive in the task list after the
  game closed, with the last lines of the log never written. Fixed by doing no SDL shutdown when `Reserved` is
  non-null, which is how Windows signals that the process itself is ending; the OS reclaims the threads and unmaps
  the libraries anyway.

A log that ends at the audio summary instead of `Game closed normally.` means `DllMain` is stuck again.

## What is still open

- **The DirectSound 3D listener has never been hooked.** There is not one line about it in any log. The engine's
  sound positions are known; where the game says the player is listening from is not. Until that is measured,
  anything about panning or about which origin the distance test uses is hypothesis.
- **Hypothesis worth testing:** if the origin of the gate-3 distance test is the camera rather than the player,
  then the camera snapping in close near a wall would change which sounds pass it without the player moving. That
  would tie the audio intermittency to the widescreen camera snap, which is also still open. Hooking
  `IDirectSound3DListener::SetPosition` and comparing it with the camera position already logged by
  `[Debug] LogCameras=1` would settle it in one session.
- The engine's effect gate reports `sound system flags 00, so effects are REFUSED for every sound`. Understood
  well enough to work around, not well enough to explain.

## The two reverb presets the game carries (measured, PIH)

The engine does not read reverb parameters from a file or from level data: it builds a `DSFXI3DL2Reverb`
field by field, with literal values, at two places in the executable.

| Field | `0x4424BE` | `0x4438FB` |
|---|---|---|
| lRoom | -1000 | -1000 |
| lRoomHF | -100 | -454 |
| flRoomRolloffFactor | 0 | 0 |
| flDecayTime | 1.49 | 0.4 |
| flDecayHFRatio | 0.83 | 0.83 |
| lReflections | -2602 | -1646 |
| flReflectionsDelay | 0.007 | 0.002 |
| lReverb | 200 | 53 |
| flReverbDelay | 0.011 | 0.003 |
| flDiffusion | 100 | 100 |
| flDensity | 100 | 100 |
| flHFReference | 5000 | 5000 |

The first is the I3DL2 **Generic** preset and the second is **Room**, both matching the reference values of
the specification digit for digit. Nothing here was tuned for this game.

There are exactly two. `flHFReference` is 5000 in every I3DL2 preset, and a search of the whole executable
for that constant returns two hits, both of them these - so no third preset is hiding anywhere in the code.

What this settles: there are no bespoke reverb values of The Suffering's own to recover from the console
builds, because the PC build's own values are the published reference ones. What a console build could still
answer, and this does not, is **which preset is chosen where** - whether a cell, a corridor and a yard get
different reverb, which would be content rather than numbers.

## Why a one-shot gets cut off (measured: log, Cheat Engine stacks, Ghidra)

The counter used to call every stopped one-shot "cut short". Split by how much had played, a session of 107
one-shots is **53 that had all but finished** - stopping those is housekeeping - and **54 genuinely cut off**.
Only the second number is a defect, and conflating them is what let a wrong theory survive a log that
contradicted it.

Two call sites do the stopping, both reaching the buffer's own `Stop` through the sound object kept at
`[this+0xA6]`. Both belong to one class, whose vtable is at `0x58FD98`:

| Return address in the log | Function | Slot | What it is |
|---|---|---|---|
| `+0x42ABA` | `0x442A50` | 10 | takes a "force" argument, sets flag `0x200`, then stops |
| `+0x42A3F` | `0x442A20` | 12 | stops, no condition at all |

Neither decides anything. The float at `+0xEE` tested against `0.0` in slot 10 is only reached when flag
`0x200` was **already** set, so for a sound that is simply playing both paths go straight to stopping. Whatever
chooses is above them, and a Cheat Engine breakpoint gave the chain that static reading could not:

```
0x486EF0   call [vtable + 0x90]   (slot 36) - a predicate on the owning object; 0 means leave the sound alone
0x486F01   push 0 / push [esi+0x24] / ecx = 0x5C9478 / call 0x441200
0x441200   the sound manager: walk its array at [this+0x43E], count at [this+0x446], for this exact sound
0x44127A   found it -> call 0x43AEF0(sound, force = 0)
0x43AF24   call [vtable + 0x28]   (slot 10) -> the buffer's Stop
```

`0x5C9478` is the sound manager, confirmed live: `EDI` held it at the breakpoint. The search is for one
particular sound, not for a victim - **this is not voice stealing.** Some object decides its own sound should
end, and asks the manager to end it.

The two stops also came in on **different threads** (`53D0` and `4B20`), which is worth remembering anywhere
else a static buffer is being considered.

What is still unknown: which class `esi` is at `0x486EF0`, and what its slot 36 asks. Reading `[esi]` at a
breakpoint there gives the vtable, and the editor's class table can turn that into a name.

The object that orders it: its vtable is at **`0x592028`** - 41 slots, `0x592028`-`0x5920C8`, installed by the
constructors at `0x48683C` and `0x487170`, which sit in the same stretch of code as the stop itself. The
executable carries no RTTI, so the binary gives no class name; that has to come from the engine's own class
table.

**Slot 36, at `0x484D90`, is five instructions, and it is the whole decision:**

```
mov eax,[ecx+0x28]    ; the object's flags
shr eax,6
not eax
and eax,1             ; return 1 when bit 0x40 is CLEAR
ret
```

So the sequence at `0x486EE6` reads: if bit `0x10` of the flags is set, leave the sound alone; otherwise ask
slot 36, and **stop the sound when bit `0x40` is clear**. Just before it, the same code clears bit `0x8` and
writes 1 to `[this+0xF0]` - the object is finishing something, and takes its sound down with it unless bit
`0x40` says not to.

That is the defect's exact condition, and it is not an accident of voices or distance: it is a deliberate
stop, on a flag.

What bit `0x40` means is **not known**. No instruction anywhere in the executable sets or clears it directly
(`or`/`and` on `[reg+0x28]` with `0x40` finds nothing), so it is written as part of a whole word from
somewhere else, or through another base. Finding that, and the class's name, are what is left.

A caution about how this address was reached. It was first read off a screenshot as `0x590028`, a digit short
of the truth. That address could not be a vtable base - the code pointers around it end at `0x590088`, so slot
36 fell past the end and read as zero - and rather than stop there, a nearby base, `0x58FFE0`, was derived to
fit and even "confirmed" by the constructors that write it. It was simply a different class. The real value,
`0x592028`, came from the user reading the register out as text. A plausible reconstruction is not a
measurement, and the second one was worse than the first because it looked checked.

Three different threads have now been seen in this code (`53D0`, `4B20`, `4FE8`).

## The other stop, the one that is actually audible

The chain above explains the stops at 70-89%, which nobody hears. The harsh ones - 8%, 9%, 32% - come from the
other call site, `+0x42A3F`, which is slot 12, the unconditional stop. Cheat Engine's stack panel came back
empty there twice, because the game is built without frame pointers (`EBP` held the same value at breakpoints in
four different functions), so nothing chains. Reading `[ESP]` at the first instruction gives the caller
directly, and stepping out from there gave the whole chain:

```
0x442A20                 slot 12, the unconditional stop
SUFFERING.EXE+0x3AECE
SUFFERING.EXE+0x416D9
Suffering.rfl+0x12BFC0   <- the decision leaves the engine here
Suffering.rfl+0x12CB56
Suffering.rfl+0xB4A7
Suffering.rfl+0x12CCB7
SUFFERING.EXE+0x55A56
```

**Four frames of it are in `Suffering.rfl`.** The audible cuts are ordered by game logic, not by the engine's
audio system - which is why nothing in the sound code explained them.

`0x43AEC0`, the engine side of the bridge, is a teardown rather than a pause:

```
test byte [esi+0xc],0x20   ; already torn down? leave
call [eax+0x30]            ; slot 12: stop the sound          <- returns to 0x43AECE
mov eax,[esi+0x4]          ; the sound resource
call [edx+0x40]            ; release it
mov [esi+0x4],0
or [esi+0xc],0x20          ; mark as done
```

And the `.rfl` frame sits immediately after `call [edx+0xa0]` on the manager held at `0x1017BF78`, with a
distance test right before it: a sum of squares compared against `[esi+0x1a]`, taking the call only when the
listener is **inside** that radius.

Proven: the chain, the teardown, and that a radius test stands immediately before the call that leads to it.
**Not proven:** that the radius test is what decides the teardown. The call is made when in range, which is the
opposite of what a cull would do, so slot `0xA0` is more likely an update that replaces whatever sound the
emitter was holding - and tears the old one down on the way. Reading slot `0xA0` of the manager at
`0x1017BF78` is the next step, and it is static work.

## The second argument of the manager's stop is a fade time (proven, static)

The chain above showed `push 0 / push [esi+0x24] / call 0x441200` and left the first push unexplained - a flag
of some kind, with one other call site pushing `0x3F800000` instead. It is a **fade time in seconds**, and the
first instructions of `0x441200` say so:

```
0x44121E  fld   [esp+0x10]        ; the second argument, as a float
0x441222  fcomp [0x58C2AC]        ; 0.0f
0x44122D  jne   0x441242          ; <= 0 -> the immediate path
0x44122F  push  it / ecx = sound / call 0x43AF80    ; > 0 -> the fade path
```

`0x43AF80` is the fade itself, and it is a volume ramp rather than a stop:

```
mov [ecx+0x68],2                      ; state 2 = winding down
fade != 0 -> [ecx+0x50] = ([0x58C988] - [ecx+0x24]) / fade   ; units per second
fade == 0 -> [ecx+0x50] = 1000.0f                            ; i.e. immediately
```

So the manager offers two ways out, and **which one a caller picks is readable statically**. All four callers,
read at their push:

| caller | fade | what the code around it does |
|---|---|---|
| `0x44EBAB` | `0` | stops now, then drops the sound from the list at `[esi+0x4C]` (`[esi+0x54]` decremented) |
| `0x44EC7E` | `0` | stops now, in a loop over that whole list (stride `0xC`) - "stop everything I own" |
| `0x4732C4` | `1.0f` | **the only fade in the executable**, while unlinking a node |
| `0x486F09` | `0` | stops now - the owner teardown, slot 36 and bit `0x40`, documented above |

**Three of the four cut hard**, and a session bears it out: 21 requests, 18 of them with no fade at all.

One correction, because this was first written too broadly. *This* entry point almost never fades - but the
fade itself, `0x43AF80`, has five callers, and only one of them (`0x441236`) is inside `0x441200`. The engine
fades in four other places that never pass through the handle-based stop, so "the engine has a fade and almost
never uses it" was wrong as a general claim and is only true of this one door.

The two call sites the Stop hook reports are the vtable at `0x58FD98` (slots `0x58FD98`-`0x58FDE8`, installed
by `0x442546` and `0x442E9F`):

| hook reports | function | slot | reached from |
|---|---|---|---|
| `+0x42ABA` | `0x442A50` | 10 | `0x43AEF0` (the immediate path) and the per-frame poll |
| `+0x42A3F` | `0x442A20` | 12 | the `.rfl` chain documented above |

Both end at `call [ecx+0x48]` on the buffer held at `[this+0xA6]`, which is `IDirectSoundBuffer::Stop`.
Slot 10 is also the poll that asks whether a wind-down has finished - on a sound that is simply playing it
stops it outright, and only on a later call, with flag `0x200` already set, does it look at `[this+0xEE]` and
return 1 when the ramp is over. That is why slot 10 appears in the log both for hard stops and for ordinary
endings, and why the caller alone cannot tell them apart. Separating them needs the fade argument, which means
hooking `0x441200` itself.

## The reverb, with DSOAL installed (measured on the reporter's machine, 0.6.26b)

`DirectSound reports 128 hardware buffers` and the reporter confirms DSOAL. Every one of 28 reverb requests
from the game failed, and the new line says how:

```
SetFX I3DL2 reverb ... DSERR_FXUNAVAILABLE, asked in no particular place, failed
```

**"asked in no particular place" refutes the fix that shipped in 0.6.26b.** The guess was that the game asked
for its effect in hardware - the documented cause of `DSERR_FXUNAVAILABLE` being an effect "expected in
hardware and found in software" - and that rewriting the request to software would place it. The game asks
with neither `DSFX_LOCHARDWARE` nor `DSFX_LOCSOFTWARE` set, so the rewrite never fires and changes nothing.
That code is dead and should be read as such until something proves otherwise.

What is left standing: DSOAL implements EAX through `IKsPropertySet` and says nothing about `SetFX`; the game
uses `SetFX` with a DMO and never touches `IKsPropertySet`. The two speak past each other. That is a
hypothesis, and the EAX probe added in 0.6.26c decides it - on this machine, not by reasoning. If the probe
answers that the EAX 2.0 listener can be set, the reverb can be carried across; if it answers "not offered"
there too, the cause is something else again.

The buffer pattern is confirmed here as well: the effects buffer is always exactly 5000 ms longer than the dry
one (856 -> 5856, 1739 -> 6739, 3460 -> 8460, 4359 -> 9359, and so on through the session).

## The whole stop graph, and what the hook actually caught (measured, 0.6.26h)

The hook went on the four call sites and the session came back with 21 requests against 34 one-shots genuinely
cut off. It cannot be the main cause, and the per-caller results say why:

| caller | calls | fade | result |
|---|---|---|---|
| `0x44EBAB` | 13 | none | **`-2` every single time** |
| `0x44EC7E` | 4 | none | `0` three times, `-2` once |
| `0x4732C4` | 3 | 1.00 s | `0` |
| `0x486F09` | 1 | none | `0` |

`-2` is `0x441200`'s own "no such sound" exit at `0x441215`. So `0x44EBAB`, the busiest caller by far, **stops
nothing**: it asks for a sound that has already gone, thirteen times out of thirteen. It is bookkeeping, not a
defect, and it should never have been a suspect. And the owner teardown at `0x486F09` - the chain that took a
whole morning in Cheat Engine, slot 36 and bit `0x40` - fired **once**, exactly as the slot-39 hook did in the
session before it. Four stops were actually carried out immediately through this door, against 34 cuts.

So the door is the wrong one, and the call graph says which the right one is. `0x43AEF0`, the immediate stop
that reaches slot 10, has exactly **two** callers:

```
0x441145   inside 0x4410F0    the sound manager, by sound OBJECT      <- six call sites, none hooked
0x44127A   inside 0x441200    the sound manager, by sound HANDLE      <- four call sites, hooked
```

`0x4410F0` takes `ret 0xC` - three arguments - finds the object in the same array at `[this+0x43E]`, and passes
the third argument straight to `0x43AEF0` as its force flag. Its six callers are `0x43EBF0`, `0x43EDE2`,
`0x43EE23`, `0x43F2CF`, `0x441580` and `0x441987`. Since those two are the only ways into `0x43AEF0`, and the
hooked door accounts for four stops, **every other immediate stop in the session came through `0x4410F0`**.
That is not a guess about behaviour; it is the call graph being exhaustive. Hooking those six the same way
finishes the immediate half of the graph.

## A percentage is the wrong measure for a short sound

Of the 20 stops the log named, nine leave between 34 and 44 ms of audio unplayed:

| length | stopped at | left |
|---|---|---|
| 212 ms | 84% | ~34 ms |
| 356 ms | 89% | ~39 ms |
| 370 ms | 89% | ~41 ms |
| 293 ms | 85% | ~44 ms |

They cluster that tightly because they are all the same event - the sound reached its end and the stop arrived
on the next update - and roughly 40 ms is how often that update runs. None of them is audible. But the
classifier asks for 90% played, and a 356 ms sound with 39 ms left is only 89%, so all nine were counted as
"genuinely cut off" and inflated the number that matters.

**Milliseconds left is the honest measure, and the percentage is not**: 40 ms left is inaudible whatever the
sound's length, and 400 ms left is audible whatever it is. The other eleven leave between 341 ms and 2158 ms,
which is the real defect, and they separate cleanly - there is nothing between 44 ms and 341 ms in the whole
session. Any threshold in that gap works; the measure is what was wrong, not the number.

## Both doors measured, and the busiest one is a reaper (0.6.26i)

With the ten call sites hooked and the measure changed to milliseconds left:

```
Audio: of those one-shots, 46 had finished (under 100 ms left) and 25 were genuinely cut off
Audio: the engine asked the sound manager to stop 30 sounds by handle, 28 of them at once with no
       fade at all, and 25 for a sound that had already gone
Audio: and 334 by sound object, 26 of those forced, 0 for a sound that had already gone
```

The two doors could not be less alike. The handle door is almost entirely misses - 25 of 30, and
`0x44EBAB` alone asked 22 times and got "no such sound" 22 times, after 13 of 13 in the session before.
Twice measured, it stops nothing. The object door asked 334 times and **found its sound every single time**.

Every one of the 334 came from one site, `0x441987`, inside `0x441700`, and the code around it says what
that function is:

```
0x441978   mov ecx,edi / call 0x43CEB0      ; advance this sound; non-zero means it is done
0x44197F   test eax,eax / je 0x441993
0x441981   push 0 / push ebx / push edi     ; force 0, the index, the sound
0x441987   call 0x4410F0                    ; remove it
0x44198C   test eax,eax / jne 0x441993
0x441990   dec ebx                          ; the slot was removed, so visit it again
```

A sweep over the manager's list that advances each sound and collects the finished ones - and `0x43CEB0` is
the same function that drives the fade machine at `0x43AFC0`. So **the busiest stop path in the game is
housekeeping**: the engine collecting sounds it has already judged to be over. Force is always 0 and nothing
is ever missing, exactly as a reaper over one's own list should look. `0x441700` itself has four callers:
`0x404763`, `0x4501CA`, `0x451108`, `0x48C1D5`.

Worth noting for reading these logs: the request line is written *after* the original call returns, so the
`Stop` line it caused appears **above** it. Lines 432/433 and 660/661 of that session are each one event, and
they are the two cuts that can be attributed with certainty - both to `0x44EC7E`.

### Where the 25 cuts actually are

**Seventeen of the 25 are a single burst at exit**, all of them 1501 ms sounds, in the last seventeen lines
before the session summary. During play there were eight:

| sound | left | in | caller |
|---|---|---|---|
| 4359 ms | 239 ms | 94% | slot 10 - matched to `0x44EC7E` |
| 1416 ms | 1416 ms | 0% | slot 12 |
| 1416 ms | 466 ms | 67% | slot 10 |
| 3173 ms | 1733 ms | 45% | slot 10 - matched to `0x44EC7E` |
| 3309 ms | 3078 ms | 6% | slot 10 |
| 1416 ms | 1185 ms | 16% | slot 12 |
| 806 ms | 806 ms | 0% | slot 12 |
| 1416 ms | 556 ms | 60% | slot 10 |

Eight in a session, not 34, and the teardown burst is separable from them by where it sits. The 4359 ms sound
losing 239 ms also shows the new measure earning its keep: under "90% played" that one was invisible.

### Why the other six could not be attributed, and the fix

The object door's log cap of 60 lines was reached at line 593, and every cut after it - including five of the
eight - happened later. Raising the cap is the wrong answer: 334 near-identical lines from one reaper is noise
whichever number is chosen.

Join the two hooks instead. Both doors run the `Stop` they cause **inside** their own call and on the same
thread, so a per-thread note of who is asking - the call site and which door - can be set before the original
is called and read by the `Stop` hook, which then names the requester in the line that matters. The repetitive
reaper lines go away, every cut carries its cause, and a cut with **no** requester becomes proof that it came
through neither door: the `.rfl` chain into slot 12, or the fade machine. That last case is the open question,
so the instrument should be built to answer it.

Use `TlsAlloc` rather than `__thread` for that note. RiotFix.asi is loaded dynamically, and static thread-local
storage in a dynamically loaded DLL is the fragile case on Windows; the explicit API has no such problem.

## Why the reverb disappears under DSOAL: measured, both ways, same machine

The reporter ran 0.6.26d twice on one machine, changing only whether DSOAL was in the game's folder.

| | Windows' own `dsound.dll` | DSOAL in the game's folder |
|---|---|---|
| DirectSound reports | 1 hardware buffer, 0 free, 0 in 3D | **128 hardware buffers, 128 free, 128 in 3D** |
| `SetFX` calls | 41 | 64 |
| of those, failed | **0** - all "ok, placed in software" | **64** - every one `DSERR_FXUNAVAILABLE` |
| reverb put on | 41 sounds | **0 sounds, refused on 64** |

That is the whole answer, and it is not a configuration problem or a RiotFix defect. **DSOAL does not implement
`IDirectSoundBuffer8::SetFX`** - the DMO effects path RiotFix's reverb is built on. It implements EAX, through
`IKsPropertySet`, which is a different interface entirely. The 128 free hardware buffers in the second column
are DSOAL announcing itself; the 64 refusals in the row below are the same library saying it has no DMOs.

So the reporter's own summary is exactly right: DSOAL fixes the bug in his video and takes the reverb with it.
Until RiotFix can speak EAX, DSOAL plus `[Audio] Reverb=0` is the configuration to recommend to anyone who has
the original fault.

### What it was costing, and the change

The refusals themselves are cheap. The flag behind them is not: to ask for an effect, a buffer must be created
with `DSBCAPS_CTRLFX`, and that forces it into software. On the DSOAL run, 64 buffers were pushed out of the
128 hardware voices DSOAL had just offered, in exchange for a reverb that was applied to none of them.

One `DSERR_FXUNAVAILABLE` is enough to know the rest of the session will go the same way, so from 0.6.26j the
first refusal latches: no more `SetFX`, no more `DSBCAPS_CTRLFX`, no padding, and one line in the log saying
so. Buffers after that are created exactly as the game asked.

### What is still open, and why the old probe could not answer it

Both of those sessions report EAX 2.0 and 3.0 as "not offered", listener and buffer - **including the one with
DSOAL installed**. That does not settle anything, because 0.6.26d's probe asked only the plain buffer, and the
`IDirectSound3DBuffer` fallback arrived in 0.6.26g. Both canonical EAX 2.0 listings obtain `IKsPropertySet`
from the 3D interface, not from the buffer, so the old probe was asking the wrong object on exactly the
machine where the answer matters.

A 0.6.26g-or-later run on that machine, with DSOAL in place, decides it in one line:

```
Audio: the property set came from the buffer's 3D interface
Audio: EAX 2.0 listener: offered
```

If that is what comes back, the EAX translation is unblocked and the reverb can come back on DSOAL machines.

## The DSOAL run, and a probe that answered before it had asked (0.6.26j)

DSOAL installed locally, confirmed by two lines:

```
Audio: DirectSound is C:\...\The Suffering\DSOUND.dll
Audio: DirectSound reports 128 hardware buffers (128 free), 128 3D hardware buffers
Audio: the property set came from the buffer
Audio: EAX 2.0 listener: not offered        (and the other three the same)
```

**That result is not conclusive, and the fault is ours.** Two things went wrong in the probe, and both are
visible in the log itself.

It stopped at the first door that opened. The code asked the buffer, and only on failure would it have asked
the buffer's 3D interface - the buffer answered, so the 3D interface was never asked at all.

And the buffer it asked was the **primary buffer**. The probe's lines sit immediately above
`[audio] buffer 0 bytes (0 ms, 0 Hz, 0 channels), flags 00000011 3d`, and `0x11` is
`DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D`: the first buffer the game creates. So the one object ever questioned
was the primary buffer, through its plain interface.

The doors never knocked on are the ones the documentation points at. Microsoft's own page obtains the property
set by calling `QueryInterface` on a **DirectSound 3D buffer**. The listener half of EAX belongs with
`IDirectSound3DListener`, which is reached from the primary buffer - and Wine had to add `IKsPropertySet` to
its primary buffer as a separate change, years after the secondary-buffer one already worked, which is proof
that an implementation can answer at one door and be silent at another without being broken.

From 0.6.26k the probe asks all four and names each one:

```
Audio: the primary buffer, EAX 2.0 listener: ...
Audio: the 3D listener, EAX 2.0 listener: ...
Audio: a 3D sound buffer, EAX 2.0 buffer: ...
Audio: that buffer's 3D interface, EAX 2.0 buffer: ...
```

### Two things the run settled anyway

`0x44EC7E` was caught in the act six times. Six cut lines are each followed immediately by its request with
result 0 - and because the request is logged after the original returns, those pairs are one event apiece. It
is the "stop everything I own" loop, and it is so far the only named cause of audible cuts:

| sound | left unplayed |
|---|---|
| 2207 ms | 1217 ms |
| 3906 ms | 3376 ms |
| 2255 ms | 1925 ms |
| 3561 ms | 791 ms |
| 1104 ms | 724 ms |
| 3599 ms | 3559 ms |

And **DSOAL changes nothing about the cuts**. Different sessions, so only the ratio is comparable, and the
ratio is the same to a decimal: 38 of 108 one-shots cut with DSOAL, 25 of 71 without - 35% both times. The
backend is not involved; this is engine logic, as the call graph already said. 23 of the 38 were the exit
burst again.

## DSOAL does offer EAX - on the one door the old probe never knocked on (0.6.26k)

Same machine, same DSOAL, the probe now asking all four:

```
Audio: the primary buffer, EAX 2.0 listener: not offered      (and the other three)
Audio: the 3D listener, EAX 2.0 listener: not offered         (and the other three)
Audio: a 3D sound buffer, EAX 2.0 listener: can be read and set
Audio: a 3D sound buffer, EAX 2.0 buffer: can be read and set
Audio: a 3D sound buffer, EAX 3.0 listener: can be read and set
Audio: a 3D sound buffer, EAX 3.0 buffer: can be read and set
Audio: that buffer's 3D interface, (all four): can be read and set
```

**EAX 2.0 and 3.0, listener and buffer, readable and settable** - through a secondary 3D sound buffer, or
through that buffer's `IDirectSound3DBuffer`. Nothing at all on the primary buffer or on the 3D listener,
which is precisely where the earlier probe had been looking. Every "not offered" before this was our own
instrument answering for an object that was never going to know.

So the picture is complete and consistent. DSOAL has no DirectSound effects at all (`SetFX`: 64 of 64
refused), and full EAX on its 3D buffers. The reverb does not disappear under DSOAL because something is
broken; it disappears because RiotFix only knows how to ask the way DSOAL does not implement.

### What the translation needs

The nine numbers line up one for one. `XAUDIO2FX_REVERB_I3DL2_PARAMETERS` is Room, RoomHF, RoomRolloffFactor,
DecayTime, DecayHFRatio, Reflections, ReflectionsDelay, Reverb, ReverbDelay, Diffusion, Density, HFReference;
`EAXLISTENERPROPERTIES` opens with `lRoom`, `lRoomHF`, `flRoomRolloffFactor`, `flDecayTime`, `flDecayHFRatio`,
`lReflections` and carries on the same way, in the same units. The settings RiotFix already holds as
`DSFXI3DL2Reverb` can be copied across field by field.

What differs is the tail. EAX has `dwEnvironment`, `flEnvironmentSize`, `flEnvironmentDiffusion`,
`flAirAbsorptionHF` and `dwFlags` where I3DL2 has `flDiffusion`, `flDensity` and `flHFReference`. Arx Fatalis,
a real EAX 2.0 user, hardcodes `dwEnvironment = 0` and `dwFlags = EAXLISTENERFLAGS_DECAYHFLIMIT`, which is a
working precedent for the two fields with no I3DL2 counterpart.

**Not written yet, and not to be written from memory.** The full field order of `EAXLISTENERPROPERTIES` and the
numeric values of the `EAXLISTENERFLAGS_*` constants have to come from a copy of `eax.h` - ideally DSOAL's own,
since DSOAL is what will be reading the bytes. Getting one field's offset wrong writes a decay time into a
rolloff factor and the result is silence or noise with nothing in the log to explain it.

## The reverb through EAX (0.6.26l)

Written against Creative's EAX 3.0 header as shipped in id Software's released Return to Castle Wolfenstein
source, because DSOAL defines none of this itself: its `eax.h` carries the GUIDs and the property enum and
nothing else, and a grep of the whole tree for `flRoomRolloffFactor` finds nothing. The bytes go through to
the OpenAL driver by a loaded `EAXSetDirect`, so the layout that matters is Creative's.

Two things the header corrected. The struct is **EAX 3.0** - `ulEnvironment` leads, and `lRoomLF`,
`flDecayLFRatio`, the two pan vectors, echo and modulation are all 3.0 additions - which suits us, since the
probe found 3.0 settable as well. And `DSPROPERTY_EAXLISTENER_ALLPARAMETERS` is **1**, not the 2 this code had
been asking about: 2 is `ENVIRONMENT`. The probe's answer still stood, because a property set that answers at
all is a property set that exists, but the number was wrong and is now right.

The mapping, nine fields straight across:

| DSFXI3DL2Reverb | EAXLISTENERPROPERTIES |
|---|---|
| `lRoom`, `lRoomHF` | the same |
| `flDecayTime`, `flDecayHFRatio` | the same |
| `lReflections`, `flReflectionsDelay` | the same |
| `lReverb`, `flReverbDelay` | the same |
| `flRoomRolloffFactor` | the same |
| `flHFReference` | the same (clamped to the header's 1000..20000) |
| `flDiffusion` (percent) | `flEnvironmentDiffusion` (0..1) |
| `flDensity` | no counterpart - dropped |

`lRoomLF`, `flDecayLFRatio`, the pans, echo, modulation, `flAirAbsorptionHF` and `flLFReference` take the
header's own defaults. `ulFlags` is `EAXLISTENER_DEFAULTFLAGS`, which is the six scaling flags the header
names, `0x3F`.

`ulEnvironment` is set to `EAX_ENVIRONMENT_UNDEFINED` (26) so no preset is laid over the numbers that follow
it. **That is the one choice the header does not make for us**, and it is the first thing to question if a
room comes out sounding like somewhere else.

### What changes besides the sound

EAX is a listener property: one call sets the room for every 3D sound at once. SetFX had to be put on each
buffer, and to take an effect a buffer needs `DSBCAPS_CTRLFX`, which forces it into software. So on a DSOAL
machine this stops pushing buffers out of the 128 hardware voices DSOAL offers - the reverb arrives and the
voices stay. When the EAX listener is settable, `SetFX` is not asked for at all.

Cycling the environment bumps a counter that the next 3D buffer notices, so the key still works.

**Confirmed by ear**, which is the only test that counts here. With DSOAL installed and the F2 key walking
the environments, the difference between them is plainly audible - `paddedcell` at 0.17 s of decay against
`hangar` at 10.05 s is a factor of sixty, and it sounds like it. The reverb reaches the speakers on exactly
the machines where `SetFX` never could.

The log side: `reverb "stonecorridor" set through EAX 3.0 (level -1000, decay 2.70 s): ok`, once, on the
first 3D buffer, with zero `SetFX` calls in a session of 1147 buffers and 929 plays.

That long session also exercised two object-door call sites that had never fired before, `0x43EBF0` and
`0x43EDE2` (the second with force set), so all six are reachable in ordinary play. And the cut rate fell to
29 of 215 one-shots - 13%, against 35 to 43% in the short sessions - which says the earlier rates were an
artefact of sessions that were mostly level loading and cutscene boundaries.

## The F2 walk, and the one environment EAX threw out (0.6.26l -> 0.6.26m)

Nine environments sent while walking. Eight accepted, one refused:

```
[audio] reverb environment: city
[audio] reverb refused by EAX 3.0: 0x80004005
```

`0x80004005` is `E_FAIL`, and the reason is in `dsound.h`, which contradicts itself. It defines
`DSFX_I3DL2REVERB_REVERB_MAX` as **2000**, and then gives the CITY preset `lReverb = 2217`. The EAX header's
`EAXLISTENER_MAXREVERB` is 2000 as well, so EAX threw the whole structure out. Checked against all fifteen
presets, CITY is the **only** one with any field outside EAX's limits.

The first guess was a lost minus sign - `-2217` - and the data refuted it: GENERIC is 200, BATHROOM 1030,
SEWERPIPE 648 and UNDERWATER 1700, so a positive late reverb is ordinary in this table and CITY is simply 217
over the line. Clamping to 2000 is the only legal reading and costs 2.17 dB on that one environment. From
0.6.26m every field is held to the header's limits before the call, so a preset or an .ini override can no
longer lose the whole structure.

### The refusal is also the proof the field order is right

A misaligned structure would have been refused for nearly every environment, not one. This table carries
`flHFReference = 5000` and levels in the -10000..0 range, and the EAX fields those would land on if the
layout were off by a slot have maxima of 1.0, 0.1 and 0.3 - so almost everything would have come back
`E_FAIL`. Eight accepted, and exactly the one value that violates exactly the limit on the field it was
written to refused: that is what a correct layout looks like. It is a natural experiment, and it cost nothing
to run.

## EAX on stock Windows, with no DSOAL (control, 0.6.26g)

Measured on a machine with `C:\WINDOWS\SYSTEM32\DSOUND.dll` and no DSOAL:

```
Audio: the property set came from the buffer
Audio: EAX 2.0 listener: not offered
Audio: EAX 2.0 buffer: not offered
Audio: EAX 3.0 listener: not offered
Audio: EAX 3.0 buffer: not offered
```

Two things are settled by it. The probe reaches a real `IKsPropertySet` - **the plain buffer answers, so the
`IDirectSound3DBuffer` detour added in 0.6.26g is a fallback and not the normal route** - and Windows' own
DirectSound offers no EAX at all, listener or buffer, which is expected: hardware audio acceleration was
removed in Vista and nothing emulates EAX in its place. So "not offered" on an ordinary machine means nothing
is wrong; it is the baseline every other measurement is read against. Only a machine with DSOAL can answer
whether the EAX path exists for this game.

The same session also repeats, on plain DirectSound, the engine's own refusal: `sound system flags 00, so
effects are REFUSED for every sound`, 215 buffers created, 0 of them asking for an effect, 0 `SetFX` calls.
Whatever reverb this game is going to get, the game is not going to ask for it.
