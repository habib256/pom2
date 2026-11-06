// POM2 Apple II Emulator
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace pom2 {

// Host input policy, independent of GLFW and the emulated hardware.
// Remember physical tokens so a release still works after a layout/modifier
// change. Suspend drops held keys; auto-repeat must not rearm them.
class KeyboardJoystick {
public:
    enum Control { None = -1, Left, Right, Up, Down, Fire0, Fire1 };

    static constexpr Control control(int key, char layoutLetter) {
        switch (key) {
        case 263: return Left;
        case 262: return Right;
        case 265: return Up;
        case 264: return Down;
        default: return layoutLetter == 'X' ? Fire0 :
                        layoutLetter == 'Z' ? Fire1 : None;
        }
    }

    void setActive(bool active) {
        active_ = active;
        if (!active) { held_.fill(-1); suppressCharacter_ = false; }
    }
    bool active() const { return active_; }

    // down=false is release; repeat never starts a new hold. Chords with
    // Ctrl/Alt/Super keep their normal meaning; Shift/caps lock are allowed.
    bool key(int token, Control control, bool down, bool repeat, bool chord) {
        suppressCharacter_ = false;
        bool owned = false;
        for (int& held : held_) {
            if (held == token && token >= 0) {
                owned = true;
                if (!down) held = -1;
            }
        }
        if (!down) return owned;
        if (!active_ || token < 0) return false;
        if (chord) {
            for (int& held : held_)
                if (held == token) held = -1;
            return false;
        }
        if (owned) return suppressCharacter_ = true;
        if (control == None) return false;
        if (!repeat) held_[control] = token;
        return suppressCharacter_ = true;
    }

    bool suppressCharacter() const { return suppressCharacter_; }
    std::uint8_t paddle(int axis) const {
        const bool negative = held_[axis == 0 ? Left : Up] >= 0;
        const bool positive = held_[axis == 0 ? Right : Down] >= 0;
        return negative == positive ? 128 : negative ? 0 : 255;
    }
    bool button(int index) const { return held_[index == 0 ? Fire0 : Fire1] >= 0; }

private:
    bool active_ = false;
    bool suppressCharacter_ = false;
    std::array<int, 6> held_{-1, -1, -1, -1, -1, -1};
};
} // namespace pom2
