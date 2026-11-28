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

// Pins OpenEmulator demod parity: the GPU shader (NtscPostProcessor.cpp
// kFragmentShader) must match renderCompositeOeCpu() (the reference CPU
// path) pixel-for-pixel. Without GL — re-simulates the full GLSL demod in
// C++ (subcarrier phase, kernel sharpness mix, the PAL delay line,
// hue rotation) and compares on HGR + DHGR, at neutral knobs AND with
// hue/sharpness/PAL engaged (those used to be GPU-only — the CPU demod
// silently ignored them). Also pins the textSharp=false path: full-screen
// TEXT must demodulate on the CPU exactly like the GPU shader does.

#include "Apple2Display.h"
#include "Memory.h"

// std::max's initializer_list overload lives here. libc++ and GCC's own
// libstdc++ both drag <algorithm> in transitively, so this compiled on macOS
// and on the Linux GCC leg for as long as it existed; Ubuntu clang against
// libstdc++ 14 does not, and said so the first time the coverage job built
// the tree. Third compiler combination, first sighting.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int   N = 8;

static const float kLumaK[N + 1] = {
    0.27941f, 0.23593f, 0.13462f, 0.03665f, -0.01538f,
    -0.02210f, -0.00999f, -0.00072f, 0.00130f
};
static const float kChromaSoft[N + 1] = {
    0.26030f, 0.24788f, 0.21373f, 0.16602f, 0.11509f,
    0.07008f, 0.03648f, 0.01543f, 0.00515f
};
static const float kChromaSharp[N + 1] = {
    0.55882f, 0.47185f, 0.26923f, 0.07331f, -0.03077f,
    -0.04421f, -0.01999f, -0.00144f, 0.00259f
};

uint32_t packRgb(float r, float g, float b)
{
    auto cl = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    const uint32_t R = static_cast<uint32_t>(cl(r) * 255.0f + 0.5f);
    const uint32_t G = static_cast<uint32_t>(cl(g) * 255.0f + 0.5f);
    const uint32_t B = static_cast<uint32_t>(cl(b) * 255.0f + 0.5f);
    return 0xFF000000u | (B << 16) | (G << 8) | R;
}

// GPU shader demod, re-simulated tap-for-tap from the GLSL: phase offset
// enters the subcarrier phase exactly ONCE, π/2·((xi + po) & 3), matching
// MAME rotl4(absX+1) / AppleWin lut[(x + phase) & 3]; the chroma kernel is
// mix(soft, sharp, clamp((sharpness-0.5)·2)); PAL is the delay line (U/V
// averaged with the line before); hue rotates U/V afterwards. (An earlier revision applied the
// phase offset twice here AND in the shader, replicating the CPU path's
// double-application bug instead of pinning the correct phase.)
uint32_t demodGpuStyle(const uint8_t* row, const uint8_t* prevRow, int x, int phaseOffset,
                       float hue, float sharpness, bool palMode,
                       float chromaGain = 1.0f)
{
    float sharp = (sharpness - 0.5f) * 2.0f;
    if (sharp < 0.0f) sharp = 0.0f;
    if (sharp > 1.0f) sharp = 1.0f;
    // PAL: the delay line — chroma averaged with the line before (blanking
    // above the picture: no chroma).
    float Y = 0.0f, U = 0.0f, V = 0.0f, Up = 0.0f, Vp = 0.0f;
    const float sigX = static_cast<float>(x) + 0.5f;
    for (int i = -N; i <= N; ++i) {
        const float fx = sigX + static_cast<float>(i);
        const int   xi = static_cast<int>(std::floor(fx));
        float s = 0.0f;
        if (xi >= 0 && xi < 560)
            s = row[xi] ? 1.0f : 0.0f;
        const int a = i < 0 ? -i : i;
        const float wc = kChromaSoft[a] + (kChromaSharp[a] - kChromaSoft[a]) * sharp;
        const int phaseIdx = (xi + phaseOffset) & 3;
        const float phase = kPi * 0.5f * static_cast<float>(phaseIdx);
        Y += s * kLumaK[a];
        U += s * std::sin(phase) * wc;
        V += s * std::cos(phase) * wc;
        const float sp = (prevRow && xi >= 0 && xi < 560 && prevRow[xi]) ? 1.0f : 0.0f;
        Up += sp * std::sin(phase) * wc;
        Vp += sp * std::cos(phase) * wc;
    }
    if (palMode) {
        U = 0.5f * (U + Up);
        V = 0.5f * (V + Vp);
    }
    U *= chromaGain;   // the line's colour burst (Apple2Display_Burst.cpp)
    V *= chromaGain;
    const float h  = hue * kPi;
    const float cs = std::cos(h), sn = std::sin(h);
    const float Ur = U * cs - V * sn;
    const float Vr = U * sn + V * cs;
    const float r = Y + 1.139883f * Vr;
    const float g = Y - 0.394642f * Ur - 0.580622f * Vr;
    const float b = Y + 2.032062f * Ur;
    return packRgb(r, g, b);
}

uint32_t pxCpu(const Apple2Display& d, int x, int y)
{
    return d.pixels()[y * d.width() + x] & 0x00FFFFFFu;
}

int maxChannelDelta(uint32_t a, uint32_t b)
{
    const int dr = static_cast<int>((a      ) & 0xFF) - static_cast<int>((b      ) & 0xFF);
    const int dg = static_cast<int>((a >> 8 ) & 0xFF) - static_cast<int>((b >> 8 ) & 0xFF);
    const int db = static_cast<int>((a >> 16) & 0xFF) - static_cast<int>((b >> 16) & 0xFF);
    return std::max({std::abs(dr), std::abs(dg), std::abs(db)});
}

void assertParity(const Apple2Display& disp, int phaseOffset,
                  const Apple2Display::OeDemodParams& p, const char* label,
                  float chromaGain = 1.0f)
{
    const int w = disp.width();
    const int h = disp.height();
    const uint8_t* sig = disp.signal();
    int worst = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = sig + static_cast<size_t>(y) * 560;
        const uint8_t* prev = y > 0 ? row - 560 : nullptr;
        for (int x = 0; x < w; ++x) {
            const uint32_t cpuPx = pxCpu(disp, x, y);
            const uint32_t gpuPx = demodGpuStyle(row, prev, x, phaseOffset,
                                                 p.hue, p.sharpness, p.palMode,
                                                 chromaGain);
            const int d = maxChannelDelta(cpuPx, gpuPx);
            if (d > worst) worst = d;
            assert(d <= 1 && "GPU-style demod must match OE CPU within 1 LSB");
        }
    }
    std::printf("  %-28s phaseOffset=%d maxDelta=%d\n", label, phaseOffset, worst);
}

void setupHgr(Memory& mem)
{
    mem.memRead(0xC050);
    mem.memRead(0xC057);
    const uint16_t row = 0x2000;
    mem.memWrite(row, 0x55);
    mem.memWrite(static_cast<uint16_t>(row + 1), 0x2A);
    mem.memWrite(static_cast<uint16_t>(row + 2), 0xD5);
}

void setupDhgr(Memory& mem)
{
    mem.setIIEMode(true);
    mem.memRead(0xC050);
    mem.memWrite(0xC00D, 0);
    mem.memRead(0xC057);
    mem.memRead(0xC05E);
    const uint16_t row = 0x2000;
    mem.memWrite(row, 0x55);
    mem.auxDataMutable()[row] = 0x2A;
}

// Knob sets: neutral (the historical pin) + engaged hue/sharpness/PAL —
// each exercised on HGR (phase 0) and DHGR (phase 1).
const Apple2Display::OeDemodParams kNeutral{};
Apple2Display::OeDemodParams knobs()
{
    Apple2Display::OeDemodParams p;
    p.hue       = 0.23f;
    p.sharpness = 0.85f;
    p.palMode   = true;
    return p;
}

void runCase(Apple2Display& disp, Memory& mem, int phaseOffset,
             const Apple2Display::OeDemodParams& p, const char* label,
             float chromaGain = 1.0f)
{
    disp.setOeDemodParams(p);
    disp.render(mem);
    assert(disp.signalProduced());
    assert(disp.signalPhaseOffset() == phaseOffset);
    assertParity(disp, phaseOffset, p, label, chromaGain);
}

} // namespace

int main()
{
    Apple2Display disp;
    disp.setHiResMode(Apple2Display::HiResMode::ColorCompositeOECpu);

    Memory memHgr;
    setupHgr(memHgr);
    std::printf("HGR parity:\n");
    runCase(disp, memHgr, 0, kNeutral, "neutral");
    runCase(disp, memHgr, 0, knobs(),  "hue+sharpness+PAL");

    Memory memDhgr;
    setupDhgr(memDhgr);
    disp.setAuxMemory(memDhgr.auxData());
    std::printf("DHGR parity:\n");
    runCase(disp, memDhgr, 1, kNeutral, "neutral");
    runCase(disp, memDhgr, 1, knobs(),  "hue+sharpness+PAL");

    // textSharp=false: full-screen TEXT is demodulated, and it has NO colour
    // burst — [B] Sather, Understanding the Apple IIe p. 8-16, "CLRGATE'
    // never drops low if TEXT is set" — so the monitor's killer leaves the
    // luminance alone (chroma gain 0, Apple2Display_Burst.cpp). The GPU
    // shader has no per-line gain: such a frame goes to this CPU twin
    // (checked below). The output is 560-wide (frame80).
    Memory memText;
    memText.memRead(0xC051);
    for (uint16_t a = 0x0400; a < 0x0800; ++a)
        memText.memWrite(a, static_cast<uint8_t>('A' | 0x80));
    Apple2Display dispText;
    dispText.setHiResMode(Apple2Display::HiResMode::ColorCompositeOECpu);
    Apple2Display::OeDemodParams pt;
    pt.textSharp = false;
    std::printf("TEXT (textSharp=off) parity:\n");
    runCase(dispText, memText, 0, pt, "demodulated text, no burst", 0.0f);
    assert(dispText.width() == 560 && "demodulated TEXT presents frame80");
    Apple2Display dispGpu;
    dispGpu.setHiResMode(Apple2Display::HiResMode::ColorCompositeOE);
    dispGpu.setOeDemodParams(pt);
    dispGpu.render(memText);
    assert(dispGpu.mixedCompositeUsesFramebuffer() &&
           "a burst-less frame leaves the shader for its CPU twin");
    assertParity(dispGpu, 0, pt, "GPU mode routed to CPU twin", 0.0f);

    std::printf("oe_demod_gpu_cpu_parity OK\n");
    return 0;
}
