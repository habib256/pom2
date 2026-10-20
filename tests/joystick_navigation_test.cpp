// POM2 — GPL-3.0-or-later
// Drive the real poll() with two simulated GLFW gamepads, without hardware.
#include "JoystickInput.h"
#include <GLFW/glfw3.h>
#include <array>
#include <cassert>
#include <cstdio>

namespace {
std::array<GLFWgamepadstate, 2> pads{};
std::array<bool, 2> connected{true, true};
bool present(int jid) { return jid >= 0 && jid < 2 && connected[jid]; }
}

extern "C" {
int glfwJoystickPresent(int jid) { return present(jid); }
const char* glfwGetJoystickName(int) { return "Simulated gamepad"; }
const float* glfwGetJoystickAxes(int, int* count) { *count = 0; return nullptr; }
const unsigned char* glfwGetJoystickButtons(int, int* count) { *count = 0; return nullptr; }
int glfwJoystickIsGamepad(int jid) { return present(jid); }
int glfwGetGamepadState(int jid, GLFWgamepadstate* state) {
    if (!present(jid)) return GLFW_FALSE;
    *state = pads[jid];
    return GLFW_TRUE;
}
}

int main()
{
    JoystickInput input;
    input.binding().hostIdx = 0;
    input.poll();                         // baseline on an idle pad
    input.poll();
    pads[1].buttons[GLFW_GAMEPAD_BUTTON_START] = GLFW_PRESS;
    input.binding().hostIdx = 1;
    input.poll();
    assert(!input.uiNav().menu && "rebinding fabricated a Start press");
    input.poll();
    assert(!input.uiNav().menu);          // still held, still no new press
    pads[1].buttons[GLFW_GAMEPAD_BUTTON_START] = GLFW_RELEASE;
    input.poll();
    pads[1].buttons[GLFW_GAMEPAD_BUTTON_START] = GLFW_PRESS;
    input.poll();
    assert(input.uiNav().menu);           // a real new press still works
    input.poll();
    assert(!input.uiNav().menu);

    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_X] = 1.0f;
    input.binding().hostIdx = 0;
    input.poll();
    assert(!input.uiNav().right && input.uiNav().rightHeld);
    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_X] = 0.0f;
    input.poll();
    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_X] = 1.0f;
    input.poll();
    assert(input.uiNav().right);          // a new stick push still works

    connected[0] = false;
    input.poll();
    connected[0] = true;
    input.poll();
    assert(!input.uiNav().right && input.uiNav().rightHeld);
    std::puts("joystick_navigation: OK");
}
