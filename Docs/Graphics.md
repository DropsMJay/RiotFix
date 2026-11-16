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

Two things follow, and they change the plan above for the better.

- **The engine already has a place to put a pixel shader over the scene.** Whatever draws with the handle at
  `+0x1016C` is the path a colour effect of ours should use, rather than a post-process pass bolted on from
  outside. Finding that draw is the next piece of work.
- **It is the shortest route to seeing our own shader on screen.** A handle in a known slot can be replaced
  with one of ours, which sidesteps finding D3DX's assembler entry for the time being.

Still unknown: which object the loader is working on (the handles are at a fixed offset inside it), what turns
the shader on, and whether the cheat that should do so reaches the PC at all - the sequence was tried in play
with RiotFix's pad and nothing happened.

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
