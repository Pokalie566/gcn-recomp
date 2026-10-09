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

## Frame interpolation (30 fps games at 60)

`--interpolate` (or `GCN_INTERPOLATE=1`) shows a game that runs at 30 fps at 60: between
two of its frames the frontend draws one more, halfway between them
(`render_execute_interpolated`), then the real one half a frame later. The game itself is
untouched -- its logic, physics and timing still run at 30 -- so nothing about it can
break; the cost is half a frame of added latency.

The in-between frame is built from the two batches (`gx/interp.cpp`, free of GL and tested
without a GPU). Vertices arrive transformed and lit, so a vertex's position, colours and
texture coordinates are each set to the midpoint of their values in the two frames, which
follows the camera, the objects and whatever is projected onto them alike. Each draw is
matched with the same draw of the previous frame by its shape (primitive, vertex count,
first texture, first and last position index), the nearest one when several match.
Unmatched draws are drawn as they are now; a draw that jumped further than its distance
from the viewer is not moved (a teleport); and a frame where under half of the draws
match, or over half moved more than a quarter of their distance, is a cut: no in-between
frame is drawn for it. `interp.h` has the reasoning.

Only a frame that arrives about two refreshes after the previous one (25 to 50 ms) gets an
in-between: a backlog or a scene the game already runs at 60 goes straight through. An
in-between frame does not count as a frame for texture ageing, frame dumps or
`present_count()`; dumped, it is `frame_NNNNN_i.png`, before frame NNNNN.
`GCN_INTERPOLATE=force` draws one before every frame and never waits, for dumping them
from an unpaced run; `GCN_INTERPLOG=1` prints, per frame, the draws matched and moved far
and the verdict.

## Custom textures

`gx/texture_pack.cpp` replaces textures from a pack in Dolphin's format, so the packs made
for Dolphin work unchanged. When a texture is decoded for the first time (a cache miss in
`texture_lookup`), and only if a pack was found or `--dump-textures` is on, its Dolphin name
is computed -- XXH64 of the top level as stored, and for an indexed texture of the palette
entries between the lowest and highest its texels use -- and looked up: the exact name,
then with `$` for the palette hash, then for the texel hash, as Dolphin does.

A replacement is stored at its own size (`TexData::data_w/data_h`) but keeps the game's size
as the texture's size: `u_texsize`, which turns coordinates into texels for indirect
offsets, must not change with the pack's resolution. If the game samples the texture with
mipmaps, the replacement's are generated (2x2 box down to 1x1); a pack's own `_mip` files
and the arbitrary mipmaps of `_arb` textures are not used, and DDS files are skipped. A
replacement is read from disk when its texture is decoded, which is once until the texture
goes unused long enough to be evicted.

`--dump-textures` marks each newly decoded texture with its name; the renderer writes it
to `textures_dump/<game id>/<name>.png` on upload, alpha kept, once per name. EFB copies are
neither replaced nor dumped.

## Stereo scaffolding

`gx/render_gl.cpp` and `gx/shadergen.cpp` carry the stereo machinery written for Wave Race:
`render_execute_eye`, the theater/stereo morph, the world-pitch rotation, and the eye
grab/refraction handling. The desktop frontend runs it through `--eye`. Where the eye stands
beyond the chase camera is the game's business: `render_set_eye_hook` takes a per-frame
callback that returns a view transform, a HUD transform and the draws to leave out, and
bluestorm-recomp's `runtime/first_person.cpp` is the one that finds a jet ski and puts the eye
on it. Nothing here knows what a hull is.
