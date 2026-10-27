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

// Horizontal (mid-scanline, per-byte-column) beam-racing for the 560-wide IIe
// modes. horizontal_split_smoke pins the legacy 280-wide path; this pins the
// 560-dot path in BOTH outputs:
//   1. the RGBA frame80 (renderInternalSegment's save/restore window), and
//   2. the composite signal (paintText80/paintDhgr now column-bounded).
// A IIe frame whose lower band re-flips $C050/$C051 every scanline — graphics
// (DHGR) from byte column 0, 80-column TEXT from byte column 20 — must paint,
// ON THE SAME LINE, the DHGR image/waveform in the left 280 dots and the
// 80-col text in the right 280 dots of the 560-wide output.
//
// Plan: TODO.md [Display] "Split horizontal mid-scanline", 560-wide increment.

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr uint16_t SET_TEXT     = 0xC051;
constexpr uint16_t CLR_TEXT     = 0xC050;
constexpr uint16_t SET_HIRES    = 0xC057;
constexpr uint16_t IIE_80COL_ON = 0xC00D;
constexpr uint16_t DHIRES_ON    = 0xC05E;

constexpr int W         = 560;
constexpr int kSplitDot = 21 * 14;     // 294 — hpos 45 → col 21 (`hpos - 24`)
constexpr int kBandTop  = 96;          // row-aligned (12 * 8)

// Fill main + aux text page ($0400-$07FF) and HGR page ($2000-$3FFF) with
// deterministic per-address data; filling every byte makes the exact text/HGR
// interleave the renderer uses irrelevant.
void populate(Memory& mem)
{
    uint8_t* aux = mem.auxDataMutable();
    for (uint32_t a = 0x0400; a < 0x0800; ++a) {
        mem.memWrite(static_cast<uint16_t>(a), static_cast<uint8_t>(0xA0 + (a & 0x3F)));
        aux[a] = static_cast<uint8_t>(0xA0 + ((a * 3) & 0x3F));
    }
    for (uint32_t a = 0x2000; a < 0x4000; ++a) {
        mem.memWrite(static_cast<uint16_t>(a), static_cast<uint8_t>(a & 0x7F));
        aux[a] = static_cast<uint8_t>((a * 5) & 0x7F);
    }
}

// IIe + 80COL + DHIRES + HIRES latched; caller picks graphics/text via $C050/1.
void setup560(Memory& mem)
{
    mem.setIIEMode(true);
    populate(mem);
    mem.memWrite(IIE_80COL_ON, 0);
    mem.memRead(DHIRES_ON);
    mem.memRead(SET_HIRES);
}

// Fresh full-screen DHGR / full-screen 80-col text / beam-raced (DHGR top,
// per-scanline DHGR|text strip in the lower band) Memory objects. Rebuilt per
// section because rendering consumes the video-event log.
void makeDhgr(Memory& m) { setup560(m); m.memRead(CLR_TEXT); }
void makeText(Memory& m) { setup560(m); m.memRead(SET_TEXT); }
void makeBeam(Memory& m)
{
    setup560(m);
    m.memRead(CLR_TEXT);                  // frame-start = DHGR
    m.setCycleCounter(0);
    m.beginVideoEventFrame();
    for (int y = kBandTop; y < 192; ++y) {
        m.setCycleCounter(static_cast<uint64_t>(y) * 65 + 5);   // HBL → col 0
        m.memRead(CLR_TEXT);              // DHGR from column 0
        m.setCycleCounter(static_cast<uint64_t>(y) * 65 + 45);  // hpos 45 → col 21 (mapping is `hpos - 24`)
        m.memRead(SET_TEXT);              // 80-col text from column 20
    }
}

template <typename T>
bool spanEqual(const std::vector<T>& a, const std::vector<T>& b, int y, int x0, int x1)
{
    return std::memcmp(a.data() + static_cast<size_t>(y) * W + x0,
                       b.data() + static_cast<size_t>(y) * W + x0,
                       static_cast<size_t>(x1 - x0) * sizeof(T)) == 0;
}

// RGBA frame80 (560-wide) in ColorNTSC — the LUT render path, consistent for
// both graphics and text end-states (ColorCompositeOECpu would demodulate
// graphics frames into frame80 instead, a different buffer).
std::vector<uint32_t> frameOf(Memory& mem)
{
    Apple2Display d;
    d.setAuxMemory(mem.auxDataMutable());
    d.render(mem);
    assert(d.width() == W);
    const uint32_t* p = d.pixels();
    return std::vector<uint32_t>(p, p + static_cast<size_t>(W) * d.height());
}

// Composite signal (560-wide) in ColorCompositeOECpu.
std::vector<uint8_t> signalOf(Memory& mem)
{
    Apple2Display d;
    d.setAuxMemory(mem.auxDataMutable());
    d.setHiResMode(Apple2Display::HiResMode::ColorCompositeOECpu);
    d.render(mem);
    assert(d.signalProduced());
    const uint8_t* s = d.signal();
    return std::vector<uint8_t>(s, s + static_cast<size_t>(d.signalWidth()) * d.signalHeight());
}

template <typename T>
void checkSplit(const std::vector<T>& dhgr, const std::vector<T>& text,
                const std::vector<T>& beam, const char* what)
{
    // Sanity: the two images/waveforms differ in both windows.
    for (int y : {100, 150}) {
        assert(!spanEqual(dhgr, text, y, 0, kSplitDot));
        assert(!spanEqual(dhgr, text, y, kSplitDot, W));
    }
    // Top band: full-width DHGR.
    for (int y : {8, 40, 88})
        assert(spanEqual(beam, dhgr, y, 0, W) && what);
    // Split band: left DHGR, right 80-col text — same line.
    for (int y : {kBandTop, 104, 150, 191}) {
        assert(spanEqual(beam, dhgr, y, 0, kSplitDot) && what);
        assert(spanEqual(beam, text, y, kSplitDot, W) && what);
        assert(!spanEqual(beam, text, y, 0, kSplitDot) && what);
        assert(!spanEqual(beam, dhgr, y, kSplitDot, W) && what);
    }
}

} // namespace

int main()
{
    // ── 1. RGBA frame80 (ColorNTSC LUT path). ────────────────────────────
    {
        Memory dh, tx, bm;
        makeDhgr(dh); makeText(tx); makeBeam(bm);
        checkSplit(frameOf(dh), frameOf(tx), frameOf(bm),
                   "frame80 560-wide split (DHGR left / 80-col text right)");
    }

    // ── 2. Composite signal (ColorCompositeOECpu). ───────────────────────
    {
        Memory dh, tx, bm;
        makeDhgr(dh); makeText(tx); makeBeam(bm);
        checkSplit(signalOf(dh), signalOf(tx), signalOf(bm),
                   "composite signal 560-wide split (DHGR left / 80-col text right)");
    }

    // ── 3. DISPLAY-005: 40-column text left, 80-column text right on the
    //       SAME line. The frame lives in frame80; the 40-column cells were
    //       painted into the 280-wide buffer and never doubled across, so
    //       they showed the previous frame (black on a fresh display).
    {
        auto any = [](Memory& mem, int& w) {
            Apple2Display d;
            d.setAuxMemory(mem.auxDataMutable());
            d.render(mem);
            w = d.width();
            const uint32_t* p = d.pixels();
            return std::vector<uint32_t>(p, p + static_cast<size_t>(w) * d.height());
        };
        auto base = [](Memory& m) {
            m.setIIEMode(true); populate(m); m.memRead(SET_TEXT);
            m.memWrite(0xC00C, 0);                       // 40 columns
        };
        Memory t40, t80, mix;
        base(t40);
        base(t80); t80.memWrite(IIE_80COL_ON, 0);
        base(mix);
        mix.setCycleCounter(0);
        mix.beginVideoEventFrame();
        for (int y = kBandTop; y < 192; ++y) {
            mix.setCycleCounter(static_cast<uint64_t>(y) * 65 + 5);
            mix.memWrite(0xC00C, 0);                     // 40 columns from column 0
            mix.setCycleCounter(static_cast<uint64_t>(y) * 65 + 45);
            mix.memWrite(IIE_80COL_ON, 0);               // 80 columns from column 21
        }
        int w40 = 0, w80 = 0, wm = 0;
        const auto a40 = any(t40, w40), a80 = any(t80, w80), am = any(mix, wm);
        assert(w40 == 280 && w80 == W && wm == W);
        for (int y = kBandTop; y < 192; ++y) {
            for (int x = 0; x < kSplitDot; ++x)
                if (am[static_cast<size_t>(y) * W + x] != a40[static_cast<size_t>(y) * 280 + x / 2]) {
                    std::fprintf(stderr, "40/80 mix: 40-col cell wrong at y=%d x=%d\n", y, x);
                    std::abort();
                }
            assert(spanEqual(am, a80, y, kSplitDot, W));
        }
        std::printf("  ok: 40- and 80-column cells share a line\n");
    }

    std::printf("horizontal_split_560 OK\n");
    return 0;
}
