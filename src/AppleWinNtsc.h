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

// AppleWin-style NTSC composite simulation (CPU-only).
//
// Faithful port of AppleWin `source/NTSC.cpp::initChromaPhaseTables()`
// (Sheldon Simms / Tom Charlesworth / Michael Pohoreski — GPL v2+). Per
// the project convention (CLAUDE.md: "MAME = source of truth … cite the
// file + line range"), AppleWin is THE reference for the ColorAppleWin
// modes: the cascaded-IIR composite model, its filter coefficients, the
// YIQ→RGB matrix and the white/black/grey special-casing are ported
// line-for-line and cited inline in the .cpp.
//
// Algorithm in three steps:
//
//   1. Walk the 14.318 MHz 1-bit luminance bitstream sample by sample.
//      For each sample, push it into a 12-bit shift register (history).
//   2. Look up `lut[phase][history]` where `phase = x & 3` — Apple II's
//      pixel clock is 4× the NTSC color subcarrier, so the subcarrier
//      rotates 90° per sample. The LUT bakes three 2-pole IIR filters
//      (signal pre-filter, chroma band-pass, luma low-pass), the I/Q
//      quadrature demodulation and the YIQ→RGB matrix into one RGBA8
//      value. Two tables: Monitor (luma y0) and Color-TV (comb luma y1).
//   3. Tv sub-mode: comb-luma table + AppleWin's in-between line, a 50 %
//      blend with the previous Apple scanline of the same frame, folded
//      into one row per line (3/4 this line + 1/4 the previous).
//      Idealized sub-mode: Monitor luma with boosted chroma (POM2-only;
//      no AppleWin equivalent) — modern flat-panel-friendly look.
//
// Cost is one LUT lookup per output dot — typical scanlines render in
// well under 100 µs on a single x86 core, comparable to the existing
// MAME LUT path.

#ifndef POM2_APPLEWIN_NTSC_H
#define POM2_APPLEWIN_NTSC_H

#include <cstdint>

namespace pom2 {

struct AppleWinNtsc
{
    enum class SubMode { Monitor, Tv, Idealized };

    // Initialise the three static `[4][4096]` phase tables (Monitor,
    // Color-TV, Idealized — ~192 KB). Idempotent — the second call is a
    // no-op. Safe to call from any thread (tables written once, read-only
    // after).
    static void ensureInitialized();

    // Diagnostic/calibration only: rebuild the tables with an additive
    // offset (radians) to the CYCLESTART subcarrier angle, to pin the
    // column→hue alignment against the MAME reference. Used by the render
    // tool's phase sweep. NOT thread-safe; call before any concurrent render.
    static void rebuildForPhase(float phaseShiftRadians);

    // Render one scanline of width `w` samples (`w` is typically 560)
    // from the 1-bit luminance signal `src` into RGBA destination `dst`.
    // `prevLine` is only consulted when `mode == Tv` and `prevValid` is
    // true: the PREVIOUS Apple scanline's unblended colours (same frame),
    // blended in as AppleWin's in-between line (see renderFrame).
    static void renderLine(const uint8_t* src,
                           uint32_t*      dst,
                           int            w,
                           SubMode        mode,
                           const uint32_t* prevLine = nullptr,
                           bool           prevValid = false,
                           int            phaseOffset = 0);

    // Convenience entry point: render the whole signal buffer (h
    // scanlines of `w` samples each) into `dst`, the Tv blend included.
    // `phaseOffset` is 1 for DHGR (MAME rotl4 absX+1), else 0.
    static void renderFrame(const uint8_t* src,
                            uint32_t*      dst,
                            int            w, int h,
                            SubMode        mode,
                            int            phaseOffset = 0,
                            const uint8_t* rowPhase = nullptr);   // per row, overrides
};

} // namespace pom2

#endif // POM2_APPLEWIN_NTSC_H
