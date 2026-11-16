# SDL3 in RiotFix — plan

Controller support first, audio later. Written before any code; every number here that is not marked as measured
is an estimate.

## What the games actually have

Measured in both binaries:

- Both exes import **only** `DirectInput8Create`. No XInput, no WinMM joystick, no HID.
- `GUID_SysKeyboard` and `GUID_SysMouse` are present (PIH `0x59784C` and `0x59785C`, TTB `0x5F3740` and
  `0x5F3750`). `GUID_Joystick` is not.
- There is not one joystick, gamepad, XInput or analog-axis string anywhere in either exe or .rfl.

So there is nothing to re-enable. The console versions' controller support was removed from the PC build, not
disabled. Whatever RiotFix does has to **synthesize** input the game already understands.

The Interface package still carries the console button art: texture `5_014c`, 64x64 DXT5, with Y, B, X, A, L and R
on it. And the games build their help text from placeholders expanded per device (`GUICancel` and its neighbours,
in the .rfl's name table at `0x101674D8`), so a path that once swapped key names for button names is still there.

## The decision that blocks everything: static or dynamic

**Static** — SDL3 compiled into `dinput8.dll` / `RiotFix.asi`.
Nothing extra to copy, which is the whole shape of this project today: one file and an .ini. The DLL grows by
roughly 1.5 to 2 MB. We have to build SDL3 ourselves for i686 MinGW, which is a one-off cost.

**Dynamic** — `SDL3.dll` beside the game.
Smaller build, trivial to swap SDL versions while testing. But it is a second file the user must place, and a
loose `SDL3.dll` next to the exe can be picked up by other mods in the same folder.

Recommendation: **static**. The cost is a one-time build; the benefit is that the project keeps its "copy one
file" shape. Open for the user to decide.

## The two layers

**Layer A — reading the controller.** SDL3 with `SDL_INIT_GAMEPAD` only, no video subsystem, no window of its own.
`SDL_OpenGamepad`, polled from the frame callback RiotFix already registers (`RiotTimingOnFrame`). Hot-plug comes
free with SDL's event queue. DualSense, Xbox, Switch Pro and everything else in SDL's database work the same way,
which is the reason for using SDL at all rather than XInput.

**Layer B — handing it to the game.** Two routes, and they are not exclusive:

- **B1, DirectInput device injection.** Wrap `IDirectInput8::CreateDevice` and hand the game our own
  `IDirectInputDevice8` for `GUID_SysKeyboard` and `GUID_SysMouse`. `GetDeviceState` and `GetDeviceData` return the
  real device's answer with our synthetic presses merged in. The game cannot tell the difference, every one of its
  own key bindings keeps working, and nothing in the engine has to be understood first. Digital only.
- **B2, the engine's own input path.** For real analog movement and camera, the stick value has to reach whatever
  the engine uses for the mouse delta and the movement axes. That needs the input path reverse-engineered first.

B1 proves the whole chain end to end and is where to start. B2 is what makes it feel like the console version.

## Milestones

Each one is testable on its own, in the game, which is how this project works.

**M0 — the toolchain.** Build SDL3 for i686 MinGW, link it statically, and write the gamepads it finds to
`RiotFix.log` (name, type, serial). The game is not touched at all. This alone proves the build, the static link
and that SDL runs inside a DLL loaded as `dinput8.dll`.

**M1 — one button, one key.** Wrap `CreateDevice` for the keyboard only and make one gamepad button appear as one
key. If the game reacts, every later milestone is bookkeeping.

**M2 — the full digital map.** A `[Controller]` section in the .ini: a line per button, naming the key it stands
for, so the user maps it to whatever they have bound in the game. Face buttons, shoulders, d-pad, sticks as
four-way digital, triggers with a threshold.

**M3 — analog.** Right stick to camera through the mouse device wrapper (relative deltas are what the mouse
already delivers), left stick to movement. Movement stays digital until B2 is understood; the mouse route should
give a usable camera long before that.

**M4 — button prompts.** The help text already expands per device, and the button art is already in the game's
Interface package. This is the part that makes it look native rather than bolted on.

**M5 — audio, later.** SDL3's audio would replace the DirectSound path and end the dependency on effect classes
Windows no longer registers, and the voice limits would become ours. Worth doing only after the controller work
is stable; it is a much larger change and today's two fixes (`Voices`, `AudibleDistance`) already recovered what
was being lost.

## Risks worth naming now

- **Recursion.** SDL's Windows joystick backend can itself use DirectInput, and our DLL *is* `dinput8.dll`. If SDL
  calls `DirectInput8Create`, it calls us. The backend has to be chosen deliberately with SDL hints (raw input and
  HIDAPI, DirectInput off) and the result verified, not assumed.
- **No window of our own.** SDL must not create one. `SDL_INIT_GAMEPAD` alone, and the game's window keeps owning
  the message loop that RiotFix already replaces.
- **Threading.** The event pump runs on the frame callback, on the game's thread. No SDL call from any other.
- **A wrapped keyboard is on the hot path.** `GetDeviceState` runs every frame; the wrapper has to be cheap and
  must never fail in a way the game does not expect. A bug here does not lose a sound, it loses the controls.
- **Size.** If the static link turns out much larger than 2 MB, the dynamic option comes back on the table.

## What is needed before writing code

1. Static or dynamic.
2. Which game M1 targets first — the same work applies to both, but the address tables are per game.
