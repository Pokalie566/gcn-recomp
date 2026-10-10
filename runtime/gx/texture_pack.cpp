// Custom textures in Dolphin's format. See texture_pack.h.
#include "texture_pack.h"
#include "texture.h"
#include "../runtime.h"
#include "../platform.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_REDEFINE
#include "../third_party/stb_image.h"

namespace fs = std::filesystem;

namespace gx {

// ---- XXH64 ------------------------------------------------------------------------------
// The 64-bit xxHash, as specified by its reference (github.com/Cyan4973/xxHash), which is
// what Dolphin names textures with. A game project's tests can check it against the
// reference's output (lm-recomp's tests/texture_pack_test.cpp does).
namespace {
constexpr uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull,
                   P4 = 0x85EBCA77C2B2AE63ull, P5 = 0x27D4EB2F165667C5ull;
uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
uint64_t le64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint64_t round64(uint64_t acc, uint64_t input) { return rotl(acc + input * P2, 31) * P1; }
uint64_t merge64(uint64_t acc, uint64_t v) { return (acc ^ round64(0, v)) * P1 + P4; }
}  // namespace

uint64_t xxh64(const void* data, size_t len, uint64_t seed) {
    const uint8_t* p = (const uint8_t*)data;
    const uint8_t* const end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        for (; p + 32 <= end; p += 32) {
            v1 = round64(v1, le64(p));
            v2 = round64(v2, le64(p + 8));
            v3 = round64(v3, le64(p + 16));
            v4 = round64(v4, le64(p + 24));
        }
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = merge64(merge64(merge64(merge64(h, v1), v2), v3), v4);
    } else {
        h = seed + P5;
    }
    h += (uint64_t)len;
    for (; p + 8 <= end; p += 8) h = rotl(h ^ round64(0, le64(p)), 27) * P1 + P4;
    if (p + 4 <= end) { h = rotl(h ^ (uint64_t)le32(p) * P1, 23) * P2 + P3; p += 4; }
    for (; p < end; p++) h = rotl(h ^ *p * P5, 11) * P1;
    h ^= h >> 33; h *= P2;
    h ^= h >> 29; h *= P3;
    return h ^ (h >> 32);
}

// ---- Names --------------------------------------------------------------------------------
TexName texture_name(const uint8_t* data, uint32_t fmt, uint32_t w, uint32_t h, bool mipmapped,
                     const uint8_t* tlut, uint32_t tlut_avail) {
    const uint32_t size = texture_size_bytes(fmt, w, h);
    const uint32_t pal_bytes = fmt == TF_CI4 ? 16 * 2 : fmt == TF_CI8 ? 256 * 2 : fmt == TF_CI14X2 ? 16384 * 2 : 0;
    // The palette is hashed over the entries the texels reach, lowest to highest.
    uint32_t lo = 0xFFFF, hi = 0;
    if (fmt == TF_CI4) {
        for (uint32_t i = 0; i < size; i++) {
            lo = std::min({lo, data[i] & 15u, (uint32_t)data[i] >> 4});
            hi = std::max({hi, data[i] & 15u, (uint32_t)data[i] >> 4});
        }
    } else if (fmt == TF_CI8) {
        for (uint32_t i = 0; i < size; i++) { lo = std::min<uint32_t>(lo, data[i]); hi = std::max<uint32_t>(hi, data[i]); }
    } else if (fmt == TF_CI14X2) {
        // As Dolphin computes it: the swap of a single byte, so the index is (byte & 0x3F) << 8.
        for (uint32_t i = 0; i < size; i += 2) {
            const uint32_t v = ((uint32_t)data[i] << 8) & 0x3FFF;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
    char buf[64];
    TexName n;
    snprintf(buf, sizeof(buf), "tex1_%ux%u%s", w, h, mipmapped ? "_m" : "");
    n.base = buf;
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)xxh64(data, size, 0));
    n.tex = buf;
    if (pal_bytes) {
        uint32_t off = 2 * lo, len = 2 * (hi + 1 - lo);
        if (off > tlut_avail) off = tlut_avail;
        len = std::min(len, tlut_avail - off);
        snprintf(buf, sizeof(buf), "_%016llx", (unsigned long long)(len ? xxh64(tlut + off, len, 0) : 0));
        n.tlut = buf;
    }
    n.fmt = std::to_string(fmt);
    return n;
}

// ---- The pack -----------------------------------------------------------------------------
namespace {
bool g_init;
bool g_dump;
bool g_enabled = true;
std::string g_dump_dir;
std::unordered_map<std::string, std::string> g_index;   // name -> file
std::unordered_set<std::string> g_dumped;

std::string base_dir() { return g_save_dir ? std::string(g_save_dir) : std::string("."); }

void init() {
    if (g_init) return;
    g_init = true;
    if (const char* e = getenv("GCN_TEXTURE_DUMP")) g_dump |= atoi(e) != 0;
    if (g_dump) {
        g_dump_dir = base_dir() + "/textures_dump/" GCN_DISC_ID;
        plat_make_dirs(g_dump_dir.c_str());
        fprintf(stderr, "[textures] dumping to %s\n", g_dump_dir.c_str());
    }
    const char* env = getenv("GCN_TEXTURES");
    const fs::path root = env ? fs::path(env) : fs::path(base_dir()) / "textures";
    std::error_code ec;
    fs::path dir = root / GCN_DISC_ID;
    if (!fs::is_directory(dir, ec)) dir = root / std::string(GCN_DISC_ID).substr(0, 3);
    if (!fs::is_directory(dir, ec)) return;
    size_t dds = 0, dupes = 0;
    auto is_png = [](const std::string& p) { return p.size() > 4 && strcasecmp(p.c_str() + p.size() - 4, ".png") == 0; };
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)tolower(c); });
        std::string name = it->path().stem().string();
        if (name.compare(0, 5, "tex1_") != 0) continue;
        if (ext != ".png" && ext != ".dds") continue;
        dds += ext == ".dds";
        // "_arb": mipmaps the pack drew itself; generated ones stand in for them.
        if (const size_t arb = name.rfind("_arb"); arb != std::string::npos) name.erase(arb, 4);
        // The same texture as PNG and DDS: the PNG, which is never lossy.
        auto [at, fresh] = g_index.emplace(name, it->path().string());
        if (!fresh) {
            if (ext == ".png" && !is_png(at->second)) at->second = it->path().string();
            else dupes++;
        }
    }
    fprintf(stderr, "[textures] %zu custom textures in %s", g_index.size(), dir.string().c_str());
    if (dds) fprintf(stderr, ", %zu of them DDS", dds);
    if (dupes) fprintf(stderr, ", %zu duplicate names ignored", dupes);
    fprintf(stderr, "\n");
}

// Each level half the last, a 2x2 box, down to 1x1. Channels are averaged byte by byte.
std::vector<uint32_t> halve(const std::vector<uint32_t>& src, uint32_t w, uint32_t h) {
    const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
    std::vector<uint32_t> out((size_t)nw * nh);
    for (uint32_t y = 0; y < nh; y++)
        for (uint32_t x = 0; x < nw; x++) {
            const uint32_t x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
            const uint32_t y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
            const uint32_t a = src[(size_t)y0 * w + x0], b = src[(size_t)y0 * w + x1];
            const uint32_t c = src[(size_t)y1 * w + x0], d = src[(size_t)y1 * w + x1];
            uint32_t v = 0;
            for (int s = 0; s < 32; s += 8)
                v |= ((((a >> s) & 255) + ((b >> s) & 255) + ((c >> s) & 255) + ((d >> s) & 255) + 2) / 4) << s;
            out[(size_t)y * nw + x] = v;
        }
    return out;
}
}  // namespace

bool texture_pack_active() {
    init();
    return g_dump || (g_enabled && !g_index.empty());
}

const char* texture_dump_dir() { return g_dump ? g_dump_dir.c_str() : nullptr; }

void texture_pack_enable_dump() { g_dump = true; }
void texture_pack_set_enabled(bool on) { g_enabled = on; }

void texture_pack_reset() {
    g_init = false;
    g_index.clear();
}

size_t texture_pack_count() {
    init();
    return g_index.size();
}

// ---- DDS ------------------------------------------------------------------------------------
namespace {
uint32_t px(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }

// A BC1 colour block's four colours, `opaque` for BC2 and BC3, whose blocks always use four.
void bc1_colours(const uint8_t* b, bool opaque, uint32_t out[4]) {
    const uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
    auto rgb = [](uint16_t c, int i) {
        const int v[3] = {(c >> 11 & 31) * 255 / 31, (c >> 5 & 63) * 255 / 63, (c & 31) * 255 / 31};
        return v[i];
    };
    for (int i = 0; i < 3; i++) {
        const int a = rgb(c0, i), z = rgb(c1, i);
        const int s = i * 8;
        out[0] |= (uint32_t)a << s;
        out[1] |= (uint32_t)z << s;
        if (c0 > c1 || opaque) {
            out[2] |= (uint32_t)((2 * a + z) / 3) << s;
            out[3] |= (uint32_t)((a + 2 * z) / 3) << s;
        } else {
            out[2] |= (uint32_t)((a + z) / 2) << s;
        }
    }
    out[0] |= 0xFF000000u;
    out[1] |= 0xFF000000u;
    out[2] |= 0xFF000000u;
    if (c0 > c1 || opaque) out[3] |= 0xFF000000u;   // else transparent black
}
}  // namespace

bool dds_decode(const uint8_t* d, size_t n, int* w, int* h, std::vector<uint32_t>* out, std::string* err) {
    if (n < 128 || memcmp(d, "DDS ", 4)) { *err = "not a DDS file"; return false; }
    *h = (int)le32(d + 12);
    *w = (int)le32(d + 16);
    const uint32_t pf_flags = le32(d + 80), bits = le32(d + 88);
    const uint8_t* fourcc = d + 84;
    size_t off = 128;
    enum { NONE, BC1, BC2, BC3, RGBA, BGRA, MASKS } kind = NONE;
    if (!memcmp(fourcc, "DXT1", 4)) kind = BC1;
    else if (!memcmp(fourcc, "DXT2", 4) || !memcmp(fourcc, "DXT3", 4)) kind = BC2;
    else if (!memcmp(fourcc, "DXT4", 4) || !memcmp(fourcc, "DXT5", 4)) kind = BC3;
    else if (!memcmp(fourcc, "DX10", 4)) {
        if (n < 148) { *err = "truncated"; return false; }
        off = 148;
        switch (le32(d + 128)) {   // DXGI_FORMAT
        case 71: case 72: kind = BC1; break;
        case 74: case 75: kind = BC2; break;
        case 77: case 78: kind = BC3; break;
        case 28: case 29: kind = RGBA; break;
        case 87: case 91: kind = BGRA; break;
        default: *err = "DXGI format " + std::to_string(le32(d + 128)) + " (BC7?) is not supported"; return false;
        }
    } else if ((pf_flags & 0x40) && bits == 32) kind = MASKS;   // DDPF_RGB, 32-bit
    if (kind == NONE || *w <= 0 || *h <= 0 || *w > 16384 || *h > 16384) { *err = "an unsupported DDS format"; return false; }
    const size_t W = (size_t)*w, H = (size_t)*h;
    out->assign(W * H, 0);
    if (kind == RGBA || kind == BGRA || kind == MASKS) {
        if (n < off + W * H * 4) { *err = "truncated"; return false; }
        uint32_t m[4] = {0xFF, 0xFF00, 0xFF0000, 0xFF000000u};
        if (kind == BGRA) { m[0] = 0xFF0000; m[2] = 0xFF; }
        if (kind == MASKS) for (int i = 0; i < 4; i++) m[i] = le32(d + 92 + 4 * i);
        auto chan = [](uint32_t v, uint32_t mask, uint32_t dflt) -> uint32_t {
            if (!mask) return dflt;
            int shift = 0;
            while (!(mask >> shift & 1)) shift++;
            return (v & mask) >> shift & 0xFF;
        };
        for (size_t i = 0; i < W * H; i++) {
            const uint32_t v = le32(d + off + 4 * i);
            (*out)[i] = px(chan(v, m[0], 0), chan(v, m[1], 0), chan(v, m[2], 0), chan(v, m[3], 255));
        }
        return true;
    }
    const size_t bw = (W + 3) / 4, bh = (H + 3) / 4, bsize = kind == BC1 ? 8 : 16;
    if (n < off + bw * bh * bsize) { *err = "truncated"; return false; }
    for (size_t by = 0; by < bh; by++)
        for (size_t bx = 0; bx < bw; bx++) {
            const uint8_t* b = d + off + (by * bw + bx) * bsize;
            const uint8_t* colour = kind == BC1 ? b : b + 8;
            uint32_t c[4] = {};
            bc1_colours(colour, kind != BC1, c);
            const uint32_t idx = le32(colour + 4);
            uint8_t alpha[16];
            if (kind == BC2) {
                for (int i = 0; i < 16; i++) alpha[i] = (uint8_t)((b[i / 2] >> (4 * (i & 1)) & 15) * 17);
            } else if (kind == BC3) {
                uint8_t a[8] = {b[0], b[1]};
                for (int i = 1; i < 7; i++)
                    a[i + 1] = b[0] > b[1] ? (uint8_t)(((7 - i) * b[0] + i * b[1]) / 7)
                             : i < 5 ? (uint8_t)(((5 - i) * b[0] + i * b[1]) / 5) : (uint8_t)(i == 5 ? 0 : 255);
                uint64_t bitsa = 0;
                for (int i = 0; i < 6; i++) bitsa |= (uint64_t)b[2 + i] << (8 * i);
                for (int i = 0; i < 16; i++) alpha[i] = a[bitsa >> (3 * i) & 7];
            }
            for (int i = 0; i < 16; i++) {
                const size_t x = bx * 4 + (i & 3), y = by * 4 + i / 4;
                if (x >= W || y >= H) continue;
                uint32_t v = c[idx >> (2 * i) & 3];
                if (kind != BC1) v = (v & 0x00FFFFFF) | (uint32_t)alpha[i] << 24;
                (*out)[y * W + x] = v;
            }
        }
    return true;
}

void texture_pack_apply(const TexName& n, TexData& td, uint32_t game_levels) {
    init();
    auto find = [](const std::string& key) -> const std::string* {
        if (!g_enabled) return nullptr;
        auto it = g_index.find(key);
        return it == g_index.end() ? nullptr : &it->second;
    };
    // Dolphin's order: the exact name, then any palette, then any texels.
    const std::string* path = find(n.full());
    if (!path) path = find(n.base + "_" + n.tex + "_$_" + n.fmt);
    if (!path) path = find(n.base + "_$" + n.tlut + "_" + n.fmt);
    if (!path) {
        if (g_dump && g_dumped.insert(n.full()).second) td.dump_name = n.full();
        return;
    }
    // ponytail: decoded from disk on every miss -- a texture unused for a few seconds is
    // evicted and read again, a hitch with a large one. Keep the decoded ones (Dolphin's
    // "prefetch") if that shows.
    int w = 0, h = 0, ch = 0;
    std::vector<std::vector<uint32_t>> levels(1);
    std::string err;
    bool ok;
    if (const size_t dot = path->size() - 4; strcasecmp(path->c_str() + dot, ".dds") == 0) {
        std::ifstream in(*path, std::ios::binary);
        const std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        ok = dds_decode(d.data(), d.size(), &w, &h, &levels[0], &err);   // the top level; mipmaps are made
    } else if (stbi_uc* p = stbi_load(path->c_str(), &w, &h, &ch, 4)) {
        levels[0].resize((size_t)w * h);
        memcpy(levels[0].data(), p, (size_t)w * h * 4);
        stbi_image_free(p);
        ok = true;
    } else {
        err = stbi_failure_reason();
        ok = false;
    }
    if (!ok) {
        static std::unordered_set<std::string> warned;
        if (warned.insert(*path).second) fprintf(stderr, "[textures] cannot read %s: %s\n", path->c_str(), err.c_str());
        return;
    }
    if (game_levels > 1)
        for (uint32_t lw = (uint32_t)w, lh = (uint32_t)h; lw > 1 || lh > 1; lw = std::max(1u, lw / 2), lh = std::max(1u, lh / 2))
            levels.push_back(halve(levels.back(), lw, lh));
    td.levels = std::move(levels);
    td.data_w = (uint32_t)w;
    td.data_h = (uint32_t)h;
    static const bool texlog = getenv("GCN_TEXLOG") != nullptr;
    if (texlog) fprintf(stderr, "[textures] %s -> %s (%dx%d)\n", n.full().c_str(), path->c_str(), w, h);
}

}  // namespace gx
