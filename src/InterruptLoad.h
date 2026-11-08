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

// Interrupt load: the share of each video frame the 6502 spends inside an
// IRQ or NMI handler.
//
// Why: a French Touch demo plays its Mockingboard music from the 6522's
// timer IRQ while the main loop races the beam. How much of a frame the
// player eats is the number that decides whether an effect fits, and until
// now it had to be read off the profiler's per-PC table by hand.
//
// Where the idea comes from: French Touch's AppleWin fork (branch
// `extended_debugger`, commit 2e8e150f95, 2020-10-22) drew the same bar in
// its debugger heatmap. It measured it differently: the guest program
// bracketed its own code with writes to $C066-$C069 ("FT Monitoring Card").
// POM2 needs no cooperation from the guest: the CPU already reports every
// interrupt entry to the debugger (M6502DebugHook::onInterruptEntered), so
// an unmodified disk measures as it is.
//
// How a handler is followed: on entry the stack level BEFORE the 3-byte push
// is recorded. The handler is over once SP climbs back to that level — its
// RTI, or a handler that drops its frame and jumps (same rule as the
// debugger's step-over, Debugger::onInstruction). The compare is a signed
// 8-bit distance so a stack that wraps through $00 still nests. A nested
// NMI inside an IRQ is a second level; all levels count as interrupt time.
//
// What a frame is: the VIDEO frame the instruction STARTS in (65 cycles x
// 262 or 312 lines of the beam's clock, Memory::videoCycleAt). The caller
// passes that index; this class never reads the machine. An instruction
// straddling the boundary (at most 7 cycles) is charged whole to the frame it
// began in. The denominator is the CPU cycles actually executed in the frame,
// not a nominal budget: under an accelerator a frame holds more CPU cycles,
// and the share is still what the program sees.
//
// Limits, stated: a handler already running when profiling starts counts as
// main-loop time until its RTI. BRK is an instruction, not an entry, and is
// not counted as interrupt time.
//
// Cost: only on the debugged loop, and only while profiling is on (the same
// gate as the per-PC profile). Nothing on the fast loop.
//
// Pinned by `interrupt_load`.

#ifndef POM2_INTERRUPT_LOAD_H
#define POM2_INTERRUPT_LOAD_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pom2 {

class InterruptLoad
{
public:
    struct Frame {
        uint64_t index = 0;             ///< video frame number (beam clock / frame length)
        uint64_t cycles = 0;            ///< CPU cycles executed in it, all of them
        uint64_t interruptCycles = 0;   ///< ...of which inside an IRQ/NMI handler
        uint32_t entries = 0;           ///< interrupts taken in it
        double share() const
        { return cycles ? static_cast<double>(interruptCycles) / static_cast<double>(cycles) : 0.0; }
    };
    struct Summary {
        std::size_t frames = 0;         ///< completed frames in the window
        double last = 0.0, mean = 0.0, peak = 0.0;   ///< shares, 0..1
    };

    static constexpr std::size_t kHistory  = 256;   ///< completed frames kept
    static constexpr std::size_t kMaxDepth = 8;     ///< nested handlers followed

    void reset()
    {
        cur_ = {};
        open_ = false;
        depth_ = 0;
        head_ = count_ = 0;
    }

    /// The CPU entered an interrupt handler. `spAfter` is SP after the push
    /// of PC and P; `cycles` the entry's own (7). Entry cycles are handler
    /// time: the interrupted instruction has not run.
    void entered(uint8_t spAfter, int cycles, uint64_t frameIndex)
    {
        account(cycles, true, frameIndex);
        if (cycles > 0) ++cur_.entries;
        if (depth_ < kMaxDepth) levels_[depth_++] = static_cast<uint8_t>(spAfter + 3);
    }

    /// One instruction executed; `spAfter` is SP once it has. Its cycles are
    /// charged first (an RTI belongs to the handler it ends), then every
    /// handler whose level SP has climbed back to is closed.
    void stepped(uint8_t spAfter, int cycles, uint64_t frameIndex)
    {
        account(cycles, depth_ != 0, frameIndex);
        while (depth_ && static_cast<int8_t>(static_cast<uint8_t>(spAfter - levels_[depth_ - 1])) >= 0)
            --depth_;
    }

    bool inInterrupt() const { return depth_ != 0; }
    std::size_t depth() const { return depth_; }

    /// The frame still being counted (not in frames()).
    const Frame& current() const { return cur_; }

    /// Completed frames, oldest first.
    std::vector<Frame> frames() const
    {
        std::vector<Frame> out;
        out.reserve(count_);
        const std::size_t start = (head_ + kHistory - count_) % kHistory;
        for (std::size_t i = 0; i < count_; ++i) out.push_back(ring_[(start + i) % kHistory]);
        return out;
    }

    Summary summary() const
    {
        Summary s;
        s.frames = count_;
        if (!count_) return s;
        double total = 0.0;
        for (const Frame& f : frames()) {
            const double share = f.share();
            total += share;
            s.peak = std::max(s.peak, share);
        }
        s.mean = total / static_cast<double>(count_);
        s.last = ring_[(head_ + kHistory - 1) % kHistory].share();
        return s;
    }

private:
    void account(int cycles, bool interrupt, uint64_t frameIndex)
    {
        if (cycles <= 0) return;
        if (!open_ || frameIndex != cur_.index) {
            if (open_) {
                ring_[head_] = cur_;
                head_ = (head_ + 1) % kHistory;
                count_ = std::min(count_ + 1, kHistory);
            }
            cur_ = {};
            cur_.index = frameIndex;
            open_ = true;
        }
        cur_.cycles += static_cast<uint64_t>(cycles);
        if (interrupt) cur_.interruptCycles += static_cast<uint64_t>(cycles);
    }

    Frame cur_{};
    bool open_ = false;
    std::array<uint8_t, kMaxDepth> levels_{};
    std::size_t depth_ = 0;
    std::array<Frame, kHistory> ring_{};
    std::size_t head_ = 0, count_ = 0;
};

} // namespace pom2

#endif // POM2_INTERRUPT_LOAD_H
