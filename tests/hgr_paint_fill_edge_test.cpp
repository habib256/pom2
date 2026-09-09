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

// HGR paint fill — canvas-edge columns (bug hunt #17).
//
// hgrpaint::fillRegion floods by PERCEIVED colour: it renders the page through
// the host's real NTSC pipeline and grows the region over equal RGB, because a
// chromatic HGR field is bit-dithered and a raw-bit flood leaks through the
// off sub-pixels. The renderer's sliding window, however, has no context at the
// ends of a line — x = 0/1 decode with a zero left word, x = 279 with a zero
// right word — so those columns come out a different colour from the field they
// belong to (measured at row 96, ColorNTSC: a solid Violet field is $FF28E6 but
// x=279 is $000000; a solid Green field is $00D719 but x=0 is $000000; Orange
// misses x=0, x=1 AND x=279). The flood then stopped there and left the OLD
// picture behind: a solid Violet page refilled Green kept 192 dark pixels in
// column 279 (26688 green dots instead of 26880).
//
// This test drives fillRegion through the REAL Apple2Display painter (the same
// pipeline Pom2HgrPaintHost::renderHgrPage hands it) and pins:
//   1. refilling a solid field of any chromatic colour with any other produces
//      exactly the same 7680 visible page bytes as painting that colour over a
//      blank page — no edge column left behind;
//   2. the edge rule does not LEAK: a white border drawn ON column 279, and a
//      lone lit dot at column 279, both survive a fill of the region next to
//      them (the dither-continuation test they fail is what excludes them).

#include "Apple2Display.h"
#include "Memory.h"
#include "hgrpaint/HgrPaintModel.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace hgrpaint;

namespace {

Memory* g_mem = nullptr;
Apple2Display* g_disp = nullptr;

// The RenderPageFn the editor supplies: an 8 KB page → 280×192 RGBA through the
// real NTSC pipeline.
void renderPage(const uint8_t* page, uint32_t* out)
{
    for (int i = 0; i < kHiresSize; ++i)
        g_mem->writeRamUnchecked(static_cast<uint16_t>(0x2000 + i), page[i]);
    g_disp->setHiResMode(Apple2Display::HiResMode::ColorNTSC);
    g_disp->render(*g_mem);
    std::memcpy(out, g_disp->pixels(), static_cast<size_t>(280) * 192 * 4);
}

// The 7680 bytes the display actually reads (40 per row × 192 rows); the rest of
// the 8 KB page is the HIRES interleave's holes.
int visibleByteDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    int n = 0;
    for (int y = 0; y < 192; ++y) {
        const int base = hgrByteOffset(0, y);
        for (int c = 0; c < 40; ++c)
            if (a[base + c] != b[base + c]) ++n;
    }
    return n;
}

const char* name(HgrColor c)
{
    switch (c) {
    case HgrColor::Black:  return "Black";
    case HgrColor::White:  return "White";
    case HgrColor::Violet: return "Violet";
    case HgrColor::Green:  return "Green";
    case HgrColor::Blue:   return "Blue";
    default:               return "Orange";
    }
}

} // namespace

int main()
{
    Memory mem;
    mem.setIIEMode(true);
    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());
    // Plain (single) HIRES, full screen, page 1.
    mem.memWrite(0xC050, 0); mem.memWrite(0xC052, 0); mem.memWrite(0xC054, 0);
    mem.memWrite(0xC057, 0); mem.memWrite(0xC00C, 0);
    g_mem = &mem;
    g_disp = &disp;

    // ── 1. Refill a solid field: every chromatic pair must land on the same
    //      bytes a direct paint would. The four chromatic colours only; White
    //      deliberately leaves the byte's palette bit alone (plotPage), so a
    //      Blue→White refill legitimately ends on $FF where a direct White
    //      paint of a blank page ends on $7F — same picture (only dot 0 differs,
    //      by the half-dot shift), different bytes.
    {
        static const HgrColor kChroma[] = {HgrColor::Violet, HgrColor::Green,
                                           HgrColor::Blue,   HgrColor::Orange};
        int worst = 0;
        for (HgrColor a : kChroma)
            for (HgrColor b : kChroma) {
                std::vector<uint8_t> page(kHiresSize, 0);
                fillRegion(page.data(), 140, 96, a, renderPage);
                fillRegion(page.data(), 140, 96, b, renderPage);

                std::vector<uint8_t> direct(kHiresSize, 0);
                for (int y = 0; y < 192; ++y)
                    for (int x = 0; x < 280; ++x)
                        plotPage(direct.data(), x, y, b);

                const int d = visibleByteDiff(page, direct);
                if (d != 0)
                    std::printf("FAIL %s -> %s: %d of 7680 visible bytes differ "
                                "from a direct fill\n", name(a), name(b), d);
                worst = (d > worst) ? d : worst;
            }
        assert(worst == 0);
    }

    // ── 2. The edge rule must not leak into a feature drawn ON the edge ──────
    {
        // White box whose RIGHT border is column 279; fill the black interior.
        std::vector<uint8_t> page(kHiresSize, 0);
        for (int y = 40; y <= 150; ++y) {
            plotPage(page.data(), 279, y, HgrColor::White);
            plotPage(page.data(), 200, y, HgrColor::White);
        }
        for (int x = 200; x <= 279; ++x) {
            plotPage(page.data(), x, 40,  HgrColor::White);
            plotPage(page.data(), x, 150, HgrColor::White);
        }
        fillRegion(page.data(), 240, 96, HgrColor::Violet, renderPage);
        for (int y = 41; y < 150; ++y)
            assert(pixelOn(page.data(), 279, y));   // border survived
    }
    {
        // A lone lit dot at column 279 on an otherwise black page must survive a
        // fill of the black region around it.
        std::vector<uint8_t> page(kHiresSize, 0);
        plotPage(page.data(), 279, 96, HgrColor::White);
        fillRegion(page.data(), 0, 0, HgrColor::Green, renderPage);
        assert(pixelOn(page.data(), 279, 96));
    }

    // ── 3. A bounded fill still neither leaks nor leaves holes ───────────────
    {
        std::vector<uint8_t> page(kHiresSize, 0);
        for (int x = 40; x <= 240; ++x) {
            plotPage(page.data(), x, 40,  HgrColor::White);
            plotPage(page.data(), x, 150, HgrColor::White);
        }
        for (int y = 40; y <= 150; ++y) {
            plotPage(page.data(), 40,  y, HgrColor::White);
            plotPage(page.data(), 240, y, HgrColor::White);
        }
        std::vector<uint8_t> before = page;
        const int region = fillRegion(page.data(), 140, 96, HgrColor::Violet,
                                      renderPage);
        assert(region == (240 - 40 - 1) * (150 - 40 - 1));
        for (int y = 0; y < 192; ++y)
            for (int x = 0; x < 280; ++x) {
                const int off = hgrByteOffset(x, y);
                const bool changed = ((page[off] ^ before[off]) >> hgrBit(x)) & 1;
                if (changed)
                    assert(x > 40 && x < 240 && y > 40 && y < 150);
            }
    }

    std::printf("hgr_paint_fill_edge: OK\n");
    return 0;
}
