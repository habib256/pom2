#pragma once
// The bit cell a WOZ2 5.25" track is played at — INFO +39, checked against
// the platter.
//
// [B] WOZ 2.1 reference, INFO +39 "Optimal Bit Timing": "The ideal rate
//     that bits should be delivered to the disk controller card", in 125 ns
//     units, 32 = 4 µs. POM2 honours it (DEV § WOZ2 optimal_bit_timing):
//     Prisoner 2 boots only when its 32 is patched to 33.
// [B] Same reference, "When Off Isn't Really Off": the drive turns at 300 RPM,
//     so one revolution is 200 ms. The TRKS bitstream is one revolution.
// [D] MAME as_dsk.cpp @ mame0287 never reads +39: it spreads every track over
//     one revolution (200 ms / bit count). AppleWin ignores it too (GH#1022).
//
// The two meet in one case: an image that declares a FAST cell (< 32) for
// tracks whose bit count already fills a revolution. Delivering those bits at
// the declared cell would turn the platter faster than 300 RPM, which no
// drive reading the disk does. Mr. Do (woz-a-day, written by Passport.py)
// declares 29 (3.625 µs) for tracks of 53,367 bits, i.e. 193.8 ms a turn;
// one revolution gives 3.748 µs, a 30.
//
// The cell decides whether the disk boots at all. [C] An independent
// simulation of the 341-0028 P6 PROM dump (2026-11-23,
// docs/audits/protected-woz-three-way-2026-11-23.md) decodes clean GCR at
// every cell from 3.5 to 4.25 µs. But the latch holds a finished nibble for
// only about two cells: at least 6.5 CPU cycles at 3.625 µs, 7 at 3.75 µs and
// 8 at 4 µs. The boot PROM polls with `LDA $C08C,X / BPL`, a 7-cycle loop. At
// 29 it misses nibbles and never leaves `$C65E`. That is POM2's behaviour,
// and on the silicon's terms it is right. At 30 it boots, as it does in
// MAME and AppleWin. [E] The disk was sold, so it booted on the iron.
//
// So the correction is only for ROUNDING. The field counts 125 ns steps, and
// Mr. Do's tracks fill a revolution at exactly ONE step slower than it
// declares (29 → 30), which is a value rounded down. That is the one case
// raised. A declared 32 or slower is never touched: every image the corpus
// pins, Prisoner 2's 33 included, plays exactly as before. Any other
// mismatch is left as declared: a synthetic test track, a partial capture,
// or an image that really asks for a fast cell.
// [F] Whether 4am's other Passport.py images carry the same rounding is
//     unknown. Settled by: the same image captured by Applesauce, whose own
//     +39 is measured from the flux.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pom2 {

/// One revolution at 300 RPM in the field's 125 ns units: 200 ms / 125 ns.
inline constexpr uint32_t kWozRevolutionQuarterUs = 1600000u;

/// The bit cell (125 ns units) a 5.25" WOZ2 image is played at, given the
/// INFO +39 value it declares and the bit counts of its bit-cell tracks.
/// Returns `declared`, unless it is faster than 32 and the median track fills
/// one revolution at exactly one 125 ns step slower: then that step.
inline uint8_t effectiveWozBitTiming(uint8_t declared, std::vector<std::size_t> bitCounts)
{
    if (declared >= 32 || bitCounts.empty()) return declared;
    std::nth_element(bitCounts.begin(), bitCounts.begin() + bitCounts.size() / 2,
                     bitCounts.end());
    const std::size_t median = bitCounts[bitCounts.size() / 2];
    if (median == 0) return declared;
    const std::size_t oneTurn = (kWozRevolutionQuarterUs + median - 1) / median;
    return oneTurn == std::size_t{declared} + 1u ? static_cast<uint8_t>(oneTurn) : declared;
}

} // namespace pom2
