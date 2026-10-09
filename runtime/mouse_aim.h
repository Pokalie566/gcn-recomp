// The mouse as a C stick (--mouse-aim), for a game that turns its camera or its player's aim
// at a rate set by that stick. Pure: the frontend feeds it SDL's relative motion, and it is
// tested without a window.
//
// The stick follows the mouse's speed, smoothed over a few tens of milliseconds. Turning
// motion into deflection when the guest polls, instead, would lose some of it: the pad is
// polled every video field and a 30 fps game reads it every other one, so whatever moved
// in between would be consumed and never seen. A speed is there whenever the game looks.
//
// Not every game wants the vertical motion: a stick's y can do something a mouse should
// not do on every twitch -- in Luigi's Mansion it turns the player to face the camera before
// tilting the light. With `use_y` off, vertical motion is ignored and the wheel moves the
// stick's y instead, a short push per notch.
//
// Most games ignore a small deflection; a mouse that moves at all must clear that dead zone
// or slow, fine aiming does nothing. So the deflection is the dead zone plus the speed, and
// the game's own response above its dead zone -- often linear up to a saturation -- is what
// the sensitivity is set against.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdlib>

struct MouseAim {
    float sens = 1.0f;          // GCN_MOUSE_SENS: 2 turns twice as fast for the same motion
    float deadzone = 0.15f;     // GCN_MOUSE_DEADZONE: the deflection the game starts to react at
    bool invert_y = false;      // GCN_MOUSE_INVERT_Y: mouse up (or wheel up) pushes the stick down
    bool use_y = true;          // GCN_MOUSE_Y: vertical motion drives the stick's y

    // Relative motion since the last update, in window points (SDL's xrel/yrel).
    void move(float dx, float dy) { acc_x += dx; acc_y += dy; }

    // Wheel notches since the last update, positive away from the user (SDL's wheel.y).
    void wheel(int notches) {
        if (!notches) return;
        const int dir = notches > 0 ? 1 : -1;
        if (dir != wheel_dir) wheel_left = 0;
        wheel_dir = dir;
        wheel_left = std::min(kWheelMax, wheel_left + kWheelHold * (float)std::abs(notches));
    }

    // Once per frontend iteration, `dt` seconds after the previous one.
    void update(float dt) {
        if (dt <= 0) return;
        wheel_left = std::max(0.0f, wheel_left - dt);
        const float a = 1.0f - std::exp(-dt / kSmoothing);
        vx += a * (acc_x / dt - vx);
        vy += a * (acc_y / dt - vy);
        acc_x = acc_y = 0;
    }

    // The stick, -1..1 on each axis (up positive, as on the pad); 0 when the mouse is still.
    float stick_x() const { return axis(vx); }
    float stick_y() const {
        if (wheel_left > 0) return (invert_y ? -kWheelDeflection : kWheelDeflection) * (float)wheel_dir;
        if (!use_y) return 0;
        return axis(invert_y ? vy : -vy);  // window y grows downwards
    }

private:
    static constexpr float kSmoothing = 0.035f;  // seconds
    static constexpr float kFull = 2400.0f;      // points/s that would take the stick past its end
    static constexpr float kWheelHold = 0.1f;    // seconds the stick is held per notch
    static constexpr float kWheelMax = 0.5f;     // however fast the wheel spins
    static constexpr float kWheelDeflection = 0.5f;
    float axis(float v) const {
        const float d = std::fabs(v) * sens / kFull;
        if (d < 0.002f) return 0;
        return std::copysign(std::min(1.0f, deadzone + d), v);
    }
    float vx = 0, vy = 0;          // smoothed speed, points per second
    float acc_x = 0, acc_y = 0;
    float wheel_left = 0;          // seconds the wheel still holds the stick
    int wheel_dir = 0;
};
