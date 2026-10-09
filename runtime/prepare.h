// Preparing the game on the player's machine: from their disc to the module game_module.h
// loads, once.
//
//   1. check the disc: its ID and the SHA-1 of its main.dol, which must be the build the
//      game project's tables were written for;
//   2. extract main.dol;
//   3. run the recompiler (recomp/recomp.py) with the project's tables and symbols;
//   4. compile the C it writes, in parallel, and link it as the module.
//
// What it needs ships with the app (prepare_sources): the recompiler, the tables, the symbol
// table, the runtime headers the generated code includes. What it needs from the machine is
// a C compiler and python3 -- on macOS, Apple's Command Line Tools.
//
// The result lives in game/<dol sha1>-<inputs>/ under save_path, the second half a hash of everything
// that went into it, so that an app update that changes the tables prepares again.
#pragma once
#include <functional>
#include <string>

struct PrepareSources {
    std::string recompiler;   // the directory holding recomp.py
    std::string tables;       // the game's recomp/ tables
    std::string symbols;      // its symbols.txt
    std::string include;      // the runtime headers: recomp.h, compat.h
};
// A development build's: the source tree it was configured from. GCN_RESOURCES=<dir>, laid
// out as an app's Resources (recomp/, tables/, symbols.txt, include/), overrides it.
PrepareSources prepare_sources();

// Reads the disc's ID and main.dol. False, with the reason, if the image cannot be read or is
// not the game and build this was made for.
bool prepare_check_disc(const char* iso, std::string* dol_sha1, std::string* err);

// Where the module prepared from a main.dol of this SHA-1 is, ready or not.
std::string prepare_module_path(const std::string& dol_sha1);

// Whether the tools are there: a C compiler and python3. `missing` says which are not.
bool prepare_tools_present(std::string* missing);

// Prepares the game from `iso`, reporting progress (0..1) and what it is doing. On success
// `module` is the path to load. A log of the tools' output is kept beside the result.
bool prepare_game(const char* iso, std::string* module, std::string* err,
                  const std::function<void(float, const std::string&)>& progress);

// What a frontend without a launcher does at startup: load the game module -- a development
// build's, else the one prepared for this disc, preparing it first if there is none, or
// always with `force` -- reporting on stderr. False, with the reason printed, if it cannot.
bool game_module_ensure(const char* iso, bool force);
