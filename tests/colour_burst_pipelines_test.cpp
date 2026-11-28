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

// Sather's colour-burst mechanism through every colour pipeline
// (Apple2Display_Burst.cpp): TEXT active during horizontal blanking removes
// that line's colour burst (CLRGATE', ColourBurst.h); NTSC MIXED text keeps
// its chroma; PAL GR+2 cuts it. The stimulus is a 65-cycle 6502 loop of this
// test's own that throws TEXT on and off inside HBL on every line, so the
// burst window sees TEXT for a chosen number of states.
//   1. PAL, two burst states a line: the decoder's identification reads no
//      R-Y swing and its killer acts (LUT/AppleWin: the 1-bit dot stream;
//      OE: luminance only).
//   2. PAL, one burst state: the identification sets the decoder's switch
//      out of step, R-Y comes out negated.
//   3. The OE GPU mode hands such frames to its CPU twin, pixel-identical.
//   4. Monochrome is untouched (no burst on that wire).
//   5. NTSC, no burst on the visible lines: the AppleColor killer keeps
//      colour for a while, then drops it.
//   6. NTSC MIXED with "Sharp text" off: the four text rows keep chroma
//      (Sather IIe p. 8-16); PAL's do not (the LS175 latch).

#include "Apple2Display.h"
#include "M6502.h"
#include "Memory.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

using Mode = Apple2Display::HiResMode;

struct Rig {
    std::unique_ptr<Memory> mem = std::make_unique<Memory>();
    std::unique_ptr<M6502>  cpu;
    Rig() { cpu = std::make_unique<M6502>(mem.get()); mem->setCpu(cpu.get()); }
};

constexpr uint16_t kProg = 0x0300;

// TEXT on at hpos offHpos-4, off at offHpos, on every line from 20 lines
// before the picture to its end. `textInHbl` false: the same loop throws
// $C050 twice (no TEXT at all) — the control frame, same timing, same RAM.
void setupFrame(Rig& r, VideoStandard vstd, int offHpos, bool textInHbl, bool mixed)
{
    Memory& m = *r.mem;
    m.setIIEMode(true);
    m.setVideoStandard(vstd);
    for (int a = 0x2000; a < 0x4000; ++a)
        m.memWrite(static_cast<uint16_t>(a), ((a & 0x7F) < 20) ? ((a & 1) ? 0xAA : 0xD5)
                                                                : ((a & 1) ? 0xD5 : 0xAA));
    for (int a = 0x0400; a < 0x0800; ++a) m.memWrite(static_cast<uint16_t>(a), 0xC1);   // 'A'
    m.memRead(0xC050);
    m.memRead(0xC057);
    m.memRead(0xC054);
    m.memRead(mixed ? 0xC053 : 0xC052);

    const int lines = pom2VideoTiming(vstd).scanlinesPerFrame;
    const uint64_t frame = 65ull * static_cast<uint64_t>(lines);
    constexpr int kLead = 20;   // start in the blanking lines before line 0
    std::vector<uint8_t> p = {0xA2, static_cast<uint8_t>(kLead + 192)};   // LDX #n
    const uint16_t loop = static_cast<uint16_t>(kProg + p.size());
    const uint8_t on = textInHbl ? 0x51 : 0x50;
    p.insert(p.end(), {0x2C, on, 0xC0, 0x2C, 0x50, 0xC0});               // BIT $C05x x2
    for (int i = 0; i < 26; ++i) p.push_back(0xEA);                      // NOP x26
    p.push_back(0xCA);                                                    // DEX
    p.push_back(0xD0);                                                    // BNE loop
    p.push_back(static_cast<uint8_t>(loop - (kProg + p.size() + 1)));
    const uint16_t spin = static_cast<uint16_t>(kProg + p.size());
    p.insert(p.end(), {0x4C, static_cast<uint8_t>(spin & 0xFF), static_cast<uint8_t>(spin >> 8)});
    for (size_t i = 0; i < p.size(); ++i) m.memWrite(static_cast<uint16_t>(kProg + i), p[i]);

    // BIT $C050 (the second access) lands on hpos offHpos: its instruction
    // starts 7 cycles earlier, the LDX 2 before that.
    const uint64_t start = frame - static_cast<uint64_t>(kLead) * 65 +
                           static_cast<uint64_t>(offHpos - 7 - 2);
    m.setCycleCounter(start);
    m.beginVideoEventFrame();
    r.cpu->setProgramCounter(kProg);
    while (m.getCycleCounter() < frame + 192ull * 65 + 40) r.cpu->step();
}

struct Shot {
    std::vector<uint32_t> px;
    int w = 0;
    std::array<uint8_t, 192> chroma{};
    bool routed = false;
};

Shot shoot(VideoStandard vstd, Mode mode, int offHpos, bool textInHbl, bool mixed = false,
           bool textSharp = true)
{
    Rig r;
    setupFrame(r, vstd, offHpos, textInHbl, mixed);
    Apple2Display d;
    d.setHiResMode(mode);
    d.setAuxMemory(r.mem->auxData());
    Apple2Display::OeDemodParams p;
    p.textSharp = textSharp;
    d.setOeDemodParams(p);
    d.render(*r.mem);
    Shot s;
    s.routed = d.mixedCompositeUsesFramebuffer();
    const uint32_t* px = d.pixels();
    s.w = d.width();
    s.px.assign(px, px + static_cast<size_t>(s.w) * 192);
    for (int y = 0; y < 192; ++y) s.chroma[static_cast<size_t>(y)] = d.lineChroma(y);
    return s;
}

void yuv(uint32_t px, float& y, float& u, float& v)
{
    const float r = float(px & 0xFF), g = float((px >> 8) & 0xFF), b = float((px >> 16) & 0xFF);
    y = 0.299f * r + 0.587f * g + 0.114f * b;
    u = 0.492f * (b - y);
    v = 0.877f * (r - y);
}

int spread(uint32_t px)
{
    const int r = px & 0xFF, g = (px >> 8) & 0xFF, b = (px >> 16) & 0xFF;
    return std::max({r, g, b}) - std::min({r, g, b});
}

int maxSpread(const Shot& s, int y0, int y1)
{
    int m = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < s.w; ++x) m = std::max(m, spread(s.px[size_t(y) * s.w + x]));
    return m;
}

// Every strongly coloured pixel of `ctl`'s row has the opposite V in `inv`.
void assertVInverted(const Shot& ctl, const Shot& inv, int y, const char* what)
{
    // A frame the burst touched is in the 560 domain; the control may be 280.
    int checked = 0;
    for (int x = 0; x < inv.w; ++x) {
        float cy, cu, cv, iy, iu, iv;
        yuv(ctl.px[size_t(y) * ctl.w + size_t(x) * ctl.w / inv.w], cy, cu, cv);
        yuv(inv.px[size_t(y) * inv.w + x], iy, iu, iv);
        if (std::fabs(cv) < 30.0f) continue;
        assert(cv * iv < 0.0f && "R-Y inverted");
        ++checked;
    }
    assert(checked > 0);
    std::printf("  ok: %s — V inverted on %d pixels of line %d\n", what, checked, y);
}

void testPal()
{
    const auto pal = VideoStandard::PAL;
    for (Mode mode : {Mode::ColorNTSC, Mode::ColorCompositeOECpu, Mode::ColorAppleWin}) {
        const char* name = Apple2Display::hiResModeName(mode);
        const Shot ctl  = shoot(pal, mode, 14, false);
        const Shot one  = shoot(pal, mode, 14, true);
        const Shot none = shoot(pal, mode, 15, true);
        for (int y = 0; y < 192; ++y) assert(ctl.chroma[size_t(y)] == 0);
        assert(maxSpread(ctl, 40, 41) > 60 && "control frame is in colour");
        for (int y = 16; y < 192; ++y) {
            assert(one.chroma[size_t(y)] == 1 && "two burst states: killed");
            assert(none.chroma[size_t(y)] == 2 && "one burst state: V inverted");
        }
        assert(maxSpread(one, 16, 192) <= 2 && "killed lines carry no chroma");
        std::printf("  ok: PAL %s — two burst states, no colour\n", name);
        assertVInverted(ctl, none, 100, name);
    }

    // OE GPU: the shader cannot vary per line; the frame goes to the CPU twin.
    const Shot gpuCtl = shoot(pal, Mode::ColorCompositeOE, 14, false);
    const Shot gpu    = shoot(pal, Mode::ColorCompositeOE, 15, true);
    const Shot cpu    = shoot(pal, Mode::ColorCompositeOECpu, 15, true);
    assert(!gpuCtl.routed && gpu.routed);
    assert(gpu.px == cpu.px && "routed GPU frame = OE CPU frame");
    std::printf("  ok: PAL OE GPU routes burst-less lines to its CPU twin\n");

    // Monochrome: nothing changes.
    const Shot monoCtl = shoot(pal, Mode::MonoWhite, 15, false);
    const Shot mono    = shoot(pal, Mode::MonoWhite, 15, true);
    assert(monoCtl.px == mono.px);
    for (int y = 0; y < 192; ++y) assert(mono.chroma[size_t(y)] == 0);
    std::printf("  ok: monochrome untouched\n");
}

void testNtsc()
{
    const auto ntsc = VideoStandard::NTSC;
    const Shot s = shoot(ntsc, Mode::ColorNTSC, 16, true);
    assert(s.chroma[10] == 0 && "the killer holds colour for a while");
    assert(s.chroma[191] == 1 && "then drops it");
    int first = -1;
    for (int y = 0; y < 192 && first < 0; ++y) if (s.chroma[size_t(y)]) first = y;
    std::printf("  ok: NTSC burst-less picture, colour off from line %d\n", first);

    // MIXED: the text rows' chroma.
    const Shot ntscBand  = shoot(ntsc, Mode::ColorCompositeOECpu, 14, false, true, false);
    const Shot ntscSharp = shoot(ntsc, Mode::ColorCompositeOECpu, 14, false, true, true);
    const Shot palBand   = shoot(VideoStandard::PAL, Mode::ColorCompositeOECpu, 14, false, true, false);
    assert(maxSpread(ntscBand, 160, 192) > 40 && "NTSC MIXED text keeps colour fringes");
    assert(maxSpread(ntscSharp, 160, 192) == 0 && "Sharp text: crisp band");
    assert(maxSpread(palBand, 160, 192) == 0 && "PAL MIXED text: chroma cut");
    const Shot ntscGpu = shoot(ntsc, Mode::ColorCompositeOE, 14, false, true, false);
    assert(!ntscGpu.routed && "a 40-column band demodulates on the GPU");
    std::printf("  ok: MIXED band — NTSC coloured (Sharp text off), PAL not\n");
}

} // namespace

int main()
{
    std::printf("colour_burst_pipelines:\n");
    testPal();
    testNtsc();
    std::printf("colour_burst_pipelines OK\n");
    return 0;
}
