// The recompiled game, as a module the runtime loads at startup.
//
// The executable holds the runtime and the game project's own code, and nothing translated
// from the game: that is what lets it be handed to anyone. The game's code is a shared
// library built from the player's own disc -- by prepare.h on their machine, or by CMake
// beside the executable in a development build -- and loaded here.
//
// The runtime reads five things from it: the four recomp.py writes to recomp_table.c (the
// function table, its length and the bounds of the code it covers) and the original body of
// OSSaveContext, which its replacement calls (threads.cpp). Everything else goes the
// other way: the module calls the runtime (and the game project's hle_* functions) by name,
// resolved against the executable when it is loaded, which is why the executable exports
// its symbols.
//
// A build that links the generated code statically (the Android app) defines
// GCN_STATIC_GAME and never loads anything.
#pragma once
#include "recomp.h"
#include <functional>
#include <string>
#include <vector>

struct GameModule {
    const RecompFunc* funcs = nullptr;
    uint32_t count = 0, code_base = 0, code_end = 0;
    RecompFn orig_OSSaveContext = nullptr;
};
extern GameModule g_game;

// The module's file name: <game>_game.dylib, .so or .dll.
std::string game_module_filename();

// Loads `path` as the game. False, with the reason, if it is missing or not a game module.
bool game_module_load(const std::string& path, std::string* err);

// Which module to load when nothing else says: GCN_GAME_MODULE, else a development build's
// beside the executable, else `prepared` if it exists. Empty when there is none.
std::string game_module_find(const std::string& prepared);

// ---- Building one ----
// With the machine's own compiler: cc on macOS and Linux, Visual Studio's C++ tools on
// Windows (found with vswhere). False, with what is missing, if they are not there.
bool game_module_compiler_present(std::string* missing);

// Compiles `sources` and links them as the module `out`, the way a game module must be
// built: position-independent, the runtime's symbols left for the executable to provide --
// on Windows through `import_lib`, the executable's import library, which also means the
// module takes the runtime's data as imported (GCN_MODULE_DLL, recomp.h). In parallel; the
// tools' output goes to `log`, and `progress` hears how many files are compiled.
bool game_module_build(const std::vector<std::string>& sources, const std::vector<std::string>& includes,
                       const std::string& out, const std::string& import_lib, const std::string& log,
                       std::string* err, const std::function<void(size_t done, size_t total)>& progress);

// The flags it compiles with, which a prepared module's hash takes in.
std::vector<std::string> game_module_cflags();
