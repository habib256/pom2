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

// DHGR paint-model smoke test — pins the hgrpaint DHGR block model against
// the REAL Apple2Display::renderDhgr pipeline:
//
//   1. dhgrPixelOffsets: plane interleave (even byte-columns = AUX plane
//      first in the 16 KB pair, odd = MAIN) and the 1-or-2-byte span of an
//      aligned 4-dot pixel.
//   2. plotDhgrPixel / dhgrColorAt round-trip for all 16 colours.
//   3. The nibble↔colour mapping (colour = rotl4(nibble,1), derived from
//      MAME's square-filter decode): a page solidly filled with colour c via
//      plotDhgrPixel must render as kLoResPalette-style uniform colour c
//      through renderDhgr in BOTH ColorComp4Bit (square) and ColorNTSC (LUT)
//      modes — pinning that what the editor plots is what the machine shows.

#include "Apple2Display.h"
#include "Memory.h"
#include "PaintCardBatcher.h"
#include "hgrpaint/HgrPaintModel.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace hgrpaint;

namespace {

// Stage a 16 KB editor pair (aux 8 KB + main 8 KB, page-relative) into an
// IIe Memory at $2000 and render DHGR through the given mode.
void renderPair(const std::vector<uint8_t>& pair, Apple2Display::HiResMode mode,
                Memory& mem, Apple2Display& disp)
{
    for (int i = 0; i < kHiresSize; ++i)
        mem.writeRamUnchecked(static_cast<uint16_t>(0x2000 + i),
                              pair[kHiresSize + i]);          // main plane
    std::memcpy(mem.auxDataMutable() + 0x2000, pair.data(), kHiresSize);
    disp.setHiResMode(mode);
    disp.render(mem);
    assert(disp.width() == Apple2Display::kWidth80);          // DHGR = 560 wide
}

} // namespace

int main()
{
    // ── 1. Plane interleave + pixel spans ────────────────────────────────────
    {
        int offs[2];
        // Pixel 0 = dots 0-3, all inside byte-column 0 → one AUX byte.
        int n = dhgrPixelOffsets(0, 0, offs);
        assert(n == 1 && offs[0] < kHiresSize);
        // Pixel 1 = dots 4-7: dot 6 ends byte-column 0 (aux), dot 7 starts
        // byte-column 1 (main) → spans the aux/main plane boundary.
        n = dhgrPixelOffsets(1, 0, offs);
        assert(n == 2 && offs[0] < kHiresSize && offs[1] >= kHiresSize);
        // Out of range.
        assert(dhgrPixelOffsets(kDhgrWidth, 0, offs) == 0);
        assert(dhgrPixelOffsets(0, 192, offs) == 0);
    }

    // ── 2. plot / colorAt round-trip, all colours, plane-straddling pixels ──
    {
        std::vector<uint8_t> pair(kDhgrPairSize, 0);
        for (int c = 0; c < 16; ++c)
            for (int x : {0, 1, 2, 69, 70, 138, 139})
                for (int y : {0, 1, 63, 64, 191}) {
                    plotDhgrPixel(pair.data(), x, y, c);
                    assert(dhgrColorAt(pair.data(), x, y) == c);
                }
        // Nibble mapping is the rotl4-by-1 derived from MAME's decode.
        for (int v = 0; v < 16; ++v) {
            assert(dhgrNibbleToColor(dhgrColorToNibble(v)) == v);
            assert(dhgrNibbleToColor(v) == (((v << 1) | (v >> 3)) & 0x0F));
        }
    }

    // ── 3. Solid fills render as the plotted colour through the REAL pipeline ─
    {
        Memory mem;
        mem.setIIEMode(true);
        Apple2Display disp;
        disp.setAuxMemory(mem.auxData());
        // DHGR soft switches: GRAPHICS, full screen, page 1, HIRES, 80COL, AN3.
        mem.memWrite(0xC050, 0); mem.memWrite(0xC052, 0); mem.memWrite(0xC054, 0);
        mem.memWrite(0xC057, 0); mem.memWrite(0xC00D, 0); mem.memWrite(0xC05E, 0);

        for (int c = 0; c < 16; ++c) {
            std::vector<uint8_t> pair(kDhgrPairSize, 0);
            for (int y = 0; y < 192; ++y)
                for (int x = 0; x < kDhgrWidth; ++x)
                    plotDhgrPixel(pair.data(), x, y, c);

            for (auto mode : {Apple2Display::HiResMode::ColorComp4Bit,
                              Apple2Display::HiResMode::ColorNTSC}) {
                renderPair(pair, mode, mem, disp);
                const uint32_t* px = disp.pixels();
                // Reference = an interior dot; the whole interior must be
                // uniform (edges keep zero context in the NTSC LUT window).
                const uint32_t ref = px[96 * 560 + 280];
                for (int y = 0; y < 192; ++y)
                    for (int d = 8; d < 552; ++d)
                        assert(px[y * 560 + d] == ref);
                // And it must be the plotted colour: pin against a fresh
                // lo-res render of the same index? Simpler: pin the exact
                // mapping via ColorComp4Bit for the two anchors that have
                // unambiguous RGB, black and white, and require every colour
                // to differ from black unless c==0 (greys 5/10 share RGB,
                // so a full 16-way RGB uniqueness check would be wrong).
                if (c == 0)  assert((ref & 0x00FFFFFF) == 0);
                if (c == 15) assert((ref & 0x00FFFFFF) == 0x00FFFFFF);
                if (c != 0)  assert((ref & 0x00FFFFFF) != 0);
            }
        }

        // Cross-pin the full 16-colour mapping against lo-res GR, which shares
        // the palette: GR block colour c and DHGR pixel colour c must render
        // the same RGB. (GR = text page $0400, 40×48 blocks.)
        for (int c = 0; c < 16; ++c) {
            // DHGR solid render (4-bit square mode — flat palette decode).
            std::vector<uint8_t> pair(kDhgrPairSize, 0);
            for (int y = 0; y < 192; ++y)
                for (int x = 0; x < kDhgrWidth; ++x)
                    plotDhgrPixel(pair.data(), x, y, c);
            renderPair(pair, Apple2Display::HiResMode::ColorComp4Bit, mem, disp);
            const uint32_t dhgrRgb = disp.pixels()[96 * 560 + 280] & 0x00FFFFFF;

            // Lo-res solid render of the same index.
            mem.memWrite(0xC00C, 0);  // 80COL off
            mem.memWrite(0xC05F, 0);  // AN3 off
            mem.memWrite(0xC056, 0);  // LORES
            const uint8_t bb = static_cast<uint8_t>(c | (c << 4));
            for (int i = 0; i < 0x400; ++i)
                mem.writeRamUnchecked(static_cast<uint16_t>(0x0400 + i), bb);
            disp.render(mem);
            assert(disp.width() == Apple2Display::kWidth);
            const uint32_t grRgb = disp.pixels()[96 * 280 + 140] & 0x00FFFFFF;
            assert(dhgrRgb == grRgb);

            // Back to DHGR switches for the next iteration.
            mem.memWrite(0xC057, 0); mem.memWrite(0xC00D, 0); mem.memWrite(0xC05E, 0);
        }
    }

    // ── 4. DLGR block model vs the real renderLoResDouble ───────────────────
    // Pins the aux/main column interleave, the pair layout (aux 1 KB first)
    // and the aux nibble rotation (display = rotl4(aux nibble)): a page solidly
    // plotted colour c must render uniformly as the SAME RGB in DLGR and GR.
    {
        Memory mem;
        mem.setIIEMode(true);
        Apple2Display disp;
        disp.setAuxMemory(mem.auxData());
        disp.setHiResMode(Apple2Display::HiResMode::ColorNTSC);

        // Round-trip + plane split first.
        {
            std::vector<uint8_t> pair(kDlgrPairSize, 0);
            for (int c = 0; c < 16; ++c)
                for (int bx : {0, 1, 78, 79})
                    for (int by : {0, 1, 46, 47}) {
                        plotDlgrBlock(pair.data(), bx, by, c);
                        assert(dlgrBlockColorAt(pair.data(), bx, by) == c);
                    }
            assert(dlgrBlockOffset(0, 0) < 0x400);    // even col = aux plane
            assert(dlgrBlockOffset(1, 0) >= 0x400);   // odd col = main plane
        }

        for (int c = 0; c < 16; ++c) {
            std::vector<uint8_t> pair(kDlgrPairSize, 0);
            for (int by = 0; by < kGrRows; ++by)
                for (int bx = 0; bx < kDlgrCols; ++bx)
                    plotDlgrBlock(pair.data(), bx, by, c);
            // Stage: aux plane → aux $0400, main plane → main $0400. DLGR
            // switches: GRAPHICS + full + page1 + LORES + 80COL + AN3.
            for (int i = 0; i < 0x400; ++i)
                mem.writeRamUnchecked(static_cast<uint16_t>(0x0400 + i),
                                      pair[0x400 + i]);
            std::memcpy(mem.auxDataMutable() + 0x0400, pair.data(), 0x400);
            mem.memWrite(0xC050, 0); mem.memWrite(0xC052, 0); mem.memWrite(0xC054, 0);
            mem.memWrite(0xC056, 0); mem.memWrite(0xC00D, 0); mem.memWrite(0xC05E, 0);
            disp.render(mem);
            assert(disp.width() == Apple2Display::kWidth80);
            const uint32_t dlgrRgb = disp.pixels()[96 * 560 + 280] & 0x00FFFFFF;
            for (int y = 0; y < 192; ++y)
                for (int x = 0; x < 560; ++x)
                    assert((disp.pixels()[y * 560 + x] & 0x00FFFFFF) == dlgrRgb);

            // Cross-pin against single lo-res of the same index.
            mem.memWrite(0xC00C, 0); mem.memWrite(0xC05F, 0);
            const uint8_t bb = static_cast<uint8_t>(c | (c << 4));
            for (int i = 0; i < 0x400; ++i)
                mem.writeRamUnchecked(static_cast<uint16_t>(0x0400 + i), bb);
            disp.render(mem);
            assert(disp.width() == Apple2Display::kWidth);
            const uint32_t grRgb = disp.pixels()[96 * 280 + 140] & 0x00FFFFFF;
            assert(dlgrRgb == grRgb);
        }
    }

    // ── 5. Monochrome lo-res rendering (2026-07-12) ──────────────────────────
    // On a mono monitor a lo-res nibble displays as its repeating 4-bit dot
    // pattern (the colour generator keeps cycling at 14.318 MHz). Pins:
    // GR white/black solid, GR grey 5 (0101 pattern → each 280-wide pixel
    // averages one lit + one dark sample = uniform 127), and DLGR grey 5 at
    // native 560 dots (alternating full/dark dots, aux rotation included).
    {
        Memory mem;
        mem.setIIEMode(true);
        Apple2Display disp;
        disp.setAuxMemory(mem.auxData());
        disp.setHiResMode(Apple2Display::HiResMode::MonoWhite);

        // GR: switches + solid fills.
        mem.memWrite(0xC050, 0); mem.memWrite(0xC052, 0); mem.memWrite(0xC054, 0);
        mem.memWrite(0xC056, 0); mem.memWrite(0xC00C, 0); mem.memWrite(0xC05F, 0);
        auto fillGr = [&](uint8_t nib) {
            const uint8_t bb = static_cast<uint8_t>(nib | (nib << 4));
            for (int i = 0; i < 0x400; ++i)
                mem.writeRamUnchecked(static_cast<uint16_t>(0x0400 + i), bb);
            disp.render(mem);
            assert(disp.width() == Apple2Display::kWidth);
        };
        fillGr(15);
        for (int i = 0; i < 280 * 192; ++i)
            assert((disp.pixels()[i] & 0xFFFFFF) == 0xFFFFFF);
        fillGr(0);
        for (int i = 0; i < 280 * 192; ++i)
            assert((disp.pixels()[i] & 0xFFFFFF) == 0x000000);
        fillGr(5);
        for (int i = 0; i < 280 * 192; ++i)
            assert((disp.pixels()[i] & 0xFFFFFF) == 0x7F7F7F);

        // DLGR: grey 5 renders as alternating full/dark dots at 560 wide.
        std::vector<uint8_t> pair(kDlgrPairSize, 0);
        for (int by = 0; by < kGrRows; ++by)
            for (int bx = 0; bx < kDlgrCols; ++bx)
                plotDlgrBlock(pair.data(), bx, by, 5);
        for (int i = 0; i < 0x400; ++i)
            mem.writeRamUnchecked(static_cast<uint16_t>(0x0400 + i), pair[0x400 + i]);
        std::memcpy(mem.auxDataMutable() + 0x0400, pair.data(), 0x400);
        mem.memWrite(0xC00D, 0); mem.memWrite(0xC05E, 0);
        disp.render(mem);
        assert(disp.width() == Apple2Display::kWidth80);
        for (int y = 0; y < 192; ++y)
            for (int x = 0; x < 560; ++x) {
                const uint32_t want = ((5 >> (x & 3)) & 1) ? 0xFFFFFF : 0x000000;
                assert((disp.pixels()[y * 560 + x] & 0xFFFFFF) == want);
            }
    }

    // ── The paint host's write batcher must always come back to depth 0 ──
    //
    // Every poke the editors make goes through this. A bracket left open
    // strands every later poke in the pending vector — the canvas silently
    // stops reaching the machine and the vector grows without bound for the
    // rest of the session — which is exactly what an editor operation nested
    // inside an open stroke used to do (HgrPaintEditor::beginStroke held a
    // plain bool, so the inner end() flipped it off and the outer commit
    // never called endBatch()). The batcher's own contract:
    //   * nesting coalesces into ONE commit, at the outermost end();
    //   * a stray end() with nothing open is a no-op, not an underflow;
    //   * balanced brackets always return depth() to 0.
    {
        int commits = 0;
        PaintCardBatcher::Writes seen;
        PaintCardBatcher b([&](const PaintCardBatcher::Writes& w) {
            ++commits;
            seen = w;
        });
        assert(b.depth() == 0);

        b.poke(0x2000, 1);                    // unbracketed: commits at once
        assert(commits == 1 && seen.size() == 1 && b.depth() == 0);

        b.begin();                            // outer (a shape drag)
        assert(b.depth() == 1);
        b.poke(0x2001, 2);
        b.begin();                            // inner (a cut fired mid-drag)
        assert(b.depth() == 2);
        b.poke(0x2002, 3);
        b.end();                              // inner end must NOT flush
        assert(b.depth() == 1 && commits == 1);
        b.poke(0x2003, 4);
        b.end();                              // outer end flushes all three
        assert(b.depth() == 0);
        assert(commits == 2 && seen.size() == 3);
        assert(seen[0].first == 0x2001 && seen[2].first == 0x2003);

        b.end();                              // unbalanced: no underflow
        assert(b.depth() == 0 && commits == 2);

        b.poke(0x2004, 5);                    // still reaching the card
        assert(commits == 3 && seen.size() == 1 && seen[0].first == 0x2004);
    }

    std::printf("dhgr_paint_model: OK (incl. PaintCardBatcher nesting)\n");
    return 0;
}
