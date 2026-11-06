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
// CONFIRMED IN PRINT 2026-11-01, where this used to be POM2's own derivation.
// Jim Sather, `Understanding the Apple II` (Quality Software, 1983), ch. 3:
// "The normal 6502 machine cycle lasts 14 PERIODS or about .978
// microseconds. The long cycle lasts 16 PERIODS or about 1.12 microseconds.
// There are three frequencies involved here: the primary frequency at which
// the 6502 is operated for 64 out of 65 cycles, 1.0227 MHz; the secondary
// frequency at which the 6502 operates for 1 out of 65 cycles, .8949 MHz;
// and the composite frequency which actually is the number of machine cycles
// per second, 1.0205 MHz." Ch. 8 says 912 again from the other side: "560 out
// of 912 14M periods are displayed in a horizontal cycle (remember the long
// cycle)". And ch. 3 gives the long cycle its REASON, which is also why POM2
// does not need to model the stretch: there are 3.5 COLOR REFERENCE cycles in
// a scanner cycle, so the burst phase alternates 180° every cycle and would
// alternate every LINE too, 65 being odd — "by delaying video shift timing
// half a color reference period once every horizontal line, the same
// beginning phase relationship occurs every horizontal line." POM2's renderer
// restarts each line at the same phase, which is that effect; and a 6502
// counting cycles sees 65 either way, because the long cycle is one cycle,
// merely a longer one.
//
// ── A DEFINITIONAL INCONSISTENCY POM2 KEEPS, now that both numbers are
// ── sourced: NTSC below is the 64-of-65 frequency (1 022 727 = 14.31818/14,
// Sather's "primary"), while PAL is the long-cycle AVERAGE (1 015 625). The
// two lines are therefore not the same KIND of number. Sather's composite —
// the one that "actually is the number of machine cycles per second" — is
// 1 020 484 Hz for NTSC, so POM2's NTSC cycles→seconds conversions (audio
// queues, SSI263 phoneme lengths, the cassette) run 0.22 % fast against real
// hardware where the PAL ones do not. NOT changed: 0.22 % is inaudible, the
// constant is documented in CLAUDE.md and pinned by a dozen tests, and the
// emulated BEAM is unaffected — `advanceCyclesVideo` and `floatingBus` derive
// the scanline from 65 × 262 = 17 030 cycles, not from this. What would be
// gained by changing it is a correct wall-clock frame rate (59.92 Hz rather
// than 60) and conversions that match the silicon on both standards.
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
// ── WHICH PAL CLOCK IS RIGHT? Four candidates, no settled answer ────────
// This matters because DIX / French Touch is PAL-timed and beam-raced, so a
// 0.1 % clock error walks a raster effect across the frame. Checked against
// primary sources 2026-11-01 (do not "correct" POM2 toward MAME here):
//
//   1 015 625 Hz  POM2. The PAL LINE RATE derived exactly: 15 625 Hz
//                 (625 lines x 25 Hz) x 65 cycles per line. No rounding.
//   1 016 966 Hz  MAME `A2PAL_1M_CLOCK` — and MAME carries
//                 `// FIXME: some of these values are probably wrong`
//                 DIRECTLY ABOVE IT (src/mame/apple/apple2e.cpp, verified on
//                 master). Its NTSC sibling there is 1 021 800, not the
//                 1 022 727 POM2 and everyone else use, which is a second
//                 reason not to treat that block as an oracle.
//   1 017 889 Hz  The crystal divided by 14 (14.250 450 MHz / 14), i.e. the
//                 NTSC board's divider applied to the PAL part.
//   ~1 015 300 Hz MEASURED on a real PAL //e — "1.0153 MHz on a good 12
//                 digit counter using a 100 ms gating time"
//                 [A] afrb2, applefritter "PAL IIe cassette interface"
//                 (node/33654, post #7 "Scope tests", 2024-02-06).
//
// Distance from that measurement: POM2 +0.032 %, MAME +0.164 %, crystal/14
// +0.255 %. So the one piece of real-hardware evidence anyone has found
// favours POM2's figure, by 5x over the MAME value cited above ([D], not an
// oracle) — and it also says the PAL board does NOT simply divide its
// crystal by 14, since 14.250 450/14 misses the measurement by the most.
//
// WHAT WE DO NOT KNOW: that is ONE hobbyist reading of ONE machine, not a
// controlled capture, and no source for the PAL board's actual divider chain
// was found. So POM2's value is the best-founded of the four, not proven.
// What would settle it: a frequency counter on phi0 of two different
// European //e boards, or the PAL //e schematic's divider chain. Until then,
// expect ~0.13 % drift when diffing a DIX capture against `apple2eefr`, and
// do not move POM2's constant on the strength of that diff.
//
// Separately: POM2's NTSC clock (14.31818/14, the naive divider) is 0.22 %
// FAST — a guest sees 60.05 Hz where a real NTSC Apple II gives 59.92 Hz. The
// two standards are therefore NOT derived on the same basis; PAL is the
// better-founded one. Left as-is because every NTSC-era timing constant, test
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

// ── The shape of the lit rectangle ───────────────────────────────────────
//
// What a ruler finds on a correctly adjusted monitor, which is NOT 4:3 and
// NOT the square-pixel 280:192. Sather derives it for NTSC in
// `Understanding the Apple II` ch. 8, § ASPECT RATIO IN THE APPLE DISPLAY,
// and the method is: the broadcast raster is 4:3 over its OWN active
// fractions, and the Apple fills only part of that raster.
//
//   NTSC raster  53.6 us of a 63.6 us line = 84.3 % wide
//                242.5 of 262.5 lines      = 92.4 % high
//                (.843 HD)/(.924 VD) = 4/3  =>  HD/VD = 1.46
//   Apple NTSC   560 of 912 dot periods    = 61.4 % wide  ("remember the
//                                                           long cycle")
//                192 of 262 lines          = 73.3 % high
//                (.614 HD)/(.733 VD) = .888 x 1.46 = 1.225
//
// — "This means the display window is 1.225 times as wide as it is high."
// Its pixels are therefore about 0.84 as wide as they are tall, so `Crt43`
// over-stretches by ~8.8 % and `Square` by ~19 %.
//
// PAL IS POM2'S OWN DERIVATION, NOT SATHER'S — he wrote about an NTSC
// machine and the 1985 PAL figures are not in the book. Same method, with
// the PAL raster's numbers:
//
//   PAL raster   52 us of a 64 us line     = 81.25 % wide
//                ~287.5 of 312.5 lines     = 92 % high
//                => HD/VD = (4/3) x .92/.8125 = 1.510
//   Apple PAL    560 of 912 dot periods    = 61.4 % wide (the SAME 912: the
//                                            PAL crystal was chosen so the
//                                            line still takes 912 periods)
//                192 of 312 lines          = 61.5 % high
//                => (.614/.615) x 1.510 = 1.507
//
// So a PAL Apple's picture is WIDER than its square pixels where an NTSC
// one is much narrower — because 192 lines sit in a 312-line field, which
// is why European machines showed those deep top and bottom borders. The
// PAL number is [F]: a derivation, unverified against any measurement, and
// its raster inputs (52 us active line, ~287.5 active lines) are standard
// broadcast figures cited here without their document. What would settle
// it is a ruler on a Péritel-era monitor showing a known-square test pattern.
constexpr double kSatherNtscWindowAspect = 1.225;   // Sather, printed
constexpr double kPom2PalWindowAspect    = 1.507;   // POM2's derivation

inline constexpr float measuredWindowAspect(VideoStandard standard)
{
    return standard == VideoStandard::PAL
         ? static_cast<float>(kPom2PalWindowAspect)
         : static_cast<float>(kSatherNtscWindowAspect);
}

// The beam rule shared by the soft-switch replay and the video write log: a
// bus access (switch OR store) on horizontal cycle `hpos` first reaches byte
// column `hpos - kVideoAccessColumnOffset` of its line. The scanner fetches
// column c in phi1 of hpos 25 + c, before the CPU's phi2, so an access AT
// 25 + c is one column late — [C] measured in POM2 by replaying MAD EFFECT
// (software tuned on real hardware, not a capture), offsets 21..24 valid,
// 25 not (Apple2Display::frameCycleToPos). One constant, so the two consumers
// cannot drift apart again (bug hunt 2026-11-01: the log used 25).
//
// PRIMARY SOURCE, 2026-11-01. The value is not just a calibration that
// happened to land: Sather, *Understanding the Apple IIe*, p. 5-10
// ("TEXT/LORES Scanning", the numbered considerations), states it outright —
//
//     "HBL scanned memory begins $18 bytes before display scanned memory.
//      The HBL base address can be computed from the displayed base address
//      using this Applesoft program sequence:
//          10 HBL = BASE-24
//          20 IF INT(HBL/128)<>INT(BASE/128) THEN HBL = HBL+128"
//
// $18 = 24, this constant exactly. The same page gives the two other rules
// POM2's scanner implements and had only second-hand through MAME/AppleWin
// comments until now:
//
//   * the 128-byte wraparound (step 20 above, and p. 5-11: "Horizontal
//     scanning wraps around at the 128-byte segment boundaries. Example: the
//     address scanned before address $400 is $47F") — `Memory::floatingBus`;
//   * "The first address of HBL is always addressed twice consecutively,
//     because H0-H5 is in the all zero state for two consecutive scans" —
//     which is exactly what `h_state = h_clock - (h_clock > 0)` encodes,
//     mapping h_clock 0 and 1 both to h_state 0;
//   * "During VBL, V3 and V4 are both true... The VBL base addresses are
//     equal to the FIRST 40 base addresses minus eight."
//
// And p. 5-11 confirms the two HIRES address equivalences POM2 ports from
// MAME: "A13 is equivalent to (PAGE2 . 80STORE')'", "A14 is equivalent to
// PAGE2 . 80STORE'", and "VA, VB, and VC are equivalent to A10, A11, and A12
// in HIRES". The book is at /Users/gistair/src/scrapling/work/dl (fetched
// with scrapling; the asimov copy is a 375-page image scan with no text
// layer, so render the page and read it — Table 5.1 and Figure 5.5 are PDF
// pages 116-117).
inline constexpr int kVideoAccessColumnOffset = 24;

inline constexpr const VideoTiming& pom2VideoTiming(VideoStandard s)
{
    return s == VideoStandard::PAL ? POM2_TIMING_PAL : POM2_TIMING_NTSC;
}

#endif // POM2_CPU_CLOCK_H
