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

// Phasor speech routing oracle: AppleWin 6c579da5bd94
// Mockingboard.cpp:698-711,800-819; SSI263.cpp:806-869,930-957.
#include "PhasorCard.h"
#include "M6502.h"
#include "Memory.h"
#include "SlotBus.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <memory>
#include <vector>

namespace {
std::vector<uint8_t> capture(const PhasorCard& card)
{
    std::vector<uint8_t> bytes;
    card.appendSnapshotState(bytes);
    return bytes;
}
void program(PhasorCard& card, int base)
{
    card.slotRomWrite(base, 0xC2);
    card.slotRomWrite(base + 7, 0x80); // FILFREQ alias, including audio replay
    card.slotRomWrite(base + 3, 0x0F);
}
void testRequestsAndAliases()
{
    SlotBus bus;
    bus.setFloatingBusSource([] { return uint8_t{0x35}; });
    auto ptr = std::make_unique<PhasorCard>(4);
    auto& card = *ptr;
    bus.plug(4, std::move(ptr));
    card.deviceSelectWrite(0x0D, 0);
    assert(card.slotRomRead(0) == 0x35);
    assert(card.slotRomRead(0x40) == 0x35);
    program(card, 0x40);
    card.advanceCycles(1000000);
    assert(card.slotRomRead(0x40) == 0xB5);
    assert(card.slotRomRead(0x20) == 0x35);
    assert(card.slotRomRead(0x60) == 0x35); // secondary read precedence
    for (int reg = 0; reg < 16; ++reg) {
        assert(card.slotRomRead(0x40 + reg) == 0xB5);
        assert(card.slotRomRead(0x20 + reg) == 0x35);
        assert(card.slotRomRead(0x60 + reg) == 0x35);
    }
    assert(card.isIrqAsserted());
    program(card, 0x20);
    card.advanceCycles(1000000);
    card.slotRomWrite(0xC1, 0); // A7 aliases primary write select
    assert(card.isIrqAsserted()); // secondary still requesting
    assert(card.slotRomRead(0x40) == 0x35);
    card.slotRomWrite(0xA1, 0); // secondary alias
    assert(!card.isIrqAsserted());
    card.advanceCycles(1000000);
    card.slotRomWrite(0x61, 0); // simultaneous acknowledgement
    assert(!card.isIrqAsserted());
    // Echo hides speech accesses and direct IRQ; the chips keep running.
    card.deviceSelectWrite(0x0F, 0);
    card.slotRomWrite(0x63, 0x80); // would power both down if decoded
    card.advanceCycles(1000000);
    assert(!card.isIrqAsserted());
    card.deviceSelectWrite(0x0D, 0);
    assert(card.isIrqAsserted());
    // Both SSI writes also reach the selected VIA in compatibility mode.
    card.onPowerCycle();
    card.slotRomWrite(0x6E, 0x82); // VIA1 IER.CA1
    program(card, 0x20); // secondary -> VIA1 CA1
    card.advanceCycles(1000000);
    assert(card.peekViaRegister(0, 13) & 2);
    assert(!(card.peekViaRegister(1, 13) & 2));
    assert(card.isIrqAsserted());
    card.slotRomRead(1); // acknowledge VIA CA1, SSI request remains sticky
    assert(!card.isIrqAsserted());
    card.deviceSelectWrite(0x0D, 0);
    assert(card.isIrqAsserted());
    // Disabled SSI IRQ still exposes D7 on completion.
    card.onPowerCycle();
    card.deviceSelectWrite(0x0D, 0);
    card.slotRomWrite(0x60, 2);
    card.slotRomWrite(0x63, 15);
    card.advanceCycles(1000000);
    assert(!card.isIrqAsserted());
    assert(card.slotRomRead(0x60) == 0xB5);
}
void testWriteDecodeAndClocks()
{
    for (int mode = 0; mode < 8; ++mode) {
        for (int address = 0; address < 256; ++address) {
            PhasorCard card(4);
            card.deviceSelectWrite(0x08 | mode, 0);
            const auto before = capture(card);
            card.slotRomWrite(address, 0x9B);
            const auto after = capture(card);
            const auto tail = after.size() - 2 * pom2::Ssi263::kSnapshotBytes;
            for (int chip = 0; chip < 2; ++chip) {
                const auto offset = tail + chip * pom2::Ssi263::kSnapshotBytes;
                const bool selected = (mode == 0 || mode == 5)
                    && (address & (chip == 0 ? 0x40 : 0x20));
                const int reg = std::min(address & 7, 4);
                for (int r = 0; r < 5; ++r)
                    assert(after[offset + r] == (selected && r == reg ? 0x9B : before[offset + r]));
            }
        }
    }
    for (const double hz : {1015625.0, 4090908.0}) {
        PhasorCard card(4);
        pom2::Ssi263 oracle;
        oracle.reset();
        card.setCpuClock(hz);
        oracle.setCpuClockHz(hz);
        card.deviceSelectWrite(0x0D, 0);
        program(card, 0x60);
        oracle.write(0, 0xC2);
        oracle.write(4, 0x80);
        oracle.write(3, 15);
        const int duration = oracle.phonemeRemainingCycles();
        card.advanceCycles(duration - 1);
        assert(!card.isIrqAsserted());
        card.advanceCycles(1);
        assert(card.isIrqAsserted());
    }
}
void testSnapshotsAndReset()
{
    PhasorCard card(4);
    card.deviceSelectWrite(0x0D, 0);
    program(card, 0x60);
    card.advanceCycles(1234);
    const auto saved = capture(card);
    assert(saved[3] == 3 && saved[5] == 0xFF);
    card.advanceCycles(1000000);
    const auto live = capture(card);
    for (std::size_t n = 0; n < saved.size(); ++n) {
        card.loadSnapshotState(saved.data(), n);
        assert(capture(card) == live); // reject every truncated SSI section
    }
    card.loadSnapshotState(saved.data(), saved.size());
    assert(capture(card) == saved);
    assert(!card.isIrqAsserted());
    card.advanceCycles(1000000);
    assert(card.isIrqAsserted());
    auto legacy = saved;
    legacy.resize(legacy.size() - 2 * pom2::Ssi263::kSnapshotBytes);
    legacy[3] = 2;
    legacy[5] &= 0x3F;
    card.loadSnapshotState(legacy.data(), legacy.size());
    assert(!card.isIrqAsserted());
    assert(!(card.slotRomRead(0x60) & 0x80));
    card.onReset(); // fixed AP silicon: warm FILFREQ=0, power FILFREQ=FF
    auto reset = capture(card);
    const auto tail = reset.size() - 2 * pom2::Ssi263::kSnapshotBytes;
    assert(reset[tail + 3] == 0x80 && reset[tail + 4] == 0);
    assert(reset[tail + pom2::Ssi263::kSnapshotBytes + 4] == 0);
    card.onPowerCycle();
    reset = capture(card);
    assert(reset[tail + 4] == 0xFF);
    assert(reset[tail + pom2::Ssi263::kSnapshotBytes + 4] == 0xFF);
}
void testSpeechAudioTimeline()
{
    for (const int base : {0x20, 0x40, 0x60}) {
        Memory memory;
        M6502 cpu(&memory);
        PhasorCard card(4);
        card.setCpu(&cpu);
        card.deviceSelectWrite(0x0D, 0);
        memory.setCycleCounter(80000);
        program(card, base);
        // Producer speech is in the future of the first render cursor.
        std::vector<float> samples(1024);
        card.audioSource()->fillAudioBuffer(samples.data(), 1024);
        for (const float x : samples) assert(x == 0);
        double energy = 0;
        for (int block = 0; block < 8; ++block) {
            memory.setCycleCounter(80000 + block * 24000);
            (void)card.slotRomRead(0x12);
            card.audioSource()->fillAudioBuffer(samples.data(), 1024);
            for (const float x : samples) energy += x * x;
        }
        assert(energy > 0.1); // each independent chip, plus both, really speaks
    }
}
}
void testPhasorSpeechCases()
{
    testRequestsAndAliases();
    testWriteDecodeAndClocks();
    testSnapshotsAndReset();
    testSpeechAudioTimeline();
}
