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

// Pom1 Apple 1 Emulator
// Copyright (C) 2000-2026 Verhille Arnaud
//
// HGR sprite blit — see HgrSpriteBlit.h. Pure byte placement, pinned by
// hgr_sprite_blit_smoke against hgrpaint::hgrByteOffset.

#include "HgrSpriteBlit.h"

#include "HgrPaintModel.h"   // hgrpaint::hgrByteOffset (page-relative interleave)

#include <vector>

namespace hgrsprite {

// Page-relative offset of byte column `bc` (0..39) at row `r` (0..191), or -1 if
// out of range. hgrByteOffset takes a pixel x; the byte column is x/7, so bc*7
// picks the leftmost pixel of the column.
static int byteAddr(int bc, int r)
{
    if (bc < 0 || bc >= kByteCols || r < 0 || r >= kRows) return -1;
    return hgrpaint::hgrByteOffset(bc * 7, r);
}

void extract(const uint8_t* page, int srcByteCol, int srcRow,
             int wBytes, int hRows, uint8_t* out)
{
    for (int r = 0; r < hRows; ++r)
        for (int b = 0; b < wBytes; ++b) {
            const int off = byteAddr(srcByteCol + b, srcRow + r);
            out[r * wBytes + b] = (off >= 0) ? page[off] : 0;
        }
}

void stamp(const uint8_t* sprite, int wBytes, int hRows,
           int dstByteCol, int dstRow,
           const std::function<void(int, uint8_t)>& poke)
{
    for (int r = 0; r < hRows; ++r)
        for (int b = 0; b < wBytes; ++b) {
            const int off = byteAddr(dstByteCol + b, dstRow + r);
            if (off >= 0) poke(off, sprite[r * wBytes + b]);
        }
}

void magnifyColor2x(const hgrpaint::HgrColor* cells, int wBytes, int hRows,
                    uint8_t* out)
{
    using hgrpaint::HgrColor;
    const int wpx = wBytes * 7;
    const int dW  = wBytes * 2;          // doubled byte width
    const int dH  = hRows * 2;           // doubled row count
    const int n   = dW * dH;
    for (int i = 0; i < n; ++i) out[i] = 0;
    std::vector<uint8_t> pal1(static_cast<size_t>(n), 0);   // per-dest-byte palette flag

    for (int sy = 0; sy < hRows; ++sy)
        for (int sx = 0; sx < wpx; ++sx) {
            const HgrColor c = cells[static_cast<size_t>(sy) * wpx + sx];
            if (c == HgrColor::Black) continue;
            const bool litLeft  = (c == HgrColor::Violet || c == HgrColor::Blue  ||
                                   c == HgrColor::White);   // even (left) column
            const bool litRight = (c == HgrColor::Green  || c == HgrColor::Orange ||
                                   c == HgrColor::White);   // odd (right) column
            const bool p1 = (c == HgrColor::Blue || c == HgrColor::Orange);
            for (int k = 0; k < 2; ++k) {                   // 0 = even/left, 1 = odd/right
                if ((k == 0 && !litLeft) || (k == 1 && !litRight)) continue;
                const int dc = 2 * sx + k;                  // 2*sx is even, +k picks the pair half
                const int byte = dc / 7, bit = dc % 7;
                for (int ky = 0; ky < 2; ++ky) {            // row doubling
                    const size_t idx = static_cast<size_t>(2 * sy + ky) * dW + byte;
                    out[idx] |= static_cast<uint8_t>(1u << bit);
                    if (p1) pal1[idx] = 1;
                }
            }
        }
    for (int i = 0; i < n; ++i) if (pal1[i]) out[i] |= 0x80u;
}

int dhgrExportRowBytes(int shapePxWide)
{
    if (shapePxWide <= 0) return 0;
    // Clip to the DHGR line width FIRST — the rasteriser does, so the export
    // must too. 140 colour pixels × 4 dots = 560 dots = 80 seven-dot byte
    // columns = 40 bytes per plane, i.e. exactly one plane row.
    const int usedPx  = (shapePxWide < hgrpaint::kDhgrWidth) ? shapePxWide
                                                             : hgrpaint::kDhgrWidth;
    const int dotCols = (usedPx * 4 + 6) / 7;
    const int nPer    = (dotCols + 1) / 2;
    return (nPer < kByteCols) ? nPer : kByteCols;
}

void extractDhgrPlanes(const uint8_t* pair, int nPer, int hRows,
                       uint8_t* auxOut, uint8_t* mainOut)
{
    if (!pair || !auxOut || !mainOut || nPer <= 0 || hRows <= 0) return;
    // `nPer` stays the caller's row STRIDE (it sized its tables with it); what
    // is clamped is how much of each row exists to be read. A plane row is
    // kByteCols bytes and a page has kRows of them, so these two bounds are
    // what keep every access inside `pair` no matter what the caller passes —
    // the property whose absence walked the ca65 export off the end of it.
    const int cols = (nPer  < kByteCols) ? nPer  : kByteCols;
    const int rows = (hRows < kRows)     ? hRows : kRows;
    for (int r = 0; r < rows; ++r) {
        const int rowBase = hgrpaint::hgrByteOffset(0, r);
        for (int i = 0; i < cols; ++i) {
            auxOut [static_cast<size_t>(r) * nPer + i] = pair[rowBase + i];
            mainOut[static_cast<size_t>(r) * nPer + i] =
                pair[static_cast<size_t>(hgrpaint::kHiresSize) + rowBase + i];
        }
    }
}

int floodFillMono(uint8_t* page, int wPx, int hRows, int x, int y, bool set)
{
    if (x < 0 || x >= wPx || y < 0 || y >= hRows) return 0;
    const bool seedOn = hgrpaint::pixelOn(page, x, y);
    if (seedOn == set) return 0;
    std::vector<uint8_t> seen(static_cast<size_t>(wPx) * hRows, 0);
    std::vector<std::pair<int,int>> st, region;
    st.emplace_back(x, y);
    seen[static_cast<size_t>(y) * wPx + x] = 1;
    while (!st.empty()) {
        const auto p = st.back(); st.pop_back();
        region.push_back(p);
        const int nb[4][2] = {{p.first-1,p.second},{p.first+1,p.second},
                              {p.first,p.second-1},{p.first,p.second+1}};
        for (auto& n : nb) {
            const int nx = n[0], ny = n[1];
            if (nx < 0 || nx >= wPx || ny < 0 || ny >= hRows) continue;
            const size_t i = static_cast<size_t>(ny) * wPx + nx;
            if (seen[i]) continue;
            if (hgrpaint::pixelOn(page, nx, ny) != seedOn) continue;
            seen[i] = 1;
            st.emplace_back(nx, ny);
        }
    }
    // Raw bits like the pencil: White lights, Black clears, never a
    // chromatic parity pattern (see the editor's transform paths).
    for (auto& p : region)
        hgrpaint::plotPage(page, p.first, p.second,
                           set ? hgrpaint::HgrColor::White : hgrpaint::HgrColor::Black);
    return static_cast<int>(region.size());
}

} // namespace hgrsprite
