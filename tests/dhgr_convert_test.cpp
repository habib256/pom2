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

// DHGR 560-dot converter smoke test — pins the three properties the importer's
// quality argument rests on:
//
//   1. DECODE PARITY: hgrpaint::dhgrDecodeScanlineRgb (the converter's internal
//      copy of the ColorNTSC DHGR pipeline — LUT row 0 + rotl4b(absX+1)) is
//      byte-identical to Apple2Display::renderDhgr on random plane pairs. This
//      is what makes the optimisation target THE SAME as the editor canvas.
//   2. SOLID FIELDS (dither off): a solid-colour source converts (560-dot
//      path) to a page whose interior renders EXACTLY that colour — the
//      search + refinement must find the trivial optimum. Only the outermost
//      columns may deviate (their sliding windows unavoidably mix with the
//      black off-screen context).
//      With dither ON at the library-default diffusion 1.0, that edge artifact
//      seeds a never-decaying compensation ripple (inherent to full-strength
//      error diffusion), so the dithered pin is TONE CONSERVATION instead:
//      the interior's mean linear RGB stays within 0.06 of the target.
//   3. REFINEMENT MONOTONICITY: the frozen-target cost is non-increasing
//      across the ICM passes (ImportStats::passCost), same contract as the
//      HGR converter.

#include "Apple2Display.h"
#include "Memory.h"
#include "hgrpaint/DhgrNtsc8Palette.h"
#include "hgrpaint/HgrConvert.h"
#include "hgrpaint/HgrPaintModel.h"
#include "hgrpaint/ImportCommon.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

using namespace hgrpaint;

namespace {

// The 16 lo-res colours as sRGB triples (mirror of Apple2Display::kLoResPalette
// components — the converter's own copy is cross-pinned to it in
// dhgr_paint_model, so one table here is enough).
constexpr uint8_t kPal[16][3] = {
    {0x00,0x00,0x00},{0xa7,0x0b,0x40},{0x40,0x1c,0xf7},{0xe6,0x28,0xff},
    {0x00,0x74,0x40},{0x80,0x80,0x80},{0x19,0x90,0xff},{0xbf,0x9c,0xff},
    {0x40,0x63,0x00},{0xe6,0x6f,0x00},{0x80,0x80,0x80},{0xff,0x8b,0xbf},
    {0x19,0xd7,0x00},{0xbf,0xe3,0x08},{0x58,0xf4,0xbf},{0xff,0xff,0xff},
};

void stageAndRender(const uint8_t* pair, Memory& mem, Apple2Display& disp)
{
    for (int i = 0; i < kHiresSize; ++i)
        mem.writeRamUnchecked(static_cast<uint16_t>(0x2000 + i), pair[kHiresSize + i]);
    std::memcpy(mem.auxDataMutable() + 0x2000, pair, kHiresSize);
    disp.render(mem);
    assert(disp.width() == Apple2Display::kWidth80);
}

} // namespace

int main()
{
    Memory mem;
    mem.setIIEMode(true);
    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());
    mem.memWrite(0xC050, 0); mem.memWrite(0xC052, 0); mem.memWrite(0xC054, 0);
    mem.memWrite(0xC057, 0); mem.memWrite(0xC00D, 0); mem.memWrite(0xC05E, 0);
    disp.setHiResMode(Apple2Display::HiResMode::ColorNTSC);

    // ── 1. Decode parity vs the real renderDhgr, random planes ──────────────
    {
        std::mt19937 rng(20260712);
        std::vector<uint8_t> pair(kDhgrPairSize);
        for (int trial = 0; trial < 20; ++trial) {
            for (auto& b : pair) b = static_cast<uint8_t>(rng());
            stageAndRender(pair.data(), mem, disp);
            const uint32_t* px = disp.pixels();
            for (int y = 0; y < 192; ++y) {
                const int rowBase = hgrRowAddress(y) - kHiresBase;
                uint32_t ours[560];
                dhgrDecodeScanlineRgb(pair.data() + rowBase,
                                      pair.data() + kHiresSize + rowBase, ours);
                for (int x = 0; x < 560; ++x)
                    assert((ours[x] & 0xFFFFFF) == (px[y * 560 + x] & 0xFFFFFF));
            }
        }
    }

    // ── 1b. HGR decode parity (LUT row 0 swap, 2026-07-12): the HGR importer's
    // internal decode is byte-identical to renderHiRes ColorNTSC too ──────────
    {
        mem.memWrite(0xC00C, 0);   // 80COL off
        mem.memWrite(0xC05F, 0);   // AN3/DHGR off → legacy 280-wide HGR path
        std::mt19937 rng(424242);
        for (int trial = 0; trial < 20; ++trial) {
            uint8_t row[40];
            for (auto& b : row) b = static_cast<uint8_t>(rng());
            for (int y = 0; y < 192; ++y) {
                const uint16_t base = hgrRowAddress(y);
                for (int c = 0; c < 40; ++c)
                    mem.writeRamUnchecked(static_cast<uint16_t>(base + c), row[c]);
            }
            disp.render(mem);
            assert(disp.width() == Apple2Display::kWidth);
            uint32_t ours[280];
            hgrDecodeScanlineRgb(row, ours);
            const uint32_t* px = disp.pixels() + 96 * 280;
            for (int x = 0; x < 280; ++x)
                assert((ours[x] & 0xFFFFFF) == (px[x] & 0xFFFFFF));
        }
        mem.memWrite(0xC00D, 0);   // back to DHGR switches
        mem.memWrite(0xC05E, 0);
    }

    // ── 2. Solid-colour sources convert to solid pages (560-dot path) ───────
    {
        for (int c = 0; c < 16; ++c) {
            std::vector<uint8_t> src(static_cast<size_t>(64) * 64 * 4);
            for (size_t i = 0; i < src.size(); i += 4) {
                src[i] = kPal[c][0]; src[i+1] = kPal[c][1];
                src[i+2] = kPal[c][2]; src[i+3] = 0xFF;
            }
            const uint32_t want =
                (uint32_t{kPal[c][2]} << 16) | (uint32_t{kPal[c][1]} << 8) | kPal[c][0];

            // 2a. Dither off → exact interior. [28,532) skips 4 byte-columns
            // per side (the off-screen black context seeds edge deviations).
            // Greys 5/10 share an RGB — compare RGB, not bit patterns.
            ImportOptions opt;
            opt.stretch = true;   // no letterbox: the whole page is the field
            opt.dither = false;
            std::vector<uint8_t> pair(kDhgrPairSize);
            imageToDhgrPage560(src.data(), 64, 64, opt, pair.data());
            stageAndRender(pair.data(), mem, disp);
            const uint32_t* px = disp.pixels();
            for (int y = 0; y < 192; ++y)
                for (int x = 28; x < 532; ++x)
                    assert((px[y * 560 + x] & 0xFFFFFF) == want);

            // 2b. Dither on (diffusion 1.0) → tone conservation: interior mean
            // linear RGB within 0.06 of the target (measured worst ≈ 0.042).
            opt.dither = true;
            imageToDhgrPage560(src.data(), 64, 64, opt, pair.data());
            stageAndRender(pair.data(), mem, disp);
            px = disp.pixels();
            double mr = 0, mg = 0, mb = 0;
            long n = 0;
            for (int y = 0; y < 192; ++y)
                for (int x = 28; x < 532; ++x) {
                    const uint32_t p = px[y * 560 + x];
                    mr += srgb8ToLinearF(static_cast<uint8_t>(p & 0xFF));
                    mg += srgb8ToLinearF(static_cast<uint8_t>((p >> 8) & 0xFF));
                    mb += srgb8ToLinearF(static_cast<uint8_t>((p >> 16) & 0xFF));
                    ++n;
                }
            assert(std::fabs(mr / n - srgb8ToLinearF(kPal[c][0])) < 0.06);
            assert(std::fabs(mg / n - srgb8ToLinearF(kPal[c][1])) < 0.06);
            assert(std::fabs(mb / n - srgb8ToLinearF(kPal[c][2])) < 0.06);
        }
    }

    // ── 3. Refinement monotonicity on a gradient (both kernels) ─────────────
    {
        const int SW = 128, SH = 96;
        std::vector<uint8_t> src(static_cast<size_t>(SW) * SH * 4);
        for (int y = 0; y < SH; ++y)
            for (int x = 0; x < SW; ++x) {
                uint8_t* p = &src[(static_cast<size_t>(y) * SW + x) * 4];
                p[0] = static_cast<uint8_t>(x * 255 / (SW - 1));
                p[1] = static_cast<uint8_t>(y * 255 / (SH - 1));
                p[2] = static_cast<uint8_t>(255 - x * 255 / (SW - 1));
                p[3] = 0xFF;
            }
        for (DitherKernel kern : {DitherKernel::FloydSteinberg, DitherKernel::JarvisMod}) {
            ImportOptions opt;
            opt.kernel = kern;
            opt.refinePasses = 2;
            ImportStats stats;
            std::vector<uint8_t> pair(kDhgrPairSize);
            imageToDhgrPage560(src.data(), SW, SH, opt, pair.data(), &stats);
            assert(stats.passCost.size() >= 2);
            for (size_t i = 1; i < stats.passCost.size(); ++i)
                assert(stats.passCost[i] <= stats.passCost[i - 1] + 1e-3f);
            // The refinement must actually do something on a hard image.
            assert(!stats.passChanged.empty() && stats.passChanged[0] > 0);
            // Diffusion clamping held (no runaway error worms).
            assert(stats.maxErrAbs <= 1.0f + 1e-4f);
        }
    }

    // ── 4. NTSC 8-px chroma model (ii-pix palette) sanity ────────────────────
    {
        // Table anchors: all-dots-off is black, all-on is (255,255,254) — the
        // ii-pix values; a corrupted regeneration of the embedded table would
        // trip these.
        for (int p = 0; p < 4; ++p) {
            assert(kDhgrNtsc8Srgb[p][0][0] == 0 && kDhgrNtsc8Srgb[p][0][1] == 0 &&
                   kDhgrNtsc8Srgb[p][0][2] == 0);
            assert(kDhgrNtsc8Srgb[p][255][0] >= 254 &&
                   kDhgrNtsc8Srgb[p][255][1] >= 254 &&
                   kDhgrNtsc8Srgb[p][255][2] >= 254);
        }
        // Extremes convert to the extreme bit patterns: a black source stays
        // all-zero, a white one saturates every dot.
        for (int white = 0; white <= 1; ++white) {
            std::vector<uint8_t> src(static_cast<size_t>(64) * 64 * 4,
                                     white ? 0xFF : 0x00);
            if (!white)
                for (size_t i = 3; i < src.size(); i += 4) src[i] = 0xFF;
            ImportOptions opt;
            opt.stretch = true;
            std::vector<uint8_t> pair(kDhgrPairSize);
            imageToDhgrPage560Ntsc(src.data(), 64, 64, opt, pair.data());
            long lit = 0;
            for (int y = 0; y < 192; ++y) {
                const int rowBase = hgrRowAddress(y) - kHiresBase;
                for (int i = 0; i < 40; ++i) {
                    lit += __builtin_popcount(pair[rowBase + i] & 0x7F);
                    lit += __builtin_popcount(pair[kHiresSize + rowBase + i] & 0x7F);
                }
            }
            if (white) assert(lit >= 560L * 192 * 95 / 100);   // ≈ all on
            else       assert(lit == 0);                       // exactly off
        }
    }

    // ── Import aspect (bug hunt #7) ──────────────────────────────────────────
    // A GR block is 7 canvas px wide and 4 tall, a 140-model DHGR pixel is 2
    // wide and 1 tall. The converters handed the resampler its 1.0 default,
    // which called both grids square: a 4:3 photo fit + letterboxed as if the
    // 40x48 grid were 40x48 canvas px and came out 2.33:1. An all-white 4:3
    // source must fill the full height and ~4/3 of it in width on both.
    {
        const int SW = 400, SH = 300;
        std::vector<uint8_t> src(static_cast<size_t>(SW) * SH * 4, 255);
        ImportOptions opt;
        opt.dither = false;

        std::vector<uint8_t> gr(0x400, 0);
        imageToGrPage(src.data(), SW, SH, opt, gr.data());
        int litRows = 0, litCols = 0;
        for (int r = 0; r < 24; ++r) {
            const int base = 0x80 * (r % 8) + 0x28 * (r / 8);
            int cols = 0;
            bool top = false, bot = false;
            for (int c = 0; c < 40; ++c) {
                const uint8_t v = gr[static_cast<size_t>(base + c)];
                if (v & 0x0F) { top = true; ++cols; }
                if (v & 0xF0) bot = true;
            }
            litRows += (top ? 1 : 0) + (bot ? 1 : 0);
            litCols = std::max(litCols, cols);
        }
        // 192 px tall x 4/3 = 256 px wide = 36.6 blocks of 7 px.
        if (litRows != 48 || litCols < 35 || litCols > 38) {
            std::fprintf(stderr, "FAIL: a 4:3 image imported to GR lit %d rows "
                                 "x %d cols (want 48 x ~37)\n", litRows, litCols);
            return 1;
        }

        std::vector<uint8_t> pair(kDhgrPairSize, 0);
        imageToDhgrPage(src.data(), SW, SH, opt, pair.data());
        int rows = 0, cols = 0;
        for (int y = 0; y < 192; ++y) {
            const int rowBase = hgrRowAddress(y) - kHiresBase;
            uint32_t px[560];
            dhgrDecodeScanlineRgb(pair.data() + rowBase,
                                  pair.data() + kHiresSize + rowBase, px);
            int lit = 0;
            for (int x = 0; x < 560; ++x) if (px[x] & 0xFFFFFF) ++lit;
            if (lit) ++rows;
            cols = std::max(cols, lit);
        }
        // 192 px tall x 4/3 = 256 HGR px = 128 of 140 DHGR pixels = 512 dots.
        if (rows != 192 || cols < 496 || cols > 528) {
            std::fprintf(stderr, "FAIL: a 4:3 image imported to DHGR lit %d "
                                 "rows x %d dots (want 192 x ~512)\n", rows, cols);
            return 1;
        }
        std::printf("  import keeps a 4:3 source at 4:3 on GR and DHGR: OK\n");
    }

    std::printf("dhgr_convert: OK\n");
    return 0;
}
