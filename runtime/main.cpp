// Entry point and frontend: the window and the GPU through Aurora (SDL3 and WebGPU), input,
// audio, the launcher and the pause menu. The guest runs on its own threads; this one owns
// the window and hands the GX stream the guest sends (gx/aurora_bridge.h) to Aurora
// (gx/aurora_gpu.h), presenting each frame with the interface over it.
#include "runtime.h"
#include "platform.h"
#include "input_script.h"
#include "input_log.h"
#include "card.h"
#include "crash_log.h"
#include "hw/pad.h"
#include "pad_bindings.h"
#include "gx/aurora_bridge.h"
#include "gx/aurora_gpu.h"
#include "gx/packs.h"
#include "mouse_aim.h"
#include "prepare.h"
#include "settings.h"
#include "ui/launcher.h"
#include "ui/pick_file.h"
#include "ui/ui.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/vi.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

uint32_t boot_load(const char* iso_path);
extern "C" void VISetFrameBufferScale(float scale);   // Aurora's: the EFB at 640x480 times this
void debug_dump_threads();
void debug_sampler_start();

static void on_interrupt() {
    plat_watchdog(2, 2);
    input_log_end();
    debug_dump_threads();
    plat_exit_now(1);
}

static void on_fault(const void* fault_addr, int code) {
    uintptr_t a = (uintptr_t)fault_addr;
    char what[128];
    if (g_mem && a >= (uintptr_t)g_mem && a < (uintptr_t)g_mem + 0x40000000)
        snprintf(what, sizeof(what), "a memory fault, guest address %08lX", (unsigned long)(a - (uintptr_t)g_mem));
    else
        snprintf(what, sizeof(what), "a memory fault, host address %p (code %d)", fault_addr, code);
    fprintf(stderr, "\nFAULT: %s\n", what);
    plat_watchdog(4, 2);
    input_log_end();
    plat_backtrace_print();
    debug_dump_threads();
    crash_report(what);
    plat_exit_now(1);
}

bool audio_open();
void audio_set_volume(float v);
void audio_set_paused(bool paused);

// ---------------------------------------------------------------------------
// Input: Aurora's pad (pad_bindings.h) -- keyboard and controllers through the player's
// bindings -- as GC pad 1, the mouse over it.
// ---------------------------------------------------------------------------
static SDL_Window* g_win;

// GCN_RAMSNAP=<dir> writes the low 8 MB of guest RAM every GCN_RAMSNAP_EVERY presented
// frames (default 150), within GCN_RAMSNAP_RANGE=a-b if set: diffing snapshots taken in
// known game states is how a game variable gets found. Taken from this thread, so a
// snapshot is the guest's state at about that frame, not at an exact instruction.
static void ram_snapshot(uint32_t frame) {
    static const char* dir = getenv("GCN_RAMSNAP");
    if (!dir) return;
    static const uint32_t every = getenv("GCN_RAMSNAP_EVERY") ? (uint32_t)atoi(getenv("GCN_RAMSNAP_EVERY")) : 150;
    static int lo = -1, hi = -1;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        plat_make_dirs(dir);
        if (const char* r = getenv("GCN_RAMSNAP_RANGE")) {
            lo = atoi(r);
            const char* dash = strchr(r, '-');
            hi = dash ? atoi(dash + 1) : lo;
        }
    }
    if (!every || frame % every || (lo >= 0 && ((int)frame < lo || (int)frame > hi))) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/ram_%05u.bin", dir, frame);
    if (FILE* f = fopen(path, "wb")) {
        fwrite(mem_ptr(0x80000000), 1, 8u << 20, f);
        fclose(f);
    }
}

// --mouse-aim (GCN_MOUSE_AIM=1, a setting): the mouse drives the C stick (see mouse_aim.h),
// its buttons the triggers. The cursor is captured while the window has it; F1 lets it go, a
// click takes it back. A game whose stick needs other settings sets their defaults at compile
// time (GCN_MOUSE_DEADZONE, GCN_MOUSE_INVERT_Y, GCN_MOUSE_Y on its target); the environment
// overrides them. The sensitivity is the player's (mouse_sens).
static bool g_mouse_aim;
static MouseAim g_aim;
#ifndef GCN_MOUSE_DEADZONE
#define GCN_MOUSE_DEADZONE 0.15f
#endif
#ifndef GCN_MOUSE_INVERT_Y
#define GCN_MOUSE_INVERT_Y 0
#endif
#ifndef GCN_MOUSE_Y
#define GCN_MOUSE_Y 1
#endif
static void mouse_aim_init() {
    g_aim.deadzone = GCN_MOUSE_DEADZONE;
    g_aim.use_y = GCN_MOUSE_Y != 0;
    if (const char* e = getenv("GCN_MOUSE_Y")) g_aim.use_y = atoi(e) != 0;
    if (const char* e = getenv("GCN_MOUSE_DEADZONE")) g_aim.deadzone = (float)atof(e);
}
// The game's way up for the mouse (GCN_MOUSE_INVERT_Y), turned over by the player's setting.
static bool mouse_invert_y(bool flip) {
    const char* e = getenv("GCN_MOUSE_INVERT_Y");
    return (e ? atoi(e) != 0 : GCN_MOUSE_INVERT_Y != 0) != flip;
}
static bool mouse_captured() { return SDL_GetWindowRelativeMouseMode(g_win); }
static void mouse_capture(bool on) { SDL_SetWindowRelativeMouseMode(g_win, on); }
// The mouse's part of the pad: the C stick while it moves, and the triggers.
static void mouse_aim_apply(PadState& p) {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    g_aim.update(std::chrono::duration<float>(now - last).count());
    last = now;
    if (!mouse_captured()) return;
    const float cx = g_aim.stick_x(), cy = g_aim.stick_y();
    if (cx != 0 || cy != 0) {
        p.cstick_x = (uint8_t)lroundf(128 + 100 * cx);
        p.cstick_y = (uint8_t)lroundf(128 + 100 * cy);
    }
    const SDL_MouseButtonFlags mb = SDL_GetMouseState(nullptr, nullptr);
    if (mb & SDL_BUTTON_LMASK) { p.buttons |= PAD_R; p.trig_r = 255; }
    if (mb & SDL_BUTTON_RMASK) { p.buttons |= PAD_L; p.trig_l = 255; }
}

// From a pause until the player has let go of everything, the game sees the pad at rest:
// whatever was held to resume must not reach it.
static bool g_hold_neutral;

static bool at_rest(const PadState& p) {
    return !p.buttons && !p.trig_l && !p.trig_r && p.stick_x == 128 && p.stick_y == 128 &&
           p.cstick_x == 128 && p.cstick_y == 128;
}

// The rumble: the game's (g_rumble_game, set while it runs the pad's motor and the setting
// lets it), or the settings' test for half a second. Brought up to date as the pad is read.
static bool g_rumble_game;
static std::chrono::steady_clock::time_point g_rumble_test_end;
static void rumble_test() { g_rumble_test_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(500); }
static void rumble_update() {
    const bool on = g_rumble_game || std::chrono::steady_clock::now() < g_rumble_test_end;
    static bool was;
    if (on != was) pad_rumble(on);
    was = on;
}

// The pad as the player holds it, through the bindings and the mouse.
static PadState read_pad() {
    rumble_update();
    PadState p = pad_read();
    p.connected = true;   // the keyboard plays when no controller does
    if (g_mouse_aim) mouse_aim_apply(p);
    return p;
}

// The controller's name, for the settings' test; the keyboard's part goes without saying.
static std::string pad_name() {
    const std::string n = pad_controller_name();
    return n.empty() ? ui_tr("No controller: the keyboard plays") : n;
}


static void update_pad(bool paused) {
    PadState p = read_pad();
    if (paused) g_hold_neutral = true;
    else if (g_hold_neutral && at_rest(p)) g_hold_neutral = false;
    if (g_hold_neutral) {
        p = PadState{};
        p.connected = true;
    }
    pad_set_state(0, p);
}

// ---------------------------------------------------------------------------
// Settings: settings.ini's, with the command line over them for this run (settings.h).
// They apply at startup, then again, whichever changed, when the game resumes from a pause:
// that is when a change to the file shows, and what a menu will use.
// ---------------------------------------------------------------------------
static int g_argc;
static char** g_argv;
static Settings g_saved;        // settings.ini's

static Settings session_settings() {
    Settings s = g_saved;
    settings_from_args(s, g_argc, g_argv);
    return s;
}

static AuroraBackend backend_of(const std::string& name) {
    if (name == "metal") return BACKEND_METAL;
    if (name == "vulkan") return BACKEND_VULKAN;
    if (name == "d3d12") return BACKEND_D3D12;
    return BACKEND_AUTO;
}

static const char* backend_name(AuroraBackend b) {
    switch (b) {
    case BACKEND_METAL: return "Metal";
    case BACKEND_VULKAN: return "Vulkan";
    case BACKEND_D3D12: return "Direct3D 12";
    case BACKEND_D3D11: return "Direct3D 11";
    case BACKEND_OPENGL: return "OpenGL";
    case BACKEND_OPENGLES: return "OpenGL ES";
    default: return "WebGPU";
    }
}

// All of them with `was` null, at startup, before the guest runs; afterwards only what
// changed, and only while the guest is stopped (guest_pause), except the window's. The GPU
// API and the samples per pixel take effect at the next start.
static void apply_settings(const Settings& s, const Settings* was) {
    auto changed = [&](auto member) { return !was || s.*member != was->*member; };
    if (changed(&Settings::scale)) VISetFrameBufferScale((float)std::clamp(s.scale, 1, 8));
    if (changed(&Settings::fullscreen)) SDL_SetWindowFullscreen(g_win, s.fullscreen);
    if (changed(&Settings::borderless)) SDL_SetWindowBordered(g_win, !s.borderless);
    if (changed(&Settings::vsync)) aurora_enable_vsync(s.vsync);
    if (changed(&Settings::textures) || changed(&Settings::textures_off)) gx::texture_packs_apply(s.textures, s.textures_off);
    if (changed(&Settings::interpolate)) gpu_set_interpolate(s.interpolate);
    if (changed(&Settings::widescreen)) {
        // At 16:9 Aurora's frame is 854 logical pixels wide, into which the bridge maps what
        // the game draws in its 640 (gx/aurora_bridge.cpp); the window is letterboxed to it.
        g_widescreen = s.widescreen;
        GXRenderModeObj mode = GXNtsc480IntDf;
        mode.fbWidth = g_widescreen ? 854 : 640;
        VIConfigure(&mode);
    }
    if (changed(&Settings::deadzone)) pad_set_deadzone(s.deadzone);
    if (was && changed(&Settings::iso)) fprintf(stderr, "settings: the new disc image is for the next start\n");
    audio_set_volume((float)s.volume / 100.0f);
    g_mouse_aim = s.mouse_aim;
    g_aim.sens = s.mouse_sens;
    g_aim.invert_y = mouse_invert_y(s.mouse_invert);
}

int main(int argc, char** argv) {
    if (crash_report_main(argc, argv)) return 0;   // the dialog of a run that crashed
    g_argc = argc;
    g_argv = argv;
    // GCN_SAVE_DIR=<dir>: this machine's state for the game -- card, settings, shader cache,
    // the prepared game -- somewhere else than saves/. A scripted run uses an empty one.
    //
    // An app (gcn-recomp CMakeLists, GCN_APP) carries what preparing the game and the menus
    // need -- in <Name>.app/Contents/Resources on macOS, in resources/ beside the executable
    // elsewhere -- and keeps this machine's state where the system keeps an app's.
    static std::string app_res, app_data;
    const std::string exe_dir = plat_executable_dir();
#ifdef __APPLE__
    if (exe_dir.find(".app/Contents/MacOS") != std::string::npos) app_res = exe_dir + "/../Resources";
#else
    if (plat_readable((exe_dir + "/resources/ui/pause.rml").c_str())) app_res = exe_dir + "/resources";
#endif
    if (!app_res.empty() && !getenv("GCN_RESOURCES")) {
#ifdef _WIN32
        _putenv_s("GCN_RESOURCES", app_res.c_str());
        const char* base = getenv("APPDATA");
        app_data = std::string(base ? base : ".") + "\\" GCN_APP_NAME;
#else
        setenv("GCN_RESOURCES", app_res.c_str(), 1);
        const char* home = getenv("HOME");
#ifdef __APPLE__
        app_data = std::string(home ? home : ".") + "/Library/Application Support/" GCN_APP_NAME;
#else
        const char* xdg = getenv("XDG_DATA_HOME");
        app_data = (xdg && *xdg ? std::string(xdg) : std::string(home ? home : ".") + "/.local/share") + "/" GCN_APP_NAME;
#endif
#endif
        g_save_dir = app_data.c_str();
    }
    if (const char* d = getenv("GCN_SAVE_DIR")) g_save_dir = d;
    plat_make_dirs(save_path(""));
    log_start(save_path(GCN_GAME_NAME ".log"));
    g_fatal_hook = crash_report;
    const std::string settings_path = save_path("settings.ini");
    if (!settings_load(settings_path, g_saved)) {
        plat_make_dirs(save_path(""));
        settings_save(settings_path, g_saved);   // the defaults, for the player to find
    }
    Settings cur = session_settings();

    // Default: the image the build was configured with, else the first image in ./rom
    std::string iso_default = GCN_DEFAULT_ISO;
    if (!plat_readable(iso_default.c_str())) iso_default = plat_find_file("rom", ".iso");
    if (iso_default.empty()) iso_default = plat_find_file("rom", ".ciso");
    const char* iso = cur.iso.empty() ? iso_default.c_str() : cur.iso.c_str();
    bool headless = false, input_log = true, fast = false, dump_textures = false;
    std::string input_log_dir, replay_dir;
    const char* dump_dir = nullptr;
    int dump_every = 0;
    bool prepare_only = false;   // --prepare: build the game module from the disc, then quit
    bool no_launcher = false;    // --no-launcher: straight into the game
    // The settings' own options are settings_from_args's.
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--log-all")) for (auto& e : g_log_enabled) e = true;
        else if (!strncmp(argv[i], "--log=", 6)) {
            if (!log_enable_list(argv[i] + 6)) { fprintf(stderr, "--log: unknown category in %s (cpu,os,hw,dvd,gx,vi,si,exi,dsp,ai,thr)\n", argv[i] + 6); return 1; }
        }
        else if (!strcmp(argv[i], "--no-input-log")) input_log = false;
        else if (!strncmp(argv[i], "--input-log=", 12)) input_log_dir = argv[i] + 12;
        else if (!strncmp(argv[i], "--replay=", 9)) replay_dir = argv[i] + 9;
        else if (!strcmp(argv[i], "--sample")) debug_sampler_start();
        else if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--fast")) fast = true;
        else if (!strcmp(argv[i], "--dump-textures")) dump_textures = true;
        else if (!strcmp(argv[i], "--prepare")) prepare_only = true;
        else if (!strcmp(argv[i], "--no-launcher")) no_launcher = true;
        else if (!strncmp(argv[i], "--dump-dir=", 11)) dump_dir = argv[i] + 11;
        else if (!strncmp(argv[i], "--dump-every=", 13)) dump_every = atoi(argv[i] + 13);
    }
    plat_install_crash_handlers(on_interrupt, on_fault);

    // The launcher comes first unless the run is scripted or asks not to: then the disc and
    // the game's module must already be there, and anything missing is said on stderr.
    const bool launcher = !headless && !prepare_only && !no_launcher && replay_dir.empty() && !getenv("GCN_INPUT");
    if (!launcher) {
        if (!plat_readable(iso)) {
            fprintf(stderr, "No game image found. Put your " GCN_GAME_TITLE " disc image in rom/ or pass its path.\n");
            return 1;
        }
        // The game's code is a module built from the disc, not part of this executable.
        if (!game_module_ensure(iso, prepare_only)) return 1;
        if (prepare_only) return 0;
    }

    // GCN_MEMCARD=<dir>: another card than the player's (card.h), made empty if absent -- for
    // a scripted run, which must neither see nor touch the real saves.
    std::string replay_card;
    if (const char* mc = getenv("GCN_MEMCARD")) {
        replay_card = mc;
        g_card_base = replay_card.c_str();
    }

    // What the guest needs before it boots, once its module is loaded.
    auto start_guest = [&]() -> bool {
        mem_init();
        timing_init();
        if (fast) clock_set_scale(0);  // run as fast as the host allows; the game sees 60 Hz regardless
        input_script_init();
        // Every run records what the guest read from the controller, so that a route to
        // wherever something went wrong can be played back with --replay=<dir>. See
        // input_log.h. --no-input-log turns it off; --input-log=<dir> names the directory.
        // The memory card is snapshotted into the log, and a replay runs on a scratch copy
        // of that snapshot rather than the real card.
        if (!replay_dir.empty()) {
            if (!input_replay_load(replay_dir, replay_card)) return false;
            if (!replay_card.empty()) g_card_base = replay_card.c_str();
        }
        card_init();
        if (input_log) {
            if (input_log_dir.empty()) {
                char stamp[64];
                time_t now = time(nullptr);
                strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
                input_log_dir = save_path(std::string("inputs/") + stamp);
            }
            input_log_start(input_log_dir, g_card_base ? g_card_base : save_path("card"));
        }
        return true;
    };

    if (headless) {
        // No window: the GX stream is taken and dropped as it comes.
        if (!start_guest()) return 1;
        uint32_t entry = boot_load(iso);
        threads_start_boot(entry);
        for (gx::Chunk c;;) {
            PadState p;
            p.connected = true;
            pad_set_state(0, p);
            if (gx::take_chunk(c, 5)) gx::chunk_done(c.seq);
        }
    }

    // The window and the GPU (Aurora): their caches with the rest of this machine's state.
    g_widescreen = cur.widescreen;
    plat_make_dirs(save_path(""));
    static const std::string aurora_dir = save_path("aurora");
    plat_make_dirs(aurora_dir);
    const char* res = getenv("GCN_RESOURCES");
    const std::string ui_dir = res ? std::string(res) + "/ui" : plat_executable_dir() + "/ui";
    AuroraConfig config{};
    config.appName = GCN_APP_NAME;
    config.userPath = aurora_dir.c_str();
    config.cachePath = aurora_dir.c_str();
    config.desiredBackend = backend_of(cur.backend);
    config.msaa = (uint32_t)std::clamp(cur.msaa, 1, 8);
    config.vsync = cur.vsync;
    config.startFullscreen = cur.fullscreen;
    config.windowWidth = cur.window_w > 0 ? (uint32_t)cur.window_w : (g_widescreen ? 1706 : 1280);
    config.windowHeight = cur.window_h > 0 ? (uint32_t)cur.window_h : 960;
    config.allowTextureDumps = dump_textures;
    config.mem1Size = 0;   // the guest's memory is g_mem
    config.mem2Size = 0;
    const AuroraInfo info = aurora_initialize(argc, argv, &config);
    g_win = info.window;
    // The window's icon, where the game project gives one (gcn_add_game's APP_ICON).
    if (SDL_Surface* icon = SDL_LoadPNG((ui_dir + "/icon.png").c_str())) {
        SDL_SetWindowIcon(g_win, icon);
        SDL_DestroySurface(icon);
    }
    SDL_SetWindowTitle(g_win, GCN_GAME_TITLE " (recompiled)");
    // The frame keeps the game's shape (4:3, or 16:9) whatever the window's, letterboxed.
    AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
    gpu_init();
    gpu_set_dump(dump_dir, dump_every);

    const bool audio_ok = audio_open();
    pad_bindings_init();
    pad_bindings_migrate(g_saved.old_binds, aurora_dir);
    mouse_aim_init();
    apply_settings(cur, nullptr);
    ui_init(g_win, ui_dir, cur.lang);
    crash_set_translator(ui_trf);
    // GCN_CRASH_TEST=1: a fault right here, =fatal a fatal error, to see the crash report.
    if (const char* t = getenv("GCN_CRASH_TEST")) {
        if (!strcmp(t, "fatal")) fatal("GCN_CRASH_TEST");
        *(volatile int*)nullptr = 1;
    }
    // A change the player made: applied, and saved over settings.ini's.
    auto commit = [&](const Settings& s) {
        auto take = [&](auto m) { if (s.*m != cur.*m) g_saved.*m = s.*m; };
        take(&Settings::iso), take(&Settings::scale), take(&Settings::widescreen), take(&Settings::interpolate);
        take(&Settings::fullscreen), take(&Settings::textures), take(&Settings::mouse_aim), take(&Settings::rumble);
        take(&Settings::mouse_sens), take(&Settings::mouse_invert), take(&Settings::volume), take(&Settings::lang);
        take(&Settings::deadzone), take(&Settings::textures_off), take(&Settings::setup_done);
        take(&Settings::borderless), take(&Settings::window_w), take(&Settings::window_h), take(&Settings::vsync);
        take(&Settings::msaa), take(&Settings::backend);
        settings_save(settings_path, g_saved);
        apply_settings(s, &cur);
        cur = s;
    };
    auto toggle_fullscreen = [&] {
        Settings s = cur;
        s.fullscreen = !s.fullscreen;
        commit(s);
        ui_set_settings(cur);
    };
    // The window's size as the player leaves it, for the next start (not a fullscreen one's).
    auto window_resized = [&] {
        if (cur.fullscreen || (SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN)) return;
        int w, h;
        SDL_GetWindowSize(g_win, &w, &h);
        if (w == cur.window_w && h == cur.window_h) return;
        Settings s = cur;
        s.window_w = w;
        s.window_h = h;
        commit(s);
    };

    if (launcher && ui_ready()) {
        card_init();   // the launcher's saves page reads it
        LauncherEnv env;
        env.win = g_win;
        const std::string device = gpu_device_name();
        env.gpu = std::string(backend_name(info.backend)) + (device.empty() ? "" : ", " + device);
        env.audio = audio_ok;
        env.commit = commit;
        env.event = [&](SDL_Event& e) {
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F11 && !e.key.repeat) toggle_fullscreen();
            else if (e.type == SDL_EVENT_WINDOW_RESIZED) window_resized();
        };
        env.pad = read_pad;
        env.pad_name = pad_name;
        env.rumble = rumble_test;
        if (cur.iso.empty()) cur.iso = iso_default;   // what it finds by itself, for this run
        if (!launcher_run(env, cur)) plat_exit_now(0);
        iso = cur.iso.c_str();
    } else if (launcher) {
        // No interface to launch from: as a run without the launcher.
        if (!plat_readable(iso) || !game_module_ensure(iso, false)) return 1;
    }
    if (!start_guest()) return 1;
    if (g_mouse_aim) mouse_capture(true);

    uint32_t entry = boot_load(iso);
    threads_start_boot(entry);

    // Esc (or a controller's Back) pauses: the guest stops (guest_pause), the last frame
    // stays on screen under the pause menu (ui.h), the sound waits. Settings the player
    // changes there apply at once, the guest being stopped, and are saved: from then on they
    // win over the command line. GCN_PAUSE_AT=<frame>[:<ms>] pauses by itself at that
    // presented frame for that long (default 1000 ms), for checking that a pause changes
    // nothing.
    bool paused = false;
    std::chrono::steady_clock::time_point resume_at{};
    auto pause = [&] {
        paused = true;
        guest_pause();
        audio_set_paused(true);
        mouse_capture(false);
        SDL_SetWindowTitle(g_win, GCN_GAME_TITLE " (recompiled) - paused");
        ui_show_pause(cur);
        saves_update();
        settings_pages_update(cur, true);
    };
    auto resume = [&] {
        if (!guest_paused()) return;   // still on its way to stopping
        ui_hide();
        paused = false;
        resume_at = {};
        if (g_mouse_aim) mouse_capture(true);
        audio_set_paused(false);
        guest_resume();
        SDL_SetWindowTitle(g_win, GCN_GAME_TITLE " (recompiled)");
    };
    const char* pause_at = getenv("GCN_PAUSE_AT");
    const uint32_t quit_at = getenv("GCN_QUIT_AT") ? (uint32_t)atoi(getenv("GCN_QUIT_AT")) : 0;

    bool running = true;
    uint32_t frames = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (running) {
        for (const AuroraEvent* ev = aurora_update(); ev && ev->type != AURORA_NONE; ev++) {
            if (ev->type == AURORA_EXIT) running = false;
            if (ev->type != AURORA_SDL_EVENT) continue;
            SDL_Event e = ev->sdl;
            if (e.type == SDL_EVENT_WINDOW_RESIZED) window_resized();
            else if (paused && ui_event(e)) continue;
            else if ((e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE && !e.key.repeat) ||
                     (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK))
                paused ? resume() : pause();
            else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F11 && !e.key.repeat)
                toggle_fullscreen();
            else if (paused) continue;
            else if (g_mouse_aim && e.type == SDL_EVENT_MOUSE_MOTION && mouse_captured())
                g_aim.move(e.motion.xrel, e.motion.yrel);
            else if (g_mouse_aim && e.type == SDL_EVENT_MOUSE_WHEEL && mouse_captured())
                g_aim.wheel((int)(e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y));
            else if (g_mouse_aim && e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F1)
                mouse_capture(!mouse_captured());
            else if (g_mouse_aim && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !mouse_captured())
                mouse_capture(true);
        }
        if (paused) {
            const UiAction a = ui_take_action();
            if (a == UI_RESUME) resume();
            else if (a == UI_QUIT) running = false;
            else if (a == UI_LAUNCHER) {
                // The game cannot be unloaded, so the launcher is this program started again.
                // The card is already on disk: it is written as the game writes it.
                input_log_end();
                std::vector<char*> args;
                for (int i = 0; i < argc; i++)
                    if (strcmp(argv[i], "--no-launcher")) args.push_back(argv[i]);
                args.push_back(nullptr);
                plat_restart(args.data());
                fprintf(stderr, "cannot start again: %s\n", strerror(errno));
            } else if (a == UI_TEST_RUMBLE) rumble_test();
            else if (a == UI_OPEN_TEXTURES) plat_open_folder(gx::texture_pack_dir());
            else if (a == UI_PICK_DISC) {
                // For the next start: this one goes on with the disc it has.
                const std::string path = ui_pick_disc(ui_tr("Choose your disc image"));
                if (!path.empty()) {
                    Settings s = cur;
                    s.iso = path;
                    commit(s);
                    ui_set_settings(cur);
                    settings_pages_update(cur, true);
                }
            } else saves_act(a, ui_action_index());
            if (ui_open()) ui_set_pad(read_pad(), pad_name());
            Settings s = cur;
            if (guest_paused() && ui_take_settings(s)) commit(s);
        }
        update_pad(paused);
        g_rumble_game = cur.rumble && !paused && pad_motor(0);
        ui_frame();
        if (paused && guest_paused()) {
            // Nothing more is coming: the last frame again, under the menu.
            if (resume_at != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() >= resume_at) resume();
            else {
                gpu_present_again();
                ui_script();
            }
            SDL_Delay(4);
            continue;
        }
        if (!gpu_pump(4)) continue;
        frames++;
        const auto now = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(now - t0).count();
        if (secs >= 2.0 && !paused) {
            char title[128];
            snprintf(title, sizeof(title), GCN_GAME_TITLE " (recompiled) - %.1f fps", frames / secs);
            SDL_SetWindowTitle(g_win, title);
            frames = 0;
            t0 = now;
        }
        if (pause_at && !paused && gpu_frames_presented() == (uint32_t)atoi(pause_at)) {
            const char* ms = strchr(pause_at, ':');
            pause();
            resume_at = now + std::chrono::milliseconds(ms ? atoi(ms + 1) : 1000);
        }
        ram_snapshot(gpu_frames_presented());
        if (quit_at && gpu_frames_presented() >= quit_at) running = false;
    }
    input_log_end();
    threads_request_quit();
    plat_exit_now(0);
}
