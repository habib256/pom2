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

// Apple2Display_OeDemod.cpp — the CPU port of the OpenEmulator demod shader
// (NtscPostProcessor's demod-only fragment shader). One concern: turning
// signalBuf into frame80 the way the GPU does. Same Y/U/V recovery, YUV→RGB
// matrix, subcarrier phase AND user knobs (hue rotation, chroma-bandwidth
// sharpness blend, the PAL delay line via setOeDemodParams) as the GPU path —
// any edit here must be mirrored in the GLSL (NtscPostProcessor.cpp
// kFragmentShader) and re-pinned by oe_demod_gpu_cpu_parity. CRT glass
// (scanlines / mask / barrel / persistence) is layered on afterward by
// CrtEffectStack when enabled.
//
// Two inputs the shader does not have, because the frames that need them are
// handed to this twin (Apple2Display_Burst.cpp): the line's colour burst as
// the monitor answers it (a chroma gain, the R-Y factor, the PAL text
// columns whose chroma the Apple cut) and the PAL //e's luminance trap.
//
// Optimised: the tap weights depend only on the tap offset i, and the
// subcarrier sin/cos depend only on (x+i) mod 4 — both hoisted out of the
// per-pixel loop, so the inner loop is mul/add only (no trig per pixel).

#include "Apple2Display.h"

#include <algorithm>
#include <array>
#include <cmath>

void Apple2Display::renderCompositeOeCpu(int rows)
{
    constexpr float kPi = 3.14159265358979f;
    constexpr int   N = 8;
    const int   sw = kSignalWidth;                       // 560
    const int   sh = std::min(rows, kSignalHeight);     // ≤ 192 (per-row FIR)
    const uint8_t* sig = signalBuf.data();

    // OpenEmulator-exact 17-tap FIR kernels (Dolph-Chebyshev(50 dB) × sinc,
    // realIDFT recipe — see NtscPostProcessor.cpp for the GPU twin and the
    // libemulation provenance). lumaK: 2.0 MHz, sum 1, notches fs/4
    // (|H(0.25)| ≈ 0.002). chromaSoft: OE-faithful 0.6 MHz, sum 2 (the ×2
    // demod gain) — the neutral kernel at Sharpness 0.5. chromaSharp: the
    // 2.0 MHz kernel × 2, blended in over the upper half of the Sharpness
    // slider exactly like the GPU shader's mix(). Symmetric, [0]=centre.
    static const float lumaK[N + 1] = {
        0.27941f, 0.23593f, 0.13462f, 0.03665f, -0.01538f,
        -0.02210f, -0.00999f, -0.00072f, 0.00130f
    };
    // A PAL line with both luminance traps out — the Apple's 3.56 MHz trap
    // (out on a line whose sync saw text, ColourBurst.h LineBurst::trap, [B]
    // Sather Fig. 8.6) and the monitor's (out while its killer acts:
    // Apple2Display_Burst.cpp, [A]/[F]) — shows the dot pattern in its
    // luminance. lumaK's fs/4 notch stands for those traps in this pipeline
    // (fs/4 of the dot clock = 3.56 MHz on a PAL //e), so such a line takes a
    // kernel without it: [F] a 1-2-1 binomial (|H(fs/4)| = 0.5, the stripes
    // at half contrast). Settled by: a capture of the monitor's response to a
    // 3.56 MHz pattern. Sum 1, like lumaK.
    static const float lumaWide[N + 1] = { 0.5f, 0.25f, 0, 0, 0, 0, 0, 0, 0 };
    static const float chromaSoft[N + 1] = {
        0.26030f, 0.24788f, 0.21373f, 0.16602f, 0.11509f,
        0.07008f, 0.03648f, 0.01543f, 0.00515f
    };
    static const float chromaSharp[N + 1] = {
        0.55882f, 0.47185f, 0.26923f, 0.07331f, -0.03077f,
        -0.04421f, -0.01999f, -0.00144f, 0.00259f
    };
    // Chroma-bandwidth sharpness: 0.5 = neutral (pure soft kernel); only the
    // upper half of the slider blends toward the sharper 2.0 MHz kernel —
    // same clamp + mix as the GPU shader.
    const float sharp =
        std::clamp((oeDemod_.sharpness - 0.5f) * 2.0f, 0.0f, 1.0f);
    float chromaK[N + 1];
    for (int a = 0; a <= N; ++a)
        chromaK[a] = chromaSoft[a] + (chromaSharp[a] - chromaSoft[a]) * sharp;
    // OpenEmulator demod: chroma = composite·(sin φ, cos φ) → U,V; YUV→RGB
    // matrix below. The subcarrier table is built at the four raw phases
    // π/2·k; signalPhaseOffset_ enters ONCE, at the index `k = (xi+po)&3`
    // below — matching MAME's rotl4(absX + offset) (apple2video.cpp DHGR
    // is_80_column → absX+1) and the AppleWin LUT path
    // (AppleWinNtsc.cpp renderLine: `lut[(x + phase) & 3]`). Baking the
    // offset into the table AND the index applied it twice (2·po) and
    // rotated every DHGR hue by 90° (N=1 demodulated green, not the MAME
    // dark blue). Probe-verified against the MAME-LUT render for the four
    // aligned nibble patterns (see dhgr_phase_signal_test).
    float sinP[4], cosP[4];
    for (int k = 0; k < 4; ++k) {
        const float ph = kPi * 0.5f * static_cast<float>(k);
        sinP[k] = std::sin(ph);
        cosP[k] = std::cos(ph);
    }
    // Optional hue rotation in the U/V plane (user knob) — same convention
    // as the GPU shader: h = uHue·π, applied after the delay line.
    const float hueCs = std::cos(oeDemod_.hue * kPi);
    const float hueSn = std::sin(oeDemod_.hue * kPi);

    // The PAL delay line (the "PAL composite" knob): a PAL decoder averages
    // each line's chroma with the line before's, both taken through its own
    // switch — [B] TDA3560 (Philips 1986): "DELAY LINE" between the chroma
    // amplifier and the (B-Y)/(R-Y) demodulators. In step, the switch undoes
    // the Apple's own alternation and the average is a vertical chroma blur;
    // out of step (the burst model's R-Y factor, demodRelV_) the two lines
    // cancel or invert. The line above the picture is blanking: no chroma.
    // Until 2026-11-28 this knob flipped V on odd rows of a signal that never
    // alternated — every other row R-Y inverted (DEV § Colour burst).
    const bool delayLine = oeDemod_.palMode;
    std::array<float, kSignalWidth> uPrev{}, vPrev{}, uRow{}, vRow{};

    for (int y = 0; y < sh; ++y) {
        const auto yi = static_cast<std::size_t>(y);
        const uint8_t* row = sig + yi * static_cast<std::size_t>(sw);
        uint32_t* outRow = frame80.data() + yi * kWidth80;
        const int rowPhase = signalPhaseRow_[yi];
        // The line's colour burst (Apple2Display_Burst.cpp): the monitor's
        // chroma gain, and on PAL the text columns whose chroma the Apple cut.
        const std::uint64_t cut = demodChromaCut_[yi];
        const float uGain = demodUGain_[yi];
        const float vGain = delayLine ? demodRelV_[yi] : demodVGain_[yi];
        const float* lk = demodLumaWide_[yi] ? lumaWide : lumaK;
        for (int x = 0; x < sw; ++x) {
            float Y = 0.0f, U = 0.0f, V = 0.0f;
            for (int i = -N; i <= N; ++i) {
                const int xi = x + i;
                if (xi < 0 || xi >= sw) continue;
                const float s = row[xi] ? 1.0f : 0.0f;
                const float c = (cut && ((cut >> (xi / 14)) & 1u)) ? 0.0f : s;
                const int   k = (xi + rowPhase) & 3;
                const int   a = i < 0 ? -i : i;
                Y += s * lk[a];                   // FIR luma (sum=1)
                U += c * sinP[k] * chromaK[a];    // FIR chroma (sum=2 → ×2 gain)
                V += c * cosP[k] * chromaK[a];
            }
            U *= uGain;
            V *= vGain;
            uRow[static_cast<std::size_t>(x)] = U;
            vRow[static_cast<std::size_t>(x)] = V;
            if (delayLine) {
                U = 0.5f * (U + uPrev[static_cast<std::size_t>(x)]);
                V = 0.5f * (V + vPrev[static_cast<std::size_t>(x)]);
            }
            const float Ur = U * hueCs - V * hueSn;
            const float Vr = U * hueSn + V * hueCs;
            // YUV → RGB (OpenEmulator libemulation OpenGLCanvas.cpp).
            float r = Y                  + 1.139883f * Vr;
            float g = Y - 0.394642f * Ur - 0.580622f * Vr;
            float b = Y + 2.032062f * Ur;
            auto cl = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
            const uint32_t R = static_cast<uint32_t>(cl(r) * 255.0f + 0.5f);
            const uint32_t G = static_cast<uint32_t>(cl(g) * 255.0f + 0.5f);
            const uint32_t B = static_cast<uint32_t>(cl(b) * 255.0f + 0.5f);
            outRow[x] = (static_cast<uint32_t>(0xFF) << 24) | (B << 16) | (G << 8) | R;
        }
        uPrev = uRow;
        vPrev = vRow;
    }
}
