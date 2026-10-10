// The player's settings: what settings.ini holds, beside the memory card
// (save_path("settings.ini")), and what the command line overrides for one run.
//
// settings.ini is `key = value` lines, `#` comments. A key it lacks keeps its default, so an
// old file goes on working when settings are added; one it has that nothing knows is ignored.
#pragma once
#include "bindings.h"
#include <string>

struct Settings {
    std::string iso;            // the disc image; empty: the build's, else the first in rom/
    int scale = 2;              // EFB samples per hardware pixel, per axis
    bool widescreen = false;    // 16:9
    bool interpolate = false;   // a 30 fps game shown at 60
    bool fullscreen = false;
    bool textures = true;       // custom textures, when there is a pack
    bool mouse_aim = false;     // the mouse as the C stick
    bool rumble = true;         // the controller's, when the game runs the pad's motor
    float mouse_sens = 1.0f;
    int volume = 100;           // percent
    Bindings bindings = bindings_default();
    bool operator==(const Settings&) const = default;
};

// Reads `path` over `s`. False, with `s` untouched, if there is no such file.
bool settings_load(const std::string& path, Settings& s);
bool settings_save(const std::string& path, const Settings& s);

// The arguments that are settings -- the disc image, --scale=N, --widescreen, --interpolate,
// --fullscreen, --mouse-aim -- and their GCN_* variables, applied over `s` for this run.
// Other arguments are left to the caller.
void settings_from_args(Settings& s, int argc, char** argv);
