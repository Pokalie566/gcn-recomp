// The game drawn by Aurora (gx/aurora_bridge.h, aurora_gpu.h): a window, the guest, and nothing else
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
#include "gx/aurora_gpu.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

uint32_t boot_load(const char* iso_path);
void debug_dump_threads();

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
    const char* dump_dir = nullptr;
    int dump_every = 0;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--dump-dir=", 11)) dump_dir = argv[i] + 11;
        else if (!strncmp(argv[i], "--dump-every=", 13)) dump_every = atoi(argv[i] + 13);
        else if (!strncmp(argv[i], "--scale=", 8)) scale = atoi(argv[i] + 8);
        else if (argv[i][0] != '-') iso = argv[i];
    }
    if (!plat_readable(iso.c_str())) iso = plat_find_file("rom", ".iso");
    if (!plat_readable(iso.c_str())) {
        fprintf(stderr, "No game image found. Pass the path to your disc image.\n");
        return 1;
    }
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
    gpu_init();
    gpu_set_dump(dump_dir, dump_every);

    mem_init();
    timing_init();
    input_script_init();
    const uint32_t entry = boot_load(iso.c_str());
    threads_start_boot(entry);

    for (;;) {
        for (const AuroraEvent* e = aurora_update(); e && e->type != AURORA_NONE; e++)
            if (e->type == AURORA_EXIT) plat_exit_now(0);
        PadState p;
        p.connected = true;
        pad_set_state(0, p);
        gpu_pump(4);
    }
}
