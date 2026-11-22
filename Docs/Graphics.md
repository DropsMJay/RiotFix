# Graphics: what could be added, and what each would cost

A planning document, not a record of work done. Every effect below is judged against what has actually been
measured in the two games, and each entry names the one unknown that would change the estimate. Difficulty is
in the same spirit as the changelog: what it would take, in plain terms.

## What the engine gives us to work with (measured)

- **Direct3D 9.** `Direct3DCreate9` is the only renderer. The device is reachable: the renderer object lives at
  `[0x6DD650]` and keeps the device at `+0x268`, which RiotFix already reads.
- **Render to texture already works.** RiotFix hooks the renderer's begin and end (slots 12 and 14, `0x49AF40`
  and `0x49B0F0`) for the shadow pass and supplies its own depth surface when the target is larger than the
  screen. Any effect that needs an off-screen pass inherits this.
- **The engine's own shaders are shader model 1.** Sources are text in `xvsh\`, `vs.1.0` / `vs.1.1` and
  `ps.1.0` to `ps.1.4`, stitched from templates into `ShaderLibrary.bin` with handles in `CachedVSHandles.bin`.
  Those caches are validated by a version float at `0x58C9B4`, **not** by the source files, so editing a `.vsh`
  changes nothing until both `.bin` files are deleted.
- **But D3DX is linked in and knows shader model 2.** `SUFFERING.EXE` carries the version strings `ps.2.0` and
  `vs.2.0` beside the 1.x ones, with `D3DXPSGP` and `D3DXDoNotMute`. So a shader of ours is not held to the
  engine's limit, and needs no `d3dx9_**.dll` on the player's machine. Where the assembler's entry point is has
  not been found yet - that is the first piece of work for anything in the shader tiers below.
- **Radial fog is already written and commented out** in `xvsHeader.vsh`.

## The black-and-white shader is already here (measured)

Reverse engineering for the colour work started at the cheat named Murnau Mode and landed somewhere better.

The engine loads its shaders at startup from a table of names in `.rdata` running `0x58F158` to `0x58F1A7`,
twenty entries, walked by two loops at `SUFFERING.EXE+0x4355B0` and `+0x435650`. The first eighteen are the
vertex shaders; the **last two are pixel shaders**, and they are:

```
0x58F1A0  xvsh\xpsBW_PC.psh        -> handle stored at <renderer> + 0x1016C
0x58F1A4  xvsh\xpsSetAlpha00.psh   -> handle stored at <renderer> + 0x10170
```

So the black-and-white pixel shader is compiled and held in a slot **on the PC**, every run, named for this
platform. Nothing in the executable mentions it by name anywhere else - it is reached only through that
handle - which fits a cheat switching it on and explains why no amount of searching for a colour feature found
one.

**Confirmed in play (0.6.14).** Writing 1 to `[0x6DD650] + 0x134` every frame draws the scene in black and
white, and the heads-up display keeps its colour - so the shader is on the **scene pass**, not on the finished
frame. That is better than a post-process for our purposes: an effect attached here reaches the world and
leaves the interface alone, with no mask and no second pass. The field is also what the game's own security
monitors use, which is why the shader survived into the PC build with a `_PC` name.

Two things follow, and they change the plan above for the better.

- **The engine already has a place to put a pixel shader over the scene.** Whatever draws with the handle at
  `+0x1016C` is the path a colour effect of ours should use, rather than a post-process pass bolted on from
  outside. Finding that draw is the next piece of work.
- **It is the shortest route to seeing our own shader on screen.** A handle in a known slot can be replaced
  with one of ours, which sidesteps finding D3DX's assembler entry for the time being.

Still unknown: which object the loader is working on (the handles are at a fixed offset inside it), what turns
the shader on, and whether the cheat that should do so reaches the PC at all - the sequence was tried in play
with RiotFix's pad and nothing happened.

## Shaders are text on disk, and that is the whole route in (measured)

The loop at `0x435650` builds a path and opens the file with `CreateFileA` - the arguments are visible at
`0x43566D`: read access, share 1, `OPEN_EXISTING`. So the engine reads its shaders as **plain text at run
time**, from `xvsh\`, and hands them to the D3DX it carries. `xvsh\xpsBW_PC.psh` is 324 bytes and reads in
full:

```
; XBOX PIXEL SHADER: Black and white, in one instruction!
ps.1.0

def c0, 0.3f, 0.55f, 0.15f, 1.0f  ; NTSC standard calls for unequal power distribution to R-G-B
tex t0

dp3 r0.rgb, c0, t0   ; multiply and add c0 with texture color to give us grayscale
mov r0.a, t0.a       ; retain texture alpha
```

There are two of them: `xpsBW.psh` declares `xps.1.1`, the Xbox dialect, and `xpsBW_PC.psh` declares `ps.1.0`.
Both shipped in the PC package. The weights are the NTSC luminance ones - the colour-space conversion this
project wants, already written by the authors.

**What this means.** A shader of ours does not need the D3DX entry point found, and does not need bytecode
written by hand. It needs text. And because RiotFix never changes a game file, the way to deliver it is to
intercept the read and hand back our own source, letting the engine's own compiler do the work exactly as it
does for its own shaders. The declared version can be raised in that text; the profile table proves the
linked D3DX knows `ps.2.0`.

**The cache does not touch this path, and the assembler is found.** The loop opens the file only to see that
it is there and closes it again at `0x43569C`; the work is done by `0x4EA48A`, called with the file's name -
`D3DXAssembleShaderFromFileA`. Inside it, `0x4F7476` reads the file into a blob and `0x4FC124` assembles.
The result goes straight to the device's `CreatePixelShader` at `0x4356D7`. So **pixel shaders are assembled
from text on every run, with no library or cache in the way**; `ShaderLibrary.bin` belongs to the stitched
vertex shaders, which is also why it is 1.2 MB and the two pixel shaders are a few hundred bytes of text.

### The route for a shader of our own (working, 0.6.18)

**Do not call the assembler.** Two builds did, using the six arguments its call site pushes, and both crashed
the game; `ret 0x18` confirms six, yet it returns success and leaves text from the file's own path where the
buffer should be, so the last two parameters are not what the call site suggests. The route below avoids it.

1. Write the source to a file in the temporary folder. The game's own folder stays untouched.
2. Point one name in the engine's shader table at it - `0x58F1A0`, the entry for `xpsBW_PC.psh`. The engine
   then opens, assembles and stores the handle with its own code, at its own time.
3. The name must be **relative**: the resolver at `0x4062C0` copies the game's base path and concatenates the
   name, with no case for a drive letter, so an absolute path becomes `C:\game\C:\Users\...`. RiotFix
   builds a climb out of the game's folder instead - `..\..\..\..\Users\...` - which needs both on one
   drive.
4. The field at `[0x6DD650] + 0x134` then turns our shader on, over the scene and under the interface.

Confirmed in play: `..\..\..\..\Users\Marcos\AppData\Local\Temp\RiotFixScene.psh` loaded, assembled by
the game, and drew with saturation and contrast applied.

### What the device accepts in ps.1.4 (measured in play, four captures of one cutscene frame)

Six shaders were written to differ in exactly one feature each, loaded one after another through the reload
key, and the resulting frames measured. Three drew, one did nothing - and a shader that fails to assemble or
to be created leaves the previous one standing, which is why "no change" reads as a failure.

| Feature under test | Shader | Measured result | Verdict |
|---|---|---|---|
| `sub` into a third register (`r2`) | `2 split` | saturation 0.000, pure grey | **works** |
| negated constant as a source modifier (`-c2`) | `3 no third register` | identical to the untouched game, saturation 0.560 | **refused** |
| `_x2` result modifier | `5 contrast by doubling` | mean 45 -> 25, median 42 -> 9, p99 137 -> 153 | **works** |
| the arithmetic alone, nothing exotic | `6 saturation only` | saturation 0.000, pure grey | **works** |

Two consequences:

- **`_x2` is the way past the constant range.** ps.1.4 takes no constant outside -1 to 1, which is what broke
  0.6.19 - it asked for saturation 1.1 and contrast 1.2 directly. Halving each number and letting `_x2`
  double the instruction's result reaches 0 to 2 with every constant legal.
- **0.6.19's structure was never the problem.** The `sub` into `r2` that it used draws correctly. Only its
  constants were out of range, and the failure was silent for the reason above.

### The old route, kept for the record

1. Write the source to a temporary file of RiotFix's own. No game file is touched, which is the rule.
2. Call `0x4EA48A(path, 0, 0, 0, &buffer, &errors)` - the engine's own compiler, so no `d3dx9_**.dll` is
   needed and the declared version can be anything the linked D3DX knows, `ps.2.0` included.
3. `CreatePixelShader` on the device at `[0x6DD650] + 0x268` with the assembled bytes.
4. Store the handle at `0x5CBF20 + 0x1016C`, which is index 0, the slot the black-and-white shader occupies.

From then on the field at `[0x6DD650] + 0x134` turns **our** shader on, over the scene and under the
interface, through the engine's own path. Errors are recoverable and visible: a bad source fails at step 2
with a message, and the slot can be left as it was.

## The shared groundwork

Most of what follows wants the same thing first: a **post-process pass**. Hook the device's `EndScene`, copy
the finished frame into a texture, draw one screen-filling rectangle with a shader of ours, and put the game's
render state back exactly as it was with a state block. It is written once and every effect after it is cheap
by comparison. Call it a day's work, and the risk is contained: it touches no game logic, so a mistake shows as
a wrong picture rather than a crash.

## Easy

- **Anisotropic filtering.** Sharper ground and walls seen at an angle. It is one sampler state, but the game
  sets its own every draw, so the call has to be intercepted and the value forced - the same kind of hook
  RiotFix already uses. Half a day. *Worth little on its own without the next one.*
- **Radial fog.** Fog that thins with true distance from the camera instead of with depth, so it stops
  thickening when you turn your head. The engine's own code for it is sitting there commented out, which means
  the shape of the data it wants already exists. The work is re-enabling it and rebuilding the shader caches.
  *Unknown: whether the constant it reads is still filled in by this build.*

## Medium

- **Colour grading in a proper colour space.** Convert to luminance and chrominance, work on them separately,
  convert back - so contrast and saturation stop dragging the shadows around with them. Needs the groundwork
  above plus one shader. *Two sources for the values rather than taste: the console build's own constants, and
  the fact that a console outputs limited range to a television, which may be most of the difference people see
  between the emulator and the PC.*
- **Bloom.** Bright things bleeding light. Same groundwork, a blur chain and a blend. Easy to overdo; the
  restraint is the hard part, not the code.
- **Mipmapping.** Removes the shimmer on distant texture detail, and is what makes anisotropic filtering worth
  having. *Unknown, and cheap to settle: whether the textures in the `.tdu` packages already carry mip levels.
  If they do, this is Easy. If they do not, they have to be generated as each texture is created, and the
  interface textures have to be kept out of it or the heads-up display goes soft.*

## Hard

- **Light shafts.** The sun or a lamp throwing visible beams. Needs the groundwork, an occlusion pass and a
  radial blur, and it only looks right where the level was built with a light worth shafting.
- **Depth of field.** Needs scene depth, which means the same work as the next entry.
- **Ambient occlusion.** Contact shadows in corners and crevices. Needs the scene's depth buffer as a texture,
  which Direct3D 9 does not give away: it wants an `INTZ` depth surface, which means changing how the game
  creates its depth buffer and hoping nothing else depends on the old one. *Unknown: whether the card and the
  engine tolerate that swap. This is the one most likely to be abandoned after the groundwork is done.*
- **Water reflections.** Render the scene again mirrored into a texture and feed it to the water material, with
  distortion and a Fresnel blend. Shader model 2 makes the shading possible where 1.4 would not have. The cost
  is elsewhere: finding the water surfaces and their plane in the level format, and a second scene pass is the
  most expensive thing on this page. *Unknown: whether the water material can be reached and replaced at all.*

## Not worth it until something changes

- **Shadows for everything.** Already a parked item; the engine decides per object, and forcing it has looked
  wrong in every attempt so far.
