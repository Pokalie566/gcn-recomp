// The player's settings: what settings.ini holds, beside the memory card
// (save_path("settings.ini")), and what the command line overrides for one run.
//
// settings.ini is `key = value` lines, `#` comments. A key it lacks keeps its default, so an
// old file goes on working when settings are added; one it has that nothing knows is ignored.
#pragma once
#include <string>
#include <utility>
#include <vector>

struct Settings {
    std::string iso;            // the disc image; empty: the build's, else the first in rom/
    int scale = 1;              // EFB samples per hardware pixel, per axis: 1 is the console's 640x528
    bool widescreen = false;    // 16:9
    bool interpolate = false;   // a 30 fps game shown at 60
    bool fullscreen = false;
    bool borderless = false;    // the window without its frame
    int window_w = 0, window_h = 0;   // the window's size as the player left it; 0: the default
    bool vsync = true;          // presentation waits for the display, as a television does
    int msaa = 1;               // samples per pixel: 1, 2, 4 or 8; at the next start
    std::string backend = "auto";   // the GPU API: auto, metal, vulkan, d3d12; at the next start
    bool textures = true;       // custom textures, when there is a pack
    bool mouse_aim = false;     // the mouse as the C stick
    bool rumble = true;         // the controller's, when the game runs the pad's motor
    float deadzone = 0.12f;     // how far a controller's stick moves before it counts
    float mouse_sens = 1.0f;
    int volume = 100;           // percent
    std::string lang = "auto";  // the menus': "auto" (the system's), "en" or "fr"; at the next start
    std::vector<std::string> textures_off;   // texture packs turned off, by their folder's name
    bool setup_done = false;    // the first launch's assistant has been through
    // The pad's bindings live with Aurora's (pad_bindings.h); an older settings.ini's
    // bind.<row> lines are read for taking them over, and not written again.
    std::vector<std::pair<std::string, std::string>> old_binds;
    bool operator==(const Settings&) const = default;
};

// Reads `path` over `s`. False, with `s` untouched, if there is no such file.
bool settings_load(const std::string& path, Settings& s);
bool settings_save(const std::string& path, const Settings& s);

// The arguments that are settings -- the disc image, --scale=N, --widescreen, --interpolate,
// --fullscreen, --mouse-aim -- and their GCN_* variables, applied over `s` for this run.
// Other arguments are left to the caller.
void settings_from_args(Settings& s, int argc, char** argv);
