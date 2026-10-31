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

// Memory — the $C000 read, and the line-at-a-time paste behind it.
//
// After a pasted Return the Keyboard holds the rest of the paste
// (Keyboard::pasteHeld). The line counts as taken once the text screen has
// stopped changing for 0.1 s of machine time — software echoes what it
// reads — or after 2 s whatever the screen does (a program that prints
// all the time). The screen is the text page 1 in main and aux memory,
// sampled every 8192 cycles while the guest polls $C000; nothing runs
// when no paste is held, nor without a CPU (no time). sbabcoc's ReplicApple2Plus TypingFeeder: the same
// 0.1 s / 2 s, found on the SoftCard II CP/M's 256-byte type-ahead ring.

#include "Memory.h"

#include "CpuClock.h"

namespace {
constexpr uint64_t kSampleCycles = 8192;
constexpr uint64_t kSettleCycles = POM2_CPU_CLOCK_HZ / 10;   // 0.1 s
constexpr uint64_t kMaxWaitCycles = 2 * POM2_CPU_CLOCK_HZ;   // 2 s
} // namespace

uint8_t Memory::readKeyLatch()
{
    // No CPU wired (a bare Memory in a test): no time passes, so there is
    // nothing to wait for.
    if (keyboard_.pasteHeld() && !cpu) keyboard_.releasePasteHold();
    if (keyboard_.pasteHeld()) {
        const uint64_t now = cycleCounter;
        uint64_t h = 1469598103934665603ull;          // FNV-1a over the text page
        for (uint32_t a = 0x0400; a < 0x0800; ++a) {
            h = (h ^ mem[a]) * 1099511628211ull;
            if (iieMode) h = (h ^ aux[a]) * 1099511628211ull;
        }
        if (!pasteSettle_.tracking) {
            pasteSettle_ = { true, now, now, now, h };
        } else if (now - pasteSettle_.lastSample >= kSampleCycles) {
            pasteSettle_.lastSample = now;
            if (h != pasteSettle_.hash) { pasteSettle_.hash = h; pasteSettle_.stableSince = now; }
        }
        if (now - pasteSettle_.stableSince >= kSettleCycles
            || now - pasteSettle_.heldSince >= kMaxWaitCycles) {
            pasteSettle_.tracking = false;
            keyboard_.releasePasteHold();
        }
    } else {
        pasteSettle_.tracking = false;
    }
    return keyboard_.readLatch();
}
