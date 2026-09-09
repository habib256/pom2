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

// Double lo-res, at the dot: POM2's DLGR bit stream against MAME's own word
// builder, and the demod phase constant that goes with it.
//
// MAME `apple2video.cpp` `lores_update<Double>` (the non-perfect-block branch,
// the one that feeds render_line):
//
//   words[col+0] = ((NIBBLE(vaux[col+0]) * 0x111)      & 0x007f)
//                + ((NIBBLE(vram[col+0]) * 0x0880)     & 0x3f80);
//   words[col+1] = (((NIBBLE(vaux[col+1]) * 0x111)>>2) & 0x007f)
//                + ((NIBBLE(vram[col+1]) * 0x2220)     & 0x3f80);
//   render_line(&bitmap.pix(row), words, startcol, stopcol, monochrome, Double);
//
// Laid end to end that is, for the absolute 14.318 MHz sample index absX:
//
//   aux  half (dots 0..6  of a cell) : bit = aNib[ absX      & 3]
//   main half (dots 7..13 of a cell) : bit = mNib[(absX + 1) & 3]
//
// and the `is_80_column` term of the artifact decoder is Double = TRUE, i.e.
// the demod's colour reference advances one sample — the same +1 DHGR gets.
//
// POM2 emitted `rotl4(aNib,1)` into the STREAM instead of the raw aux nibble
// and left the phase term at 0. `rotl4` is the COLOUR that stream demodulates
// to (MAME's square filter maps aNib[absX&3] onto rotl4(aNib,1) precisely
// because is_80_column adds that +1), so the rotation was applied twice in the
// signal domain and cancelled against the missing phase term. The visible
// consequences were a one-dot right shift of the whole DLGR line under
// MonoWhite / MonoGreen / MonoAmber, plus ~39 dots per line at the half-cell
// boundaries that no shift accounts for.
//
// Three checks, none of which the old convention can pass:
//   1. the composite signal, dot for dot, against the MAME words;
//   2. the MonoWhite framebuffer, dot for dot, against the same;
//   3. `signalPhaseOffset() == 1` for a DLGR frame (MAME's is_80_column).
// Plus a regression guard: the RGB block colours (renderLoResDouble's colour
// branch, which is NOT touched) stay rotl4(aux,1) / main.

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

constexpr uint16_t CLR_TEXT = 0xC050, CLR_HIRES = 0xC056, SET_PAGE1 = 0xC054;
constexpr uint16_t IIE_80COL_ON = 0xC00D, DHIRES_ON = 0xC05E;

unsigned rotl4(unsigned n, unsigned c)
{ c &= 3u; return ((n << c) | (n >> (4 - c))) & 0x0Fu; }

uint16_t textRowAddr(int row)
{ return static_cast<uint16_t>(0x0400 + ((row & 7) << 7) + ((row & 0x18) * 5)); }

// MAME lores_update<Double>'s 40 words for one block row, laid out as 560 dots.
void mameDlgrStream(const uint8_t* main_, const uint8_t* aux_, uint16_t rowAddr,
                    bool upperHalf, uint8_t out[560])
{
    auto NIB = [&](uint8_t b) -> unsigned {
        return upperHalf ? (b & 0x0Fu) : ((b >> 4) & 0x0Fu);
    };
    uint16_t words[40];
    for (int col = 0; col < 40; col += 2) {
        words[col + 0] = static_cast<uint16_t>(
            ((NIB(aux_ [rowAddr + col + 0]) * 0x111u) & 0x007Fu) +
            ((NIB(main_[rowAddr + col + 0]) * 0x0880u) & 0x3F80u));
        words[col + 1] = static_cast<uint16_t>(
            (((NIB(aux_ [rowAddr + col + 1]) * 0x111u) >> 2) & 0x007Fu) +
            ((NIB(main_[rowAddr + col + 1]) * 0x2220u) & 0x3F80u));
    }
    for (int col = 0; col < 40; ++col)
        for (int b = 0; b < 14; ++b)
            out[col * 14 + b] = static_cast<uint8_t>((words[col] >> b) & 1u);
}

void setupDlgr(Memory& mem)
{
    mem.setIIEMode(true);
    mem.memRead(CLR_TEXT);
    mem.memRead(CLR_HIRES);
    mem.memRead(SET_PAGE1);
    mem.memWrite(IIE_80COL_ON, 0);
    mem.memRead(DHIRES_ON);

    // A deterministic pattern that uses all 16 nibble values in both halves
    // of every byte, in both banks — a uniform fill would hide a phase error
    // for the nibbles that are symmetric under rotation ($0, $5, $A, $F).
    uint8_t* aux = mem.auxDataMutable();
    for (int row = 0; row < 24; ++row) {
        const uint16_t ra = textRowAddr(row);
        for (int col = 0; col < 40; ++col) {
            const unsigned k = static_cast<unsigned>(row * 40 + col);
            const uint8_t aLo = static_cast<uint8_t>( k        & 0x0F);
            const uint8_t aHi = static_cast<uint8_t>((k * 3 + 1) & 0x0F);
            const uint8_t mLo = static_cast<uint8_t>((k * 5 + 3) & 0x0F);
            const uint8_t mHi = static_cast<uint8_t>((k * 7 + 2) & 0x0F);
            aux[ra + col] = static_cast<uint8_t>(aLo | (aHi << 4));
            mem.memWrite(static_cast<uint16_t>(ra + col),
                         static_cast<uint8_t>(mLo | (mHi << 4)));
        }
    }
}

} // namespace

int main()
{
    Memory mem;
    setupDlgr(mem);

    // ── 1. composite signal, dot for dot ─────────────────────────────────
    {
        Apple2Display disp;
        disp.setAuxMemory(mem.auxData());
        disp.setHiResMode(Apple2Display::HiResMode::ColorCompositeOE);
        disp.render(mem);
        assert(disp.signalProduced() && "DLGR must produce a composite signal");
        const uint8_t* sig = disp.signal();
        for (int y = 0; y < 192; ++y) {
            const int blockRow = y / 4;
            uint8_t ref[560];
            mameDlgrStream(mem.data(), mem.auxData(), textRowAddr(blockRow / 2),
                           (blockRow % 2) == 0, ref);
            for (int x = 0; x < 560; ++x) {
                if ((sig[y * 560 + x] != 0) != (ref[x] != 0)) {
                    std::printf("dlgr_mame_phase: signal y=%d x=%d POM2=%d MAME=%d\n",
                                y, x, sig[y * 560 + x] ? 1 : 0, ref[x]);
                    assert(false && "DLGR composite signal must be MAME's "
                                    "lores_update<Double> stream, dot for dot");
                }
            }
        }
    }

    // ── 2. MonoWhite framebuffer, dot for dot ────────────────────────────
    {
        Apple2Display disp;
        disp.setAuxMemory(mem.auxData());
        disp.setHiResMode(Apple2Display::HiResMode::MonoWhite);
        disp.render(mem);
        assert(disp.width() == 560 && "DLGR renders into frame80");
        const uint32_t* fb = disp.pixels();
        for (int y = 0; y < 192; ++y) {
            const int blockRow = y / 4;
            uint8_t ref[560];
            mameDlgrStream(mem.data(), mem.auxData(), textRowAddr(blockRow / 2),
                           (blockRow % 2) == 0, ref);
            for (int x = 0; x < 560; ++x) {
                const bool lit = (fb[y * 560 + x] & 0x00FFFFFFu) != 0;
                if (lit != (ref[x] != 0)) {
                    std::printf("dlgr_mame_phase: mono y=%d x=%d POM2=%d MAME=%d\n",
                                y, x, lit ? 1 : 0, ref[x]);
                    assert(false && "DLGR MonoWhite dots must be MAME's "
                                    "lores_update<Double> stream, dot for dot");
                }
            }
        }
    }

    // ── 3. the is_80_column demod phase term ─────────────────────────────
    {
        Apple2Display disp;
        disp.setAuxMemory(mem.auxData());
        disp.setHiResMode(Apple2Display::HiResMode::ColorCompositeOE);
        disp.render(mem);
        assert(disp.signalPhaseOffset() == 1 &&
               "DLGR is an 80-column mode: MAME render_line(..., Double) "
               "carries is_80_column = 1, same as DHGR");
    }

    // ── 4. the RGB block colours are untouched (regression guard) ────────
    // The 16 palette entries are read back from a 40-column lo-res render
    // (kLoResPalette is private), so this compares two POM2 painters rather
    // than hard-coding RGB.
    uint32_t pal[16];
    for (unsigned n = 0; n < 16; ++n) {
        Memory lg;
        lg.setIIEMode(true);
        lg.memRead(CLR_TEXT); lg.memRead(CLR_HIRES); lg.memRead(SET_PAGE1);
        for (int row = 0; row < 24; ++row) {
            const uint16_t ra = textRowAddr(row);
            for (int col = 0; col < 40; ++col)
                lg.memWrite(static_cast<uint16_t>(ra + col),
                            static_cast<uint8_t>(n | (n << 4)));
        }
        Apple2Display d;
        d.setAuxMemory(lg.auxData());
        d.setHiResMode(Apple2Display::HiResMode::ColorNTSC);
        d.render(lg);
        pal[n] = d.pixels()[0];
    }
    {
        Apple2Display disp;
        disp.setAuxMemory(mem.auxData());
        disp.setHiResMode(Apple2Display::HiResMode::ColorNTSC);
        disp.render(mem);
        const uint32_t* fb = disp.pixels();
        for (int col = 0; col < 40; ++col) {
            const unsigned aNib = mem.auxData()[textRowAddr(0) + col] & 0x0Fu;
            const unsigned mNib = mem.data()   [textRowAddr(0) + col] & 0x0Fu;
            assert(fb[0 * 560 + col * 14 + 3] == pal[rotl4(aNib, 1)] &&
                   "DLGR aux block colour is rotl4(NIBBLE(vaux),1)");
            assert(fb[0 * 560 + col * 14 + 10] == pal[mNib] &&
                   "DLGR main block colour is NIBBLE(vram)");
        }
    }

    std::printf("dlgr_mame_phase OK\n");
    return 0;
}
