// Preparing the game on the player's machine. See prepare.h.
#include "prepare.h"
#include "game_module.h"
#include "platform.h"
#include "runtime.h"
#include "sha1.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>
#ifdef __APPLE__
#include <sys/xattr.h>
#endif

namespace fs = std::filesystem;

#ifndef GCN_RES_IMPORT_LIB
#define GCN_RES_IMPORT_LIB ""   // set on the game's own target, on Windows
#endif

PrepareSources prepare_sources() {
    if (const char* r = getenv("GCN_RESOURCES")) {
        const std::string d = r;
        return {d + "/recomp", d + "/tables", d + "/symbols.txt", d + "/include", d + "/lib/" GCN_GAME_NAME ".lib"};
    }
    return {GCN_RES_RECOMP, GCN_RES_TABLES, GCN_RES_SYMBOLS, GCN_RES_INCLUDE, GCN_RES_IMPORT_LIB};
}

// How python 3 is started: `python3` on macOS and Linux; on Windows the py launcher that
// python.org's installer puts in, else `python`.
static std::vector<std::string> python() {
#ifdef _WIN32
    if (plat_run({"py", "-3", "--version"}, kPlatNull) == 0) return {"py", "-3"};
    return {"python"};
#else
    return {"python3"};
#endif
}

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// main.dol as the disc stores it: header, then its sections up to the furthest one's end.
static bool read_dol(const char* iso, std::vector<uint8_t>& dol, std::string* disc_id, std::string* err) {
    if (!plat_readable(iso)) { *err = std::string("cannot read the disc image ") + iso; return false; }
    di_init(iso);
    uint8_t hdr[0x440];
    if (!iso_read(0, hdr, sizeof(hdr))) { *err = "cannot read the disc header"; return false; }
    *disc_id = std::string((const char*)hdr, 6);
    const uint32_t dol_off = be32(hdr + 0x420);
    uint8_t dh[0x100];
    if (!iso_read(dol_off, dh, sizeof(dh))) { *err = "cannot read main.dol's header"; return false; }
    uint32_t size = 0x100;
    for (int i = 0; i < 18; i++) {
        const uint32_t off = be32(dh + 4 * i), len = be32(dh + 0x90 + 4 * i);
        if (len) size = std::max(size, off + len);
    }
    if (size > 64u << 20) { *err = "main.dol is implausibly large: not a GameCube disc?"; return false; }
    dol.resize(size);
    if (!iso_read(dol_off, dol.data(), size)) { *err = "cannot read main.dol"; return false; }
    return true;
}

bool prepare_check_disc(const char* iso, std::string* dol_sha1, std::string* err) {
    std::vector<uint8_t> dol;
    std::string id;
    if (!read_dol(iso, dol, &id, err)) return false;
    if (id != GCN_DISC_ID) {
        *err = "this disc is " + id + ", not " GCN_DISC_ID " (" GCN_GAME_TITLE ")";
        return false;
    }
    Sha1 h;
    h.update(dol.data(), dol.size());
    *dol_sha1 = h.hex();
    if (*dol_sha1 != GCN_DOL_SHA1) {
        *err = "this " GCN_DISC_ID " disc is another revision (main.dol " + *dol_sha1 +
               ", expected " GCN_DOL_SHA1 ")";
        return false;
    }
    return true;
}

// A hash of everything a prepared game depends on besides the disc: the recompiler, the
// tables, the symbols, the headers, the flags. Any change prepares again.
static std::string inputs_hash() {
    const PrepareSources s = prepare_sources();
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& dir : {s.recompiler, s.tables, s.include})
        for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec)) files.push_back(it->path());
    files.push_back(s.symbols);
    std::sort(files.begin(), files.end());
    Sha1 h;
    for (const auto& p : files) {
        const std::string name = p.filename().string();
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".pyc") == 0) continue;
        std::ifstream in(p, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        h.update(name.data(), name.size());
        h.update(data.data(), data.size());
    }
    for (const auto& f : game_module_cflags()) h.update(f.data(), f.size());
    return h.hex();
}

std::string prepare_module_path(const std::string& dol_sha1) {
    return save_path("game/" + dol_sha1.substr(0, 12) + "-" + inputs_hash().substr(0, 12) + "/" +
                     game_module_filename());
}

bool prepare_tools_present(std::string* missing) {
    missing->clear();
#ifdef __APPLE__
    // Without the Command Line Tools, cc and python3 are stubs that put up Apple's installer
    // when run; asking xcode-select first keeps a check from doing that by itself.
    if (plat_run({"xcode-select", "-p"}, kPlatNull) != 0) {
        *missing = "Apple's Command Line Tools";
        return false;
    }
#endif
    std::string compiler;
    if (!game_module_compiler_present(&compiler)) *missing = compiler;
    std::vector<std::string> py = python();
    py.push_back("--version");
    if (plat_run(py, kPlatNull) != 0) *missing += std::string(missing->empty() ? "" : ", ") + "python3";
    return missing->empty();
}

bool prepare_game(const char* iso, std::string* module, std::string* err,
                  const std::function<void(float, const std::string&)>& progress) {
    auto say = [&](float f, const std::string& what) { if (progress) progress(f, what); };
    say(0.0f, "checking the disc");
    std::string sha1;
    if (!prepare_check_disc(iso, &sha1, err)) return false;
    std::string missing;
    if (!prepare_tools_present(&missing)) {
        *err = "missing tools: " + missing +
#if defined(__APPLE__)
               " -- install Apple's Command Line Tools: xcode-select --install";
#elif defined(_WIN32)
               " -- install Visual Studio's C++ build tools and Python 3 (python.org)";
#else
               " -- install a C compiler and python3";
#endif
        return false;
    }

    const fs::path final_path = prepare_module_path(sha1);
    const fs::path final_dir = final_path.parent_path();
    const fs::path work = final_dir.string() + ".work";
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work / "gen", ec);
    if (ec) { *err = "cannot create " + work.string() + ": " + ec.message(); return false; }
    const std::string log = (work / "prepare.log").string();

    say(0.02f, "extracting main.dol");
    std::vector<uint8_t> dol;
    std::string id;
    if (!read_dol(iso, dol, &id, err)) return false;
    {
        std::ofstream out(work / "main.dol", std::ios::binary);
        out.write((const char*)dol.data(), (std::streamsize)dol.size());
        if (!out) { *err = "cannot write main.dol"; return false; }
    }

    say(0.05f, "recompiling");
    const PrepareSources src = prepare_sources();
    std::vector<std::string> recomp = python();
    recomp.insert(recomp.end(), {src.recompiler + "/recomp.py", (work / "main.dol").string(), src.symbols,
                                 (work / "gen").string(), src.tables});
    if (plat_run(recomp, log.c_str()) != 0) {
        *err = "the recompiler failed; see " + log;
        return false;
    }
    fs::remove(work / "main.dol", ec);   // the player's own data, not kept beyond its use

    std::vector<std::string> sources;
    for (auto it = fs::directory_iterator(work / "gen", ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->path().extension() == ".c") sources.push_back(it->path().string());
    std::sort(sources.begin(), sources.end());
    if (sources.empty()) { *err = "the recompiler wrote nothing; see " + log; return false; }

    say(0.25f, "compiling");
    if (!game_module_build(sources, {src.include, (work / "gen").string()}, (work / game_module_filename()).string(),
                           src.import_lib, log, err, [&](size_t done, size_t total) {
                               say(0.25f + 0.70f * (float)done / (float)total,
                                   "compiling " + std::to_string(done) + "/" + std::to_string(total));
                           }))
        return false;

    // Only the module and the log are kept: the generated C and the objects are the game's
    // code, and nothing needs them once it is linked.
    fs::remove_all(work / "gen", ec);
    fs::remove_all(final_dir, ec);
    fs::rename(work, final_dir, ec);
    if (ec) { *err = "cannot move the prepared game into place: " + ec.message(); return false; }
#ifdef __APPLE__
    // An app downloaded from the web is quarantined, and what it makes may be marked so too;
    // the module is the player's own, built here, and must load without macOS asking.
    removexattr(final_path.c_str(), "com.apple.quarantine", 0);
#endif
    say(1.0f, "ready");
    *module = final_path.string();
    return true;
}

bool game_module_ensure(const char* iso, bool force) {
    std::string sha1, err;
    if (!prepare_check_disc(iso, &sha1, &err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return false;
    }
    std::string path = force ? std::string() : game_module_find(prepare_module_path(sha1));
    if (path.empty()) {
        fprintf(stderr, "[prepare] preparing " GCN_GAME_TITLE " from %s (once)\n", iso);
        int last = -1;
        const bool ok = prepare_game(iso, &path, &err, [&](float f, const std::string& what) {
            const int pct = (int)(f * 100);
            if (pct != last) { last = pct; fprintf(stderr, "\r[prepare] %3d%%  %-30s", pct, what.c_str()); }
        });
        fprintf(stderr, "\n");
        if (!ok) {
            fprintf(stderr, "[prepare] %s\n", err.c_str());
            return false;
        }
    }
    if (!game_module_load(path, &err)) {
        fprintf(stderr, "cannot load the game: %s\n", err.c_str());
        return false;
    }
    return true;
}
