# RiotFix

Runtime fixes and improvements for **The Suffering** (2004) and **The Suffering: Ties That Bind** (2005), PC versions (addresses checked against the GOG executables).

RiotFix is a `dinput8.dll` proxy (or `.asi` plugin) that patches the game in memory at start-up. **No game file is changed.** Every patch checks the original bytes before touching anything, and anything that does not match is skipped and reported in `RiotFix.log`. One file works for both games.

> Current version: **0.2.5** (work in progress — feedback welcome).

## Installation

**With an .asi loader** (recommended if the game already has one): copy `RiotFix.asi` and `RiotFix.ini` to the folder where the loader looks for `.asi` files (the exe folder or `scripts`).

**Without a loader:** copy `dinput8.dll` and `RiotFix.ini` to the game folder (next to `Suffering.exe` / `SufferingTTB.exe`).

Do not use both at the same time, and do not overwrite an existing `dinput8.dll` (it may be the `.asi` loader itself).

Every run writes `RiotFix.log` next to the plugin. To undo everything, delete the files you copied.

All options live in `RiotFix.ini`; each one is documented there. `0` / `1` turn a fix off / on.

## Features and fixes

## System

- The game starts on Windows 8 and later without asking for DirectPlay ("DirectX is not installed").
- Windows no longer flags the game as "not responding".
- Level loading no longer hangs on multi-core processors.
- The game no longer refuses to save on disks with a lot of free space.
- The Riot Engine configuration dialog no longer uses a whole processor core.
- A screen copy that closed the game (seen in windowed mode) is now skipped.
- The game no longer forces registry flushes or sends an empty debug message every frame.
- The game's process no longer stays stuck in Task Manager after the game closes.
- A frame-rate cap (60 by default), because above that the physics and some animations of both games go wrong.
- The start-up logo movie can be skipped.

## Display

- Every resolution the graphics card offers is listed, in full screen and in a window.
- Windows no longer enlarges the game window on scaled desktops (125%, 150%…).
- The full-screen refresh rate can be chosen.
- A borderless, centred windowed mode with the cursor kept inside the window; while the window is inactive the game
  rests instead of using a whole core.
- The title bar shows the game's name (the first game left it blank) and the RiotFix version, with the build's date
  and time.
- Wide screens show more at the sides instead of cutting the top and bottom.
- The field of view can be set in degrees, in third person and (Ties That Bind only) in first person.
- The HUD keeps its original proportions and stays on its side of the screen; menus sit in a centred 4:3 box.
- While the game is paused, the HUD stays at the sides behind the menu.
- In Ties That Bind, the health bar, ammunition and weapon panel are placed correctly too.
- Cutscenes keep their original framing, and their black bars can be kept or removed. The game now knows for certain
  when a cutscene starts and ends, so the picture no longer flickers at the edges on those changes.
- Notes, photos and maps on the archive screen take the same share of the screen at any resolution (at 1080p notes
  were too small to read).
- The Suffering: the 3D weapon icons in the inventory appear inside their squares (a bug of the original game, at
  any resolution).

## Accessibility

- One size for the HUD, its text and the cursor, up to three times the original.
- An even outline on text (subtitles, HUD, items) instead of the game's offset shadow.

## Controls

- Mouse movement no longer loses frames or repeats the previous one.
- The menu cursor stops with the mouse and no longer jumps; it is drawn at the screen's size and is not squashed in
  the 4:3 box.
- Controller support, which the PC version never had:
  - the five console layouts, as on the PS2's settings screen;
  - sticks to move and look; buttons to attack, jump, throw, reload, use, heal, first person, insanity,
    flashlight, map, pause and weapon changes;
  - in menus, cross accepts, circle or triangle goes back, the d-pad moves;
  - buttons follow the keys the player has bound in the game;
  - keyboard and controller work at the same time;
  - adjustable dead zone, look speed, and stick swap and inversion.

## Audio

- 3D sounds that Windows refused to create since Vista are now created.
- The game had too few voices and refused about 9 sounds in 10; with more voices almost none are lost.
- Distant sounds the game refused now play.
- Cutscene and interface sounds that were never heard now play.
- Ties That Bind: the levels' reverb works again (Windows 11 no longer has the effect the game asks for, so RiotFix
  hands it another). The Suffering gains an optional reverb with fifteen environments.

## Graphics

- Sharper shadows (up to 2048), with options for distance, who casts shadows and light angle.
- The Suffering: large shadows no longer fill the texture cache, which made the game crash.
- Characters and objects keep their full model at longer distances.
- The Suffering: adjustable object draw distance.

## Modding

- The game's resource budgets (models, textures, animations, sounds, strings, lighting…) can be raised, so levels
  can hold more and the game unloads and reloads less. Each level sets its own budgets; one that asks for less keeps
  the configured value, and the log can show each budget's peak use to choose values from real numbers.

## Debug

- A log (RiotFix.log) on every run, with what was applied and what was refused.
- On a crash: the location, the memory state and a .dmp file.
- Optional logs for testing: sounds, cameras, HUD, mouse, controller actions, keyboard, memory use.
- Two games running from the same folder write separate logs.
- RiotPatch.py writes the shadow and level-of-detail changes straight into the executable, and can set its Large
  Address Aware flag (optional; RiotFix itself changes no file).
- Scripts/NameSounds.py reads the game's sound packages and names each sound in the log.
- Scripts/Args.py tells how many arguments a function of the game takes, for writing new hooks.

## Known bugs

- Wide screens: parts of the scenery vanish at the sides (the cells of the first level, for example), only outside
  4:3. The two attempts so far (widening the culling and forcing portals open) did not fix it.
- A separate first-person field of view does not exist in The Suffering yet.
- Controller in Ties That Bind: not tested in play yet.

## Building

Requires MinGW-w64 (i686). Run `Build.sh`; the output is `Build/dinput8.dll` and `Build/RiotFix.asi` (the same file). The code is plain C, split into modules (`RiotCore`, `RiotDisplay`, `RiotInput`, `RiotMemory`, `RiotRendering`, `RiotSystem`, `RiotTiming`, `RiotDiagnostics`), each with its own per-game address table.

`RiotPatch.py` can write the shadow and LOD changes directly into the executable and set its Large Address Aware flag (optional; RiotFix itself changes no file).

## Credits

- Widescreen and several system fixes follow the methods of **FusionFix / WidescreenFixesPack by ThirteenAG** (MIT), reimplemented in C from the original executables.
- uCyborg for the original All-In-One patch

## Licence

MIT.
