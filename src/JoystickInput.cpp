// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "JoystickInput.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#else
#include <GLFW/glfw3.h>
#endif

#include <algorithm>
#include <cmath>

JoystickInput::JoystickInput() = default;

void JoystickInput::applySquareGate(float& x, float& y)
{
    // Circle→square gate. A modern analog stick is limited by a ROUND gate:
    // a full diagonal only reaches (~0.707, ~0.707), so both paddles top out
    // near 217/255 at once and the extreme corners are physically
    // unreachable. The original Apple II stick rode in a SQUARE gate, so the
    // four corners (full X *and* full Y together) were reachable — which
    // games like Wings of Fury need to take off.
    //
    // Scale the vector out along its own ray until its largest component
    // hits the square edge: s = mag / max(|x|,|y|). This maps the inscribed
    // unit circle onto the full unit square (45° full-tilt → (1,1)) while
    // leaving the pure-axis directions untouched (there s = 1). Partial
    // tilts scale proportionally, so the L∞ norm of the result equals the
    // L2 norm of the input — a faithful model of a stick whose travel is
    // bounded by a square, not a disc.
    const float m = std::max(std::fabs(x), std::fabs(y));
    if (m <= 1e-4f) return;                       // centered: nothing to do
    const float mag = std::sqrt(x * x + y * y);
    const float s   = mag / m;
    x *= s;
    y *= s;
    // A stick reporting slightly outside the unit circle (mag > 1) would
    // push a component past 1.0; clamp so the paddle mapping stays in range.
    if (x < -1.0f) x = -1.0f; else if (x > 1.0f) x = 1.0f;
    if (y < -1.0f) y = -1.0f; else if (y > 1.0f) y = 1.0f;
}

uint8_t JoystickInput::axisToPaddle01(float axis)
{
    // A NaN axis (broken driver / hotplug glitch) slides through every
    // ordered comparison below and hits static_cast<uint8_t>(NaN) — UB.
    // Treat it as centered.
    if (std::isnan(axis)) return 128;
    // Map [-1, +1] → [0, 255]. The +0.5 round-to-nearest puts 0.0 on 128
    // (center, matching paddleValue()'s 128 fallback) and +1.0 on 255
    // cleanly. Clamp guards against callers passing slightly out-of-range
    // values.
    float v = (axis + 1.0f) * 127.5f;
    if (v < 0.0f)   v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    return static_cast<uint8_t>(v + 0.5f);
}

void JoystickInput::poll()
{
#ifdef __EMSCRIPTEN__
    emscripten_sample_gamepad_data();
    const int browserPads = emscripten_get_num_gamepads();
    for (int h = 0; h < kHostCount; ++h) {
        DeviceState& d = devices[h];
        d.present = false;
        d.name.clear();
        d.axis.fill(0.0f);
        d.buttons.fill(false);

        if (browserPads <= 0 || h >= browserPads) continue;

        EmscriptenGamepadEvent gp{};
        if (emscripten_get_gamepad_status(h, &gp) != EMSCRIPTEN_RESULT_SUCCESS ||
            !gp.connected) {
            continue;
        }

        d.present = true;
        d.name = gp.id[0] ? gp.id : "Browser Gamepad";
        for (int i = 0; i < kAxes; ++i) {
            d.axis[i] = (i < gp.numAxes) ? static_cast<float>(gp.axis[i]) : 0.0f;
        }
        for (int i = 0; i < kButtons; ++i) {
            d.buttons[i] = (i < gp.numButtons) &&
                           (gp.digitalButton[i] || gp.analogButton[i] >= 0.5);
        }
    }
#else
    for (int h = 0; h < kHostCount; ++h) {
        const int jid = GLFW_JOYSTICK_1 + h;
        DeviceState& d = devices[h];
        if (!glfwJoystickPresent(jid)) {
            d.present = false;
            d.name.clear();
            d.axis.fill(0.0f);
            d.buttons.fill(false);
            continue;
        }
        d.present = true;
        if (const char* name = glfwGetJoystickName(jid)) d.name = name;
        else d.name = "Unknown";

        int axisCount = 0;
        const float* axes = glfwGetJoystickAxes(jid, &axisCount);
        for (int i = 0; i < kAxes; ++i) {
            d.axis[i] = (i < 2 && axes && i < axisCount) ? axes[i] : 0.0f;
        }
        // Raw axes 2/3 can be triggers. Only a standard mapping identifies
        // the right stick reliably; unmapped sticks stay centered.
        GLFWgamepadstate mapped{};
        if (glfwGetGamepadState(jid, &mapped)) {
            d.axis = {mapped.axes[GLFW_GAMEPAD_AXIS_LEFT_X],
                      mapped.axes[GLFW_GAMEPAD_AXIS_LEFT_Y],
                      mapped.axes[GLFW_GAMEPAD_AXIS_RIGHT_X],
                      mapped.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y]};
        }

        int btnCount = 0;
        const unsigned char* btns = glfwGetJoystickButtons(jid, &btnCount);
        for (int i = 0; i < kButtons; ++i) {
            d.buttons[i] = (btns && i < btnCount && btns[i] == GLFW_PRESS);
        }
    }

    // ── UI-navigation edges for the on-screen (kiosk) disk selector ──────
    // Read the *standard* gamepad mapping of the bound pad so Start / A / B /
    // D-pad land on fixed physical buttons regardless of the raw index order.
    // These physical buttons are deliberately NOT the 3 Apple game-port
    // buttons, so menu navigation never bleeds into the running game.
    nav_ = UiNav{};
    activeIsGamepad_ = false;
    const int navJid = GLFW_JOYSTICK_1 + active.hostIdx;
    GLFWgamepadstate gs{};
    if (active.hostIdx >= 0 && active.hostIdx < kHostCount &&
        glfwJoystickIsGamepad(navJid) &&
        glfwGetGamepadState(navJid, &gs)) {

        activeIsGamepad_ = true;

        auto edge = [&](int b) -> bool {
            // Require history: on the first poll after a (re)bind or a
            // transient glfwGetGamepadState failure, a button already held
            // must NOT read as a fresh press (Start held across a rebind
            // would pop the kiosk menu open). Costs at most one legitimately
            // new press in that single frame.
            return gs.buttons[b] == GLFW_PRESS &&
                   navPrevValid_ && navPrevButtons_[b] == GLFW_RELEASE;
        };

        // Left stick folded into a virtual D-pad (up/left = negative on the
        // GLFW/SDL layout), edge-detected against the previous direction so a
        // held stick steps once per push, not once per frame.
        const float sy = gs.axes[GLFW_GAMEPAD_AXIS_LEFT_Y];
        const float sx = gs.axes[GLFW_GAMEPAD_AXIS_LEFT_X];
        const int curStickY = sy < -0.5f ? -1 : (sy > 0.5f ? 1 : 0);
        const int curStickX = sx < -0.5f ? -1 : (sx > 0.5f ? 1 : 0);
        const bool stickUpEdge    = curStickY == -1 && navPrevStickY_ != -1;
        const bool stickDownEdge  = curStickY ==  1 && navPrevStickY_ !=  1;
        const bool stickLeftEdge  = curStickX == -1 && navPrevStickX_ != -1;
        const bool stickRightEdge = curStickX ==  1 && navPrevStickX_ !=  1;

        // Level (held) state — the D-pad button OR the stick past its gate.
        auto held = [&](int b) { return gs.buttons[b] == GLFW_PRESS; };

        nav_.menu     = edge(GLFW_GAMEPAD_BUTTON_START);
        nav_.select   = edge(GLFW_GAMEPAD_BUTTON_BACK);
        nav_.confirm  = edge(GLFW_GAMEPAD_BUTTON_A);
        nav_.cancel   = edge(GLFW_GAMEPAD_BUTTON_B);
        nav_.up       = edge(GLFW_GAMEPAD_BUTTON_DPAD_UP)    || stickUpEdge;
        nav_.down     = edge(GLFW_GAMEPAD_BUTTON_DPAD_DOWN)  || stickDownEdge;
        nav_.left     = edge(GLFW_GAMEPAD_BUTTON_DPAD_LEFT)  || stickLeftEdge;
        nav_.right    = edge(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT) || stickRightEdge;
        nav_.pageUp   = edge(GLFW_GAMEPAD_BUTTON_LEFT_BUMPER);
        nav_.pageDown = edge(GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER);

        nav_.upHeld       = held(GLFW_GAMEPAD_BUTTON_DPAD_UP)    || curStickY == -1;
        nav_.downHeld     = held(GLFW_GAMEPAD_BUTTON_DPAD_DOWN)  || curStickY ==  1;
        nav_.leftHeld     = held(GLFW_GAMEPAD_BUTTON_DPAD_LEFT)  || curStickX == -1;
        nav_.rightHeld    = held(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT) || curStickX ==  1;
        nav_.pageUpHeld   = held(GLFW_GAMEPAD_BUTTON_LEFT_BUMPER);
        nav_.pageDownHeld = held(GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER);

        // In-game routing (menu-gated by MainWindow). The D-pad here is the
        // *button* d-pad ONLY — the analog stick stays the Apple II paddles,
        // so we never fold stickX/Y in as we do for nav_.
        play_.valid     = true;
        play_.button0   = held(GLFW_GAMEPAD_BUTTON_B);          // Circle → PB0
        play_.button1   = held(GLFW_GAMEPAD_BUTTON_A);          // Cross  → PB1
        play_.dpadUp    = held(GLFW_GAMEPAD_BUTTON_DPAD_UP);
        play_.dpadDown  = held(GLFW_GAMEPAD_BUTTON_DPAD_DOWN);
        play_.dpadLeft  = held(GLFW_GAMEPAD_BUTTON_DPAD_LEFT);
        play_.dpadRight = held(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT);
        play_.spaceEdge = edge(GLFW_GAMEPAD_BUTTON_X);          // Square   → SPACE
        play_.enterEdge = edge(GLFW_GAMEPAD_BUTTON_Y);          // Triangle → RETURN

        for (int i = 0; i < 15; ++i) navPrevButtons_[i] = gs.buttons[i];
        navPrevStickY_ = curStickY;
        navPrevStickX_ = curStickX;
        navPrevValid_  = true;
    } else {
        // Not a mapped gamepad (or unbound): drop edge history so a later
        // re-bind starts clean instead of firing a spurious edge. Raw pads
        // fall back to buttonDown(0/1/2) → PB0/1/2 (no arrows/keys, since the
        // physical layout is unknown).
        play_ = GamepadPlay{};
        navPrevValid_  = false;
        navPrevStickY_ = 0;
        navPrevStickX_ = 0;
    }
#endif
}

void JoystickInput::autoBindIfUnconfigured()
{
#ifdef __EMSCRIPTEN__
    if (active.hostIdx >= 0 && active.hostIdx < kHostCount &&
        devices[active.hostIdx].present) {
        return;
    }
    for (int h = 0; h < kHostCount; ++h) {
        if (devices[h].present) {
            active.hostIdx = h;
            autoBindDone = true;
            return;
        }
    }
    active.hostIdx = -1;
#else
    if (autoBindDone) return;
    if (active.hostIdx >= 0) return;
    for (int h = 0; h < kHostCount; ++h) {
        if (devices[h].present) {
            active.hostIdx = h;
            autoBindDone = true;
            return;
        }
    }
#endif
}

void JoystickInput::stickToPaddles(float x, float y, const Binding& b,
                                   uint8_t& padX, uint8_t& padY)
{
    // The deadzone and square-gate steps couple the two axes, so a single
    // axis can't be resolved in isolation — always transform the pair.
    if (b.invert[0]) x = -x;
    if (b.invert[1]) y = -y;
    if (std::isnan(x)) x = 0.0f;
    if (std::isnan(y)) y = 0.0f;

    const float dz  = std::min(std::max(b.deadzone, 0.0f), 0.9f);
    const float mag = std::sqrt(x * x + y * y);
    if (mag < dz || mag <= 0.0f) {
        x = 0.0f;
        y = 0.0f;
    } else {
        // Radial deadzone, RESCALED: kill stick drift by magnitude (a
        // per-axis deadzone would notch the diagonals), and remap
        // [dz..1] → [0..1] along the ray so the paddle reading is
        // continuous across the engage threshold — a hard cutoff stepped
        // ~12 counts (128 → ~140 on a diagonal at dz=0.10) the instant the
        // stick left the dead zone. Full deflection still lands exactly on
        // 1.0, so the gate's corner guarantee is untouched.
        const float scale = (mag - dz) / ((1.0f - dz) * mag);
        x *= scale;
        y *= scale;
        // Axis-snap notch: during a strong single-axis push, zero the other
        // axis while it sits inside the same dz *proportion* of the big one
        // (5 % Y drift at full X used to read PDL(1)≈134 and creep the
        // game). The threshold scales with the dominant component, so
        // diagonals — where both components are comparable — are never
        // notched and the square-gate corners stay reachable.
        const float ax = std::fabs(x), ay = std::fabs(y);
        if      (ay < dz * ax) y = 0.0f;
        else if (ax < dz * ay) x = 0.0f;
        if (b.squareGate) applySquareGate(x, y);
    }

    padX = axisToPaddle01(x);
    padY = axisToPaddle01(y);
}

uint8_t JoystickInput::paddleValue(int paddleIdx) const
{
    if (active.hostIdx < 0 || active.hostIdx >= kHostCount) return 128;
    return paddleValue(devices[active.hostIdx], active, paddleIdx);
}

uint8_t JoystickInput::paddleValue(const DeviceState& d, const Binding& b, int paddleIdx)
{
    if (!d.present || paddleIdx < 0 || paddleIdx >= kAxes) return 128;
    const int firstAxis = paddleIdx & ~1;
    Binding pair = b;
    pair.invert[0] = b.invert[firstAxis];
    pair.invert[1] = b.invert[firstAxis + 1];
    uint8_t px = 128, py = 128;
    stickToPaddles(d.axis[firstAxis], d.axis[firstAxis + 1], pair, px, py);
    return (paddleIdx & 1) == 0 ? px : py;
}

bool JoystickInput::buttonDown(int buttonIdx) const
{
    if (buttonIdx < 0 || buttonIdx >= kButtons) return false;
    if (active.hostIdx < 0 || active.hostIdx >= kHostCount) return false;
    const DeviceState& d = devices[active.hostIdx];
    if (!d.present) return false;
    return d.buttons[buttonIdx];
}

bool JoystickInput::anyPresent() const
{
    for (const auto& d : devices) if (d.present) return true;
    return false;
}
