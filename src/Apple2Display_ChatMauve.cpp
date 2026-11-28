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

#include <algorithm>
#include <array>
#include <cstdint>

using namespace pom2::a2disp;

namespace {

// Le Chat Mauve / Video-7 AppleColor RGB — 6-color HGR palette, applied
// per-pixel-pair with the byte's MSB selecting the bank. ABGR-in-uint32
// (R lowest byte) to match `kLoResPalette`.
//
// On STANDARD HGR (no DHGR), real Chat Mauve / Video-7 hardware reads the
// motherboard's digital pre-modulation video signals and decodes them
// directly into RGB — bypassing the NTSC modulator entirely. [B] The
// Féline and the Eve take them from the aux slot (they plug into slot 3
// and the aux slot at once: Féline manual p. 8; Apple IIe TRM Table 7-21),
// the //c adapter from the DB-15 (Apple IIc TRM Table 11-17); only the RVB
// Graph is a plain slot card. What this means concretely ([A] fenarinarsa,
// //c adapter and a Féline, AppleWin #764 and PR #837; [D] AppleWin
// RGBMonitor.cpp @ 7fd7fd7c):
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
// [D] AppleWin `RGBMonitor.cpp::PaletteRGB_Feline` @ 3e8054b462 (same capture
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
// § 3.2. [A] fenarinarsa, every bit combination on the //c adapter
// (AppleWin #764, 2020-02-24) and a Féline (PR #837); [D] pixel for pixel
// AppleWin `RGBMonitor.cpp` `UpdateHiResRGBCell` @ 7fd7fd7c (pinned by
// chatmauve_dot_rules against a port of it). [F] The Eve's HRAPPLE is
// presumed the same (fenarinarsa: «I can only guess it's the same for EVE»).
//
//   1. The 280 dots of a line are 140 CELLS of 2 bits, aligned to the line
//      (a cell straddles every other byte boundary). `01` → colour 1, `10`
//      → colour 2; the bank (violet/green vs blue/orange) is bit 7 of the
//      byte the DOT lives in — not the byte the cell started in ([D]
//      AppleWin's choice — an implementation detail, not a measurement).
//   2. Each dot is judged with its two neighbours (the dot before the first
//      and after the last are 0): `010` or `101` → the dot takes its cell's
//      colour; anything else → white if 1, black if 0.
//
// No half-dot shift, no fringing, no NTSC: runs of 1s are white at the full
// 280-dot sharpness. Each HGR dot becomes 2 dots of `frame80`.
//
// The Eve's decoder variants ([B] table IX-1 p. 118, manual IV-3.1: SPEC1
// «HR std + pts noirs/fond blanc», SPEC2 «… pts blancs/fond noir») sit on
// the same rule:
//   HRSPEC1  an isolated colour dot on WHITE (`11011`) → black;
//   HRSPEC2  SPEC1, plus an isolated colour dot on BLACK (`00100`) → white;
//   HRBW     everything → its own bit ([B] IV-3.1; the `Mono` path, shared
//            with the Féline's AN3-off state).
// [F] "Isolated" needs the 5-dot window — POM2's reading of the manual's
// prose, which the PLA cannot settle (P3 closed): inside an alternating run
// (`01010`) the same 3-dot patterns occur and the run stays coloured — that
// reading is what leaves SPEC2 distinct from HRBW. Settled by: a
// schematic, a trace of the public board photos, or an owner's capture.
// HRDASH ([B] IV-3.1, "coloured horizontal lines drawn dotted") is P3 and
// renders as LcmColor for now.
void Apple2Display::renderHiResChatMauve80(Memory& mem,
                                           const Memory::DisplayState& state,
                                           int firstScanline,
                                           int lastScanline,
                                           LeChatMauveCard::HgrMode hm)
{
    using HgrMode = LeChatMauveCard::HgrMode;
    if (hm == HgrMode::RvbGraph) {
        renderHiResRvbGraph(mem, state, firstScanline, lastScanline);
        return;
    }
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

// Single HGR under the RVB Graph — its Manuel de Référence, chapters III-IV
// (pp. 9-15), against the scan. A dot's POSITION is n = (column & 1) +
// 2 × bit 7 of its byte: 0 "violet" (even, bit 7 = 0), 1 "vert" (odd, 0),
// 2 "bleu" (even, 1), 3 "orange" (odd, 1).
//
//   - a lit dot with no lit neighbour is "isolé" → Cn of its own position;
//   - 2+ consecutive lit dots → Bn of the run's LEFTMOST dot ("si le premier
//     point à gauche est en colonne paire…", p. 15) — or, with S bit 1 set
//     (S = 2/3), each dot's own Cn: "la couleur B n'apparaît plus" (p. 13);
//   - an unlit dot is black with S bit 0 set (S = 1/3, "tout point
//     représenté en mémoire par un 0 apparaît noir"). With it clear, the
//     "traits horizontaux des couleurs C0, C1, C2, C3 sont continus" (p. 13):
//     a horizontal line in Cn is alternate 1s and 0s (p. 12), so a 0 between
//     two lit dots takes the colour of an ISOLATED neighbour, the left one
//     first. The manual only describes such lines; a 0 between two runs
//     (11011) has no Cn to take and stays black — POM2's reading.
//   - U = 0 (registers ignored): the usual colours C = 3, 12, 6, 9 (the
//     values p. 10 calls "la façon habituelle") and every B = CT — "la
//     couleur des points usuellement blanc … est liée à la couleur
//     attribuée aux lignes de texte" (p. 13).
//
// Colours index the lo-res table, which is the manual's own (chapter II).
// No half-dot shift. Each HGR dot becomes 2 dots of `frame80`.
// [B] FR 2 509 560 Fig. 5 — the RVB Graph's patent (two independent
// readings, 2026-11-28): the 4-bit colour code M1-M4 through a 74LS244 into
// a resistor network per channel — M1: R 470 Ω, B 1 kΩ; M2: B 620 Ω; M3:
// G 560 Ω, B 1k5; M4: R 620 Ω, G 820 Ω — into the Péritel's 75 Ω. These are
// the relative levels (white = 1 per channel), with [F] assumptions: TTL
// high ≈ 3.4 V (the ratios do not depend on it), the "620" read as 620 not
// 680, and the CRT's own gamma left to the CRT stage. The production board
// adds three 1 kΩ trim pots (manual p. 7: "jusqu'à ce que le texte
// apparaisse blanc") — the per-channel gain is the user's. Settled by: a
// capture of a real card's output. ABGR, like kLoResPalette.
const uint32_t Apple2Display::kRvbGraphPalette[16] = {
    0xFF000000, //  0 Black       rgb(  0,   0,   0)
    0xFF4C0091, //  1 Magenta     rgb(145,   0,  76)
    0xFF7D0000, //  2 Dark blue   rgb(  0,   0, 125)
    0xFFCC0091, //  3 Violet      rgb(145,   0, 204)
    0xFF339600, //  4 Dark green  rgb(  0, 150,  51)
    0xFF829691, //  5 Grey 1      rgb(145, 150, 130)
    0xFFB29600, //  6 Blue        rgb(  0, 150, 178)
    0xFFFF9691, //  7 Light blue  rgb(145, 150, 255)
    0xFF00696E, //  8 Brown       rgb(110, 105,   0)
    0xFF4C69FF, //  9 Orange      rgb(255, 105,  76)
    0xFF7D696E, // 10 Grey 2      rgb(110, 105, 125)
    0xFFCC69FF, // 11 Pink        rgb(255, 105, 204)
    0xFF33FF6E, // 12 Light green rgb(110, 255,  51)
    0xFF82FFFF, // 13 Yellow      rgb(255, 255, 130)
    0xFFB2FF6E, // 14 Aqua        rgb(110, 255, 178)
    0xFFFFFFFF, // 15 White       rgb(255, 255, 255)
};

const uint32_t* Apple2Display::chatMauvePalette() const
{
    return (chatMauve && chatMauve->variant() == LeChatMauveCard::Variant::RvbGraph)
        ? kRvbGraphPalette : kChatMauveLoResPalette;
}

void Apple2Display::renderHiResRvbGraph(Memory& mem, const Memory::DisplayState& state,
                                        int firstScanline, int lastScanline)
{
    const LeChatMauveCard::RvbGraphState& g = chatMauve->rvbGraph();
    uint32_t cRgb[4], bRgb[4];
    static constexpr uint8_t kUsual[4] = { 3, 12, 6, 9 };
    for (int n = 0; n < 4; ++n) {
        cRgb[n] = kRvbGraphPalette[g.useRegs ? g.c[n] : kUsual[n]];
        bRgb[n] = kRvbGraphPalette[g.useRegs ? g.b[n] : g.textColour];
    }
    const bool dotted   = (g.s & 1) != 0;
    const bool fourOnly = (g.s & 2) != 0;
    const uint32_t black = kRvbGraphPalette[0];

    const uint8_t* ram = mem.data();
    uint8_t  pixels[kWidth + 2];   // 280 dots, one zero guard each side
    uint8_t  pos[kWidth + 2];      // n of each dot
    pixels[0] = pixels[kWidth + 1] = 0;
    pos[0] = pos[kWidth + 1] = 0;
    uint8_t* px = pixels + 1;
    uint8_t* np = pos + 1;

    for (int y = firstScanline; y < lastScanline; ++y) {
        const uint16_t rowAddr = hgrRowAddress(y, videoHgrPage2(state));
        for (int col = 0; col < 40; ++col) {
            const uint8_t b = ram[rowAddr + col];
            const uint8_t bank = static_cast<uint8_t>((b >> 6) & 2u);
            for (int k = 0; k < 7; ++k) {
                const int x = col * 7 + k;
                px[x] = static_cast<uint8_t>((b >> k) & 1u);
                np[x] = static_cast<uint8_t>(bank | (x & 1));
            }
        }

        uint32_t* outRow = frame80.data() + static_cast<size_t>(y) * kWidth80;
        int runStart = 0;
        for (int i = 0; i < kWidth; ++i) {
            uint32_t rgb = black;
            if (px[i]) {
                if (!px[i - 1]) runStart = i;
                if (!px[i - 1] && !px[i + 1]) rgb = cRgb[np[i]];
                else if (fourOnly)            rgb = cRgb[np[i]];
                else                          rgb = bRgb[np[runStart]];
            } else if (!dotted && px[i - 1] && px[i + 1]) {
                // [B] FR 2 509 560 Fig. 2 (gate N2, chip 5's skipped load):
                // a single 0 before a lit dot repeats the LEFT neighbour's
                // sample, phase included — the C register at its position,
                // even when it closes a run (11011 → B B C B B). Two 0s or
                // more stay black; S1 (dotted) turns the fill off.
                rgb = cRgb[np[i - 1]];
            }
            outRow[i * 2 + 0] = rgb;
            outRow[i * 2 + 1] = rgb;
        }
    }
}

// Foreground/background HGR. Bitmap from MAIN $2000-$3FFF, one byte of
// colours per 7-dot block from AUX at the same address. Two cards, two
// nibble orders (see the header): Video-7 F/B hi = fg ([D] MAME
// `render_line_color_array` @ e1479b7a); Eve CP280 hi = bg ([B] Eve manual
// VI-3). [D] AppleWin `UpdateHiResDuochromeCell` @ 7fd7fd7c is the Video-7
// order. Each HGR dot becomes 2 dots of `frame80`.
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

// Eve COL280A / COL280B ([B] table IX-1 p. 118, manual VI-3 "point par
// point"): the same 560-dot stream as COL140 (aux byte then main byte, bit 0
// first) cut into 2-DOT cells instead of 4 — code = first dot + 2 × second
// dot — through a fixed palette of four: A = black, orange, green, white; B
// = black, light blue, pink, yellow ([B] the manual's colour lists, in code
// order). [C] Read off Purplesoft's `& PLOT` (rev B): `& COLOR= 9` (orange)
// writes (main $2A, aux $55) — the stream 1,0,1,0… = code 1 in every cell;
// `12` (green) writes ($55, $2A) = code 2; `15` writes ($7F, $7F) = code 3;
// in COL280B the same three codes come out of `& COLOR= 7`, `11`, `13`. Not
// "one bit from each bank per dot", which was the plan's assumption. Each
// cell = 2 frame80 dots.
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

// Eve TXTGREEN and the RVB Graph's text colour are a post-pass over
// whatever painted the text rows: the decision tree has a dozen exits and
// the text painters are shared with every other pipeline, so the tint is
// applied once per band (renderInternalBand) — and once over the composite
// mixed-text patch, which paints rows 160-191 outside that tree.
void Apple2Display::textTintPass(const Memory& mem, const Memory::DisplayState& state,
                                 int scanY0, int scanY1, int x0, int x1)
{
    if (chatMauveActive()) {
        const auto tm = chatMauve->textMode(state.eightyCol, /*an3On=*/!state.dhgr);
        if (tm == LeChatMauveCard::TextMode::Green)
            tintText(state, scanY0, scanY1,
                     0xFF000000u | (uint32_t{0x33} << 16) | (uint32_t{0xFF} << 8) | 0x33u,
                     0, x0, x1);
        else if (tm == LeChatMauveCard::TextMode::Tinted)
            tintText(state, scanY0, scanY1,
                     chatMauvePalette()[chatMauve->textTintColour() & 0x0F], 0, x0, x1,
                     chatMauve->tintKeepsInverseWhite() ? &mem : nullptr);
    }
    // A VidHD's IIgs text colours ($C022) on the Apple's own text, when the
    // card is not showing its Super Hi-Res picture and the monitor is not a
    // monochrome one (its lit dot is not white, so there is nothing to map).
    uint32_t fg = 0, bg = 0;
    if (cardVideo_ && !cardOwnsMonitor(state.textMode, state.an0) &&
        !isMonochrome() && cardTextColours(fg, bg))
        tintText(state, scanY0, scanY1, fg, bg, x0, x1);
}

// Text in one colour: Eve TXTGREEN ([B] Eve manual ch. IX: "blanc → vert",
// 40 and 80 columns; [E] the shade is the P31 tint the MonoGreen pipeline
// uses — the manual gives none) and the RVB Graph's CT. The text
// painters put pure white on pure black, so remapping white in the text
// rows of the band is the whole effect.
void Apple2Display::tintText(const Memory::DisplayState& state, int scanY0, int scanY1,
                             uint32_t colour, uint32_t background, int x0, int x1,
                             const Memory* keepInverseOf)
{
    int lo = 0, hi = 0;
    if (state.textMode) {
        if (!bandScanlines(scanY0, scanY1, 0, kHeight, &lo, &hi)) return;
    } else if (state.mixedMode) {
        if (!bandScanlines(scanY0, scanY1, 160, kHeight, &lo, &hi)) return;
    } else {
        return;
    }
    const int w = useFrame80_ ? kWidth80 : kWidth;
    uint32_t* base = useFrame80_ ? frame80.data() : frame.data();
    const int xa = std::max(0, x0);
    const int xb = (x1 < 0) ? w : std::min(w, x1);
    // The Video-7: inverse and flashing characters (below $80) keep their
    // black and white ([B] SL7 manual via AppleWin PR #837; [D] AppleWin
    // UpdateText40/80ColorCell @ 7fd7fd7c). The cell's byte decides: main in
    // 40 columns, aux then main in 80.
    const uint8_t* mainRam = keepInverseOf ? keepInverseOf->data() : nullptr;
    const int cells = state.eightyCol ? 80 : 40;
    const int cellW = w / cells;
    for (int y = lo; y < hi; ++y) {
        uint32_t* row = base + static_cast<size_t>(y) * w;
        const uint16_t rowAddr = mainRam ? textRowAddress(y / 8, videoTextPage2(state)) : 0;
        for (int x = xa; x < xb; ++x) {
            if (mainRam) {
                const int cell = x / cellW;
                uint8_t ch = 0xFF;
                if (state.eightyCol) {
                    const int col = cell / 2;
                    ch = (cell & 1) ? mainRam[rowAddr + col] : (auxRam ? auxRam[rowAddr + col] : 0xFF);
                } else {
                    ch = mainRam[rowAddr + cell];
                }
                if (ch < 0x80) continue;
            }
            if (row[x] == 0xFFFFFFFFu) row[x] = colour;
            // background == 0: leave the paper alone (no alpha-0 colour is
            // ever painted, so 0 is free as "not asked").
            else if (background && row[x] == 0xFF000000u) row[x] = background;
        }
    }
}

// GR + MIXED with the card's colour text (Video-7 F/B, Eve TXT16 — 80COL
// off): the lo-res rows as usual, bridged to 560, and rows 20-23 through
// the fg/bg painter, as [D] MAME's `text_update` @ e1479b7a does for the
// mixed rows (render_line_color_array). The 280-wide legacy path painted them white
// on black.
bool Apple2Display::renderChatMauveGrMixed(Memory& mem, const Memory::DisplayState& state,
                                           int scanY0, int scanY1)
{
    if (state.textMode || state.hiRes || !state.mixedMode || state.eightyCol ||
        !mem.isIIE() || auxRam == nullptr ||
        chatMauve->textMode(state.eightyCol, /*an3On=*/!state.dhgr)
            != LeChatMauveCard::TextMode::Color)
        return false;
    int gLo = 0, gHi = 0;
    if (bandScanlines(scanY0, scanY1, 0, kMixedTextFirstScanline, &gLo, &gHi)) {
        renderLoRes(mem, state, gLo / 4, (gHi + 3) / 4, 0, 40, gLo, gHi);
        upscaleFrameToFrame80(gLo, gHi);
    }
    renderChatMauveMixedColourText(mem, state, scanY0, scanY1);
    setUseFrame80(true);
    return true;
}

bool Apple2Display::renderChatMauveMixedColourText(Memory& mem,
                                                   const Memory::DisplayState& state,
                                                   int scanY0, int scanY1)
{
    if (!chatMauve || !mem.isIIE() || auxRam == nullptr ||
        chatMauve->textMode(state.eightyCol, /*an3On=*/!state.dhgr)
            != LeChatMauveCard::TextMode::Color)
        return false;
    int tLo = 0, tHi = 0;
    if (bandRows(scanY0, scanY1, 20, 24, &tLo, &tHi))
        renderTextChatMauveFgBg(mem, state, tLo, tHi, scanY0, scanY1,
                                chatMauve->auxHiNibbleIsForeground());
    return true;
}
