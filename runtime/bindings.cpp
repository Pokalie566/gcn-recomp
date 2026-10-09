// Pad bindings. See bindings.h.
#include "bindings.h"
#include <algorithm>
#include <cmath>
#include <cstring>

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

static bool starts(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

static std::string button_name(const std::string& b) {
    static const struct { const char* sdl; const char* name; } kNames[] = {
        {"a", "A"}, {"b", "B"}, {"x", "X"}, {"y", "Y"}, {"back", "Back"}, {"guide", "Guide"},
        {"start", "Start"}, {"leftstick", "L3"}, {"rightstick", "R3"}, {"leftshoulder", "LB"},
        {"rightshoulder", "RB"}, {"dpup", "D-pad up"}, {"dpdown", "D-pad down"},
        {"dpleft", "D-pad left"}, {"dpright", "D-pad right"},
    };
    for (const auto& n : kNames)
        if (b == n.sdl) return n.name;
    return b;
}

static std::string axis_name(const std::string& in) {   // "axis:-lefty"
    const bool neg = in[5] == '-';
    const std::string a = in.substr(6);
    if (a == "lefttrigger") return "LT";
    if (a == "righttrigger") return "RT";
    const bool x = a.back() == 'x';
    return std::string(starts(a, "left") ? "Left stick " : "Right stick ") +
           (x ? (neg ? "left" : "right") : (neg ? "up" : "down"));
}

std::string binding_label(const std::vector<std::string>& inputs, bool controller) {
    for (const std::string& in : inputs) {
        if (!controller && starts(in, "key:")) return in.substr(4);
        if (controller && starts(in, "pad:")) return button_name(in.substr(4));
        if (controller && starts(in, "axis:") && in.size() > 6) return axis_name(in);
    }
    return "-";
}

void binding_replace(std::vector<std::string>& inputs, bool controller, const std::string& input) {
    inputs.erase(std::remove_if(inputs.begin(), inputs.end(),
                                [&](const std::string& in) { return starts(in, "key:") != controller; }),
                 inputs.end());
    if (!input.empty()) inputs.push_back(input);
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
