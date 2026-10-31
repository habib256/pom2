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

// fast_chip — the FASTChip //e (FastChip.h) against its User's Guide:
//
//  1. Registers through a real controller on a //e: closed while locked,
//     the four-$6A unlock, $C06B enable and status, speed / slot / config
//     registers, the lock keeping the speed, the speed table.
//  2. The manual's ENABLE_FASTCHIP_IIE + SET_SPEED run by the 6502, and the
//     frame budget that follows (8.3 MHz).
//  3. Slow-downs: a slot set to normal, the speaker; NONE leaves it fast.
//  4. One socket: the Zip Chip takes the FASTChip out and back.
//  5. Snapshot round trip.

#include "EmulationController.h"
#include "FastChip.h"
#include "M6502.h"
#include "Memory.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void expect(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

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
    expect(pom2::FastChip::speedMHz(9) == 1.00 && pom2::FastChip::speedMHz(40) == 16.60 &&
           pom2::FastChip::speedMHz(32) == 4.55 && pom2::FastChip::speedMHz(1) == 0.20,
           "the speed table (User's Guide p. 22)");

    EmulationController c;
    c.setCyclesPerFrame(17045);
    c.setVideoStandard(VideoStandard::NTSC);
    {
        auto st = c.lockState();
        st.memory().setIIEMode(true);
        st.memory().memWrite(0x0900, 0x4C);         // JMP $0900
        st.memory().memWrite(0x0901, 0x00);
        st.memory().memWrite(0x0902, 0x09);
        st.cpu().setProgramCounter(0x0900);
    }
    c.fastChip().setPowerOnSpeed(pom2::FastChip::kSpeed1MHz);
    c.setFastChipInstalled(true);
    c.setMode(EmulationController::Mode::Running);
    pom2::FastChip& fc = c.fastChip();
    {
        auto st = c.lockState();
        Memory& m = st.memory();
        expect(fc.speed() == 9 && m.cpuSpeedMultiplier() == 1.0, "power-on: the saved speed, 1.00 MHz");

        // ── Locked: nothing ──────────────────────────────────────────────
        m.memWrite(0xC06B, 0x00);
        m.memWrite(0xC06D, 40);
        expect(!fc.enabled() && fc.speed() == 9, "locked: enable and speed ignored");

        // ── Unlock (an interruption restarts it), enable ─────────────────
        m.memWrite(0xC06A, 0x6A); m.memWrite(0xC06A, 0x6A);
        m.memWrite(0xC06A, 0x00);
        m.memWrite(0xC06A, 0x6A); m.memWrite(0xC06A, 0x6A);
        expect(!fc.unlocked(), "another value restarts the count");
        m.memWrite(0xC06A, 0x6A); m.memWrite(0xC06A, 0x6A);
        expect(fc.unlocked() && !fc.enabled(), "four $6A unlock");
        m.memWrite(0xC06B, 0x55);
        expect(fc.enabled() && (m.memRead(0xC06B) & 0x80), "$C06B enables; bit 7 says so");

        m.memWrite(0xC06D, 33);
        expect(m.memRead(0xC06D) == 33 && std::fabs(m.cpuSpeedMultiplier() - 5.0) < 1e-9, "33 = 5.00 MHz");
        m.memWrite(0xC06D, 77);
        expect(fc.speed() == pom2::FastChip::kSpeedMax, "a speed past 40 is 16.6 MHz");
        m.memWrite(0xC06C, 0x40);
        expect(m.memRead(0xC06C) == 0x40, "$C06C slot speeds");
        m.memWrite(0xC06E, 0x00); m.memWrite(0xC06F, 21);
        expect(m.memRead(0xC06D) == 21 && m.memRead(0xC06E) == 0, "config 0 is the speed");
        m.memWrite(0xC06E, 0x02); m.memWrite(0xC06F, 0x00);
        expect(m.memRead(0xC06F) == 0x00, "config 2: speaker delay NONE");

        m.memWrite(0xC06A, 0xA6);
        expect(!fc.enabled() && !fc.unlocked() && fc.speed() == 21, "$A6 locks; the speed stays");
        expect(fc.slotMask() == 0x40, "and the slot speeds");
    }

    // ── The manual's routines, run by the 6502 ───────────────────────────
    {
        auto st = c.lockState();
        Memory& m = st.memory();
        const uint8_t prog[] = {
            0xA0, 0x6A, 0x8C, 0x6A, 0xC0, 0x8C, 0x6A, 0xC0,   // LDY #$6A / STY $C06A ×4
            0x8C, 0x6A, 0xC0, 0x8C, 0x6A, 0xC0,
            0x8C, 0x6B, 0xC0, 0xAC, 0x6B, 0xC0,               // STY $C06B / LDY $C06B
            0x10, 0x05,                                       // BPL nope
            0xA9, 0x25, 0x8D, 0x6D, 0xC0,                     // LDA #37 (8.30 MHz) / STA $C06D
            0xA0, 0xA6, 0x8C, 0x6A, 0xC0,                     // nope: LDY #$A6 / STY $C06A
            0x4C, 0x20, 0x08,                                 // JMP *
        };
        for (std::size_t i = 0; i < sizeof prog; ++i) m.memWrite(uint16_t(0x0800 + i), prog[i]);
        st.cpu().setProgramCounter(0x0800);
    }
    burnFrame(c);
    const long fastCycles = burnFrame(c);
    expect(fc.speed() == 37 && !fc.enabled(), "ENABLE + SET_SPEED + lock from 6502 code");
    expect(fastCycles > 17045 * 8 && fastCycles < 17045 * 9, "an 8.3 MHz frame runs ~8.3x the cycles");
    std::printf("  frame at speed 37: %ld cycles\n", fastCycles);

    // ── Slow-downs ───────────────────────────────────────────────────────
    {
        auto st = c.lockState();
        Memory& m = st.memory();
        (void)m.memRead(0xC0EC);                         // slot 6, set fast by mask $40
        expect(!fc.delayActive(m.getCycleCounter()), "slot 6 set fast: no slow-down");
        (void)m.memRead(0xC0DC);                         // slot 5, normal
        expect(fc.delayActive(m.getCycleCounter()) && m.cpuSpeedMultiplier() == 1.0,
               "slot 5 normal: 1 MHz for a while");
    }
    {
        pom2::FastChip f;
        f.setPowerOnSpeed(40);
        f.onReset(0);
        f.ioWrite(0x30, 0, 1000);
        expect(f.delayActive(1001), "the speaker: NORMAL slows");
        expect(std::fabs(f.cpuSpeedMultiplier(1001) - 1.0) < 1e-9, "at 1 MHz");
        std::vector<uint8_t> blob;
        f.appendSnapshotState(blob);
        pom2::FastChip g;
        expect(g.loadSnapshotState(blob.data(), blob.size()) && g.speed() == 40 && g.delayActive(1001),
               "snapshot round trip");
        blob[11] = 99;
        expect(!g.validateSnapshotState(blob.data(), blob.size()), "a speed past 40 is refused");
    }

    // ── One socket ───────────────────────────────────────────────────────
    c.setZipChipInstalled(true);
    expect(c.zipChipInstalled() && !c.fastChipInstalled(), "the Zip Chip takes the socket");
    c.setFastChipInstalled(true);
    expect(c.fastChipInstalled() && !c.zipChipInstalled(), "and gives it back");

    if (failures) { std::printf("fast_chip: %d failure(s)\n", failures); return 1; }
    std::printf("fast_chip: PASS\n");
    return 0;
}
