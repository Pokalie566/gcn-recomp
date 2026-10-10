# gcn-recomp

A toolkit for static recompilation of Nintendo GameCube games to native code: a Gekko
PowerPC → C recompiler, and a runtime that stands in for the GameCube hardware (CPU helpers,
OS context switching, graphics through an OpenGL GX pipeline, audio, DVD, controllers,
memory card) with an SDL desktop frontend.

It is the common layer under per-game projects, each of which supplies its own game's
layout and steering files and nothing else:

- [prime-recomp](https://github.com/ethannicholas/prime-recomp) — *Metroid Prime*
- [bluestorm-recomp](https://github.com/ethannicholas/bluestorm-recomp) — *Wave Race: Blue
  Storm*, where this code was first written

**This repository contains no game code or data.** A game project builds from the user's
own disc image; the recompiled C lands in that project's build directory and nowhere else.

## Using it from a game project

Add this repository as a submodule and call `gcn_add_game()`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(prime C CXX)
add_subdirectory(gcn-recomp)
gcn_add_game(
  NAME prime
  TITLE "Metroid Prime"
  DISC_ID GM8E01
  DOL_SHA1 39a2f928159ad46491e9b1ef0a72613d91e0cc40
  SYMBOLS ${CMAKE_SOURCE_DIR}/analysis/symbols.txt
  TABLES ${CMAKE_SOURCE_DIR}/recomp
  FPS 60)
```

That defines the game executable and a headless benchmark (`<name>_bench`). Neither executable holds any game code, and both build without a
disc: they load the recompiled game as a module, `<name>_game` (`runtime/game_module.h`),
which the build makes beside them when it has a disc image, and which they otherwise
prepare from the player's disc on first run (`runtime/prepare.h`). With `ANDROID_PACKAGE <id>`, an Android build (the NDK's toolchain
file, `-DGCN_BENCH_ONLY=ON`) also builds the headset app, `lib<name>.so`, which
`tools/package-apk.ps1` turns into an APK, and `<name>_egl`, the renderer headless on a
device. A game tells those what it knows -- when to show stereo, how big its world is --
through `vr::GameHooks` (`runtime/vr_game.h`) in its `SOURCES`.
The game project provides:

- `analysis/symbols.txt` — the DOL's function layout in decomp-toolkit's format, either
  from `dtk dol split` (`tools/fetch_dtk.sh` downloads it) or from a decompilation project.
- `recomp/hle.txt` — functions whose body the runtime replaces (`OSSaveContext`,
  `OSLoadContext`, `OSReport`, `OSPanic` at minimum).
- `recomp/special_calls.txt` — the address of `OSSaveContext`, whose call sites become
  resume points.
- `recomp/idle.txt` — the backward branch of the SDK scheduler's idle spin (`SelectThread`
  waiting for a runnable thread), where the runtime skips guest time to the next event.
- `recomp/names.txt`, `recomp/patches.txt` — hand-named functions and instruction patches,
  usually empty.

## Playing

`<name> [image.iso]` runs the game; with no path it finds the image the build was
configured with. Options for the player:

| Option | |
|---|---|
| `--widescreen` | a 16:9 frame; the game project widens its camera (docs/graphics.md, *Widescreen*) |
| `--interpolate` | a 30 fps game shown at 60, an in-between frame drawn between two of its own (*Frame interpolation*) |
| `--mouse-aim` | the mouse drives the C stick, its buttons the triggers (`runtime/mouse_aim.h`) |
| `--scale=N` | internal resolution, N times the console's (default 2) |
| `--fullscreen` | fullscreen; F11 switches |
| `--dump-textures` | write every texture the game uses to `textures_dump/<game id>/`, named for a pack |

These, the disc image, the volume, whether a texture pack is used and the pad bindings are
**settings**, kept in `saves/settings.ini` (`runtime/settings.h`), which the first run
writes with the defaults. An option on the command line overrides its setting for that run
only. **Esc** (or a controller's Back) pauses, with a menu over the frozen frame
(`runtime/ui`, drawn with [RmlUi](https://github.com/mikke89/RmlUi)): resume, settings --
graphics, controls with every pad button rebindable, audio -- and quit. A setting changed
there applies at once and is saved. The menus' documents are `runtime/ui/assets`; a game
project gives them its look with its own `theme.rcss` (gcn_add_game's `UI`).

`-DGCN_APP=ON` builds the game as an app to hand out: no disc and no game code in it, SDL2
built in, and on macOS a `<APP_NAME>.app` whose Resources hold what preparing the game on
the player's machine needs. It opens on a launcher (`runtime/ui/launcher.h`) that checks
everything, prepares the game from the disc the player drops on it, and plays; the player's
state goes to `~/Library/Application Support/<APP_NAME>`. Built from the same build of the
game, without `GCN_APP`, the launcher is there too (`--no-launcher` skips it).

The menus use RmlUi (MIT) and FreeType (the FreeType License: portions of this software are
copyright © The FreeType Project, www.freetype.org), both fetched and built by CMake, and
the Fira Sans font (SIL Open Font License, `runtime/ui/assets/fonts/OFL.txt`).

**Texture packs** are Dolphin's: copy a pack's `<game id>` folder (Dolphin's
`Load/Textures/<game id>`) to `textures/` in the working directory and it is used as it is
-- same names, same `$` wildcards, any resolution. PNG or DDS (uncompressed, DXT1/3/5; not BC7). `GCN_TEXTURES=<dir>`
looks elsewhere. See `runtime/gx/texture_pack.h`.

Keyboard, by default: arrows the control stick, X A, Z B, C X, S Y, D Z, Q L, W R, Enter
Start, I J K L the d-pad. A controller works as it is plugged in. Both are bound in
settings.ini, `bind.<action> = key:<SDL key name>, pad:<SDL button>, axis:+<axis>`. With
`--mouse-aim` the cursor is captured; F1 releases it and a click takes it back. `mouse_sens`
scales the mouse (1 by default; `GCN_MOUSE_SENS` for a run). A game whose C stick needs other settings gives its own defaults at
compile time on its target -- `GCN_MOUSE_DEADZONE` (the deflection it starts to react at,
0.15), `GCN_MOUSE_INVERT_Y`, `GCN_MOUSE_Y=0` (vertical motion ignored, the wheel pushes
the stick's y instead) -- which the environment variables of the same names override.

## Layout

- `recomp/`: `recomp.py` drives the translation; `ppc.py` turns each instruction into C;
  `dol.py` reads DOLs, `.iso` and `.ciso` images and symbol files; `extract_dol.py` pulls
  and verifies the DOL.
- `runtime/`: the GameCube in software. `recomp.h` is the contract with generated code.
  `cpu.cpp`, `threads.cpp` and `hle_os.cpp` are the CPU and OS; `hw/` the hardware
  registers; `gx/` the graphics pipeline; `main.cpp` the SDL frontend and `bench_main.cpp`
  the benchmark.
- `android/`: the headset frontends. `openxr_main.cpp` is the OpenXR app (the game on a
  theater panel or in stereo, Touch controllers as the pad), `egl_main.cpp` the headless
  harness and `audio_aaudio.cpp` the audio device. The `vr.txt` settings
  (`runtime/vr_config.*`) and what a game tells the headset (`runtime/vr_game.h`) are in the
  runtime, so the desktop can check a game's hooks too.
- `tools/package-apk.ps1`: packages the app into an APK without Gradle, and installs it.
- `docs/`: working notes on diagnostics and the renderer.

## Requirements

CMake 3.20+, Ninja, Python 3, a C++20 compiler, SDL2 (via pkg-config; built from source on
Windows). On macOS: `brew install cmake ninja sdl2 python`.
