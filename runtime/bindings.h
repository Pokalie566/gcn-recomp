// What each part of the GameCube pad is bound to, as a player sets it.
//
// An input is named, so that settings.ini stays readable and the names mean the same thing
// on every machine: "key:X" is a key by its SDL scancode name, "pad:a" a controller button by
// its SDL name, "axis:+lefty" / "axis:-lefty" one half of a controller axis. An action may
// have several; the strongest one pressed wins. The frontend reads them (main.cpp); nothing
// here depends on SDL.
#pragma once
#include "hw/pad.h"
#include <string>
#include <vector>

enum PadAction {
    ACT_A, ACT_B, ACT_X, ACT_Y, ACT_Z, ACT_START,
    ACT_DPAD_UP, ACT_DPAD_DOWN, ACT_DPAD_LEFT, ACT_DPAD_RIGHT,
    ACT_L, ACT_R,
    ACT_STICK_UP, ACT_STICK_DOWN, ACT_STICK_LEFT, ACT_STICK_RIGHT,
    ACT_C_UP, ACT_C_DOWN, ACT_C_LEFT, ACT_C_RIGHT,
    ACT_COUNT
};

// Each action's name in settings.ini (bind.<name>): "a", "dpad_up", "stick_left"...
extern const char* const kPadActionNames[ACT_COUNT];

struct Bindings {
    std::vector<std::string> inputs[ACT_COUNT];
    bool operator==(const Bindings&) const = default;
};

// Keyboard and controller, laid out like a GameCube pad.
Bindings bindings_default();

// The pad, given how far each action is pushed, 0 to 1. The triggers are analog, their
// button clicking in at the end of the travel, as on the real pad; the sticks go 100 either
// side of centre.
PadState pad_from_actions(const float amount[ACT_COUNT]);
