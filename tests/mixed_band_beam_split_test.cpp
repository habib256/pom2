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

// The mixed-mode text band when MIXED is cleared INSIDE it.
//
// fillCompositeSignal deliberately leaves scanlines [160,192) BLACK for
// every band that is mixed GRAPHICS: crisp mono text is composited over the
// demodulated picture afterwards by patchMixedTextBand. Which frames need
// that patch is therefore a property of the BANDS, not of the frame's final
// soft-switch state — and gating it on the final state lost the rows on
// every frame that left mixed mode inside or below the band:
//
//   HGR + MIXED from scanline 0, `$C052` thrown at scanline 170
//     beam truth : 160..169 = text rows 20-21, 170..191 = HGR
//     LUT / mono : exactly that
//     OE-CPU / AppleWin / OE-GPU : ten BLACK scanlines
//
// A French Touch-style raster split that ends in graphics is precisely this
// shape. What this pins:
//
//   1. rows 160..169 carry pixels in the CPU-demodulated pipelines, not
//      just in the LUT one (HGR, lo-res and DHGR flavours);
//   2. the OE-GPU path routes that frame to the framebuffer
//      (mixedCompositeUsesFramebuffer()), which is how the patched band
//      reaches the screen instead of the shader's black rows;
//   3. the controls still hold — MIXED cleared ABOVE the band leaves the
//      band pure graphics, and an unbroken mixed frame is unchanged.

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int kBandTop = 160;

void fill(Memory& mem)
{
    for (uint32_t a = 0x0400; a < 0x0C00; ++a)
        mem.memWrite(static_cast<uint16_t>(a),
                     static_cast<uint8_t>(0xC1 + (a % 0x1A)));   // 'A'.. text
    for (uint32_t a = 0x2000; a < 0x6000; ++a)
        mem.memWrite(static_cast<uint16_t>(a), 0x2A);            // HGR pattern
}

// HGR (or lo-res) + MIXED from scanline 0; `$C052` at `clearLine`, in HBL so
// it takes effect from byte column 0 of that line.
void build(Memory& mem, int clearLine, bool hiRes, bool dhgr80)
{
    if (dhgr80) mem.setIIEMode(true);
    fill(mem);
    if (dhgr80) {
        uint8_t* aux = mem.auxDataMutable();
        for (uint32_t a = 0x2000; a < 0x6000; ++a) aux[a] = 0x55;
        mem.memWrite(0xC00D, 0);          // 80COL on
    }
    mem.memRead(0xC050);                  // graphics
    mem.memRead(hiRes ? 0xC057 : 0xC056);
    mem.memRead(0xC053);                  // MIXED on
    mem.memRead(0xC054);                  // page 1
    if (dhgr80) mem.memRead(0xC05E);      // DHIRES on
    mem.setCycleCounter(0);
    mem.beginVideoEventFrame();
    mem.setCycleCounter(static_cast<uint64_t>(clearLine) * 65 + 5);
    mem.memRead(0xC052);                  // MIXED off, mid-band
}

int litOnRow(const uint32_t* px, int w, int y)
{
    int n = 0;
    for (int x = 0; x < w; ++x)
        if ((px[static_cast<size_t>(y) * w + x] & 0x00FFFFFFu) != 0) ++n;
    return n;
}

struct Frame {
    int w = 0;
    std::vector<uint32_t> px;
    bool usesFb = false;
};

Frame render(int clearLine, bool hiRes, bool dhgr80, Apple2Display::HiResMode hm)
{
    Memory mem;
    build(mem, clearLine, hiRes, dhgr80);
    Apple2Display d;
    d.setHiResMode(hm);
    d.setAuxMemory(mem.auxData());
    d.render(mem);
    Frame f;
    f.w = d.width();
    const uint32_t* p = d.pixels();
    f.px.assign(p, p + static_cast<size_t>(f.w) * Apple2Display::kHeight);
    f.usesFb = d.mixedCompositeUsesFramebuffer();
    return f;
}

// Every scanline the LUT pipeline lights must be lit in `hm` too.
void expectNoDroppedRows(const char* what, int clearLine, bool hiRes, bool dhgr80,
                         Apple2Display::HiResMode hm)
{
    const Frame lut  = render(clearLine, hiRes, dhgr80,
                              Apple2Display::HiResMode::ColorNTSC);
    const Frame comp = render(clearLine, hiRes, dhgr80, hm);
    for (int y = 0; y < Apple2Display::kHeight; ++y) {
        const int a = litOnRow(lut.px.data(), lut.w, y);
        const int b = litOnRow(comp.px.data(), comp.w, y);
        if (a > 0 && b == 0) {
            std::printf("FAIL %s: scanline %d lit in the LUT pipeline (%d px) "
                        "but BLACK in the composite one\n", what, y, a);
            assert(false && "composite pipeline dropped a scanline");
        }
    }
    std::printf("  ok  %s\n", what);
}

} // namespace

int main()
{
    std::printf("mixed-mode band, MIXED cleared inside it:\n");

    // 1. The band must survive in every CPU-side composite pipeline.
    for (int L : {161, 165, 170, 180, 190}) {
        char tag[96];
        std::snprintf(tag, sizeof tag, "HGR   $C052@L%-3d  OE-CPU  ", L);
        expectNoDroppedRows(tag, L, true, false,
                            Apple2Display::HiResMode::ColorCompositeOECpu);
        std::snprintf(tag, sizeof tag, "HGR   $C052@L%-3d  AppleWin", L);
        expectNoDroppedRows(tag, L, true, false,
                            Apple2Display::HiResMode::ColorAppleWin);
    }
    expectNoDroppedRows("LORES $C052@L170  OE-CPU  ", 170, false, false,
                        Apple2Display::HiResMode::ColorCompositeOECpu);
    expectNoDroppedRows("DHGR  $C052@L170  OE-CPU  ", 170, true, true,
                        Apple2Display::HiResMode::ColorCompositeOECpu);

    // The text rows really are text: row 20 (scanlines 160-167) of the
    // reference character data is not a solid HGR fill, so its lit count
    // differs from the graphics rows below the split.
    {
        const Frame f = render(170, true, false,
                               Apple2Display::HiResMode::ColorCompositeOECpu);
        const int inBand  = litOnRow(f.px.data(), f.w, 163);   // text row 20
        const int belowHi = litOnRow(f.px.data(), f.w, 175);   // HGR again
        assert(inBand > 0);
        assert(inBand != belowHi);
    }

    // 2. The OE-GPU path must present the framebuffer for such a frame —
    //    that is the flag MainWindow reads to bypass the shader, and the
    //    only way the patched band reaches the screen.
    {
        const Frame f = render(170, true, false,
                               Apple2Display::HiResMode::ColorCompositeOE);
        assert(f.usesFb && "OE-GPU frame with a blanked mixed band must use "
                           "the framebuffer");
        assert(litOnRow(f.px.data(), f.w, 163) > 0);
        std::printf("  ok  OE-GPU routes the split frame to the framebuffer\n");
    }

    // 3. Controls.
    //    (a) MIXED cleared ABOVE the band: rows 160..191 are pure graphics in
    //        every pipeline (no text patched over them).
    for (int L : {100, 150}) {
        const Frame lut = render(L, true, false,
                                 Apple2Display::HiResMode::ColorNTSC);
        const Frame oe  = render(L, true, false,
                                 Apple2Display::HiResMode::ColorCompositeOECpu);
        for (int y = kBandTop; y < Apple2Display::kHeight; ++y) {
            assert(litOnRow(lut.px.data(), lut.w, y) > 0);
            assert(litOnRow(oe.px.data(),  oe.w,  y) > 0);
        }
        // A uniform HGR fill lights the same number of dots on every row of
        // the band — i.e. nobody painted text there.
        const int r0 = litOnRow(oe.px.data(), oe.w, 163);
        for (int y = kBandTop; y < Apple2Display::kHeight; ++y)
            assert(litOnRow(oe.px.data(), oe.w, y) == r0);
        std::printf("  ok  control: $C052@L%d leaves the band graphics\n", L);
    }
    //    (b) an unbroken mixed frame still gets its text band.
    {
        Memory mem;
        fill(mem);
        mem.memRead(0xC050); mem.memRead(0xC057); mem.memRead(0xC053);
        mem.memRead(0xC054);
        Apple2Display d;
        d.setHiResMode(Apple2Display::HiResMode::ColorCompositeOECpu);
        d.setAuxMemory(mem.auxData());
        d.render(mem);
        const uint32_t* p = d.pixels();
        assert(litOnRow(p, d.width(), 163) > 0);
        std::printf("  ok  control: unbroken mixed frame keeps its text band\n");
    }

    std::printf("mixed_band_beam_split: all checks passed\n");
    return 0;
}
