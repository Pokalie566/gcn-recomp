// The recompiled game as a module. See game_module.h.
#include "game_module.h"
#include "platform.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

GameModule g_game;

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
    return plat_readable(prepared.c_str()) ? prepared : std::string();
}

// ---- Building one ----

#ifndef GCN_RES_TOOLCHAIN
#define GCN_RES_TOOLCHAIN ""   // a development build's: the one CMake fetched into its build directory
#endif

std::string toolchain_dir() {
    if (const char* t = getenv("GCN_TOOLCHAIN")) return t;
    if (const char* r = getenv("GCN_RESOURCES")) return std::string(r) + "/toolchain";
    return GCN_RES_TOOLCHAIN;
}

std::vector<std::string> game_module_cflags() {
#ifdef _WIN32
    std::vector<std::string> f = {"-std=gnu11", "-O2", "-fno-strict-aliasing", "-ffp-contract=off", "-w", "-DGCN_MODULE_DLL"};
#else
    std::vector<std::string> f = {"-std=gnu11", "-O2", "-fPIC", "-fno-strict-aliasing", "-ffp-contract=off", "-w"};
#endif
#ifdef GCN_TRACE_CALLS
    f.push_back("-DGCN_TRACE_CALLS");
#endif
    return f;
}

bool game_module_build(const std::vector<std::string>& sources, const std::vector<std::string>& includes,
                       const std::string& out, const std::string& import_lib, const std::string& log,
                       std::string* err, const std::function<void(size_t, size_t)>& progress) {
#ifdef _WIN32
    const std::string zig = toolchain_dir() + "/zig/zig.exe";
#else
    const std::string zig = toolchain_dir() + "/zig/zig";
#endif
    if (!plat_readable(zig.c_str())) { *err = "no zig in the toolchain (" + toolchain_dir() + ")"; return false; }
    const std::vector<std::string> cc = {zig, "cc"};
    if (!getenv("ZIG_GLOBAL_CACHE_DIR")) {
        // zig's cache (the libc pieces it builds once) beside the prepared games, in the
        // player's data folder: <data>/game/zig-cache for <data>/game/<hash>.work/<module>.
        const std::string cache = (std::filesystem::path(out).parent_path().parent_path() / "zig-cache").string();
#ifdef _WIN32
        _putenv_s("ZIG_GLOBAL_CACHE_DIR", cache.c_str());
        _putenv_s("ZIG_LOCAL_CACHE_DIR", cache.c_str());
#else
        setenv("ZIG_GLOBAL_CACHE_DIR", cache.c_str(), 1);
        setenv("ZIG_LOCAL_CACHE_DIR", cache.c_str(), 1);
#endif
    }
    // Compiled in parallel, one compiler per core.
    std::atomic<size_t> next{0}, done{0};
    std::atomic<bool> failed{false};
    std::mutex progress_mutex;
    const std::vector<std::string> flags = game_module_cflags();
    auto worker = [&] {
        for (size_t i; !failed && (i = next++) < sources.size();) {
            std::vector<std::string> argv = cc;
            argv.insert(argv.end(), flags.begin(), flags.end());
            for (const auto& inc : includes) argv.push_back("-I" + inc);
            argv.insert(argv.end(), {"-c", sources[i], "-o", sources[i] + ".o"});
            if (plat_run(argv, log.c_str()) != 0) { failed = true; return; }
            const size_t n = ++done;
            std::lock_guard<std::mutex> lk(progress_mutex);
            if (progress) progress(n, sources.size());
        }
    };
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < std::max(1u, std::thread::hardware_concurrency()); i++) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    if (failed) { *err = "compiling the game failed; see " + log; return false; }
    std::vector<std::string> link = cc;
#if defined(__APPLE__)
    // The runtime's symbols are looked up in the executable as the module loads.
    link.insert(link.end(), {"-dynamiclib", "-undefined", "dynamic_lookup"});
#else
    link.push_back("-shared");
#endif
    link.insert(link.end(), {"-o", out});
    for (const auto& s : sources) link.push_back(s + ".o");
#ifdef _WIN32
    link.push_back(import_lib);   // the runtime's symbols, from the executable
#else
    (void)import_lib;
#endif
    if (plat_run(link, log.c_str()) != 0) { *err = "linking the game failed; see " + log; return false; }
    return true;
}
