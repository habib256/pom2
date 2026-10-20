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

// IIe DHGR rendering smoke test — pins:
//  - The dhgr bit is set/cleared by $C05E/$C05F when iieMode is on.
//  - Aux/main interleave: aux byte at column c covers dots [c*14..c*14+6],
//    main byte covers [c*14+7..c*14+13]; bit 0 of each byte is the
//    leftmost dot of its half.
//  - Three color paths (one per HiResMode group):
//      ColorNTSC    → MAME composite artifact LUT, per-pixel decode with
//                     `rotl4b(_, absX+1)` (matches MAME's is_80_column=1).
//      ChatMauveRGB → MAME RGB-card 4-dot block decode, `rotl4(n,1)`,
//                     indexes the Chat Mauve palette (distinct grays).
//      MonoWhite/Green/Amber → dot-by-dot luminance × phosphor tint.
//  - The DHGR dispatcher only triggers on iieMode + 80COL + HIRES + DHGR
//    AND textMode is off.
//
// Headless: builds a Memory + Apple2Display + LeChatMauveCard directly.

#include "Apple2Display.h"
#include "LeChatMauveCard.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint16_t IIE_80COL_ON    = 0xC00D;
constexpr uint16_t CLR_TEXT        = 0xC050;
constexpr uint16_t SET_HIRES       = 0xC057;
constexpr uint16_t DHIRES_ON       = 0xC05E;
constexpr uint16_t DHIRES_OFF      = 0xC05F;

// Lo-res palette indices used in the assertions below. Values are in
// `kLoResPalette[]` (NTSC) and `kChatMauveLoResPalette[]` (Péritel).
constexpr uint32_t kBlack            = 0xFF000000u;
constexpr uint32_t kNtscDark5_Light10 = 0xFF808080u; // NTSC: idx 5 == idx 10
// Chat Mauve palette = AppleWin `PaletteRGB_Feline` (empirical capture
// of a real Le Chat Mauve "Feline" board). Indices 5 and 10 are tinted
// (olive / mauve) but still visibly distinct — the two-grays trademark.
constexpr uint32_t kCmDark5          = 0xFF7E979Fu;  // rgb(0x9f,0x97,0x7e)
constexpr uint32_t kCmLight10        = 0xFF7F6878u;  // rgb(0x78,0x68,0x7f)
constexpr uint32_t kNtscMediumBlue6  = 0xFFFF9019u;  // NTSC idx 6 unchanged
constexpr uint32_t kCmMediumBlue6    = 0xFFB58A00u;  // rgb(0x00,0x8a,0xb5)

uint16_t hgrAddr(int y)
{
    return static_cast<uint16_t>(0x2000
        + 0x400 * (y & 7)
        + 0x80  * ((y >> 3) & 7)
        + 0x28  * (y >> 6));
}

void clearAux(Memory& mem)
{
    std::memset(mem.auxDataMutable(), 0, 0x10000);
}

void clearMain(Memory& mem)
{
    for (uint32_t a = 0x2000; a < 0x4000; ++a)
        mem.memWrite(static_cast<uint16_t>(a), 0);
}

}  // namespace

int main()
{
    Memory mem;
    mem.setIIEMode(true);
    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());

    // Plug a Le Chat Mauve card so ChatMauveRGB DHGR doesn't fall back.
    // The card's render-time interface is not exercised by DHGR (we only
    // check `chatMauve != nullptr` to gate the palette + 4-dot path).
    LeChatMauveCard chatMauve;
    disp.setChatMauveCard(&chatMauve);

    // ── Soft-switch wiring: dhgr bit toggles on $C05E/$C05F.
    assert(!mem.getDisplayState().dhgr);
    mem.memRead(DHIRES_ON);   assert( mem.getDisplayState().dhgr);
    mem.memRead(DHIRES_OFF);  assert(!mem.getDisplayState().dhgr);
    mem.memRead(DHIRES_ON);   assert( mem.getDisplayState().dhgr);

    // Switch to graphics + 80col + hires + dhgr.
    mem.memRead(CLR_TEXT);
    mem.memRead(SET_HIRES);
    mem.memWrite(IIE_80COL_ON, 0);

    // ── Test pattern A: aux=0x55, main=0x14 — designed to put two
    // *distinct* gray-coded cells side by side, so the Chat Mauve
    // "two-grays" trademark is empirically visible.
    //
    //   aux 0x55  → dots 0..6 = 1,0,1,0,1,0,1
    //   main 0x14 → dots 7..13 = 0,0,1,0,1,0,0      (0x14 = 0b0001_0100)
    //
    // RGB-card "raw nibble" interpretation (bit 0 = leftmost):
    //   Cell 0 (dots 0..3):  raw = 0b0101 = 5  → rotl4 = 0b1010 = 10
    //   Cell 1 (dots 4..7):  raw = 0b0101 = 5  → rotl4 = 0b1010 = 10
    //   Cell 2 (dots 8..11): raw = 0b1010 = 10 → rotl4 = 0b0101 = 5
    //   Cell 3 (dots 12..15, last 2 zero from byte 1):
    //                        raw = 0b0000 = 0  → rotl4 = 0b0000 = 0  (black)
    {
        const uint16_t addr0 = hgrAddr(0);
        clearAux(mem);
        clearMain(mem);
        mem.auxDataMutable()[addr0] = 0x55;
        mem.memWrite(addr0, 0x14);
    }

    // ── ColorNTSC (composite) path. Per-pixel decode via kArtifactColorLut.
    // For the leftmost pixels the LUT produces palette idx 10 (Light Gray
    // in NTSC = 0xFF808080). We don't pin every pixel — composite per-
    // pixel coloring varies — but the leftmost cell anchor and the all-
    // zero region are stable.
    disp.setHiResMode(Apple2Display::HiResMode::ColorNTSC);
    disp.render(mem);
    assert(disp.width()  == 560);
    assert(disp.height() == 192);
    {
        const uint32_t* pix = disp.pixels();
        // Anchor: far-right scanline of the all-zero region is black.
        assert(pix[100 * 560 + 540] == kBlack);
        // Composite at the leftmost pixel: LUT[0x28]=0x55, rotl4b(0x55,1)
        // = 10 → kLoResPalette[10] = 0xFF808080.
        assert(pix[0] == kNtscDark5_Light10);
    }

    // ── ChatMauveRGB (RGB-card) path. Same pattern, but per-cell (4-dot
    // block), `rotl4(n,1)` rotation, and the Chat Mauve palette.
    //   Cells 0..1 → idx 10 = #AAAAAA (Light Gray Chat Mauve).
    //   Cell 2     → idx 5  = #555555 (Dark Gray Chat Mauve, distinct
    //                from Light Gray — the famous two-grays trademark).
    //   Cell 3     → idx 0  = black.
    disp.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
    disp.render(mem);
    {
        const uint32_t* pix = disp.pixels();
        // Cells 0 + 1 → idx 10 (Light Gray Chat Mauve).
        assert(pix[0]  == kCmLight10);
        assert(pix[3]  == kCmLight10);   // last pixel of cell 0
        assert(pix[4]  == kCmLight10);   // first pixel of cell 1
        assert(pix[7]  == kCmLight10);   // last pixel of cell 1
        // Cell 2 → idx 5 (Dark Gray Chat Mauve, distinct from Light).
        assert(pix[8]  == kCmDark5);
        assert(pix[9]  == kCmDark5);
        assert(pix[10] == kCmDark5);
        assert(pix[11] == kCmDark5);
        // Cell 3 → black.
        assert(pix[12] == kBlack);
        assert(pix[15] == kBlack);
        // The trademark: idx 5 and idx 10 are *distinct* under Chat Mauve.
        assert(kCmDark5 != kCmLight10);
    }

    // ── MAME "too much mauve" regression pin.
    // Pattern [1,1,0,0] (raw nibble 3) must produce Medium Blue (idx 6 =
    // 0xFFFF9019) and *not* Purple (idx 3 = 0xFFFF28E6). Both the
    // composite path (LUT → rotl4b yields idx 6 at absX=0) and the RGB-card
    // path (rotl4(3,1)=6) agree at the leftmost pixel.
    {
        const uint16_t addr1 = hgrAddr(1);
        mem.auxDataMutable()[addr1] = 0x03;     // dots 0,1 = 1,1; dots 2..6 = 0
        mem.memWrite(addr1, 0x00);

        // Composite first: pix[0..1] should be Medium Blue (NTSC palette,
        // 0xFFFF9019), pix[2..3] black. The "different colours within a
        // single 4-dot cell" property is unique to composite — proves the
        // per-pixel artifact decode is active (the RGB-card path would
        // paint all 4 the same).
        disp.setHiResMode(Apple2Display::HiResMode::ColorNTSC);
        disp.render(mem);
        const uint32_t* pix = disp.pixels();
        assert(pix[1 * 560 + 0] == kNtscMediumBlue6);
        assert(pix[1 * 560 + 1] == kNtscMediumBlue6);
        assert(pix[1 * 560 + 2] == kBlack);
        assert(pix[1 * 560 + 3] == kBlack);
        assert(pix[1 * 560 + 0] != pix[1 * 560 + 2]);   // intra-cell variation

        // RGB-card path on the same pattern: cell 0 is all Medium Blue
        // (Chat Mauve palette idx 6 = rgb(0x00,0x8a,0xb5), the AppleWin
        // Feline BLUE — different shade from NTSC idx 6 because the
        // Péritel RGB decoder reads a different point on the colour wheel).
        disp.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
        disp.render(mem);
        const uint32_t* pix2 = disp.pixels();
        assert(pix2[1 * 560 + 0] == kCmMediumBlue6);
        assert(pix2[1 * 560 + 1] == kCmMediumBlue6);
        assert(pix2[1 * 560 + 2] == kCmMediumBlue6);
        assert(pix2[1 * 560 + 3] == kCmMediumBlue6);
        // Both modes agree the colour is in the "blue" family — neither
        // is purple. This is the original "too much mauve" regression pin.
        assert(pix [1 * 560 + 0] != 0xFFFF28E6u);   // NTSC Purple idx 3
        assert(pix2[1 * 560 + 0] != 0xFFD11AAAu);   // Chat Mauve idx 3 (Feline magenta)
    }

    // ── Mixed-mode flag: DHGR top + 80-col text bottom. Just exercise the
    // dispatcher; pixel values for the text rows aren't pinned here.
    disp.setHiResMode(Apple2Display::HiResMode::ColorNTSC);
    mem.memRead(0xC053);  // SET_MIXED
    disp.render(mem);
    assert(disp.width() == 560);

    // ── Disable DHGR: dispatcher falls back to the 280-wide HGR path.
    mem.memRead(DHIRES_OFF);
    mem.memRead(0xC052);  // CLR_MIXED
    assert(!mem.getDisplayState().dhgr);
    disp.render(mem);
    assert(disp.width() == 280);

    // ── Mono path: enable DHGR, MonoGreen tint must be greenish at the
    // first lit dot (aux bit 0 = 1).
    mem.memRead(DHIRES_ON);
    disp.setHiResMode(Apple2Display::HiResMode::MonoGreen);
    disp.render(mem);
    assert(disp.width() == 560);
    {
        const uint32_t* pix = disp.pixels();
        const uint32_t litGreen = pix[0];
        assert(((litGreen >> 24) & 0xFF) == 0xFF);
        const uint32_t rPix = (litGreen >>  0) & 0xFF;
        const uint32_t gPix = (litGreen >>  8) & 0xFF;
        const uint32_t bPix = (litGreen >> 16) & 0xFF;
        assert(gPix > rPix);
        assert(gPix > bPix);
    }

    std::printf("dhgr_render_smoke OK\n");
    return 0;
}
