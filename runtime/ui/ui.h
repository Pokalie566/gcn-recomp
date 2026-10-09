// The in-game interface: the pause menu and the settings, drawn with RmlUi over the game.
//
// The documents and their style are in runtime/ui/assets, copied beside the executable as
// ui/, with whatever the game project adds or replaces there (gcn_add_game's UI): its own
// theme.rcss, typically. Layout is in dp, one dp being a 720th of the window's height, so
// the menus keep their proportions at any size.
//
// The frontend (main.cpp) owns the pause: it opens the menu when it pauses the game, feeds
// it events while it is open, and takes back what the player asked for. Settings change
// only while the guest is stopped, which is why the frontend asks for them then.
#pragma once
#include "settings.h"
#include <SDL.h>
#include <string>

// False, with the reason printed, if the interface cannot start (no ui/ directory): the
// game then runs without it, Esc still pausing.
bool ui_init(SDL_Window* win, const std::string& dir);

// The pause menu, over the game, showing these settings.
void ui_show_pause(const Settings& s);
// The settings the menus show, changed from elsewhere (F11) while they are open.
void ui_set_settings(const Settings& s);
void ui_hide();
bool ui_open();

// Hands an event to the interface while it is open. True if it took it.
bool ui_event(SDL_Event& e);

// Draws the interface over what is in the window framebuffer, `w` x `h` pixels.
void ui_render(int w, int h);

enum UiAction { UI_NONE, UI_RESUME, UI_QUIT };
// What the player chose since the last call.
UiAction ui_take_action();

// True, with `s` updated, when the player has changed a setting since the last call.
bool ui_take_settings(Settings& s);

// GCN_UI_SHOT=<dir>: each page of the menu written there as a PNG, then the game resumed.
// Called by the frontend once a frame while the menu is open; true while it is at it.
bool ui_shot_tour(int w, int h);

// GCN_UI_INPUT=<key>,<key>...: those keys (SDL names: Down, Return, Escape...) pressed in
// the menu, one every 16 frames, through the event queue -- the menus driven as a player
// would, for checking them. Called by the frontend once a frame while the menu is open.
void ui_script();
