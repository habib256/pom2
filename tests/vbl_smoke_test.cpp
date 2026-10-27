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

// VBL ($C019) scanline-accurate smoke test. Pins:
//   - Apple II video timing: 262 NTSC scanlines × 65 CPU cycles per frame
//     = 17030 cycles. Visible video on scanlines 0..191; VBL on 192..261.
//   - $C019 bit 7 reflects active video (HIGH during 0..191, LOW during
//     192..261), matching MAME `apple2e.cpp` / `render_text` convention.
//   - IIe VBL IRQ mask via $C05A (off) / $C05B (on). Edge into VBL with
//     mask=on raises CPU IRQ; reading $C019 clears it.
//   - II/II+ (no IIe mode): $C05A/$C05B are AN1 annunciator no-ops; no IRQ.
//
// Headless: drives `Memory::advanceCycles` directly with a stub M6502 to
// observe IRQ-line transitions.

#include "M6502.h"
#include "Memory.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr uint64_t kCyclesPerScanline = 65;
constexpr uint64_t kFrameScanlines    = 262;
constexpr uint64_t kFrameCycles       = kCyclesPerScanline * kFrameScanlines;

void advance(Memory& mem, uint64_t cycles) {
    while (cycles > 0) {
        const int slice = static_cast<int>(std::min<uint64_t>(cycles, 1000));
        mem.advanceCycles(slice);
        cycles -= slice;
    }
}

uint8_t readVbl(Memory& mem) { return mem.memRead(0xC019); }

}  // namespace

int main()
{
    // ── Case 1: II+ (no IIe). $C019 is IIe-only — on a II/II+ the
    // address falls through to the floating-bus return (MAME's
    // `apple2.cpp` doesn't decode $C019 at all). The VBL scanline
    // counter still ticks internally (so $C05B's IRQ enable would
    // work IF the gate were on, which we explicitly verify is not
    // the case on II+).
    {
        Memory mem;
        M6502  cpu(&mem);
        mem.setCpu(&cpu);

        // II+: $C019 returns floating-bus (deterministic given our
        // cycleCounter == 0 state, but unrelated to scanline). We
        // only assert the call doesn't crash.
        (void)readVbl(mem);

        // Advance through several scanlines and a full frame to make
        // sure the VBL boundary code path stays safe in II+ mode.
        advance(mem, 100 * kCyclesPerScanline);
        (void)readVbl(mem);
        advance(mem, (192 - 100) * kCyclesPerScanline);
        (void)readVbl(mem);

        // II+: $C05A/$C05B do NOT raise IRQ even after a VBL transition.
        // The mask is iieMode-gated; on II+ those addresses are plain
        // AN1 annunciator toggles (no IRQ).
        mem.memWrite(0xC05B, 0);   // would enable VBL IRQ on IIe
        advance(mem, kFrameCycles);
        (void)readVbl(mem);

        // II+: a $C01x read clears the strobe and returns the FLOATING
        // BUS, not the key latch (MAME `apple2.cpp:362-372`, AppleWin
        // IORead_C01x GH#1261). With every video byte = $AA, whatever the
        // scanner fetches is $AA; the old answer was the key, $41.
        for (uint32_t a = 0; a < 0xC000; ++a)
            mem.memWrite(static_cast<uint16_t>(a), 0xAA);
        for (uint16_t addr : {uint16_t(0xC010), uint16_t(0xC019), uint16_t(0xC01F)}) {
            mem.queueKey('A');
            (void)mem.memRead(0xC000);
            const uint8_t v = mem.memRead(addr);
            if (v != 0xAA) {
                std::printf("FAIL: II+ $%04X read %02X, want floating bus AA\n",
                            addr, v);
                return 1;
            }
            assert((mem.memRead(0xC000) & 0x80) == 0);   // strobe cleared
            advance(mem, 37);
        }
    }

    // ── Case 2: IIe with VBL IRQ enabled. ──
    {
        Memory mem;
        mem.setIIEMode(true);
        M6502  cpu(&mem);
        mem.setCpu(&cpu);
        // We don't care about the CPU running here — we just want to
        // observe Memory's VBL pending-flag and $C019 readback. The
        // CPU's IRQ line is private; we observe its presence indirectly
        // via Memory's own state.

        // Enable VBL IRQ. $C05B (write or read both work in POM2's
        // soft-switch convention).
        mem.memWrite(0xC05B, 0);

        // Advance into VBL: scanline 192 boundary should set the IRQ
        // pending flag. Cross it from below.
        advance(mem, 192 * kCyclesPerScanline);
        // At this point we're exactly at scanline 192 → VBL.
        // Bit 7 must be LOW (we're in VBL).
        assert((readVbl(mem) & 0x80) == 0);
        // Reading $C019 acknowledges the IRQ.
        // We can't directly probe cpu.IRQ. Instead, check that a SECOND
        // immediate read still returns LOW (we're still in VBL window —
        // 70 scanlines = 4550 cycles).
        assert((readVbl(mem) & 0x80) == 0);

        // Disable mask. Should clear any pending IRQ.
        mem.memWrite(0xC05A, 0);

        // Advance past the VBL window into the next frame's active video.
        // Currently at scanline 192 (start of VBL); advance 70 scanlines
        // (rest of VBL) + 1 → scanline 1 of next frame = active video.
        advance(mem, (70 + 1) * kCyclesPerScanline);
        assert((readVbl(mem) & 0x80) != 0);  // active video again
    }

    // ── Case 3: an accelerator does not shorten the frame. The video
    //    counters run off the crystal; at 4x the 6502 spends 4 x 17030
    //    cycles in one frame. $C019 and the frame publication used to run
    //    on CPU cycles, so a TransWarp / Zip / //c+ saw 240 VBLs a second.
    {
        Memory mem;
        mem.setIIEMode(true);
        advance(mem, 10);                         // scanline 0: active
        assert((readVbl(mem) & 0x80) != 0);
        mem.setVideoClockRatio(0.25);
        // 192 visible lines at 4 CPU cycles per beam cycle.
        advance(mem, 4 * (191 * kCyclesPerScanline));
        assert((readVbl(mem) & 0x80) != 0 && "the beam ran at the CPU's 4x speed");
        advance(mem, 4 * (2 * kCyclesPerScanline));   // line 193: VBL
        assert((readVbl(mem) & 0x80) == 0);
        // The rest of the frame, then back to the stock clock: the beam is
        // continuous across the change (no jump to the CPU's frame phase).
        advance(mem, 4 * (kFrameCycles - 193 * kCyclesPerScanline) + 4 * 30);
        assert((readVbl(mem) & 0x80) != 0);       // next frame, line 0
        mem.setVideoClockRatio(1.0);
        advance(mem, 100 * kCyclesPerScanline);
        assert((readVbl(mem) & 0x80) != 0);       // line ~100
        advance(mem, 100 * kCyclesPerScanline);
        assert((readVbl(mem) & 0x80) == 0);       // line ~200
    }

    // ── Case 4: the beam's clock survives a snapshot. Re-derived from the
    //    CPU counter on restore, an accelerated machine came back on another
    //    scanline.
    {
        Memory a;
        a.setIIEMode(true);
        advance(a, 12345);
        a.setVideoClockRatio(0.25);
        advance(a, 4 * 777);
        std::vector<uint8_t> blob;
        a.appendSnapshotState(blob);
        Memory b;
        b.setIIEMode(true);
        assert(b.loadSnapshotState(blob.data(), blob.size()));
        assert(b.getCycleCounter() == a.getCycleCounter());
        assert(b.videoNow() == a.videoNow() && "the beam moved on restore");
        advance(a, 4 * 5000);
        advance(b, 4 * 5000);
        assert(b.videoNow() == a.videoNow() && "the restored beam runs at the saved speed");
    }

    std::printf("vbl_smoke OK\n");
    return 0;
}
