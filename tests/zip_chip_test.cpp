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

// zip_chip — the Zip Chip in the 6502 socket (ZipChip.h), per MAME
// src/mame/apple/apple2e.cpp. Driven through a real EmulationController on
// a //e, because the hooks live in Memory's $C0xx path and the speed lives
// in the controller's per-chunk budget:
//
//   * locked: $C05A/$C05B/$C05C writes do nothing to the chip, reads of
//     $C058-$C05F are plain annunciators — and AN0 still follows them;
//   * unlock = four $5A writes to $C05A in a row; an intervening I/O write
//     cancels it; $A5 locks;
//   * unlocked: the register reads ($C058 = $C0, $C05C = mask, $C05E state),
//     $C05A = stop, $C05B = go — visible in Memory::cpuSpeedMultiplier AND in
//     the cycles a frame actually burns;
//   * unlocked + fast, a READ of $C058 skips the annunciator; a WRITE never
//     does;
//   * slot 6 ($C0E0) opens the 52 ms slow-down window under the reset mask;
//   * a ][+ (no iieMode) never sees the chip; reset re-locks it.

#include "CpuClock.h"
#include "EmulationController.h"
#include "M6502.h"
#include "Memory.h"
#include "ZipChip.h"

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
void expect(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

void unlock(Memory& m) { for (int i = 0; i < 4; ++i) m.memWrite(0xC05A, 0x5A); }

long burnFrame(EmulationController& c)
{
    uint64_t before;
    { auto st = c.lockState(); before = st.memory().getCycleCounter(); }
    c.tickFrame();
    auto st = c.lockState();
    return static_cast<long>(st.memory().getCycleCounter() - before);
}
} // namespace

int main()
{
    EmulationController c;
    c.setCyclesPerFrame(17045);
    c.setVideoStandard(VideoStandard::NTSC);
    const double fastMul = pom2::ZipChip::kFastHz / 1022727.0;
    {
        auto st = c.lockState();
        st.memory().setIIEMode(true);
        for (uint16_t a = 0x0800; a < 0x0900; ++a) st.memory().memWrite(a, 0xEA);
        st.memory().memWrite(0x0900, 0x4C);
        st.memory().memWrite(0x0901, 0x00);
        st.memory().memWrite(0x0902, 0x08);
        st.cpu().setProgramCounter(0x0800);
    }
    c.setZipChipInstalled(true);
    c.setMode(EmulationController::Mode::Running);
    pom2::ZipChip& zip = c.zipChip();

    {
        auto st = c.lockState();
        Memory& m = st.memory();
        expect(!zip.unlocked() && zip.fast(), "power-up: locked and fast");
        expect(std::fabs(m.cpuSpeedMultiplier() - fastMul) < 1e-9, "4 MHz multiplier");

        // ── Locked: no register access, annunciators intact ──────────────
        m.memWrite(0xC05A, 0x00);                       // would be "stop" unlocked
        expect(zip.fast(), "locked: $C05A write does not stop the chip");
        m.memWrite(0xC05C, 0x00);
        expect(zip.slotSpeakerMask() == pom2::ZipChip::kResetSlotSpk, "locked: mask untouched");
        (void)m.memRead(0xC059);
        expect(m.getDisplayState().an0, "locked: read $C059 sets AN0");
        (void)m.memRead(0xC058);
        expect(!m.getDisplayState().an0, "locked: read $C058 clears AN0");

        // ── Unlock, interrupted then complete ────────────────────────────
        m.memWrite(0xC05A, 0x5A); m.memWrite(0xC05A, 0x5A);
        m.memWrite(0xC030, 0x00);                       // another write in between
        m.memWrite(0xC05A, 0x5A); m.memWrite(0xC05A, 0x5A);
        expect(!zip.unlocked(), "an intervening write cancels the unlock");
        unlock(m);
        expect(zip.unlocked(), "four $5A writes unlock");

        expect(m.memRead(0xC058) == 0xC0, "unlocked: $C058 reads $C0");
        expect(m.memRead(0xC05C) == pom2::ZipChip::kResetSlotSpk, "unlocked: $C05C reads the mask");
        expect((m.memRead(0xC05B) & 0x13) == 0x03, "unlocked: $C05B status = fast, 64 KB cache");
        expect((m.memRead(0xC05E) & 0x01) == 0x01, "unlocked: $C05E bit 0 = LC reads ROM");

        // Unlocked + fast: a READ of $C059 skips the annunciator...
        (void)m.memRead(0xC059);
        expect(!m.getDisplayState().an0, "unlocked+fast: read $C059 skips AN0");
        // ...a WRITE never does (MAME calls do_io on every write).
        m.memWrite(0xC059, 0x00);
        expect(m.getDisplayState().an0, "unlocked: write $C059 still sets AN0");

        // ── Speed control ────────────────────────────────────────────────
        m.memWrite(0xC05A, 0x00);
        expect(!zip.fast() && m.cpuSpeedMultiplier() == 1.0, "$C05A stops: 1x");
        expect((m.memRead(0xC05B) & 0x10) == 0x10, "$C05B status bit 4 = disabled");
        // Slow + unlocked: the annunciator read is NOT skipped.
        (void)m.memRead(0xC058);
        expect(!m.getDisplayState().an0, "unlocked+slow: read $C058 clears AN0");
    }
    const long slowCycles = burnFrame(c);
    {
        auto st = c.lockState();
        st.memory().memWrite(0xC05B, 0x00);
        expect(zip.fast(), "$C05B goes");
    }
    const long fastCycles = burnFrame(c);
    std::printf("frame: %ld cycles stopped, %ld cycles fast (x%.3f), clock %.0f Hz\n",
                slowCycles, fastCycles, double(fastCycles) / double(slowCycles),
                c.emulatedCpuClockHz());
    expect(std::abs(slowCycles - 17045) < 64, "stopped: a stock frame");
    expect(std::fabs(double(fastCycles) / 17045.0 - fastMul) < 0.02, "fast: a 4 MHz frame");
    expect(std::fabs(c.emulatedCpuClockHz() - 4000000.0) < 2.0, "devices told 4 MHz");

    {
        auto st = c.lockState();
        Memory& m = st.memory();
        // ── Slot slow-down: slot 6 is in the reset mask ($E4) ────────────
        (void)m.memRead(0xC0E0);
        const uint64_t now = m.getCycleCounter();
        expect(zip.delayActive(now) && m.cpuSpeedMultiplier() == 1.0, "slot 6 access slows down");
        expect((m.memRead(0xC05B) & 0x08) == 0x08, "$C05B bit 3 = delay active");
        m.advanceCycles(static_cast<int>(0.053 * 1022727));
        expect(m.cpuSpeedMultiplier() > 3.0, "52 ms later the chip is fast again");
        (void)m.memRead(0xC090);                    // slot 1: not in the mask
        expect(m.cpuSpeedMultiplier() > 3.0, "slot 1 is not slowed under $E4");
        // $C05E bit 7 disables the delays.
        m.memWrite(0xC05E, 0x80);
        (void)m.memRead(0xC0E0);
        expect(m.cpuSpeedMultiplier() > 3.0, "$C05E bit 7 disables the delays");

        // ── Lock, then reset ─────────────────────────────────────────────
        m.memWrite(0xC05A, 0xA5);
        expect(!zip.unlocked(), "$A5 locks");
        const uint8_t v = m.memRead(0xC05C);
        (void)v;   // floating bus: not asserted, only that the chip let go
        expect(!zip.unlocked(), "a read does not unlock");
        unlock(m);
        m.memWrite(0xC05A, 0x00);
    }
    c.softReset();
    expect(!zip.unlocked() && zip.fast(), "reset re-locks and boots fast");
    {
        auto st = c.lockState();
        st.memory().setIIEMode(false);              // a ][+: no Zip decode
        expect(st.memory().cpuSpeedMultiplier() == 1.0, "][+ ignores the chip");
        unlock(st.memory());
        expect(!zip.unlocked(), "][+: $C05A never reaches the chip");
    }
    c.setZipChipInstalled(false);

    std::printf("zip_chip: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
