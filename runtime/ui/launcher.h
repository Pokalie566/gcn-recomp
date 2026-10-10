// The launcher: what the app shows first. It checks what the game needs -- the disc, the
// prepared game and the tools that prepare it, graphics, sound, a controller, the memory
// card, a texture pack -- prepares the game from the disc when it must, and starts it. A
// disc image dropped on the window is the one to play.
//
// The saves page belongs to it and to the pause menu alike: the game's saves on the card,
// and copies of the card, made each time the game starts and on demand, kept in backups/
// beside it.
#pragma once
#include "hw/pad.h"
#include "settings.h"
#include <SDL3/SDL.h>
#include <functional>
#include <string>

struct LauncherEnv {
    SDL_Window* win = nullptr;
    std::string gpu;                               // the graphics API and device in use
    bool audio = false;                            // whether the sound device opened
    std::function<void(const Settings&)> commit;   // a change of settings: applied and saved
    std::function<void(SDL_Event&)> event;         // the frontend's own events: controllers, F11
    std::function<PadState()> pad;                 // the pad as the player holds it, for the test
    std::function<std::string()> pad_name;
    std::function<void()> rumble;                  // the controller's, half a second
};

// Runs until the player plays -- true, with the game's module loaded and `cur.iso` its disc
// -- or quits.
bool launcher_run(LauncherEnv& env, Settings& cur);

// The saves page brought up to date; `message` says what was just done.
void saves_update(const std::string& message = "");
// The saves page's actions (UI_BACKUP, UI_RESTORE with its index, UI_OPEN_SAVES). False for
// any other.
bool saves_act(int action, int index);
// The copy of the card made as the game starts.
void saves_backup_on_start();

// The settings' Disc and Textures pages brought up to date: the disc chosen -- in game, for
// the next start -- and the packs in the textures folder.
void settings_pages_update(const Settings& cur, bool in_game);
