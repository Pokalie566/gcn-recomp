// Preparing the game on the player's machine: from their disc to the module game_module.h
// loads, once.
//
//   1. check the disc: its ID and the SHA-1 of its main.dol, which must be the build the
//      game project's tables were written for;
//   2. extract main.dol;
//   3. run the recompiler (recomp/recomp.py) with the project's tables and symbols;
//   4. compile the C it writes, in parallel, and link it as the module;
//   5. keep a copy of the disc (disc.h), so that the player's image is no longer needed.
//
// What it needs ships with the app (prepare_sources): the recompiler, the tables, the symbol
// table, the runtime headers the generated code includes -- and the toolchain that runs them,
// zig and Python (game_module.h, toolchain_dir), so that the machine needs nothing. A
// development build takes them from the source tree and its build directory.
//
// The result lives in game/<dol sha1>-<inputs>/ under save_path, the second half a hash of everything
// that went into it, so that an app update that changes the tables prepares again.
#pragma once
#include <functional>
#include <string>
#include <vector>

struct PrepareSources {
    std::string recompiler;   // the directory holding recomp.py
    std::string tables;       // the game's recomp/ tables
    std::string symbols;      // its symbols.txt
    std::string include;      // the runtime headers: recomp.h, compat.h
    std::string import_lib;   // on Windows, the executable's import library, for the module to link with
};
// A development build's: the source tree it was configured from. GCN_RESOURCES=<dir>, laid
// out as an app's Resources (recomp/, tables/, symbols.txt, include/), overrides it.
PrepareSources prepare_sources();

// Reads the disc's ID and main.dol and looks them up in the versions the game knows
// (versions.h). False, with the reason, if the image cannot be read or is not a version this
// was made for; `verdict`, if given, has the reason or the version, ready for ui_trf.
struct DiscVerdict;
struct DiscVersion;
std::vector<DiscVersion> prepare_versions();   // the game's versions.txt (versions.h)
bool prepare_check_disc(const char* iso, std::string* dol_sha1, std::string* err, DiscVerdict* verdict = nullptr);

// Where the module prepared from a main.dol of this SHA-1 is, ready or not.
std::string prepare_module_path(const std::string& dol_sha1);

// Where preparing keeps its copy of the disc (disc_copy_ciso): the image to play from then on.
std::string prepare_disc_copy_path(const std::string& dol_sha1);

// Prepares the game from `iso`, reporting progress (0..1) and what it is doing. On success
// `module` is the path to load and `disc_copy` (if given) the copy of the disc. Beside the
// module, a log of the tools' output and disc.txt: the disc's ID and the whole image's SHA-1.
bool prepare_game(const char* iso, std::string* module, std::string* disc_copy, std::string* err,
                  const std::function<void(float, const std::string&)>& progress);

// What a frontend without a launcher does at startup: load the game module prepared for this
// disc, preparing it first if there is none, or always with `force` -- reporting on stderr.
// False, with the reason printed, if it cannot.
bool game_module_ensure(const char* iso, bool force);
