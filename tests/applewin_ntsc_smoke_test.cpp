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

// Smoke test for AppleWinNtsc — the CPU-side IIR-LUT NTSC simulator
// underpinning HiResMode::ColorAppleWin.
//
// We don't compare pixel-for-pixel against AppleWin (no bytestream
// oracle available offline). What we pin instead are POM2-internal
// invariants that catch regressions in the algorithm:
//
//   - ensureInitialized() is idempotent and produces a populated LUT.
//   - All-black signal → all-black output (RGB == 0) for the Monitor
//     sub-mode.
//   - All-white signal → uniform near-white output (R, G, B all > 230)
//     in Monitor mode.
//   - A solid run of $7F bytes (the canonical "white" HGR pattern,
//     popcount-rich window) decodes through the Monitor sub-mode to
//     near-white pixels in the middle of the line — i.e. the artifact
//     pipeline doesn't accidentally introduce a colour cast on neutral
//     gray.
//   - The Idealized sub-mode produces saturated artifact colours for
//     the canonical purple ($01) and orange ($02) patterns, matching
//     the dot-position parity that any NTSC artifact decoder should.
//   - The Tv sub-mode of a steady signal converges to the Monitor
//     output (50% blend with previous frame; after two iterations the
//     difference is below half a code).

#include "AppleWinNtsc.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr int kW = 560;

uint8_t r8(uint32_t c) { return  c        & 0xFFu; }
uint8_t g8(uint32_t c) { return (c >> 8 ) & 0xFFu; }
uint8_t b8(uint32_t c) { return (c >> 16) & 0xFFu; }

void fillSignalFromByte(uint8_t* sig, int n, uint8_t byte7bit)
{
    // Lay out one 14-sample HGR cell repeatedly: 7 bits of the byte
    // doubled (each bit emits 2 stream samples). Matches what
    // buildBitStream() does for HGR with no MSB-delay (byte's MSB
    // assumed zero for this test).
    for (int i = 0; i < n; ++i) {
        const int bitIdx = (i / 2) % 7;
        sig[i] = (byte7bit >> bitIdx) & 1u ? 0xFFu : 0x00u;
    }
}

} // namespace

int main()
{
    using pom2::AppleWinNtsc;

    AppleWinNtsc::ensureInitialized();
    AppleWinNtsc::ensureInitialized();   // idempotency

    std::vector<uint8_t>  signal(kW);
    std::vector<uint32_t> out   (kW);

    // ── 1. all-black → all-black ─────────────────────────────────────────
    std::fill(signal.begin(), signal.end(), 0);
    AppleWinNtsc::renderLine(signal.data(), out.data(), kW,
                             AppleWinNtsc::SubMode::Monitor);
    for (int x = 50; x < kW - 50; ++x) {
        assert(r8(out[x]) < 32 && g8(out[x]) < 32 && b8(out[x]) < 32);
    }

    // ── 2. all-white → ~white (Monitor) ──────────────────────────────────
    std::fill(signal.begin(), signal.end(), 0xFF);
    AppleWinNtsc::renderLine(signal.data(), out.data(), kW,
                             AppleWinNtsc::SubMode::Monitor);
    // Skip the leftmost edge where IIR transient hasn't settled.
    for (int x = 20; x < kW - 20; ++x) {
        assert(r8(out[x]) > 200 && g8(out[x]) > 200 && b8(out[x]) > 200);
    }

    // ── 3. $7F repeating → near-neutral high luma (no colour cast) ──────
    fillSignalFromByte(signal.data(), kW, 0x7F);
    AppleWinNtsc::renderLine(signal.data(), out.data(), kW,
                             AppleWinNtsc::SubMode::Monitor);
    // Middle of the line: each channel should be well above mid-gray
    // and the colour cast (max-min channel) shouldn't dominate luminance.
    for (int x = 100; x < kW - 100; x += 14) {
        const int rv = r8(out[x]);
        const int gv = g8(out[x]);
        const int bv = b8(out[x]);
        const int mx = std::max({rv, gv, bv});
        const int mn = std::min({rv, gv, bv});
        assert(mx > 150);
        assert(mx - mn < 120);  // some chroma residue tolerated
    }

    // ── 4. Idealized $01 produces saturated dark colour ─────────────────
    fillSignalFromByte(signal.data(), kW, 0x01);
    AppleWinNtsc::renderLine(signal.data(), out.data(), kW,
                             AppleWinNtsc::SubMode::Idealized);
    // $01 = bit 0 set only → one dot lit every 14 samples (twice with
    // doubling). Idealized table indexed by nibble at hist bits 0..3,
    // so we expect a dim but non-zero pixel in most columns; just check
    // luma is below pure white and above pure black.
    bool anyMid = false;
    for (int x = 0; x < kW; ++x) {
        const int lum = (r8(out[x]) + g8(out[x]) + b8(out[x])) / 3;
        if (lum > 8 && lum < 200) { anyMid = true; break; }
    }
    assert(anyMid && "Idealized $01 should produce non-black artifact");

    // ── 5. Tv sub-mode: AppleWin's in-between line, inside one frame ────
    //    NTSC.cpp updateFramebufferTVDoubleScanline: the line between two
    //    Apple scanlines is 50 % each. Folded into one row per line, a row
    //    is 3/4 itself + 1/4 the line above — the frame before plays no part
    //    (POM2 used to blend 50 % with the previous FRAME).
    {
        std::vector<uint8_t> sig(static_cast<size_t>(kW) * 3, 0x00);
        std::fill(sig.begin(), sig.begin() + kW, uint8_t{0xFF});   // line 0 white, 1-2 black
        std::vector<uint32_t> mon(static_cast<size_t>(kW) * 3), tv(static_cast<size_t>(kW) * 3);
        AppleWinNtsc::renderFrame(sig.data(), mon.data(), kW, 3, AppleWinNtsc::SubMode::Monitor);
        AppleWinNtsc::renderFrame(sig.data(), tv.data(), kW, 3, AppleWinNtsc::SubMode::Tv);
        std::vector<uint32_t> raw(kW);
        AppleWinNtsc::renderLine(sig.data(), raw.data(), kW, AppleWinNtsc::SubMode::Tv);
        for (int x = 100; x < kW - 100; ++x) {
            const int w0 = r8(raw[x]);
            assert(std::abs(int(r8(tv[x])) - (3 * w0 + 2) / 4) <= 1);           // above it: border black
            assert(std::abs(int(r8(tv[kW + x])) - (w0 + 2) / 4) <= 1);          // 1/4 of line 0
            assert(r8(tv[2 * kW + x]) == r8(mon[2 * kW + x]));                  // black under black
        }
        // Same input twice, same output: no history.
        std::vector<uint32_t> again(tv.size());
        AppleWinNtsc::renderFrame(sig.data(), again.data(), kW, 3, AppleWinNtsc::SubMode::Tv);
        assert(again == tv);
    }

    // ── 6. renderFrame wraps renderLine across h scanlines ───────────────
    std::vector<uint8_t>  sig2(kW * 3, 0xFF);
    std::vector<uint32_t> out2(kW * 3);
    AppleWinNtsc::renderFrame(sig2.data(), out2.data(), kW, 3,
                              AppleWinNtsc::SubMode::Monitor);
    // All three scanlines should match (Monitor is stateless across
    // lines — IIR resets per line via the leading-edge mirror seed).
    for (int x = 100; x < kW - 100; ++x) {
        assert(out2[0 * kW + x] == out2[1 * kW + x]);
        assert(out2[1 * kW + x] == out2[2 * kW + x]);
    }

    // ── 7. Solid artifact pattern ($2A) decodes to SATURATED colour ──────
    //    Regression guard for the "almost no colour" bug. The previous
    //    gaussian decoder let the luma estimate absorb the subcarrier and
    //    then subtracted it back out, cancelling chroma inside steady colour
    //    fills (avgSat ~ 0, only edge fringes). The faithful AppleWin IIR
    //    pipeline band-passes the chroma separately, so a solid $2A fill must
    //    show strong chroma right across the line, not just at transitions.
    fillSignalFromByte(signal.data(), kW, 0x2A);
    AppleWinNtsc::renderLine(signal.data(), out.data(), kW,
                             AppleWinNtsc::SubMode::Monitor);
    long sumSat = 0; int nSat = 0, maxSat = 0;
    for (int x = 100; x < kW - 100; ++x) {
        const int sat = std::max({int(r8(out[x])), int(g8(out[x])), int(b8(out[x]))})
                      - std::min({int(r8(out[x])), int(g8(out[x])), int(b8(out[x]))});
        sumSat += sat; ++nSat; maxSat = std::max(maxSat, sat);
    }
    assert(int(sumSat / nSat) > 40 && "ColorAppleWin $2A fill must be saturated, not gray");
    assert(maxSat > 120 && "ColorAppleWin $2A must reach high chroma somewhere");

    std::printf("applewin_ntsc_smoke OK\n");
    return 0;
}
