// GX on Aurora (github.com/encounter/aurora, MIT): the game's command stream, as the
// hardware's FIFO carries it, translated for Aurora's command processor, which draws it with
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
//   - display lists (CALL_DL): read here, command by command, from guest memory;
//   - indexed XF loads (LOAD_INDX_A-D): the matrices read here and sent as direct loads.
//
// And it leaves out what the hardware would not draw and Aurora would mishandle: strips of
// no triangles, and triangles while both faces are culled.
//
// At 16:9 (g_widescreen) it also widens the frame, as docs/graphics.md § Widescreen says:
// Aurora's frame is 854 logical pixels wide (VIConfigure, main.cpp), and every x the game
// gives in its 640 -- viewport, scissor, EFB copy -- is mapped into it here: stretched when
// it spans the whole width, moved into the middle 4:3 when narrower. An orthographic draw
// through a full-width viewport that does not reach both edges (the HUD, text) has its
// projection squeezed back to 4:3. At 4:3 none of this changes a byte.
//
// The result goes to the GPU side in chunks (aurora_bridge.h); this file never calls Aurora,
// so the benchmark can take the same stream with no GPU at all. The copy to the external
// framebuffer (BP 0x52 with the display bit) ends a frame.
#include "aurora_bridge.h"
#include "../runtime.h"
#include "gx.h"
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXCommandList.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>

void pe_signal_token(uint16_t token, bool interrupt);
void pe_signal_finish();

namespace gx {

std::atomic<uint32_t> g_frames_submitted{0};
std::atomic<bool> g_hold_frames{false};
State g_state;   // the BP and CP registers as last written (gx.h); XF goes to Aurora only

namespace {

uint32_t (&g_bp)[256] = g_state.bp;
uint32_t (&g_cp)[256] = g_state.cp;
uint8_t g_tmem_tlut[0x80000];   // the upper half of TMEM, where palettes go (tmem 0x80000+)
uint32_t g_tex_dirty = 0xFF;    // texture maps whose registers changed since their LOAD_TEXOBJ
bool g_cull_all;                // gen mode culls both faces: no triangle is drawn
uint32_t g_tev_rk[2][8];        // BP 0xE0-0xE7 by bit 23: [0] colour registers, [1] konst (same addresses)

inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// ---- The stream, and its hand-over to the GPU side ----
Chunk g_cur;
std::mutex g_q_mutex;
std::condition_variable g_q_cv;
std::deque<Chunk> g_queue;
uint64_t g_next_seq = 1, g_done_seq = 0;
uint32_t g_frames_queued;           // frame ends handed over and not yet done
std::deque<uint64_t> g_frame_seqs;  // their chunks' seq, oldest first

bool g_hold;   // g_hold_frames, as it was when this frame began

uint64_t hand_over(bool frame_end) {
    std::lock_guard<std::mutex> lk(g_q_mutex);
    g_cur.seq = g_next_seq++;
    g_cur.frame_end = frame_end;
    g_cur.held = g_hold;
    if (frame_end) {
        g_frames_queued++;
        g_frame_seqs.push_back(g_cur.seq);
    }
    const uint64_t seq = g_cur.seq;
    g_queue.push_back(std::move(g_cur));
    g_cur = Chunk{};
    g_q_cv.notify_all();
    return seq;
}

// Until the GPU side has processed chunk `seq`; or, if nothing takes chunks at all, a while.
void wait_done(uint64_t seq) {
    std::unique_lock<std::mutex> lk(g_q_mutex);
    if (!g_q_cv.wait_for(lk, std::chrono::seconds(2), [&] { return g_done_seq >= seq; }))
        LOG(LOG_GX, "nothing processed the GPU stream for 2 s; going on");
}

void put(const void* p, uint32_t n) {
    const auto* b = (const uint8_t*)p;
    g_cur.bytes.insert(g_cur.bytes.end(), b, b + n);
    if (g_cur.bytes.size() >= (256u << 10)) hand_over(false);
}

struct Out {   // one of Aurora's commands, big-endian like the hardware's
    uint8_t buf[64];
    uint32_t n = 0;
    Out& u8(uint32_t v) { buf[n++] = (uint8_t)v; return *this; }
    Out& u16(uint32_t v) { return u8(v >> 8).u8(v); }
    Out& u32(uint32_t v) { return u16(v >> 16).u16(v); }
    Out& u64(uint64_t v) { return u32((uint32_t)(v >> 32)).u32((uint32_t)v); }
    Out& cmd(uint32_t sub) { return u8(GX_AURORA).u16(sub); }
    ~Out() { put(buf, n); }
};

// GCN_FRAMETIME=1: one line per frame, what the guest sent and how long it waited on the GPU.
const bool g_frametime = getenv("GCN_FRAMETIME") != nullptr;
uint32_t g_draws, g_vtx_bytes, g_copies;
double g_wait_ms;
using Clock = std::chrono::steady_clock;
Clock::time_point g_frame_start = Clock::now();
double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// ---- Vertices: how many bytes a draw's vertices take, from the VCD/VAT (as the hardware) ----
const uint32_t kCompSize[8] = {1, 1, 2, 2, 4, 0, 0, 0};
const uint32_t kColorSize[8] = {2, 3, 4, 2, 3, 4, 0, 0};

// A vertex of format `vat`: its size, and where its indexed attributes' indices sit.
struct Index { uint8_t array, off, bytes; };
struct Layout {
    uint32_t size = 0;
    int n = 0;
    Index idx[16];
    int pos_off = -1;     // where the position sits in a vertex (direct, or its index)
    uint32_t pos_desc = 0;
};

Layout layout(int vat) {
    const uint32_t lo = g_cp[0x50], hi = g_cp[0x60], a = g_cp[0x70 + vat], b = g_cp[0x80 + vat], c = g_cp[0x90 + vat];
    Layout L;
    auto attr = [&](int array, uint32_t desc, uint32_t direct, uint32_t indices) {
        if (desc == 1) L.size += direct;
        else if (desc >= 2)
            for (uint32_t k = 0; k < indices; k++) L.idx[L.n++] = {(uint8_t)array, (uint8_t)L.size, (uint8_t)(desc - 1)}, L.size += desc - 1;
    };
    for (int i = 0; i < 9; i++) L.size += (lo >> i) & 1;   // position/normal matrix index, 8 texture matrix indices
    L.pos_off = (int)L.size;
    L.pos_desc = (lo >> 9) & 3;
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

// ---- Vertex arrays ----
// Aurora copies a vertex array whole into the frame's storage buffer (8 MB) the first time a
// draw uses it, so it must be told how long the array is -- which the hardware never knows:
// it reads where the indices point. Here an array's length is how far the draws that use it
// have indexed into it so far (per base address, kept from frame to frame), given to Aurora
// before the draw that needs it; it grows by doubling, since each growth is a fresh copy of
// the whole array (growing to each draw's furthest index was 146 copies of 3 arrays in one
// frame). The matrix arrays (12-15) are only read by indexed XF loads, which are sent as
// direct ones (xf_indexed): Aurora is not told of them.
//
// A frame the GPU side holds (g_hold) points at copies of its arrays instead, made once per
// stretch between two waits on the GPU, after which the game may have rewritten them.
std::unordered_map<uint32_t, uint32_t> g_extent;
struct Sent { const uint8_t* ptr = nullptr; uint32_t size = 0; } g_sent[16];
std::unordered_map<uint32_t, std::pair<const uint8_t*, uint32_t>> g_copied;

uint32_t ram_left(uint32_t pa) { return pa < 0x01800000 ? 0x01800000 - pa : 0; }

const uint8_t* array_data(uint32_t base, uint32_t size) {
    if (!g_hold) return phys_ptr(base);
    auto& c = g_copied[base];
    if (c.first && c.second >= size) return c.first;
    g_cur.keep.emplace_back(new uint8_t[size]);
    memcpy(g_cur.keep.back().get(), phys_ptr(base), size);
    c = {g_cur.keep.back().get(), size};
    return c.first;
}

void send_array(int a, uint32_t base, uint32_t size) {
    const uint8_t* ptr = array_data(base, size);
    if (g_sent[a].ptr == ptr && g_sent[a].size == size) return;
    g_sent[a] = {ptr, size};
    Out().cmd(GX_AURORA_LOAD_ARRAYBASE + a).u64((uint64_t)ptr).u32(size).u8(0);
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
// so an id of 0 -- hash every time -- cost a full hash per draw. Here a texture's id stands
// for its address, size and format, and its version is the frame: each texture is read
// again once per frame, which is what the OpenGL renderer did. A palette's version moves
// with every palette load, and each frame; each palette sent is a copy of TMEM as it stood,
// since Aurora reads it after the next load may have overwritten it.
std::unordered_map<uint64_t, uint32_t> g_tex_ids;
uint32_t g_tlut_gen = 1;
struct SentTlut { uint32_t id = 0, version = 0; } g_sent_tlut[8];

uint32_t tex_id(uint32_t img3, uint32_t img0, bool mips) {
    const uint64_t key = (uint64_t)(img3 & 0x1FFFFF) << 32 | img0 << 1 | mips;
    return g_tex_ids.try_emplace(key, (uint32_t)g_tex_ids.size() + 1).first->second;
}

void load_textures() {
    for (int m = 0; g_tex_dirty && m < 8; m++) {
        if (!(g_tex_dirty & (1u << m))) continue;
        g_tex_dirty &= ~(1u << m);
        const uint32_t base = m < 4 ? 0x80 + m : 0xA0 + (m - 4);
        const uint32_t mode0 = g_bp[base], img0 = g_bp[base + 8], img3 = g_bp[base + 0x14], tlut = g_bp[base + 0x18];
        if (!img3) continue;
        const uint32_t fmt = (img0 >> 20) & 0xF;
        if (is_palette(fmt)) {
            const uint32_t off = std::min<uint32_t>((tlut & 0x3FF) << 9, sizeof(g_tmem_tlut));
            const uint32_t entries = fmt == 8 ? 16 : fmt == 9 ? 256 : 16384;
            const uint32_t id = (off >> 9) + 1 + ((tlut >> 10 & 3) << 16) + (entries << 18);
            if (g_sent_tlut[m].id != id || g_sent_tlut[m].version != g_tlut_gen) {
                g_sent_tlut[m] = {id, g_tlut_gen};
                auto copy = std::make_unique<uint8_t[]>(entries * 2);
                memset(copy.get(), 0, entries * 2);
                memcpy(copy.get(), g_tmem_tlut + off, std::min<uint32_t>(entries * 2, sizeof(g_tmem_tlut) - off));
                Out().cmd(GX_AURORA_LOAD_TLUT).u8(m).u64((uint64_t)copy.get()).u32((tlut >> 10) & 3).u16(entries).u32(id).u32(g_tlut_gen);
                g_cur.keep.push_back(std::move(copy));
            }
        }
        const bool mips = (mode0 >> 5) & 3;   // a mipmapping min filter
        static const uint32_t log_frame = getenv("GCN_DRAW_LOG_FRAME") ? (uint32_t)atoi(getenv("GCN_DRAW_LOG_FRAME")) : ~0u;
        if (g_frames_submitted.load() == log_frame)
            fprintf(stderr, "[tex] f%u draw %u  map %d  addr %08X  %ux%u  fmt %X  mode0 %06X\n", log_frame, g_draws, m,
                    (img3 & 0x1FFFFF) << 5, (img0 & 0x3FF) + 1, ((img0 >> 10) & 0x3FF) + 1, fmt, mode0);
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
// from the colour buffer, the raw channels -- or, with the intensity bit, intensity for the
// red formats (R4, R8, RA4, RA8); from depth, the Z formats.
uint32_t copy_format(uint32_t trigger, bool depth) {
    const uint32_t f = ((trigger >> 4) & 7) | ((trigger >> 3) & 1) << 3;
    if (depth) {
        static const uint32_t z[16] = {0x30, 0x11, 0x11, 0x13, 0x13, 0x13, 0x16, 0x16, 0x11, 0x39, 0x3A, 0x13, 0x3C, 0x13, 0x13, 0x13};
        return z[f];
    }
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

// Where the game waits on the GPU: everything so far is processed first, as on the hardware.
void sync() {
    const auto t = Clock::now();
    wait_done(hand_over(false));
    g_wait_ms += ms_since(t);
    g_copied.clear();   // the game may now rewrite what was copied
}

void set_wide(float wide);   // 16:9, below

void end_frame() {
    if (g_frametime)
        fprintf(stderr, "[ft] f%u %7.2fms  draws %u  vtx %uKB  copies %u  waited on the GPU %.2fms\n", g_frames_submitted.load(),
                ms_since(g_frame_start), g_draws, g_vtx_bytes / 1024, g_copies, g_wait_ms);
    g_draws = g_vtx_bytes = g_copies = 0;
    g_wait_ms = 0;
    g_frames_submitted++;
    debug_guest_check("frame");
    heap_trace_frame(g_frames_submitted.load());
    hand_over(true);
    // At most one frame ahead of the GPU side, as the hardware's FIFO would hold it.
    {
        std::unique_lock<std::mutex> lk(g_q_mutex);
        if (!g_q_cv.wait_for(lk, std::chrono::seconds(2), [] { return g_frames_queued <= 1; }))
            LOG(LOG_GX, "nothing presented a frame for 2 s; going on");
    }
    g_tex_dirty = 0xFF;   // describe the textures again: each is read once per frame
    g_tlut_gen++;
    g_frame_start = Clock::now();
    // Each frame sends its arrays again, so it can be replayed on its own.
    for (Sent& a : g_sent) a = {};
    g_copied.clear();
    g_hold = g_hold_frames.load();
    set_wide(g_widescreen ? 4.0f / 3.0f : 1.0f);
}

// ---- 16:9 ----
float g_wide = 1.0f;     // the frame's width over 4:3's: 4/3 at 16:9, else 1; set between frames
uint32_t g_xf[0x1058];   // XF memory and registers as the game wrote them
bool g_squeezed;         // Aurora has the game's projection squeezed to 4:3 (draw_in_43)

float xf_f(uint32_t a) { float f; memcpy(&f, &g_xf[a], 4); return f; }
uint32_t f_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
int x_offset() { return (int)(g_bp[0x59] & 0x3FF) * 2; }   // the scissor offset, in pixels
// An x in the game's 640 into the 854: stretched, or kept 4:3 in the middle.
float wide_x(float x, bool in43) { return in43 ? x + 320.0f * (g_wide - 1.0f) : x * g_wide; }

void xf_out(uint32_t addr, const uint32_t* v, uint32_t n) {
    g_cur.marks.push_back({(uint32_t)g_cur.bytes.size(), 0});
    Out o;
    o.u8(0x10).u32((n - 1) << 16 | addr);
    for (uint32_t i = 0; i < n; i++) o.u32(v[i]);
}

bool viewport_partial() { return 2.0f * xf_f(0x101A) < 640.0f - 0.5f; }

// The game's viewport, as Aurora must see it.
void viewport_out() {
    const bool in43 = viewport_partial();
    uint32_t v[6];
    memcpy(v, &g_xf[0x101A], sizeof(v));
    v[0] = f_bits(in43 ? xf_f(0x101A) : xf_f(0x101A) * g_wide);
    v[3] = f_bits(wide_x(xf_f(0x101D) - (float)x_offset(), in43) + (float)x_offset());
    xf_out(0x101A, v, 6);
}

void projection_out(bool squeeze) {
    uint32_t v[7];
    memcpy(v, &g_xf[0x1020], sizeof(v));
    if (squeeze) v[0] = f_bits(xf_f(0x1020) / g_wide), v[1] = f_bits(xf_f(0x1021) / g_wide);
    xf_out(0x1020, v, 7);
    g_squeezed = squeeze;
}

// The scissor rect, BP 0x20/0x21, x in bits 12-22, both written whichever changed.
void scissor_out() {
    const int off = x_offset();
    const int x0 = (int)(g_bp[0x20] >> 12 & 0x7FF) - off, x1 = (int)(g_bp[0x21] >> 12 & 0x7FF) - off;
    const bool in43 = x0 > 0 || x1 + 1 < 640;
    const int n0 = (int)lroundf(wide_x((float)x0, in43)), n1 = (int)lroundf(wide_x((float)(x1 + 1), in43)) - 1;
    for (uint32_t r : {0x20u, 0x21u}) {
        const int x = (r == 0x20 ? n0 : n1) + off;
        Out().u8(GX_LOAD_BP_REG).u32(r << 24 | (g_bp[r] & ~(0x7FFu << 12)) | (uint32_t)(x & 0x7FF) << 12);
    }
}

// What the game wrote to XF: kept, and at 16:9 the viewport is sent again, mapped.
void xf_written(uint32_t addr, const uint8_t* data, uint32_t n) {
    for (uint32_t i = 0; i < n && addr + i < 0x1058; i++) g_xf[addr + i] = be32(data + 4 * i);
    if (addr <= 0x1026 && addr + n > 0x1020) g_squeezed = false;
    if (g_wide != 1.0f && addr <= 0x101F && addr + n > 0x101A) viewport_out();
}

// An indexed XF load (LOAD_INDX_A-D): matrices from the arrays 12-15 in RAM, read now -- as
// the hardware would, at this point of the stream -- and sent as a direct load, so that the
// stream holds the values and can be replayed (or interpolated) later.
void xf_indexed(uint8_t cmd, uint32_t w) {
    const int array = 12 + ((cmd >> 3) & 3);
    const uint32_t index = w >> 16, n = ((w >> 12) & 0xF) + 1, addr = w & 0xFFF;
    const uint32_t src = (g_cp[0xA0 + array] & 0x01FFFFFF) + index * (g_cp[0xB0 + array] & 0xFF);
    const uint8_t* p = phys_ptr(src);
    uint32_t v[16];
    for (uint32_t i = 0; i < n; i++) v[i] = be32(p + 4 * i);
    for (uint32_t i = 0; i < n && addr + i < 0x1058; i++) g_xf[addr + i] = v[i];
    xf_out(addr, v, n);
}

// A position component of type `t` (u8 s8 u16 s16 f32) with `frac` fraction bits.
float pos_comp(const uint8_t* p, uint32_t t, uint32_t frac) {
    const float k = 1.0f / (float)(1u << frac);
    switch (t) {
    case 0: return p[0] * k;
    case 1: return (int8_t)p[0] * k;
    case 2: return be16(p) * k;
    case 3: return (int16_t)be16(p) * k;
    default: { const uint32_t u = be32(p); float f; memcpy(&f, &u, 4); return f; }
    }
}

// An orthographic draw whose vertices do not reach both edges of the frame (in clip space,
// x = p0 * x + p1 under GX's ortho, x in view space) is a 2D element, not a backdrop.
bool draw_in_43(const Layout& L, int vat, const uint8_t* v, uint32_t count) {
    if (g_xf[0x1026] == 0 || L.pos_desc == 0) return false;
    const uint32_t a = g_cp[0x70 + vat], t = (a >> 1) & 7, frac = (a >> 4) & 0x1F;
    const uint32_t comps = (a & 1) ? 3 : 2, csize = kCompSize[t] ? kCompSize[t] : 4;
    const bool per_vertex_mtx = g_cp[0x50] & 1;
    const float p0 = xf_f(0x1020), p1 = xf_f(0x1021);
    float lo = 1e30f, hi = -1e30f;
    for (uint32_t i = 0; i < count; i++, v += L.size) {
        const uint8_t* q = v + L.pos_off;
        if (L.pos_desc >= 2) {
            const uint32_t at = L.pos_desc == 2 ? q[0] : be16(q);
            q = phys_ptr((g_cp[0xA0] & 0x01FFFFFF) + at * (g_cp[0xB0] & 0xFF));
        }
        const float x = pos_comp(q, t, frac), y = pos_comp(q + csize, t, frac), z = comps == 3 ? pos_comp(q + 2 * csize, t, frac) : 0.0f;
        const uint32_t m = (per_vertex_mtx ? v[0] : g_cp[0x30]) & 0x3F;   // its matrix, a row of 4 per index
        const float vx = xf_f(m * 4) * x + xf_f(m * 4 + 1) * y + xf_f(m * 4 + 2) * z + xf_f(m * 4 + 3);
        const float cx = p0 * vx + p1;
        lo = std::min(lo, cx);
        hi = std::max(hi, cx);
    }
    return lo > -0.98f || hi < 0.98f;
}

// What a draw draws, as far as the stream tells from one frame to the next: its primitive,
// format and size, and the indices of its first and last vertices (a mesh's do not change
// as it moves; positions given directly would).
uint32_t draw_sig(const Layout& L, const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t v) { h = (h ^ v) * 16777619u; };
    mix(p[0]);
    mix(n);
    for (const uint8_t* v : {p + 3, p + 3 + (n - 1) * L.size})
        for (int k = 0; k < L.n; k++) mix(L.idx[k].bytes == 1 ? v[L.idx[k].off] : be16(v + L.idx[k].off));
    return h | 1;
}

// GCN_DRAW_LOG: what decides how a draw looks, as the game set it.
void log_draw_state(uint8_t cmd, uint32_t n) {
    fprintf(stderr, "[draw] f%u #%u cmd %02X n %u  genmode %06X  zmode %06X  blend %06X  alphacmp %06X  zcomp %06X\n",
            g_frames_submitted.load(), g_draws, cmd, n, g_bp[0x00], g_bp[0x40], g_bp[0x41], g_bp[0xF3], g_bp[0x43]);
    const uint32_t stages = ((g_bp[0x00] >> 10) & 0xF) + 1, gens = g_bp[0x00] & 0xF;
    for (uint32_t i = 0; i < stages; i++)
        fprintf(stderr, "  tev%u  color %06X alpha %06X  order %06X  ksel %06X\n", i, g_bp[0xC0 + 2 * i], g_bp[0xC1 + 2 * i],
                g_bp[0x28 + i / 2], g_bp[0xF6 + i / 2]);
    for (uint32_t i = 0; i < gens; i++) fprintf(stderr, "  texgen%u %08X post %08X\n", i, g_xf[0x1040 + i], g_xf[0x1050 + i]);
    fprintf(stderr, "  chan ctrl c0 %08X c1 %08X a0 %08X a1 %08X  mat %08X %08X amb %08X %08X  numchans %u\n",
            g_xf[0x100E], g_xf[0x100F], g_xf[0x1010], g_xf[0x1011], g_xf[0x100C], g_xf[0x100D], g_xf[0x100A], g_xf[0x100B], g_xf[0x1009]);
    for (int i = 0; i < 4; i++) fprintf(stderr, "  tevreg%d %06X %06X  konst %06X %06X\n", i, g_tev_rk[0][2 * i], g_tev_rk[0][2 * i + 1], g_tev_rk[1][2 * i], g_tev_rk[1][2 * i + 1]);
    fprintf(stderr, "  fog %06X %06X %06X %06X %06X  indirect %06X\n", g_bp[0xEE], g_bp[0xEF], g_bp[0xF0], g_bp[0xF1], g_bp[0xF2], g_bp[0x10]);
}

// A new frame's width: what was mapped for the old one is sent again.
void set_wide(float wide) {
    if (wide == g_wide) return;
    g_wide = wide;
    viewport_out();
    scissor_out();
    projection_out(false);
}

void load_bp(uint32_t w) {
    const uint32_t reg = w >> 24;
    uint32_t v = w & 0xFFFFFF;
    if (reg == 0xFE) { g_state.bp_mask = v; return; }
    if (g_state.bp_mask != 0xFFFFFF) {
        v = (g_bp[reg] & ~g_state.bp_mask) | (v & g_state.bp_mask);
        g_state.bp_mask = 0xFFFFFF;
    }
    g_bp[reg] = v;
    if (reg >= 0xE0 && reg <= 0xE7) g_tev_rk[v >> 23 & 1][reg - 0xE0] = v;
    if ((reg >= 0x80 && reg <= 0x9B) || (reg >= 0xA0 && reg <= 0xBB)) g_tex_dirty |= 1u << ((reg & 3) + (reg >= 0xA0 ? 4 : 0));
    switch (reg) {
    case 0x00:   // gen mode. Culling both faces, which Aurora rejects, draws no triangle.
        g_cull_all = ((v >> 14) & 3) == 3;
        if (g_cull_all) v &= ~(3u << 14);
        break;
    case 0x45:   // draw done
        sync();
        if (v & 2) pe_signal_finish();
        return;
    case 0x47: sync(); pe_signal_token((uint16_t)v, false); return;
    case 0x48: sync(); pe_signal_token((uint16_t)v, true); return;
    case 0x64: return;
    case 0x20: case 0x21: case 0x59:   // scissor, and its offset
        if (g_wide == 1.0f) break;
        Out().u8(GX_LOAD_BP_REG).u32(reg << 24 | v);
        scissor_out();
        viewport_out();
        return;
    case 0x65: {   // a palette into TMEM
        const uint32_t src = (g_bp[0x64] & 0x1FFFFF) << 5, tmem = (v & 0x3FF) << 9, bytes = ((v >> 10) & 0x7FF) * 32;
        if (tmem < sizeof(g_tmem_tlut))
            memcpy(g_tmem_tlut + tmem, phys_ptr(src), dma_fit("TLUT", src, std::min<uint32_t>(bytes, sizeof(g_tmem_tlut) - tmem)));
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
        const bool half = (v >> 9) & 1, depth = (g_bp[0x43] & 7) == 3;
        // At 16:9 the copy of the whole width takes the whole 854, at 854's resolution; a
        // narrower one, its 4:3 place in the middle (where its viewport put what it copies).
        const bool in43 = x > 0 || x + sw < 640;
        const uint32_t wx = (uint32_t)lroundf(wide_x((float)x, in43)), wsw = in43 ? sw : (uint32_t)lroundf(sw * g_wide);
        g_copies++;
        static const uint32_t log_frame = getenv("GCN_DRAW_LOG_FRAME") ? (uint32_t)atoi(getenv("GCN_DRAW_LOG_FRAME")) : ~0u;
        if (g_frames_submitted.load() == log_frame)
            fprintf(stderr, "[copy] f%u after draw %u  src %u,%u %ux%u  trigger %06X  pixfmt %06X  fmt %02X%s  dest %08X  clear %u\n",
                    log_frame, g_draws, x, y, sw, sh, v, g_bp[0x43], copy_format(v, depth), depth ? " (depth)" : "",
                    (g_bp[0x4B] & 0x1FFFFF) << 5, (v >> 11) & 1);
        Out().cmd(GX_AURORA_LOAD_COPY_SRC).u32(wx).u32(y).u32(wsw).u32(sh);
        Out().cmd(GX_AURORA_LOAD_COPY_DST).u32(half ? wsw / 2 : wsw).u32(half ? sh / 2 : sh).u32(copy_format(v, depth)).u8(0);
        Out().cmd(GX_AURORA_LOAD_COPY_DEST).u64((uint64_t)phys_ptr((g_bp[0x4B] & 0x1FFFFF) << 5));
        break;
    }
    }
    Out().u8(GX_LOAD_BP_REG).u32(reg << 24 | v);
}

void load_cp(uint8_t reg, uint32_t v) {
    g_cp[reg] = v;
    if (reg >= 0xA0 && reg <= 0xAF) return;   // array base: a pointer, for Aurora (load_arrays)
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
        case 0x48: put(p, 1); pos += 1; continue;   // invalidate vertex cache
        case 0x08:                                  // LOAD_CP_REG
            if (avail < 6) goto partial;
            load_cp(p[1], be32(p + 2));
            pos += 6;
            continue;
        case 0x10:                                  // LOAD_XF_REG
            if (avail < 5) goto partial;
            need = 5 + 4 * (((be32(p + 1) >> 16) & 0xF) + 1);
            if (avail < need) goto partial;
            g_cur.marks.push_back({(uint32_t)g_cur.bytes.size(), 0});
            put(p, need);
            xf_written(be32(p + 1) & 0xFFFF, p + 5, (need - 5) / 4);
            pos += need;
            continue;
        case 0x20: case 0x28: case 0x30: case 0x38:  // LOAD_INDX_A..D
            if (avail < 5) goto partial;
            xf_indexed(cmd, be32(p + 1));
            pos += 5;
            continue;
        case 0x40: {                                 // CALL_DL
            if (avail < 9) goto partial;
            const uint32_t addr = be32(p + 1) & 0x03FFFFFF, size = be32(p + 5);
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
                const uint16_t n = be16(p + 1);
                const Layout L = layout(cmd & 7);
                need = 3 + L.size * n;
                if (avail < need) goto partial;
                pos += need;
                // What the hardware would not draw: Aurora counts a strip's triangles as
                // n - 2, unsigned, and rejects culling both faces.
                const uint8_t prim = cmd & 0xF8;
                if (n == 0 || ((prim == 0x98 || prim == 0xA0) && n < 3) || (g_cull_all && prim < 0xA8)) continue;
                // GCN_DRAW_SKIP=a-b[,c-d...]: the frame's draws a to b left out, to find which
                // draws make something; !a-b,... keeps only those (with GCN_DRAW_LOG=<n>: draw
                // n's state on stderr).
                static std::vector<std::pair<uint32_t, uint32_t>> skip_ranges;
                static bool skip_keep;
                static const uint32_t log_draw = getenv("GCN_DRAW_LOG") ? (uint32_t)atoi(getenv("GCN_DRAW_LOG")) : ~0u;
                static const bool skip_parsed = [] {
                    if (const char* r = getenv("GCN_DRAW_SKIP")) {
                        skip_keep = *r == '!';
                        uint32_t lo, hi;
                        int len;
                        for (r += skip_keep; sscanf(r, "%u-%u%n", &lo, &hi, &len) == 2; r += len + (r[len] == ','))
                            skip_ranges.push_back({lo, hi});
                    }
                    return true;
                }();
                (void)skip_parsed;
                static const uint32_t log_frame = getenv("GCN_DRAW_LOG_FRAME") ? (uint32_t)atoi(getenv("GCN_DRAW_LOG_FRAME")) : ~0u;
                static const bool skip_srtg = getenv("GCN_DRAW_SKIP") && !strcmp(getenv("GCN_DRAW_SKIP"), "srtg");
                bool srtg = false;   // a colour texgen (GCN_DRAW_LOG_FRAME, GCN_DRAW_SKIP=srtg)
                for (uint32_t i = 0; i < (g_bp[0x00] & 0xF); i++) srtg |= ((g_xf[0x1040 + i] >> 4) & 7) >= 2;
                if (g_draws == log_draw || (srtg && g_frames_submitted.load() == log_frame)) log_draw_state(cmd, n);
                bool in_range = false;
                for (const auto& [lo, hi] : skip_ranges) in_range |= g_draws >= lo && g_draws <= hi;
                if ((!skip_ranges.empty() && in_range != skip_keep) || (srtg && skip_srtg)) { g_draws++; continue; }
                load_arrays(L, p + 3, n);
                load_textures();
                if (g_wide != 1.0f) {
                    const bool squeeze = !viewport_partial() && draw_in_43(L, cmd & 7, p + 3, n);
                    if (squeeze != g_squeezed) projection_out(squeeze);
                }
                g_cur.marks.push_back({(uint32_t)g_cur.bytes.size(), draw_sig(L, p, n)});
                put(p, need);
                g_draws++;
                g_vtx_bytes += need;
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
uint32_t vertex_size(int vat) { return layout(vat).size; }

bool take_chunk(Chunk& out, int timeout_ms) {
    std::unique_lock<std::mutex> lk(g_q_mutex);
    if (!g_q_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [] { return !g_queue.empty(); })) return false;
    out = std::move(g_queue.front());
    g_queue.pop_front();
    return true;
}

void chunk_done(uint64_t seq) {
    std::lock_guard<std::mutex> lk(g_q_mutex);
    if (seq > g_done_seq) g_done_seq = seq;
    while (!g_frame_seqs.empty() && g_frame_seqs.front() <= g_done_seq) {
        g_frame_seqs.pop_front();
        g_frames_queued--;
    }
    g_q_cv.notify_all();
}

}  // namespace gx

uint32_t gx_frames_submitted() { return gx::g_frames_submitted.load(); }
