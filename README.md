# RiotFix

Runtime fixes and improvements for **The Suffering** (2004) and **The Suffering: Ties That Bind** (2005), PC versions (addresses checked against the GOG executables).

RiotFix is a `dinput8.dll` proxy (or `.asi` plugin) that patches the game in memory at start-up. **No game file is changed.** Every patch checks the original bytes before touching anything, and anything that does not match is skipped and reported in `RiotFix.log`. One file works for both games.

> Current version: **0.0.8** (work in progress — feedback welcome, see [Testing](#testing)).

## Installation

**With an .asi loader** (recommended if the game already has one): copy `RiotFix.asi` and `RiotFix.ini` to the folder where the loader looks for `.asi` files (the exe folder or `scripts`).

**Without a loader:** copy `dinput8.dll` and `RiotFix.ini` to the game folder (next to `Suffering.exe` / `SufferingTTB.exe`).

Do not use both at the same time, and do not overwrite an existing `dinput8.dll` (it may be the `.asi` loader itself).

Every run writes `RiotFix.log` next to the plugin. To undo everything, delete the files you copied.

All options live in `RiotFix.ini`; each one is documented there. `0` / `1` turn a fix off / on.

## Features and fixes

### Widescreen (`[Widescreen]`)
- **Hor+ field of view:** wide screens show more at the sides instead of cutting the top and bottom. Cutscenes keep the game's own framing.
- **`FieldOfView` / `FirstPersonFieldOfView`:** set the horizontal field of view in degrees for any resolution (first-person option: Ties That Bind only for now).
- **HUD kept at its original proportions** and anchored to its side of the screen (left, centre, right) instead of being stretched; each string of text keeps one anchor so it is never split.
- **Menus in a centred 4:3 box**, loading screens included.
- **HUD stays at the sides behind the pause menu** (only the menu goes into the 4:3 box).
- **Ties That Bind interface scene** (health bar, ammunition, weapon panel) anchored per element, also while paused.
- **`HudScale`:** make the in-game HUD and its text bigger (1 = original, up to 3). Elements grow away from their corner; the crosshair stays centred; menus keep their size.
- **`FixInventoryIcons`** (The Suffering): the 3D weapon icons in the inventory were placed with the wrong camera angles, so they landed beside their square and got hidden behind the scenery (a bug of the original game at any resolution). They now sit inside their square, follow the HUD layout and no longer disappear behind objects; inventory cells slide as one piece.
- **`CutsceneBars`:** keep or remove the black bars of cutscenes.

### Text (`[Text]`)
- **Outline:** an even outline around the text instead of the game's offset shadow, for subtitles, HUD, items and menus (`OutlineSize`, `OutlineColor`, `OutlineShadow`).
- **`OutlineSkipFonts`:** fonts left without an outline (by default the hand-drawn menu font).

### Mouse and cursor (`[Mouse]`, `[Cursor]`)
- **`PreciseTime`:** the game timed mouse movement with a 15.6 ms clock, so some frames measured 0 ms, lost their movement and repeated the previous one. Mouse time now comes from a precise timer.
- **`Cursor Direct`:** the menu cursor kept moving for about 5 frames after the mouse stopped (the game used the largest of the last 10 readings). It now follows the newest reading.
- **`Cursor PreciseTime`:** the cursor moved by the game's frame time, which comes in 15.6 ms steps and counted about one frame in 16 as 31-32 ms, so the cursor jumped twice as far on those frames (felt as uneven or smoothed movement, mostly in Ties That Bind menus). It now uses the frame time measured with a precise timer.
- **`Cursor Scale`:** bigger menu cursor (0 = automatic, screen height / 480), drawn smoothly when enlarged and no longer squashed in the 4:3 menu box.

### Display (`[Display]`)
- **`AllResolutions`:** lists every resolution the graphics card offers, in full screen and in windowed mode (the original hides very wide modes and limits windowed mode to 640x480–1600x1200).
- **`DpiAware`:** Windows no longer scales the game window on scaled desktops (125%, 150%…).
- **`ConventionalMessageLoop`:** the game handles every pending window message each frame, so Windows no longer flags it as "not responding".
- **`WaitInConfigurationDialog`:** the Riot Engine configuration dialog no longer uses a whole processor core.
- **`FixFrontBufferCopy`:** a screen copy without its texture is skipped instead of crashing (seen in windowed mode).

### Stability and system (`[System]`, `[Timing]`)
- **`SkipDirectPlayCheck`:** Direct3D is created without loading DirectPlay, so the game starts on Windows 8 and later without the "DirectX is not installed" / install-DirectPlay prompt.
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
- `RiotFix.log` from the game folder;
- a screenshot if it is visual.

Known open items:
- Ties That Bind: menu cursor feel after `Cursor PreciseTime` — reports welcome.
- `FirstPersonFieldOfView` is not available in The Suffering yet.

## Building

Requires MinGW-w64 (i686). Run `Build.sh`; the output is `Build/dinput8.dll` and `Build/RiotFix.asi` (the same file). The code is plain C, split into modules (`RiotCore`, `RiotDisplay`, `RiotInput`, `RiotMemory`, `RiotRendering`, `RiotSystem`, `RiotTiming`, `RiotDiagnostics`), each with its own per-game address table.

`RiotPatch.py` can write the shadow and LOD changes directly into the executable and set its Large Address Aware flag (optional; RiotFix itself changes no file).

## Credits

- Widescreen and several system fixes follow the methods of **FusionFix / WidescreenFixesPack by ThirteenAG** (MIT), reimplemented in C from the original executables.
- uCyborg for the original All-In-One patch

## Licence

MIT.
