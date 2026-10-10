// The player's settings. See settings.h.
#include "settings.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

static std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static bool to_bool(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }

bool settings_load(const std::string& path, Settings& s) {
    std::ifstream in(path);
    if (!in) return false;
    Settings r = s;
    std::string line;
    while (std::getline(in, line)) {
        const size_t hash = line.find('#'), eq = line.find('=');
        if (hash != std::string::npos) line.erase(hash);
        if (eq == std::string::npos || eq >= line.size()) continue;
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "iso") r.iso = v;
        else if (k == "scale") r.scale = atoi(v.c_str());
        else if (k == "widescreen") r.widescreen = to_bool(v);
        else if (k == "interpolate") r.interpolate = to_bool(v);
        else if (k == "fullscreen") r.fullscreen = to_bool(v);
        else if (k == "borderless") r.borderless = to_bool(v);
        else if (k == "window_w") r.window_w = atoi(v.c_str());
        else if (k == "window_h") r.window_h = atoi(v.c_str());
        else if (k == "vsync") r.vsync = to_bool(v);
        else if (k == "msaa") r.msaa = atoi(v.c_str());
        else if (k == "backend") r.backend = v;
        else if (k == "textures") r.textures = to_bool(v);
        else if (k == "mouse_aim") r.mouse_aim = to_bool(v);
        else if (k == "rumble") r.rumble = to_bool(v);
        else if (k == "mouse_sens") r.mouse_sens = (float)atof(v.c_str());
        else if (k == "volume") r.volume = atoi(v.c_str());
        else if (k == "lang") r.lang = v;
        else if (k == "deadzone") r.deadzone = (float)atof(v.c_str());
        else if (k == "setup_done") r.setup_done = to_bool(v);
        else if (k == "textures_off") {
            r.textures_off.clear();
            std::stringstream ss(v);
            for (std::string item; std::getline(ss, item, ',');)
                if (!(item = trim(item)).empty()) r.textures_off.push_back(item);
        }
        else if (k.compare(0, 5, "bind.") == 0) {
            for (int a = 0; a < ACT_COUNT; a++) {
                if (k.compare(5, std::string::npos, kPadActionNames[a]) != 0) continue;
                auto& list = r.bindings.inputs[a];
                list.clear();
                std::stringstream ss(v);
                for (std::string item; std::getline(ss, item, ',');)
                    if (!(item = trim(item)).empty()) list.push_back(item);
            }
        }
    }
    s = r;
    return true;
}

bool settings_save(const std::string& path, const Settings& s) {
    std::ofstream out(path);
    out << "# Settings, read at startup; the pause menu (Esc) changes them.\n"
           "# Options on the command line override them for one run.\n"
        << "iso = " << s.iso << "\n"
        << "scale = " << s.scale << "\n"
        << "widescreen = " << s.widescreen << "\n"
        << "interpolate = " << s.interpolate << "\n"
        << "fullscreen = " << s.fullscreen << "\n"
        << "borderless = " << s.borderless << "\n"
        << "window_w = " << s.window_w << "\n"
        << "window_h = " << s.window_h << "\n"
        << "vsync = " << s.vsync << "\n"
        << "msaa = " << s.msaa << "\n"
        << "backend = " << s.backend << "\n"
        << "textures = " << s.textures << "\n"
        << "mouse_aim = " << s.mouse_aim << "\n"
        << "rumble = " << s.rumble << "\n"
        << "mouse_sens = " << s.mouse_sens << "\n"
        << "volume = " << s.volume << "\n"
        << "lang = " << s.lang << "\n"
        << "deadzone = " << s.deadzone << "\n"
        << "setup_done = " << s.setup_done << "\n"
        << "textures_off = ";
    for (size_t i = 0; i < s.textures_off.size(); i++) out << (i ? ", " : "") << s.textures_off[i];
    out << "\n"
        << "\n# Pad bindings: key:<key>, pad:<button>, axis:+<axis> or axis:-<axis>, comma-separated.\n";
    for (int a = 0; a < ACT_COUNT; a++) {
        out << "bind." << kPadActionNames[a] << " =";
        for (size_t i = 0; i < s.bindings.inputs[a].size(); i++)
            out << (i ? ", " : " ") << s.bindings.inputs[a][i];
        out << "\n";
    }
    return (bool)out;
}

void settings_from_args(Settings& s, int argc, char** argv) {
    if (const char* e = getenv("GCN_WIDESCREEN")) s.widescreen = atoi(e) != 0;
    if (const char* e = getenv("GCN_INTERPOLATE")) s.interpolate = !strcmp(e, "force") || atoi(e) != 0;
    if (const char* e = getenv("GCN_MOUSE_AIM")) s.mouse_aim = atoi(e) != 0;
    if (const char* e = getenv("GCN_MOUSE_SENS")) s.mouse_sens = (float)atof(e);
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (a[0] != '-') s.iso = a;
        else if (!strncmp(a, "--scale=", 8)) s.scale = atoi(a + 8);
        else if (!strcmp(a, "--widescreen")) s.widescreen = true;
        else if (!strcmp(a, "--interpolate")) s.interpolate = true;
        else if (!strcmp(a, "--fullscreen")) s.fullscreen = true;
        else if (!strcmp(a, "--mouse-aim")) s.mouse_aim = true;
    }
}
