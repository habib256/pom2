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

// Bug hunt #17 — the card's 65C02 ran at whatever rate the CALLER's
// granularity produced.
//
// `M6502::run(n)` finishes the instruction it is inside, so it returns
// n..n+6 cycles; `WorkstationCard::advanceCycles` threw the surplus away
// instead of charging it to the next slice. That surplus is a fixed cost per
// CALL, so the card's clock was a function of how finely it was paced — and
// the pacing is not a knob: `SlotBus::advanceCycles` is fanned out from
// `M6502::step()`, once per HOST INSTRUCTION, with that instruction's 2-7
// cycles. Measured against the SCC and the interval timer, which are ticked
// from the nominal slice:
//
//     grain 4096 (what the other pins hand it)   card ran +3.5 %
//     grain 2..7 (what a running machine hands it)       +22.4 %
//     grain 3                                            +65.6 %
//
// so the firmware reached LocalTalk node acquisition in 5.37 M Apple II
// cycles when the test paced it and 4.63 M when the machine did — a 14 %
// disagreement between the emulator and its own pins, and a card whose
// 230.4 kbit/s byte-time budget was a fifth too generous.
//
// The pin is the INVARIANT, not a number: the same firmware must cost the
// same number of Apple II cycles whatever size the slices arrive in.
// ROM-gated: SKIPs cleanly.

#include "WorkstationCard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

bool exists(const char* p) { std::ifstream f(p, std::ios::binary); return f.good(); }

/// Host cycles spent before the card's firmware acquires its LocalTalk node
/// address — the last milestone of its boot, ~5.6 M cycles in. `grain` 0
/// means "a realistic instruction stream", 2 to 7 cycles at a time.
uint64_t cyclesToNodeAcquisition(const std::string& rom, int grain)
{
    pom2::WorkstationCard card(7);
    if (!card.loadRom(rom)) return 0;

    uint64_t host = 0;
    unsigned lfsr = 0xACE1u;
    while (host < 40'000'000ull) {
        int g = grain;
        if (g == 0) {   // a 16-bit maximal LFSR, so the mix is reproducible
            lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
            g = 2 + static_cast<int>(lfsr % 6);
        }
        card.advanceCycles(g);
        host += static_cast<uint64_t>(g);
        if (card.localTalkNode() != 0) return host;
    }
    return 0;
}

} // namespace

int main()
{
    const char* rom = exists("roms/341-0358-A.bin") ? "roms/341-0358-A.bin"
                    : exists("roms/341-0358-a.bin") ? "roms/341-0358-a.bin"
                                                    : nullptr;
    if (!rom) {
        std::printf("workstation_card_pacing: SKIP (needs roms/341-0358-A.bin)\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    // The slot bus hands out one host instruction at a time; the other pins
    // hand out 4096-cycle chunks; `kSliceCycles` is 24. All three must buy
    // the same amount of card time.
    const uint64_t chunky = cyclesToNodeAcquisition(rom, 4096);
    const uint64_t slice  = cyclesToNodeAcquisition(rom, 24);
    const uint64_t fine   = cyclesToNodeAcquisition(rom, 3);
    const uint64_t real   = cyclesToNodeAcquisition(rom, 0);
    assert(chunky && slice && fine && real && "the firmware never acquired a node");

    std::printf("  node acquired after %llu / %llu / %llu / %llu host cycles"
                " (grain 4096 / 24 / 3 / 2-7)\n",
                (unsigned long long)chunky, (unsigned long long)slice,
                (unsigned long long)fine,   (unsigned long long)real);

    const uint64_t all[4] = { chunky, slice, fine, real };
    uint64_t lo = all[0], hi = all[0];
    for (uint64_t v : all) { if (v < lo) lo = v; if (v > hi) hi = v; }
    const double spread = 100.0 * (double)(hi - lo) / (double)lo;
    std::printf("  spread %.2f %%\n", spread);
    assert(spread < 1.0 &&
           "the card's 65C02 runs at a rate that depends on the slice size");

    std::printf("OK workstation_card_pacing\n");
    return 0;
}
