// POM2 — SPDX-License-Identifier: GPL-3.0-or-later
#ifndef POM2_SIRIUS_JOYPORT_H
#define POM2_SIRIUS_JOYPORT_H

#include <array>
#include <cstdint>

namespace pom2 {
// Atari mode only; the rear Apple sockets are empty. All access is under
// the controller state lock. This device has no clock and no reset line:
// AN0/AN1 belong to Memory, and are sampled at EACH guest bus access.
// [B] Sirius Joyport User Manual pp. 10–13. See docs/hardware/sirius-joyport.md
// for the contradictory AN0 table and the scope of the chosen wiring.
class SiriusJoyport {
public:
    enum Mode : uint8_t { Off = 0, Left = 1, Centre = 2, Right = 3 };
    enum Contact : uint8_t { Up = 1, Down = 2, Leftward = 4, Rightward = 8, Fire = 16 };
    static bool validMode(int mode) { return mode >= Off && mode <= Right; }
    void setMode(int mode) { mode_ = validMode(mode) ? Mode(mode) : Off; }
    Mode mode() const { return mode_; }
    void setContacts(int jack, uint8_t contacts) {
        if (jack >= 0 && jack < 2) contacts_[jack] = contacts & 31;
    }
    uint8_t contacts(int jack) const { return contacts_[jack]; }
    uint8_t readButton(int button, bool an0, bool an1) const {
        const int jack = mode_ == Right || (mode_ == Centre && an0) ? 1 : 0;
        const auto contact = button == 0 ? Fire
            : button == 1 ? (an1 ? Up : Leftward) : (an1 ? Down : Rightward);
        return contacts_[jack] & contact ? 0 : 0x80;
    }
private:
    Mode mode_ = Off;
    std::array<uint8_t, 2> contacts_{};
};
}
#endif
