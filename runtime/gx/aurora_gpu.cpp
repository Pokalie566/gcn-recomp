// The GPU side of the game's GX stream. See aurora_gpu.h.
#include "aurora_gpu.h"
#include "aurora_bridge.h"
#include "aurora_interp.h"
#include "../platform.h"
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include "gfx/render_worker.hpp"   // Aurora's
#include "gx/fifo.hpp"             // Aurora's
#include "webgpu/gpu.hpp"          // Aurora's
#include "rmlui.hpp"                // Aurora's (lib/rmlui.hpp)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

bool write_png(const char* path, const uint8_t* rgba, int w, int h);

namespace {

bool g_open;                       // an Aurora frame is being recorded
std::vector<gx::Chunk> g_last;     // the last finished frame, for gpu_present_again
std::vector<gx::Chunk> g_partial;  // the frame being received
bool g_replay_partial;             // g_partial was cut short by gpu_present_again: send it again first
uint32_t g_presented;
const char* g_dump_dir;
int g_dump_every;

// 60 fps (aurora_interp.h): a finished frame is held, an in-between frame shown, and the
// held one shown half a frame later.
using Time = std::chrono::steady_clock::time_point;
bool g_interp;                     // the setting
bool g_interp_force;               // GCN_INTERPOLATE=force: an in-between before every frame, no waiting
const bool g_interp_log = getenv("GCN_INTERPLOG") != nullptr;
std::vector<gx::Chunk> g_held;     // the frame being received, then held
bool g_due;                        // g_held is complete: shown at g_due_at
Time g_due_at;
Time g_last_arrival;               // when the last held frame was complete (not when shown)

void begin() {
    while (!aurora_begin_frame()) std::this_thread::sleep_for(std::chrono::milliseconds(1));   // minimised: wait
    g_open = true;
}

void feed(const gx::Chunk& c) { aurora::gx::fifo::write_data(c.bytes.data(), (uint32_t)c.bytes.size()); }

void end() {
    aurora_end_frame();
    g_open = false;
    // GCN_FRAMETIME: also the frames shown, once a second.
    static const bool log = getenv("GCN_FRAMETIME") != nullptr;
    static Time since = std::chrono::steady_clock::now();
    static int shown;
    if (log && ++shown && std::chrono::steady_clock::now() - since >= std::chrono::seconds(1)) {
        fprintf(stderr, "[present] %d frames/s\n", shown);
        shown = 0;
        since = std::chrono::steady_clock::now();
    }
}

void dump_frame(bool between) {
    if (!g_dump_dir || !g_dump_every || (g_presented + between) % g_dump_every) return;
    static uint32_t lo = 0, hi = ~0u;
    static const bool parsed = [] {
        if (const char* r = getenv("GCN_DUMP_RANGE")) sscanf(r, "%u-%u", &lo, &hi);
        return true;
    }();
    (void)parsed;
    const uint32_t n = g_presented + between;
    if (n < lo || n > hi) return;
    char path[512];
    snprintf(path, sizeof(path), between ? "%s/frame_%05u_i.png" : "%s/frame_%05u.png", g_dump_dir, n);
    gpu_dump(path, false);
}

// A frame finished: it becomes the last one.
void finished(std::vector<gx::Chunk>&& frame) {
    g_last = std::move(frame);
    g_presented++;
    dump_frame(false);
}

// The held frame, shown.
void show_held() {
    begin();
    for (const gx::Chunk& c : g_held) feed(c);
    aurora::gx::fifo::drain();
    end();
    gx::chunk_done(g_held.back().seq);   // its end: the guest may finish the next one
    g_due = false;
    finished(std::move(g_held));
    g_held.clear();
}

// The in-between frame for the held one, if it is not a cut. True if shown.
bool show_between() {
    if (g_last.empty()) return false;
    std::vector<std::vector<uint8_t>> bytes;
    gx::InterpStats st;
    const bool ok = gx::interpolate_frame(g_last, g_held, 0.5f, bytes, &st);
    if (g_interp_log)
        fprintf(stderr, "[interp] f%u loads %u matched %u far %u%s\n", g_presented + 1, st.loads, st.matched, st.far, ok ? "" : "  cut");
    if (!ok) return false;
    begin();
    for (const auto& b : bytes) aurora::gx::fifo::write_data(b.data(), (uint32_t)b.size());
    aurora::gx::fifo::drain();
    end();
    dump_frame(true);
    return true;
}

// A held chunk taken. True if a frame was shown.
bool take_held(gx::Chunk&& c) {
    const bool frame_end = c.frame_end;
    if (!frame_end) gx::chunk_done(c.seq);   // it points at nothing the guest may change
    g_held.push_back(std::move(c));
    if (!frame_end) return false;
    // Only a frame that comes about two refreshes after the last one gets an in-between: a
    // backlog, or a scene the game runs at 60, goes straight through.
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - g_last_arrival).count();
    g_last_arrival = now;
    if ((g_interp_force || (ms >= 25 && ms <= 50)) && show_between()) {
        g_due = true;
        g_due_at = g_interp_force ? now : now + std::chrono::microseconds((int64_t)(ms * 500));
        return true;
    }
    show_held();
    return true;
}

}  // namespace

void gpu_init() {
    // A draw waits for its pipeline, as on the console: no geometry is missing from a frame
    // the first time a shader is needed (otherwise it is skipped until compiled).
    aurora_set_pipeline_blocking(true);
    if (const char* e = getenv("GCN_INTERPOLATE")) {
        g_interp_force = !strcmp(e, "force");
        gpu_set_interpolate(g_interp_force || atoi(e) != 0);
    }
}

void gpu_set_interpolate(bool on) {
    if (getenv("GCN_INTERPOLATE")) on = g_interp_force || atoi(getenv("GCN_INTERPOLATE")) != 0;
    g_interp = on;
    gx::g_hold_frames = on;
}

bool gpu_pump(int timeout_ms) {
    if (g_due) {   // nothing more is taken until the held frame is shown
        const auto now = std::chrono::steady_clock::now();
        if (now < g_due_at) {
            std::this_thread::sleep_for(std::min(g_due_at - now, std::chrono::steady_clock::duration(std::chrono::milliseconds(timeout_ms))));
            if (std::chrono::steady_clock::now() < g_due_at) return false;
        }
        show_held();
        return true;
    }
    gx::Chunk c;
    for (int wait = timeout_ms; gx::take_chunk(c, wait); wait = 0) {
        if (c.held && !g_open) {
            if (take_held(std::move(c))) return true;
            continue;
        }
        if (!g_open) {
            begin();
            if (g_replay_partial) {
                for (const gx::Chunk& p : g_partial) feed(p);
                g_replay_partial = false;
            }
        }
        feed(c);
        aurora::gx::fifo::drain();   // processed: the guest may go past it
        gx::chunk_done(c.seq);
        const bool frame_end = c.frame_end;
        g_partial.push_back(std::move(c));
        if (frame_end) {
            end();
            finished(std::move(g_partial));
            g_partial.clear();
            return true;
        }
    }
    return false;
}

void gpu_present_again() {
    if (g_due) show_held();   // the frame held for an in-between is the last one now
    if (g_open) {   // the guest stopped in the middle of a frame: shown as it stands, sent again later
        end();
        g_replay_partial = true;
    }
    begin();
    for (const gx::Chunk& c : g_last) feed(c);
    end();
}

void gpu_set_dump(const char* dir, int every) {
    g_dump_dir = dir;
    g_dump_every = every;
    if (dir) plat_make_dirs(dir);
}

uint32_t gpu_frames_presented() { return g_presented; }

std::string gpu_device_name() {
    wgpu::AdapterInfo info;
    if (!aurora::webgpu::g_device || !aurora::webgpu::g_device.GetAdapter().GetInfo(&info)) return {};
    return std::string(info.device.data, info.device.length);
}

namespace {
// A texture of Aurora's, read back as RGBA rows. Empty if it cannot be.
std::vector<uint8_t> read_back(const aurora::webgpu::TextureWithSampler& src, uint32_t& w, uint32_t& h) {
    using namespace aurora::webgpu;
    w = src.size.width;
    h = src.size.height;
    if (!src.texture || !w || !h) return {};
    const uint32_t row = (w * 4 + 255) & ~255u;
    const wgpu::BufferDescriptor bd{.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead, .size = (uint64_t)row * h};
    const wgpu::Buffer buf = g_device.CreateBuffer(&bd);
    const wgpu::CommandEncoder enc = g_device.CreateCommandEncoder();
    const wgpu::TexelCopyTextureInfo from{.texture = src.texture};
    const wgpu::TexelCopyBufferInfo to{.layout = {.offset = 0, .bytesPerRow = row, .rowsPerImage = h}, .buffer = buf};
    const wgpu::Extent3D size{w, h, 1};
    enc.CopyTextureToBuffer(&from, &to, &size);
    const wgpu::CommandBuffer cb = enc.Finish();
    g_queue.Submit(1, &cb);
    bool ok = false;
    const wgpu::Future f = buf.MapAsync(wgpu::MapMode::Read, 0, bd.size, wgpu::CallbackMode::WaitAnyOnly,
                                        [&](wgpu::MapAsyncStatus s, wgpu::StringView) { ok = s == wgpu::MapAsyncStatus::Success; });
    g_instance.WaitAny(f, UINT64_MAX);
    if (!ok) return {};
    const auto* px = (const uint8_t*)buf.GetConstMappedRange(0, bd.size);
    const bool bgra = src.format == wgpu::TextureFormat::BGRA8Unorm || src.format == wgpu::TextureFormat::BGRA8UnormSrgb;
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* q = px + (size_t)y * row + x * 4;
            uint8_t* d = &rgba[((size_t)y * w + x) * 4];
            d[0] = bgra ? q[2] : q[0]; d[1] = q[1]; d[2] = bgra ? q[0] : q[2]; d[3] = q[3];
        }
    buf.Unmap();
    return rgba;
}
}  // namespace

// The framebuffer Aurora presents from, read back -- with the interface over it, as the
// window shows it, for `with_ui`.
void gpu_dump(const std::string& path, bool with_ui) {
    aurora::gfx::render_worker::synchronize();
    uint32_t w, h;
    std::vector<uint8_t> img = read_back(aurora::webgpu::present_source(), w, h);
    if (img.empty()) { fprintf(stderr, "[dump] %s: cannot read the framebuffer back\n", path.c_str()); return; }
    for (size_t i = 3; i < img.size(); i += 4) img[i] = 255;
    uint32_t uw, uh;
    const std::vector<uint8_t> ui = with_ui ? read_back(aurora::rmlui::render_target(), uw, uh) : std::vector<uint8_t>{};
    if (!ui.empty()) {
        // The game's frame stretched to the interface's size, under it (premultiplied).
        std::vector<uint8_t> out((size_t)uw * uh * 4);
        for (uint32_t y = 0; y < uh; y++)
            for (uint32_t x = 0; x < uw; x++) {
                const uint8_t* g = &img[(((size_t)y * h / uh) * w + (size_t)x * w / uw) * 4];
                const uint8_t* u = &ui[((size_t)y * uw + x) * 4];
                uint8_t* d = &out[((size_t)y * uw + x) * 4];
                for (int c = 0; c < 3; c++) d[c] = (uint8_t)std::min(255, u[c] + g[c] * (255 - u[3]) / 255);
                d[3] = 255;
            }
        img = std::move(out);
        w = uw;
        h = uh;
    }
    write_png(path.c_str(), img.data(), (int)w, (int)h);
}
