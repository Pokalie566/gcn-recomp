// The interface: the launcher, the pause menu, the settings and the saves, drawn with RmlUi.
//
// The documents and their style are in runtime/ui/assets, copied beside the executable as
// ui/, with whatever the game project adds or replaces there (gcn_add_game's UI): its own
// theme.rcss, typically. Layout is in dp, one dp being a 720th of the window's height, so
// the pages keep their proportions at any size.
//
// The frontend owns what happens: the launcher (launcher.h) and the pause (main.cpp) open
// their page, feed it events while it is open, and take back what the player asked for.
// Settings change only while the guest is stopped (or before it starts), which is why the
// frontend asks for them then.
#pragma once
#include "hw/pad.h"
#include "saves.h"
#include "settings.h"
#include <SDL3/SDL.h>
#include <string>
#include <vector>

// False, with the reason printed, if the interface cannot start (no ui/ directory): the
// game then runs without it, Esc still pausing. `lang` is the setting: "auto" for the
// system's language, else "en" or "fr".
bool ui_init(SDL_Window* win, const std::string& dir, const std::string& lang);

// The interface's language. Its pages are written in English, and ui/lang/<lang>.txt holds
// `English = translation` lines, which RmlUi applies to every text, the bound ones included,
// by exact match. ui_tr translates a sentence the program puts together itself; one with a
// part that varies has {} for it, filled in by ui_trf.
std::string ui_tr(const std::string& english);
std::string ui_trf(const std::string& english, const std::string& arg);
bool ui_ready();

// The pause menu, over the game, showing these settings.
void ui_show_pause(const Settings& s);
// The settings the pages show, changed from elsewhere (F11) while they are open.
void ui_set_settings(const Settings& s);
void ui_hide();
bool ui_open();

// ---- The launcher ----
struct UiCheck {
    std::string label;
    std::string state;   // "ok", "warn", "fail" or "busy"
    std::string detail;
};
struct UiLauncher {
    std::string title = GCN_GAME_TITLE;
    std::string description;   // from the disc's banner
    std::string banner;        // the banner as an image file, or empty
    std::vector<UiCheck> checks;
    bool no_disc = true;       // the hint to drop one on the window shows
    bool disc_ok = false;      // the disc is the game, in the version this port is for
    bool can_play = false, can_prepare = false;
    bool preparing = false;
    float progress = 0;        // 0..1
    std::string progress_text;
};
void ui_show_launcher(const Settings& s, const UiLauncher& l);
void ui_set_launcher(const UiLauncher& l);

// The first launch's assistant: disc, preparation, controller, display, texture packs.
void ui_show_setup(const Settings& s, const UiLauncher& l);
bool ui_setup_open();

// What the settings pages show that is not a setting. The pad as the game would read it,
// for the controller test (cheap: call it every frame); the texture packs found; the disc.
void ui_set_pad(const PadState& p, const std::string& controller);
struct UiPack {
    std::string name;
    size_t textures;
};
void ui_set_packs(const std::vector<UiPack>& packs);
void ui_set_disc(const std::string& name, const std::string& state, const std::string& detail);

// ---- The saves page: what is on the card, and the copies of it ----
struct UiSaves {
    std::vector<CardFile> files;      // the game's own, on the card
    std::vector<CardBackup> backups;
    std::string message;              // what the last action did
};
void ui_set_saves(const UiSaves& s);

// Hands an event to the interface while it is open. True if it took it.
bool ui_event(SDL_Event& e);

// Once a frame, before it is presented: Aurora draws the interface as it presents.
void ui_frame();

enum UiAction {
    UI_NONE, UI_RESUME, UI_QUIT,
    UI_PLAY, UI_PREPARE,   // the launcher's
    UI_LAUNCHER,                             // the pause menu's: back to the launcher
    UI_BACKUP, UI_RESTORE, UI_OPEN_SAVES,    // the saves page's; UI_RESTORE with ui_action_index
    UI_PICK_DISC, UI_PREPARE_AGAIN,          // the Disc settings page's
    UI_TEST_RUMBLE, UI_OPEN_TEXTURES,        // the Controller and Textures pages'
    UI_SETUP_DONE, UI_SETUP_AGAIN,           // the assistant's end (index 1: play), and running it again
};
// What the player chose since the last call.
UiAction ui_take_action();
// The backup UI_RESTORE is about, as an index into the last UiSaves' backups.
int ui_action_index();

// True, with `s` updated, when the player has changed a setting since the last call.
bool ui_take_settings(Settings& s);

// GCN_UI_INPUT=<key>,<key>...: those keys (SDL names: Down, Return, Escape...) pressed in
// the interface, one every 16 frames, through the event queue -- the pages driven as a
// player would, for checking them. "shot:<name>" writes the window as it is to
// GCN_UI_SHOT/ui_<name>.png, and "drop:<path>" drops that file on the
// window. Called by the frontend once a frame while the interface is open and nothing is busy.
void ui_script();

// A dropped file, as GCN_UI_INPUT sends one: an SDL_EVENT_USER with this code and the path,
// SDL_strdup'd, in data1.
constexpr int kUiDropEvent = 0x44524F50;
