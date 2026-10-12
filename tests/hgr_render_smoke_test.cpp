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

// Smoke test for Apple2Display's HGR pipeline (3.0 — MAME-style decoder).
//
// The colour path follows MAME apple2video.cpp by benrg (PR #10773):
//   1. Build a 14-bit-per-byte stream (each visible HGR bit doubled);
//      MSB=1 inserts a 1-sub-pixel half-dot delay.
//   2. Walk a 7-bit sliding window with 3-bit left context.
//   3. Look up a 128-entry static artifact LUT (verbatim from MAME);
//      apply rotl4b(entry, absoluteX) to extract the lo-res palette
//      index for the current NTSC phase.
//   4. The 4-bit result indexes Apple2Display::kLoResPalette (also from
//      MAME) — same 16-colour table that drives the lo-res mode.
//   5. Pair sub-pixels into 280 framebuffer pixels via RGB averaging.
//
// Test colour expectations therefore match MAME's reference palette:
//   - Purple      = lo-res 3   (rgb 0xe6, 0x28, 0xff)
//   - Light Green = lo-res 12  (rgb 0x19, 0xd7, 0x00)
//   - Orange      = lo-res 9   (rgb 0xe6, 0x6f, 0x00)
//
// What this test pins:
//   - black byte ($00)              → 7 black pixels
//   - $7F at col 0                  → 7 white pixels (popcount-rich
//                                     window → LUT entry 0xff)
//   - $01 isolated, group 1, even   → pixel 0 = MAME purple
//                                     pixels 1..6 = black
//   - $01 isolated, group 1, odd    → pixel 7 = MAME light green
//   - $81 isolated (MSB-shifted) col 1 → orange-leaning pixel 7
//                                     (R dominant, G mid, B near 0)
//   - $40 + $01 inter-byte seam     → pixels 6,7 paint white
//   - Mono Green: $7F row → P31-tinted phosphor; clear next frame
//                                     leaves a decayed afterglow
//   - Mono Amber: amber tint with strong persistence (>90% retention)
//   - Mode switch clears persistence buffer

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

constexpr uint32_t kBlack  = 0xFF000000u;
constexpr uint32_t kWhite  = 0xFFFFFFFFu;

constexpr uint32_t pack(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | r;
}
// MAME palette entries used in the assertions below.
constexpr uint32_t kPurple     = pack(0xe6, 0x28, 0xff);   // lo-res 3
constexpr uint32_t kLightGreen = pack(0x19, 0xd7, 0x00);   // lo-res 12

void writeHgrByte(Memory& mem, int y, int col, uint8_t v) {
    const uint16_t addr = static_cast<uint16_t>(0x2000
        + 0x400 * (y & 7)
        + 0x80  * ((y >> 3) & 7)
        + 0x28  * (y >> 6));
    mem.memWrite(static_cast<uint16_t>(addr + col), v);
}

void clearScanline(Memory& mem, int y) {
    for (int col = 0; col < 40; ++col) writeHgrByte(mem, y, col, 0x00);
}

const uint32_t* pixelAt(const Apple2Display& d, int x, int y) {
    return d.pixels() + (y * d.width() + x);
}

uint8_t r8(uint32_t c) { return  c        & 0xFFu; }
uint8_t g8(uint32_t c) { return (c >> 8 ) & 0xFFu; }
uint8_t b8(uint32_t c) { return (c >> 16) & 0xFFu; }

} // namespace

// A colour fill is a dot pattern on a monochrome monitor, even when bit 7
// delays HGR by half a dot. In particular, orange must not become flat grey.
static void testMonochromeDotPatterns()
{
    for (auto mode : {Apple2Display::HiResMode::MonoWhite,
                      Apple2Display::HiResMode::MonoGreen,
                      Apple2Display::HiResMode::MonoAmber}) {
        const uint32_t litColour = mode == Apple2Display::HiResMode::MonoWhite
            ? kWhite : mode == Apple2Display::HiResMode::MonoGreen
            ? pack(0x33, 0xFF, 0x33) : pack(0xFF, 0xB0, 0);
        for (int delay = 0; delay < 2; ++delay) {
            for (int opposite = 0; opposite < 2; ++opposite) {
                Memory mem;
                mem.memRead(0xC050); mem.memRead(0xC052); mem.memRead(0xC057);
                for (int col = 0; col < 40; ++col)
                    writeHgrByte(mem, 0, col,
                        (((col & 1) ^ opposite) ? 0x55 : 0x2A) | (delay ? 0x80 : 0));
                Apple2Display d; d.setHiResMode(mode); d.render(mem);
                assert(d.width() == 560 && "mono must retain half-dot resolution");
                for (int x = 20; x < 540; ++x) {
                    const bool on = ((((x - delay) / 2) & 1) ^ opposite) != 0;
                    assert(d.pixels()[x] == (on ? litColour : kBlack));
                }
            }
        }
        for (int nibble = 0; nibble < 16; ++nibble) {
            Memory mem;
            mem.memRead(0xC050); mem.memRead(0xC052); mem.memRead(0xC056);
            for (int col = 0; col < 40; ++col)
                mem.memWrite(0x400 + col, nibble | (nibble << 4));
            Apple2Display d; d.setHiResMode(mode); d.render(mem);
            assert(d.width() == 560);
            for (int x = 0; x < 560; ++x)
                assert(d.pixels()[x] == ((nibble & (1 << (x & 3))) ? litColour : kBlack));
        }
    }
}

int main()
{
    testMonochromeDotPatterns();
    Memory mem;
    Apple2Display display;
    // Raw LUT output — easy to assert exactly. The CPU-side glow has been
    // retired; the GPU CRT shader (handled in MainWindow) does halation now.

    // Force HGR mode: $C050 set graphics, $C057 set hi-res, page 1.
    (void)mem.memRead(0xC050);
    (void)mem.memRead(0xC057);
    (void)mem.memRead(0xC054);

    // ── Black byte: 7 black pixels ───────────────────────────────────────
    clearScanline(mem, 0);
    display.render(mem);
    for (int x = 0; x < 7; ++x) assert(*pixelAt(display, x, 0) == kBlack);

    // ── $7F at col 0 (group 1): all 7 pixels white. The new renderer
    //    achieves this because every 7-bit window over a fully-lit run
    //    has either popcount ≥ 6 or all four NTSC phases lit, both of
    //    which short-circuit to white in the LUT.
    clearScanline(mem, 1);
    writeHgrByte(mem, 1, 0, 0x7F);
    display.render(mem);
    for (int x = 0; x < 7; ++x) {
        const uint32_t got = *pixelAt(display, x, 1);
        assert(got == kWhite);
    }

    // ── $01 at col 0 (group 1, even parity): pixel 0 = MAME purple ──────
    //    Doubled byte = 0x0003 → window 0x18 at sub 0 → lut[0x18] = 0x33
    //    → rotl4b(0x33, 0) = 0x3 → lo-res Purple. Remaining pixels stay
    //    black (windows past sub 1 see all-zero bytes).
    clearScanline(mem, 2);
    writeHgrByte(mem, 2, 0, 0x01);
    display.render(mem);
    assert(*pixelAt(display, 0, 2) == kPurple);
    for (int x = 1; x < 7; ++x) assert(*pixelAt(display, x, 2) == kBlack);

    // ── $01 at col 1 (group 1, odd parity): pixel 7 = MAME light green ───
    //    Same window pattern as above but at absX=14 → rotl4b(0x33, 14)
    //    = (0x33 >> 2) & 0xf = 0xC → lo-res Light Green.
    clearScanline(mem, 3);
    writeHgrByte(mem, 3, 1, 0x01);
    display.render(mem);
    assert(*pixelAt(display, 7, 3) == kLightGreen);

    // ── $81 at col 1: isolated bit on the MSB-shifted (group 2) palette.
    //    The half-dot delay shifts bit 0 by 1 sub-pixel — pixel 7 ends
    //    up averaging a black sub-pixel with an orange one, giving
    //    a dim-orange chroma. We assert R-dominant (orange) rather than
    //    exact equality, because the 50/50 average lands between LUT
    //    entries.
    clearScanline(mem, 5);
    writeHgrByte(mem, 5, 1, 0x81);
    display.render(mem);
    {
        const uint32_t got = *pixelAt(display, 7, 5);
        // MAME orange = (0xe6, 0x6f, 0x00). Halved: (~115, ~55, 0).
        // Test: R dominant, R > G > B, R well above black.
        assert(r8(got) > g8(got));
        assert(g8(got) >= b8(got));
        assert(r8(got) > 60);
    }

    // ── Inter-byte seam: $40 (bit 6 lit) at col 0 + $01 (bit 0 lit) at col 1
    //    → seam pixels 6 and 7 both paint white. With the bit stream the
    //    four contiguous lit sub-pixels at the boundary cover all 4
    //    phases simultaneously → chroma cancels → white.
    clearScanline(mem, 6);
    writeHgrByte(mem, 6, 0, 0x40);
    writeHgrByte(mem, 6, 1, 0x01);
    display.render(mem);
    assert(*pixelAt(display, 6, 6) == kWhite);
    assert(*pixelAt(display, 7, 6) == kWhite);

    // ── MSB-toggle fringe. $55 in col 0 (group 1), $D5 in col 1 (group 2,
    //    same visible bits + MSB → half-dot delay kicks in). MAME's LUT
    //    produces a transition pattern at the boundary: not pure purple,
    //    not black. We just assert that at least one pixel in the
    //    boundary region is colourful and different from pure purple.
    clearScanline(mem, 8);
    writeHgrByte(mem, 8, 0, 0x55);
    writeHgrByte(mem, 8, 1, 0xD5);     // $55 + MSB
    display.render(mem);
    {
        bool sawTransition = false;
        for (int x = 5; x <= 9; ++x) {
            const uint32_t got = *pixelAt(display, x, 8);
            if (got != kBlack && got != kPurple) {
                sawTransition = true;
                break;
            }
        }
        assert(sawTransition);
    }

    // ── Full-row $7F: every pixel white (popcount-rich windows hit the
    //    LUT's all-on entries 0xff → rotl4b → palette index 15).
    clearScanline(mem, 7);
    for (int col = 0; col < 40; ++col) writeHgrByte(mem, 7, col, 0x7F);
    display.render(mem);
    for (int x = 0; x < 280; ++x) assert(*pixelAt(display, x, 7) == kWhite);

    // ── Mono Green (P31): $7F row tints to phosphor green ────────────────
    //    P31 phosphor = (0x33, 0xFF, 0x33). With a fully-lit row every
    //    pixel lands on full luminance × phosphor.
    display.setHiResMode(Apple2Display::HiResMode::MonoGreen);
    clearScanline(mem, 10);
    for (int col = 0; col < 40; ++col) writeHgrByte(mem, 10, col, 0x7F);
    display.render(mem);
    {
        const uint32_t got = *pixelAt(display, 100, 10);
        assert(r8(got) == 0x33 && g8(got) == 0xFF && b8(got) == 0x33);
    }

    // ── Mono Green afterglow: clear and re-render → expect ~85% retention.
    //    The history buffer's max-of-target-and-decayed rule means the
    //    new (black) frame keeps decay×prev = 0.85 × 255 ≈ 216.
    //    Decay is paced by EMULATED frames (render() derives the frame
    //    index from cycleCounter), so step the machine one video frame —
    //    re-rendering the same frozen frame must NOT decay.
    clearScanline(mem, 10);
    mem.advanceCycles(65 * 262);   // one NTSC video frame
    display.render(mem);
    {
        const uint32_t got = *pixelAt(display, 100, 10);
        // Green channel should decay to ~217 ± a few.
        assert(g8(got) > 200 && g8(got) < 230);
        // Red and blue should follow the same scaling on the green tint
        // (R = 0x33 × 0.85 ≈ 0x2B = 43).
        assert(r8(got) > 30 && r8(got) < 60);
    }

    // ── Mono Amber: longer persistence (decay = 0.96) ────────────────────
    display.setHiResMode(Apple2Display::HiResMode::MonoAmber);
    clearScanline(mem, 11);
    for (int col = 0; col < 40; ++col) writeHgrByte(mem, 11, col, 0x7F);
    display.render(mem);
    {
        const uint32_t got = *pixelAt(display, 100, 11);
        assert(r8(got) == 0xFF && g8(got) == 0xB0 && b8(got) == 0x00);
    }
    clearScanline(mem, 11);
    mem.advanceCycles(65 * 262);   // one NTSC video frame
    display.render(mem);
    {
        const uint32_t got = *pixelAt(display, 100, 11);
        // Amber decay 0.96 × 255 ≈ 244.
        assert(r8(got) > 235 && r8(got) <= 255);
    }
    // Same frozen frame re-rendered (no emulated time passed) → NO decay:
    // pins the host-refresh independence (a 144 Hz monitor re-renders the
    // same emulated frame 2-3×; afterglow speed must not change).
    display.render(mem);
    {
        const uint32_t got = *pixelAt(display, 100, 11);
        assert(r8(got) > 235 && r8(got) <= 255);
    }

    // ── Mode switch clears persistence ───────────────────────────────────
    display.setHiResMode(Apple2Display::HiResMode::ColorNTSC);
    display.render(mem);
    {
        const uint32_t got = *pixelAt(display, 100, 11);
        assert(got == kBlack);
    }

    // ── 80STORE+PAGE2 must NOT redirect single-HGR to aux (round-5 fix) ──
    //    The //e video scanner only reads aux RAM in DHGR (80COL on); single
    //    hi-res always displays MAIN page 1. A MAIN row of $7F must render
    //    white even when aux page 1 holds $00 and 80STORE+HIRES+PAGE2 are
    //    all set. (The bug redirected the scanner to aux → black garbage.)
    {
        Memory m2;
        m2.setIIEMode(true);
        Apple2Display d2;
        d2.setAuxMemory(m2.auxData());   // bind aux so the (buggy) redirect could fire
        (void)m2.memRead(0xC050);   // graphics (text off)
        (void)m2.memRead(0xC057);   // hi-res on
        (void)m2.memRead(0xC055);   // PAGE2 on
        m2.memWrite(0xC001, 0);     // 80STORE on
        uint8_t* mainRam = const_cast<uint8_t*>(m2.data());
        uint8_t* auxRamP = m2.auxDataMutable();
        for (int col = 0; col < 40; ++col) {
            mainRam[0x2000 + col] = 0x7F;   // MAIN HGR page-1 row 0 = white
            auxRamP[0x2000 + col] = 0x00;   // AUX  HGR page-1 row 0 = black
        }
        d2.render(m2);
        for (int x = 0; x < 280; ++x) assert(*pixelAt(d2, x, 0) == kWhite);
    }

    std::printf("HGR render smoke: OK (sliding window NTSC, mono phosphors, persistence)\n");
    return 0;
}
