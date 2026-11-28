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

// What a colour MONITOR does with the burst the Apple sends on each line
// (ColourBurst.h is the Apple's side). One concern: the monitor's burst-
// gated circuits, line by line — the NTSC killer, and the PAL decoder's
// H/2 identification, its PAL switch and its killer — and what they leave
// of each visible line's chroma. Nothing here knows a program or a picture:
// it reads the burst record and runs the circuits. Every constant a
// measurement could move is here, graded.
//
// The monitor is modelled in STEADY STATE: the circuits run over the frame
// three times before the answer is read on a fourth, i.e. the monitor is
// assumed to have seen this frame's burst on the frames before. Exact for an
// effect a program repeats every frame, and a re-render of one frame (60 Hz
// UI over 50 Hz PAL) stays byte-identical. The transient of a mode change,
// a few frames on real iron, is not modelled. [F] settled by: a video
// capture of a real monitor across a TEXT -> graphics switch.

#ifndef POM2_BURST_MONITOR_H
#define POM2_BURST_MONITOR_H

#include "ColourBurst.h"
#include "CpuClock.h"

#include <array>
#include <cstdint>

namespace pom2 {

/// What the monitor leaves of one visible line's chroma.
enum class LineChroma : uint8_t {
    Normal     = 0,   ///< colour as usual
    Killed     = 1,   ///< colour killer active: luminance only
    VInverted  = 2,   ///< PAL: the decoder's switch out of step, R-Y negated
    VCancelled = 3,   ///< PAL: the delay line averaged an in-step and an
                      ///< out-of-step line — R-Y cancels, B-Y stays
};

/// NTSC: the AppleColor Composite Monitor IIe killer.
/// [D] GSSquared @ e569985e, AppleColorKiller.hpp:21-77 — hysteresis fitted
///     by its author to photographs of an NTSC //e on an AppleColor monitor
///     (its issues #217, #223: "100 full-burst lines then text: color holds
///     131 lines"; a box of graphics after text gets colour ~12 lines in).
///     The gate sees only the 3rd and 4th burst states (hpos 15-16); each
///     line adds 4 per gated state, a line with no gated state takes 5; on
///     above 96, off below 60, capped at 716. An isolated burst-less line
///     therefore keeps its colour.
/// [F] Other NTSC monitors differ. Settled by: the same photographs on a
///     second monitor model.
struct NtscColourKiller {
    static constexpr int kGateFirst = 2;            // window positions 2,3
    static constexpr int kPerCycle  = 4;
    static constexpr int kDecay     = 5;
    static constexpr int kOnLevel   = 24 * kPerCycle;
    static constexpr int kOffLevel  = 15 * kPerCycle;
    static constexpr int kMaxLevel  = 179 * kPerCycle;

    bool colour = false;
    int  level  = 0;

    bool observe(const LineBurst& b);
};

/// PAL: a TDA3560-class decoder ([B] Philips data book 1986, TDA3560 pins 2,
/// 5, 6, 8: the burst is keyed by the sandcastle "just after the sync
/// pulse", ~4 us — the Apple's whole window, hpos 13-16; identification is a
/// synchronous H/2 detector on a 330 nF capacitor; "when the colour killer
/// is active ... the chroma amplifier supplies no signal"; the PAL switch is
/// a bistable stepped every line and set by the identification; the delay
/// line averages each line with the one before. The A.C.C. is "synchronous
/// detection of the burst signal followed by a PEAK detector": a short burst
/// keeps its peak, so the gain is left at 1).
///
/// Per line: the PAL switch steps; the Apple's switch steps once per
/// CLRGATE' fall (ColourBurst.h), so `relative` — the decoder's phase
/// against the picture's — flips on a line with an even number of falls.
/// The H/2 detector reads the burst's R-Y swing (LineBurst::ident) against
/// `relative` into a one-pole integrator; when it goes below -kFlip the
/// bistable is corrected (and the reading turns positive); colour is on above
/// kOn, off below kOff. The picture's R-Y comes out as (relative + the
/// previous line's) / 2 — +1 normal, -1 inverted, 0 cancelled.
///
/// [A] Real PAL //e + JVC monitor, photographs, 2026-11-28 (private test
///     program): with a 2-state burst (TEXT period ending on hpos 14) the
///     monitor's colour killer acts; with a 1-state burst (ending on hpos 15) it gives
///     colour with R-Y inverted (orange shows green, blue shows pink — the
///     photographs' hues move by about +130 deg, the V flip within the
///     camera's own offsets). Through ColourBurst.h's reversal a 2-state
///     burst carries almost no R-Y swing (ident ~+0.01: the detector reads
///     nothing, the killer acts) and a 1-state burst an inverted one (~-0.09:
///     the detector sets the bistable the wrong way and keeps colour on).
/// [F] The thresholds and the integrator are not in the data book (they are
///     the IC's internal currents and the 330 nF): fitted inside the band
///     the two photographs leave (kOff above +0.013, kFlip and kOn below
///     0.089), and kIdentLines short enough that a 208-line effect settles in
///     the blanking lines. AccurApple's whole-frame killer (gfx_tv3.rs @
///     15ed7739, fitted to a Philips CM8802) and AppleWin @ 0aee064a (colour
///     off at once on 50 Hz) are other monitors. Settled by: photographs of
///     3-state, 0-state and isolated burst-less lines on this monitor and a
///     second one; a scope on the decoder's identification pin.
struct PalDecoder {
    static constexpr float kIdentLines = 3.0f;    ///< integrator time constant, lines
    static constexpr float kFlip       = 0.05f;   ///< bistable corrected below -kFlip
    static constexpr float kOn         = 0.06f;   ///< killer releases above
    static constexpr float kOff        = 0.04f;   ///< killer acts below

    int   relative = 1;      ///< decoder switch against the Apple's: +1 / -1
    int   prevRelative = 1;
    float ident  = 0.0f;
    bool  colour = false;

    /// Runs one scanline; returns the line's chroma.
    LineChroma observe(const LineBurst& b);
};

/// The monitor's answer for the 192 visible lines of a frame.
/// `standard` picks the monitor (NTSC killer, PAL decoder). `relative`, when
/// given, receives each line's R-Y factor BEFORE the delay line: +1 / -1 the
/// decoder's switch against the picture's, 0 with the killer acting — what a
/// pipeline that runs the delay line itself (the OE demod's PAL mode) needs;
/// `out` already folds the delay line in for those that do not.
void monitorLineChroma(const ColourBurstFrame& frame, VideoStandard standard,
                       std::array<LineChroma, 192>& out,
                       std::array<int8_t, 192>* relative = nullptr);

} // namespace pom2

#endif // POM2_BURST_MONITOR_H
