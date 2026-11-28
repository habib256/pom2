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

// The colour burst the Apple //e puts on each scanline — the MACHINE side of
// colour (what reaches the video jack). What a monitor does with it is
// BurstMonitor.h. One concern: from the TEXT switch's edges over one video
// frame, how many of the four burst states each scanline carries.
//
// [B] Sather, Understanding the Apple IIe, Fig. 8.5 (p. 8-10):
//       HBL = H5'·(H4'+H3'),  HSYNC = HBL·H3·H2',
//       CLRGATE' = (TEXT'·HBL·H3·H2)'
//     re-registered every scanner state by Q3 falling during PHASE 0. The
//     scanner's horizontal count runs $00, $40..$7F (65 states); POM2's hpos
//     counts the same states from 0, so HBL is hpos 0-24, HSYNC hpos 9-12
//     and the burst gate hpos 13-16 — Table 3.2 (p. 3-15) puts sync at
//     CK 49-52 and BURST at CK 53-56 on a count that starts at the first
//     visible state (CK = hpos + 40 mod 65): the same states.
//     p. 8-16: "COLOR BURST' is a 14-cycle (four video scanner states)
//     inverted sample of the COLOR REFERENCE ... gated by CLRGATE' ... which
//     drops low immediately after horizontal sync if the TEXT soft switch is
//     reset"; "CLRGATE' never drops low if TEXT is set". The gate is decided
//     state by state, so a TEXT flip inside hpos 13-16 truncates the burst:
//     0 to 4 burst states per line.
// [B] PAL //e (Sather pp. 8-16..8-19, Fig. 8.6): same horizontal timing; the
//     TCA650 makes the burst from CLRGATE', and its R-Y subcarrier phase
//     switch (pin 16) "toggles when CLRGATE' falls" — a line whose gate never
//     drops gets no burst AND no V toggle. `falls` counts those edges, and
//     `ident` the burst's R-Y swing through the reversal (kVSwitchTauStates).
//
// Nothing here runs on the CPU's path: the display computes it once per
// rendered frame from the beam-stamped soft-switch log Memory already keeps.

#ifndef POM2_COLOUR_BURST_H
#define POM2_COLOUR_BURST_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace pom2 {

/// First burst state, in POM2's hpos (0-64). [B] Sather Fig. 8.5 / Table 3.2.
inline constexpr int kBurstFirstHpos = 13;
/// Burst states per line. [B] Sather p. 8-16 ("four video scanner states").
inline constexpr int kBurstStates = 4;
/// States between the bus cycle that throws $C050/$C051 (the stamp of a
/// VideoEvent, Memory::accessCycle — the 4th cycle of `BIT abs`) and the
/// first burst state whose gate sees the new TEXT level.
///
/// [B] Sather p. 7-3 note 3: the IOU's soft-switch window is PHASE 0 · RAS'
///     · Q3; p. 8-16: CLRGATE' is clocked out by Q3 falling during PHASE 0
///     — the very edge that closes that window. A register clocked by the
///     edge that ends the write cannot take the new level on it: CLRGATE'
///     first shows it one state later. Lag 1.
/// [A] Real PAL //e + JVC monitor, photographs, 2026-11-28 (private test
///     program): a TEXT period ending on stamp hpos 14 makes the monitor's
///     killer act, one ending on hpos 15 gives R-Y-inverted colours. With lag 1
///     those are a 2-state and a 1-state burst, and the TCA650 switch below
///     plus the monitor (BurstMonitor.h) turn them into exactly that; lag 0
///     (3 and 2 states) and lag 2 (1 and none) do not.
/// [F] Unmeasured on a board. Settled by: a scope on IOU pin 37 (CLRGATE')
///     against the bus cycle of a `BIT $C050`.
inline constexpr int kBurstTextLag = 1;

/// The PAL //e's R-Y subcarrier phase reversal, in scanner states.
/// [B] Sather pp. 8-18..8-19: the TCA650's phase-alternated output (pin 15)
///     "is coupled to pin 11, the subcarrier input to the R-Y modulator";
///     the switch (pin 16) "toggles when CLRGATE' falls" — at the START of
///     the burst, so the burst and the picture of a line carry the same
///     phase only once the reversal has settled.
/// [F] How long it takes. A 4.43 MHz carrier through a tuned coupling
///     reverses as v(t) = s(1 - 2 e^(-t/tau)), tau = Q / (pi f): passing
///     through zero after tau·ln 2. tau = 1.2 states (1.17 us, Q ~ 16) is
///     the value that makes a 2-state burst carry no net R-Y swing and a
///     1-state burst carry an INVERTED one — the two photographs (BurstMonitor
///     .h). Settled by: a scope on TCA650 pin 11 across a CLRGATE' fall, or
///     the circuit values of Sather Fig. 8.6.
inline constexpr float kVSwitchTauStates = 1.2f;

/// Which machine's burst gate. The //e numbers above are the default; the
/// ][ and ][+ differ by board revision.
/// [B] Sather, Understanding the Apple II (1983) pp. 8-5 and 8-11: in
///     Revision 0 "the COLOR BURST was always present on the VIDEO signal"
///     (green and violet text); a Color Burst Killer (Q6) "was added in
///     Revision 1 to eliminate the COLOR BURST in TEXT MODE" — MIXED-mode
///     text keeps its colour; Revision 7 moved the gate from HBL·H4·H2'
///     (states $50-$53, hpos 17-20, after an eight-state sync) to HBL·H3·H2
///     (hpos 13-16, after the four-state sync), "in either equation ...
///     fourteen COLOR REFERENCE cycles occurring immediately after the
///     horizontal sync"; Revision 8/9 gates it with LSTTL before Q6.
/// [F] Which revision a POM2 profile is: the profiles name a model, not a
///     board. The ][+ (1979) is taken as Revision 7 or later and the
///     "Apple ][ (1977)" as Revisions 1-6 — the killer, the old window —
///     because Revision 0 lasted months. The ][ / ][+ lag is the //e's: its
///     TEXT latch is an LS259, not the IOU, and nobody has scoped it.
///     Settled by: a board revision setting, or a scope on a ][ of each era.
/// //c and //c+: [F] the //e's rule, assumed from the shared IOU design;
///     settled by the //c Technical Reference's video timing or a scope.
struct BurstRules {
    int  firstHpos     = kBurstFirstHpos;
    int  textLag       = kBurstTextLag;
    bool textKillsBurst = true;   ///< false only on a Revision 0 ][
};
inline constexpr BurstRules kBurstIIe{};                       ///< //e, //c, //c+, ][+ (Rev 7+)
inline constexpr BurstRules kBurstIIPreRev7{17, kBurstTextLag, true};   ///< ][ Rev 1-6
inline constexpr BurstRules kBurstIIRev0{17, kBurstTextLag, false};     ///< ][ Rev 0 (no killer)

/// One scanline's burst. Bit i of `mask` = burst state hpos 13+i emitted.
struct LineBurst {
    uint8_t mask  = 0;   ///< 4 bits
    uint8_t falls = 0;   ///< CLRGATE' falling edges (PAL V toggles): 0..2
    /// PAL: the burst's R-Y swing as a monitor's H/2 detector integrates it,
    /// signed against the phase the line's PICTURE carries, in units of four
    /// settled states (a full burst, settled from its start, would be 1).
    /// A normal line gives ~0.42 (the reversal eats the first states).
    float ident = 0.0f;
    /// PAL: the Apple's 3.56 MHz luminance trap is in circuit on this line.
    /// [B] Sather Fig. 8.6 and p. 8-19: "a gated trap which removes 3.56 MHz
    ///     variations from the LUMINANCE signal during GRAPHICS time" — L7/C111
    ///     grounded by Q3, driven by the second half of UC15 (LS74), whose D
    ///     is GR (GR+2) and whose clock is SYNC; COLOUR/MONO holds it clear.
    ///     The trap therefore follows GR+2 as it stood at the line's sync, for
    ///     the whole line: a line whose sync saw text keeps the dot pattern in
    ///     its luminance, graphics or not.
    /// [F] Which SYNC edge clocks it (hpos 9 is taken) — they differ only for
    ///     a switch thrown at hpos 7-10; settled by a scope on UC15 pin 9.
    bool trap = true;
    int states() const
    {
        return (mask & 1) + ((mask >> 1) & 1) + ((mask >> 2) & 1) + ((mask >> 3) & 1);
    }
};

/// A TEXT switch edge, by the bus cycle that threw it (VideoEvent::emuCycle).
struct TextEdge {
    uint64_t cycle = 0;
    bool     text  = false;   ///< the new level
    bool     mixed = false;   ///< a MIXED edge (the trap's GR), not a TEXT one
};

/// Where the trap's flip-flop samples GR+2: the start of horizontal sync,
/// two states after the access that GR+2 shows (Sather p. 8-11).
inline constexpr int kTrapClockHpos = 9;
inline constexpr int kGrLagStates   = 2;

/// The burst on every scanline of one video frame (262 NTSC / 312 PAL lines;
/// line 0 is the first visible line, as in Memory's frame).
struct ColourBurstFrame {
    static constexpr int kMaxLines = 312;
    std::array<LineBurst, kMaxLines> lines{};
    int  lineCount = 0;
    bool allFull   = false;   ///< every line carries 4 states (the common case)
    bool allNone   = false;   ///< no line carries any (full-time TEXT)
};

/// Computes the frame's burst from the TEXT level at the frame start and the
/// TEXT edges recorded during it (any order; cycles map into the frame by
/// cycle / 65 mod lines, as Memory stamps them). With no edge it costs a
/// fill, not a walk.
ColourBurstFrame computeColourBurst(bool textAtFrameStart, const TextEdge* edges,
                                    std::size_t edgeCount, int scanlinesPerFrame,
                                    const BurstRules& rules = kBurstIIe,
                                    bool mixedAtFrameStart = false);

} // namespace pom2

#endif // POM2_COLOUR_BURST_H
