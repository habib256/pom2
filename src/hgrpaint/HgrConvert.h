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
// Image → Apple II HGR conversion, ii-pix style — the high-quality importer for
// the HGR Paint editor. Part of the portable hgrpaint/ toolkit (no emulator
// dependency): it decodes PNG/JPG (via stb_image, in a separate TU), resamples
// to 280×192 in LINEAR light (gamma-correct averaging), then dithers to the
// 8 KB HGR page by **analysis-by-synthesis**: for every byte it searches all
// 256 (7 pixels + palette) patterns (branch-and-bound over the pattern bits,
// warm-started from the previous row and pinned bit-identical to the exhaustive
// scan), renders each through this module's own copy of the Apple II NTSC
// artifact pipeline (the same maths as GraphicsCard — pinned equal in the
// test), measures the result against the target in **CAM16-UCS** perceptual
// space with an in-candidate sequential error walk (ii-pix apply_one_line), and
// keeps the closest. Error diffusion runs in clamped linear RGB (selectable
// Floyd-Steinberg / jarvis-mod kernel), and 1-2 ICM refinement passes re-choose
// every byte against its ACTUAL committed neighbour words, monotonically
// lowering the perceptual cost (fixes colour fringes at byte seams).
//
// This beats Buckshot/bmp2dhr because it dithers against the *true* NTSC colours
// (including the sliding-window pixel coupling) rather than a fixed palette.

#ifndef HGRPAINT_CONVERT_H
#define HGRPAINT_CONVERT_H

#include <cstdint>
#include <string>
#include <vector>

#include "ImportCommon.h"   // DitherKernel + shared linear-RGB resampler

namespace hgrpaint {

struct ImportOptions {
    bool  stretch = false;     // false = fit + letterbox (keep aspect), true = stretch
    bool  dither  = true;      // error diffusion (kernel selectable below)
    float diffusion = 1.0f;    // fraction of error propagated (1 = full grain, <1 = smoother)
    float brightness = 1.0f;   // multiply the source (1 = none)
    float contrast   = 1.0f;   // contrast around mid-grey (1 = none)
    float gamma      = 1.0f;   // mid-tone gamma applied before conversion (>1 brightens)
    // Perceptual chroma penalty ("colour noise" knob): low → vivid/noisy colour,
    // high → flat greys dither clean black/white instead of magenta confetti.
    float chromaWeight = 2.4f;
    // Error-diffusion kernel. The editor defaults HGR to JarvisMod (ii-pix's HGR
    // choice: its 4-pixel forward reach matches the NTSC sliding window); the
    // library default stays Floyd-Steinberg so existing callers/tests keep their
    // pinned behaviour.
    DitherKernel kernel = DitherKernel::FloydSteinberg;
    // ICM-style cross-byte refinement passes after the dither pass: each byte is
    // re-chosen against the ACTUAL committed neighbour words (instead of pass 1's
    // guessed right-hand context), accepting only moves that strictly lower the
    // frozen-target perceptual cost — monotone, fixes colour fringes at byte seams.
    int refinePasses = 2;
    // Test/debug: disable the early-abort + warm-start candidate search. The fast
    // search is pinned bit-identical to this exhaustive one in hgr_convert_smoke.
    bool exhaustiveSearch = false;
    // Optional source crop rectangle in source pixels, [x0,x1) × [y0,y1): only this
    // sub-region is resampled into the HGR page (its aspect drives fit/letterbox).
    // Degenerate (x1<=x0 or y1<=y0) → use the whole image.
    int cropX0 = 0, cropY0 = 0, cropX1 = 0, cropY1 = 0;
};

// Optional conversion telemetry (pinned by the smoke test).
struct ImportStats {
    // Frozen-target perceptual objective after the dither pass ([0]) and after
    // each refinement pass — non-increasing by construction.
    std::vector<float> passCost;
    std::vector<int>   passChanged;   // bytes changed per refinement pass
    float maxErrAbs = 0.0f;           // max |linear-RGB diffusion error| seen (≤1: clamped)
};

// Decode one HGR scanline (40 bytes) to 280 RGBA pixels through this module's
// NTSC pipeline. Exposed mainly so the test can pin it equal to GraphicsCard.
void hgrDecodeScanlineRgb(const uint8_t bytes[40], uint32_t out[280]);

// Convert a decoded RGBA image (srcW×srcH, 4 bytes/pixel, top-down) into an 8 KB
// HGR page (page-relative, $2000 layout). `outPage` must hold kHiresSize bytes.
// `stats` (optional) receives refinement/diffusion telemetry.
void imageToHgrPage(const uint8_t* rgba, int srcW, int srcH,
                    const ImportOptions& opt, uint8_t* outPage,
                    ImportStats* stats = nullptr);

// Convert a decoded RGBA image into a 1 KB Apple II lo-res (GR) text page. GR has
// no NTSC coupling: each of kGrCols×kGrRows blocks is one flat palette index, so
// this quantises each block to the nearest of the 16 colours in CAM16-UCS with
// linear-RGB error diffusion (kernel/dither/chroma-weight knobs shared with the
// HGR path). `outPage` must hold at least 0x400 bytes (text-page $0400 layout).
void imageToGrPage(const uint8_t* rgba, int srcW, int srcH,
                   const ImportOptions& opt, uint8_t* outPage);

// Convert a decoded RGBA image into a 16 KB Apple IIe DHGR pair (aux 8 KB then
// main 8 KB, page-relative — the HgrPaintModel/A2FC layout). The aligned block
// model is a flat 140×192 grid of 16-colour pixels (same palette as GR), so
// this is the GR quantiser at DHGR resolution: CAM16-UCS nearest-colour with
// linear-RGB error diffusion, packed via plotDhgrPixel. `outPair` must hold
// kDhgrPairSize bytes. This is the fast "Dazzle Draw"-style block mode; the
// 560-dot converter below is the high-quality default.
void imageToDhgrPage(const uint8_t* rgba, int srcW, int srcH,
                     const ImportOptions& opt, uint8_t* outPair);

// High-quality DHGR conversion at the TRUE 560-dot resolution (ii-pix's "4-pixel
// colour" model, which dropped 140px conversion as fundamentally wrong for
// DHGR). Every dot is chosen individually by per-byte-column analysis-by-
// synthesis: 128 candidate bit patterns per 7-dot column are rendered through
// this module's copy of POM2's exact ColorNTSC DHGR decode (the MAME
// sliding-window LUT with the absX+1 phase rotation — pinned equal to
// Apple2Display::renderDhgr in the dhgr_convert test, so the optimisation
// target IS what the canvas shows), scored in CAM16-UCS with the in-candidate
// linear-RGB error walk, then 1-2 ICM refinement passes re-choose each column
// against its ACTUAL committed neighbours (monotone on the frozen-target cost).
// This both avoids and exploits the colour fringing the 140px block model
// suffers from. `outPair` must hold kDhgrPairSize bytes; `stats` (optional)
// receives the same telemetry as imageToHgrPage.
void imageToDhgrPage560(const uint8_t* rgba, int srcW, int srcH,
                        const ImportOptions& opt, uint8_t* outPair,
                        ImportStats* stats = nullptr);

// Convert a decoded RGBA image into a 2 KB Apple IIe DLGR pair (aux 1 KB then
// main 1 KB — the HgrPaintModel layout). The GR quantiser at 80×48 double
// lo-res resolution; plotDlgrBlock handles the aux nibble rotation.
void imageToDlgrPage(const uint8_t* rgba, int srcW, int srcH,
                     const ImportOptions& opt, uint8_t* outPair);

// DHGR conversion against the NTSC 8-px chroma model (ii-pix --palette=ntsc):
// the colour of every dot is looked up from the trailing 8-dot pattern × NTSC
// phase (86 unique colours — DhgrNtsc8Palette, verbatim ii-pix data). The
// model is CAUSAL (no right context), so the per-column analysis-by-synthesis
// needs no guessed context and no refinement pass. The extra colours only
// materialise on composite viewing targets (POM2's OE / AppleWin modes, real
// hardware); on the MAME-LUT canvas the picture reads as heavy dithering.
void imageToDhgrPage560Ntsc(const uint8_t* rgba, int srcW, int srcH,
                            const ImportOptions& opt, uint8_t* outPair);

// Monochrome DHGR conversion (ii-pix dhr_mono): every one of the 560×192 dots
// is a free 1-bit pixel, dithered against linear LUMA with the shared error-
// diffusion kernels. Meant for viewing on the mono canvas / a mono monitor —
// the colour pipelines will show artifact colours over the dither patterns.
void imageToDhgrMonoPage(const uint8_t* rgba, int srcW, int srcH,
                         const ImportOptions& opt, uint8_t* outPair);

// Decode one DHGR scanline (40 aux + 40 main bytes) to 560 RGBA dots through
// this module's copy of the ColorNTSC DHGR pipeline. Exposed so the test can
// pin it equal to Apple2Display::renderDhgr.
void dhgrDecodeScanlineRgb(const uint8_t auxBytes[40], const uint8_t mainBytes[40],
                           uint32_t out[560]);

// Decode a PNG/JPG/BMP file to RGBA (4 bytes/pixel). Implemented in
// HgrImageDecode.cpp (uses stb_image; the impl is linked from the host app), so
// the pure converter above stays unit-testable without an image decoder.
// Returns false + sets `err` on failure.
bool decodeImageFile(const std::string& path, int& w, int& h,
                     std::vector<uint8_t>& rgba, std::string& err);

} // namespace hgrpaint

#endif // HGRPAINT_CONVERT_H
