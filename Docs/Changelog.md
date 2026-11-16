# RiotFix — what has been fixed and added

Everything RiotFix does in both games (The Suffering and Ties That Bind), by subject, in a player's terms.
The technical detail (addresses, measurements, option names) lives in LEIAME, README and the rest of Docs/.
No game file is changed: everything happens in memory while the game runs.

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
- While the game is paused, the HUD stays at the sides behind the menu, and it stays there through the cross-fade
  that opens a level instead of sitting in the 4:3 box until the fade ends.
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
  - in menus, cross accepts and opens an item, start opens one too, triangle closes, the right stick scrolls a long
    list, and the d-pad and the left stick move between items;
  - the mouse cursor hides while the controller is in use and comes back when the mouse moves;
  - buttons follow the keys the player has bound in the game;
  - keyboard and controller work at the same time;
  - adjustable dead zone, look speed, and stick swap and inversion;
  - rumble, which this version of the game never had: the mixer is there and shakes whatever is listed with
    it, but nothing on the PC ever listed a device, so the pad lists itself and the strength comes from the
    game's own mixer at the ceiling the console used;
  - walking by pushing the stick part way, as the console does it: the engine already works out a player's speed
    as one minus the value of its walk action, so the pad answers that action with how far short of the edge the
    stick is, and at rest it answers nothing so the keyboard's walk key still decides;
  - the pad is not yet named in the game's hints: the fix for the crash below refuses the pad's records, so the
    hint leaves it out rather than crashing. Cause and the real fix are in `Docs/PadPort.md` section 7;
  - the pad is not named in the game's hints yet: the crash fix below refuses the pad's records, so a hint
    leaves it out instead of crashing. The cause and the real fix are in `Docs/PadPort.md` section 7;
  - a crash of the game's own making, where a hint that names an action pair ("use <LadderMove> to rotate crank")
    collected bindings into an array built for two devices and wrote past it once a third existed.
  - being rebuilt as a real input device of the engine, the way the console versions read it, so that the game
    itself works out presses, holds, analog values and rumble (in progress, `Docs/PadPort.md`).

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

## Releases

Everything above is what RiotFix does today. This is when it arrived, newest first, under the same headings.
Only what changed in a release is listed, so a heading with nothing to say is left out.

### 0.6.13 - 15 November 2026

**Controls**

- Walking by pushing the stick part way, as the console does it, instead of always running. The engine already
  worked out a player's speed as one minus the value of its walk action, so the pad answers that action with how
  far short of the edge the stick is; at rest it answers nothing, so the keyboard's walk key still decides.
  `[Controller] AnalogWalk=0` goes back to always running.
- Rumble, which this version of the game never had. The mixer was there and complete, but nothing on the PC ever
  listed a device with it, so it never ran. The pad now lists itself, and the strength comes from the game's own
  mixer at the ceiling the console used.
- Menus close with the top button rather than the right one.
- A crash of the game's own making, where a hint that names an action pair ("use the ladder controls to rotate
  the crank") collected bindings into an array built for two devices and wrote past it once a third existed.

**Display**

- The heads-up display no longer sits in its old position during a level's opening fade and jump to the new one
  when the fade ends.

**Debug**

- A bug-reporting guide, `Docs/Reporting.md`: what to say, which file answers what, and when a full memory dump
  is needed.
- `[Debug] FullCrashDump` writes a dump holding the whole of the game's memory, for a crash the ordinary one
  cannot explain.
- RiotFix could silently drop its own per-frame work when its table was full, which is what stopped the
  screen-text watcher from ever running even with the option on. The table is larger and a refusal is now
  written to the log.

**Modding**

- The game's string file is documented: `COMMON\System\System.xdu`, an `SRSC` header, UTF-16 text with a
  16-bit byte length, and a table of 14-byte records at the end carrying each string's id, offset and length.
  The number SurrealEd shows for a string is that id less one.

## Still Open

- Wide screens: parts of the scenery vanish at the sides (the cells of the first level, for example), only outside
  4:3. The two attempts so far (widening the culling and forcing portals open) did not fix it.
- A separate first-person field of view does not exist in The Suffering yet.
- Controller in Ties That Bind: not tested in play yet.
- The controller is not named in the game's hints. The crash fix above refuses the pad's records rather than
  growing the array the game builds for two devices, so a hint leaves the pad out instead of crashing. The cause
  and the real fix are in `Docs/PadPort.md` section 7.
