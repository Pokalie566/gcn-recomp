// Pad bindings on Aurora. See pad_bindings.h.
#include "pad_bindings.h"
#include <dolphin/pad.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <filesystem>
#include <sstream>

const char* const kPadRowNames[kPadRows] = {
    "a", "b", "x", "y", "z", "start",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "l", "r",
    "stick_up", "stick_down", "stick_left", "stick_right",
    "c_up", "c_down", "c_left", "c_right",
};

namespace {

constexpr int kNone = -1;

// What each row drives: a button, an axis, or both (the triggers: a key or a button pulls
// the analog trigger all the way and clicks it).
struct Row {
    int button;   // PAD_BUTTON_* / PAD_TRIGGER_*, or kNone
    int axis;     // PAD_AXIS_*, or kNone
    const char* key;   // the keyboard's default, an SDL scancode name, or nullptr
    const char* old_default;   // the row's bind.* value in an older settings.ini's defaults
};
const Row kRows[kPadRows] = {
    {PAD_BUTTON_A, kNone, "X", "key:X, pad:a"},
    {PAD_BUTTON_B, kNone, "Z", "key:Z, pad:x"},
    {PAD_BUTTON_X, kNone, "C", "key:C, pad:b"},
    {PAD_BUTTON_Y, kNone, "S", "key:S, pad:y"},
    {PAD_TRIGGER_Z, kNone, "D", "key:D, pad:rightshoulder"},
    {PAD_BUTTON_START, kNone, "Return", "key:Return, pad:start"},
    {PAD_BUTTON_UP, kNone, "I", "key:I, pad:dpup"},
    {PAD_BUTTON_DOWN, kNone, "K", "key:K, pad:dpdown"},
    {PAD_BUTTON_LEFT, kNone, "J", "key:J, pad:dpleft"},
    {PAD_BUTTON_RIGHT, kNone, "L", "key:L, pad:dpright"},
    {PAD_TRIGGER_L, PAD_AXIS_TRIGGER_L, "Q", "key:Q, axis:+lefttrigger"},
    {PAD_TRIGGER_R, PAD_AXIS_TRIGGER_R, "W", "key:W, axis:+righttrigger"},
    {kNone, PAD_AXIS_LEFT_Y_POS, "Up", "key:Up, axis:-lefty"},
    {kNone, PAD_AXIS_LEFT_Y_NEG, "Down", "key:Down, axis:+lefty"},
    {kNone, PAD_AXIS_LEFT_X_NEG, "Left", "key:Left, axis:-leftx"},
    {kNone, PAD_AXIS_LEFT_X_POS, "Right", "key:Right, axis:+leftx"},
    {kNone, PAD_AXIS_RIGHT_Y_POS, nullptr, "axis:-righty"},
    {kNone, PAD_AXIS_RIGHT_Y_NEG, nullptr, "axis:+righty"},
    {kNone, PAD_AXIS_RIGHT_X_NEG, nullptr, "axis:-rightx"},
    {kNone, PAD_AXIS_RIGHT_X_POS, nullptr, "axis:+rightx"},
};

bool valid(int row) { return row >= 0 && row < kPadRows; }

PADButtonMapping* button_mapping(int padButton) {
    u32 n = 0;
    PADButtonMapping* m = PADGetButtonMappings(0, &n);
    for (u32 i = 0; m && i < n; i++)
        if (m[i].padButton == padButton) return &m[i];
    return nullptr;
}
PADAxisMapping* axis_mapping(int padAxis) {
    u32 n = 0;
    PADAxisMapping* m = PADGetAxisMappings(0, &n);
    for (u32 i = 0; m && i < n; i++)
        if (m[i].padAxis == padAxis) return &m[i];
    return nullptr;
}
int key_of(const Row& r) {
    u32 n = 0;
    if (r.button != kNone)
        if (PADKeyButtonBinding* b = PADGetKeyButtonBindings(0, &n))
            for (u32 i = 0; i < n; i++)
                if (b[i].padButton == r.button) return b[i].scancode;
    if (r.axis != kNone)
        if (PADKeyAxisBinding* b = PADGetKeyAxisBindings(0, &n))
            for (u32 i = 0; i < n; i++)
                if (b[i].padAxis == r.axis) return b[i].scancode;
    return PAD_KEY_INVALID;
}

void set_key(const Row& r, int scancode) {
    if (r.button != kNone) PADSetKeyButtonBinding(0, {scancode, (PADButton)r.button});
    if (r.axis != kNone) PADSetKeyAxisBinding(0, {scancode, (PADAxis)r.axis, 100});
}

// A controller row's input: a native button (into the button and the axis, for a trigger),
// or one way of a native axis.
void set_pad(const Row& r, int button, int axis, int sign) {
    if (r.button != kNone) PADSetButtonMapping(0, {(u32)(button >= 0 ? button : PAD_NATIVE_BUTTON_INVALID), (PADButton)r.button});
    if (r.axis != kNone)
        PADSetAxisMapping(0, {{axis >= 0 ? axis : SDL_GAMEPAD_AXIS_INVALID, sign < 0 ? AXIS_SIGN_NEGATIVE : AXIS_SIGN_POSITIVE},
                              button >= 0 && axis < 0 ? button : SDL_GAMEPAD_BUTTON_INVALID, (PADAxis)r.axis});
}

void save() { PADSerializeMappings(); }

// An older settings.ini's input names: "key:X", "pad:a", "axis:-lefty".
void apply_old(int row, const std::string& value) {
    const Row& r = kRows[row];
    bool key = false, pad = false;
    std::stringstream ss(value);
    for (std::string in; std::getline(ss, in, ',');) {
        in.erase(0, in.find_first_not_of(' '));
        in.erase(in.find_last_not_of(' ') + 1);
        if (!key && !in.compare(0, 4, "key:")) {
            const SDL_Scancode sc = SDL_GetScancodeFromName(in.c_str() + 4);
            if (sc != SDL_SCANCODE_UNKNOWN) set_key(r, sc), key = true;
        } else if (!pad && !in.compare(0, 4, "pad:")) {
            const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(in.c_str() + 4);
            if (b != SDL_GAMEPAD_BUTTON_INVALID) set_pad(r, b, -1, 1), pad = true;
        } else if (!pad && !in.compare(0, 5, "axis:") && in.size() > 6 && r.axis != kNone) {
            const SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(in.c_str() + 6);
            if (a != SDL_GAMEPAD_AXIS_INVALID) set_pad(r, -1, a, in[5] == '-' ? -1 : 1), pad = true;
        }
    }
    if (!key) set_key(r, PAD_KEY_INVALID);
}

}  // namespace

void pad_bindings_init() {
    PADDefaultKeyBindings keys;
    for (int b = 0; b < PAD_BUTTON_COUNT; b++) keys.buttons[b] = {PAD_KEY_INVALID, 0};
    for (int a = 0; a < PAD_AXIS_COUNT; a++) keys.axes[a] = {PAD_KEY_INVALID, (PADAxis)a, 100};
    int nb = 0;
    for (const Row& r : kRows) {
        const int sc = r.key ? (int)SDL_GetScancodeFromName(r.key) : PAD_KEY_INVALID;
        if (r.button != kNone) keys.buttons[nb++] = {sc, (PADButton)r.button};
        if (r.axis != kNone) keys.axes[r.axis] = {sc, (PADAxis)r.axis, 100};
    }
    PADSetDefaultKeyBindings(0, &keys);
    PADInit();
    PADSetKeyboardActive(0, TRUE);
}

void pad_bindings_migrate(const std::vector<std::pair<std::string, std::string>>& binds, const std::string& aurora_dir) {
    std::error_code ec;
    if (binds.empty() || std::filesystem::exists(std::filesystem::path(aurora_dir) / "keyboard_bindings.dat", ec)) return;
    bool custom = false;
    for (const auto& [name, value] : binds)
        for (int row = 0; row < kPadRows; row++)
            if (name == kPadRowNames[row] && value != kRows[row].old_default) custom = true;
    if (!custom) return;
    for (const auto& [name, value] : binds)
        for (int row = 0; row < kPadRows; row++)
            if (name == kPadRowNames[row]) apply_old(row, value);
    save();
    fprintf(stderr, "settings: the pad bindings of settings.ini taken over\n");
}

std::string pad_binding_label(int row, bool controller) {
    if (!valid(row)) return "-";
    const Row& r = kRows[row];
    if (!controller) {
        const int sc = key_of(r);
        const char* name = sc >= 0 ? SDL_GetScancodeName((SDL_Scancode)sc) : "";
        return name && *name ? name : "-";
    }
    if (r.button != kNone)
        if (const PADButtonMapping* m = button_mapping(r.button); m && m->nativeButton != PAD_NATIVE_BUTTON_INVALID)
            if (const char* n = PADGetNativeButtonName(m->nativeButton)) return n;
    if (r.axis != kNone)
        if (const PADAxisMapping* m = axis_mapping(r.axis)) {
            if (m->nativeAxis.nativeAxis != SDL_GAMEPAD_AXIS_INVALID)
                if (const char* n = PADGetNativeAxisName(m->nativeAxis)) return n;
            if (m->nativeButton != SDL_GAMEPAD_BUTTON_INVALID)
                if (const char* n = PADGetNativeButtonName(m->nativeButton)) return n;
        }
    return "-";
}

bool pad_bind_key(int row, int scancode) {
    if (!valid(row)) return false;
    set_key(kRows[row], scancode);
    save();
    return true;
}

bool pad_bind_button(int row, int button) {
    if (!valid(row)) return false;
    set_pad(kRows[row], button, -1, 1);
    save();
    return true;
}

bool pad_bind_axis(int row, int axis, int sign) {
    if (!valid(row) || kRows[row].axis == kNone) return false;
    set_pad(kRows[row], -1, axis, sign);
    save();
    return true;
}

void pad_bind_clear(int row, bool controller) {
    if (!valid(row)) return;
    if (controller) set_pad(kRows[row], -1, -1, 1);
    else set_key(kRows[row], PAD_KEY_INVALID);
    save();
}

void pad_bindings_reset() {
    PADRestoreDefaultKeyBindings(0);
    PADRestoreDefaultMapping(0);
    save();
}

PadState pad_read() {
    PADStatus st[PAD_CHANMAX];
    PADRead(st);
    PadState p;
    p.connected = st[0].err != PAD_ERR_NO_CONTROLLER;
    p.buttons = st[0].button & 0x1F7F;
    p.stick_x = (uint8_t)(128 + st[0].stickX);
    p.stick_y = (uint8_t)(128 + st[0].stickY);
    p.cstick_x = (uint8_t)(128 + st[0].substickX);
    p.cstick_y = (uint8_t)(128 + st[0].substickY);
    p.trig_l = st[0].triggerLeft;
    p.trig_r = st[0].triggerRight;
    return p;
}

std::string pad_controller_name() {
    const char* n = PADGetIndexForPort(0) >= 0 ? PADGetName(0) : nullptr;
    return n ? n : "";
}

void pad_rumble(bool on) { PADControlMotor(0, on ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP); }

void pad_set_deadzone(float deadzone) {
    if (PADDeadZones* dz = PADGetDeadZones(0)) {
        dz->useDeadzones = true;
        dz->stickDeadZone = dz->substickDeadZone = (u16)std::clamp(deadzone * 32767.0f, 0.0f, 32767.0f);
    }
}
