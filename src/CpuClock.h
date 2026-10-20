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

// 6502 clock: 1.0227 MHz. The Apple II master oscillator runs at
// 14.31818 MHz; the CPU clock is that divided by 14, with a "long cycle"
// every 65 cycles to keep television scan-line timing aligned with NTSC
// (colour subcarrier). We use the nominal value — long-cycle timing
// isn't modelled.

#ifndef POM2_CPU_CLOCK_H
#define POM2_CPU_CLOCK_H

inline constexpr int POM2_CPU_CLOCK_HZ = 1022727;
inline constexpr int POM2_CPU_CYCLES_PER_FRAME_60HZ = (POM2_CPU_CLOCK_HZ + 30) / 60;

// Hard ceiling for the per-frame worker cycle budget (turbo). ONE constant
// shared by the CLI `--speed` clamp and the AI server's POST /speed so the
// two knobs cannot drift apart (the invariant used to be enforced by prose
// pointing from one literal at the other). ~2 M ≈ 117× realtime covers every
// legitimate turbo use; an unbounded budget freezes the UI for seconds per
// frame while the worker holds the state lock, and stop()'s park guarantee
// relies on frames staying bounded.
inline constexpr int POM2_MAX_CYCLES_PER_FRAME = 2'000'000;

// ── The IWM's own clock ─────────────────────────────────────────────────
//
// The Integrated Wozniak Machine does not run on the CPU clock. On a //c /
// //c+ it is wired to A2BUS_7M — 14.31818 MHz / 2 = 7.159 MHz — which is
// EXACTLY seven times the 6502's 14.31818/14. That factor of seven is why
// MAME's window constants (28 / 14 / 36 / 18 ticks) can be used verbatim
// once POM2 counts in the same unit.
//
// It matters because of the 3.5" Sony drive, and only there. A 5.25" bit
// cell is 4 µs ≈ 4 CPU cycles, so a state machine clocked in whole CPU
// cycles has four samples per cell and enough room to place a window edge.
// A Sony cell is 2 µs = **2.02 CPU cycles**: two samples, and a resync that
// can only land on a cycle boundary is half a cell out. Measured
// consequence, before this constant existed: ~4 address fields recovered per
// revolution and zero complete sectors, while the same track decoded 12/12
// with no IWM in the path (`tests/sony35_iwm_read_path_test.cpp`).
//
// So `IWMDevice`'s state-machine clock and `Sony35Drive`'s flux timeline are
// both denominated in these ticks. Everything crossing back out to the rest
// of POM2 — `tick()`, snapshots, the sound sinks — stays in CPU cycles.
inline constexpr int POM2_IWM_TICKS_PER_CPU_CYCLE = 7;

// ── Video standard (machine timing) ─────────────────────────────────────
//
// NTSC and PAL Apple IIs differ in the *whole machine clock*, not just the
// colour encoding. The European/French machines (and the Apple //c that took
// the Le Chat Mauve RGB adapter on its DB-15 port) run PAL: a 14.25045 MHz
// master oscillator, 65 cycles/line × 312 lines/frame. French Touch / DIX
// productions are timed to this 50 Hz / 312-line geometry, so beam-raced
// effects and the Mockingboard-T2 frame sync only land correctly under PAL.
//
//   NTSC: 14.31818 MHz / 14 = 1 022 727 Hz, 262 lines, 60 Hz
//   PAL : 1 015 625 Hz (= 15 625 Hz line rate × 65), 312 lines, 50 Hz
//
// PAL clock provenance — DO NOT "fix" this toward MAME, it is already the
// more accurate number (corrected 2026-07-30; the previous note here had
// the reasoning backwards):
//
// An Apple II scanline is 65 CPU cycles but 912 MASTER-clock periods, not
// 910: 64 cycles of 14 periods plus one stretched "long cycle" of 16. That
// long cycle is why the NTSC machine's line rate is the famously off-spec
// 15 699.8 Hz (→ 59.92 Hz frame), and why its true long-cycle-AVERAGED CPU
// clock is 14 318 180 / 912 × 65 = 1 020 484 Hz.
//
// The PAL crystal 14.250450 MHz was chosen so that the SAME 912-period line
// lands exactly on the PAL broadcast line rate: 912 × 15 625 = 14 250 000.
// So for PAL the long-cycle average is simply 15 625 × 65 = 1 015 625 Hz —
// the value below, accurate to 0.003 % of 14 250 450 / 912 × 65.
//
// Cross-check of the alternatives:
//   * 14.25045/14 = 1 017 889 Hz is the NAIVE divider — it ignores the long
//     cycle and is 0.22 % fast.
//   * MAME's `m_pal ? 1016966 : 1021800` is NOT long-cycle-averaged either:
//     1016966/1021800 == 14250450/14318180, i.e. MAME scales its NTSC figure
//     by the crystal ratio, and that NTSC figure is itself 0.13 % above the
//     true 1 020 484. MAME's PAL number inherits the same 0.13 % error.
//
// MAME oracle for European //e PAL work is `apple2eefr` (enhanced France),
// NOT the US `apple2ee`. Its `-listxml` screen reports the true PAL geometry:
//   vtotal=312, refresh=50.146252 Hz, pixclock=14 237 524 Hz (≈14.2375 MHz),
//   CPU W65C02 @ 1 016 966 Hz (= pixclock/14). Use that machine for
//   like-for-like DIX / French Touch captures; `apple2ee` is NTSC-only.
//
// Consequence worth knowing: POM2's PAL clock is right to 0.003 %, while
// POM2's NTSC clock (14.31818/14, the naive divider) is 0.22 % FAST — a
// guest sees 60.05 Hz where a real NTSC Apple II gives 59.92 Hz. The two
// standards are therefore NOT derived on the same basis; PAL is the
// accurate one. Left as-is because every NTSC-era timing constant, test
// and golden capture in the tree is calibrated against 1 022 727.
//
// `cyclesPerFrame` mirrors the NTSC convention (round(clock / refresh)); it is
// the CPU budget the worker runs per UI tick and is intentionally decoupled
// from `scanlinesPerFrame` (the video geometry used by the beam-racing /
// floating-bus scanner), exactly as the NTSC 17045-vs-262×65 split already is.
enum class VideoStandard { NTSC, PAL };

struct VideoTiming {
    VideoStandard standard;
    int  cpuClockHz;          // nominal 6502 clock
    int  scanlinesPerFrame;   // 262 NTSC / 312 PAL — video geometry
    int  cyclesPerScanline;   // 65 both
    int  visibleScanlines;    // 192 both
    int  cyclesPerFrame;      // CPU budget per UI frame (round(clock/refresh))
    int  refreshHz;           // 60 NTSC / 50 PAL
};

inline constexpr VideoTiming POM2_TIMING_NTSC{
    VideoStandard::NTSC, 1022727, 262, 65, 192, 17045, 60 };
inline constexpr VideoTiming POM2_TIMING_PAL{
    VideoStandard::PAL,  1015625, 312, 65, 192, 20313, 50 };

inline constexpr const VideoTiming& pom2VideoTiming(VideoStandard s)
{
    return s == VideoStandard::PAL ? POM2_TIMING_PAL : POM2_TIMING_NTSC;
}

#endif // POM2_CPU_CLOCK_H
