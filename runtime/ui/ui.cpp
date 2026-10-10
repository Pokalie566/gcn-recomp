// The interface. See ui.h.
#include "ui/ui.h"
#include "gx/gl.h"
#include "platform.h"
#include <RmlUi/Core.h>
#include <RmlUi_Platform_SDL.h>
#include <RmlUi_Renderer_GL3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

bool write_png(const char* path, const uint8_t* rgba, int w, int h);

namespace {

// One row of the bindings table: an action and what it is bound to on each device.
struct BindRow {
    Rml::String label, key, pad;
};
// A save on the card, and a copy of the card, as the saves page lists them.
struct SaveRow {
    Rml::String comment, when;
};
struct BackupRow {
    Rml::String when, saves;
};

const char* const kActionLabels[ACT_COUNT] = {
    "A", "B", "X", "Y", "Z", "Start",
    "D-pad up", "D-pad down", "D-pad left", "D-pad right",
    "L trigger", "R trigger",
    "Stick up", "Stick down", "Stick left", "Stick right",
    "C-stick up", "C-stick down", "C-stick left", "C-stick right",
};

// RmlUi takes a path that starts with / as relative to the document's directory; the banner
// is in the save directory, an absolute path, which this lets through as it is.
class System : public SystemInterface_SDL {
public:
    using SystemInterface_SDL::SystemInterface_SDL;
    void JoinPath(Rml::String& out, const Rml::String& document, const Rml::String& path) override {
        if (!path.empty() && (path[0] == '/' || (path.size() > 2 && path[1] == ':'))) out = path;
        else SystemInterface_SDL::JoinPath(out, document, path);
    }
};

SDL_Window* g_win;
System* g_system;
RenderInterface_GL3* g_render;
Rml::Context* g_ctx;
Rml::DataModelHandle g_model;
Rml::ElementDocument* g_launcher;
Rml::ElementDocument* g_pause;
Rml::ElementDocument* g_settings;
Rml::ElementDocument* g_saves;
Rml::ElementDocument* g_home;   // where Esc leads back to from settings or saves

Settings g_s;                  // what the pages show, and change
Rml::String g_title = GCN_GAME_TITLE;
int g_tab;                     // the settings page shown: graphics, controls, audio
Rml::Vector<BindRow> g_rows;
int g_capture = -1;            // the row waiting for an input, if any...
bool g_capture_pad;            // ...from the controller rather than the keyboard
UiAction g_action = UI_NONE;
int g_action_index;

UiLauncher g_l;
Rml::String g_progress_width = "0%";
bool g_has_banner;
bool g_in_game;                // the saves page from the pause menu: no restoring under the game
Rml::Vector<SaveRow> g_save_rows;
Rml::Vector<BackupRow> g_backup_rows;
Rml::String g_saves_message;

bool starts(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

void rows_update() {
    g_rows.resize(ACT_COUNT);
    for (int a = 0; a < ACT_COUNT; a++) {
        g_rows[a].label = kActionLabels[a];
        g_rows[a].key = binding_label(g_s.bindings.inputs[a], false);
        g_rows[a].pad = binding_label(g_s.bindings.inputs[a], true);
    }
    if (g_capture >= 0) (g_capture_pad ? g_rows[g_capture].pad : g_rows[g_capture].key) = g_capture_pad ? "Press a button..." : "Press a key...";
    g_model.DirtyVariable("binds");
}

// The input that ends a capture replaces the action's inputs from the same device.
void bind_input(const std::string& input) { binding_replace(g_s.bindings.inputs[g_capture], g_capture_pad, input); }

void capture_end() {
    g_capture = -1;
    rows_update();
}

// While a row waits for an input: the next key, button or stick push is it. Esc or the
// controller's Back cancels, Delete clears the row.
bool capture_event(const SDL_Event& e) {
    if (e.type == SDL_KEYDOWN && !e.key.repeat) {
        const SDL_Scancode sc = e.key.keysym.scancode;
        if (sc == SDL_SCANCODE_ESCAPE) capture_end();
        else if (sc == SDL_SCANCODE_DELETE || sc == SDL_SCANCODE_BACKSPACE) { bind_input(""); capture_end(); }
        else if (!g_capture_pad) { bind_input(std::string("key:") + SDL_GetScancodeName(sc)); capture_end(); }
        return true;
    }
    if (e.type == SDL_CONTROLLERBUTTONDOWN) {
        const auto b = (SDL_GameControllerButton)e.cbutton.button;
        if (b == SDL_CONTROLLER_BUTTON_BACK) capture_end();
        else if (g_capture_pad) { bind_input(std::string("pad:") + SDL_GameControllerGetStringForButton(b)); capture_end(); }
        return true;
    }
    if (e.type == SDL_CONTROLLERAXISMOTION && g_capture_pad && std::abs((int)e.caxis.value) > 24000) {
        const auto a = (SDL_GameControllerAxis)e.caxis.axis;
        bind_input(std::string("axis:") + (e.caxis.value < 0 ? "-" : "+") + SDL_GameControllerGetStringForAxis(a));
        capture_end();
        return true;
    }
    return e.type != SDL_QUIT && e.type != SDL_WINDOWEVENT;
}

// Left and right on a row that holds a value: one step down or up.
void nudge(const Rml::String& var, int dir) {
    if (var == "scale") g_s.scale = std::clamp(g_s.scale + dir, 1, 4);
    else if (var == "volume") g_s.volume = std::clamp(g_s.volume + 5 * dir, 0, 100);
    else if (var == "mouse_sens") g_s.mouse_sens = std::clamp(std::round((g_s.mouse_sens + 0.25f * dir) * 4) / 4, 0.25f, 4.0f);
    g_model.DirtyVariable(var);
}

void show(Rml::ElementDocument* doc) {
    for (Rml::ElementDocument* d : {g_launcher, g_pause, g_settings, g_saves})
        if (d != doc) d->Hide();
    doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::Auto);
}

// Esc, or the controller's B: one page back; from the pause menu, back to the game.
void back() {
    if (g_settings->IsVisible() || g_saves->IsVisible()) show(g_home);
    else if (g_pause->IsVisible()) g_action = UI_RESUME;
}

// The keys that move about the pages and act on them, from a keyboard or a controller.
class Keys : public Rml::EventListener {
public:
    void ProcessEvent(Rml::Event& ev) override {
        Rml::Element* t = ev.GetTargetElement();
        if (ev.GetId() == Rml::EventId::Focus) {
            t->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
            // A tab shows its page as soon as it has the focus, as a console's menus do.
            const Rml::String& id = t->GetId();
            if (id.size() == 4 && starts(id, "tab")) {
                g_tab = id[3] - '0';
                g_model.DirtyVariable("tab");
            }
            return;
        }
        const auto key = (Rml::Input::KeyIdentifier)ev.GetParameter<int>("key_identifier", 0);
        if ((key == Rml::Input::KI_LEFT || key == Rml::Input::KI_RIGHT) && t->HasAttribute("nudge")) {
            nudge(t->GetAttribute<Rml::String>("nudge", ""), key == Rml::Input::KI_LEFT ? -1 : 1);
            ev.StopPropagation();
            return;
        }
        // RmlUi's navigation does not go into a scrolling box or out of one, and each settings
        // page is one: down from the tabs is the page's first item, up from that the tab.
        if (!g_settings->IsVisible()) return;
        Rml::Element* page = g_settings->QuerySelector(".page.shown");
        Rml::Element* first = page ? page->QuerySelector("button") : nullptr;
        Rml::Element* to = nullptr;
        if (key == Rml::Input::KI_DOWN && t->GetParentNode() && t->GetParentNode()->IsClassSet("tabs")) to = first;
        else if (key == Rml::Input::KI_UP && t == first) to = g_settings->GetElementById("tab" + std::to_string(g_tab));
        if (to) {
            to->Focus(true);
            ev.StopPropagation();
        }
    }
} g_keys;

Rml::Input::KeyIdentifier controller_key(uint8_t button) {
    switch (button) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return Rml::Input::KI_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return Rml::Input::KI_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return Rml::Input::KI_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return Rml::Input::KI_RIGHT;
    case SDL_CONTROLLER_BUTTON_A: return Rml::Input::KI_SPACE;   // what clicks the focused element
    default: return Rml::Input::KI_UNKNOWN;
    }
}

void render_frame(int w, int h) {
    g_ctx->SetDimensions({w, h});
    g_ctx->SetDensityIndependentPixelRatio((float)h / 720.0f);
    g_ctx->Update();
    g_render->SetViewport(w, h);
    g_render->BeginFrame();
    g_ctx->Render();
    g_render->EndFrame();
}

void act(UiAction a, int index = 0) {
    g_action = a;
    g_action_index = index;
}

// The card stores seconds since 2000-01-01, in the console's local time.
Rml::String card_time(uint32_t t) {
    const time_t u = (time_t)946684800 + t;
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", gmtime(&u));
    return buf;
}

}  // namespace

bool ui_init(SDL_Window* win, const std::string& dir) {
    if (!plat_readable((dir + "/pause.rml").c_str())) {
        fprintf(stderr, "ui: no interface in %s, so none\n", dir.c_str());
        return false;
    }
    g_win = win;
    g_system = new System(win);
    g_render = new RenderInterface_GL3();
    if (!*g_render) {
        fprintf(stderr, "ui: the GL renderer did not start, so no interface\n");
        return false;
    }
    Rml::SetSystemInterface(g_system);
    Rml::SetRenderInterface(g_render);
    Rml::Initialise();
    for (const char* f : {"fonts/FiraSans-Regular.ttf", "fonts/FiraSans-Bold.ttf"}) Rml::LoadFontFace(dir + "/" + f);
    int w, h;
    SDL_GL_GetDrawableSize(win, &w, &h);
    g_ctx = Rml::CreateContext("ui", {w, h});

    Rml::DataModelConstructor m = g_ctx->CreateDataModel("ui");
    if (auto s = m.RegisterStruct<BindRow>()) {
        s.RegisterMember("label", &BindRow::label);
        s.RegisterMember("key", &BindRow::key);
        s.RegisterMember("pad", &BindRow::pad);
    }
    m.RegisterArray<Rml::Vector<BindRow>>();
    if (auto s = m.RegisterStruct<UiCheck>()) {
        s.RegisterMember("label", &UiCheck::label);
        s.RegisterMember("state", &UiCheck::state);
        s.RegisterMember("detail", &UiCheck::detail);
    }
    m.RegisterArray<std::vector<UiCheck>>();
    if (auto s = m.RegisterStruct<SaveRow>()) {
        s.RegisterMember("comment", &SaveRow::comment);
        s.RegisterMember("when", &SaveRow::when);
    }
    m.RegisterArray<Rml::Vector<SaveRow>>();
    if (auto s = m.RegisterStruct<BackupRow>()) {
        s.RegisterMember("when", &BackupRow::when);
        s.RegisterMember("saves", &BackupRow::saves);
    }
    m.RegisterArray<Rml::Vector<BackupRow>>();

    m.Bind("title", &g_title);
    m.Bind("tab", &g_tab);
    m.Bind("scale", &g_s.scale);
    m.Bind("widescreen", &g_s.widescreen);
    m.Bind("interpolate", &g_s.interpolate);
    m.Bind("fullscreen", &g_s.fullscreen);
    m.Bind("textures", &g_s.textures);
    m.Bind("mouse_aim", &g_s.mouse_aim);
    m.Bind("mouse_sens", &g_s.mouse_sens);
    m.Bind("volume", &g_s.volume);
    m.Bind("binds", &g_rows);
    m.Bind("game_title", &g_l.title);
    m.Bind("description", &g_l.description);
    m.Bind("banner", &g_l.banner);
    m.Bind("has_banner", &g_has_banner);
    m.Bind("checks", &g_l.checks);
    m.Bind("no_disc", &g_l.no_disc);
    m.Bind("can_play", &g_l.can_play);
    m.Bind("can_prepare", &g_l.can_prepare);
    m.Bind("can_install_tools", &g_l.can_install_tools);
    m.Bind("preparing", &g_l.preparing);
    m.Bind("progress_width", &g_progress_width);
    m.Bind("progress_text", &g_l.progress_text);
    m.Bind("in_game", &g_in_game);
    m.Bind("saves", &g_save_rows);
    m.Bind("backups", &g_backup_rows);
    m.Bind("saves_message", &g_saves_message);

    using Args = const Rml::VariantList&;
    using H = Rml::DataModelHandle;
    m.BindEventCallback("resume", [](H, Rml::Event&, Args) { act(UI_RESUME); });
    m.BindEventCallback("quit", [](H, Rml::Event&, Args) { act(UI_QUIT); });
    m.BindEventCallback("play", [](H, Rml::Event&, Args) { if (g_l.can_play) act(UI_PLAY); });
    m.BindEventCallback("prepare", [](H, Rml::Event&, Args) { if (g_l.can_prepare) act(UI_PREPARE); });
    m.BindEventCallback("install_tools", [](H, Rml::Event&, Args) { act(UI_INSTALL_TOOLS); });
    m.BindEventCallback("launcher", [](H, Rml::Event&, Args) { act(UI_LAUNCHER); });
    m.BindEventCallback("backup", [](H, Rml::Event&, Args) { act(UI_BACKUP); });
    m.BindEventCallback("restore", [](H, Rml::Event&, Args a) { if (!g_in_game && !a.empty()) act(UI_RESTORE, a[0].Get<int>()); });
    m.BindEventCallback("open_saves", [](H, Rml::Event&, Args) { act(UI_OPEN_SAVES); });
    m.BindEventCallback("settings", [](H, Rml::Event&, Args) { show(g_settings); });
    m.BindEventCallback("saves", [](H, Rml::Event&, Args) { show(g_saves); });
    m.BindEventCallback("back", [](H, Rml::Event&, Args) { back(); });
    m.BindEventCallback("toggle", [](H h, Rml::Event&, Args a) {
        const Rml::String var = a.empty() ? "" : a[0].Get<Rml::String>();
        for (auto [name, field] : {std::pair{"widescreen", &g_s.widescreen}, {"interpolate", &g_s.interpolate},
                                   {"fullscreen", &g_s.fullscreen}, {"textures", &g_s.textures}, {"mouse_aim", &g_s.mouse_aim}})
            if (var == name) *field = !*field;
        h.DirtyVariable(var);
    });
    m.BindEventCallback("nudge", [](H, Rml::Event&, Args a) {
        if (a.size() == 2) nudge(a[0].Get<Rml::String>(), a[1].Get<int>());
    });
    m.BindEventCallback("tab", [](H h, Rml::Event&, Args a) {
        if (!a.empty()) g_tab = a[0].Get<int>();
        h.DirtyVariable("tab");
    });
    m.BindEventCallback("capture", [](H, Rml::Event&, Args a) {
        if (a.size() != 2) return;
        g_capture = a[0].Get<int>();
        g_capture_pad = a[1].Get<Rml::String>() == "pad";
        rows_update();
    });
    m.BindEventCallback("reset_bindings", [](H, Rml::Event&, Args) {
        g_s.bindings = bindings_default();
        rows_update();
    });
    g_model = m.GetModelHandle();

    g_launcher = g_ctx->LoadDocument(dir + "/launcher.rml");
    g_pause = g_ctx->LoadDocument(dir + "/pause.rml");
    g_settings = g_ctx->LoadDocument(dir + "/settings.rml");
    g_saves = g_ctx->LoadDocument(dir + "/saves.rml");
    if (!g_launcher || !g_pause || !g_settings || !g_saves) {
        fprintf(stderr, "ui: cannot load the documents in %s\n", dir.c_str());
        g_ctx = nullptr;
        return false;
    }
    for (Rml::ElementDocument* d : {g_launcher, g_pause, g_settings, g_saves}) {
        d->AddEventListener(Rml::EventId::Keydown, &g_keys);
        d->AddEventListener(Rml::EventId::Focus, &g_keys, true);   // focus does not bubble
    }
    return true;
}

bool ui_ready() { return g_ctx != nullptr; }

void ui_set_settings(const Settings& s) {
    if (!g_ctx) return;
    g_s = s;
    rows_update();
    g_model.DirtyAllVariables();
}

void ui_show_pause(const Settings& s) {
    if (!g_ctx) return;
    g_tab = 0;
    g_capture = -1;
    g_action = UI_NONE;
    g_in_game = true;
    g_home = g_pause;
    ui_set_settings(s);
    show(g_pause);
}

void ui_show_launcher(const Settings& s, const UiLauncher& l) {
    if (!g_ctx) return;
    g_tab = 0;
    g_capture = -1;
    g_action = UI_NONE;
    g_in_game = false;
    g_home = g_launcher;
    ui_set_settings(s);
    ui_set_launcher(l);
    show(g_launcher);
}

void ui_set_launcher(const UiLauncher& l) {
    if (!g_ctx) return;
    g_l = l;
    g_has_banner = !l.banner.empty();
    g_progress_width = std::to_string((int)std::lround(std::clamp(l.progress, 0.0f, 1.0f) * 100)) + "%";
    g_model.DirtyAllVariables();
}

void ui_set_saves(const UiSaves& s) {
    if (!g_ctx) return;
    g_save_rows.clear();
    for (const CardFile& f : s.files) g_save_rows.push_back({f.comment.empty() ? f.name : f.comment, card_time(f.modified)});
    g_backup_rows.clear();
    for (const CardBackup& b : s.backups) {
        size_t n = 0;
        for (const CardFile& f : card_files(b.path)) n += f.game == GCN_DISC_ID;
        g_backup_rows.push_back({b.when, n == 1 ? "1 save" : std::to_string(n) + " saves"});
    }
    g_saves_message = s.message;
    g_model.DirtyAllVariables();
}

void ui_hide() {
    if (!g_ctx) return;
    for (Rml::ElementDocument* d : {g_launcher, g_pause, g_settings, g_saves}) d->Hide();
}

bool ui_open() {
    return g_ctx && (g_launcher->IsVisible() || g_pause->IsVisible() || g_settings->IsVisible() || g_saves->IsVisible());
}

bool ui_event(SDL_Event& e) {
    if (!ui_open()) return false;
    if (g_capture >= 0) return capture_event(e);
    switch (e.type) {
    case SDL_KEYDOWN:
        if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) { back(); return true; }
        // Enter acts on the focused element, as A does.
        if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) {
            g_ctx->ProcessKeyDown(Rml::Input::KI_SPACE, 0);
            g_ctx->ProcessKeyUp(Rml::Input::KI_SPACE, 0);
            return true;
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) return false;   // the frontend's: resume
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_B) back();
        else if (g_settings->IsVisible() && (e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER ||
                                             e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) {
            g_tab = (g_tab + (e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER ? 2 : 1)) % 3;
            g_model.DirtyVariable("tab");
            if (Rml::Element* t = g_settings->GetElementById("tab" + std::to_string(g_tab))) t->Focus(true);
        } else if (const auto k = controller_key(e.cbutton.button); k != Rml::Input::KI_UNKNOWN) {
            g_ctx->ProcessKeyDown(k, 0);
            g_ctx->ProcessKeyUp(k, 0);
        }
        return true;
    case SDL_MOUSEMOTION: {
        // RmlUi lays out in drawable pixels; SDL2 reports the mouse in window points.
        int ww, wh, dw, dh;
        SDL_GetWindowSize(g_win, &ww, &wh);
        SDL_GL_GetDrawableSize(g_win, &dw, &dh);
        g_ctx->ProcessMouseMove(e.motion.x * dw / std::max(ww, 1), e.motion.y * dh / std::max(wh, 1), 0);
        return true;
    }
    default:
        break;
    }
    RmlSDL::InputEventHandler(g_ctx, g_win, e);
    return e.type != SDL_QUIT && e.type != SDL_WINDOWEVENT && e.type != SDL_DROPFILE;
}

void ui_render(int w, int h) {
    if (ui_open()) render_frame(w, h);
}

UiAction ui_take_action() {
    const UiAction a = g_action;
    g_action = UI_NONE;
    return a;
}

int ui_action_index() { return g_action_index; }

bool ui_take_settings(Settings& s) {
    if (!g_ctx || g_s == s) return false;
    s = g_s;
    return true;
}

void ui_script(int w, int h) {
    static const char* keys = getenv("GCN_UI_INPUT");
    static int frames;
    if (!keys || !*keys || !ui_open() || ++frames % 16) return;
    const char* comma = strchr(keys, ',');
    const std::string name = comma ? std::string(keys, comma) : std::string(keys);
    keys = comma ? comma + 1 : "";
    if (starts(name, "drop:")) {   // a file dropped on the window (see kUiDropEvent)
        SDL_Event e{};
        e.type = SDL_USEREVENT;
        e.user.code = kUiDropEvent;
        e.user.data1 = SDL_strdup(name.c_str() + 5);
        SDL_PushEvent(&e);
        return;
    }
    if (starts(name, "shot:")) {
        const char* dir = getenv("GCN_UI_SHOT");
        if (!dir) return;
        std::vector<uint8_t> px((size_t)w * h * 4), flipped(px.size());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        for (int y = 0; y < h; y++) memcpy(&flipped[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
        plat_make_dirs(dir);
        write_png((std::string(dir) + "/ui_" + name.substr(5) + ".png").c_str(), flipped.data(), w, h);
        return;
    }
    SDL_Event e{};
    e.type = SDL_KEYDOWN;
    e.key.keysym.scancode = SDL_GetScancodeFromName(name.c_str());
    e.key.keysym.sym = SDL_GetKeyFromScancode(e.key.keysym.scancode);
    SDL_PushEvent(&e);
    e.type = SDL_KEYUP;
    SDL_PushEvent(&e);
}
