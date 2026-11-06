// POM2 — SPDX-License-Identifier: GPL-3.0-or-later
// Exercise production poll/routing with three simulated host controllers.
#include "JoystickInput.h"
#include "SiriusJoyport.h"
#include <GLFW/glfw3.h>
#include <array>
#include <cassert>

namespace {
std::array<GLFWgamepadstate, 3> pads{};
std::array<bool, 3> connected{true, true, false};
std::array<bool, 3> mapped{true, true, true};
std::array<std::array<unsigned char, 3>, 3> rawButtons{};
bool present(int jid) { return jid >= 0 && jid < 3 && connected[jid]; }
}
extern "C" {
int glfwJoystickPresent(int jid) { return present(jid); }
const char* glfwGetJoystickName(int) { return "Simulated controller"; }
const float* glfwGetJoystickAxes(int jid, int* count) {
    *count = present(jid) ? 4 : 0;
    return present(jid) ? pads[jid].axes : nullptr;
}
const unsigned char* glfwGetJoystickButtons(int jid, int* count) {
    *count = present(jid) ? 3 : 0;
    return present(jid) ? rawButtons[jid].data() : nullptr;
}
int glfwJoystickIsGamepad(int jid) { return present(jid) && mapped[jid]; }
int glfwGetGamepadState(int jid, GLFWgamepadstate* state) {
    if (!glfwJoystickIsGamepad(jid)) return GLFW_FALSE;
    *state = pads[jid]; return GLFW_TRUE;
}
}

int main()
{
    using J = JoystickInput;
    J input;
    auto& b = input.binding();
    b.deadzone = 0;
    b.squareGate = false;
    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_X] = 1;
    pads[0].axes[GLFW_GAMEPAD_AXIS_RIGHT_X] = -1;
    pads[1].axes[GLFW_GAMEPAD_AXIS_LEFT_Y] = 1;
    input.poll();
    assert(b.hostIdx == 0 && input.hostPreference() == J::kAutomaticHost);
    assert(!input.twoPlayers());
    assert(input.paddleValue(0) == 255 && input.paddleValue(2) == 0);
    pads[0].buttons[GLFW_GAMEPAD_BUTTON_A] = GLFW_PRESS;
    input.poll();
    assert(!input.gamePortButton(0) && input.gamePortButton(1)); // legacy A -> PB1

    b.secondHost = J::kAutomaticHost;
    input.poll();
    assert(input.secondHostIndex() == 1);
    assert(input.paddleValue(0) == 255 && input.paddleValue(1) == 128);
    assert(input.paddleValue(2) == 128 && input.paddleValue(3) == 255);
    using S = pom2::SiriusJoyport;
    assert(input.joyportContacts(0) == (S::Rightward | S::Fire));
    assert(input.joyportContacts(1) == S::Down);
    b.mode[0] = J::Velocity; // digital contacts never use retained positions
    input.advancePaddles(0.05);
    assert(input.joyportContacts(0) == (S::Rightward | S::Fire));
    pads[0].buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT] = GLFW_PRESS;
    pads[1].buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT] = GLFW_PRESS;
    input.poll();
    assert(input.joyportContacts(0) == S::Fire); // opposing sources cancel
    assert(input.joyportContacts(1) == (S::Down | S::Rightward));
    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_X] = 0;
    input.poll();
    assert(input.joyportContacts(0) == (S::Leftward | S::Fire));
    pads[0].buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT] = GLFW_RELEASE;
    pads[1].buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT] = GLFW_RELEASE;
    pads[0].axes[GLFW_GAMEPAD_AXIS_LEFT_X] = 1;
    b.mode[0] = J::Joystick;
    input.poll();
    assert(input.gamePortButton(0) && !input.gamePortButton(1)); // P1 A -> PB0
    pads[1].buttons[GLFW_GAMEPAD_BUTTON_B] = GLFW_PRESS;
    pads[1].buttons[GLFW_GAMEPAD_BUTTON_START] = GLFW_PRESS;
    pads[1].buttons[GLFW_GAMEPAD_BUTTON_X] = GLFW_PRESS;
    input.poll();
    assert(input.gamePortButton(0) && input.gamePortButton(1) && !input.gamePortButton(2));
    assert(!input.uiNav().menu && !input.play().spaceEdge); // P2 cannot navigate/type
    b.invert[3] = true;
    input.poll();
    assert(input.paddleValue(3) == 0 && input.paddleValue(0) == 255);
    b.invert[3] = false;
    b.secondSource[0] = J::SrcStick2X;
    pads[1].axes[GLFW_GAMEPAD_AXIS_RIGHT_X] = -1;
    input.poll();
    assert(input.paddleValue(2) == 0); // independent sources

    connected[1] = false;
    input.poll();
    assert(input.joyportContacts(1) == 0);
    assert(input.secondHostIndex() == -1);
    assert(input.paddleValue(2) == 128 && input.paddleValue(3) == 128);
    assert(!input.gamePortButton(1) && input.gamePortButton(0));
    connected[2] = true;
    input.poll();
    assert(input.secondHostIndex() == 2);
    connected[1] = true;
    input.poll();
    assert(input.secondHostIndex() == 2); // keep live assignment on new hotplug
    connected[0] = false;
    input.poll();
    assert(b.hostIdx == 0 && input.secondHostIndex() == 2); // do not swap players
    assert(input.paddleValue(0) == 128 && !input.gamePortButton(0));
    connected[0] = true;

    b.secondHost = 1;
    input.poll();
    assert(input.secondHostIndex() == 1);
    connected[1] = false;
    input.poll();
    assert(input.secondHostIndex() == -1); // explicit slot never takes pad 3
    b.secondHost = 0;
    input.poll();
    assert(input.secondHostIndex() == -1 && !input.gamePortButton(1)); // conflict
    b.secondHost = 99;
    input.poll();
    assert(!input.twoPlayers()); // malformed configuration is safe

    b.secondHost = -1;
    input.poll();
    assert(input.paddleValue(2) == 0 && input.gamePortButton(1)); // single restored
    mapped[0] = false;
    rawButtons[0] = {GLFW_PRESS, GLFW_RELEASE, GLFW_PRESS};
    input.poll();
    assert(input.gamePortButton(0) && !input.gamePortButton(1) && input.gamePortButton(2));
    connected[1] = true; mapped[1] = false;
    rawButtons[1][0] = GLFW_PRESS;
    b.secondHost = 1;
    input.poll();
    assert(input.gamePortButton(0) && input.gamePortButton(1) && !input.gamePortButton(2));

    // A stored explicit "none" survives polls. Automatic can be restored.
    rawButtons[1][0] = GLFW_RELEASE;
    input.poll();
    assert(!input.gamePortButton(1)); // old mapped B must not linger
    assert(!(input.joyportContacts(1) & S::Fire));
    rawButtons[1][0] = GLFW_PRESS;
    b.hostIdx = -1; input.markBindingExplicit();
    input.poll();
    assert((input.joyportContacts(1) & S::Fire) && input.joyportContacts(0) == 0);
    assert(b.hostIdx == -1 && input.hostPreference() == -1);
    assert(input.paddleValue(0) == 128 && !input.gamePortButton(0));
    assert(input.gamePortButton(1)); // keyboard P1 + physical P2 is allowed
    input.useAutomaticBinding();
    b.secondHost = 0; // reserve slot 0 for P2 before choosing P1
    input.poll();
    assert(b.hostIdx == 1 && input.secondHostIndex() == 0);
    assert(input.hostPreference() == J::kAutomaticHost);
    assert(!input.gamePortButton(-1) && !input.gamePortButton(3));

    connected.fill(false);
    J waiting;
    waiting.binding().secondHost = J::kAutomaticHost;
    waiting.poll();
    assert(waiting.binding().hostIdx == -1 && waiting.secondHostIndex() == -1);
    connected[1] = true;
    waiting.poll();
    assert(waiting.binding().hostIdx == 1 && waiting.secondHostIndex() == -1);
    connected[0] = true;
    waiting.poll();
    assert(waiting.binding().hostIdx == 1 && waiting.secondHostIndex() == 0);
    assert(waiting.hostPreference() == J::kAutomaticHost);
    return 0;
}
