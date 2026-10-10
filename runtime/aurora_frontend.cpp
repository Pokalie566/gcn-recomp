// The game drawn by Aurora (gx/aurora_bridge.cpp): a window, the guest, and nothing else
// yet -- no launcher, no menus, no sound. What the step that moves the renderer to Aurora
// starts from; main.cpp takes it over once Aurora draws the game as the OpenGL renderer
// does.
//
//   <game>_aurora [disc image] [--dump-dir=DIR --dump-every=N] [--scale=N]
//
// Scripted input (GCN_INPUT) and GCN_TIMESCALE work as with the game.
#include "runtime.h"
#include "platform.h"
#include "prepare.h"
#include "input_script.h"
#include "hw/pad.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include "gfx/render_worker.hpp"   // Aurora's
#include "webgpu/gpu.hpp"          // Aurora's
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

uint32_t boot_load(const char* iso_path);
void debug_dump_threads();
bool write_png(const char* path, const uint8_t* rgba, int w, int h);
namespace gx { void aurora_bridge_start(); }

static const char* g_dump_dir;
static int g_dump_every;

// --dump-dir: every Nth frame as a PNG, read back from the framebuffer Aurora presents.
static void dump(const char* name);
void aurora_dump_frame(uint32_t frame) {
    if (!g_dump_dir || !g_dump_every || frame % g_dump_every) return;
    // GCN_DUMP_RANGE=a-b: only those frames, as with the OpenGL renderer.
    static uint32_t lo = 0, hi = ~0u;
    static const bool parsed = [] {
        if (const char* r = getenv("GCN_DUMP_RANGE")) sscanf(r, "%u-%u", &lo, &hi);
        return true;
    }();
    (void)parsed;
    if (frame < lo || frame > hi) return;
    char name[32];
    snprintf(name, sizeof(name), "frame_%05u", frame);
    dump(name);
}
void aurora_dump_named(const char* name) { if (g_dump_dir) dump(name); }

static void dump(const char* name) {
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

static void on_interrupt() {
    plat_watchdog(2, 2);
    debug_dump_threads();
    plat_exit_now(1);
}

static void on_fault(const void* fault_addr, int code) {
    const uintptr_t a = (uintptr_t)fault_addr;
    if (a >= (uintptr_t)g_mem && a < (uintptr_t)g_mem + 0x40000000)
        fprintf(stderr, "\nFAULT: guest memory access at (addr & 0x3FFFFFFF) = %08lX\n", (unsigned long)(a - (uintptr_t)g_mem));
    else
        fprintf(stderr, "\nFAULT: host address %p (code %d)\n", fault_addr, code);
    plat_watchdog(2, 2);
    plat_backtrace_print();
    debug_dump_threads();
    plat_exit_now(1);
}

int main(int argc, char** argv) {
    std::string iso = GCN_DEFAULT_ISO;
    int scale = 1;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--dump-dir=", 11)) g_dump_dir = argv[i] + 11;
        else if (!strncmp(argv[i], "--dump-every=", 13)) g_dump_every = atoi(argv[i] + 13);
        else if (!strncmp(argv[i], "--scale=", 8)) scale = atoi(argv[i] + 8);
        else if (argv[i][0] != '-') iso = argv[i];
    }
    if (!plat_readable(iso.c_str())) iso = plat_find_file("rom", ".iso");
    if (!plat_readable(iso.c_str())) {
        fprintf(stderr, "No game image found. Pass the path to your disc image.\n");
        return 1;
    }
    if (g_dump_dir) plat_make_dirs(g_dump_dir);
    // GCN_SAVE_DIR: this machine's state for the game somewhere else than saves/ -- a
    // scripted run starts from an empty one (tools/route.sh), as with the game.
    if (const char* d = getenv("GCN_SAVE_DIR")) g_save_dir = d;
    plat_install_crash_handlers(on_interrupt, on_fault);
    if (!game_module_ensure(iso.c_str(), false)) return 1;

    // Aurora's caches (pipelines, Dawn's shaders, ImGui's layout) with the rest of this
    // machine's state for the game (save_path), not in a folder of Aurora's choosing.
    static const std::string aurora_dir = save_path("aurora");
    plat_make_dirs(aurora_dir);
    AuroraConfig config{};
    config.appName = GCN_APP_NAME;
    config.userPath = aurora_dir.c_str();
    config.cachePath = aurora_dir.c_str();
    config.windowWidth = 640 * (scale < 1 ? 1 : scale);
    config.windowHeight = 480 * (scale < 1 ? 1 : scale);
    config.vsync = false;   // the guest keeps the game's pace
    config.mem1Size = 0;    // the guest's memory is ours (g_mem)
    config.mem2Size = 0;
    aurora_initialize(argc, argv, &config);

    mem_init();
    timing_init();
    input_script_init();
    const uint32_t entry = boot_load(iso.c_str());
    gx::aurora_bridge_start();
    threads_start_boot(entry);

    for (;;) {
        for (const AuroraEvent* e = aurora_update(); e && e->type != AURORA_NONE; e++)
            if (e->type == AURORA_EXIT) plat_exit_now(0);
        PadState p;
        p.connected = true;
        pad_set_state(0, p);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}
