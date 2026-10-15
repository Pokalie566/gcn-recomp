# Working notes for this repository

This is the shared GameCube recompilation layer, split out of bluestorm-recomp on 2026-10-07;
upstream (ethannicholas/gcn-recomp) serves prime-recomp and bluestorm-recomp. This fork's
`lm-recomp` branch serves lm-recomp only: since 2026-10-10 it draws through Aurora (WebGPU) --
the OpenGL renderer, the VR code and `android/` were deleted then -- and on 2026-10-15 the
diagnostics lm-recomp did not use (benchmark, heap tracing, guest checks, `GCN_WATCH`, the host
clock) went too. All of it is in the history.

**Nothing game-specific belongs here.** No disc IDs, guest addresses, game names or
per-game heuristics in code; those arrive through `gcn_add_game()` and the game project's
tables. If a change needs to know something about one game, it needs a parameter or a hook,
not a constant. Environment variables, CMake options and macros use the `GCN_` prefix.

**No game code or data**, as in the game projects: never commit or quote the generated C,
disassembly or anything from a disc.

Record what you learn in `docs/`: `diagnostics.md` for the `GCN_*` variables, input logging
and replay and the crash dumps; `graphics.md` for the renderer. Game-specific findings go in
the game project's `docs/`, not here.

lm-recomp pins a commit of this repository as a submodule. After changing shared code: commit
here on `lm-recomp`, push to the remote `fork` (Pokalie566/gcn-recomp; `origin` is upstream),
then update the submodule pointer in lm-recomp and commit that too.
