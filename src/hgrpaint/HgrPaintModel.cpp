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
// HGR paint model — the emulator-agnostic bit-plotting logic behind
// HgrPaintEditor, split out so it can be unit-tested (hgr_paint_plot_smoke) with
// no GL/ImGui/emulator dependency. Carries its own copy of the Apple II row
// interleave (a hardware fact, identical on any host).

#include "HgrPaintModel.h"

#include <utility>
#include <algorithm>
#include <vector>

namespace hgrpaint {

uint16_t hgrRowAddress(int y)
{
    // Apple II HIRES non-linear memory layout: 192 lines split into 3 groups of
    // 64, each interleaved in blocks of 8 (Woz reused the row counter for DRAM
    // refresh). Page 1 base ($2000); the model works page-relative.
    const int group    = y / 64;          // 0, 1, 2
    const int subGroup = (y % 64) / 8;     // 0-7
    const int line     = y % 8;            // 0-7
    return static_cast<uint16_t>(kHiresBase)
         + static_cast<uint16_t>(group)    * 0x28
         + static_cast<uint16_t>(subGroup) * 0x80
         + static_cast<uint16_t>(line)     * 0x400;
}

int hgrByteOffset(int x, int y)
{
    // hgrRowAddress is $2000-based; subtract the base for a page-relative
    // offset (identical interleave for both pages).
    return (hgrRowAddress(y) - kHiresBase) + x / 7;
}

int hgrBit(int x) { return x % 7; }

int snapColumn(int x, HgrColor c)
{
    if (c == HgrColor::Black || c == HgrColor::White) return x;
    // Violet/Blue live on even columns, Green/Orange on odd.
    const int desiredParity = (c == HgrColor::Violet || c == HgrColor::Blue) ? 0 : 1;
    if ((x & 1) != desiredParity) x += (x > 0) ? -1 : +1;
    return x;
}

int targetOffset(int x, int y, HgrColor c)
{
    if (x < 0 || x > 279 || y < 0 || y > 191) return -1;
    const int sx = snapColumn(x, c);
    if (sx < 0 || sx > 279) return -1;
    return hgrByteOffset(sx, y);
}

int plotPage(uint8_t* page, int x, int y, HgrColor c)
{
    if (x < 0 || x > 279 || y < 0 || y > 191) return -1;
    const int sx = snapColumn(x, c);
    if (sx < 0 || sx > 279) return -1;
    const int off = hgrByteOffset(sx, y);
    const int bit = hgrBit(sx);
    const uint8_t b = page[off];
    uint8_t nb = b;
    switch (c) {
    case HgrColor::Black: nb &= static_cast<uint8_t>(~(1u << bit)); break;
    case HgrColor::White: nb |= static_cast<uint8_t>(1u << bit); break;   // palette untouched
    default:
        // Chromatic: set the byte's shared high bit to the colour's palette,
        // then light the pixel. Flipping the high bit recolours the byte's
        // other pixels — the real HGR/NTSC behaviour.
        if (c == HgrColor::Blue || c == HgrColor::Orange) nb |= 0x80u;
        else                                              nb &= 0x7Fu;
        nb |= static_cast<uint8_t>(1u << bit);
        break;
    }
    if (nb == b) return -1;
    page[off] = nb;
    return off;
}

bool pixelOn(const uint8_t* page, int x, int y)
{
    if (x < 0 || x > 279 || y < 0 || y > 191) return false;
    return (page[hgrByteOffset(x, y)] >> hgrBit(x)) & 1;
}

HgrColor colorAt(const uint8_t* page, int x, int y)
{
    if (x < 0 || x > 279 || y < 0 || y > 191) return HgrColor::Black;
    if (!pixelOn(page, x, y)) return HgrColor::Black;

    // White heuristic: a lit pixel reads white when both horizontal neighbours are
    // also lit (the dot pattern fills, so NTSC sees no isolated colour fringe).
    // pixelOn() reads the neighbour across the byte boundary and bounds-checks the
    // edges, so a white run crossing a byte boundary reads white on its bit-0/bit-6
    // edge columns too (the same-byte-only test used to mis-label those as Violet/
    // Green). For an interior bit this is identical to inspecting the two adjacent
    // bits of the same byte; an isolated lit pixel still has both neighbours off and
    // falls through to the chromatic classifier, so plotPage round-trips unchanged.
    if (pixelOn(page, x - 1, y) && pixelOn(page, x + 1, y))
        return HgrColor::White;

    // Chromatic classification: palette (byte high bit) + column parity.
    // Mirrors snapColumn: palette0 even=Violet, palette0 odd=Green,
    // palette1 even=Blue, palette1 odd=Orange.
    const bool palette1 = (page[hgrByteOffset(x, y)] & 0x80u) != 0;
    const bool even = (x & 1) == 0;
    if (palette1) return even ? HgrColor::Blue   : HgrColor::Orange;
    else          return even ? HgrColor::Violet : HgrColor::Green;
}

bool byteHasPaletteSeam(const uint8_t* page, int byteCol, int y)
{
    // A "palette seam" is the boundary at which artifact-colour bleed occurs:
    // two horizontally adjacent bytes that both have lit pixels but disagree on
    // the shared high bit (palette select). At that seam the NTSC renderer mixes
    // the two palettes' colours, which surprises users. We flag the LEFT byte of
    // any such adjacent pair (byteCol .. byteCol+1).
    //
    // Pure + simple by design so the overlay matches a property the test pins.
    if (byteCol < 0 || byteCol > 38 || y < 0 || y > 191) return false;
    const int base = hgrByteOffset(0, y);
    const uint8_t a = page[base + byteCol];
    const uint8_t b = page[base + byteCol + 1];
    const bool aLit = (a & 0x7Fu) != 0;
    const bool bLit = (b & 0x7Fu) != 0;
    if (!aLit || !bLit) return false;
    return (a & 0x80u) != (b & 0x80u);
}

int setBytePalette(uint8_t* page, int byteCol, int y, int msb)
{
    // Flip only the shared high bit (palette select) of one byte — recolours its
    // 7 pixels (orange<->green, blue<->violet) WITHOUT lighting/clearing any
    // pixel. msb: 0 = clear, 1 = set, anything else = toggle. Returns the changed
    // page offset, or -1 if out of range / no change. The HGR-unique "transparent"
    // palette-shift fadden exposes as HI_BIT_CLEAR/HI_BIT_SET patterns.
    if (byteCol < 0 || byteCol > 39 || y < 0 || y > 191) return -1;
    const int off = hgrByteOffset(0, y) + byteCol;
    const uint8_t b = page[off];
    uint8_t nb = b;
    if (msb == 0)      nb &= 0x7Fu;
    else if (msb == 1) nb |= 0x80u;
    else               nb ^= 0x80u;
    if (nb == b) return -1;
    page[off] = nb;
    return off;
}

int fillRegion(uint8_t* page, int x, int y, HgrColor c, const RenderPageFn& render,
               const std::function<bool(int, int)>& pattern)
{
    if (x < 0 || x > 279 || y < 0 || y > 191 || !render) return 0;

    // Render the page through the host's real NTSC pipeline so connectivity
    // matches the displayed image (see header).
    constexpr int W = kHiresWidth, H = kHiresHeight;
    std::vector<uint32_t> col(static_cast<size_t>(W) * H, 0);
    render(page, col.data());
    auto rgb = [&](int px, int py) { return col[static_cast<size_t>(py) * W + px] & 0x00FFFFFFu; };
    const uint32_t seed = rgb(x, y);

    // 4-connected flood over equal perceived colour.
    std::vector<uint8_t> seen(static_cast<size_t>(W) * H, 0);
    std::vector<std::pair<int,int>> stack, region;
    stack.emplace_back(x, y);
    seen[static_cast<size_t>(y) * W + x] = 1;
    while (!stack.empty()) {
        const std::pair<int,int> p = stack.back(); stack.pop_back();
        const int px = p.first, py = p.second;
        region.emplace_back(px, py);
        const int nb[4][2] = {{px - 1, py}, {px + 1, py}, {px, py - 1}, {px, py + 1}};
        for (auto& n : nb) {
            const int nx = n[0], ny = n[1];
            if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
            const size_t idx = static_cast<size_t>(ny) * W + nx;
            if (seen[idx]) continue;
            if (rgb(nx, ny) != seed) continue;
            seen[idx] = 1;
            stack.emplace_back(nx, ny);
        }
    }

    // Recolour: clear the whole region first (so leftover bits of the old colour
    // can't OR with the new colour's bits into white), then stamp c's pattern.
    // Chromatic colours only live on one column parity, so we light just the
    // matching-parity pixels — no snap-bleed past the region edge.
    for (auto& p : region) plotPage(page, p.first, p.second, HgrColor::Black);
    if (c != HgrColor::Black) {
        const int parity = (c == HgrColor::Violet || c == HgrColor::Blue)  ? 0
                         : (c == HgrColor::Green  || c == HgrColor::Orange) ? 1
                         : -1;   // White: parity-agnostic, light every pixel
        for (auto& p : region) {
            // Pattern gap: leave the pixel black. The tool panel shows the
            // MacPaint pattern strip for the Fill tool in EVERY mode, and the
            // three 16-colour fills have always sampled it — only the 280-HGR
            // fill ignored it, so picking a pattern here silently filled solid.
            if (pattern && !pattern(p.first, p.second)) continue;
            if (parity < 0)                  plotPage(page, p.first, p.second, c);   // White
            else if ((p.first & 1) == parity) plotPage(page, p.first, p.second, c);
        }
    }
    return static_cast<int>(region.size());
}

// ── Apple II lo-res (GR) block model ─────────────────────────────────────────

// Page-relative text/lo-res row address: the same DRAM-refresh interleave the
// GraphicsCard text/lo-res renderer uses (base $0400/$0800 added at call site).
static uint16_t grTextRowAddr(int textRow)
{
    return static_cast<uint16_t>(0x80 * (textRow & 7) + 0x28 * (textRow >> 3));
}

int grBlockOffset(int bx, int by)
{
    if (bx < 0 || bx >= kGrCols || by < 0 || by >= kGrRows) return -1;
    return grTextRowAddr(by / 2) + bx;   // two block-rows share one text row
}

int plotGrBlock(uint8_t* page, int bx, int by, int colorIndex)
{
    const int off = grBlockOffset(bx, by);
    if (off < 0) return -1;
    colorIndex &= 0x0F;
    const uint8_t b = page[off];
    // Even block-row → low nibble (upper block); odd → high nibble (lower block).
    const uint8_t nb = (by & 1)
        ? static_cast<uint8_t>((b & 0x0F) | (colorIndex << 4))
        : static_cast<uint8_t>((b & 0xF0) | colorIndex);
    if (nb == b) return -1;
    page[off] = nb;
    return off;
}

int grBlockColorAt(const uint8_t* page, int bx, int by)
{
    const int off = grBlockOffset(bx, by);
    if (off < 0) return -1;
    return (by & 1) ? (page[off] >> 4) : (page[off] & 0x0F);
}

// ── Apple IIe DHGR (double hi-res) block model ───────────────────────────────

// Pair-buffer offset of dot d (0..559) at row y: even byte-columns are the AUX
// plane (offset 0..0x1FFF), odd ones MAIN (offset 0x2000..0x3FFF); both share
// the HIRES row interleave.
static inline int dhgrDotOffset(int d, int rowBase)
{
    const int byteCol = d / 7;
    return ((byteCol & 1) ? kHiresSize : 0) + rowBase + byteCol / 2;
}

int dhgrPixelOffsets(int x, int y, int outOff[2])
{
    if (x < 0 || x >= kDhgrWidth || y < 0 || y > 191) return 0;
    const int rowBase = hgrRowAddress(y) - kHiresBase;
    outOff[0] = dhgrDotOffset(4 * x, rowBase);
    outOff[1] = dhgrDotOffset(4 * x + 3, rowBase);
    return (outOff[1] == outOff[0]) ? 1 : 2;
}

int plotDhgrPixel(uint8_t* pagePair, int x, int y, int colorIndex)
{
    if (x < 0 || x >= kDhgrWidth || y < 0 || y > 191) return -1;
    const int rowBase = hgrRowAddress(y) - kHiresBase;
    const int v = dhgrColorToNibble(colorIndex & 0x0F);
    int changed = 0;
    for (int i = 0; i < 4; ++i) {
        const int d    = 4 * x + i;
        const int off  = dhgrDotOffset(d, rowBase);
        const int bit  = d % 7;
        const uint8_t old = pagePair[off];
        const uint8_t nb  = (v >> i) & 1
            ? static_cast<uint8_t>(old |  (1u << bit))
            : static_cast<uint8_t>(old & ~(1u << bit));
        if (nb != old) { pagePair[off] = nb; ++changed; }
    }
    return changed;
}

int dhgrColorAt(const uint8_t* pagePair, int x, int y)
{
    if (x < 0 || x >= kDhgrWidth || y < 0 || y > 191) return -1;
    const int rowBase = hgrRowAddress(y) - kHiresBase;
    int v = 0;
    for (int i = 0; i < 4; ++i) {
        const int d = 4 * x + i;
        if ((pagePair[dhgrDotOffset(d, rowBase)] >> (d % 7)) & 1) v |= 1 << i;
    }
    return dhgrNibbleToColor(v);
}

// ── Apple IIe double lo-res (DLGR) block model ───────────────────────────────

int dlgrBlockOffset(int bx, int by)
{
    if (bx < 0 || bx >= kDlgrCols || by < 0 || by >= kGrRows) return -1;
    const int textOff = grTextRowAddr(by / 2) + bx / 2;
    return ((bx & 1) ? 0x400 : 0) + textOff;   // even = aux plane (first)
}

int plotDlgrBlock(uint8_t* pair, int bx, int by, int colorIndex)
{
    const int off = dlgrBlockOffset(bx, by);
    if (off < 0) return -1;
    int v = colorIndex & 0x0F;
    // Aux nibbles display rotated left by 1 — pre-rotate right so the screen
    // shows colourIndex (renderLoResDouble: palette[rotl4(aNib)]).
    if (!(bx & 1)) v = ((v >> 1) | (v << 3)) & 0x0F;
    const uint8_t b = pair[off];
    const uint8_t nb = (by & 1)
        ? static_cast<uint8_t>((b & 0x0F) | (v << 4))
        : static_cast<uint8_t>((b & 0xF0) | v);
    if (nb == b) return -1;
    pair[off] = nb;
    return off;
}

int dlgrBlockColorAt(const uint8_t* pair, int bx, int by)
{
    const int off = dlgrBlockOffset(bx, by);
    if (off < 0) return -1;
    int v = (by & 1) ? (pair[off] >> 4) : (pair[off] & 0x0F);
    if (!(bx & 1)) v = ((v << 1) | (v >> 3)) & 0x0F;   // undo the aux rotation
    return v;
}

void rotateClipCW(int& w, int& h, bool sixteen, bool blockMode,
                  std::vector<HgrColor>& px, std::vector<int8_t>& idx)
{
    if (w <= 0 || h <= 0) return;
    auto at = [&](int x, int y) { return static_cast<size_t>(y) * w + x; };

    if (blockMode && sixteen) {
        // Rotate the BLOCK grid and re-expand it at the block pitch (see the
        // header): sample each block's centre, transpose the grid, expand.
        const int bw = (w + 6) / 7, bh = (h + 3) / 4;
        std::vector<int8_t> blk(static_cast<size_t>(bw) * bh, 0);
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx)
                blk[static_cast<size_t>(by) * bw + bx] =
                    idx[at(std::min(bx * 7 + 3, w - 1), std::min(by * 4 + 1, h - 1))];
        const int rw = bh * 7, rh = bw * 4;
        std::vector<int8_t> out(static_cast<size_t>(rw) * rh, 0);
        for (int ry = 0; ry < bw; ++ry)          // rotated block row = old column
            for (int rx = 0; rx < bh; ++rx) {    // rotated block col = old row
                const int8_t v = blk[static_cast<size_t>(bh - 1 - rx) * bw + ry];
                for (int yy = 0; yy < 4; ++yy)
                    for (int xx = 0; xx < 7; ++xx)
                        out[static_cast<size_t>(ry * 4 + yy) * rw + rx * 7 + xx] = v;
            }
        w = rw; h = rh; idx = std::move(out);
        return;
    }

    // Per-pixel clip: (x,y) -> (h-1-y, x), dims swap.
    const int rw = h, rh = w;
    std::vector<int8_t>   oi;
    std::vector<HgrColor> op;
    if (sixteen) oi.assign(static_cast<size_t>(rw) * rh, 0);
    else         op.assign(static_cast<size_t>(rw) * rh, HgrColor::Black);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t d = static_cast<size_t>(x) * rw + (h - 1 - y);
            if (sixteen) oi[d] = idx[at(x, y)];
            else         op[d] = px[at(x, y)];
        }
    w = rw; h = rh;
    if (sixteen) idx = std::move(oi); else px = std::move(op);
}

} // namespace hgrpaint
