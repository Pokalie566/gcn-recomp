// The recompiled game as a module. See game_module.h.
#include "game_module.h"
#include "platform.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

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

// ---- Building one ----

std::vector<std::string> game_module_cflags() {
#ifdef _WIN32
    // As game_recomp is built with MSVC (gcn-recomp CMakeLists), and the C runtime the
    // executable uses, which setjmp/longjmp across the two must share.
    std::vector<std::string> f = {"/O2", "/fp:precise", "/fp:except-", "/bigobj", "/MD", "/w", "/DGCN_MODULE_DLL"};
#else
    std::vector<std::string> f = {"-std=gnu11", "-O2", "-fPIC", "-fno-strict-aliasing", "-ffp-contract=off", "-w"};
#endif
#ifdef GCN_TRACE_CALLS
    f.push_back(f[0][0] == '/' ? "/DGCN_TRACE_CALLS" : "-DGCN_TRACE_CALLS");
#endif
    return f;
}

#ifdef _WIN32
// Visual Studio's environment script for 64-bit tools, if a Visual Studio with the C++ tools
// is installed: vswhere is where every one since 2017 says where it is.
static std::string vcvars() {
    const char* pf = getenv("ProgramFiles(x86)");
    const std::string vswhere = std::string(pf ? pf : "C:\\Program Files (x86)") + "\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    if (!plat_readable(vswhere.c_str())) return {};
    const std::string out = (std::filesystem::temp_directory_path() / "gcn_vswhere.txt").string();
    std::filesystem::remove(out);
    if (plat_run({vswhere, "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                  "-property", "installationPath"}, out.c_str()) != 0) return {};
    std::ifstream in(out);
    std::string dir;
    std::getline(in, dir);
    while (!dir.empty() && (dir.back() == '\r' || dir.back() == ' ')) dir.pop_back();
    const std::string bat = dir + "\\VC\\Auxiliary\\Build\\vcvars64.bat";
    return dir.empty() || !plat_readable(bat.c_str()) ? std::string() : bat;
}

bool game_module_compiler_present(std::string* missing) {
    if (!vcvars().empty()) return true;
    *missing = "Visual Studio's C++ build tools";
    return false;
}

// One script that sets up Visual Studio's tools, compiles every file (cl /MP, a compiler per
// core) and links: the environment script is slow, so it runs once.
bool game_module_build(const std::vector<std::string>& sources, const std::vector<std::string>& includes,
                       const std::string& out, const std::string& import_lib, const std::string& log,
                       std::string* err, const std::function<void(size_t, size_t)>& progress) {
    const std::string bat_env = vcvars();
    if (bat_env.empty()) { *err = "Visual Studio's C++ build tools are not installed"; return false; }
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(out).parent_path() / "obj";
    std::error_code ec;
    fs::create_directories(dir, ec);
    {
        std::ofstream rsp(dir / "cl.rsp");
        rsp << "/nologo /c /MP";
        for (const auto& f : game_module_cflags()) rsp << " " << f;
        for (const auto& i : includes) rsp << " \"/I" << i << "\"";
        rsp << " \"/Fo" << dir.string() << "\\\\\"";
        for (const auto& s : sources) rsp << " \"" << s << "\"";
        rsp << "\n";
        std::ofstream lnk(dir / "link.rsp");
        lnk << "/nologo /DLL \"/OUT:" << out << "\" \"" << (dir / "*.obj").string() << "\" \"" << import_lib << "\"";
        for (const char* e : {"g_recomp_funcs,DATA", "g_recomp_func_count,DATA", "g_recomp_code_base,DATA",
                              "g_recomp_code_end,DATA", "orig_OSSaveContext"})
            lnk << " /EXPORT:" << e;
        lnk << "\n";
        std::ofstream bat(dir / "build.bat");
        bat << "@echo off\r\necho [build] Visual Studio environment\r\ncall \"" << bat_env << "\" >nul || exit /b 1\r\n"
            << "echo [build] compiling\r\ncl @\"" << (dir / "cl.rsp").string() << "\" || exit /b 1\r\n"
            << "echo [build] linking\r\nlink @\"" << (dir / "link.rsp").string() << "\" || exit /b 1\r\n"
            << "echo [build] done\r\n";
    }
    if (progress) progress(0, sources.size());
    if (plat_run({"cmd", "/c", (dir / "build.bat").string()}, log.c_str()) != 0) {
        *err = "building the game failed; see " + log;
        return false;
    }
    if (progress) progress(sources.size(), sources.size());
    fs::remove_all(dir, ec);
    return true;
}
#else
bool game_module_compiler_present(std::string* missing) {
    if (plat_run({"cc", "--version"}, kPlatNull) == 0) return true;
    *missing = "cc";
    return false;
}

bool game_module_build(const std::vector<std::string>& sources, const std::vector<std::string>& includes,
                       const std::string& out, const std::string& import_lib, const std::string& log,
                       std::string* err, const std::function<void(size_t, size_t)>& progress) {
    (void)import_lib;   // the executable's symbols are looked up as the module loads
    // Compiled in parallel, one compiler per core.
    std::atomic<size_t> next{0}, done{0};
    std::atomic<bool> failed{false};
    std::mutex progress_mutex;
    const std::vector<std::string> flags = game_module_cflags();
    auto worker = [&] {
        for (size_t i; !failed && (i = next++) < sources.size();) {
            std::vector<std::string> argv = {"cc"};
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
    std::vector<std::string> link = {"cc",
#ifdef __APPLE__
                                     "-dynamiclib", "-undefined", "dynamic_lookup",
#else
                                     "-shared",
#endif
                                     "-o", out};
    for (const auto& s : sources) link.push_back(s + ".o");
    if (plat_run(link, log.c_str()) != 0) { *err = "linking the game failed; see " + log; return false; }
    return true;
}
#endif
