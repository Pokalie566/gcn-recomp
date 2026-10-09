// The recompiled game as a module. See game_module.h.
#include "game_module.h"
#include "platform.h"
#include <cstdlib>

GameModule g_game;

#ifdef GCN_STATIC_GAME
// Linked in: the table is there before main runs.
extern "C" void orig_OSSaveContext(CPU* c);
static const bool g_static_game = [] {
    g_game = {g_recomp_funcs, g_recomp_func_count, g_recomp_code_base, g_recomp_code_end, orig_OSSaveContext};
    return true;
}();
#endif

std::string game_module_filename() {
#if defined(_WIN32)
    return GCN_GAME_NAME "_game.dll";
#elif defined(__APPLE__)
    return GCN_GAME_NAME "_game.dylib";
#else
    return GCN_GAME_NAME "_game.so";
#endif
}

bool game_module_load(const std::string& path, std::string* err) {
    void* m = plat_module_open(path.c_str(), err);
    if (!m) return false;
    auto funcs = (const RecompFunc*)plat_module_symbol(m, "g_recomp_funcs");
    auto count = (const uint32_t*)plat_module_symbol(m, "g_recomp_func_count");
    auto base = (const uint32_t*)plat_module_symbol(m, "g_recomp_code_base");
    auto end = (const uint32_t*)plat_module_symbol(m, "g_recomp_code_end");
    auto save_ctx = (RecompFn)plat_module_symbol(m, "orig_OSSaveContext");
    if (!funcs || !count || !base || !end || !save_ctx) {
        if (err) *err = path + " is not a game module (no function table or OSSaveContext)";
        return false;
    }
    g_game = {funcs, *count, *base, *end, save_ctx};
    return true;
}

std::string game_module_find(const std::string& prepared) {
    if (const char* e = getenv("GCN_GAME_MODULE")) return e;
    const std::string dev = plat_executable_dir() + "/" + game_module_filename();
    if (plat_readable(dev.c_str())) return dev;
    if (!prepared.empty() && plat_readable(prepared.c_str())) return prepared;
    return {};
}
