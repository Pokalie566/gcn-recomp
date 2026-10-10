# Graphics back end notes

The game draws through **Aurora** (github.com/encounter/aurora): its GX command processor
on WebGPU (Dawn: Metal, Direct3D 12, Vulkan), with SDL3 for the window and RmlUi for the
menus. It replaced an OpenGL renderer of our own (CPU transform and lighting, one shader
per TEV configuration, stereo for VR), deleted on 2026-10-10; it is in the history before
that date.

## Aurora

- **Bridge** (`gx/aurora_bridge.cpp`, guest thread): the game's FIFO, parsed as the hardware
  would, re-emitted as Aurora's big-endian stream, with Aurora's own commands (`GX_AURORA`,
  0x50) for what Aurora cannot read from guest memory itself: vertex arrays, textures and
  TLUTs (copied, keyed by address and frame so they are re-uploaded once per frame), EFB
  copies. A few things the hardware does that Aurora does not take: cull mode 3 (no
  triangles: dropped here), strips and fans of fewer than 3 vertices (dropped), the copy
  trigger's format field (bits 4-6 plus bit 3 as the high bit; the intensity bit 15 only for
  R4/R8/RA4/RA8; a depth copy as RG8 is Z16).
- **GPU side** (`gx/aurora_gpu.cpp`, the window's thread): takes the bridge's chunks, feeds
  them to `aurora::gx::fifo`, presents each frame. The guest waits at draw-done and token
  syncs until its chunk is processed, and stays at most one frame ahead. The last frame is
  kept, to be shown again under the pause menu and the launcher (`gpu_present_again`).
- **Patches** (`patches/aurora-*.patch`, applied to the pinned commit at configure time):
  `srtg-lit-color` (GX_TG_SRTG takes the lit colour channel, not the raw vertex colour: the
  flashlight cone), `blocking-pipelines` (a draw waits for its pipeline rather than being
  skipped the first time a shader is needed, as on the console), `rmlui-hooks` (a translator
  for every text, absolute document paths, and the interface's render target for captures).
- **16:9**: the rule of § Widescreen below, in the bridge. Aurora's frame is 854 logical
  pixels wide (`VIConfigure` with `fbWidth` 854, viewport policy FIT so it stays 16:9 in any
  window); viewports, scissors and EFB copies are mapped from the game's 640 into it
  (stretched if full-width, moved into the middle 4:3 if not), and an orthographic draw that
  does not reach both edges gets its projection divided by 4/3. The bridge keeps a copy of
  XF (direct and indexed loads) to find a draw's view-space x.
- **60 fps** (`gx/aurora_interp.h`): Aurora transforms on the GPU, so the in-between frame
  is the new frame's stream replayed with its XF transforms (position, normal and texture
  matrices, projection, light positions) halfway to the previous frame's. Loads are matched
  through the draws they lead to (signature: primitive, size, first and last indices), so
  objects drawn in another order keep their own matrices; jumps and cuts are left alone as
  before. For that the bridge makes its chunks self-contained: indexed XF loads are read
  and sent as direct ones (always), and while interpolating the vertex arrays are copied
  into the chunk (`Chunk::held`), so the GPU side can hold a frame back without the game
  rewriting what it points at. Interval between frames measured from arrival, not display.
- **Settings**: the internal resolution is `VISetFrameBufferScale`; MSAA and the graphics API
  (`backend`) take effect at the next start; vsync and borderless at once.
- **Builds**: SDL3 is always built here, static. An app (`GCN_APP`) also builds every other
  dependency of Aurora (no Homebrew, no pkg-config). Dawn is prebuilt for one architecture
  at a time, so a universal macOS app is two builds joined with `lipo`; Dawn needs macOS 12.

## Widescreen

`--widescreen` (or `GCN_WIDESCREEN=1`) shows a 16:9 frame. The game still draws into its
640-wide EFB; Aurora's frame is 854 wide and the bridge maps the game's x into it (see
Aurora above), letterboxed to 16:9 in the window. On its own that stretches a 4:3
picture: the game project widens its camera to fill the frame -- `g_widescreen` in
`runtime.h` tells it to, typically by scaling the aspect the game passes to
`MTXPerspective` for its main camera.

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
  same mapping (`wide_x` in `aurora_bridge.cpp`), so a render-to-texture drawn into a
  corner and copied out still lines up.

At 4:3 every one of these paths is the identity: `g_wide` is 1 and the stream is the
same as without the option.

## Custom textures

`gx/packs.cpp` hands the packs in the textures folder to Aurora's replacement API
(`aurora::texture`): packs in Dolphin's format work unchanged, a folder per pack, loose
files beside them as one more. Each can be turned off in the settings (`textures_off`).
`--dump-textures` turns on Aurora's texture dumps (`allowTextureDumps`), into its user
folder.
