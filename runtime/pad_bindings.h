// The pad as the player binds it, on Aurora's pad (dolphin/pad.h): Aurora opens the
// controllers, reads them through its mappings, one per controller model, and saves those
// and the keyboard's in its own files (in the save folder's aurora/). The Controls page
// shows a row per part of the GameCube pad -- a button, or one way of a stick -- with one
// key and one controller input each.
//
// The keyboard starts laid out as it always was here (X Z C S D Return, I J K L for the
// d-pad, Q W for the triggers, the arrows for the stick); a controller starts with Aurora's
// mapping for its kind. Bindings made in an older settings.ini (bind.<row> = ...) are taken
// over once.
#pragma once
#include "hw/pad.h"
#include <string>
#include <utility>
#include <vector>

constexpr int kPadRows = 20;
// Each row's name, as an older settings.ini had it: "a", "dpad_up", "stick_left"...
extern const char* const kPadRowNames[kPadRows];

// Before the guest reads the pad: the keyboard's defaults, then Aurora's pad started.
void pad_bindings_init();
// The bind.* lines of an older settings.ini, applied unless Aurora already has bindings of
// its own (in `aurora_dir`) or they are the old defaults.
void pad_bindings_migrate(const std::vector<std::pair<std::string, std::string>>& binds, const std::string& aurora_dir);

// What the Controls page shows for a row: its key, or its controller input ("A", "Left
// stick up"...); "-" if none.
std::string pad_binding_label(int row, bool controller);
// The row bound to a key (an SDL scancode), a controller button (SDL_GamepadButton) or one
// way of an axis (SDL_GamepadAxis, sign -1 or 1); -1 clears the row on that device. False
// if the row cannot take that input (an axis for a plain button). Saved at once.
bool pad_bind_key(int row, int scancode);
bool pad_bind_button(int row, int button);
bool pad_bind_axis(int row, int axis, int sign);
void pad_bind_clear(int row, bool controller);
void pad_bindings_reset();

// The first port as the game would read it.
PadState pad_read();
// The controller on the first port, by its name, or "" if none.
std::string pad_controller_name();
void pad_rumble(bool on);
// How far a stick moves before it counts, 0 to 1.
void pad_set_deadzone(float deadzone);
