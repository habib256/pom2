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

// PaddleInputs — the Apple II game-port state, split out of the Memory
// god-object (TODO P2, "one concern per file"). Four paddles ($C064-$C067),
// three push-buttons ($C061-$C063), and the Open/Solid-Apple + Shift modifier
// keys OR'd onto PB0/PB1/PB2. Memory owns one of these and forwards its
// $C061-$C067 reads and $C070 re-arm to it; the only piece Memory keeps is
// the master `cycleCounter`, which the RC-discharge read needs and which is
// passed in rather than duplicated here.
//
// Threading: the modifier keys are written from the GLFW key callback (the UI
// thread) and read by the CPU worker, so they are atomics — exactly as they
// were as bare Memory members. The paddle value/button arrays are written
// only under stateMutex (MainWindow's joystick block takes lockState()) and
// read by the worker while it holds the same lock, so they need no atomics;
// `input_io_smoke` and `ui_worker_contention` pin both facts.

#ifndef POM2_PADDLE_INPUTS_H
#define POM2_PADDLE_INPUTS_H

#include <array>
#include <atomic>
#include <cstdint>

namespace pom2 {

class PaddleInputs
{
public:
    // ── Writers ───────────────────────────────────────────────────────────
    void setPaddle(int idx, uint8_t value) {
        if (idx >= 0 && idx < 4) value_[idx] = value;
    }
    void setPaddleButton(int idx, bool down) {
        if (idx >= 0 && idx < 3) button_[idx] = down;
    }
    void setOpenAppleKey (bool down) { openApple_.store(down,  std::memory_order_relaxed); }
    void setSolidAppleKey(bool down) { solidApple_.store(down, std::memory_order_relaxed); }
    void setShiftKey     (bool down) { shift_.store(down,      std::memory_order_relaxed); }
    void setControlKey   (bool down) { ctrl_.store(down,       std::memory_order_relaxed); }
    /// The //e "Shift key = SW2" modification (MAME `kbd_shift_mod`, Apple
    /// IIe Technical Note #9): a wire from the Shift key to PB2. Off by
    /// default, as in MAME.
    void setShiftKeyMod  (bool on)   { shiftMod_.store(on,     std::memory_order_relaxed); }
    bool shiftKeyMod() const         { return shiftMod_.load(std::memory_order_relaxed); }

    /// The ][ / ][+ modifications: Shift (and, for some word processors,
    /// Control) wired to a game-I/O input. MAME `apple2.cpp` "Shift key mod"
    /// (a2_config 0x0c): SW2 = Shift; PADDL2 = Shift + PADDL3 = Ctrl (Zardax,
    /// the "lewis" lowercase Monitor); SW0 = Shift + SW1 = Ctrl (Format-II,
    /// Sandy's Word Processor). Each wire is active LOW, as the //e one:
    /// `a2kbd.h` "0 = pressed, 1 = not pressed". Separate from the //e
    /// setting, and read only on a ][ / ][+. Not fitted by default (a stock
    /// machine; MAME defaults to SW2).
    enum class IiShiftWiring : uint8_t { None = 0, Sw2 = 1, Paddl23 = 2, Sw01 = 3 };

    /// Is a joystick or a pair of paddles plugged into the game port? With
    /// nothing there each pot is an open circuit, so the 558 never times out
    /// and every paddle reads 255; and on a ][ / ][+ the pull-downs of PB0
    /// and PB1 are in the controller, so those two inputs float HIGH and
    /// read pressed — the //e put its pull-downs on the motherboard (MAME
    /// `apple2.cpp` c060_r "polls high if not connected, unlike IIe";
    /// `apple2e.cpp` cases $C064-$C067 `!is_device_connected()`). Default
    /// connected: a ][+ with nothing plugged in looks like a fire button
    /// held down to most games.
    void setConnected(bool on) { connected_.store(on, std::memory_order_relaxed); }
    bool connected() const     { return connected_.load(std::memory_order_relaxed); }
    void setIiShiftWiring(IiShiftWiring w) { iiWiring_.store(uint8_t(w), std::memory_order_relaxed); }
    IiShiftWiring iiShiftWiring() const {
        return IiShiftWiring(iiWiring_.load(std::memory_order_relaxed));
    }

    // ── Push-button reads ($C061-$C063 bit 7) ─────────────────────────────
    // PB0/PB1 OR the paddle button with Open/Solid-Apple. PB2 is the game
    // port's third button — unless the //e Shift-key mod is fitted, and then
    // it is the Shift key ALONE and active LOW: the wire grounds PB2 while
    // Shift is held, so $C063 reads $80 with Shift UP. MAME `apple2e.cpp`
    // c000_r, $C063: `m_kbd_shift_mod ? (kbspecial & 0x06) == 0 : sw2_r()`.
    // The dead code this replaced OR'd Shift in HIGH and was never called
    // (INPUT-001).
    //
    // `iiPlus` = a ][ / ][+ (its own wirings); `iieMod` = a //e whose PB2
    // may carry the //e mod (not a //c). MAME `apple2.cpp` c060_r, cases
    // 1-3 and 6-7.
    bool button0(bool iiPlus = false) const {
        if (iiPlus && iiShiftWiring() == IiShiftWiring::Sw01) return !shift_.load(std::memory_order_relaxed);
        if (!connected()) return iiPlus || openApple_.load(std::memory_order_relaxed);
        return button_[0] || openApple_.load(std::memory_order_relaxed);
    }
    bool button1(bool iiPlus = false) const {
        if (iiPlus && iiShiftWiring() == IiShiftWiring::Sw01) return !ctrl_.load(std::memory_order_relaxed);
        if (!connected()) return iiPlus || solidApple_.load(std::memory_order_relaxed);
        return button_[1] || solidApple_.load(std::memory_order_relaxed);
    }
    bool button2(bool iieMod, bool iiPlus = false) const {
        if ((iieMod && shiftMod_.load(std::memory_order_relaxed))
            || (iiPlus && iiShiftWiring() == IiShiftWiring::Sw2))
            return !shift_.load(std::memory_order_relaxed);
        return connected() && button_[2];
    }
    /// PADDL2 / PADDL3 replaced by Shift / Ctrl (][+ only). -1 = the timer.
    int wiredPaddle(int idx, bool iiPlus) const {
        if (!iiPlus || iiShiftWiring() != IiShiftWiring::Paddl23 || idx < 2) return -1;
        return !(idx == 2 ? shift_ : ctrl_).load(std::memory_order_relaxed);
    }

    // ── Paddle read ($C064-$C067 bit 7) ───────────────────────────────────
    // The RC network is still charging — bit 7 high — while fewer than
    // `value * 11` STOCK cycles (~10.8 us a count, MAME apple2e.cpp:1007)
    // have elapsed since that axis's timer was last triggered. The 558 is an
    // RC timer: real time, not CPU cycles — the caller converts
    // (Memory::stockCyclesBetween), or under a Zip with its paddle slow-down
    // off the paddles read 3-4x low. The count is the value AT the trigger,
    // as MAME stores an expiry time.
    bool discharging(int idx, uint64_t stockElapsed) const {
        return !connected() || stockElapsed < armed_[idx];
    }

    // $C070 (mirrored $C070-$C07F). The 558 is four monostables, and a
    // running one cannot be retriggered: only an axis whose count has run out
    // restarts (MAME apple2e.cpp:2053-2056, apple2.cpp controller_strobe_w).
    // So PDL(1) read straight after PDL(0) still sees the first strobe's
    // count — the "add a delay between PDL reads" artifact of the hardware.
    // `stockSince(latch)` converts a latch cycle to stock cycles elapsed.
    template <class StockSince>
    void rearm(uint64_t cycleCounter, StockSince stockSince) {
        for (int i = 0; i < 4; ++i) {
            if (discharging(i, stockSince(latch_[i]))) continue;
            latch_[i] = cycleCounter;
            armed_[i] = static_cast<uint32_t>(value_[i]) * 11;
        }
    }

    // ── Snapshot ───────────────────────────────────────────────────────────
    uint64_t latchCycle(int idx = 0) const { return latch_[idx]; }
    uint32_t armedCount(int idx) const     { return armed_[idx]; }
    /// An older snapshot carries one latch: every axis triggered then, with
    /// the live values.
    void setLatchCycle(uint64_t c) {
        for (int i = 0; i < 4; ++i) {
            latch_[i] = c;
            armed_[i] = static_cast<uint32_t>(value_[i]) * 11;
        }
    }
    void setAxis(int idx, uint64_t latch, uint32_t armed) {
        latch_[idx] = latch;
        armed_[idx] = armed;
    }

private:
    std::array<uint8_t, 4> value_ { 128, 128, 128, 128 };
    std::array<bool,    3> button_{ false, false, false };
    std::atomic<bool> openApple_ { false };
    std::atomic<bool> solidApple_{ false };
    std::atomic<bool> shift_     { false };
    std::atomic<bool> shiftMod_  { false };
    std::atomic<bool> ctrl_      { false };
    std::atomic<uint8_t> iiWiring_ { 0 };
    std::atomic<bool> connected_ { true };
    // Boot-time value: cycleCounter - latch_ must exceed every paddle
    // threshold (max 255*11 = 2805) at power-on so an un-strobed $C064-$C067
    // reads low — else the //e "no paddle connected" self-test is confused.
    // `0 - (~0 - 0xFFFF)` = 0x10000 > 2805. (Copied verbatim from the old
    // Memory member's rationale.)
    std::array<uint64_t, 4> latch_{ ~uint64_t(0) - 0xFFFFu, ~uint64_t(0) - 0xFFFFu,
                                    ~uint64_t(0) - 0xFFFFu, ~uint64_t(0) - 0xFFFFu };
    std::array<uint32_t, 4> armed_{ 0, 0, 0, 0 };   // value * 11 at the trigger
};

} // namespace pom2

#endif // POM2_PADDLE_INPUTS_H
