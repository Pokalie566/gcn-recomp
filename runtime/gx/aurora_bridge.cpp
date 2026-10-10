// GX on Aurora (github.com/encounter/aurora, MIT): the game's command stream, as the
// hardware's FIFO carries it, handed to Aurora's command processor, which draws it with
// WebGPU -- Metal, D3D12 or Vulkan -- transform and lighting included, on the GPU.
//
// Aurora is made for decompiled games, whose GX calls are its own SDK. Here the game's SDK
// runs, recompiled, and writes what the hardware would read (gx/fifo.cpp feeds it to
// gx::process). Most of that Aurora reads as it is: BP, CP and XF registers and draws, in
// the hardware's encoding. This file parses the stream and passes those through, and
// translates the rest -- everything that names the console's memory by address, which
// Aurora's SDK would have handed over as a pointer through a command of its own (GX_AURORA):
//
//   - vertex arrays (CP 0xA0-0xAF): GX_AURORA_LOAD_ARRAYBASE, a host pointer into g_mem;
//   - textures (BP 0x80-0xBF): GX_AURORA_LOAD_TEXOBJ before the draw that samples them;
//   - palettes (BP 0x64/0x65 loads into TMEM, 0x98+ per texture): kept here, a copy per
//     load, and GX_AURORA_LOAD_TLUT for the texture that reads one;
//   - EFB copies (BP 0x49-0x52): GX_AURORA_LOAD_COPY_SRC/DST/DEST, then the trigger;
//   - display lists (CALL_DL): read here, command by command, from guest memory.
//
// The copy to the external framebuffer (a BP 0x52 with the display bit) is the end of a
// frame: Aurora shows the EFB as it stands and starts the next frame, clearing the EFB with
// the clear colour the game has set.
//
// Aurora processes what it is given on a worker thread, so a draw is read from guest
// memory a little after the game issued it, as on the hardware. Where the game waits for
// the GPU -- a draw-done or a token (BP 0x45, 0x47, 0x48) -- everything given so far is
// processed first, as the hardware would have.
#include "../runtime.h"
#include "../platform.h"
#include "gx.h"
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXCommandList.h>
#include "gx/fifo.hpp"   // Aurora's (lib/gx/fifo.hpp)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

void pe_signal_token(uint16_t token, bool interrupt);
void pe_signal_finish();
void aurora_dump_frame(uint32_t frame);   // aurora_frontend.cpp: --dump-dir
void aurora_dump_named(const char* name);

namespace gx {

std::atomic<uint32_t> g_frames_submitted{0};
State g_state;   // the BP and CP registers as last written (gx.h); XF goes to Aurora only

namespace {

uint32_t (&g_bp)[256] = g_state.bp;
uint32_t (&g_cp)[256] = g_state.cp;
uint32_t& g_bp_mask = g_state.bp_mask;
uint8_t g_tmem_tlut[0x80000];   // the upper half of TMEM, where palettes go (tmem 0x80000+)
uint32_t g_tex_dirty = 0xFF;
bool g_cull_all;                // gen mode's cull mode is "all": no triangle is drawn    // texture maps whose registers changed since their last LOAD_TEXOBJ
// The palettes given to Aurora this frame: Aurora reads one when it draws, which may be
// after the next load has overwritten that part of TMEM.
std::vector<std::unique_ptr<uint8_t[]>> g_frame_tluts;

inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// GCN_FRAMETIME=1: one line per frame -- draws, vertex bytes (what fills Aurora's per-frame
// buffers), and where the guest thread waited on Aurora.
const bool g_frametime = getenv("GCN_FRAMETIME") != nullptr;
uint32_t g_draws, g_vtx_bytes, g_drains, g_copies, g_dls, g_indx, g_tluts, g_arr_sends, g_arr_bytes, g_arr_max;
double g_drain_ms, g_tex_ms, g_arr_ms;
using Clock = std::chrono::steady_clock;
Clock::time_point g_frame_start = Clock::now();
double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// ---- What goes to Aurora: its FIFO, big-endian like the hardware's ----
struct Out {
    uint8_t buf[64];
    uint32_t n = 0;
    Out& u8(uint32_t v) { buf[n++] = (uint8_t)v; return *this; }
    Out& u16(uint32_t v) { return u8(v >> 8).u8(v); }
    Out& u32(uint32_t v) { return u16(v >> 16).u16(v); }
    Out& u64(uint64_t v) { return u32((uint32_t)(v >> 32)).u32((uint32_t)v); }
    Out& f32(float f) { uint32_t v; memcpy(&v, &f, 4); return u32(v); }
    Out& cmd(uint32_t sub) { return u8(GX_AURORA).u16(sub); }
    ~Out() { aurora::gx::fifo::write_data(buf, n); }
};
void pass(const uint8_t* p, uint32_t n) { aurora::gx::fifo::write_data(p, n); }

// ---- Vertices: how many bytes a draw's vertices take, from the VCD/VAT (as the hardware) ----
const uint32_t kCompSize[8] = {1, 1, 2, 2, 4, 0, 0, 0};
const uint32_t kColorSize[8] = {2, 3, 4, 2, 3, 4, 0, 0};

// A vertex of format `vat`: its size, and where its indexed attributes' indices sit.
struct Index { uint8_t array, off, bytes; };
struct Layout { uint32_t size = 0; int n = 0; Index idx[16]; };

Layout layout(int vat) {
    const uint32_t lo = g_cp[0x50], hi = g_cp[0x60], a = g_cp[0x70 + vat], b = g_cp[0x80 + vat], c = g_cp[0x90 + vat];
    Layout L;
    auto attr = [&](int array, uint32_t desc, uint32_t direct, uint32_t indices) {
        if (desc == 1) L.size += direct;
        else if (desc >= 2) {
            for (uint32_t k = 0; k < indices; k++) L.idx[L.n++] = {(uint8_t)array, (uint8_t)L.size, (uint8_t)(desc - 1)}, L.size += desc - 1;
        }
    };
    for (int i = 0; i < 9; i++) L.size += (lo >> i) & 1;   // position/normal matrix index, 8 texture matrix indices
    attr(0, (lo >> 9) & 3, ((a & 1) ? 3 : 2) * kCompSize[(a >> 1) & 7], 1);
    const bool nbt = (a >> 9) & 1, idx3 = (a >> 31) & 1;
    attr(1, (lo >> 11) & 3, (nbt ? 9 : 3) * kCompSize[(a >> 10) & 7], nbt && idx3 ? 3 : 1);
    attr(2, (lo >> 13) & 3, kColorSize[(a >> 14) & 7], 1);
    attr(3, (lo >> 15) & 3, kColorSize[(a >> 18) & 7], 1);
    const uint32_t cnt[8] = {(a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1, (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1};
    const uint32_t fmt[8] = {(a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7};
    for (int i = 0; i < 8; i++) attr(4 + i, (hi >> (2 * i)) & 3, (cnt[i] ? 2 : 1) * kCompSize[fmt[i]], 1);
    return L;
}

uint32_t vertex_size_of(int vat) { return layout(vat).size; }

// ---- Vertex arrays ----
// Aurora copies a vertex array whole into the frame's storage buffer (8 MB) the first time a
// draw uses it, so it must be told how long the array is -- which the hardware never knows:
// it reads where the indices point. Here an array's length is how far the draws that use it
// have indexed into it so far (grow-only, per base address, kept from frame to frame),
// given to Aurora before the draw that needs it. The matrix arrays (12-15) are only read from, by indexed XF loads, and
// are given the rest of RAM when the game sets them.
std::unordered_map<uint32_t, uint32_t> g_extent;   // array base -> bytes the draws have used
struct Sent { uint32_t base = ~0u, size = 0; } g_sent[16];

uint32_t ram_left(uint32_t pa) { return pa < 0x01800000 ? 0x01800000 - pa : 0; }

void send_array(int a, uint32_t base, uint32_t size) {
    if (g_sent[a].base == base && g_sent[a].size == size) return;
    g_sent[a] = {base, size};
    if (a < 12) {
        g_arr_sends++;
        g_arr_bytes += size;
        if (size > g_arr_max) g_arr_max = size;
        static std::unordered_map<uint32_t, uint32_t> uniq;
        static uint32_t uniq_frame = ~0u;
        if (uniq_frame != g_frames_submitted.load()) { uniq.clear(); uniq_frame = g_frames_submitted.load(); }
        uint32_t& u = uniq[base];
        if (size > u) u = size;
        if (g_frametime && g_arr_bytes > (6u << 20) && g_arr_bytes - size <= (6u << 20)) {
            uint64_t ub = 0;
            for (auto& [b, z] : uniq) ub += z;
            fprintf(stderr, "[ft] f%u arrays past 6 MB: %u sends, largest %u KB; %zu distinct arrays, %llu KB\n", g_frames_submitted.load(),
                    g_arr_sends, g_arr_max / 1024, uniq.size(), (unsigned long long)(ub / 1024));
        }
    }
    Out().cmd(GX_AURORA_LOAD_ARRAYBASE + a).u64((uint64_t)phys_ptr(base)).u32(size).u8(0);
}

void load_arrays(const Layout& L, const uint8_t* v, uint32_t count) {
    if (!L.n) return;
    uint32_t max[12] = {};
    uint16_t used = 0;
    for (uint32_t i = 0; i < count; i++, v += L.size)
        for (int k = 0; k < L.n; k++) {
            const Index& x = L.idx[k];
            const uint32_t at = x.bytes == 1 ? v[x.off] : be16(v + x.off);
            used |= 1u << x.array;
            if (at > max[x.array]) max[x.array] = at;
        }
    for (int a = 0; a < 12; a++) {
        if (!(used & (1u << a))) continue;
        const uint32_t base = g_cp[0xA0 + a] & 0x01FFFFFF, stride = g_cp[0xB0 + a] & 0xFF;
        // Grown by doubling: every growth is a fresh copy of the whole array in Aurora, and
        // growing to each draw's furthest index was 146 copies of 3 arrays in one frame.
        uint32_t& e = g_extent[base];
        const uint32_t need = (max[a] + 1) * (stride ? stride : 4);
        if (need > e) e = std::min(std::max(need, e * 2), ram_left(base));
        send_array(a, base, e);
    }
}

// ---- Textures ----
bool is_palette(uint32_t fmt) { return fmt == 8 || fmt == 9 || fmt == 10; }

// Before a draw: every texture map whose registers changed, as Aurora's SDK would describe
// it. Aurora keeps a converted texture under an id and a version, and hashes the texels
// again only when the version moves; the game rewrites a map's registers before most draws,
// so an id of 0 -- hash every time -- costs a full hash per draw. Here a texture's id stands
// for its address, size and format, and its version is the frame: each texture is read
// again once per frame, which is what the OpenGL renderer did. A palette's version moves
// with every palette load, and each frame.
std::unordered_map<uint64_t, uint32_t> g_tex_ids;
uint32_t g_tlut_gen = 1;   // bumped by every palette load, and every frame
struct SentTlut { uint32_t id = 0, version = 0; } g_sent_tlut[8];

uint32_t tex_id(uint32_t img3, uint32_t img0, bool mips) {
    const uint64_t key = (uint64_t)(img3 & 0x1FFFFF) << 32 | img0 << 1 | mips;
    auto [it, fresh] = g_tex_ids.try_emplace(key, (uint32_t)g_tex_ids.size() + 1);
    return it->second;
}

void load_textures() {
    for (int m = 0; g_tex_dirty && m < 8; m++) {
        if (!(g_tex_dirty & (1u << m))) continue;
        g_tex_dirty &= ~(1u << m);
        const uint32_t base = m < 4 ? 0x80 + m : 0xA0 + (m - 4);
        const uint32_t mode0 = g_bp[base], img0 = g_bp[base + 8], img3 = g_bp[base + 0x14], tlut = g_bp[base + 0x18];
        if (!img3) continue;
        const uint32_t fmt = (img0 >> 20) & 0xF;
        uint32_t tlut_id = 0;
        if (is_palette(fmt)) {
            const uint32_t off = std::min<uint32_t>((tlut & 0x3FF) << 9, sizeof(g_tmem_tlut)), entries = fmt == 8 ? 16 : fmt == 9 ? 256 : 16384;
            tlut_id = (off >> 9) + 1 + ((tlut >> 10 & 3) << 16) + (entries << 18);
            if (g_sent_tlut[m].id != tlut_id || g_sent_tlut[m].version != g_tlut_gen) {
                g_sent_tlut[m] = {tlut_id, g_tlut_gen};
                auto copy = std::make_unique<uint8_t[]>(entries * 2);
                memset(copy.get(), 0, entries * 2);
                memcpy(copy.get(), g_tmem_tlut + off, std::min<uint32_t>(entries * 2, sizeof(g_tmem_tlut) - off));
                Out().cmd(GX_AURORA_LOAD_TLUT).u8(m).u64((uint64_t)copy.get()).u32((tlut >> 10) & 3).u16(entries).u32(tlut_id).u32(g_tlut_gen);
                g_frame_tluts.push_back(std::move(copy));
                g_tluts++;
            }
        }
        const bool mips = (mode0 >> 5) & 3;   // a mipmapping min filter
        Out()
            .cmd(GX_AURORA_LOAD_TEXOBJ)
            .u8(m)
            .u64((uint64_t)phys_ptr((img3 & 0x1FFFFF) << 5))
            .u32((img0 & 0x3FF) + 1)
            .u32(((img0 >> 10) & 0x3FF) + 1)
            .u32(fmt)
            .u32(is_palette(fmt) ? m : 0)
            .u8(mips)
            .u32(tex_id(img3, img0, mips))
            .u32(g_frames_submitted.load() + 1);
    }
}

// The copy's format, as GXTexFmt, from the copy trigger (Dolphin's EFBCopyFormat decoding):
// from the colour buffer, intensity or the raw channels; from depth, the Z formats.
uint32_t copy_format(uint32_t trigger, bool depth) {
    const uint32_t f = ((trigger >> 4) & 7) | ((trigger >> 3) & 1) << 3;
    if (depth) {
        static const uint32_t z[16] = {0x30, 0x11, 0x11, 0x13, 0x13, 0x13, 0x16, 0x16, 0x11, 0x39, 0x3A, 0x13, 0x3C, 0x13, 0x13, 0x13};
        return z[f];
    }
    // The intensity bit turns the red formats -- R4, R8, RA4, RA8 -- into intensity (I4, I8,
    // IA4, IA8); the others ignore it.
    if ((trigger >> 15) & 1) {
        switch (f) {
        case 0: return 0x0;
        case 1: case 8: return 0x1;
        case 2: return 0x2;
        case 3: return 0x3;
        }
    }
    static const uint32_t c[16] = {0x20, 0x28, 0x22, 0x23, 0x4, 0x5, 0x6, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x6, 0x6, 0x6};
    return c[f];
}

void drain() {
    const auto t = Clock::now();
    aurora::gx::fifo::drain();
    g_drains++;
    g_drain_ms += ms_since(t);
}

void end_frame() {
    const auto t = Clock::now();
    const double guest = ms_since(g_frame_start);
    g_frames_submitted++;
    debug_guest_check("frame");
    heap_trace_frame(g_frames_submitted.load());
    aurora_end_frame();
    const double end_ms = ms_since(t);
    g_frame_tluts.clear();   // end_frame has processed everything
    aurora_dump_frame(g_frames_submitted.load());
    while (!aurora_begin_frame()) std::this_thread::sleep_for(std::chrono::milliseconds(1));   // minimised: wait
    g_tex_dirty = 0xFF;      // a new frame's recording: describe the textures again
    g_tlut_gen++;
    if (g_frametime)
        fprintf(stderr, "[ft] f%u %7.2fms  draws %u vtx %uKB  drains %u %.2fms  copies %u dls %u indx %u tluts %u  end+begin %.2fms  pipelines %u queued %u  last frame: vert %uK unif %uK idx %uK stor %uK\n",
                g_frames_submitted.load(), guest, g_draws, g_vtx_bytes / 1024, g_drains, g_drain_ms, g_copies, g_dls, g_indx, g_tluts, ms_since(t),
                aurora_get_stats()->createdPipelines, aurora_get_stats()->queuedPipelines, aurora_get_stats()->lastVertSize / 1024,
                aurora_get_stats()->lastUniformSize / 1024, aurora_get_stats()->lastIndexSize / 1024, aurora_get_stats()->lastStorageSize / 1024);
    (void)end_ms;
    g_draws = g_vtx_bytes = g_drains = g_copies = g_dls = g_indx = g_tluts = g_arr_sends = g_arr_bytes = g_arr_max = 0;
    g_drain_ms = g_tex_ms = g_arr_ms = 0;
    g_frame_start = Clock::now();
}

void load_bp(uint32_t w) {
    const uint32_t reg = w >> 24;
    uint32_t v = w & 0xFFFFFF;
    if (reg == 0xFE) { g_bp_mask = v; return; }
    if (g_bp_mask != 0xFFFFFF) {
        v = (g_bp[reg] & ~g_bp_mask) | (v & g_bp_mask);
        g_bp_mask = 0xFFFFFF;
    }
    g_bp[reg] = v;
    if ((reg >= 0x80 && reg <= 0x9B) || (reg >= 0xA0 && reg <= 0xBB)) g_tex_dirty |= 1u << ((reg & 3) + (reg >= 0xA0 ? 4 : 0));
    switch (reg) {
    case 0x00:   // gen mode. Culling both faces, which Aurora does not take, draws no triangle.
        g_cull_all = ((v >> 14) & 3) == 3;
        if (g_cull_all) v &= ~(3u << 14);
        break;
    case 0x45:   // draw done
        drain();
        if (v & 2) pe_signal_finish();
        return;
    case 0x47: drain(); pe_signal_token((uint16_t)v, false); return;
    case 0x48: drain(); pe_signal_token((uint16_t)v, true); return;
    case 0x64: return;
    case 0x65: {   // a palette into TMEM
        const uint32_t src = (g_bp[0x64] & 0x1FFFFF) << 5, tmem = (v & 0x3FF) << 9, bytes = ((v >> 10) & 0x7FF) * 32;
        if (tmem < sizeof(g_tmem_tlut)) memcpy(g_tmem_tlut + tmem, phys_ptr(src), dma_fit("TLUT", src, std::min<uint32_t>(bytes, sizeof(g_tmem_tlut) - tmem)));
        g_tex_dirty = 0xFF;   // the palettes of textures bound now may have changed
        g_tlut_gen++;
        return;
    }
    case 0x52: {   // EFB copy
        if ((v >> 14) & 1) {   // to the external framebuffer: the end of a frame
            end_frame();
            return;
        }
        const uint32_t x = g_bp[0x49] & 0x3FF, y = (g_bp[0x49] >> 10) & 0x3FF;
        const uint32_t sw = (g_bp[0x4A] & 0x3FF) + 1, sh = ((g_bp[0x4A] >> 10) & 0x3FF) + 1;
        const bool half = (v >> 9) & 1;
        g_copies++;
        if (g_frametime) fprintf(stderr, "[copy] f%u after draw %u: %ux%u@%u,%u half %d fmt %X depth %d clear %d cmode0 %06X dest %06X\n", g_frames_submitted.load(), g_draws, sw, sh, x, y, half,
                                 copy_format(v, (g_bp[0x43] & 7) == 3), (g_bp[0x43] & 7) == 3, (v >> 11) & 1, g_bp[0x41], (g_bp[0x4B] & 0x1FFFFF) << 5);
        const bool depth = (g_bp[0x43] & 7) == 3;
        Out().cmd(GX_AURORA_LOAD_COPY_SRC).u32(x).u32(y).u32(sw).u32(sh);
        Out().cmd(GX_AURORA_LOAD_COPY_DST).u32(half ? sw / 2 : sw).u32(half ? sh / 2 : sh).u32(copy_format(v, depth)).u8(0);
        Out().cmd(GX_AURORA_LOAD_COPY_DEST).u64((uint64_t)phys_ptr((g_bp[0x4B] & 0x1FFFFF) << 5));
        break;
    }
    }
    Out().u8(GX_LOAD_BP_REG).u32(reg << 24 | v);
}

void load_cp(uint8_t reg, uint32_t v) {
    g_cp[reg] = v;
    if (reg >= 0xA0 && reg <= 0xAF) {   // array base: a pointer, for Aurora (load_arrays)
        if (reg >= 0xAC) send_array(reg - 0xA0, v & 0x01FFFFFF, ram_left(v & 0x01FFFFFF));
        return;
    }
    Out().u8(GX_LOAD_CP_REG).u8(reg).u32(v);
}

uint32_t parse(const uint8_t* data, uint32_t len, bool partial_ok) {
    uint32_t pos = 0;
    while (pos < len) {
        const uint8_t* p = data + pos;
        const uint32_t avail = len - pos;
        const uint8_t cmd = p[0];
        uint32_t need;
        switch (cmd) {
        case 0x00: case 0x44: pos += 1; continue;   // NOP, and a no-op the SDK sends
        case 0x48: pass(p, 1); pos += 1; continue;  // invalidate vertex cache
        case 0x08:                                  // LOAD_CP_REG
            if (avail < 6) goto partial;
            load_cp(p[1], be32(p + 2));
            pos += 6;
            continue;
        case 0x10: {                                // LOAD_XF_REG
            if (avail < 5) goto partial;
            need = 5 + 4 * (((be32(p + 1) >> 16) & 0xF) + 1);
            if (avail < need) goto partial;
            pass(p, need);
            pos += need;
            continue;
        }
        case 0x20: case 0x28: case 0x30: case 0x38:  // LOAD_INDX_A..D
            if (avail < 5) goto partial;
            g_indx++;
            pass(p, 5);
            pos += 5;
            continue;
        case 0x40: {                                 // CALL_DL
            if (avail < 9) goto partial;
            const uint32_t addr = be32(p + 1) & 0x03FFFFFF, size = be32(p + 5);
            g_dls++;
            parse(phys_ptr(addr), dma_fit("display list", addr, size), false);
            pos += 9;
            continue;
        }
        case 0x61:                                   // LOAD_BP_REG
            if (avail < 5) goto partial;
            load_bp(be32(p + 1));
            pos += 5;
            continue;
        default:
            if (cmd & 0x80) {                        // a draw
                if (avail < 3) goto partial;
                const Layout L = layout(cmd & 7);
                need = 3 + L.size * be16(p + 1);
                if (avail < need) goto partial;
                {
                    const auto t = Clock::now();
                    // A draw that draws nothing the hardware skips: strips of zero vertices, which
                // Aurora would count as n - 2 triangles, unsigned; triangles while both faces
                // are culled.
                const uint16_t n = be16(p + 1);
                const uint8_t prim = cmd & 0xF8;
                if (n == 0 || ((prim == 0x98 || prim == 0xA0) && n < 3) || (g_cull_all && prim < 0xA8)) {
                    pos += need;
                    continue;
                }
                load_arrays(L, p + 3, be16(p + 1));
                    const auto t2 = Clock::now();
                    g_arr_ms += std::chrono::duration<double, std::milli>(t2 - t).count();
                    load_textures();
                    g_tex_ms += ms_since(t2);
                }
                pass(p, need);
                g_draws++;
                g_vtx_bytes += need;
                static const uint32_t drawlog = getenv("GCN_DRAWLOG") ? (uint32_t)atoi(getenv("GCN_DRAWLOG")) : ~0u;
                if (g_frames_submitted.load() >= drawlog)
                    fprintf(stderr, "[draw] f%u %02X x%u vsz %u vcd %08X %08X vat %08X %08X %08X tex0 %06X img0 %06X mode0 %06X tev %06X ind %06X tex1 %06X img1 %06X tex2 %06X img2 %06X blend %06X zmode %06X\n",
                            g_frames_submitted.load(), cmd, be16(p + 1), vertex_size_of(cmd & 7), g_cp[0x50], g_cp[0x60], g_cp[0x70 + (cmd & 7)],
                            g_cp[0x80 + (cmd & 7)], g_cp[0x90 + (cmd & 7)], (g_bp[0x94] & 0x1FFFFF) << 5, g_bp[0x88], g_bp[0x80], g_bp[0x00], g_bp[0x10],
                            (g_bp[0x95] & 0x1FFFFF) << 5, g_bp[0x89], (g_bp[0x96] & 0x1FFFFF) << 5, g_bp[0x8A], g_bp[0x41], g_bp[0x40]);
                if (g_frametime && g_draws % 5000 == 0)
                    fprintf(stderr, "[ft] f%u still drawing: %u draws, %u KB, last %02X x%u\n", g_frames_submitted.load(), g_draws,
                            g_vtx_bytes / 1024, cmd, be16(p + 1));
                aurora::gx::fifo::publish();
                // GCN_PEEK=<frame>,<draw>[,<draw>...]: the EFB as it stands after those draws of
                // that frame, presented and dumped as peek_<draw>.png (the rest of that frame is
                // then drawn into a cleared EFB). To see what a pass of the game draws.
                static const std::vector<uint32_t> peeks = [] {
                    std::vector<uint32_t> v;
                    if (const char* e = getenv("GCN_PEEK"))
                        for (const char* q = e; q && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : nullptr) v.push_back((uint32_t)atoi(q));
                    return v;
                }();
                if (peeks.size() > 1 && g_frames_submitted.load() == peeks[0])
                    for (size_t k = 1; k < peeks.size(); k++)
                        if (g_draws == peeks[k]) {
                            aurora_end_frame();
                            char name[32];
                            snprintf(name, sizeof(name), "peek_%u", peeks[k]);
                            aurora_dump_named(name);
                            while (!aurora_begin_frame()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            g_tex_dirty = 0xFF;
                            std::fill(std::begin(g_sent), std::end(g_sent), Sent{});
                        }
                pos += need;
                continue;
            }
            LOG(LOG_GX, "unknown GX opcode %02X at +%u of %u", cmd, pos, len);
            pos += 1;
            continue;
        }
    partial:
        if (!partial_ok) LOG(LOG_GX, "truncated command %02X in display list", cmd);
        return partial_ok ? pos : len;
    }
    return pos;
}

}  // namespace

uint32_t process(const uint8_t* data, uint32_t len, bool partial_ok) { return parse(data, len, partial_ok); }
uint32_t vertex_size(int vat) { return vertex_size_of(vat); }

// The first frame's recording, before the game issues anything.
void aurora_bridge_start() {
    // A draw waits for its pipeline, as on the console: no geometry missing from a frame the
    // first time a shader is needed (it is skipped otherwise, until compiled).
    aurora_set_pipeline_blocking(true);
    while (!aurora_begin_frame()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

}  // namespace gx

uint32_t gx_frames_submitted() { return gx::g_frames_submitted.load(); }
