// Paint editor model pins — bug hunt #11.
//
//  1. fillRegion honours the MacPaint fill pattern. The tool panel offers the
//     pattern strip for the Fill tool in every mode and the three 16-colour
//     fills sample it; the 280-HGR fill was the one tool that filled solid.
//  2. rotateClipCW is block-aware for lo-res clips. A GR/DLGR clip is stored
//     at canvas-pixel resolution but one sample is a 7x4 block; the sample-
//     space transpose turned a 3x3-block selection into a 2x6 smear with a
//     whole source column dropped.

#include "hgrpaint/HgrConvert.h"
#include "hgrpaint/HgrPaintModel.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace hgrpaint;

namespace {

void renderPage(const uint8_t* page8k, uint32_t* out)
{
    for (int y = 0; y < kHiresHeight; ++y)
        hgrDecodeScanlineRgb(page8k + hgrByteOffset(0, y), out + static_cast<size_t>(y) * kHiresWidth);
}

int litInside(const uint8_t* page, int x0, int y0, int x1, int y1)
{
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            if (pixelOn(page, x, y)) ++n;
    return n;
}

}  // namespace

int main()
{
    // ── 1. fillRegion + pattern ──────────────────────────────────────────
    {
        auto box = [] {
            std::vector<uint8_t> page(kHiresSize, 0);
            // A white frame from (20,20) to (60,60); the interior is black.
            for (int x = 20; x <= 60; ++x) { plotPage(page.data(), x, 20, HgrColor::White); plotPage(page.data(), x, 60, HgrColor::White); }
            for (int y = 20; y <= 60; ++y) { plotPage(page.data(), 20, y, HgrColor::White); plotPage(page.data(), 60, y, HgrColor::White); }
            return page;
        };
        std::vector<uint8_t> solid = box(), patterned = box();
        const int nSolid = fillRegion(solid.data(), 40, 40, HgrColor::White, renderPage);
        auto checker = [](int x, int y) { return ((x + y) & 1) == 0; };
        const int nPat = fillRegion(patterned.data(), 40, 40, HgrColor::White, renderPage, checker);
        assert(nSolid == nPat && nSolid > 0 && "the region itself must not depend on the pattern");
        const int litSolid = litInside(solid.data(), 21, 21, 60, 60);
        const int litPat   = litInside(patterned.data(), 21, 21, 60, 60);
        assert(litSolid == 39 * 39 && "a solid fill lights the whole interior");
        int expect = 0;
        for (int y = 21; y < 60; ++y) for (int x = 21; x < 60; ++x) if (checker(x, y)) ++expect;
        if (litPat != expect)
            std::printf("FAIL: patterned fill lit %d interior pixels, want %d (solid=%d)\n", litPat, expect, litSolid);
        assert(litPat == expect && "the HGR fill ignored the fill pattern");
        for (int y = 21; y < 60; ++y)
            for (int x = 21; x < 60; ++x)
                assert(pixelOn(patterned.data(), x, y) == checker(x, y) && "wrong pixel lit");
        std::printf("  fillRegion honours the pattern: OK\n");
    }

    // ── 2. rotateClipCW, block-aware ─────────────────────────────────────
    {
        // 3x3 lo-res blocks holding 1..9, stored at canvas-pixel resolution.
        int w = 21, h = 12;
        std::vector<int8_t> idx(static_cast<size_t>(w) * h, 0);
        std::vector<HgrColor> px;
        for (int by = 0; by < 3; ++by)
            for (int bx = 0; bx < 3; ++bx)
                for (int yy = 0; yy < 4; ++yy)
                    for (int xx = 0; xx < 7; ++xx)
                        idx[static_cast<size_t>(by * 4 + yy) * w + bx * 7 + xx] = static_cast<int8_t>(by * 3 + bx + 1);
        rotateClipCW(w, h, /*sixteen=*/true, /*blockMode=*/true, px, idx);
        assert(w == 21 && h == 12 && "a 3x3-block clip stays 3x3 blocks after a rotation");
        const int want[3][3] = { {7, 4, 1}, {8, 5, 2}, {9, 6, 3} };
        for (int by = 0; by < 3; ++by)
            for (int bx = 0; bx < 3; ++bx) {
                const int got = idx[static_cast<size_t>(by * 4 + 1) * w + bx * 7 + 3];
                if (got != want[by][bx])
                    std::printf("FAIL: rotated block (%d,%d) = %d, want %d\n", bx, by, got, want[by][bx]);
                assert(got == want[by][bx]);
            }
        // Per-pixel clip: the plain transpose is unchanged.
        int pw = 3, ph = 2;
        std::vector<HgrColor> ppx = { HgrColor::White, HgrColor::Black, HgrColor::Violet,
                                      HgrColor::Green, HgrColor::Orange, HgrColor::Blue };
        std::vector<int8_t> pidx;
        rotateClipCW(pw, ph, false, false, ppx, pidx);
        assert(pw == 2 && ph == 3);
        // (x,y) -> (h-1-y, x): old (0,0)=White lands at (1,0); old (2,1)=Blue at (0,2).
        assert(ppx[0 * 2 + 1] == HgrColor::White);
        assert(ppx[2 * 2 + 0] == HgrColor::Blue);
        std::printf("  rotateClipCW keeps lo-res blocks and transposes pixels: OK\n");
    }
    std::printf("hgr_paint_fill_pattern OK\n");
    return 0;
}
