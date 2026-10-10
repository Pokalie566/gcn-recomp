// The GPU side of the game's GX stream. See aurora_gpu.h.
#include "aurora_gpu.h"
#include "aurora_bridge.h"
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

void begin() {
    while (!aurora_begin_frame()) std::this_thread::sleep_for(std::chrono::milliseconds(1));   // minimised: wait
    g_open = true;
}

void feed(const gx::Chunk& c) { aurora::gx::fifo::write_data(c.bytes.data(), (uint32_t)c.bytes.size()); }

void end() {
    aurora_end_frame();
    g_open = false;
}

}  // namespace

void gpu_init() {
    // A draw waits for its pipeline, as on the console: no geometry is missing from a frame
    // the first time a shader is needed (otherwise it is skipped until compiled).
    aurora_set_pipeline_blocking(true);
}

bool gpu_pump(int timeout_ms) {
    gx::Chunk c;
    for (int wait = timeout_ms; gx::take_chunk(c, wait); wait = 0) {
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
            g_last = std::move(g_partial);
            g_partial.clear();
            g_presented++;
            if (g_dump_dir && g_dump_every && g_presented % g_dump_every == 0) {
                static uint32_t lo = 0, hi = ~0u;
                static const bool parsed = [] {
                    if (const char* r = getenv("GCN_DUMP_RANGE")) sscanf(r, "%u-%u", &lo, &hi);
                    return true;
                }();
                (void)parsed;
                if (g_presented >= lo && g_presented <= hi) {
                    char name[32];
                    snprintf(name, sizeof(name), "frame_%05u", g_presented);
                    gpu_dump(name, false);
                }
            }
            return true;
        }
    }
    return false;
}

void gpu_present_again() {
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
void gpu_dump(const char* name, bool with_ui) {
    if (!g_dump_dir) return;
    aurora::gfx::render_worker::synchronize();
    uint32_t w, h;
    std::vector<uint8_t> img = read_back(aurora::webgpu::present_source(), w, h);
    if (img.empty()) { fprintf(stderr, "[dump] %s: cannot read the framebuffer back\n", name); return; }
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
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.png", g_dump_dir, name);
    write_png(path, img.data(), (int)w, (int)h);
}
