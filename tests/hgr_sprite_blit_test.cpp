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

// HGR sprite blit smoke test — the harness `HgrSpriteBlit.h` has always
// claimed ("unit-tests standalone (hgr_sprite_blit_smoke)") and POM2 never
// had. It pins the byte-level layer the sprite editor adds on top of the
// already-pinned hgrpaint pixel model:
//
//   1. extract / stamp round-trip through the Apple II non-linear row
//      interleave, and their documented clipping at the page edges.
//   2. magnifyColor2x: the doubled sprite's footprint, its 2-aligned colour
//      clock (Violet/Blue = even column, Green/Orange = odd, White = both)
//      and the per-byte palette high bit.
//   3. dhgrExportRowBytes / extractDhgrPlanes — the DHGR-target ca65 export
//      path, and the bug hunt 8 defect behind them: the export used to derive
//      its per-row byte count from the UN-CLIPPED shape width, while the
//      rasteriser clips at kDhgrWidth (140 colour pixels = one 40-byte plane
//      row). From W = 21 bytes the tables ran on into the following rows'
//      bytes; from W = 25, on the last row (y = 191, page offset $1FD0), they
//      ran past the end of the 16 KB pair itself — a heap over-read of up to
//      31 bytes at the UI maximum W = 40 / H = 192.

#include "hgrpaint/HgrPaintModel.h"
#include "hgrsprite/HgrSpriteBlit.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <vector>

using hgrpaint::HgrColor;
using namespace hgrsprite;

namespace {

void testExtractStampRoundTrip()
{
    std::vector<uint8_t> page(hgrpaint::kHiresSize, 0);
    // A 3x16 sprite of distinctive bytes at byte column 5, row 37.
    const int wB = 3, hR = 16, col = 5, row = 37;
    std::vector<uint8_t> src(static_cast<size_t>(wB) * hR);
    for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>(i * 7 + 1);

    stamp(src.data(), wB, hR, col, row,
          [&](int off, uint8_t v) { page[off] = v; });

    // Every byte must have landed on the interleaved address the pixel model
    // computes for that (column, row) — the one fact this module owns.
    for (int r = 0; r < hR; ++r)
        for (int b = 0; b < wB; ++b)
            assert(page[hgrpaint::hgrByteOffset((col + b) * 7, row + r)] ==
                   src[static_cast<size_t>(r) * wB + b]);

    std::vector<uint8_t> back(src.size(), 0xFF);
    extract(page.data(), col, row, wB, hR, back.data());
    assert(back == src);

    std::printf("  ok: extract/stamp round-trip through the row interleave\n");
}

void testStampClipsAtThePageEdges()
{
    std::vector<uint8_t> page(hgrpaint::kHiresSize, 0);
    const int wB = 4, hR = 4;
    std::vector<uint8_t> src(static_cast<size_t>(wB) * hR, 0xAA);

    // Straddling both edges: only the in-page cells may be written, and
    // nothing outside the 8 KB page may be touched (the callback is the only
    // way out, so an off-page cell must simply not call it).
    int writes = 0;
    stamp(src.data(), wB, hR, kByteCols - 2, kRows - 2,
          [&](int off, uint8_t v) {
              assert(off >= 0 && off < hgrpaint::kHiresSize);
              page[off] = v;
              ++writes;
          });
    assert(writes == 2 * 2);

    // extract mirrors it: off-page cells read as 0, not as neighbouring bytes.
    std::vector<uint8_t> back(src.size(), 0xFF);
    extract(page.data(), kByteCols - 2, kRows - 2, wB, hR, back.data());
    for (int r = 0; r < hR; ++r)
        for (int b = 0; b < wB; ++b) {
            const uint8_t got = back[static_cast<size_t>(r) * wB + b];
            assert(got == ((r < 2 && b < 2) ? 0xAA : 0x00));
        }

    std::printf("  ok: stamp/extract clip at the page edges\n");
}

void testMagnifyColor2x()
{
    const int wB = 2, hR = 3;
    const int wpx = wB * 7;
    std::vector<HgrColor> cells(static_cast<size_t>(wpx) * hR, HgrColor::Black);
    cells[0] = HgrColor::Violet;                 // palette 0, even column only
    cells[1] = HgrColor::Green;                  // palette 0, odd column only
    cells[2] = HgrColor::White;                  // both columns
    cells[3] = HgrColor::Blue;                   // palette 1, even column only
    cells[static_cast<size_t>(wpx)] = HgrColor::Orange;   // row 1, palette 1, odd

    std::vector<uint8_t> out(static_cast<size_t>(wB * 2) * (hR * 2), 0x5A);
    magnifyColor2x(cells.data(), wB, hR, out.data());

    const int dW = wB * 2;
    auto dot = [&](int dx, int dy) {
        return (out[static_cast<size_t>(dy) * dW + dx / 7] >> (dx % 7)) & 1;
    };
    // Source cell 0 (Violet) → dest dots 0,1 on rows 0,1: even lit, odd dark.
    assert(dot(0, 0) && !dot(1, 0) && dot(0, 1) && !dot(1, 1));
    // Cell 1 (Green) → dots 2,3: odd lit, even dark.
    assert(!dot(2, 0) && dot(3, 0));
    // Cell 2 (White) → dots 4,5: both lit.
    assert(dot(4, 0) && dot(5, 0));
    // Cell 3 (Blue) → dots 6,7: even lit, and its byte carries palette 1.
    assert(dot(6, 0) && !dot(7, 0));
    assert(out[0] & 0x80);            // byte 0 holds the Blue dot (dot 6)
    // Row doubling: row 1 of the output repeats row 0.
    assert(out[0] == out[static_cast<size_t>(dW)]);
    // Orange on source row 1 lands on output rows 2 and 3, palette 1.
    assert(out[static_cast<size_t>(2) * dW] & 0x80);
    // Untouched trailing bytes were zeroed by magnifyColor2x, not left as 0x5A.
    assert(out.back() == 0);

    std::printf("  ok: magnifyColor2x colour clock + palette bit + doubling\n");
}

void testDhgrExportRowBytes()
{
    // A DHGR line is kDhgrWidth colour pixels = 560 dots = 80 seven-dot byte
    // columns = 40 bytes per plane. A shape exactly that wide fills a plane row.
    assert(dhgrExportRowBytes(hgrpaint::kDhgrWidth) == kByteCols);
    // Narrower shapes scale linearly: 2 bytes of plane row per byte of shape.
    assert(dhgrExportRowBytes(7) == 2);
    assert(dhgrExportRowBytes(70) == 20);
    // Degenerate input is not a row.
    assert(dhgrExportRowBytes(0) == 0);
    assert(dhgrExportRowBytes(-5) == 0);
    // THE REGRESSION: every width the editor's sliders can produce (1..40
    // bytes, i.e. 7..280 shape pixels) must stay inside one plane row. The old
    // formula returned 2*wBytes here — 80 at the maximum, twice the row.
    for (int wBytes = 1; wBytes <= kByteCols; ++wBytes) {
        const int nPer = dhgrExportRowBytes(wBytes * 7);
        assert(nPer >= 1 && nPer <= kByteCols);
    }
    std::printf("  ok: dhgrExportRowBytes clips at the DHGR line width\n");
}

void testExtractDhgrPlanesStaysInsideThePair()
{
    // Fill a 16 KB pair with a per-plane, per-offset pattern so a byte lifted
    // from the wrong plane or the wrong row is detectable.
    std::vector<uint8_t> pair(hgrpaint::kDhgrPairSize, 0);
    for (int i = 0; i < hgrpaint::kDhgrPairSize; ++i)
        pair[static_cast<size_t>(i)] = static_cast<uint8_t>(i * 31 + (i >> 8));

    const int nPer = dhgrExportRowBytes(hgrpaint::kDhgrWidth);   // widest legal
    const int hR   = hgrpaint::kHiresHeight;                     // UI maximum
    std::vector<uint8_t> aux(static_cast<size_t>(nPer) * hR, 0);
    std::vector<uint8_t> main(aux.size(), 0);
    extractDhgrPlanes(pair.data(), nPer, hR, aux.data(), main.data());

    for (int r = 0; r < hR; ++r) {
        const int rowBase = hgrpaint::hgrByteOffset(0, r);
        // The row must fit the plane it is read from — the property whose
        // absence let the last row (rowBase $1FD0) run off the end of `pair`.
        assert(rowBase + nPer <= hgrpaint::kHiresSize);
        for (int i = 0; i < nPer; ++i) {
            assert(aux [static_cast<size_t>(r) * nPer + i] == pair[rowBase + i]);
            assert(main[static_cast<size_t>(r) * nPer + i] ==
                   pair[static_cast<size_t>(hgrpaint::kHiresSize) + rowBase + i]);
        }
    }

    // Over-large arguments cannot take it outside `pair`: the reads clamp to
    // one plane row and kRows rows, and the columns/rows past those bounds are
    // left as the caller initialised them. (The sprite editor is not the only
    // conceivable caller of a module documented as portable.)
    const int wideStride = kByteCols * 4;
    const int tallRows   = kRows * 2;
    std::vector<uint8_t> aux2(static_cast<size_t>(wideStride) * tallRows, 0xEE);
    std::vector<uint8_t> main2(aux2.size(), 0xEE);
    extractDhgrPlanes(pair.data(), wideStride, tallRows,
                      aux2.data(), main2.data());
    assert(aux2[0] == pair[0]);
    assert(aux2[kByteCols - 1] == pair[kByteCols - 1]);
    assert(aux2[kByteCols] == 0xEE);                 // past the plane row
    assert(aux2[static_cast<size_t>(kRows) * wideStride] == 0xEE);   // past row 191

    std::printf("  ok: extractDhgrPlanes reads only inside the 16 KB pair\n");
}

}  // namespace

// Bug hunt #11: the sprite editor's flood used artifact-colour connectivity,
// so a full-width shape's first and last pixel columns never joined the
// region — a fill (or erase) left a 1-px frame down both sides.
static void testFloodFillMonoReachesTheEdgeColumns()
{
    using namespace hgrpaint;
    const int wPx = 21, hRows = 10;
    std::vector<uint8_t> page(kHiresSize, 0);
    for (int y = 2; y < 8; ++y)
        for (int x = 0; x < wPx; ++x) plotPage(page.data(), x, y, HgrColor::White);
    const int cleared = hgrsprite::floodFillMono(page.data(), wPx, hRows, 10, 4, /*set=*/false);
    assert(cleared == wPx * 6 && "the erase must cover the whole solid block");
    for (int y = 2; y < 8; ++y) {
        assert(!pixelOn(page.data(), 0, y) && "column 0 survived the erase");
        assert(!pixelOn(page.data(), wPx - 1, y) && "the last column survived the erase");
    }
    const int lit = hgrsprite::floodFillMono(page.data(), wPx, hRows, 10, 4, /*set=*/true);
    assert(lit == wPx * hRows && "an all-black sprite fills completely, edges included");
    assert(pixelOn(page.data(), 0, 0) && pixelOn(page.data(), wPx - 1, hRows - 1));
    std::printf("  floodFillMono reaches both edge columns: OK\n");
}

int main()
{
    testFloodFillMonoReachesTheEdgeColumns();
    std::printf("hgr_sprite_blit_smoke\n");
    testExtractStampRoundTrip();
    testStampClipsAtThePageEdges();
    testMagnifyColor2x();
    testDhgrExportRowBytes();
    testExtractDhgrPlanesStaysInsideThePair();
    std::printf("PASS\n");
    return 0;
}
