# Graphics back end notes

The renderer is an **OpenGL 3.3 core profile** back end behind a GX front end that runs
vertex decoding, transform, lighting and texgen on the CPU, and generates one fragment
shader per TEV configuration. On macOS the system framework is linked directly; elsewhere a
vendored glad loader resolves the entry points.

## Shader cache

Each TEV configuration is compiled the first time a draw uses it, mid-frame. Every key
compiled is appended to `saves/shaders.bin`, and the next run builds all of them before the
game boots. Deleting the file is always safe. The startup line reports what happened:

```
[shaders] 29 programs from cache (0 from binaries, 29 compiled) in 44 ms
```

## Widescreen

`--widescreen` (or `GCN_WIDESCREEN=1`) shows a 16:9 frame. The game still draws into its
640-wide EFB; the renderer stores the EFB 4/3 wider (`g_wide` in `render_gl.cpp`) and
letterboxes the window to 16:9. On its own that stretches a 4:3 picture: the game
project widens its camera to fill the frame -- `g_widescreen` in `runtime.h` tells it to,
typically by scaling the aspect the game passes to `MTXPerspective` for its main camera.

Not everything should widen. The rule is one: **whatever spans the whole width is
stretched edge to edge, anything narrower is a 4:3 element and keeps its 4:3 shape and
place in the middle 4:3 of the frame.**

- An orthographic draw through a full-width viewport whose vertices do not reach both
  edges (a HUD element, text, a logo) has its projection squeezed back to 4:3. One that
  does reach them (a background, a fade, a letterbox bar) is stretched.
- A viewport narrower than the frame (a picture-in-picture: a 3D portrait drawn in a
  corner) is moved into the middle 4:3 as a whole, and its projection is left alone -- so
  the game project must not widen that camera.
- A scissor rect, an EFB copy and a clear of a region narrower than the frame follow the
  same mapping (`px_x43`), so a render-to-texture drawn into a corner and copied out still
  lines up.

At 4:3 every one of these paths is the identity: `g_wide` is 1 and the frame is the
same as without the option.

## Stereo scaffolding

`gx/render_gl.cpp` and `gx/shadergen.cpp` carry the stereo machinery written for Wave Race:
`render_execute_eye`, the theater/stereo morph, the world-pitch rotation, and the eye
grab/refraction handling. The desktop frontend runs it through `--eye`. Where the eye stands
beyond the chase camera is the game's business: `render_set_eye_hook` takes a per-frame
callback that returns a view transform, a HUD transform and the draws to leave out, and
bluestorm-recomp's `runtime/first_person.cpp` is the one that finds a jet ski and puts the eye
on it. Nothing here knows what a hull is.
