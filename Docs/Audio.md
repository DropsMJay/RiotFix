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
