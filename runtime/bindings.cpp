// Pad bindings. See bindings.h.
#include "bindings.h"
#include <algorithm>
#include <cmath>

const char* const kPadActionNames[ACT_COUNT] = {
    "a", "b", "x", "y", "z", "start",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "l", "r",
    "stick_up", "stick_down", "stick_left", "stick_right",
    "c_up", "c_down", "c_left", "c_right",
};

Bindings bindings_default() {
    Bindings b;
    auto set = [&](PadAction a, std::vector<std::string> in) { b.inputs[a] = std::move(in); };
    // The controller's face buttons by position, not by label: GameCube A is the big one.
    set(ACT_A, {"key:X", "pad:a"});
    set(ACT_B, {"key:Z", "pad:x"});
    set(ACT_X, {"key:C", "pad:b"});
    set(ACT_Y, {"key:S", "pad:y"});
    set(ACT_Z, {"key:D", "pad:rightshoulder"});
    set(ACT_START, {"key:Return", "pad:start"});
    set(ACT_DPAD_UP, {"key:I", "pad:dpup"});
    set(ACT_DPAD_DOWN, {"key:K", "pad:dpdown"});
    set(ACT_DPAD_LEFT, {"key:J", "pad:dpleft"});
    set(ACT_DPAD_RIGHT, {"key:L", "pad:dpright"});
    set(ACT_L, {"key:Q", "axis:+lefttrigger"});
    set(ACT_R, {"key:W", "axis:+righttrigger"});
    set(ACT_STICK_UP, {"key:Up", "axis:-lefty"});
    set(ACT_STICK_DOWN, {"key:Down", "axis:+lefty"});
    set(ACT_STICK_LEFT, {"key:Left", "axis:-leftx"});
    set(ACT_STICK_RIGHT, {"key:Right", "axis:+leftx"});
    set(ACT_C_UP, {"axis:-righty"});
    set(ACT_C_DOWN, {"axis:+righty"});
    set(ACT_C_LEFT, {"axis:-rightx"});
    set(ACT_C_RIGHT, {"axis:+rightx"});
    return b;
}

PadState pad_from_actions(const float a[ACT_COUNT]) {
    PadState p;
    p.connected = true;
    static const struct { PadAction act; uint16_t bit; } kButtons[] = {
        {ACT_A, PAD_A}, {ACT_B, PAD_B}, {ACT_X, PAD_X}, {ACT_Y, PAD_Y}, {ACT_Z, PAD_Z},
        {ACT_START, PAD_START}, {ACT_DPAD_UP, PAD_UP}, {ACT_DPAD_DOWN, PAD_DOWN},
        {ACT_DPAD_LEFT, PAD_LEFT}, {ACT_DPAD_RIGHT, PAD_RIGHT},
    };
    for (const auto& b : kButtons)
        if (a[b.act] > 0.5f) p.buttons |= b.bit;
    auto trigger = [&](PadAction act, uint8_t& value, uint16_t bit) {
        const float v = std::clamp(a[act], 0.0f, 1.0f);
        value = (uint8_t)lroundf(255 * v);
        if (v > 0.915f) p.buttons |= bit;
    };
    trigger(ACT_L, p.trig_l, PAD_L);
    trigger(ACT_R, p.trig_r, PAD_R);
    auto axis = [&](PadAction neg, PadAction pos) {
        return (uint8_t)lroundf(128 + 100 * std::clamp(a[pos] - a[neg], -1.0f, 1.0f));
    };
    p.stick_x = axis(ACT_STICK_LEFT, ACT_STICK_RIGHT);
    p.stick_y = axis(ACT_STICK_DOWN, ACT_STICK_UP);
    p.cstick_x = axis(ACT_C_LEFT, ACT_C_RIGHT);
    p.cstick_y = axis(ACT_C_DOWN, ACT_C_UP);
    return p;
}
