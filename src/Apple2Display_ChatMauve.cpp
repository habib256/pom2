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

// Apple2Display_ChatMauve.cpp — the Le Chat Mauve / Video-7 painters, split
// out of Apple2Display.cpp (file-size ratchet): the LCM HGR rule and the
// Eve's SPEC variants, the fg/bg HGR of the Video-7 (F/B) and the Eve
// (CP280), the Eve's COL280A/B, and the TXTGREEN pass. The DHGR paths
// (COL140 / BW560 / mixed / 160) stay in renderDhgr, which owns the 560-dot
// stream. Rules and sources: docs/chatmauve_plan.md; the routing that
// selects these is renderInternalBandImpl in Apple2Display.cpp.

#include "Apple2Display.h"
#include "Apple2Display_Internal.h"
#include "LeChatMauveCard.h"
#include "Memory.h"

#include <array>
#include <cstdint>

using namespace pom2::a2disp;

namespace {

// Le Chat Mauve / Video-7 AppleColor RGB — 6-color HGR palette, applied
// per-pixel-pair with the byte's MSB selecting the bank. ABGR-in-uint32
// (R lowest byte) to match `kLoResPalette`.
//
// On STANDARD HGR (no DHGR), real Chat Mauve / Video-7 hardware sniffs
// the digital pre-modulation video signal at the slot connector and
// decodes it directly into RGB — bypassing the NTSC modulator entirely.
// What this means concretely (cf. AppleWin RGBMonitor.cpp PR #837 and
// *Le Chat Mauve* manual):
//
//   - The MSB ("high bit") of each byte is a **palette bank flag**, NOT
//     a half-dot delay. Real Chat Mauve does NOT shift pixels by ½ dot;
//     that shift is purely an NTSC artefact mechanism Wozniak co-opted
//     to access the orange/blue half of the wheel via composite phase.
//   - A clean RGB decoder doesn't double bits to 14 sub-pixels per byte
//     either — it processes the raw 7-bit-per-byte stream directly.
//   - Color comes from **pairs of consecutive bits**. With the byte's
//     MSB selecting the bank:
//          MSB=0:  00→black  01→VIOLET  10→GREEN   11→white
//          MSB=1:  00→black  01→BLUE    10→ORANGE  11→white
//     6 distinct colours total — same as NTSC HGR on a colour TV, but
//     emitted cleanly with no inter-byte fringing and crisp edges
//     because the MSB transition is instantaneous, not phase-encoded.
//
// The 16-color palette with two distinct grays (the famous Chat Mauve /
// French Touch trademark) ONLY applies in DHGR mode (4-bit windows over
// the aux+main interleaved stream) — that path IS modelled, in renderDhgr()
// (the rmode 0/1/2/3 Video-7 decode against kChatMauveLoResPalette); this
// kChatMauveHGR table is the standard-HGR (non-DHGR) 6-colour decode only.
// On standard HGR the $5 / $A bit patterns that NTSC reads as "gray"
// actually decode to VIOLET / GREEN (or BLUE / ORANGE with MSB=1) under
// Chat Mauve too — they're never grays on plain HGR.
//
// Indexing convention: kChatMauveHGR[msb][bit_pair]. Colour values from
// AppleWin `RGBMonitor.cpp::PaletteRGB_Feline` (same empirical capture
// of a real Le Chat Mauve "Feline" board used by kChatMauveLoResPalette
// — kept in sync so HGR and lo-res share the same visual identity).
constexpr std::array<std::array<uint32_t, 4>, 2> kChatMauveHGR = {{
    // MSB = 0 → "violet bank"
    { 0xFF000000,   //  00  black
      0xFFD11AAA,   //  01  magenta  rgb(0xaa, 0x1a, 0xd1) (Feline MAGENTA)
      0xFF2CE66F,   //  10  green    rgb(0x6f, 0xe6, 0x2c) (Feline GREEN)
      0xFFFFFFFF }, //  11  white
    // MSB = 1 → "blue bank"
    { 0xFF000000,   //  00  black
      0xFFB58A00,   //  01  blue     rgb(0x00, 0x8a, 0xb5) (Feline BLUE)
      0xFF4772FF,   //  10  orange   rgb(0xff, 0x72, 0x47) (Feline ORANGE)
      0xFFFFFFFF }, //  11  white
}};

} // namespace

// Single HGR under a Le Chat Mauve / Video-7 card — docs/chatmauve_plan.md
// § 3.2, verified by fenarinarsa against every bit combination on the //c
// adapter; pixel for pixel AppleWin `RGBMonitor.cpp` `UpdateHiResRGBCell`
// (pinned by chatmauve_dot_rules against a port of it):
//
//   1. The 280 dots of a line are 140 CELLS of 2 bits, aligned to the line
//      (a cell straddles every other byte boundary). `01` → colour 1, `10`
//      → colour 2; the bank (violet/green vs blue/orange) is bit 7 of the
//      byte the DOT lives in — not the byte the cell started in.
//   2. Each dot is judged with its two neighbours (the dot before the first
//      and after the last are 0): `010` or `101` → the dot takes its cell's
//      colour; anything else → white if 1, black if 0.
//
// No half-dot shift, no fringing, no NTSC: runs of 1s are white at the full
// 280-dot sharpness. Each HGR dot becomes 2 dots of `frame80`.
//
// The Eve's decoder variants (table IX-1, manual IV-3) sit on the same rule:
//   HRSPEC1  an isolated colour dot on WHITE (`11011`) → black;
//   HRSPEC2  SPEC1, plus an isolated colour dot on BLACK (`00100`) → white;
//   HRBW     everything → its own bit (the `Mono` path, shared with the
//            Féline's AN3-off state).
// "Isolated" needs the 5-dot window: inside an alternating run (`01010`)
// the same 3-dot patterns occur and the run stays coloured — that reading
// is what leaves SPEC2 distinct from HRBW. HRDASH ("coloured horizontal
// lines drawn dotted") is P3 and renders as LcmColor for now.
void Apple2Display::renderHiResChatMauve80(Memory& mem,
                                           const Memory::DisplayState& state,
                                           int firstScanline,
                                           int lastScanline,
                                           LeChatMauveCard::HgrMode hm)
{
    using HgrMode = LeChatMauveCard::HgrMode;
    // Single hi-res displays MAIN page 1/2; aux HGR is only shown via DHGR.
    const uint8_t* ram = mem.data();

    const bool monochrome = (hm == HgrMode::Mono);
    const bool spec1 = (hm == HgrMode::Spec1 || hm == HgrMode::Spec2);
    const bool spec2 = (hm == HgrMode::Spec2);

    uint8_t  pixels[kWidth + 4];   // 280 raw HGR bits, 2 zero guards each side
    uint8_t  msbHigh[40];          // per-byte palette-bank flag
    pixels[0] = pixels[1] = pixels[kWidth + 2] = pixels[kWidth + 3] = 0;
    uint8_t* px = pixels + 2;

    for (int y = firstScanline; y < lastScanline; ++y) {
        const uint16_t rowAddr = hgrRowAddress(y, videoHgrPage2(state));

        for (int col = 0; col < 40; ++col) {
            const uint8_t b = ram[rowAddr + col];
            msbHigh[col] = static_cast<uint8_t>((b >> 7) & 1u);
            for (int k = 0; k < 7; ++k)
                px[col * 7 + k] = static_cast<uint8_t>((b >> k) & 1u);
        }

        uint32_t* outRow = frame80.data() + static_cast<size_t>(y) * kWidth80;
        if (monochrome) {
            for (int x = 0; x < kWidth; ++x) {
                const uint32_t c = px[x] ? 0xFFFFFFFFu : 0xFF000000u;
                outRow[x * 2 + 0] = c;
                outRow[x * 2 + 1] = c;
            }
            continue;
        }
        for (int i = 0; i < kWidth; ++i) {
            const int l = px[i - 1], ctr = px[i], r = px[i + 1];
            const bool lone1 = ( ctr && !l && !r);      // 010
            const bool lone0 = (!ctr &&  l &&  r);      // 101
            bool colour = lone1 || lone0;
            if (colour && spec1 && lone0 && px[i - 2] && px[i + 2]) colour = false;  // 11011
            if (colour && spec2 && lone1 && !px[i - 2] && !px[i + 2]) colour = false; // 00100
            uint32_t rgb;
            if (colour) {
                const int      pair = i & ~1;
                const unsigned code = px[pair] | (px[pair + 1] << 1);
                rgb = kChatMauveHGR[msbHigh[i / 7]][code];
            } else {
                rgb = ctr ? 0xFFFFFFFFu : 0xFF000000u;
            }
            outRow[i * 2 + 0] = rgb;
            outRow[i * 2 + 1] = rgb;
        }
    }
}

// Foreground/background HGR. Bitmap from MAIN $2000-$3FFF, one byte of
// colours per 7-dot block from AUX at the same address. Two cards, two
// nibble orders (see the header): Video-7 F/B hi = fg; Eve CP280 hi = bg.
// AppleWin `UpdateHiResDuochromeCell` is the Video-7 order. Each HGR dot
// becomes 2 dots of `frame80`.
void Apple2Display::renderHgrDuochrome(Memory& mem,
                                       const Memory::DisplayState& state,
                                       int firstScanline, int lastScanline,
                                       bool auxHiIsForeground)
{
    const uint8_t* main_ = mem.data();
    const uint8_t* aux_  = auxRam ? auxRam : main_;

    for (int y = firstScanline; y < lastScanline; ++y) {
        const uint16_t rowAddr = hgrRowAddress(y, videoHgrPage2(state));
        uint32_t* outRow = frame80.data() + static_cast<size_t>(y) * kWidth80;

        for (int col = 0; col < 40; ++col) {
            const uint8_t  pix  = main_[rowAddr + col];
            const uint8_t  attr = aux_ [rowAddr + col];
            const uint32_t hi = kChatMauveLoResPalette[(attr >> 4) & 0x0Fu];
            const uint32_t lo = kChatMauveLoResPalette[ attr       & 0x0Fu];
            const uint32_t fg = auxHiIsForeground ? hi : lo;
            const uint32_t bg = auxHiIsForeground ? lo : hi;
            for (int b = 0; b < 7; ++b) {
                const uint32_t c = ((pix >> b) & 1u) ? fg : bg;
                outRow[col * 14 + 2 * b + 0] = c;
                outRow[col * 14 + 2 * b + 1] = c;
            }
        }
    }
}

// Eve COL280A / COL280B (table IX-1, manual VI-3 "point par point"): the
// same 560-dot stream as COL140 (aux byte then main byte, bit 0 first) cut
// into 2-DOT cells instead of 4 — code = first dot + 2 × second dot — through
// a fixed palette of four: A = black, orange, green, white; B = black, light
// blue, pink, yellow (the manual's colour lists, in code order). Read off
// Purplesoft's `& PLOT` (rev B): `& COLOR= 9` (orange) writes (main $2A,
// aux $55) — the stream 1,0,1,0… = code 1 in every cell; `12` (green) writes
// ($55, $2A) = code 2; `15` writes ($7F, $7F) = code 3; in COL280B the same
// three codes come out of `& COLOR= 7`, `11`, `13`. Not "one bit from each
// bank per dot", which was the plan's assumption. Each cell = 2 frame80 dots.
void Apple2Display::renderDhgrCol280(Memory& mem, const Memory::DisplayState& state,
                                     int firstScanline, int lastScanline, bool paletteB)
{
    const uint8_t* main_ = mem.data();
    const uint8_t* aux_  = auxRam ? auxRam : main_;
    const uint32_t palA[4] = { kChatMauveLoResPalette[0], kChatMauveLoResPalette[9],
                               kChatMauveLoResPalette[12], kChatMauveLoResPalette[15] };
    const uint32_t palB[4] = { kChatMauveLoResPalette[0], kChatMauveLoResPalette[7],
                               kChatMauveLoResPalette[11], kChatMauveLoResPalette[13] };
    const uint32_t* pal = paletteB ? palB : palA;

    uint8_t dots[kWidth80];
    for (int y = firstScanline; y < lastScanline; ++y) {
        const uint16_t rowAddr = hgrRowAddress(y, videoHgrPage2(state));
        uint32_t* outRow = frame80.data() + static_cast<size_t>(y) * kWidth80;
        for (int c = 0; c < 40; ++c) {
            const uint8_t auxB  = aux_ [rowAddr + c];
            const uint8_t mainB = main_[rowAddr + c];
            for (int i = 0; i < 7; ++i) {
                dots[c * 14 + i]     = static_cast<uint8_t>((auxB  >> i) & 1u);
                dots[c * 14 + 7 + i] = static_cast<uint8_t>((mainB >> i) & 1u);
            }
        }
        for (int d = 0; d < kWidth80; d += 2) {
            const unsigned code = dots[d] | (dots[d + 1] << 1);
            outRow[d] = outRow[d + 1] = pal[code];
        }
    }
}

// Eve TXTGREEN: "blanc → vert", 40 and 80 columns. The text painters put
// pure white on pure black, so remapping white in the text rows of the band
// is the whole effect; the green is the P31 tint the MonoGreen pipeline uses.
void Apple2Display::tintTextGreen(const Memory::DisplayState& state, int scanY0, int scanY1)
{
    int lo = 0, hi = 0;
    // The RVB Graph's $C0F3 is whole-screen "monochrome green", not green
    // text (plan § 3.6: -16143 = colours + green text, -16141 = monochrome
    // green). hgrMode()/dhgrModeFor() already answer Mono there, so the
    // graphics rows are white dots on black and the same white→green remap
    // finishes the job; without this the mode came out green text over a
    // WHITE picture.
    if (chatMauve &&
        chatMauve->variant() == LeChatMauveCard::Variant::RvbGraph &&
        chatMauve->rvbMode() == 3) {
        if (!bandScanlines(scanY0, scanY1, 0, kHeight, &lo, &hi)) return;
    } else if (state.textMode) {
        if (!bandScanlines(scanY0, scanY1, 0, kHeight, &lo, &hi)) return;
    } else if (state.mixedMode) {
        if (!bandScanlines(scanY0, scanY1, 160, kHeight, &lo, &hi)) return;
    } else {
        return;
    }
    constexpr uint32_t kGreen = 0xFF000000u | (uint32_t{0x33} << 16) | (uint32_t{0xFF} << 8) | 0x33u;
    const int w = useFrame80_ ? kWidth80 : kWidth;
    uint32_t* base = useFrame80_ ? frame80.data() : frame.data();
    for (int y = lo; y < hi; ++y) {
        uint32_t* row = base + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x)
            if (row[x] == 0xFFFFFFFFu) row[x] = kGreen;
    }
}
