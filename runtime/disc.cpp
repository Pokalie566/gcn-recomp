// The disc image, read through nod. See disc.h.
#include "disc.h"
#include "sha1.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <nod.h>
#include <vector>

static NodHandle* g_disc;
static std::mutex g_disc_mutex;   // the drive and the audio stream's thread both read

bool disc_open(const char* path, std::string* err) {
    std::lock_guard<std::mutex> lk(g_disc_mutex);
    nod_free(g_disc);
    g_disc = nullptr;
    if (nod_disc_open(path, nullptr, &g_disc) == NOD_RESULT_OK) return true;
    g_disc = nullptr;
    if (err) *err = nod_error_message() ? nod_error_message() : "cannot read the disc image";
    return false;
}

bool iso_read(uint64_t offset, void* dst, uint32_t len) {
    std::lock_guard<std::mutex> lk(g_disc_mutex);
    if (!g_disc || nod_seek(g_disc, (int64_t)offset, 0) < 0) return false;
    uint8_t* p = (uint8_t*)dst;
    while (len) {
        const int64_t n = nod_read(g_disc, p, len);
        if (n <= 0) return false;
        p += n;
        len -= (uint32_t)n;
    }
    return true;
}

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// Where the game can read: [begin, end) ranges of the disc.
static bool used_ranges(std::vector<std::pair<uint64_t, uint64_t>>& used, std::string* err) {
    uint8_t hdr[0x440], app[0x20], dol[0x100];
    if (!iso_read(0, hdr, sizeof(hdr)) || !iso_read(0x2440, app, sizeof(app))) { *err = "cannot read the disc header"; return false; }
    used.push_back({0, 0x2460 + (uint64_t)be32(app + 0x14) + be32(app + 0x18)});   // header, apploader
    const uint32_t dol_off = be32(hdr + 0x420), fst_off = be32(hdr + 0x424), fst_size = be32(hdr + 0x428);
    if (!iso_read(dol_off, dol, sizeof(dol))) { *err = "cannot read main.dol's header"; return false; }
    uint64_t dol_end = dol_off + 0x100;
    for (int i = 0; i < 18; i++)
        if (be32(dol + 0x90 + 4 * i)) dol_end = std::max<uint64_t>(dol_end, (uint64_t)dol_off + be32(dol + 4 * i) + be32(dol + 0x90 + 4 * i));
    used.push_back({dol_off, dol_end});
    if (!fst_size || fst_size > (64u << 20)) { *err = "the disc has no file system"; return false; }
    std::vector<uint8_t> fst(fst_size);
    if (!iso_read(fst_off, fst.data(), fst_size)) { *err = "cannot read the file system"; return false; }
    used.push_back({fst_off, (uint64_t)fst_off + fst_size});
    const uint32_t n = be32(&fst[8]);
    if ((uint64_t)n * 12 > fst_size) { *err = "the file system is damaged"; return false; }
    for (uint32_t i = 1; i < n; i++)
        if (!fst[i * 12]) used.push_back({be32(&fst[i * 12 + 4]), (uint64_t)be32(&fst[i * 12 + 4]) + be32(&fst[i * 12 + 8])});
    return true;
}

bool disc_copy_ciso(const std::string& dst, std::string* sha1, std::string* err,
                    const std::function<void(float)>& progress) {
    std::vector<std::pair<uint64_t, uint64_t>> used;
    if (!used_ranges(used, err)) return false;
    uint64_t size;
    {
        std::lock_guard<std::mutex> lk(g_disc_mutex);
        size = g_disc ? nod_disc_size(g_disc) : 0;
    }
    if (!size) { *err = "no disc open"; return false; }
    // CISO: "CISO", the block size (little-endian), then one byte per block, 1 if stored, in
    // a 0x8000-byte header. The smallest block size whose map covers the disc.
    const size_t kMap = 0x8000 - 8;
    uint32_t bs = 0x8000;
    while ((size + bs - 1) / bs > kMap) bs <<= 1;
    const uint64_t blocks = (size + bs - 1) / bs;
    std::vector<uint8_t> head(0x8000, 0);
    memcpy(head.data(), "CISO", 4);
    for (int i = 0; i < 4; i++) head[4 + i] = (uint8_t)(bs >> (8 * i));
    uint8_t* map = &head[8];
    for (const auto& [a, b] : used)
        for (uint64_t k = a / bs; k * bs < b && k < blocks; k++) map[k] = 1;
    map[blocks - 1] = 1;   // so that the copy is as long as the disc

    const std::string tmp = dst + ".part";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dst).parent_path(), ec);
    FILE* out = fopen(tmp.c_str(), "wb");
    if (!out) { *err = "cannot write " + tmp; return false; }
    bool ok = fwrite(head.data(), 1, head.size(), out) == head.size();
    std::vector<uint8_t> buf(bs);
    Sha1 h;
    for (uint64_t k = 0; ok && k < blocks; k++) {
        const uint32_t n = (uint32_t)std::min<uint64_t>(bs, size - k * bs);
        std::fill(buf.begin(), buf.end(), 0);
        ok = iso_read(k * bs, buf.data(), n);
        h.update(buf.data(), n);
        if (ok && map[k]) ok = fwrite(buf.data(), 1, bs, out) == bs;
        if (progress && k % 64 == 0) progress((float)k / (float)blocks);
    }
    ok = fclose(out) == 0 && ok;
    if (ok) std::filesystem::rename(tmp, dst, ec);
    if (!ok || ec) {
        std::filesystem::remove(tmp, ec);
        *err = "cannot copy the disc to " + dst;
        return false;
    }
    *sha1 = h.hex();
    return true;
}
