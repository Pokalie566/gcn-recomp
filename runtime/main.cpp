// Entry point and SDL frontend: window, input, audio. The main thread executes GX
// batches produced by the guest thread.
#include "runtime.h"
#include "platform.h"
#include "input_script.h"
#include "input_log.h"
#include "gx/render_gl.h"
#include "gx/gl.h"
#include "hw/pad.h"
#include "vr_game.h"
#ifdef _WIN32
// Console app: keep our own main() rather than SDL2main's WinMain shim.
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>
#include "mouse_aim.h"
#include "gx/texture.h"
#include "gx/texture_pack.h"
#include "prepare.h"
#include "settings.h"
#include "ui/launcher.h"
#include "ui/ui.h"
#include <vector>

uint32_t boot_load(const char* iso_path);
void debug_dump_threads();
void debug_sampler_start();
bool write_png(const char* path, const uint8_t* rgba, int w, int h);

// ---------------------------------------------------------------------------
// --eye renders through the stereo path into an offscreen target and shows that in the
// window instead of the flat frame: the eye sits where the game's camera is, looking
// straight ahead with a 90 degree field, so a VR change can be looked at here before it
// goes anywhere near a headset. --first-person puts the eye wherever the game project's
// eye hook says (render_set_first_person), on the player's vehicle for a racing game.
// --dump-dir/--dump-every write the eye's frames as PNGs.
// ---------------------------------------------------------------------------
static bool g_eye_mode = false;
static GLuint g_eye_fbo, g_eye_tex, g_eye_depth;
static const int kEyeW = 960, kEyeH = 720;
// A VR frontend's typical HUD placement, in game units at 50 units per metre.
static const float kHudDist = 200.0f, kHudScale = 0.5f, kHudHeight = -18.0f;
static const float kWorldPitch = 23.2f * 3.14159265f / 180.0f;

static void eye_init() {
    glGenTextures(1, &g_eye_tex);
    glBindTexture(GL_TEXTURE_2D, g_eye_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEyeW, kEyeH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenRenderbuffers(1, &g_eye_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_eye_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kEyeW, kEyeH);
    glGenFramebuffers(1, &g_eye_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_eye_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_eye_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_eye_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("eye fbo incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gx::render_set_world_pitch(kWorldPitch);
    gx::render_set_vr_morph(1.0f, nullptr);
}

// Column-major, as glUniformMatrix4fv takes them. The view is the identity: the eye is
// wherever the renderer's world transform puts it.
static void eye_matrices(float* proj, float* view, float* hud) {
    const float fov = 1.0f;   // tan(45 deg): a 90 degree vertical field
    const float aspect = (float)kEyeW / (float)kEyeH;
    const float n = 5.0f, f = 100000.0f;
    memset(proj, 0, 16 * sizeof(float));
    proj[0] = 1.0f / (fov * aspect);
    proj[5] = 1.0f / fov;
    proj[10] = -(f + n) / (f - n);
    proj[11] = -1.0f;
    proj[14] = -(2.0f * f * n) / (f - n);
    memset(view, 0, 16 * sizeof(float));
    view[0] = view[5] = view[10] = view[15] = 1.0f;
    gx::render_hud_frame(kHudDist, fov, kHudScale, kHudHeight, 0.0f, hud);
}

static void eye_dump(uint32_t n) {
    std::vector<uint8_t> px((size_t)kEyeW * kEyeH * 4), fl(px.size());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_eye_fbo);
    glReadPixels(0, 0, kEyeW, kEyeH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    const size_t stride = (size_t)kEyeW * 4;
    for (int y = 0; y < kEyeH; y++) memcpy(&fl[y * stride], &px[(size_t)(kEyeH - 1 - y) * stride], stride);
    for (size_t i = 3; i < fl.size(); i += 4) fl[i] = 255;
    char path[512];
    snprintf(path, sizeof(path), "%s/eye_%05u.png", gx::g_dump_dir, n);
    write_png(path, fl.data(), kEyeW, kEyeH);
}

// Stringify, to quote the required GL version in a message without a format argument.
#define GCN_STR_(x) #x
#define GCN_STR(x) GCN_STR_(x)

static void on_interrupt() {
    plat_watchdog(2, 2);  // never hang in here (stdio locks may be held by other threads)
    input_log_end();
    debug_dump_threads();
    plat_exit_now(1);
}

static void on_fault(const void* fault_addr, int code) {
    uintptr_t a = (uintptr_t)fault_addr;
    if (a >= (uintptr_t)g_mem && a < (uintptr_t)g_mem + 0x40000000)
        fprintf(stderr, "\nFAULT: guest memory access at (addr & 0x3FFFFFFF) = %08lX\n", (unsigned long)(a - (uintptr_t)g_mem));
    else
        fprintf(stderr, "\nFAULT: host address %p (code %d)\n", fault_addr, code);
    plat_watchdog(2, 2);
    input_log_end();
    plat_backtrace_print();
    debug_dump_threads();
    plat_exit_now(1);
}

bool audio_open();
void audio_set_volume(float v);
void audio_set_paused(bool paused);

// ---------------------------------------------------------------------------
// Input: keyboard + first game controller -> GC pad 1, through the player's bindings
// (bindings.h), which name their inputs; here the names become SDL's codes.
// ---------------------------------------------------------------------------
static SDL_GameController* g_ctrl;

struct BoundInput {
    enum Kind { KEY, BUTTON, AXIS } kind;
    int code;
    int sign;   // the half of an axis
};
static std::vector<BoundInput> g_bound[ACT_COUNT];

static void bindings_resolve(const Bindings& b) {
    for (int a = 0; a < ACT_COUNT; a++) {
        g_bound[a].clear();
        for (const std::string& in : b.inputs[a]) {
            BoundInput r{BoundInput::KEY, -1, 1};
            if (!in.compare(0, 4, "key:")) {
                const SDL_Scancode k = SDL_GetScancodeFromName(in.c_str() + 4);
                if (k != SDL_SCANCODE_UNKNOWN) r = {BoundInput::KEY, k, 1};
            } else if (!in.compare(0, 4, "pad:")) {
                const SDL_GameControllerButton k = SDL_GameControllerGetButtonFromString(in.c_str() + 4);
                if (k != SDL_CONTROLLER_BUTTON_INVALID) r = {BoundInput::BUTTON, k, 1};
            } else if (!in.compare(0, 5, "axis:") && in.size() > 6 && (in[5] == '+' || in[5] == '-')) {
                const SDL_GameControllerAxis k = SDL_GameControllerGetAxisFromString(in.c_str() + 6);
                if (k != SDL_CONTROLLER_AXIS_INVALID) r = {BoundInput::AXIS, k, in[5] == '+' ? 1 : -1};
            }
            if (r.code >= 0) g_bound[a].push_back(r);
            else fprintf(stderr, "settings: bind.%s: no such input \"%s\"\n", kPadActionNames[a], in.c_str());
        }
    }
}

// How far an input is pushed, 0 to 1.
static float read_input(const BoundInput& r, const uint8_t* keys) {
    switch (r.kind) {
    case BoundInput::KEY:
        return keys[r.code] ? 1.0f : 0.0f;
    case BoundInput::BUTTON:
        return g_ctrl && SDL_GameControllerGetButton(g_ctrl, (SDL_GameControllerButton)r.code) ? 1.0f : 0.0f;
    case BoundInput::AXIS: {
        if (!g_ctrl) return 0.0f;
        // A stick at rest is never quite centred; a trigger at rest is.
        const bool trigger = r.code == SDL_CONTROLLER_AXIS_TRIGGERLEFT || r.code == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        const float v = (float)r.sign * SDL_GameControllerGetAxis(g_ctrl, (SDL_GameControllerAxis)r.code) / 32767.0f;
        return v > (trigger ? 0.03f : 0.12f) ? std::min(v, 1.0f) : 0.0f;
    }
    }
    return 0.0f;
}

// GCN_RAMSNAP=<dir> writes the low 8 MB of guest RAM every GCN_RAMSNAP_EVERY presented
// frames (default 150), within GCN_RAMSNAP_RANGE=a-b if set: diffing snapshots taken in
// known game states is how a game variable gets found. The same as the headset harness's
// (android/egl_main.cpp). Taken from the render thread, so a snapshot is the guest's state
// at about that frame, not at an exact instruction.
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
    g_aim.invert_y = GCN_MOUSE_INVERT_Y != 0;
    g_aim.use_y = GCN_MOUSE_Y != 0;
    if (const char* e = getenv("GCN_MOUSE_Y")) g_aim.use_y = atoi(e) != 0;
    if (const char* e = getenv("GCN_MOUSE_DEADZONE")) g_aim.deadzone = (float)atof(e);
    if (const char* e = getenv("GCN_MOUSE_INVERT_Y")) g_aim.invert_y = atoi(e) != 0;
}
// The mouse's part of the pad: the C stick while it moves, and the triggers.
static void mouse_aim_apply(PadState& p) {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    g_aim.update(std::chrono::duration<float>(now - last).count());
    last = now;
    if (!SDL_GetRelativeMouseMode()) return;
    const float cx = g_aim.stick_x(), cy = g_aim.stick_y();
    if (cx != 0 || cy != 0) {
        p.cstick_x = (uint8_t)lroundf(128 + 100 * cx);
        p.cstick_y = (uint8_t)lroundf(128 + 100 * cy);
    }
    const uint32_t mb = SDL_GetMouseState(nullptr, nullptr);
    if (mb & SDL_BUTTON(SDL_BUTTON_LEFT)) { p.buttons |= PAD_R; p.trig_r = 255; }
    if (mb & SDL_BUTTON(SDL_BUTTON_RIGHT)) { p.buttons |= PAD_L; p.trig_l = 255; }
}

// From a pause until the player has let go of everything, the game sees the pad at rest:
// whatever was held to resume must not reach it.
static bool g_hold_neutral;

static bool at_rest(const PadState& p) {
    return !p.buttons && !p.trig_l && !p.trig_r && p.stick_x == 128 && p.stick_y == 128 &&
           p.cstick_x == 128 && p.cstick_y == 128;
}

static void update_pad(bool paused) {
    const uint8_t* keys = SDL_GetKeyboardState(nullptr);
    float amount[ACT_COUNT];
    for (int a = 0; a < ACT_COUNT; a++) {
        amount[a] = 0.0f;
        for (const BoundInput& r : g_bound[a]) amount[a] = std::max(amount[a], read_input(r, keys));
    }
    PadState p = pad_from_actions(amount);
    if (g_mouse_aim) mouse_aim_apply(p);
    if (paused) g_hold_neutral = true;
    else if (g_hold_neutral && at_rest(p)) g_hold_neutral = false;
    if (g_hold_neutral) p = pad_from_actions(std::vector<float>(ACT_COUNT).data());
    pad_set_state(0, p);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Settings: settings.ini's, with the command line over them for this run (settings.h).
// They apply at startup, then again, whichever changed, when the game resumes from a pause:
// that is when a change to the file shows, and what a menu will use.
// ---------------------------------------------------------------------------
static int g_argc;
static char** g_argv;
static Settings g_saved;        // settings.ini's
static int g_interpolate;       // 0 off, 1 on, 2 GCN_INTERPOLATE=force (see main)

static Settings session_settings() {
    Settings s = g_saved;
    settings_from_args(s, g_argc, g_argv);
    return s;
}

// All of them with `was` null, at startup, before the guest runs; afterwards only what
// changed, and only while the guest is stopped (guest_pause), except the window's.
static void apply_settings(SDL_Window* win, const Settings& s, const Settings* was) {
    auto changed = [&](auto member) { return !was || s.*member != was->*member; };
    if (was && changed(&Settings::scale)) gx::render_set_internal_scale(s.scale);
    if (was && changed(&Settings::widescreen)) gx::render_set_widescreen(s.widescreen);
    if (changed(&Settings::fullscreen)) SDL_SetWindowFullscreen(win, s.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (changed(&Settings::textures)) {
        gx::texture_pack_set_enabled(s.textures);
        if (was) gx::texture_cache_clear();
    }
    if (changed(&Settings::bindings)) bindings_resolve(s.bindings);
    if (was && changed(&Settings::iso)) fprintf(stderr, "settings: the new disc image is for the next start\n");
    audio_set_volume((float)s.volume / 100.0f);
    if (g_interpolate != 2) g_interpolate = s.interpolate;
    g_mouse_aim = s.mouse_aim;
    g_aim.sens = s.mouse_sens;
}

int main(int argc, char** argv) {
    g_argc = argc;
    g_argv = argv;
    // GCN_SAVE_DIR=<dir>: this machine's state for the game -- card, settings, shader cache,
    // the prepared game -- somewhere else than saves/. A scripted run uses an empty one.
    //
    // In a macOS app (<Name>.app/Contents/MacOS/<exe>), what preparing the game and the
    // menus need is in Contents/Resources (GCN_RESOURCES), and this machine's state is the
    // user's, in ~/Library/Application Support/<Name>.
#ifdef __APPLE__
    static std::string app_res, app_data;
    const std::string exe_dir = plat_executable_dir();
    if (const size_t app = exe_dir.rfind(".app/Contents/MacOS"); app != std::string::npos && !getenv("GCN_RESOURCES")) {
        app_res = exe_dir.substr(0, app) + ".app/Contents/Resources";
        setenv("GCN_RESOURCES", app_res.c_str(), 1);
        const std::string name = exe_dir.substr(exe_dir.rfind('/', app) + 1, app - exe_dir.rfind('/', app) - 1);
        if (const char* home = getenv("HOME")) app_data = std::string(home) + "/Library/Application Support/" + name;
        g_save_dir = app_data.c_str();
    }
#endif
    if (const char* d = getenv("GCN_SAVE_DIR")) g_save_dir = d;
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
    bool headless = false, hidden = false, input_log = true, first_person = false, fast = false;
    std::string input_log_dir, replay_dir;
    // --interpolate: a game that runs at 30 fps shown at 60, one in-between frame drawn
    // between two of its frames (gx::render_execute_interpolated). GCN_INTERPOLATE=force
    // draws one before every frame and never waits, for looking at them in frame dumps of an
    // unpaced run.
    if (const char* ip = getenv("GCN_INTERPOLATE"); ip && !strcmp(ip, "force")) g_interpolate = 2;
    bool prepare_only = false;   // --prepare: build the game module from the disc, then quit
    bool no_launcher = false;    // --no-launcher: straight into the game
    // The settings' own options are settings_from_args's.
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--log-all")) for (auto& e : g_log_enabled) e = true;
        else if (!strncmp(argv[i], "--log=", 6)) {
            if (!log_enable_list(argv[i] + 6)) { fprintf(stderr, "--log: unknown category in %s (cpu,os,hw,dvd,gx,vi,si,exi,dsp,ai,thr)\n", argv[i] + 6); return 1; }
        }
        else if (!strcmp(argv[i], "--eye")) g_eye_mode = true;
        else if (!strcmp(argv[i], "--first-person")) first_person = true;
        else if (!strcmp(argv[i], "--no-input-log")) input_log = false;
        else if (!strncmp(argv[i], "--input-log=", 12)) input_log_dir = argv[i] + 12;
        else if (!strncmp(argv[i], "--replay=", 9)) replay_dir = argv[i] + 9;
        else if (!strcmp(argv[i], "--sample")) debug_sampler_start();
        else if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--hidden")) hidden = true;
        else if (!strcmp(argv[i], "--fast")) fast = true;
        else if (!strcmp(argv[i], "--cull-swap")) gx::g_cull_swap = true;
        else if (!strcmp(argv[i], "--dump-textures")) gx::texture_pack_enable_dump();
        else if (!strcmp(argv[i], "--prepare")) prepare_only = true;
        else if (!strcmp(argv[i], "--no-launcher")) no_launcher = true;
        else if (!strncmp(argv[i], "--dump-dir=", 11)) gx::g_dump_dir = argv[i] + 11;
        else if (!strncmp(argv[i], "--dump-every=", 13)) gx::g_dump_every = atoi(argv[i] + 13);
    }
    plat_install_crash_handlers(on_interrupt, on_fault);

    // The launcher comes first unless the run is scripted or asks not to: then the disc and
    // the game's module must already be there, and anything missing is said on stderr.
    const bool launcher = !headless && !prepare_only && !no_launcher && replay_dir.empty() && !getenv("GCN_INPUT");
    if (!launcher) {
        if (!plat_readable(iso)) {
            fprintf(stderr, "No game image found. Put your " GCN_GAME_TITLE " .iso or .ciso in rom/ or pass its path.\n");
            return 1;
        }
        // The game's code is a module built from the disc, not part of this executable.
        if (!game_module_ensure(iso, prepare_only)) return 1;
        if (prepare_only) return 0;
    }

    // What the guest needs before it boots, once its module is loaded.
    std::string replay_card;
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
            if (!replay_card.empty()) g_memcard_path = replay_card.c_str();
        }
        // GCN_MEMCARD=<file>: another card than the player's, created formatted if absent --
        // for a scripted run, which must neither see nor touch the real saves.
        if (const char* mc = getenv("GCN_MEMCARD"); mc && replay_card.empty()) {
            replay_card = mc;
            g_memcard_path = replay_card.c_str();
        }
        if (input_log) {
            if (input_log_dir.empty()) {
                char stamp[64];
                time_t now = time(nullptr);
                strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
                input_log_dir = save_path(std::string("inputs/") + stamp);
            }
            input_log_start(input_log_dir, replay_card.empty() ? save_path("memcard_a.raw") : replay_card);
        }
        return true;
    };

    if (headless) {
        if (!start_guest()) return 1;
        uint32_t entry = boot_load(iso);
        threads_start_boot(entry);
        for (;;) {
            PadState p;
            p.connected = true;
            pad_set_state(0, p);
            auto b = gx::take_batch(5);  // discard
            (void)b;
        }
    }

#ifdef _WIN32
    SDL_SetMainReady();
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) fatal("SDL_Init: %s", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, GCN_GL_MAJOR);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, GCN_GL_MINOR);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_widescreen = cur.widescreen;
    SDL_Window* win = SDL_CreateWindow(GCN_GAME_TITLE " (recompiled)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       g_widescreen ? 1706 : 1280, 960, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                       (hidden ? SDL_WINDOW_HIDDEN : 0));
    if (!win) fatal("SDL_CreateWindow: %s", SDL_GetError());
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    static const char* kGLHelp =
        "  The renderer needs an OpenGL " GCN_STR(GCN_GL_MAJOR) "." GCN_STR(GCN_GL_MINOR)
        " core profile. If this host only\n"
        "  offers legacy OpenGL (\"GDI Generic\" 1.1, typical for a VM with no GL driver),\n"
        "  install a GL implementation that provides it -- see docs/dev/graphics.md.";
    if (!ctx) fatal("SDL_GL_CreateContext: %s\n%s", SDL_GetError(), kGLHelp);
    SDL_GL_SetSwapInterval(0);
    // A legacy driver still "loads": it resolves the GL 1.1 exports and leaves every
    // 2.0+ entry point null, so check the version before handing off to the renderer.
    int glver = gl_load_with(SDL_GL_GetProcAddress);
    if (glver < GCN_GL_VERSION_MIN) {
        fatal("OpenGL %d.%d is too old (got \"%s\" / \"%s\")\n%s",
              glver / 10, glver % 10, glGetString(GL_VERSION) ? (const char*)glGetString(GL_VERSION) : "?",
              glGetString(GL_RENDERER) ? (const char*)glGetString(GL_RENDERER) : "?", kGLHelp);
    }
    LOG(LOG_GX, "GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    // Beside the memory card: both are state this machine accumulates for this game.
    plat_make_dirs(save_path(""));
    gx::render_set_shader_cache(save_path("shaders.bin").c_str());
    gx::render_init(cur.scale);
    if (g_eye_mode) eye_init();
    if (first_person) gx::render_set_first_person(true);
    if (gx::g_dump_dir) plat_make_dirs(gx::g_dump_dir);

    const bool audio_ok = audio_open();
    mouse_aim_init();
    apply_settings(win, cur, nullptr);
    // The menus' documents: beside the executable, or in an app's resources.
    const char* res = getenv("GCN_RESOURCES");
    ui_init(win, res ? std::string(res) + "/ui" : plat_executable_dir() + "/ui");
    // A change the player made: applied, and saved over settings.ini's.
    auto commit = [&](const Settings& s) {
        auto take = [&](auto m) { if (s.*m != cur.*m) g_saved.*m = s.*m; };
        take(&Settings::iso), take(&Settings::scale), take(&Settings::widescreen), take(&Settings::interpolate);
        take(&Settings::fullscreen), take(&Settings::textures), take(&Settings::mouse_aim), take(&Settings::rumble);
        take(&Settings::mouse_sens), take(&Settings::volume), take(&Settings::bindings);
        settings_save(settings_path, g_saved);
        apply_settings(win, s, &cur);
        cur = s;
    };
    auto toggle_fullscreen = [&] {
        Settings s = cur;
        s.fullscreen = !s.fullscreen;
        commit(s);
        ui_set_settings(cur);
    };

    if (launcher && ui_ready()) {
        LauncherEnv env;
        env.win = win;
        env.gl = glGetString(GL_RENDERER) ? (const char*)glGetString(GL_RENDERER) : "OpenGL";
        env.audio = audio_ok;
        env.commit = commit;
        env.event = [&](SDL_Event& e) {
            if (e.type == SDL_CONTROLLERDEVICEADDED && !g_ctrl) g_ctrl = SDL_GameControllerOpen(e.cdevice.which);
            else if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F11 && !e.key.repeat) toggle_fullscreen();
        };
        if (cur.iso.empty()) cur.iso = iso_default;   // what it finds by itself, for this run
        if (!launcher_run(env, cur)) plat_exit_now(0);
        iso = cur.iso.c_str();
    } else if (launcher) {
        // No interface to launch from: as a run without the launcher.
        if (!plat_readable(iso) || !game_module_ensure(iso, false)) return 1;
    }
    if (!start_guest()) return 1;
    if (g_mouse_aim && !hidden) SDL_SetRelativeMouseMode(SDL_TRUE);

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
        SDL_SetRelativeMouseMode(SDL_FALSE);
        SDL_SetWindowTitle(win, GCN_GAME_TITLE " (recompiled) - paused");
        ui_show_pause(cur);
        saves_update();
    };
    auto resume = [&] {
        if (!guest_paused()) return;   // still on its way to stopping
        ui_hide();
        paused = false;
        resume_at = {};
        if (g_mouse_aim && !hidden) SDL_SetRelativeMouseMode(SDL_TRUE);
        audio_set_paused(false);
        guest_resume();
        SDL_SetWindowTitle(win, GCN_GAME_TITLE " (recompiled)");
    };
    const char* pause_at = getenv("GCN_PAUSE_AT");

    bool running = true;
    uint32_t frames = 0;
    auto t0 = std::chrono::steady_clock::now();
    std::unique_ptr<gx::Batch> prev;   // the game's previous frame, for --interpolate
    auto prev_arrival = t0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_CONTROLLERDEVICEADDED && !g_ctrl) g_ctrl = SDL_GameControllerOpen(e.cdevice.which);
            else if (paused && ui_event(e)) continue;
            else if ((e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE && !e.key.repeat) ||
                     (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK))
                paused ? resume() : pause();
            else if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F11 && !e.key.repeat)
                toggle_fullscreen();
            else if (paused) continue;
            else if (g_mouse_aim && e.type == SDL_MOUSEMOTION && SDL_GetRelativeMouseMode())
                g_aim.move((float)e.motion.xrel, (float)e.motion.yrel);
            else if (g_mouse_aim && e.type == SDL_MOUSEWHEEL && SDL_GetRelativeMouseMode())
                g_aim.wheel(e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y);
            else if (g_mouse_aim && e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F1)
                SDL_SetRelativeMouseMode(SDL_GetRelativeMouseMode() ? SDL_FALSE : SDL_TRUE);
            else if (g_mouse_aim && e.type == SDL_MOUSEBUTTONDOWN && !SDL_GetRelativeMouseMode())
                SDL_SetRelativeMouseMode(SDL_TRUE);
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
            } else saves_act(a, ui_action_index());
            Settings s = cur;
            if (guest_paused() && ui_take_settings(s)) commit(s);
        }
        update_pad(paused);
        // The game's rumble, to the controller, renewed while it lasts: SDL wants a duration.
        if (g_ctrl) {
            const bool on = cur.rumble && !paused && pad_motor(0);
            static bool was;
            if (on || was) SDL_GameControllerRumble(g_ctrl, on ? 0xC000 : 0, on ? 0xC000 : 0, on ? 200 : 0);
            was = on;
        }
        int dw, dh;
        SDL_GL_GetDrawableSize(win, &dw, &dh);
        gx::render_set_window_size(dw, dh);
        auto b = gx::take_batch(4);
        if (!b) {
            if (paused && guest_paused()) {
                // Nothing more is coming: the last frame again, at about a display's pace.
                if (resume_at != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() >= resume_at)
                    resume();
                else if (gx::render_present_last()) {
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    ui_render(dw, dh);
                    ui_script(dw, dh);
                    SDL_GL_SwapWindow(win);
                }
                SDL_Delay(12);
            }
            continue;
        }
        const auto arrival = std::chrono::steady_clock::now();
        // GCN_STEREOLOG=1: what the game's stereo hook (vr_game.h) would have a headset
        // show, whenever the answer changes, numbered as the frame dumps are.
        static const bool stereolog = getenv("GCN_STEREOLOG") != nullptr;
        if (stereolog && vr::game_hooks().wants_stereo) {
            static int last = -1;
            const int want = vr::game_hooks().wants_stereo(*b) ? 1 : 0;
            if (want != last) {
                last = want;
                fprintf(stderr, "[stereo] frame %u: %s\n", gx::present_count(), want ? "stereo" : "theater");
            }
        }
        bool presented;
        if (g_eye_mode) {
            float P[16], V[16], H[16];
            eye_matrices(P, V, H);
            gx::render_set_vr_eye(P, V, H);
            presented = gx::render_execute_eye(*b, g_eye_fbo, kEyeW, kEyeH, true);
            static uint32_t eye_frames = 0;
            eye_frames++;
            if (gx::g_dump_dir && gx::g_dump_every && eye_frames % gx::g_dump_every == 0) eye_dump(eye_frames);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, g_eye_fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glBlitFramebuffer(0, 0, kEyeW, kEyeH, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        } else {
            if (g_interpolate && prev) {
                // Only a frame that lasts about two refreshes has room for one in between. A
                // backlog arrives with no gap, a 60 fps scene with half of one: both go
                // straight through.
                const auto gap = arrival - prev_arrival;
                const double secs = std::chrono::duration<double>(gap).count();
                if ((g_interpolate == 2 || (secs > 0.025 && secs < 0.050)) &&
                    gx::render_execute_interpolated(*b, *prev)) {
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    ui_render(dw, dh);
                    SDL_GL_SwapWindow(win);
                    frames++;
                    if (g_interpolate == 1) std::this_thread::sleep_until(arrival + gap / 2);
                }
            }
            presented = gx::render_execute(*b);
        }
        if (presented) {
            // macOS (GL on Metal) only presents correctly with the window framebuffer bound.
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            ui_render(dw, dh);
            SDL_GL_SwapWindow(win);
            frames++;
            auto now = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(now - t0).count();
            if (secs >= 2.0 && !paused) {
                char title[128];
                snprintf(title, sizeof(title), GCN_GAME_TITLE " (recompiled) - %.1f fps", frames / secs);
                SDL_SetWindowTitle(win, title);
                frames = 0;
                t0 = now;
            }
            if (pause_at && !paused && gx::present_count() == (uint32_t)atoi(pause_at)) {
                const char* ms = strchr(pause_at, ':');
                pause();
                resume_at = now + std::chrono::milliseconds(ms ? atoi(ms + 1) : 1000);
            }
        }
        if (presented) ram_snapshot(gx::present_count());
        if (g_interpolate) { prev = std::move(b); prev_arrival = arrival; }
    }
    input_log_end();
    threads_request_quit();
    plat_exit_now(0);
}
