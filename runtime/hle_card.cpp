// The memory card, on Aurora's (dolphin/card.h, aurora::card): the game's CARD calls -- the
// SDK's public functions, listed in the game's hle.txt -- go to it instead of the SDK's own
// code driving the card over EXI. Aurora keeps the card as a folder of .gci files, one per
// save, as Dolphin does (card.h).
//
// Aurora's CARD is synchronous: an ...Async call does the work, then calls its callback --
// the game's, here, called before the HLE returns, which the SDK's callers allow (the
// result comes back the same way it would from the card's interrupt).
#include "card.h"
#include "runtime.h"
#include <aurora/card.h>
#include <dolphin/card.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

const char* g_card_base;

namespace aurora {
extern char g_gameName[5];   // Aurora's (lib/internal.hpp; patches/aurora-card-game-name.patch): its DVD sets it, ours does not
}

namespace {

uint32_t arg(CPU* c, int n) { return c->r[3 + n]; }
void ret(CPU* c, int32_t v) { c->r[3] = (uint32_t)v; }

// The game's callback, if it gave one, with (chan, result), as the card's interrupt would.
void callback(CPU* c, uint32_t cb, int32_t chan, int32_t result) {
    if (!cb) return;
    const uint32_t lr = c->lr;
    c->r[3] = (uint32_t)chan;
    c->r[4] = (uint32_t)result;
    call_indirect(c, cb);
    c->lr = lr;
}

// An async call: started (READY) and its result to the callback, or, with no callback, the
// result itself.
void async_done(CPU* c, uint32_t cb, int32_t chan, int32_t result) {
    callback(c, cb, chan, result);
    ret(c, cb ? CARD_RESULT_READY : result);
}

void put_s32(uint32_t addr, int32_t v) { if (addr) mem_w32(addr, (uint32_t)v); }

// CARDFileInfo: chan, fileNo, offset, length (s32 each), iBlock (u16).
CARDFileInfo info_in(uint32_t a) {
    CARDFileInfo f{};
    f.chan = (s32)mem_r32(a);
    f.fileNo = (s32)mem_r32(a + 4);
    f.offset = (s32)mem_r32(a + 8);
    f.length = (s32)mem_r32(a + 12);
    f.iBlock = mem_r16(a + 16);
    return f;
}
void info_out(uint32_t a, const CARDFileInfo& f) {
    mem_w32(a, (uint32_t)f.chan);
    mem_w32(a + 4, (uint32_t)f.fileNo);
    mem_w32(a + 8, (uint32_t)f.offset);
    mem_w32(a + 12, (uint32_t)f.length);
    mem_w16(a + 16, f.iBlock);
}

// CARDStat, same layout as the SDK's (card.h), big-endian in the game's memory.
CARDStat stat_in(uint32_t a) {
    CARDStat s{};
    memcpy(s.fileName, mem_ptr(a), CARD_FILENAME_MAX);
    s.length = mem_r32(a + 0x20);
    s.time = mem_r32(a + 0x24);
    memcpy(s.gameName, mem_ptr(a + 0x28), 4);
    memcpy(s.company, mem_ptr(a + 0x2C), 2);
    s.bannerFormat = mem_r8(a + 0x2E);
    s.iconAddr = mem_r32(a + 0x30);
    s.iconFormat = mem_r16(a + 0x34);
    s.iconSpeed = mem_r16(a + 0x36);
    s.commentAddr = mem_r32(a + 0x38);
    s.offsetBanner = mem_r32(a + 0x3C);
    s.offsetBannerTlut = mem_r32(a + 0x40);
    for (int i = 0; i < CARD_ICON_MAX; i++) s.offsetIcon[i] = mem_r32(a + 0x44 + 4 * i);
    s.offsetIconTlut = mem_r32(a + 0x64);
    s.offsetData = mem_r32(a + 0x68);
    return s;
}
void stat_out(uint32_t a, const CARDStat& s) {
    memcpy(mem_ptr(a), s.fileName, CARD_FILENAME_MAX);
    mem_w32(a + 0x20, s.length);
    mem_w32(a + 0x24, s.time);
    memcpy(mem_ptr(a + 0x28), s.gameName, 4);
    memcpy(mem_ptr(a + 0x2C), s.company, 2);
    mem_w8(a + 0x2E, s.bannerFormat);
    mem_w32(a + 0x30, s.iconAddr);
    mem_w16(a + 0x34, s.iconFormat);
    mem_w16(a + 0x36, s.iconSpeed);
    mem_w32(a + 0x38, s.commentAddr);
    mem_w32(a + 0x3C, s.offsetBanner);
    mem_w32(a + 0x40, s.offsetBannerTlut);
    for (int i = 0; i < CARD_ICON_MAX; i++) mem_w32(a + 0x44 + 4 * i, s.offsetIcon[i]);
    mem_w32(a + 0x64, s.offsetIconTlut);
    mem_w32(a + 0x68, s.offsetData);
}

std::string game4() { return std::string(g_disc_id).substr(0, 4); }
std::string maker2() { return std::string(g_disc_id).substr(4, 2); }

// The older card, a raw image the runtime emulated over EXI: this game's files, as .gci in
// the folder, once (marked in `base`). The image is left as it is.
void migrate(const std::string& raw, const std::string& base, const std::string& folder) {
    std::error_code ec;
    if (!fs::exists(raw, ec) || fs::exists(fs::path(base) / "migrated.txt", ec)) return;
    struct Ctx { const std::string* raw; const std::string* folder; int n; } ctx{&raw, &folder, 0};
    aurora_card_raw_list(raw.c_str(), game4().c_str(), maker2().c_str(), [](const char* name, void* user) {
        auto& x = *(Ctx*)user;
        std::vector<uint8_t> gci(aurora_card_raw_extract(x.raw->c_str(), game4().c_str(), maker2().c_str(), name, nullptr, 0));
        if (gci.empty() || !aurora_card_raw_extract(x.raw->c_str(), game4().c_str(), maker2().c_str(), name, gci.data(), gci.size()))
            return;
        char file[256];
        aurora_card_gci_filename(game4().c_str(), maker2().c_str(), name, file, sizeof(file));
        const fs::path to = fs::path(*x.folder) / file;
        std::error_code e;
        if (fs::exists(to, e)) return;   // never over a save already in the folder
        std::ofstream(to, std::ios::binary).write((const char*)gci.data(), (std::streamsize)gci.size());
        x.n++;
    }, &ctx);
    std::ofstream(fs::path(base) / "migrated.txt") << "the saves of " << raw << " were copied here as .gci\n";
    if (ctx.n) fprintf(stderr, "card: %d save(s) of %s taken over as .gci\n", ctx.n, raw.c_str());
}

}  // namespace

void card_init() {
    static bool done;
    if (done) return;
    done = true;
    const std::string base = g_card_base ? g_card_base : save_path("card");
    // Slot A's folder made first, as Aurora lays it out (aurora/card.h: <base>/<region>/Card A):
    // Aurora mounts a folder that exists, and would otherwise fall back on its own.
    const char region = g_disc_id[3];
    std::error_code ec;
    fs::create_directories(fs::path(base) / (region == 'P' ? "EUR" : region == 'J' ? "JAP" : "USA") / "Card A", ec);
    snprintf(aurora::g_gameName, sizeof(aurora::g_gameName), "%s", game4().c_str());
    CARDSetLoadType(CARD_GCIFOLDER);
    CARDSetBasePath(base.c_str(), -1);
    CARDInit(game4().c_str(), maker2().c_str());
    // An older card beside it: the game's own raw card, or a replayed older log's.
    migrate(g_card_base ? base + "/memcard_a.raw" : save_path("memcard_a.raw"), base, card_folder());
    aurora_card_remount(0);
}

std::string card_folder() {
    char path[1024];
    aurora_card_get_path(game4().c_str(), AURORA_CARD_GCI_DIRECTORY, 0, path, sizeof(path));
    return path;
}

void card_remount() { aurora_card_remount(0); }

// ---- The SDK's functions ----
extern "C" {

void hle_CARDInit(CPU*) {}   // card_init, before the game starts

void hle_CARDProbe(CPU* c) { ret(c, CARDProbe((s32)arg(c, 0))); }

void hle_CARDProbeEx(CPU* c) {
    s32 mem = 0, sector = 0;
    const s32 r = CARDProbeEx((s32)arg(c, 0), &mem, &sector);
    put_s32(arg(c, 1), mem);
    put_s32(arg(c, 2), sector);
    ret(c, r);
}

void hle_CARDMount(CPU* c) { ret(c, CARDMount((s32)arg(c, 0), nullptr, nullptr)); }
void hle_CARDMountAsync(CPU* c) {
    const s32 chan = (s32)arg(c, 0);
    async_done(c, arg(c, 3), chan, CARDMount(chan, nullptr, nullptr));
}
void hle_CARDUnmount(CPU* c) { ret(c, CARDUnmount((s32)arg(c, 0))); }

void hle_CARDCheck(CPU* c) { ret(c, CARDCheck((s32)arg(c, 0))); }
void hle_CARDCheckExAsync(CPU* c) {
    const s32 chan = (s32)arg(c, 0);
    put_s32(arg(c, 1), 0);
    async_done(c, arg(c, 2), chan, CARDCheck(chan));
}

void hle_CARDFormat(CPU* c) { ret(c, CARDFormat((s32)arg(c, 0))); }
void hle_CARDFormatAsync(CPU* c) {
    const s32 chan = (s32)arg(c, 0);
    async_done(c, arg(c, 1), chan, CARDFormat(chan));
}

void hle_CARDFreeBlocks(CPU* c) {
    s32 bytes = 0, files = 0;
    const s32 r = CARDFreeBlocks((s32)arg(c, 0), &bytes, &files);
    put_s32(arg(c, 1), bytes);
    put_s32(arg(c, 2), files);
    ret(c, r);
}

void hle_CARDOpen(CPU* c) {
    CARDFileInfo f{};
    const s32 r = CARDOpen((s32)arg(c, 0), guest_str(arg(c, 1), CARD_FILENAME_MAX).c_str(), &f);
    info_out(arg(c, 2), f);
    ret(c, r);
}

void hle_CARDClose(CPU* c) {
    CARDFileInfo f = info_in(arg(c, 0));
    const s32 r = CARDClose(&f);
    info_out(arg(c, 0), f);
    ret(c, r);
}

void hle_CARDCreate(CPU* c) {
    CARDFileInfo f{};
    const s32 r = CARDCreate((s32)arg(c, 0), guest_str(arg(c, 1), CARD_FILENAME_MAX).c_str(), arg(c, 2), &f);
    info_out(arg(c, 3), f);
    ret(c, r);
}
void hle_CARDCreateAsync(CPU* c) {
    CARDFileInfo f{};
    const s32 chan = (s32)arg(c, 0);
    const s32 r = CARDCreate(chan, guest_str(arg(c, 1), CARD_FILENAME_MAX).c_str(), arg(c, 2), &f);
    info_out(arg(c, 3), f);
    async_done(c, arg(c, 4), chan, r);
}

void hle_CARDRead(CPU* c) {
    const CARDFileInfo f = info_in(arg(c, 0));
    ret(c, CARDRead(&f, mem_ptr(arg(c, 1)), (s32)arg(c, 2), (s32)arg(c, 3)));
}
void hle_CARDReadAsync(CPU* c) {
    const CARDFileInfo f = info_in(arg(c, 0));
    async_done(c, arg(c, 4), f.chan, CARDRead(&f, mem_ptr(arg(c, 1)), (s32)arg(c, 2), (s32)arg(c, 3)));
}

void hle_CARDWrite(CPU* c) {
    const CARDFileInfo f = info_in(arg(c, 0));
    ret(c, CARDWrite(&f, mem_ptr(arg(c, 1)), (s32)arg(c, 2), (s32)arg(c, 3)));
}
void hle_CARDWriteAsync(CPU* c) {
    const CARDFileInfo f = info_in(arg(c, 0));
    async_done(c, arg(c, 4), f.chan, CARDWrite(&f, mem_ptr(arg(c, 1)), (s32)arg(c, 2), (s32)arg(c, 3)));
}

void hle_CARDGetStatus(CPU* c) {
    CARDStat s{};
    const s32 r = CARDGetStatus((s32)arg(c, 0), (s32)arg(c, 1), &s);
    if (r == CARD_RESULT_READY) stat_out(arg(c, 2), s);
    ret(c, r);
}

void hle_CARDSetStatus(CPU* c) {
    const CARDStat s = stat_in(arg(c, 2));
    ret(c, CARDSetStatus((s32)arg(c, 0), (s32)arg(c, 1), &s));
}
void hle_CARDSetStatusAsync(CPU* c) {
    const CARDStat s = stat_in(arg(c, 2));
    const s32 chan = (s32)arg(c, 0);
    async_done(c, arg(c, 3), chan, CARDSetStatus(chan, (s32)arg(c, 1), &s));
}

}  // extern "C"
