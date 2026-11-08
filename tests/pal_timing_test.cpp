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

// PAL 50 Hz machine timing — pins the NTSC↔PAL video geometry that French
// Touch / DIX (PAL, 312 lines) need. Three checks:
//   1. the VideoTiming table constants (NTSC 262/60, PAL 312/50/1.0156 MHz);
//   2. Apple2Display::frameCycleToPos maps a cycle in the 262..311 line band to
//      a DIFFERENT scanline under PAL vs NTSC (the 262↔312 frame wrap), while
//      the byte column (65-cycle line) is identical;
//   3. Memory::pushVideoEventLocked stamps soft-switch edges with the active
//      standard's geometry, so beam-racing positions PAL effects correctly.

#include "Apple2Display.h"
#include "CpuClock.h"
#include "M6502.h"
#include "Memory.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {
constexpr uint16_t SET_TEXT = 0xC051;
}

int main()
{
    // ── 1. Timing table. ─────────────────────────────────────────────────
    const VideoTiming& n = pom2VideoTiming(VideoStandard::NTSC);
    const VideoTiming& p = pom2VideoTiming(VideoStandard::PAL);
    assert(n.scanlinesPerFrame == 262 && n.refreshHz == 60 && n.cyclesPerFrame == 17030);
    assert(p.scanlinesPerFrame == 312 && p.refreshHz == 50 && p.cyclesPerFrame == 20280);
    // One tick = one video frame, paced at frame / clock (2026-11-07).
    assert(pom2FrameIntervalNs(p) == 19968000 && pom2FrameIntervalNs(n) == 16651560);
    // Each tick ends ON a frame boundary (CpuClock.h § cyclesPerFrame).
    {
        const int64_t f = p.cyclesPerFrame;
        // In phase: a whole frame.
        assert(pom2AlignedTickBudget(f, f, 7 * uint64_t(f)) == f);
        // The last instruction overshot by 5: the next tick is 5 short.
        assert(pom2AlignedTickBudget(f, f, 7 * uint64_t(f) + 5) == f - 5);
        // Arbitrary first phase past half a frame: reach the next boundary.
        assert(pom2AlignedTickBudget(f, f, 7 * uint64_t(f) + 15000) == f - 15000 + f);
        // ... and before it: the boundary one frame on.
        assert(pom2AlignedTickBudget(f, f, 7 * uint64_t(f) + 3000) == f - 3000);
        // The 2x preset ends on a boundary too; a custom budget is untouched.
        assert(pom2AlignedTickBudget(2 * f, f, 7 * uint64_t(f) + 5) == 2 * f - 5);
        assert(pom2AlignedTickBudget(30000, f, 12345) == 30000);
        // Over a run of ticks with random overshoot, every tick ENDS on a
        // boundary and the machine still advances exactly one frame a tick.
        uint64_t now = 12345, seed = 1;
        for (int tick = 0; tick < 1000; ++tick) {
            const int64_t b = pom2AlignedTickBudget(f, f, now);
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            const uint64_t overshoot = (seed >> 33) % 7;          // 0-6 cycles
            now += uint64_t(b) + overshoot;
            assert(now % uint64_t(f) == overshoot);
        }
        // The first tick ran from phase 12345 past half a frame to the
        // SECOND boundary; every other tick was exactly one frame.
        assert(now / uint64_t(f) == 1001);   // no lap lost, none gained
    }
    assert(p.cpuClockHz == 1015625 && n.cpuClockHz == 1022727);
    assert(n.cyclesPerScanline == 65 && p.cyclesPerScanline == 65);
    assert(n.visibleScanlines == 192 && p.visibleScanlines == 192);
    // Effective clock = cyclesPerFrame × refreshHz lands near the real rates.
    // The effective clock is the standard's own: one frame per frame-long
    // tick (it was budget x nominal refresh, 1 015 650 / 1 022 700, until
    // 2026-11-08).
    auto effectiveHz = [](const VideoTiming& t) {
        return double(t.cyclesPerFrame) * 1e9 / double(pom2FrameIntervalNs(t));
    };
    assert(std::fabs(effectiveHz(p) - 1015625.0) < 0.01);
    assert(std::fabs(effectiveHz(n) - 1022727.0) < 0.1);

    // ── 2. frameCycleToPos geometry. ─────────────────────────────────────
    // Absolute line 280: NTSC wraps (280 % 262 = 18 → visible line 18); PAL
    // does not (280 % 312 = 280, beyond the 192 visible band → clamped 191).
    const uint64_t cyc280 = static_cast<uint64_t>(280) * 65 + 45;  // hpos 45
    auto ntsc = Apple2Display::frameCycleToPos(cyc280, VideoStandard::NTSC);
    auto pal  = Apple2Display::frameCycleToPos(cyc280, VideoStandard::PAL);
    assert(ntsc.scanline == 18  && "NTSC: line 280 wraps to 18");
    assert(pal.scanline  == 192 && "PAL: line 280 is VBL → frame-end stamp 192 "
                                   "(mirrors pushVideoEventLocked)");
    // 21, not 20: the switch→column mapping is `hpos - 24` (see
    // Apple2Display::frameCycleToPos). What this line pins is that the
    // value does not depend on the video standard.
    assert(ntsc.byteCol == 21 && pal.byteCol == 21 && "byteCol independent of standard");

    // A visible-region line is identical under both standards (no regression).
    const uint64_t cyc100 = static_cast<uint64_t>(100) * 65 + 45;
    assert(Apple2Display::frameCycleToPos(cyc100, VideoStandard::NTSC).scanline == 100);
    assert(Apple2Display::frameCycleToPos(cyc100, VideoStandard::PAL).scanline  == 100);

    // Default arg is NTSC (existing call sites unchanged).
    assert(Apple2Display::frameCycleToPos(cyc280).scanline == 18);

    // ── 3. Memory stamps the active standard's scanline. ─────────────────
    auto recordedScanline = [](VideoStandard std, int absLine) {
        Memory mem;
        mem.setVideoStandard(std);
        assert(mem.videoStandard() == std);
        mem.setCycleCounter(static_cast<uint64_t>(absLine) * 65);
        mem.beginVideoEventFrame();
        mem.memRead(SET_TEXT);                 // logs a TextMode edge
        auto evs = mem.takeVideoEvents();
        assert(evs.size() == 1);
        return static_cast<int>(evs[0].scanline);
    };
    // VBL lines stamp as 192 ("frame end" — excluded from the visible
    // replay; see Memory::pushVideoEventLocked). They used to clamp to
    // 191, painting a spurious split on the last visible line.
    assert(recordedScanline(VideoStandard::NTSC, 280) == 18);   // 280 % 262
    assert(recordedScanline(VideoStandard::PAL,  280) == 192);  // 280 % 312 → VBL
    assert(recordedScanline(VideoStandard::NTSC, 100) == 100);
    assert(recordedScanline(VideoStandard::PAL,  100) == 100);
    // A line that only "wraps" under NTSC: absolute 300 → NTSC 300%262=38,
    // PAL 300%312=300 → VBL stamp 192. Proves the geometry is 312 for PAL.
    assert(recordedScanline(VideoStandard::NTSC, 300) == 38);
    assert(recordedScanline(VideoStandard::PAL,  300) == 192);

    // ── 4. $C019 VBL frame period follows the standard. ──────────────────
    // A loader that measures the VBL period to detect PAL vs NTSC must see a
    // 20280-cycle frame under PAL (312×65), not 17030: lines 262..311 exist
    // only on PAL — under NTSC the same absolute cycle has already wrapped
    // into the next frame's active video. Bit 7 of $C019 = active video
    // (IIe RDVBLBAR convention, see Memory::softSwitchAccess).
    auto vblActiveAt = [](VideoStandard std, uint64_t absLine) {
        Memory mem;
        M6502  cpu(&mem);
        mem.setCpu(&cpu);
        mem.setIIEMode(true);
        mem.setVideoStandard(std);
        mem.setCycleCounter(absLine * 65);
        return (mem.memRead(0xC019) & 0x80) != 0;
    };
    assert(vblActiveAt(VideoStandard::NTSC, 191) == true);
    assert(vblActiveAt(VideoStandard::NTSC, 192) == false);  // VBL entry
    assert(vblActiveAt(VideoStandard::NTSC, 261) == false);  // VBL tail
    assert(vblActiveAt(VideoStandard::NTSC, 262) == true);   // frame wrapped
    assert(vblActiveAt(VideoStandard::PAL,  192) == false);
    assert(vblActiveAt(VideoStandard::PAL,  262) == false);  // still VBL on PAL
    assert(vblActiveAt(VideoStandard::PAL,  311) == false);  // VBL tail
    assert(vblActiveAt(VideoStandard::PAL,  312) == true);   // 20280 → wrap

    // ── 5. Floating bus follows the PAL geometry. ───────────────────────
    // The video scanner's vertical counter runs $FA..$1FF on NTSC (262
    // lines) and $C8..$1FF on PAL (312) — the extra 50 PAL lines land in
    // VBL and the scanner keeps fetching through them. A stray hard-coded
    // 262 in floatingBus() would be SILENT (a wrong RNG byte, a vapor-lock
    // that never fires), which is exactly why it needs pinning: this is
    // the one PAL-geometry consumer the rest of this test doesn't reach.
    {
        // Fill RAM so each byte encodes its own address — otherwise a
        // uniform RAM makes two DIFFERENT scanner addresses return the
        // same byte and the comparison below proves nothing.
        auto busAt = [](VideoStandard std, uint64_t absCycle) {
            Memory mem;
            M6502  cpu(&mem);
            mem.setCpu(&cpu);
            mem.setIIEMode(true);
            mem.setVideoStandard(std);
            for (uint32_t a = 0x0400; a < 0x0C00; ++a)
                mem.memWrite(static_cast<uint16_t>(a),
                             static_cast<uint8_t>(a ^ (a >> 8)));
            for (uint32_t a = 0x2000; a < 0x6000; ++a)
                mem.memWrite(static_cast<uint16_t>(a),
                             static_cast<uint8_t>(a ^ (a >> 8)));
            mem.setCycleCounter(absCycle);
            return mem.peekFloatingBus();
        };
        // Line 280 exists on both, but is a DIFFERENT point in the
        // scanner's counter sequence: NTSC has wrapped (280 % 262 = 18,
        // visible), PAL has not (still in VBL). The scanner address, and
        // so the byte, must differ.
        const uint64_t cyc = static_cast<uint64_t>(280) * 65 + 20;
        assert(busAt(VideoStandard::NTSC, cyc) != busAt(VideoStandard::PAL, cyc)
               && "floatingBus must use the live scanlinesPerFrame");

        // The PAL frame repeats with period 312 × 65 = 20280 cycles, NOT
        // the 20280 CPU budget per worker tick (those are deliberately
        // decoupled — see CpuClock.h).
        constexpr uint64_t kPalFrame = 312ull * 65;
        for (uint64_t probe : {0ull, 4321ull, 15000ull}) {
            assert(busAt(VideoStandard::PAL, probe) ==
                   busAt(VideoStandard::PAL, probe + kPalFrame)
                   && "PAL floating bus period must be 20280 cycles");
        }
    }

    std::printf("pal_timing OK\n");
    return 0;
}
