// The GPU side of the game's GX stream. See aurora_gpu.h.
#include "aurora_gpu.h"
#include "aurora_bridge.h"
#include "../platform.h"
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include "gfx/render_worker.hpp"   // Aurora's
#include "gx/fifo.hpp"             // Aurora's
#include "webgpu/gpu.hpp"          // Aurora's
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
                    gpu_dump(name);
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

// The framebuffer Aurora presents from, read back.
void gpu_dump(const char* name) {
    if (!g_dump_dir) return;
    using namespace aurora::webgpu;
    aurora::gfx::render_worker::synchronize();
    const auto& src = present_source();
    const uint32_t w = src.size.width, h = src.size.height, row = (w * 4 + 255) & ~255u;
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
    if (!ok) { fprintf(stderr, "[dump] %s: cannot read the framebuffer back\n", name); return; }
    const auto* px = (const uint8_t*)buf.GetConstMappedRange(0, bd.size);
    const bool bgra = src.format == wgpu::TextureFormat::BGRA8Unorm || src.format == wgpu::TextureFormat::BGRA8UnormSrgb;
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* s = px + (size_t)y * row + x * 4;
            uint8_t* d = &rgba[((size_t)y * w + x) * 4];
            d[0] = bgra ? s[2] : s[0]; d[1] = s[1]; d[2] = bgra ? s[0] : s[2]; d[3] = 255;
        }
    buf.Unmap();
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.png", g_dump_dir, name);
    write_png(path, rgba.data(), (int)w, (int)h);
}
