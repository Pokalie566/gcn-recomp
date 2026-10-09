// The in-game interface. See ui.h.
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
#include <vector>

bool write_png(const char* path, const uint8_t* rgba, int w, int h);

namespace {

// One row of the bindings table: an action and what it is bound to on each device.
struct BindRow {
    Rml::String label, key, pad;
};

const char* const kActionLabels[ACT_COUNT] = {
    "A", "B", "X", "Y", "Z", "Start",
    "D-pad up", "D-pad down", "D-pad left", "D-pad right",
    "L trigger", "R trigger",
    "Stick up", "Stick down", "Stick left", "Stick right",
    "C-stick up", "C-stick down", "C-stick left", "C-stick right",
};

SDL_Window* g_win;
SystemInterface_SDL* g_system;
RenderInterface_GL3* g_render;
Rml::Context* g_ctx;
Rml::DataModelHandle g_model;
Rml::ElementDocument* g_pause;
Rml::ElementDocument* g_settings;

Settings g_s;                  // what the menus show, and change
Rml::String g_title = GCN_GAME_TITLE;
int g_tab;                     // the settings page shown: graphics, controls, audio
Rml::Vector<BindRow> g_rows;
int g_capture = -1;            // the row waiting for an input, if any...
bool g_capture_pad;            // ...from the controller rather than the keyboard
UiAction g_action = UI_NONE;

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
    for (Rml::ElementDocument* d : {g_pause, g_settings})
        if (d != doc) d->Hide();
    doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::Auto);
}

// Esc, or the controller's B: one page back, and from the pause menu back to the game.
void back() {
    if (g_settings->IsVisible()) show(g_pause);
    else g_action = UI_RESUME;
}

// The keys that move about the menus and act on them, from a keyboard or a controller.
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

}  // namespace

bool ui_init(SDL_Window* win, const std::string& dir) {
    if (!plat_readable((dir + "/pause.rml").c_str())) {
        fprintf(stderr, "ui: no interface in %s, so none\n", dir.c_str());
        return false;
    }
    g_win = win;
    g_system = new SystemInterface_SDL(win);
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
    if (auto row = m.RegisterStruct<BindRow>()) {
        row.RegisterMember("label", &BindRow::label);
        row.RegisterMember("key", &BindRow::key);
        row.RegisterMember("pad", &BindRow::pad);
    }
    m.RegisterArray<Rml::Vector<BindRow>>();
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
    using Args = const Rml::VariantList&;
    m.BindEventCallback("resume", [](Rml::DataModelHandle, Rml::Event&, Args) { g_action = UI_RESUME; });
    m.BindEventCallback("quit", [](Rml::DataModelHandle, Rml::Event&, Args) { g_action = UI_QUIT; });
    m.BindEventCallback("settings", [](Rml::DataModelHandle, Rml::Event&, Args) { show(g_settings); });
    m.BindEventCallback("back", [](Rml::DataModelHandle, Rml::Event&, Args) { back(); });
    m.BindEventCallback("toggle", [](Rml::DataModelHandle h, Rml::Event&, Args a) {
        const Rml::String var = a.empty() ? "" : a[0].Get<Rml::String>();
        for (auto [name, field] : {std::pair{"widescreen", &g_s.widescreen}, {"interpolate", &g_s.interpolate},
                                   {"fullscreen", &g_s.fullscreen}, {"textures", &g_s.textures}, {"mouse_aim", &g_s.mouse_aim}})
            if (var == name) *field = !*field;
        h.DirtyVariable(var);
    });
    m.BindEventCallback("nudge", [](Rml::DataModelHandle, Rml::Event&, Args a) {
        if (a.size() == 2) nudge(a[0].Get<Rml::String>(), a[1].Get<int>());
    });
    m.BindEventCallback("tab", [](Rml::DataModelHandle h, Rml::Event&, Args a) {
        if (!a.empty()) g_tab = a[0].Get<int>();
        h.DirtyVariable("tab");
    });
    m.BindEventCallback("capture", [](Rml::DataModelHandle, Rml::Event&, Args a) {
        if (a.size() != 2) return;
        g_capture = a[0].Get<int>();
        g_capture_pad = a[1].Get<Rml::String>() == "pad";
        rows_update();
    });
    m.BindEventCallback("reset_bindings", [](Rml::DataModelHandle, Rml::Event&, Args) {
        g_s.bindings = bindings_default();
        rows_update();
    });
    g_model = m.GetModelHandle();

    g_pause = g_ctx->LoadDocument(dir + "/pause.rml");
    g_settings = g_ctx->LoadDocument(dir + "/settings.rml");
    if (!g_pause || !g_settings) {
        fprintf(stderr, "ui: cannot load the documents in %s\n", dir.c_str());
        g_ctx = nullptr;
        return false;
    }
    for (Rml::ElementDocument* d : {g_pause, g_settings}) {
        d->AddEventListener(Rml::EventId::Keydown, &g_keys);
        d->AddEventListener(Rml::EventId::Focus, &g_keys, true);   // focus does not bubble
    }
    return true;
}

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
    ui_set_settings(s);
    show(g_pause);
}

void ui_hide() {
    if (!g_ctx) return;
    g_pause->Hide();
    g_settings->Hide();
}

bool ui_open() { return g_ctx && (g_pause->IsVisible() || g_settings->IsVisible()); }

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
        else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
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
    return e.type != SDL_QUIT && e.type != SDL_WINDOWEVENT;
}

void ui_render(int w, int h) {
    if (ui_open()) render_frame(w, h);
}

UiAction ui_take_action() {
    const UiAction a = g_action;
    g_action = UI_NONE;
    return a;
}

bool ui_take_settings(Settings& s) {
    if (!g_ctx || g_s == s) return false;
    s = g_s;
    return true;
}

void ui_script() {
    static const char* keys = getenv("GCN_UI_INPUT");
    static int frames;
    if (!keys || !*keys || !ui_open() || ++frames % 16) return;
    const char* comma = strchr(keys, ',');
    const std::string name = comma ? std::string(keys, comma) : std::string(keys);
    keys = comma ? comma + 1 : "";
    SDL_Event e{};
    e.type = SDL_KEYDOWN;
    e.key.keysym.scancode = SDL_GetScancodeFromName(name.c_str());
    e.key.keysym.sym = SDL_GetKeyFromScancode(e.key.keysym.scancode);
    SDL_PushEvent(&e);
    e.type = SDL_KEYUP;
    SDL_PushEvent(&e);
}

bool ui_shot_tour(int w, int h) {
    static const char* dir = getenv("GCN_UI_SHOT");
    static int step, frames;
    if (!dir || !ui_open() || step > 4) return false;
    if (++frames % 16) return true;   // each page settled first: its transitions run their course
    // Pause menu, then each settings page, then a capture waiting for its key.
    static const char* kNames[] = {"pause", "graphics", "controls", "audio", "capture"};
    std::vector<uint8_t> px((size_t)w * h * 4), flipped(px.size());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < h; y++) memcpy(&flipped[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
    plat_make_dirs(dir);
    write_png((std::string(dir) + "/ui_" + kNames[step] + ".png").c_str(), flipped.data(), w, h);
    switch (++step) {
    case 1: show(g_settings); break;
    case 2: case 3: g_tab = step - 1; g_model.DirtyVariable("tab"); break;
    case 4: g_tab = 1; g_capture = 0; g_capture_pad = false; g_model.DirtyVariable("tab"); rows_update(); break;
    default: g_capture = -1; g_action = UI_RESUME; break;
    }
    return true;
}
