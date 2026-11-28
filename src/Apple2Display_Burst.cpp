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

// Apple2Display_Burst.cpp — the colour burst, scanline by scanline, in every
// colour-composite pipeline. One concern: turning the Apple's burst
// (ColourBurst.h) and the monitor's answer to it (BurstMonitor.h) into what
// each pipeline paints. DEV.md § Colour burst.
//
// What each line's graphics dots get (pom2::LineChroma):
//   Normal     colour as before;
//   Killed     luminance only: the 1-bit dot stream where the dot pattern
//              reaches the monitor (NTSC; PAL with the Apple's trap out),
//              the colour's luminance elsewhere (LUT/MAME and AppleWin); the
//              luminance FIR (OpenEmulator demod);
//   Reduced    PAL: chroma coming back after the killer releases;
//   VInverted  PAL: R-Y negated (LUT/AppleWin: per pixel in Y'UV; OE: V);
//   VCancelled PAL: R-Y zero (the delay line averaged opposite phases).
// Text dots:
//   PAL — the LS175 colour-word latch (UA15) has GR+2 on its /CLR, so no
//     chroma leaves the Apple wherever the picture is text: [B] Sather
//     Fig. 8.6 / pp. 8-16..8-19; [D] AccurApple @ 15ed7739
//     composite_to_rgb.wgsl:455-466 cuts it per character the same way.
//     The MIXED band's crisp text was already right; a TEXT segment on a
//     line that also shows graphics now has its chroma cut column by column
//     in the OE demod (and painted from the dot stream under AppleWin).
//   NTSC — no such latch: text dots carry chroma whenever the monitor's
//     colour is on. [B] Sather, Understanding the Apple IIe p. 8-16:
//     "Coloring of TEXT characters does occur in MIXED GRAPHICS mode
//     because the COLOR BURST is not turned off, even during TEXT time";
//     Understanding the Apple II p. 8-5 says the same of the ][. With
//     "Sharp text" OFF the composite-signal pipelines (OE GPU/CPU, AppleWin)
//     now demodulate the MIXED band instead of pasting crisp text over it;
//     with it ON (the default) the band stays crisp — the same readability
//     choice that keeps full-screen text crisp. The LUT/MAME pipelines draw
//     text crisp, as MAME does.
// Monochrome monitors and the Le Chat Mauve's RGB are untouched: there is
// no burst on those wires.
//
// Cost: computeBurstResponse scans the frame's events for a TEXT edge; with
// none and TEXT off (every frame of nearly every program) it returns there,
// and nothing else in this file runs.

#include "Apple2Display.h"
#include "BurstMonitor.h"
#include "ColourBurst.h"
#include "Memory.h"

#include <algorithm>
#include <vector>

namespace {

constexpr std::uint64_t kAllCols = (std::uint64_t{1} << 40) - 1u;

bool graphicsAt(const Memory::DisplayState& s, int y)
{
    return !s.textMode && !(s.mixedMode && y >= 160);
}

pom2::BurstRules rulesFor(std::uint8_t board)
{
    switch (board) {
        case 1:  return pom2::kBurstIIPreRev7;
        case 2:  return pom2::kBurstIIRev0;
        default: return pom2::kBurstIIe;
    }
}

// B-Y scaled by `ku`, R-Y by `kv` in Y'UV (the line's chroma gain and R-Y
// factor), luminance kept (BT.601, the matrix the OE demod uses).
std::uint32_t scaleUV(std::uint32_t px, float ku, float kv)
{
    const float r = static_cast<float>(px & 0xFFu);
    const float g = static_cast<float>((px >> 8) & 0xFFu);
    const float b = static_cast<float>((px >> 16) & 0xFFu);
    const float y = 0.299f * r + 0.587f * g + 0.114f * b;
    const float u = ku * 0.492f * (b - y);
    const float v = kv * 0.877f * (r - y);
    auto cl = [](float c) {
        return static_cast<std::uint32_t>(std::clamp(c, 0.0f, 255.0f) + 0.5f);
    };
    const std::uint32_t R = cl(y + 1.139883f * v);
    const std::uint32_t G = cl(y - 0.394642f * u - 0.580622f * v);
    const std::uint32_t B = cl(y + 2.032062f * u);
    return (px & 0xFF000000u) | (B << 16) | (G << 8) | R;
}

} // namespace

void Apple2Display::computeBurstResponse(Memory& mem,
                                         const std::vector<Memory::VideoEvent>& events)
{
    burstAllNormal_    = true;
    burstDemodTouched_ = false;
    lineChroma_.fill(0);
    lineGfxCols_.fill(0);
    demodUGain_.fill(1.0f);
    demodVGain_.fill(1.0f);
    demodChromaCut_.fill(0);
    demodRelV_.fill(1.0f);
    demodLumaWide_.fill(0);
    lineGain_.fill(1.0f);
    lineV_.fill(1.0f);
    const VideoStandard vstd = mem.videoStandard();
    const bool pal = vstd == VideoStandard::PAL;
    const bool lut = hiResMode == HiResMode::ColorNTSC ||
                     hiResMode == HiResMode::ColorCompMedium ||
                     hiResMode == HiResMode::ColorComp4Bit;
    const bool signalPipe = hiResMode == HiResMode::ColorCompositeOE ||
                            hiResMode == HiResMode::ColorCompositeOECpu ||
                            hiResMode == HiResMode::ColorAppleWin;
    mixedTextDots_ = !pal && signalPipe && !oeDemod_.textSharp;
    if (!lut && !signalPipe) return;                 // mono, RGB: no burst wire

    // The TEXT and MIXED edges of the published frame (TEXT for the burst
    // gate, both for GR — the PAL trap and chroma cut).
    std::vector<pom2::TextEdge> edges;
    bool splitKinds = false;
    for (const auto& e : events) {
        if (e.kind == Memory::VideoEventKind::TextMode) {
            edges.push_back({e.emuCycle, e.value, false});
            splitKinds = true;
        } else if (e.kind == Memory::VideoEventKind::MixedMode) {
            edges.push_back({e.emuCycle, e.value, true});
            splitKinds = true;
        }
    }
    // Same frame-start rule as render() / fillCompositeSignal.
    Memory::DisplayState start = (!events.empty() || frameCounter > 0)
                                     ? mem.getDisplayStateAtFrameStart()
                                     : mem.getDisplayState();
    applyIdleSwitchOverride(start, mem);
    if (!splitKinds && !start.textMode) return;      // the common frame: full burst, no cut
    const auto rules = rulesFor(burstBoard_.load());
    const pom2::ColourBurstFrame burst = pom2::computeColourBurst(
        start.textMode, edges.data(), edges.size(),
        pom2VideoTiming(vstd).scanlinesPerFrame, rules, start.mixedMode);
    if (burst.allFull && !(pal && splitKinds)) return;

    std::array<pom2::LineChroma, kHeight> resp{};
    std::array<pom2::LineColour, kHeight> detail{};
    pom2::monitorLineChroma(burst, vstd, resp, &detail);

    // Which byte columns of each line show graphics, by the picture's own
    // column rule (forEachBeamSegment / beamColForEvent).
    if (events.empty()) {
        for (int y = 0; y < kHeight; ++y)
            lineGfxCols_[static_cast<std::size_t>(y)] = graphicsAt(start, y) ? kAllCols : 0;
    } else {
        forEachBeamSegment(start, events, vstd, 0b11,
            [&](const Memory::DisplayState& st, int y0, int y1, int c0, int c1, std::uint8_t) {
                if (c1 <= c0) return;
                const std::uint64_t bits =
                    (((std::uint64_t{1} << (c1 - c0)) - 1u) << c0) & kAllCols;
                for (int y = y0; y < y1; ++y)
                    if (graphicsAt(st, y)) lineGfxCols_[static_cast<std::size_t>(y)] |= bits;
            });
    }

    for (int y = 0; y < kHeight; ++y) {
        const auto i = static_cast<std::size_t>(y);
        const std::uint64_t gfx = lineGfxCols_[i];
        pom2::LineChroma r = resp[i];
        lineChroma_[i] = static_cast<std::uint8_t>(r);
        if (gfx != 0 && r != pom2::LineChroma::Normal) burstAllNormal_ = false;
        // A line with no graphics dot: on PAL the latch holds the colour word
        // cleared, whatever the burst; on NTSC the monitor decides.
        pom2::LineColour c = detail[i];
        if (gfx == 0 && pal) c = pom2::LineColour{0.0f, 0.0f, 0};
        demodUGain_[i] = c.gain;
        demodVGain_[i] = c.gain * c.v;
        lineGain_[i] = c.gain;
        lineV_[i]    = c.v;
        if (pal && gfx != 0 && gfx != kAllCols) demodChromaCut_[i] = ~gfx & kAllCols;
        // The OE demod's own delay line (PAL mode) takes the line's factor
        // before averaging. The Apple's 3.56 MHz luminance trap is out on a
        // PAL line whose sync saw text (ColourBurst.h trap, [B] Sather Fig.
        // 8.6): the dot pattern reaches the monitor in the luminance, in
        // colour or not. [A] the photographs agree: the dot texture shows on
        // the lines the TEXT period touched, killed or coloured, and a flat
        // grey band follows them where the killer still acts but the Apple's
        // trap is back.
        demodRelV_[i] = c.gain * static_cast<float>(c.relative);
        demodLumaWide_[i] = (pal && !burst.lines[i].trap) ? 1 : 0;
        if (demodUGain_[i] != 1.0f || demodVGain_[i] != 1.0f || demodChromaCut_[i] != 0 ||
            demodRelV_[i] != 1.0f || (demodLumaWide_[i] && gfx != 0))
            burstDemodTouched_ = true;
    }
}

void Apple2Display::finishBurstFrame(Memory& mem,
                                     const std::vector<Memory::VideoEvent>& events,
                                     const Memory::DisplayState& state)
{
    const bool aw     = hiResMode == HiResMode::ColorAppleWin;
    const bool oeGpu  = hiResMode == HiResMode::ColorCompositeOE;
    const bool oeCpu  = hiResMode == HiResMode::ColorCompositeOECpu;
    const bool lut    = hiResMode == HiResMode::ColorNTSC ||
                        hiResMode == HiResMode::ColorCompMedium ||
                        hiResMode == HiResMode::ColorComp4Bit;

    // The GPU shader has one set of uniforms for the frame: a frame whose
    // lines differ goes to its CPU twin, row by row — exactly the route a
    // frame with mixed subcarrier phases already takes, so the two stay
    // pixel-identical (oe_demod_gpu_cpu_parity) without a shader change.
    if (oeGpu && burstDemodTouched_ && signalProducedFlag && !mixedCompositeUsesFb_ &&
        !(state.textMode && oeDemod_.textSharp)) {
        scheduleCpuDemodInto80(kSignalHeight);
        mixedCompositeUsesFb_ = true;
    }

    // The painters' framebuffer is what is shown (no demod overwrites it).
    const bool painted = lut || aw || ((oeGpu || oeCpu) && pendingCpuDemodRows_ == 0);
    if (!painted) return;
    const bool palAwCut = aw && signalProducedFlag && !state.textMode &&
                          mem.videoStandard() == VideoStandard::PAL;
    bool work = !burstAllNormal_;
    if (palAwCut)
        work = work || std::any_of(demodChromaCut_.begin(), demodChromaCut_.end(),
                                   [](std::uint64_t c) { return c != 0; });
    if (!work || useCardPicture_) return;

    // Killed lines are drawn from the dot stream: build it if this pipeline
    // has none (the LUT/MAME paths). Rows it blanks (a mixed band) are text,
    // never touched below.
    if (!signalProducedFlag) {
        const bool band = mixedBandLeftBlack_;
        (void)fillCompositeSignal(mem, events);
        mixedBandLeftBlack_ = band;
    }
    // The dot stream is 560 samples a line and an HGR stripe is one sample
    // wide where the half-dot delay moved it: a 280-wide frame cannot hold
    // it, so the frame goes to the 560 domain first (each 280 pixel doubled).
    if (!useFrame80_) {
        upscaleFrameToFrame80(0, kHeight);
        setUseFrame80(true);
    }
    uint32_t* fb = frame80.data();
    const bool palStd = mem.videoStandard() == VideoStandard::PAL;
    constexpr int width = kWidth80, perCol = 14;
    constexpr std::uint32_t kLit = 0xFFFFFFFFu, kDark = 0xFF000000u;
    for (int y = 0; y < kHeight; ++y) {
        const auto i = static_cast<std::size_t>(y);
        const auto r = static_cast<pom2::LineChroma>(lineChroma_[i]);
        const std::uint64_t gfx  = (r != pom2::LineChroma::Normal) ? lineGfxCols_[i] : 0;
        // A killed line shows the dot pattern only where it reaches the
        // monitor: always on NTSC (no trap), on PAL where the Apple's trap is
        // out (ColourBurst.h trap). Elsewhere it is the colour's luminance.
        const bool dots = !palStd || demodLumaWide_[i];
        const std::uint64_t mono = (r == pom2::LineChroma::Killed && dots ? gfx : 0) |
                                   (palAwCut ? demodChromaCut_[i] : 0);
        const std::uint64_t inv  = (r != pom2::LineChroma::Killed || !dots) ? gfx : 0;
        const float ku = lineGain_[i], kv = lineGain_[i] * lineV_[i];
        if (!mono && !inv) continue;
        const uint8_t* sig = signalBuf.data() + i * kSignalWidth;
        uint32_t* row = fb + i * static_cast<std::size_t>(width);
        for (int c = 0; c < 40; ++c) {
            const bool m = (mono >> c) & 1u, v = (inv >> c) & 1u;
            if (!m && !v) continue;
            for (int x = c * perCol; x < (c + 1) * perCol; ++x) {
                if (m) {
                    row[x] = sig[x] ? kLit : kDark;
                } else {
                    row[x] = scaleUV(row[x], ku, kv);
                }
            }
        }
    }
    framebufferMutated();
}
