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

// Smoke test for Double Lo-Res (DLGR) — the 80-column lo-res mode where aux
// RAM supplies the even 7-dot half of each column (nibble rotated left 1) and
// main RAM the odd half (MAME apple2video.cpp lores_update<Double>).
//
// We don't hardcode palette RGB values; instead we pin the structural
// invariants that prove the path is wired correctly:
//   - DLGR routes to the 560-wide frame80 (width() == 560).
//   - Each 14-dot column splits into a uniform aux cell (x..x+6) and a uniform
//     main cell (x+7..x+13), constant over the 4 scanlines of a block row.
//   - With main nibble == aux nibble, the aux cell still differs from the main
//     cell — proving the rotl4(aux,1) rotation is applied (without it they'd
//     be identical).

#include "Apple2Display.h"
#include "Apple2VideoDecode.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
constexpr uint16_t CLR_TEXT  = 0xC050;
constexpr uint16_t CLR_HIRES = 0xC056;
constexpr uint16_t SET_PAGE1 = 0xC054;
constexpr uint16_t IIE_80COL_ON = 0xC00D;
constexpr uint16_t DHIRES_ON = 0xC05E;
}

int main()
{
    Memory mem;
    mem.setIIEMode(true);

    // DLGR soft-switch state: graphics + lo-res + 80COL + AN3(DHIRES).
    mem.memRead(CLR_TEXT);
    mem.memRead(CLR_HIRES);
    mem.memRead(SET_PAGE1);
    mem.memWrite(IIE_80COL_ON, 0);
    mem.memRead(DHIRES_ON);

    // Block row 0 = text row 0 (addr $0400), upper half → low nibble.
    // main nibble == aux nibble == 1: the rotl4(aux,1) → 2, so aux ≠ main.
    uint8_t* aux = mem.auxDataMutable();
    mem.memWrite(0x0400, 0x01);   // main col 0 low nibble = 1
    aux[0x0400] = 0x01;           // aux  col 0 low nibble = 1
    mem.memWrite(0x0401, 0x03);   // col 1: distinct nibbles to exercise both
    aux[0x0401] = 0x05;

    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());
    disp.setHiResMode(Apple2Display::HiResMode::ColorNTSC);
    disp.render(mem);

    assert(disp.width() == 560 && "DLGR must render into the 560-wide frame80");
    const uint32_t* fb = disp.pixels();
    const int W = 560;

    auto px = [&](int x, int y) { return fb[y * W + x] & 0x00FFFFFFu; };

    // Column 0: aux cell = x[0..6], main cell = x[7..13].
    const uint32_t aux0  = px(3, 0);
    const uint32_t main0 = px(10, 0);
    // Aux cell uniform across its 7 dots and 4 scanlines.
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 7; ++x)
            assert(px(x, y) == aux0 && "aux cell must be uniform");
    // Main cell uniform.
    for (int y = 0; y < 4; ++y)
        for (int x = 7; x < 14; ++x)
            assert(px(x, y) == main0 && "main cell must be uniform");
    // rotl4(aux,1) applied → with equal nibbles the cells still differ.
    assert(aux0 != main0 && "DLGR must rotate the aux nibble (rotl4 ,1)");

    // Column 1 (x 14..27) decodes too and differs from column 0's cells.
    const uint32_t aux1  = px(14 + 3, 0);
    const uint32_t main1 = px(14 + 10, 0);
    assert(aux1 != main1 && "col1 aux/main differ");

    // Composite signal path must interleave aux+main like the framebuffer
    // (fillCompositeSignal::paintLoResDouble), not fall back to main-only GR.
    disp.setHiResMode(Apple2Display::HiResMode::ColorCompositeOE);
    disp.render(mem);
    assert(disp.signalProduced());
    const uint8_t* sig = disp.signal();
    // Scanline 0, column 0: samples 0..6 = aux half, 7..13 = main half.
    //
    // Exact-sample pin (rotl4 + absolute phase together). Aux nibble 1 is
    // emitted RAW (MAME lores_update<Double>: aux = (nib*0x111)&0x7f, so
    // dot absX = aNib[absX & 3]; main = (nib*0x0880)&0x3f80, so dot absX =
    // mNib[(absX+1) & 3]). The rotl4 that the COLOUR path applies to the aux
    // nibble is the hue that raw stream demodulates to — pre-rotating the bit
    // stream applied it twice (bug hunt #15, `dlgr_mame_phase`).
    // NOTE: a naive sig[i] != sig[7+i] comparison is NOT a valid pin; only
    // the absolute-phase sample identities below are.
    for (int x = 0; x < 7; ++x) {
        const bool want = ((0x1u >> (x & 3)) & 1) != 0;   // aux: raw nibble 1
        assert((sig[x] != 0) == want &&
               "DLGR aux half: raw nibble at absolute phase");
    }
    for (int x = 7; x < 14; ++x) {
        const bool want = ((0x1u >> ((x + 1) & 3)) & 1) != 0;   // main: nibble 1, one sample later
        assert((sig[x] != 0) == want &&
               "DLGR main half: nibble at absolute phase + 1");
    }

    // ── Phase pin: the nibble pattern is locked to the ABSOLUTE 14.318 MHz
    // sample counter, like paintLoRes40's `(nibble >> (absX & 3))` — NOT
    // restarted per 7-dot half-cell. With a uniform fill (main = aux =
    // nibble 1 everywhere) every sample must equal the absolute-phase
    // reference; the old per-half-cell `dx & 3` indexing rotated the hue by
    // (2·col) mod 4 per column (14 ≡ 2 mod 4), i.e. the pattern phase
    // alternated column to column.
    {
        Memory uni;
        uni.setIIEMode(true);
        uni.memRead(CLR_TEXT);
        uni.memRead(CLR_HIRES);
        uni.memRead(SET_PAGE1);
        uni.memWrite(IIE_80COL_ON, 0);
        uni.memRead(DHIRES_ON);
        uint8_t* uaux = uni.auxDataMutable();
        for (uint32_t a = 0x0400; a < 0x0800; ++a) {
            uni.memWrite(static_cast<uint16_t>(a), 0x11);   // both nibbles = 1
            uaux[a] = 0x11;
        }

        Apple2Display d;
        d.setAuxMemory(uni.auxData());
        d.setHiResMode(Apple2Display::HiResMode::ColorCompositeOE);
        d.render(uni);
        assert(d.signalProduced());
        const uint8_t* s = d.signal();
        const uint8_t auxPat = 0x01;   // raw aux nibble (MAME word builder)
        const uint8_t mainPat = 0x01;
        for (int y = 0; y < 192; y += 37) {            // sample a few lines
            for (int x = 0; x < 560; ++x) {
                const bool auxHalf = (x % 14) < 7;
                const uint8_t pat = auxHalf ? auxPat : mainPat;
                const int phase = auxHalf ? (x & 3) : ((x + 1) & 3);   // main: one sample later
                const uint8_t want = ((pat >> phase) & 1u) ? 0xFFu : 0x00u;
                assert(s[y * 560 + x] == want
                       && "DLGR signal must emit the nibble at absolute beam phase");
            }
        }

        // And the OE CPU demod of that signal must be column-uniform: same
        // RGBA at x and x+28 (the signal's exact period, lcm(14,4)) across
        // the interior. The phase bug made the colour alternate per column.
        Apple2Display oe;
        oe.setAuxMemory(uni.auxData());
        oe.setHiResMode(Apple2Display::HiResMode::ColorCompositeOECpu);
        oe.render(uni);
        const uint32_t* ofb = oe.pixels();
        assert(oe.width() == 560);
        for (int x = 28; x < 560 - 36; ++x) {          // clear of FIR edge taps
            assert(ofb[96 * 560 + x] == ofb[96 * 560 + x + 28]
                   && "uniform DLGR fill must demodulate column-uniform "
                      "(period-28 exact)");
        }
    }

    // ── 7M LORES: the same switches with 80COL OFF ───────────────────────
    // [B] Sather, Understanding the Apple IIe, p. 8-22: LORES40 GRAPHICS with
    // 80COL reset and AN3 low shifts the lo-res pattern at HIRES40 speed,
    // undelayed. Sather's own checks first: 0000 black, 1111 white, 0101
    // violet in even columns and green in odd ones (HGR $55 at each), 1010
    // the other way ($2A at each).
    {
        using pom2::a2v::sevenMLoresByte;
        assert(sevenMLoresByte(0x0, 0) == 0x00 && sevenMLoresByte(0x0, 1) == 0x00);
        assert(sevenMLoresByte(0xF, 0) == 0x7F && sevenMLoresByte(0xF, 1) == 0x7F);
        assert(sevenMLoresByte(0x5, 0) == 0x55 && sevenMLoresByte(0x5, 1) == 0x55);
        assert(sevenMLoresByte(0xA, 0) == 0x2A && sevenMLoresByte(0xA, 1) == 0x2A);
        // Any other pattern fragments: 0001 lights one dot pair of four.
        assert(sevenMLoresByte(0x1, 0) == 0x11 && sevenMLoresByte(0x1, 1) == 0x44);

        // A screen of mixed nibbles in 7M LORES draws exactly what an
        // undelayed HGR page holding those bytes draws, in every pipeline.
        auto setup = [](Memory& m, bool hires, bool an3Low) {
            m.setIIEMode(true);
            m.memRead(CLR_TEXT);
            m.memRead(hires ? 0xC057 : CLR_HIRES);
            m.memRead(SET_PAGE1);
            m.memWrite(0xC00C, 0);                       // 80COL OFF
            m.memRead(an3Low ? DHIRES_ON : 0xC05F);
        };
        Memory lo, hgr, plain;
        setup(lo, false, true);
        setup(hgr, true, true);                          // undelayed HGR40
        setup(plain, false, false);                      // ordinary lo-res
        for (uint16_t a = 0x0400; a < 0x0800; ++a) {
            const uint8_t v = static_cast<uint8_t>((a * 37u + 0x5A) & 0xFF);
            lo.memWrite(a, v);
            plain.memWrite(a, v);
        }
        for (int y = 0; y < 192; ++y) {
            uint8_t row[40];
            pom2::a2v::buildSevenMLoresRow(lo.data(),
                Apple2Display::textRowAddress(y / 8, false), y, row);
            for (int c = 0; c < 40; ++c)
                hgr.memWrite(static_cast<uint16_t>(0x2000 + (y & 7) * 0x400 + ((y >> 3) & 7) * 0x80
                                                   + (y >> 6) * 0x28 + c),
                             row[c]);
        }
        using HM = Apple2Display::HiResMode;
        for (HM mode : { HM::ColorNTSC, HM::ColorComp4Bit, HM::MonoWhite,
                         HM::ColorCompositeOE }) {
            Apple2Display a, b, c;
            a.setHiResMode(mode); b.setHiResMode(mode); c.setHiResMode(mode);
            a.render(lo); b.render(hgr); c.render(plain);
            assert(a.width() == b.width() && a.height() == b.height());
            const size_t n = static_cast<size_t>(a.width()) * a.height();
            assert(std::memcmp(a.pixels(), b.pixels(), n * 4) == 0 &&
                   "7M LORES must draw the shift register's undelayed HGR bytes");
            if (a.width() == c.width())
                assert(std::memcmp(a.pixels(), c.pixels(), n * 4) != 0 &&
                       "and differ from the ordinary 14 MHz lo-res");
            if (mode == HM::ColorCompositeOE) {
                assert(a.signalProduced() && b.signalProduced());
                assert(std::memcmp(a.signal(), b.signal(), 560u * 192u) == 0 &&
                       "the composite signal too");
            }
        }
        // 80COL ON with AN3 low stays double lo-res (the block above).
        std::printf("  7M LORES (Sather 8-22) OK\n");
    }

    std::printf("dlgr_render_smoke OK\n");
    return 0;
}
