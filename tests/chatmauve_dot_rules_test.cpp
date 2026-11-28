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

// Le Chat Mauve dot rules — docs/chatmauve_plan.md P1.
//
// The oracle is AppleWin `RGBMonitor.cpp` (GPL): `UpdateHiResRGBCell` (the
// LCM HGR rule fenarinarsa verified against every bit combination on a real
// //c adapter) and `UpdateDHiResCellRGB` (the mixed-mode boundary rules of
// PR #837, validated on the same box). Both are re-implemented here as
// literally as C++ allows — same 28-bit windows, same static carry across
// cells — and POM2's per-line renderers are compared with them over every
// dot of random rows plus the hand-built boundary cases. What this pins:
//
//   1. HGR LCM rule: 2-bit cell aligned to the line, 3-bit window, bank from
//      the dot's own byte; guards of 0 at both ends. Every byte pair
//      (x, x+1) for 4096 random rows + all 16 bit-7 bank combinations.
//   2. Mixed DHGR: per-byte mux, free-running 4-dot cell, "cut" into a BW
//      byte, "repeat last BW dot" into a colour byte. Random rows, and the
//      three named cases with the expected dots written out.
//   3. COL140 stays the same picture whether the row is decoded through the
//      MAME 28-bit window (the previous code) or the per-cell path (the
//      new one) — the refactor did not move a nibble.
//   4. Variant fallbacks: Féline latch 160 → COL140 (AppleWin
//      RGB_Is140Mode), Eve latch mixed → COL140 (Manuel Arlequin).

#include "Apple2Display.h"
#include "LeChatMauveCard.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>   // std::size

namespace {

using Mode    = LeChatMauveCard::RenderMode;
using Variant = LeChatMauveCard::Variant;

constexpr uint32_t kWhite = 0xFFFFFFFFu;
constexpr uint32_t kBlack = 0xFF000000u;

// Mirrors Apple2Display.cpp::kChatMauveLoResPalette (AppleWin PaletteRGB_Feline,
// entries 12..27) — lo-res order.
const uint32_t kFeline[16] = {
    0xFF000000u, 0xFF4C12ACu, 0xFF830700u, 0xFFD11AAAu,
    0xFF2F8300u, 0xFF7E979Fu, 0xFFB58A00u, 0xFFFF9E9Fu,
    0xFF005F7Au, 0xFF4772FFu, 0xFF7F6878u, 0xFFCF7AFFu,
    0xFF2CE66Fu, 0xFF7BF6FFu, 0xFFB2EE6Cu, 0xFFFFFFFFu,
};
// AppleWin g_pPaletteRGB[0..5] for the Feline: BLACK, WHITE, BLUE, ORANGE,
// GREEN, MAGENTA (RGBMonitor.cpp PaletteRGB_Feline[]).
const uint32_t kAwHgr[6] = {
    kBlack, kWhite, 0xFFB58A00u, 0xFF4772FFu, 0xFF2CE66Fu, 0xFFD11AAAu,
};

uint32_t lcg(uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

uint16_t hgrAddr(int y)
{
    return static_cast<uint16_t>(0x2000 + 0x400 * (y & 7) + 0x80 * ((y >> 3) & 7)
                                 + 0x28 * (y >> 6));
}

// ── Oracle 1: AppleWin UpdateHiResRGBCell, one byte column x → 14 dots ────
void awHiResRGBCell(int x, const uint8_t main[40], uint32_t out[14])
{
    int xoffset = x & 1;
    const int addr = x - xoffset;
    const uint8_t byteval1 = (x < 2 ? 0 : main[addr - 1]);
    const uint8_t byteval2 = main[addr];
    const uint8_t byteval3 = main[addr + 1];
    const uint8_t byteval4 = (x >= 38 ? 0 : main[addr + 2]);
    uint32_t dwordval = (byteval1 & 0x7F) | ((byteval2 & 0x7F) << 7)
                      | ((byteval3 & 0x7F) << 14) | ((byteval4 & 0x7F) << 21);

    uint32_t colors[14];
    uint32_t dwordval_tmp = dwordval >> 7;
    bool offset = (byteval2 & 0x80) != 0;
    for (int i = 0; i < 14; i++) {
        if (i == 7) offset = (byteval3 & 0x80) != 0;
        const int color = dwordval_tmp & 0x3;
        // AppleWin indexes g_pPaletteRGB, its FULL 16-entry palette, so
        // `6 - color` at color == 0 lands on a real (if meaningless) entry
        // there. kAwHgr is only the 6-entry slice this oracle needs, so the
        // same expression runs one past the end — which is what the nightly
        // UBSan leg reported ("index 6 out of bounds for type 'unsigned
        // int [6]'"). The value is dead either way: the loop below reads
        // colors[i] ONLY when the 3-dot window is 010 or 101, i.e. exactly
        // when color is 1 or 2. Keep AppleWin's arithmetic verbatim — it is
        // the point of the oracle — and give the entries nothing reads a
        // defined value instead of a read off the end.
        const int idx = offset ? 1 + color : 6 - color;
        colors[i] = (idx < static_cast<int>(std::size(kAwHgr)))
                        ? kAwHgr[idx] : kBlack;
        if (i % 2) dwordval_tmp >>= 2;
    }
    const uint32_t mask = 0x01C0, chck1 = 0x0140, chck2 = 0x0080;
    if (xoffset) { dwordval >>= 7; xoffset = 7; }
    int o = 0;
    for (int i = xoffset; i < xoffset + 7; i++) {
        uint32_t c;
        if (((dwordval & mask) == chck1) || ((dwordval & mask) == chck2)) c = colors[i];
        else c = (dwordval & chck2) ? kWhite : kBlack;
        out[o++] = c; out[o++] = c;
        dwordval >>= 1;
    }
}

// ── Oracle 2: AppleWin UpdateDHiResCellRGB, one byte column x → 14 dots ───
// The two statics carry across cells exactly as in AppleWin (they are reset
// per row here; AppleWin lets them run across rows, which only matters for
// a partial cell at column 0 — there is none, 0 % 4 == 0).
bool g_dhgrLastCellIsColor = true;
int  g_dhgrLastBit = 0;

void awDHiResCellRGB(int x, const uint8_t aux[40], const uint8_t main[40],
                     uint32_t out[14], bool isMixMode, bool isBit7Inversed)
{
    const int xoffset = x & 1;
    const int addr = x - xoffset;
    uint8_t byteval1 = aux[addr], byteval2 = main[addr];
    uint8_t byteval3 = aux[addr + 1], byteval4 = main[addr + 1];
    uint32_t dwordval = (byteval1 & 0x7F) | ((byteval2 & 0x7F) << 7)
                      | ((byteval3 & 0x7F) << 14) | ((byteval4 & 0x7F) << 21);
    uint32_t colors[7];
    uint32_t dwordval_tmp = dwordval;
    for (int i = 0; i < 7; i++) {
        const int bits = dwordval_tmp & 0xF;
        const int color = ((bits & 7) << 1) | ((bits & 8) >> 3);
        colors[i] = kFeline[color];
        dwordval_tmp >>= 4;
    }
    const uint32_t bw[2] = { kFeline[0], kFeline[15] };
    if (isBit7Inversed) {
        byteval1 = static_cast<uint8_t>(~byteval1); byteval2 = static_cast<uint8_t>(~byteval2);
        byteval3 = static_cast<uint8_t>(~byteval3); byteval4 = static_cast<uint8_t>(~byteval4);
    }
    uint32_t* pDst = out;
    auto bwRun = [&]() {
        for (int i = 0; i < 7; i++) { g_dhgrLastBit = dwordval & 1; *(pDst++) = bw[g_dhgrLastBit]; dwordval >>= 1; }
        g_dhgrLastCellIsColor = false;
    };
    if (xoffset == 0) {
        if ((byteval1 & 0x80) || !isMixMode) {
            for (int k = 0; k < 4; ++k) *(pDst++) = colors[0];
            for (int k = 0; k < 3; ++k) *(pDst++) = colors[1];
            dwordval >>= 7; g_dhgrLastCellIsColor = true;
        } else bwRun();
        if ((byteval2 & 0x80) || !isMixMode) {
            *(pDst++) = g_dhgrLastCellIsColor ? colors[1] : bw[g_dhgrLastBit];
            for (int k = 0; k < 4; ++k) *(pDst++) = colors[2];
            for (int k = 0; k < 2; ++k) *(pDst++) = colors[3];
            g_dhgrLastCellIsColor = true;
        } else bwRun();
    } else {
        dwordval >>= 14;
        if ((byteval3 & 0x80) || !isMixMode) {
            for (int k = 0; k < 2; ++k) *(pDst++) = g_dhgrLastCellIsColor ? colors[3] : bw[g_dhgrLastBit];
            for (int k = 0; k < 4; ++k) *(pDst++) = colors[4];
            *(pDst++) = colors[5];
            dwordval >>= 7; g_dhgrLastCellIsColor = true;
        } else bwRun();
        if ((byteval4 & 0x80) || !isMixMode) {
            for (int k = 0; k < 3; ++k) *(pDst++) = g_dhgrLastCellIsColor ? colors[5] : bw[g_dhgrLastBit];
            for (int k = 0; k < 4; ++k) *(pDst++) = colors[6];
            g_dhgrLastCellIsColor = true;
        } else bwRun();
    }
}

void awDhgrRow(const uint8_t aux[40], const uint8_t main[40], uint32_t out[560],
               bool mix, bool inv)
{
    g_dhgrLastCellIsColor = true; g_dhgrLastBit = 0;
    for (int x = 0; x < 40; ++x) awDHiResCellRGB(x, aux, main, out + x * 14, mix, inv);
}

// ── Oracle 3: MAME dhgr_update rgbmode 3 (COL140) — the previous POM2 code ─
void mameCol140Row(const uint8_t aux[40], const uint8_t main[40], uint32_t out[560])
{
    for (int c = 0; c < 40; c += 2) {
        const unsigned w = (aux[c] & 0x7Fu) | ((main[c] & 0x7Fu) << 7)
                         | ((aux[c + 1] & 0x7Fu) << 14) | ((main[c + 1] & 0x7Fu) << 21);
        for (int b = 0; b < 28; ++b) {
            const unsigned nib = (w >> (b & ~3u)) & 0x0Fu;
            out[c * 14 + b] = kFeline[((nib << 1) | (nib >> 3)) & 0x0Fu];
        }
    }
}

struct Rig {
    Memory mem;
    Apple2Display disp;
    LeChatMauveCard card;
    explicit Rig(Variant v) : card(7, v) {
        mem.setIIEMode(true);
        disp.setAuxMemory(mem.auxData());
        disp.setChatMauveCard(&card);
        disp.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
        mem.memRead(0xC050); mem.memRead(0xC057); mem.memRead(0xC054);
    }
    void dhgrOn()  { mem.memWrite(0xC00D, 0); mem.memRead(0xC05E); }
    void putRow(int y, const uint8_t* aux, const uint8_t* main) {
        const uint16_t a = hgrAddr(y);
        for (int c = 0; c < 40; ++c) {
            if (aux) mem.auxDataMutable()[a + c] = aux[c];
            mem.memWrite(static_cast<uint16_t>(a + c), main[c]);
        }
    }
    const uint32_t* row(int y) { return disp.pixels() + static_cast<size_t>(y) * 560; }
};

int compareRow(const uint32_t* got, const uint32_t* want, const char* what, int y)
{
    for (int x = 0; x < 560; ++x) {
        if (got[x] != want[x]) {
            std::printf("MISMATCH %s row %d x %d: pom2=%08X oracle=%08X\n",
                        what, y, x, got[x], want[x]);
            return 1;
        }
    }
    return 0;
}

// ── 1. HGR LCM rule vs UpdateHiResRGBCell ─────────────────────────────────
void testHgrLcm()
{
    Rig rig(Variant::Feline);
    uint32_t seed = 0x5EED1234u;
    int bad = 0;
    for (int round = 0; round < 64; ++round) {
        uint8_t rows[192][40];
        for (int y = 0; y < 192; ++y) {
            for (int c = 0; c < 40; ++c) rows[y][c] = static_cast<uint8_t>(lcg(seed) >> 24);
            // Sprinkle the sparse patterns the rule is about (lone dots,
            // lone holes) so random noise does not drown them.
            if (y % 3 == 0) for (int c = 0; c < 40; ++c) rows[y][c] &= static_cast<uint8_t>(lcg(seed) >> 24);
            if (y % 5 == 0) for (int c = 0; c < 40; ++c) rows[y][c] |= static_cast<uint8_t>(lcg(seed) >> 24) & 0x7F;
            rig.putRow(y, nullptr, rows[y]);
        }
        rig.disp.render(rig.mem);
        assert(rig.disp.width() == 560);
        for (int y = 0; y < 192; ++y) {
            uint32_t want[560];
            for (int x = 0; x < 40; ++x) awHiResRGBCell(x, rows[y], want + x * 14);
            bad += compareRow(rig.row(y), want, "hgr-lcm", y);
            if (bad > 5) { assert(false && "HGR LCM diverges from UpdateHiResRGBCell"); }
        }
    }
    assert(bad == 0);
    std::printf("  HGR LCM rule: 64 x 192 rows == AppleWin UpdateHiResRGBCell\n");
}

// ── 2. Mixed DHGR vs UpdateDHiResCellRGB ──────────────────────────────────
void testMixedRandom(Variant v, bool inv)
{
    Rig rig(v);
    rig.dhgrOn();
    rig.card.overrideMode(Mode::Mixed);
    rig.card.setInvertBit7(inv);
    assert(rig.card.dhgrMode() == LeChatMauveCard::DhgrMode::Mixed);
    uint32_t seed = 0xC0FFEEu ^ (inv ? 0x77u : 0u);
    int bad = 0;
    for (int round = 0; round < 32; ++round) {
        uint8_t aux[192][40], main[192][40];
        for (int y = 0; y < 192; ++y) {
            for (int c = 0; c < 40; ++c) {
                aux[y][c]  = static_cast<uint8_t>(lcg(seed) >> 24);
                main[y][c] = static_cast<uint8_t>(lcg(seed) >> 24);
            }
            rig.putRow(y, aux[y], main[y]);
        }
        rig.disp.render(rig.mem);
        for (int y = 0; y < 192; ++y) {
            uint32_t want[560];
            awDhgrRow(aux[y], main[y], want, /*mix=*/true, inv);
            bad += compareRow(rig.row(y), want, inv ? "mixed-inv" : "mixed", y);
            if (bad > 5) { assert(false && "mixed DHGR diverges from UpdateDHiResCellRGB"); }
        }
    }
    assert(bad == 0);
    std::printf("  Mixed DHGR (%s%s): 32 x 192 rows == AppleWin UpdateDHiResCellRGB\n",
                LeChatMauveCard::variantKey(v), inv ? ", bit7 inverted" : "");
}

// The three boundary cases, with the dots written out by hand from the
// rules (plan § 3.3), so the test says what the picture IS, not only that it
// matches somebody else's code.
void testMixedBoundaries()
{
    Rig rig(Variant::Feline);
    rig.dhgrOn();
    rig.card.overrideMode(Mode::Mixed);

    uint8_t aux[40] = {0}, main[40] = {0};

    // Row 0 — ALIGNED: aux col 0 colour with bits 0-3 = $A (nibble 1010 →
    // dots 0,2 lit) → cell 0 = code rotl(1010)= 0101 = 5 (grey 1); bits 4-6
    // = 000 start cell 1 whose 4th bit is main col 0's bit 0 (0) → cell 1 =
    // colour 0 (black). main col 0 also colour, bits 1-6 all 1 → cell 2
    // (dots 8-11 = main bits 1-4 → 1111 → white), cell 3 (dots 12,13 = main
    // bits 5,6 = 11, + aux col 1 bits 0,1 = 00 → nibble 0011 → rotl → 0110
    // = 6, medium blue).
    aux[0] = 0x8A; main[0] = 0xFE; aux[1] = 0x80; main[1] = 0x80;
    rig.putRow(0, aux, main);

    // Row 1 — COLOUR → BW CUT: aux col 0 colour, all seven bits 1 (cells 0
    // and the first 3 dots of cell 1 = white? cell 1's nibble = aux bits
    // 4,5,6 + main bit 0 = 1,1,1,0 → 0111 → rotl → 1110 = 14 aqua). main
    // col 0 BW with bits 0110000 → dot 7 (=main bit 0) black, dots 8,9
    // white, rest black. The colour cell 1 is CUT at dot 7: dots 4,5,6
    // aqua, dot 7 black (its own bit).
    std::memset(aux, 0, 40); std::memset(main, 0, 40);
    aux[0] = 0xFF; main[0] = 0x06;
    rig.putRow(1, aux, main);

    // Row 2 — BW → COLOUR REPEAT: aux col 0 BW with bit 6 = 1 (dot 6 white,
    // dots 0-5 black); main col 0 colour with bits = 1111111 → dot 7 is the
    // tail of cell 1, which began in the BW byte → REPEATS the last BW dot
    // (white). Then cells 2 (dots 8-11) and 3 (dots 12,13 + aux col 1 bits
    // 0,1 = 00 → nibble 0011 → 6 medium blue).
    std::memset(aux, 0, 40); std::memset(main, 0, 40);
    aux[0] = 0x40; main[0] = 0xFF; aux[1] = 0x80; main[1] = 0x80;
    rig.putRow(2, aux, main);

    // Row 3 — same as row 2 but the last BW dot is 0 → the repeated dot is
    // black, not the cell's colour (the cell's nibble 0111 would be aqua).
    std::memset(aux, 0, 40); std::memset(main, 0, 40);
    aux[0] = 0x3F; main[0] = 0xFF; aux[1] = 0x80; main[1] = 0x80;
    rig.putRow(3, aux, main);

    rig.disp.render(rig.mem);

    const uint32_t grey1 = kFeline[5], white = kFeline[15], black = kFeline[0];
    const uint32_t mblue = kFeline[6], aqua = kFeline[14];

    { const uint32_t* r = rig.row(0);
      for (int d = 0; d < 4; ++d)   assert(r[d] == grey1);
      for (int d = 4; d < 8; ++d)   assert(r[d] == black);
      for (int d = 8; d < 12; ++d)  assert(r[d] == white);
      for (int d = 12; d < 16; ++d) assert(r[d] == mblue); }
    { const uint32_t* r = rig.row(1);
      for (int d = 0; d < 4; ++d)   assert(r[d] == white);
      for (int d = 4; d < 7; ++d)   assert(r[d] == aqua);
      assert(r[7] == black);                               // the cut
      assert(r[8] == white && r[9] == white);
      for (int d = 10; d < 14; ++d) assert(r[d] == black); }
    { const uint32_t* r = rig.row(2);
      for (int d = 0; d < 6; ++d)   assert(r[d] == black);
      assert(r[6] == white);
      assert(r[7] == white);                               // repeat of dot 6
      for (int d = 8; d < 12; ++d)  assert(r[d] == white);
      for (int d = 12; d < 16; ++d) assert(r[d] == mblue); }
    { const uint32_t* r = rig.row(3);
      for (int d = 0; d < 6; ++d)   assert(r[d] == white);
      assert(r[6] == black);
      assert(r[7] == black);                               // repeat of dot 6 (0)
      for (int d = 8; d < 12; ++d)  assert(r[d] == white); }
    std::printf("  Mixed DHGR boundaries: aligned / colour->BW cut / BW->colour repeat (1 and 0)\n");
}

// ── 3. COL140: per-cell path == the MAME 28-bit window it replaced ────────
void testCol140Unchanged()
{
    Rig rig(Variant::Feline);
    rig.dhgrOn();
    assert(rig.card.currentMode() == Mode::COL140);
    uint32_t seed = 0x140140u;
    uint8_t aux[192][40], main[192][40];
    for (int y = 0; y < 192; ++y) {
        for (int c = 0; c < 40; ++c) {
            aux[y][c]  = static_cast<uint8_t>(lcg(seed) >> 24);
            main[y][c] = static_cast<uint8_t>(lcg(seed) >> 24);
        }
        rig.putRow(y, aux[y], main[y]);
    }
    rig.disp.render(rig.mem);
    for (int y = 0; y < 192; ++y) {
        uint32_t want[560], want2[560];
        mameCol140Row(aux[y], main[y], want);
        awDhgrRow(aux[y], main[y], want2, /*mix=*/false, false);
        assert(compareRow(rig.row(y), want, "col140-mame", y) == 0);
        assert(compareRow(rig.row(y), want2, "col140-applewin", y) == 0);
    }
    std::printf("  COL140: 192 rows == MAME dhgr_update == AppleWin (no-mix)\n");
}

// ── 4. Variant fallbacks ──────────────────────────────────────────────────
void testFallbacks()
{
    using D = LeChatMauveCard::DhgrMode;
    LeChatMauveCard feline(7, Variant::Feline), iic(7, Variant::IIcAdapter);
    LeChatMauveCard eve(7, Variant::Eve), v7(7, Variant::Video7);
    for (auto* c : { &feline, &iic, &eve, &v7 }) c->overrideMode(Mode::Chunky160);
    assert(feline.dhgrMode() == D::COL140 && iic.dhgrMode() == D::COL140);
    assert(eve.dhgrMode() == D::COL140);
    assert(v7.dhgrMode() == D::Chunky160);
    for (auto* c : { &feline, &iic, &eve, &v7 }) c->overrideMode(Mode::Mixed);
    assert(feline.dhgrMode() == D::Mixed && iic.dhgrMode() == D::Mixed);
    assert(eve.dhgrMode() == D::COL140);           // "la carte Eve n'est pas compatible"
    assert(v7.dhgrMode() == D::Mixed);
    for (auto* c : { &feline, &iic, &eve, &v7 }) c->overrideMode(Mode::BW560);
    for (auto* c : { &feline, &iic, &v7 }) assert(c->dhgrMode() == D::BW560);
    // The Eve follows the latch at HR 000 only while ENHRCPREG is off ([A]
    // AppleWin #764 with the AppleColor demo, switches at power-on); with it
    // on — Purplesoft's AN3-off modes — table IX-1 rules ([C] `& GR 6`).
    assert(eve.dhgrMode() == D::BW560);
    eve.setEveSwitch(LeChatMauveCard::ENHRCPREG, true);
    assert(eve.dhgrMode() == D::COL140);

    // The picture agrees: a Féline with the latch at 160 renders COL140.
    Rig rig(Variant::Feline);
    rig.dhgrOn();
    uint8_t aux[40], main[40];
    uint32_t seed = 0x160u;
    for (int c = 0; c < 40; ++c) { aux[c] = static_cast<uint8_t>(lcg(seed) >> 24); main[c] = static_cast<uint8_t>(lcg(seed) >> 24); }
    rig.putRow(0, aux, main);
    rig.card.overrideMode(Mode::Chunky160);
    rig.disp.render(rig.mem);
    uint32_t want[560];
    mameCol140Row(aux, main, want);
    assert(compareRow(rig.row(0), want, "feline-160-fallback", 0) == 0);
    std::printf("  Fallbacks: Feline/IIc 160->140, Eve mixed->140 and 160->140, Video-7 keeps all four\n");
}

} // namespace

int main()
{
    std::printf("Le Chat Mauve dot rules:\n");
    testHgrLcm();
    testMixedRandom(Variant::Feline, false);
    testMixedRandom(Variant::IIcAdapter, true);
    testMixedRandom(Variant::Video7, false);
    testMixedBoundaries();
    testCol140Unchanged();
    testFallbacks();
    std::printf("Le Chat Mauve dot rules: OK\n");
    return 0;
}
