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

- The picture can be drawn in black and white, through the shader the game itself carries and uses for its
  security monitors. On the console this is a cheat; on the PC its button sequence was never reachable.
  `[Display] BlackAndWhite=1`. The heads-up display keeps its colour, because the shader is on the scene.

- The scene can be drawn through a colour shader of RiotFix's own - saturation and contrast, mixing towards
  the game's own NTSC grey. Nothing is added to the game and no file of its own is touched: the engine reads
  its pixel shaders as text and assembles them itself, so RiotFix writes a source to the temporary folder and
  points one name in the engine's table at it. `[Display] ColourGrading=1`, `Saturation`, `Contrast`.

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

### 0.6.27

What is in the lettered test builds and not yet in a release. When the number goes up, this heading takes it.

**Audio**

- The log now says which `dsound.dll` the game is actually using, and asks that DirectSound, on the machine it is
  running on, whether it speaks EAX - the EAX 2.0 and 3.0 listener and buffer property sets, and whether each can
  be set or only read. A wrapper in the game's folder (DSOAL, IndirectSound) answers DirectSound's calls through
  something else, and the two ways of asking for reverb are not the same way: these games use `SetFX` with a DMO,
  while a wrapper of that kind implements EAX through `IKsPropertySet`. Nothing is carried across yet - this
  measures whether it could be.
- A sound stopped near its end is no longer reported as cut off. The measure was the percentage played, which
  calls a 356 ms sound with 39 ms left "89% in" and counts it as a defect; it is now the milliseconds still to
  play, and the log prints that number. In one session nine of twenty reported cuts were this - all of them
  leaving 34 to 44 ms, which is a sound reaching its end and being stopped on the next update.
- The "city" reverb environment works again. Windows' own dsound.h gives that preset a late-reverb level above
  the maximum the same header defines, so EAX refused it outright and the environment fell silent; every value
  is now held inside the documented range before it is sent.
- The reverb can now be asked for through EAX, which is what DSOAL offers in place of the DirectSound effects
  it does not implement. Anyone running DSOAL had no reverb at all; now they do, and because EAX sets the room
  once for every 3D sound instead of per buffer, no sound has to be moved into software to carry it.
- RiotFix stops asking for reverb once DirectSound has refused an effect outright (DSERR_FXUNAVAILABLE). It was
  asking for every sound and being refused for every sound, and each request forced its buffer into software for
  nothing. Affects anyone running DSOAL, which implements EAX but not DirectSound effects.
- The log now says who asked for a sound to stop, and whether a fade was asked for with it. The engine's stop
  takes a fade time in seconds and three of the four places that use it push a literal zero, so a sound that ends
  halfway through was not faded out - something asked for silence at once. Each of the four is a different cause,
  and the log now names which one, for both of the engine's stop entry points - the one that takes a sound
  handle and the one that takes the sound itself - and whether the sound it asked about still existed.
  Diagnostic only, behind `[Debug] LogAudio`; the game is left to do exactly what it did.
- `SetFX` now says in the log where the game asked for its effect and what DirectSound answered for it, effect by
  effect. The results array was being passed straight through unread, and when the game supplies none, nobody read
  anything; RiotFix now lends one. An effect asked for in hardware is now asked for in software instead, hardware
  effects having not existed on Windows since Vista.

**Settings**

- A switch written as a word - `true`, `yes`, `on`, `enabled`, and their opposites - is now read as one. It used
  to be read as 0 and the feature stayed off, with nothing in the log to say why, which is the worst kind of
  wrong: the line is in the file, spelled correctly, and reads as "on". No file that already worked changes
  meaning, because every one of those words parsed as 0 before.

### 0.6.26 - 21 November 2026

**Widescreen**

- A field of view far wider than the one asked for, after the first cutscene of a new game, with
  `ThirdPersonFieldOfView` set and `OriginalCutsceneFraming=1` (issue #2). Two mechanisms were working against
  each other: cutscene framing changes the lens by multiplying whatever each camera currently holds, while the
  chosen field of view is written as an absolute angle - and that angle was always converted as though the
  widescreen lens constant were in force, including during a cutscene, when it is not. The level sets its angle
  while the opening cutscene has the view, so the camera already carried the widescreen correction, and the
  handover at the end of the cutscene applied it a second time. The angle is now converted against the constant
  actually in force, which is the same number as before whenever no cutscene is involved. Loading a save was
  unaffected and still is: there the level's call happens with no cutscene in the way.

### 0.6.25 - 16 November 2026

**Shader**

- The shader settings have a section of their own, `[Shader]`: `BlackAndWhite`, `ColourGrading`,
  `Saturation`, `Contrast`, `ConsoleRange`, `ShaderFile` and `ShaderReloadKey`. They arrived in `[Display]`
  one at a time and had outgrown it. An .ini that still has them under `[Display]` keeps working - each key
  is looked for in `[Shader]` first and in ``[Display]` only if the new section does not answer - and the log
  says once that the old section is in use. `RefreshRate` stays in `[Display]`, having nothing to do with
  shaders.

- `Saturation` and `Contrast` above 1.0 were doing nothing at all. The .ini offered a range up to 2.0 and the
  code accepted it, but a pixel shader of this version takes no constant outside -1 to 1, and one that holds
  such a constant is refused when it is created - without an error, because the engine simply keeps the shader
  it already had. So the setting read as ignored rather than as broken. Each constant is now written as half
  of its value and doubled back by the instruction itself, which reaches 2.0 with every constant legal. The
  range the .ini promises is now the range that works.
- A shader of your own, `[Display] ShaderFile`. A bare name is looked for in a `CustomShader` folder beside
  the game; an absolute path still works. The engine resolves shader names against its own folder, so a name
  that stays inside it needs none of the climbing the temporary folder required.
- `[Display] ShaderReloadKey` reloads the scene shader while the game runs, through the engine's own loader.
  Edit the `.psh` in a text editor, tap the key, and the change is on screen. It reloads all twenty shaders
  and does not free what it replaces, so a long session leaks a little; off unless asked for.
- `ColourGrading=1` with a shader that could not be installed turned the picture black and white instead of
  leaving it alone, because the field that switches the slot on was being written whether or not the slot held
  our shader.

### 0.6.18 - 16 November 2026

**Display**

- A colour shader of RiotFix's own over the scene: `[Display] ColourGrading=1` with `Saturation` and
  `Contrast`. It mixes towards the same NTSC grey the game's own black-and-white shader uses, so grey is the
  shade the game itself would pick. The heads-up display keeps its colour, as with black and white.
- Two builds crashed before this one worked, both of them calling the engine's shader assembler directly on an
  interface read out of its call site. The third does not call it at all: the engine loads every name in its
  own table with its own code, so RiotFix writes the source to the temporary folder and changes one name. The
  name has to be relative, because the resolver concatenates it onto the game's folder with no case for a
  drive letter, so it is built as a climb out of that folder; both must be on one drive, and the log says so
  when they are not.

### 0.6.14 - 15 November 2026

**Display**

- Black and white, `[Display] BlackAndWhite=1`. Not a filter of RiotFix's own: the engine loads a
  black-and-white pixel shader every run and draws the scene with it when one field in the renderer is set -
  which is what the game's own security monitors do. On the console the same field is the Murnau Mode cheat,
  entered as a controller button sequence this build never had a controller for, so it has been loaded and
  unreachable since release. The heads-up display keeps its colour, because the shader sits on the scene pass
  rather than on the finished frame.

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
