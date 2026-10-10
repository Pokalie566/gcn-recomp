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
#include <fstream>
#include <unordered_map>
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
struct PackRow {
    Rml::String name, count;
    bool on;
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

std::unordered_map<std::string, std::string> g_strings;   // English -> the language's

// RmlUi takes a path that starts with / as relative to the document's directory; the banner
// is in the save directory, an absolute path, which this lets through as it is. And every
// text goes through the language's strings, its surrounding spaces kept.
class System : public SystemInterface_SDL {
public:
    using SystemInterface_SDL::SystemInterface_SDL;
    int TranslateString(Rml::String& out, const Rml::String& in) override {
        const size_t a = in.find_first_not_of(" \t\r\n"), b = in.find_last_not_of(" \t\r\n");
        if (a != std::string::npos) {
            auto it = g_strings.find(in.substr(a, b - a + 1));
            if (it != g_strings.end()) {
                out = in.substr(0, a) + it->second + in.substr(b + 1);
                return 1;
            }
        }
        out = in;
        return 0;
    }
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

Rml::ElementDocument* g_setup;
int g_setup_step;
bool g_setup_can_next;
bool g_refocus;                // after the next update, focus what the page now shows

Rml::String g_pad_name, g_stick_x = "50%", g_stick_y = "50%", g_cstick_x = "50%", g_cstick_y = "50%";
Rml::String g_trig_l = "0%", g_trig_r = "0%";
bool g_pad[10];                // A B X Y Z Start up down left right
const char* const kPadVars[10] = {"pad_a", "pad_b", "pad_x", "pad_y", "pad_z", "pad_start", "pad_up", "pad_down", "pad_left", "pad_right"};
PadState g_pad_last;

std::vector<UiPack> g_pack_list;
Rml::Vector<PackRow> g_packs;
Rml::String g_packs_summary;
Rml::String g_disc_name, g_disc_state, g_disc_detail;

void packs_update();
void setup_update();

bool starts(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

void rows_update() {
    g_rows.resize(ACT_COUNT);
    for (int a = 0; a < ACT_COUNT; a++) {
        g_rows[a].label = kActionLabels[a];
        g_rows[a].key = binding_label(g_s.bindings.inputs[a], false);
        g_rows[a].pad = binding_label(g_s.bindings.inputs[a], true);
    }
    if (g_capture >= 0)
        (g_capture_pad ? g_rows[g_capture].pad : g_rows[g_capture].key) = ui_tr(g_capture_pad ? "Press a button..." : "Press a key...");
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

Rml::String g_lang_label;
const char* const kLangs[][2] = {{"auto", "Automatic"}, {"en", "English"}, {"fr", "Français"}};

void lang_label_update() {
    g_lang_label = g_s.lang;
    for (const auto& l : kLangs)
        if (g_s.lang == l[0]) g_lang_label = ui_tr(l[1]);
    g_model.DirtyVariable("lang_label");
}

// Left and right on a row that holds a value: one step down or up.
void nudge(const Rml::String& var, int dir) {
    if (var == "lang") {
        int i = 0;
        while (i < 3 && g_s.lang != kLangs[i][0]) i++;
        g_s.lang = kLangs[((i % 3) + dir + 3) % 3][0];
        lang_label_update();
    }
    if (var == "scale") g_s.scale = std::clamp(g_s.scale + dir, 1, 4);
    else if (var == "volume") g_s.volume = std::clamp(g_s.volume + 5 * dir, 0, 100);
    else if (var == "mouse_sens") g_s.mouse_sens = std::clamp(std::round((g_s.mouse_sens + 0.25f * dir) * 4) / 4, 0.25f, 4.0f);
    else if (var == "deadzone") g_s.deadzone = std::clamp(std::round(g_s.deadzone * 100 + dir) / 100, 0.02f, 0.40f);
    g_model.DirtyVariable(var);
}

void show(Rml::ElementDocument* doc) {
    for (Rml::ElementDocument* d : {g_launcher, g_pause, g_settings, g_saves, g_setup})
        if (d != doc) d->Hide();
    doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::Auto);
}

// Esc, or the controller's B: one page back; from the pause menu, back to the game.
void back() {
    if (g_setup->IsVisible()) {
        if (g_setup_step > 0 && g_setup_step < 6) {
            g_setup_step--;
            setup_update();
        }
        return;
    }
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
    if (g_refocus) {
        // The page's elements are the ones the last change made: the assistant's step.
        g_refocus = false;
        // Next, or on the last step, which has none, its first choice.
        Rml::Element* to = g_setup_step == 6 ? g_setup->QuerySelector(".step.shown button")
                                             : g_setup->GetElementById("setup_next");
        if (to) to->Focus(true);
    }
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


// The language to show: the setting, or for "auto" the first of the system's that has strings.
std::string pick_language(const std::string& setting, const std::string& dir) {
    if (setting != "auto") return setting;
    if (SDL_Locale* locales = SDL_GetPreferredLocales()) {
        std::string found = "en";
        for (SDL_Locale* l = locales; l->language; l++)
            if (!strcmp(l->language, "en") || plat_readable((dir + "/lang/" + l->language + ".txt").c_str())) {
                found = l->language;
                break;
            }
        SDL_free(locales);
        return found;
    }
    return "en";
}

void load_strings(const std::string& path) {
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        const size_t eq = line.find(" = ");
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        g_strings[line.substr(0, eq)] = line.substr(eq + 3);
    }
}

// Whether the assistant may go on from its step: not without a disc that is the game, nor
// before the game is prepared from it.
void setup_update() {
    g_setup_can_next = g_setup_step == 1 ? g_l.disc_ok : g_setup_step == 2 ? g_l.can_play : true;
    g_model.DirtyVariable("setup_step");
    g_model.DirtyVariable("setup_can_next");
    g_refocus = true;
}

void packs_update() {
    g_packs.clear();
    size_t total = 0;
    for (const UiPack& p : g_pack_list) {
        const bool on = std::find(g_s.textures_off.begin(), g_s.textures_off.end(), p.name) == g_s.textures_off.end();
        g_packs.push_back({p.name.empty() ? ui_tr("Loose files") : p.name, p.textures == 1 ? ui_tr("1 texture") : ui_trf("{} textures", std::to_string(p.textures)), on});
        if (on) total += p.textures;
    }
    g_packs_summary = g_pack_list.empty() ? ui_tr("No pack yet.")
                                          : ui_trf("{} textures in use.", std::to_string(total));
    g_model.DirtyVariable("packs");
    g_model.DirtyVariable("packs_summary");
}

}  // namespace

std::string ui_tr(const std::string& english) {
    auto it = g_strings.find(english);
    return it == g_strings.end() ? english : it->second;
}

std::string ui_trf(const std::string& english, const std::string& arg) {
    std::string s = ui_tr(english);
    if (const size_t at = s.find("{}"); at != std::string::npos) s.replace(at, 2, arg);
    return s;
}

bool ui_init(SDL_Window* win, const std::string& dir, const std::string& lang) {
    if (!plat_readable((dir + "/pause.rml").c_str())) {
        fprintf(stderr, "ui: no interface in %s, so none\n", dir.c_str());
        return false;
    }
    if (const std::string l = pick_language(lang, dir); l != "en") load_strings(dir + "/lang/" + l + ".txt");
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
    m.Bind("rumble", &g_s.rumble);
    m.Bind("mouse_sens", &g_s.mouse_sens);
    m.Bind("volume", &g_s.volume);
    m.Bind("lang_label", &g_lang_label);
    m.Bind("binds", &g_rows);
    m.Bind("game_title", &g_l.title);
    m.Bind("description", &g_l.description);
    m.Bind("banner", &g_l.banner);
    m.Bind("has_banner", &g_has_banner);
    m.Bind("checks", &g_l.checks);
    m.Bind("no_disc", &g_l.no_disc);
    m.Bind("can_play", &g_l.can_play);
    m.Bind("can_prepare", &g_l.can_prepare);
    m.Bind("preparing", &g_l.preparing);
    m.Bind("progress_width", &g_progress_width);
    m.Bind("progress_text", &g_l.progress_text);
    m.Bind("in_game", &g_in_game);
    m.Bind("saves", &g_save_rows);
    m.Bind("backups", &g_backup_rows);
    m.Bind("saves_message", &g_saves_message);
    m.Bind("deadzone", &g_s.deadzone);
    if (auto s = m.RegisterStruct<PackRow>()) {
        s.RegisterMember("name", &PackRow::name);
        s.RegisterMember("count", &PackRow::count);
        s.RegisterMember("on", &PackRow::on);
    }
    m.RegisterArray<Rml::Vector<PackRow>>();
    m.Bind("packs", &g_packs);
    m.Bind("packs_summary", &g_packs_summary);
    m.Bind("pad_name", &g_pad_name);
    for (int i = 0; i < 10; i++) m.Bind(kPadVars[i], &g_pad[i]);
    m.Bind("stick_x", &g_stick_x);
    m.Bind("stick_y", &g_stick_y);
    m.Bind("cstick_x", &g_cstick_x);
    m.Bind("cstick_y", &g_cstick_y);
    m.Bind("trig_l", &g_trig_l);
    m.Bind("trig_r", &g_trig_r);
    m.Bind("disc_name", &g_disc_name);
    m.Bind("disc_state", &g_disc_state);
    m.Bind("disc_detail", &g_disc_detail);
    m.Bind("disc_ok", &g_l.disc_ok);
    m.Bind("setup_step", &g_setup_step);
    m.Bind("setup_can_next", &g_setup_can_next);

    using Args = const Rml::VariantList&;
    using H = Rml::DataModelHandle;
    m.BindEventCallback("resume", [](H, Rml::Event&, Args) { act(UI_RESUME); });
    m.BindEventCallback("quit", [](H, Rml::Event&, Args) { act(UI_QUIT); });
    m.BindEventCallback("play", [](H, Rml::Event&, Args) { if (g_l.can_play) act(UI_PLAY); });
    m.BindEventCallback("prepare", [](H, Rml::Event&, Args) { if (g_l.can_prepare) act(UI_PREPARE); });
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
                                   {"fullscreen", &g_s.fullscreen}, {"textures", &g_s.textures}, {"mouse_aim", &g_s.mouse_aim},
                                   {"rumble", &g_s.rumble}})
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
    m.BindEventCallback("pick_disc", [](H, Rml::Event&, Args) { act(UI_PICK_DISC); });
    m.BindEventCallback("prepare_again", [](H, Rml::Event&, Args) { act(UI_PREPARE_AGAIN); });
    m.BindEventCallback("test_rumble", [](H, Rml::Event&, Args) { act(UI_TEST_RUMBLE); });
    m.BindEventCallback("open_textures", [](H, Rml::Event&, Args) { act(UI_OPEN_TEXTURES); });
    m.BindEventCallback("setup_again", [](H, Rml::Event&, Args) { act(UI_SETUP_AGAIN); });
    m.BindEventCallback("setup_done", [](H, Rml::Event&, Args a) { act(UI_SETUP_DONE, a.empty() ? 0 : a[0].Get<int>()); });
    m.BindEventCallback("setup_step_by", [](H, Rml::Event&, Args a) {
        const int d = a.empty() ? 1 : a[0].Get<int>();
        if (d > 0 && !g_setup_can_next) return;
        g_setup_step = std::clamp(g_setup_step + d, 0, 6);
        setup_update();
    });
    m.BindEventCallback("toggle_pack", [](H, Rml::Event&, Args a) {
        const int i = a.empty() ? -1 : a[0].Get<int>();
        if (i < 0 || i >= (int)g_pack_list.size()) return;
        auto& off = g_s.textures_off;
        const auto it = std::find(off.begin(), off.end(), g_pack_list[i].name);
        if (it == off.end()) off.push_back(g_pack_list[i].name);
        else off.erase(it);
        packs_update();
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
    g_setup = g_ctx->LoadDocument(dir + "/setup.rml");
    if (!g_launcher || !g_pause || !g_settings || !g_saves || !g_setup) {
        fprintf(stderr, "ui: cannot load the documents in %s\n", dir.c_str());
        g_ctx = nullptr;
        return false;
    }
    for (Rml::ElementDocument* d : {g_launcher, g_pause, g_settings, g_saves, g_setup}) {
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
    lang_label_update();
    packs_update();
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
    if (g_setup->IsVisible()) setup_update();
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
        g_backup_rows.push_back({b.when, n == 1 ? ui_tr("1 save") : ui_trf("{} saves", std::to_string(n))});
    }
    g_saves_message = s.message;
    g_model.DirtyAllVariables();
}

void ui_hide() {
    if (!g_ctx) return;
    for (Rml::ElementDocument* d : {g_launcher, g_pause, g_settings, g_saves, g_setup}) d->Hide();
}

bool ui_open() {
    return g_ctx && (g_launcher->IsVisible() || g_pause->IsVisible() || g_settings->IsVisible() ||
                     g_saves->IsVisible() || g_setup->IsVisible());
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
            g_tab = (g_tab + (e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER ? 5 : 1)) % 6;
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

void ui_show_setup(const Settings& s, const UiLauncher& l) {
    if (!g_ctx) return;
    g_capture = -1;
    g_action = UI_NONE;
    g_in_game = false;
    g_home = g_launcher;
    g_setup_step = 0;
    ui_set_settings(s);
    ui_set_launcher(l);
    show(g_setup);
    setup_update();
}

void ui_set_pad(const PadState& p, const std::string& controller) {
    if (!g_ctx) return;
    if (g_pad_name != controller) {
        g_pad_name = controller;
        g_model.DirtyVariable("pad_name");
    }
    if (!memcmp(&p, &g_pad_last, sizeof(p))) return;
    g_pad_last = p;
    static const uint16_t bits[10] = {PAD_A, PAD_B, PAD_X, PAD_Y, PAD_Z, PAD_START, PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT};
    for (int i = 0; i < 10; i++)
        if (g_pad[i] != ((p.buttons & bits[i]) != 0)) {
            g_pad[i] = !g_pad[i];
            g_model.DirtyVariable(kPadVars[i]);
        }
    // A stick's 28..228 across its circle, up at the top.
    auto pct = [](int v) { return std::to_string(std::clamp((v - 28) * 100 / 200, 0, 100)) + "%"; };
    g_stick_x = pct(p.stick_x);
    g_stick_y = pct(256 - p.stick_y);
    g_cstick_x = pct(p.cstick_x);
    g_cstick_y = pct(256 - p.cstick_y);
    g_trig_l = std::to_string(p.trig_l * 100 / 255) + "%";
    g_trig_r = std::to_string(p.trig_r * 100 / 255) + "%";
    for (const char* v : {"stick_x", "stick_y", "cstick_x", "cstick_y", "trig_l", "trig_r"}) g_model.DirtyVariable(v);
}

void ui_set_packs(const std::vector<UiPack>& packs) {
    if (!g_ctx) return;
    g_pack_list = packs;
    packs_update();
}

void ui_set_disc(const std::string& name, const std::string& state, const std::string& detail) {
    if (!g_ctx) return;
    g_disc_name = name;
    g_disc_state = state;
    g_disc_detail = detail;
    for (const char* v : {"disc_name", "disc_state", "disc_detail"}) g_model.DirtyVariable(v);
}

bool ui_setup_open() { return g_ctx && g_setup->IsVisible(); }
