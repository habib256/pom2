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

// Applied Engineering TransWarp — port of MAME `a2bus/transwarp.cpp`.
//
// The card has no readable register, so nothing here can be checked by
// asking it what it is. Everything is checked through what it DOES: the
// multiplier it publishes, the accesses it swallows, and the 4 KB it swaps
// under $F000. The last group drives the card through a real Memory + a
// real SlotBus, because the snoop hooks live in those two files and a test
// that called the card directly would pin the card and not the wiring.

#include "Memory.h"
#include "SlotBus.h"
#include "TranswarpCard.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

using pom2::TranswarpCard;

namespace {

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

/// A card on its own, already reset (plug() runs onPlug → onReset).
TranswarpCard* plug(Memory& mem, int slot)
{
    auto card = std::make_unique<TranswarpCard>(slot);
    card->setMemory(&mem);
    TranswarpCard* raw = card.get();
    mem.slotBus().plug(slot, std::move(card));
    return raw;
}

void testSpeedsAreExactRatios()
{
    // 3.5 and 1.75 are not fitted constants: the card runs at the Apple's
    // 7M line over 2 or 4, against a CPU at 14.31818/14. If either ever
    // becomes a rounded decimal this catches it.
    assert(near(TranswarpCard::kFullSpeed, 14.0 / 4.0));
    assert(near(TranswarpCard::kHalfSpeed, 14.0 / 8.0));

    Memory mem;
    TranswarpCard* tw = plug(mem, 4);
    assert(near(tw->cpuSpeedMultiplier(), 3.5) && "defaults are full acceleration");

    tw->setFullAcceleration(false);
    assert(near(tw->cpuSpeedMultiplier(), 1.75));
    tw->setFullAcceleration(true);
    assert(near(tw->cpuSpeedMultiplier(), 3.5));

    // DSW2 bit 7 is the master switch and outranks everything.
    tw->setAccelerationEnabled(false);
    assert(near(tw->cpuSpeedMultiplier(), 1.0));
    tw->setAccelerationEnabled(true);
    assert(near(tw->cpuSpeedMultiplier(), 3.5));
    std::printf("  ok: 3.5x / 1.75x, and the master DIP outranks both\n");
}

void testDipDefaultsMatchTheShippedSwitchBlock()
{
    // MAME `transwarp.cpp` INPUT_PORTS defaults. The one that matters is
    // DSW2 bit 5 = 0: slot 6 ships at STOCK SPEED, because that is the
    // Disk II and AE did not trust it at 3.5x.
    assert(TranswarpCard::kDsw1Default == 0x7F);
    assert(TranswarpCard::kDsw2Default == 0x5F);

    Memory mem;
    TranswarpCard* tw = plug(mem, 4);
    for (int s = 1; s <= 7; ++s)
        assert(tw->slotAccelerated(s) == (s != 6));
    std::printf("  ok: DIP defaults ship slot 6 at stock speed\n");
}

void testC074SpeedRegisterThroughMemory()
{
    Memory mem;
    TranswarpCard* tw = plug(mem, 4);

    // 1 = drop to 1 MHz.
    mem.memWrite(0xC074, 1);
    assert(tw->inOneMhzMode());
    assert(near(tw->cpuSpeedMultiplier(), 1.0));

    // 0 = back to the DIP-selected speed.
    mem.memWrite(0xC074, 0);
    assert(!tw->inOneMhzMode());
    assert(near(tw->cpuSpeedMultiplier(), 3.5));

    // 3 = halt the card's CPU. Sticky until a reset — unlike mode 1, there
    // is no register write that undoes it.
    mem.memWrite(0xC074, 3);
    assert(tw->cpuHalted());
    assert(near(tw->cpuSpeedMultiplier(), 1.0));
    mem.memWrite(0xC074, 0);
    assert(tw->cpuHalted() && "$C074=0 must not revive a halted card");
    assert(near(tw->cpuSpeedMultiplier(), 1.0));

    mem.slotBus().reset();
    assert(!tw->cpuHalted() && near(tw->cpuSpeedMultiplier(), 3.5));
    std::printf("  ok: $C074 0/1/3, and only a reset revives a halted card\n");
}

void testC074IsConsumedButC072IsNot()
{
    // MAME's dma_w RETURNS on $C074 and falls through on $C072. The
    // difference is observable on the Apple side: every $C07x access rearms
    // the paddle timer, so an access the card swallows leaves the paddles
    // alone. $C064 reads negative (bit 7 set) while the RC is discharging.
    Memory mem;
    TranswarpCard* tw = plug(mem, 4);
    (void)tw;

    auto paddleIsCounting = [&] { return (mem.memRead(0xC064) & 0x80) != 0; };

    // Let any power-on charge expire, then confirm the quiescent state.
    mem.advanceCycles(20000);
    assert(!paddleIsCounting());

    mem.memWrite(0xC074, 0);
    assert(!paddleIsCounting() && "$C074 must never reach the paddle latch");

    mem.memWrite(0xC072, 0);
    assert(paddleIsCounting() && "$C072 is snooped AND passed through");
    std::printf("  ok: $C074 is taken off the bus, $C072 is only watched\n");
}

void testSlotSlowdownWindows()
{
    Memory mem;
    TranswarpCard* tw = plug(mem, 4);

    // Slot 6 is stock speed by default, so touching its device-select
    // window ($C0E0-$C0EF — the Disk II) drops the machine to 1 MHz.
    (void)mem.memRead(0xC0EC);
    assert(near(tw->cpuSpeedMultiplier(), 1.0));
    assert(tw->slowCyclesRemaining() > 0);

    // ~20 µs, and it expires on its own as cycles are consumed.
    const int window = tw->slowCyclesRemaining();
    assert(window >= 18 && window <= 22);
    mem.advanceCycles(window - 1);
    assert(near(tw->cpuSpeedMultiplier(), 1.0));
    mem.advanceCycles(1);
    assert(tw->slowCyclesRemaining() == 0);
    assert(near(tw->cpuSpeedMultiplier(), 3.5));

    // Slot 5 ships accelerated, so its window costs nothing.
    (void)mem.memRead(0xC0D0);
    assert(near(tw->cpuSpeedMultiplier(), 3.5));

    // ...until the user flips its DIP.
    tw->setSlotAccelerated(5, false);
    (void)mem.memRead(0xC0D0);
    assert(near(tw->cpuSpeedMultiplier(), 1.0));
    mem.advanceCycles(1000);

    // The slot ROM window slows down too — that is where RWTS runs.
    (void)mem.memRead(0xC600);
    assert(near(tw->cpuSpeedMultiplier(), 1.0) && "slot 6 ROM is stock speed");
    mem.advanceCycles(1000);
    (void)mem.memRead(0xC400);
    assert(near(tw->cpuSpeedMultiplier(), 3.5) && "slot 4 ROM is accelerated");
    std::printf("  ok: per-slot 20 us windows, on both $C0nX and $CnXX\n");
}

void testJoystickWindowIsAWholePread()
{
    // MAME: 11*257 µs, long enough to cover a full PREAD sweep. No per-slot
    // gate — the paddles are on the motherboard.
    Memory mem;
    TranswarpCard* tw = plug(mem, 4);

    (void)mem.memRead(0xC070);
    const int window = tw->slowCyclesRemaining();
    assert(window > 2800 && window < 3000);
    assert(near(tw->cpuSpeedMultiplier(), 1.0));

    // It is more than two orders of magnitude longer than a slot window,
    // which is the point: a paddle read cannot be allowed to finish early.
    mem.advanceCycles(30);
    assert(tw->slowCyclesRemaining() > 2700);
    std::printf("  ok: $C070 buys a whole PREAD at 1 MHz\n");
}

void testRomShadow()
{
    Memory mem;
    // Something recognisable in the Apple's F8 ROM so the swap-back is
    // checkable. loadRomBytes is how the real loader gets bytes in there.
    std::vector<uint8_t> apple(0x1000, 0xA5);
    mem.loadRomBytes(apple.data(), apple.size(), 0xF000);
    assert(mem.memRead(0xF000) == 0xA5);

    TranswarpCard* tw = plug(mem, 4);
    assert(!tw->hasRom() && !tw->shadowActive() &&
           "no dump plugged yet — the card still accelerates");
    assert(mem.memRead(0xF000) == 0xA5);

    std::vector<uint8_t> warp(TranswarpCard::kRomSize, 0x5A);
    warp[0x000] = 0xAE;
    warp[0xFFF] = 0xEA;
    assert(tw->setRom(warp));
    assert(tw->shadowActive());
    assert(mem.memRead(0xF000) == 0xAE);
    assert(mem.memRead(0xFFFF) == 0xEA);
    assert(mem.memRead(0xEFFF) != 0xAE && "the shadow stops below $F000");

    // $C072 hands $F000-$FFFF back to the Apple.
    mem.memWrite(0xC072, 0);
    assert(tw->readsAppleRom() && !tw->shadowActive());
    assert(mem.memRead(0xF000) == 0xA5);
    assert(mem.memRead(0xFFFF) == 0xA5);

    // A reset re-covers it (MAME reset_from_bus clears m_bReadA2ROM).
    mem.slotBus().reset();
    assert(!tw->readsAppleRom() && tw->shadowActive());
    assert(mem.memRead(0xF000) == 0xAE);

    // $C074 = 3 is the OTHER release. MAME `transwarp.cpp:317-323` asserts
    // HALT on the card's CPU *and* calls `lower_slot_dma()` — the Apple gets
    // its bus back and reads its own ROM again (`dma_r:273` is the only path
    // that ever served the card's $F000 bytes). POM2 used to leave the
    // shadow up, so a self-disabling accelerator left the machine running
    // Applied Engineering's Monitor for good.
    assert(tw->shadowActive());
    mem.memWrite(0xC074, 3);
    assert(tw->cpuHalted());
    assert(!tw->shadowActive() && "$C074 = 3 must lower the DMA / ROM shadow");
    assert(mem.memRead(0xF000) == 0xA5);
    assert(mem.memRead(0xFFFF) == 0xA5);
    assert(!tw->readsAppleRom() &&
           "$C074 = 3 is not $C072 — the passthrough flag stays clear");
    // ...and a reset re-covers it, because `reset_from_bus` (`:212-217`)
    // clears the halt and raises DMA again.
    mem.slotBus().reset();
    assert(!tw->cpuHalted() && tw->shadowActive());
    assert(mem.memRead(0xF000) == 0xAE);

    // ...and unplugging must never leave the machine running AE's Monitor.
    mem.slotBus().unplug(4);
    assert(mem.memRead(0xF000) == 0xA5);
    std::printf("  ok: $F000 shadow engages, releases on $C072 and $C074=3, "
                "and unplugs clean\n");
}

void testBusAggregationAndAbsence()
{
    // The multiplier is a property of the BUS, and an ordinary machine must
    // report exactly 1.0 — that is the value EmulationController checks to
    // skip the whole scaling path.
    Memory mem;
    assert(near(mem.slotBus().cpuSpeedMultiplier(), 1.0));
    assert(mem.slotBus().busSnooper() == nullptr);

    TranswarpCard* tw = plug(mem, 4);
    assert(near(mem.slotBus().cpuSpeedMultiplier(), 3.5));
    assert(mem.slotBus().busSnooper() == tw);

    mem.slotBus().unplug(4);
    assert(near(mem.slotBus().cpuSpeedMultiplier(), 1.0));
    assert(mem.slotBus().busSnooper() == nullptr &&
           "a freed card must not stay cached as the snooper");
    std::printf("  ok: the bus aggregates, and forgets on unplug\n");
}

void testSnapshotRoundTrip()
{
    Memory mem;
    TranswarpCard* tw = plug(mem, 4);
    tw->setFullAcceleration(false);
    tw->setSlotAccelerated(3, false);
    mem.memWrite(0xC074, 1);
    (void)mem.memRead(0xC070);

    std::vector<uint8_t> blob;
    tw->appendSnapshotState(blob);
    assert(!blob.empty());

    Memory mem2;
    TranswarpCard* tw2 = plug(mem2, 4);
    tw2->loadSnapshotState(blob.data(), blob.size());
    assert(tw2->dsw1() == tw->dsw1() && tw2->dsw2() == tw->dsw2());
    assert(tw2->inOneMhzMode());
    assert(tw2->slowCyclesRemaining() == tw->slowCyclesRemaining());
    assert(near(tw2->cpuSpeedMultiplier(), 1.0));

    // A blob from a different card in the same slot must be ignored, not
    // half-applied — the slot a snapshot lands in may hold anything.
    const uint8_t foreign[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    TranswarpCard* tw3 = plug(mem2, 5);
    const uint8_t before = tw3->dsw2();
    tw3->loadSnapshotState(foreign, sizeof foreign);
    assert(tw3->dsw2() == before);
    std::printf("  ok: snapshot round-trips, and rejects a foreign blob\n");
}

} // namespace

int main()
{
    testSpeedsAreExactRatios();
    testDipDefaultsMatchTheShippedSwitchBlock();
    testC074SpeedRegisterThroughMemory();
    testC074IsConsumedButC072IsNot();
    testSlotSlowdownWindows();
    testJoystickWindowIsAWholePread();
    testRomShadow();
    testBusAggregationAndAbsence();
    testSnapshotRoundTrip();
    std::printf("OK transwarp_card\n");
    return 0;
}
