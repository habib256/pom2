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

// The diagnostic capture is ONE acquisition, and it is atomic — hunt #19.
//
// The Mockingboard panel used to build its view one accessor at a time:
// `peekViaRegister` × 9 per VIA + `getAyRegister` × 16 per chip +
// `snapshotSsi263` = 51 separate locks of the card mutex per rendered
// frame (82 on the Phasor), at 60 Hz, on the same mutex the realtime audio
// thread takes on every callback and the CPU thread takes on every MMIO
// access.
//
// The cost that matters is not the lock traffic, it is that the panel
// showed a machine state that never existed: VIA1 read at one instant,
// VIA2 fifty acquisitions later, with the guest running in between. This
// pins the invariant that makes the panel trustworthy — everything in one
// capture comes from ONE instant — by writing the same counter to both
// chips from another thread and checking the two never disagree by more
// than the one step that legitimately separates them.

#include "Mockingboard.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

int g_failures = 0;
void fail(const char* what) { std::printf("FAIL: %s\n", what); ++g_failures; }

// PB0 = BC1, PB1 = BDIR, PB2 = /RESET (active low).
void ayWrite(MockingboardCard& c, int chip, uint8_t reg, uint8_t v)
{
    const uint8_t b = chip ? 0x80 : 0x00;
    c.slotRomWrite(b | 0x01, reg);
    c.slotRomWrite(b | 0x00, 0x07);   // LATCH
    c.slotRomWrite(b | 0x00, 0x04);   // INACTIVE
    c.slotRomWrite(b | 0x01, v);
    c.slotRomWrite(b | 0x00, 0x06);   // WRITE
    c.slotRomWrite(b | 0x00, 0x04);   // INACTIVE
}

// ── 1. One capture = one instant ─────────────────────────────────────────
void testCaptureIsAtomic()
{
    MockingboardCard card(4);
    for (int chip = 0; chip < 2; ++chip) {
        const uint8_t b = chip ? 0x80 : 0x00;
        card.slotRomWrite(b | 0x03, 0xFF);     // DDRA
        card.slotRomWrite(b | 0x02, 0x07);     // DDRB
    }

    // The writer walks a counter through AY register 0 on BOTH chips. A
    // reader that takes the lock once can only ever catch them equal, or
    // one step apart (caught between the two writes). A reader that takes
    // it 51 times catches whatever the writer did in between.
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        uint8_t v = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            ayWrite(card, 0, 0, v);
            ayWrite(card, 1, 0, v);
            ++v;
        }
    });

    int worst = 0;
    for (int i = 0; i < 200000 && g_failures == 0; ++i) {
        const MockingboardCard::Diagnostics d = card.captureDiagnostics();
        // Unsigned 8-bit distance, forward from chip 1 to chip 0.
        const int skew = static_cast<uint8_t>(d.chip[0].ay[0] - d.chip[1].ay[0]);
        if (skew > worst) worst = skew;
        if (skew > 1) {
            std::printf("FAIL: one capture saw chip0 r0=$%02X and chip1 r0=$%02X "
                        "— %d writes apart, so it is not one instant\n",
                        d.chip[0].ay[0], d.chip[1].ay[0], skew);
            ++g_failures;
        }
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    if (g_failures == 0)
        std::printf("  ok: 200000 captures, worst chip-to-chip skew %d write(s)\n",
                    worst);
}

// ── 2. …and it reports exactly what the accessors did ────────────────────
//
// The capture replaced 51 accessor calls in AudioCoordinator. On a quiet
// card it must answer identically to the accessors it replaced, or the
// panel silently changed meaning.
void testCaptureMatchesTheAccessors()
{
    MockingboardCard card(4, MockingboardCard::Variant::SoundII);
    for (int chip = 0; chip < 2; ++chip) {
        const uint8_t b = chip ? 0x80 : 0x00;
        card.slotRomWrite(b | 0x03, 0xFF);
        card.slotRomWrite(b | 0x02, 0x07);
        card.slotRomWrite(b | 0x0B, 0x40);     // ACR: T1 free-run
        card.slotRomWrite(b | 0x0E, 0xC0);     // IER: enable T1
    }
    ayWrite(card, 0, 7, 0x38);
    ayWrite(card, 0, 8, 0x0F);
    ayWrite(card, 1, 7, 0x3E);
    card.slotRomWrite(0x40, 0x0A);             // an SSI263 phoneme

    // No CPU is attached, so nothing moves between the calls and the
    // comparison is exact — counters included.
    const MockingboardCard::Diagnostics d = card.captureDiagnostics();
    for (int c = 0; c < 2; ++c) {
        for (int r = 0; r < 16; ++r) {
            if (d.chip[c].via[r] != card.peekViaRegister(c, r))
                fail("a captured VIA register differs from peekViaRegister");
            if (d.chip[c].ay[r] != card.getAyRegister(c, r))
                fail("a captured AY register differs from getAyRegister");
        }
        if (d.chip[c].viaWrites != card.getViaWriteCount(c) ||
            d.chip[c].ayWrites  != card.getAyWriteCount(c)  ||
            d.chip[c].ayResets  != card.getAyResetCount(c))
            fail("a captured telemetry counter differs from its accessor");
        for (int k = 0; k < 4; ++k)
            if (d.chip[c].cmd[k] != card.getAyCommandCount(c, k))
                fail("a captured AY command count differs from its accessor");
    }
    MockingboardCard::Ssi263Snap ssi{};
    const bool has = card.snapshotSsi263(&ssi);
    if (d.hasSsi != has) fail("the capture disagrees about having an SSI263");
    if (has && (d.ssi.currentPhoneme != ssi.currentPhoneme ||
                d.ssi.phonemeWriteCount != ssi.phonemeWriteCount))
        fail("the captured SSI263 state differs from snapshotSsi263");
    if (d.irqAsserted != card.isIrqAsserted())
        fail("the captured IRQ state differs from isIrqAsserted");
    if (g_failures == 0)
        std::printf("  ok: the capture answers exactly what the 51 accessors did\n");
}

}  // namespace

int main()
{
    testCaptureMatchesTheAccessors();
    testCaptureIsAtomic();
    if (g_failures) return 1;
    std::puts("card_diagnostics_atomic OK");
    return 0;
}
