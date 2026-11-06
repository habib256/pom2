// POM2 — SPDX-License-Identifier: GPL-3.0-or-later
// Production polling and paddle routing, with explicit host-frame durations.
#include "JoystickInput.h"
#include <GLFW/glfw3.h>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>

namespace {
using J = JoystickInput;
std::array<GLFWgamepadstate, 2> pads{};
std::array<bool, 2> connected{true, true};
bool present(int jid) { return jid >= 0 && jid < 2 && connected[jid]; }
void tick(J& input, double seconds, unsigned mask = 0xF) {
    input.poll(); input.advancePaddles(seconds, mask);
}
void frames(J& input, int count, double seconds, unsigned mask = 0xF) {
    for (int i = 0; i < count; ++i) tick(input, seconds, mask);
}
}
extern "C" {
int glfwJoystickPresent(int jid) { return present(jid); }
const char* glfwGetJoystickName(int) { return "Simulated controller"; }
const float* glfwGetJoystickAxes(int jid, int* count) {
    *count = present(jid) ? 6 : 0;
    return present(jid) ? pads[jid].axes : nullptr;
}
const unsigned char* glfwGetJoystickButtons(int, int* count) { *count = 0; return nullptr; }
int glfwJoystickIsGamepad(int jid) { return present(jid); }
int glfwGetGamepadState(int jid, GLFWgamepadstate* state) {
    if (!present(jid)) return GLFW_FALSE;
    *state = pads[jid]; return GLFW_TRUE;
}
}

int main()
{
    J slow, fast;
    for (J* input : {&slow, &fast}) {
        input->binding().mode[0] = J::Velocity;
        input->binding().deadzone = 0;
        tick(*input, 0); // initialise identity without movement
    }
    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_X] = 0.25f;
    frames(slow, 30, 1.0 / 30);
    frames(fast, 144, 1.0 / 144);
    assert(slow.paddleValue(0) == 192 && fast.paddleValue(0) == 192);
    // Reading the panel/output repeatedly never integrates another frame.
    for (int i = 0; i < 100; ++i) assert(slow.paddleValue(0) == 192);
    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_X] = 0;
    frames(slow, 60, 1.0 / 60);
    assert(slow.paddleValue(0) == 192); // releasing holds position
    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_X] = 1;
    frames(slow, 60, 1.0 / 60, 0); // paused/menu/focus lost
    assert(slow.paddleValue(0) == 192);
    tick(slow, 2); // long stall cannot catch up
    tick(slow, -1);
    tick(slow, std::numeric_limits<double>::quiet_NaN());
    assert(slow.paddleValue(0) == 192);
    slow.centrePaddles(1);
    assert(slow.paddleValue(0) == 128);
    frames(slow, 60, 1.0 / 60);
    assert(slow.paddleValue(0) == 255);
    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_X] = -1;
    frames(slow, 120, 1.0 / 60);
    assert(slow.paddleValue(0) == 0);

    auto& b = slow.binding();
    b.deadzone = 0.2f;
    slow.centrePaddles();
    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_X] = 0.1f;
    frames(slow, 60, 1.0 / 60);
    assert(slow.paddleValue(0) == 128); // drift inside the deadzone
    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_X] = 1;
    b.velocitySpeed[0] = 0.25f;
    frames(slow, 60, 1.0 / 60);
    assert(slow.paddleValue(0) == 192); // per-axis sensitivity
    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_X] = std::numeric_limits<float>::infinity();
    tick(slow, 0.05);
    assert(slow.paddleValue(0) == 192);
    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_X] = std::numeric_limits<float>::quiet_NaN();
    tick(slow, 0.05);
    assert(slow.paddleValue(0) == 192);
    connected[0] = false;
    tick(slow, 0.05);
    assert(slow.paddleValue(0) == 128);
    connected[0] = true;
    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_X] = 1;
    tick(slow, 0.05);
    assert(slow.paddleValue(0) == 128); // no old position or elapsed time on reconnect
    b.source[0] = J::SrcStick1Y;
    tick(slow, 0.05);
    assert(slow.paddleValue(0) == 128);
    b.source[0] = J::SrcRaw0 + 7; // nonexistent axis
    tick(slow, 0.05);
    assert(slow.paddleValue(0) == 128);

    b.source[0] = J::SrcTriggerL;
    b.mode[0] = J::Absolute;
    b.mode[1] = J::Joystick;
    b.deadzone = 0.5f;
    b.squareGate = true;
    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = -1;
    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_Y] = 0.75f;
    tick(slow, 0.01);
    assert(slow.paddleValue(0) == 0 && slow.paddleValue(1) == 191);
    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = 1;
    tick(slow, 0.01);
    assert(slow.paddleValue(0) == 255 && slow.paddleValue(1) == 191);
    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = 0.05f;
    tick(slow, 0.01);
    assert(slow.paddleValue(0) == 134); // no deadzone on the throttle
    b.invert[0] = true;
    tick(slow, 0.01);
    assert(slow.paddleValue(0) == 121);
    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = std::numeric_limits<float>::quiet_NaN();
    tick(slow, 0.01);
    assert(slow.paddleValue(0) == 128);
    b.source[0] = J::SrcNone;
    tick(slow, 0.01);
    assert(slow.paddleValue(0) == 128);

    // Two controllers, independent velocity accumulators and suspension mask.
    b.source[0] = J::SrcStick1X;
    b.invert[0] = false;
    b.deadzone = 0;
    b.velocitySpeed[0] = 1;
    b.mode[0] = J::Velocity;
    b.mode[2] = J::Velocity;
    b.secondHost = 1;
    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_X] = 0.25f;
    pads[1].axes[GLFW_GAMEPAD_AXIS_LEFT_X] = -0.25f;
    tick(slow, 0);
    frames(slow, 60, 1.0 / 60, 0xC); // keyboard/mouse overrides only PDL0/1
    assert(slow.paddleValue(0) == 128 && slow.paddleValue(2) == 64);
    slow.centrePaddles(1);
    assert(slow.paddleValue(2) == 64);
    b.invert[2] = true;
    tick(slow, 0); // direction change resets its accumulator
    assert(slow.paddleValue(2) == 128);
    frames(slow, 60, 1.0 / 60);
    assert(slow.paddleValue(0) == 192 && slow.paddleValue(2) == 192);
    b.secondHost = -1;
    tick(slow, 0.01);
    assert(slow.paddleValue(2) == 128); // swapping host/source does not retain P2

    assert(J::validVelocitySpeed(-1) == 0.1f);
    assert(J::validVelocitySpeed(100) == 4.0f);
    assert(J::validVelocitySpeed(std::numeric_limits<float>::quiet_NaN()) == 1);
    assert(!J::validMode(-1) && !J::validMode(3));
    return 0;
}
