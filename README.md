# RiotFix

Runtime fixes and improvements for **The Suffering** (2004) and **The Suffering: Ties That Bind** (2005), PC versions (addresses checked against the GOG executables).

RiotFix is a `dinput8.dll` proxy (or `.asi` plugin) that patches the game in memory at start-up. **No game file is changed.** Every patch checks the original bytes before touching anything, and anything that does not match is skipped and reported in `RiotFix.log`. One file works for both games.

> Current version: **0.3.8** (work in progress — feedback welcome, see [Testing](#testing)).

## Installation

**With an .asi loader** (recommended if the game already has one): copy `RiotFix.asi` and `RiotFix.ini` to the folder where the loader looks for `.asi` files (the exe folder or `scripts`).

**Without a loader:** copy `dinput8.dll` and `RiotFix.ini` to the game folder (next to `Suffering.exe` / `SufferingTTB.exe`).

Do not use both at the same time, and do not overwrite an existing `dinput8.dll` (it may be the `.asi` loader itself).

Every run writes `RiotFix.log` next to the plugin. To undo everything, delete the files you copied.

Copy `SDL3.dll` beside it too: controllers are read through SDL3. Without it RiotFix still works, with no controller.

All options live in `RiotFix.ini`; each one is documented there. `0` / `1` turn a fix off / on.

Fixes with no reason to be turned off are always on and are not listed in `RiotFix.ini`. Each still reads its old
key, so one that misbehaves on a given machine can be turned off by adding that key with `0`:
`[System]` SkipFrameDebugOutput, SkipRegistryFlush, SkipDirectPlayCheck, FixDiskSpace;
`[Display]` ConventionalMessageLoop, WaitInConfigurationDialog, AllResolutions, DpiAware, FixFrontBufferCopy;
`[Widescreen]` FixInventoryIcons; `[Mouse]` PreciseTime, CursorDirect, CursorPreciseTime;
`[Audio]` SoftwareBuffers, FixUnplacedSounds, CarryEngineReverb; `[Timing]` FixLoadingDeadlock; `[Debug]` CrashLog;
`[Sdl]` Enable. `[Audio]` Voices / ThreeDimensionalVoices (64 / 32) and AudibleDistance (1000000) are fixed the same
way; `0` gives the game's own value back.

## Features and fixes

### Widescreen (`[Widescreen]`)
- **Hor+ field of view:** wide screens show more at the sides instead of cutting the top and bottom. Cutscenes keep the game's own framing.
- **`ThirdPersonFieldOfView` / `FirstPersonFieldOfView`:** set the horizontal field of view in degrees for any resolution, with `Enable=1` (first-person option: Ties That Bind only for now). The ini lists the games' own angles (67° in The Suffering; 69° and 79° in Ties That Bind, on a 4:3 screen) and how `Enable` widens them, so you can pick a number or keep the original framing.
- **HUD kept at its original proportions** and anchored to its side of the screen (left, centre, right) instead of being stretched; each string of text keeps one anchor so it is never split.
- **Menus in a centred 4:3 box**, loading screens included.
- **HUD stays at the sides behind the pause menu** (only the menu goes into the 4:3 box).
- **Ties That Bind interface scene** (health bar, ammunition, weapon panel) anchored per element, also while paused.
- **`FixInventoryIcons`** (The Suffering): the 3D weapon icons in the inventory were placed with the wrong camera angles, so they landed beside their square and got hidden behind the scenery (a bug of the original game at any resolution). They now sit inside their square, follow the HUD layout and no longer disappear behind objects; inventory cells slide as one piece.
- **`OriginalCutsceneFraming`:** cutscenes keep the field of view they were framed with instead of showing more at the sides (what a wide screen loses is the top and bottom, where the cutscene's black bars already are).
- **`CutsceneBars`:** keep or remove the black bars of cutscenes.

### Accessibility (`[Accessibility]`)
- **`InterfaceScale`:** one size for the HUD, its text and the mouse cursor (1 = the game's, up to 3). Elements grow away from their corner or side, the crosshair stays centred, menus keep their size. It works with the widescreen off.
- **Text outline:** an even outline around the text instead of the game's offset shadow, for subtitles, HUD, items and menus (`OutlineSize`, `OutlineColor`, `OutlineShadow`).
- **`OutlineSkipFonts`:** fonts left without an outline (by default the hand-drawn menu font).

### Mouse (`[Mouse]`)
- **`PreciseTime`:** the game timed mouse movement with a 15.6 ms clock, so some frames measured 0 ms, lost their movement and repeated the previous one. Mouse time now comes from a precise timer.
- **`CursorDirect`:** the menu cursor kept moving for about 5 frames after the mouse stopped (the game used the largest of the last 10 readings). It now follows the newest reading.
- **`CursorPreciseTime`:** the cursor moved by the game's frame time, which comes in 15.6 ms steps and counted about one frame in 16 as 31-32 ms, so the cursor jumped twice as far on those frames (felt as uneven or smoothed movement, mostly in Ties That Bind menus). It now uses the frame time measured with a precise timer.
- The menu cursor is drawn at the screen's size (the game drew its 480-line texture as it was), smoothly when enlarged and no longer squashed in the 4:3 menu box. Its size follows `[Accessibility] InterfaceScale`.

### Display (`[Display]`)
- **`AllResolutions`:** lists every resolution the graphics card offers, in full screen and in windowed mode (the original hides very wide modes and limits windowed mode to 640x480–1600x1200).
- **`DpiAware`:** Windows no longer scales the game window on scaled desktops (125%, 150%…).
- **`ConventionalMessageLoop`:** the game handles every pending window message each frame, so Windows no longer flags it as "not responding".
- **`WaitInConfigurationDialog`:** the Riot Engine configuration dialog no longer uses a whole processor core.
- **`FixFrontBufferCopy`:** a screen copy without its texture is skipped instead of crashing (seen in windowed mode).

### Window (`[Window]`)
- **`Borderless`:** in windowed mode the window has no border or title bar, keeps the size of the chosen resolution and is centred (pick the desktop's resolution for a borderless full screen). Full screen is untouched.
- **`ConfineCursor` / `HideCursor`:** the mouse stays inside the window while the game has the focus, and Windows' own cursor is hidden over it.
- **`IdleWhenInactive`:** with the window inactive the game sleeps between frames instead of using a whole processor core.
- **`Title`:** text of the title bar and of the taskbar button (The Suffering left it blank).

### Audio (`[Audio]`)

The engine turns a sound down twice before DirectSound is ever asked, and a sound refused in either place leaves no
trace in any log of the sound card. Both were found by hooking the engine's own play request (`SUFFERING.EXE+0x441410`,
slot 40 of the sound system) and counting what it answered.

- **`Voices` / `ThreeDimensionalVoices`** - the engine gives itself 20 voices and 8 placed in 3D, decided when
  hardware mixing still existed, and raises them only if the sound card reports more, which no card has done since
  Windows Vista. **Measured: with the game's own numbers, 3765 of 4032 real sound requests were refused for want of a
  voice - 93.4%. With 64 and 32, 6 of 279 - 2.2%.**
- **`AudibleDistance`** - past a limit of about 102400 units the engine refuses a sound outright unless it is marked
  always audible. Raised only for the length of the call that tests it. Measured in one place with the same sounds
  asked for the same number of times: 5 refusals of 14 with the game's limit, 0 of 15 at 1000000.
- **`FixUnplacedSounds`** - the engine gives every sound it positions a minimum distance of 1000000000, which
  switches DirectSound's own attenuation off and leaves the volume it worked out alone. The sounds a cutscene
  sequence plays are made with 3D control but never given a position or a distance, so they keep DirectSound's
  defaults: the origin, and a minimum distance of 1 unit. The levels put the player around a million units from the
  origin, which attenuates those sounds by something like 123 dB - they play, DirectSound reports success, and
  nobody hears them. **Measured: of 61 sounds played in 3D in one session, exactly one was in this state, and it was
  the one reported missing.** A sound with no position is not meant to be anywhere, so its 3D is turned off rather
  than a distance invented for it. The mode belongs to the buffer and the engine reuses buffers, so it is
  decided from the buffer's state at every play and never left behind.
- **`SoftwareBuffers`** - every 3D sound is asked for with `DSBCAPS_LOCHARDWARE` and Windows has had no hardware
  mixing since Vista, so those buffers were never created: 4406 failures in one logged session, 0 with this on.
- **`Reverb` and `CarryEngineReverb`** - Ties That Bind asks DirectSound for the reverb its levels choose, but
  Windows 11 no longer registers the effect class it asks for while the others in the same `dsdmo.dll` still are;
  RiotFix hands it the Waves reverb instead. The Suffering clears the flag that opens its own reverb path, so there
  `Reverb` adds one of fifteen environments instead.

`Scripts/NameSounds.py` reads the game's `.sdu` packages and writes the package and record of each sound next to the
sizes in `RiotFix.log`, so a log can be read by sound rather than by byte count.

`Docs/Audio.md` has the whole picture: the four gates a sound has to pass, the measurement behind each one, how the
engine's play request was hooked to find them, the `.sdu` format, and what is still open.

### Stability and system (`[System]`, `[Timing]`)
- **`SkipDirectPlayCheck`:** Direct3D is created without loading DirectPlay, so the game starts on Windows 8 and later without the "DirectX is not installed" / install-DirectPlay prompt.
- **`MaxFrameRate`:** caps the frame rate (60 by default, 0 = no limit). Both games misbehave above 60 frames per second: the physics and some animations follow the frame time with no cap of their own.
- **`FixLoadingDeadlock`:** models and resources are always locked in the same order (level loads could hang on multi-core processors).
- **`FixDiskSpace`:** reports at most 2 GB of free disk space (with a lot of free space the game misread the value and could refuse to save).
- **`SkipRegistryFlush`:** skips the forced registry flushes that only block the game.
- **`SkipFrameDebugOutput`:** skips the empty debug message the game sends every frame.
- **`SkipIntro`:** skips the start-up logo movie.

### Memory (`[Memory]`)
- **Bigger resource caches:** the budget of each cache (models, textures, animations, sounds, strings, lighting…) can be raised, so the game unloads and reloads resources less often. Levels that ask for less keep the configured budget. `LogUsage=1` logs each cache's peak usage to choose values from real numbers.
- **The Suffering:** large shadows no longer fill the small texture cache (each 1024 shadow used to count as 4 MB, which made the loader recycle textures until it crashed).

### Graphics (`[Shadows]`, `[LOD]`, `[Draw]`)
- **Sharper shadows:** shadow texture size per character (original 256, up to 2048), with a matching depth buffer when the shadow is larger than the screen.
- **Shadow options:** fade distance, who casts shadows (player / characters / everything), minimum light angle for longer shadows.
- **Level of detail:** characters and objects keep their full mesh at longer distances.
- **Draw distance** (The Suffering): object draw distance multiplier.

### Diagnostics (`[Debug]`)
- **Crash log:** crashes are logged with location, registers, stack and process memory, and `RiotFix.dmp` is written.
- Optional logs for testing: `LogCameras`, `LogHud`, `LogMouse`, `LogAxes`, `Trace`.

## Testing

When reporting a problem, please include:
- which game (The Suffering or Ties That Bind) and your resolution;
- which build, and the last build where it was right;
- `RiotFix.log` from the game folder, and `RiotFix.dmp` as well if it crashed;
- a screenshot if it is visual — two, if it changes during a transition.

`Docs/Reporting.md` says what each of those answers, when the ordinary crash dump is not enough, and why the
stack list printed in the log is a sweep rather than a call stack. Worth reading once before the first report.

Known open items:
- `FirstPersonFieldOfView` is not available in The Suffering yet.

## Building

Requires MinGW-w64 (i686). Run `Build.sh`; the output is `Build/dinput8.dll` and `Build/RiotFix.asi` (the same file). The code is plain C, split into modules (`RiotCore`, `RiotDisplay`, `RiotAccessibility`, `RiotWindow`, `RiotInput`, `RiotMemory`, `RiotRendering`, `RiotSystem`, `RiotTiming`, `RiotDiagnostics`), each with its own per-game address table.

`RiotFix.ini` is the only place options are explained. `RiotFix.PIH.ini` and `RiotFix.TTB.ini` are written from it by `Scripts/GameInis.py`, with each game's recommended values; edit the main file and run the script.

`RiotPatch.py` can write the shadow and LOD changes directly into the executable and set its Large Address Aware flag (optional; RiotFix itself changes no file).

## Credits

- Widescreen and several system fixes follow the methods of **FusionFix / WidescreenFixesPack by ThirteenAG** (MIT), reimplemented in C from the original executables.

## Licence

MIT.
