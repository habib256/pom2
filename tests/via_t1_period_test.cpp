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

// 6522 Timer 1 continuous-mode period — must be latch + 2, not latch + 3.
//
// Why this test exists
// --------------------
// T1 free-run is the Apple II demoscene's frame clock: arm it with one
// video frame's worth of cycles and every IRQ lands on the same beam
// position. The period is therefore not a detail — it is the whole point.
// One cycle too long and the sync DRIFTS a cycle per frame, which is
// inaudible in a music tick (the usual Mockingboard use) but destroys any
// beam-raced effect once the accumulated slip crosses a scanline.
//
// POM2 used to reload with `latch + 3`, borrowed from MAME's
// `TIMER1_VALUE + IFR_DELAY`. IFR_DELAY models the one-off underflow→IFR
// latency; it is not part of the recurring period, and folding it in
// stretched every frame.
//
// The contract, stated by French Touch's "MAD EFFECT" while computing its
// own latch (`Sources/main.a`, GPLv3, shipped in disks_5.4/demo/madef/):
//
//     ; define DELAY for INT1
//     ; PAL delay = 65*(192+70+50) = 20280
//     ; -2 (6522 takes 2 cycles to generate INT)
//     ; = 20278 = $4F36
//
// i.e. a latch of N yields a period of N + 2. The demo's 192-line drawing
// loop slid one cycle per frame under +3 until whole scanlines of the
// picture fell into VBL and were dropped by the renderer.

#include "Via6522.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

// Drive the VIA one cycle at a time and return the cycle count at which T1
// next fires. `advance()` reports the underflow directly, so this does not
// depend on IFR read/clear side effects.
int cyclesToFire(pom2::Via6522& via, int limit)
{
    for (int c = 1; c <= limit; ++c)
        if (via.advance(1)) return c;
    return -1;
}

}  // namespace

int main()
{
    constexpr uint16_t kLatch = 20278;          // $4F36 — MAD EFFECT's PAL frame
    constexpr int      kPeriod = kLatch + 2;    // 20280 = 65 * 312

    pom2::Via6522 via;
    // ACR bit 6 = T1 continuous, PB7 disabled — exactly what the demo writes
    // to $C40B (`LDA #%01000000 / STA $C40B`).
    via.write(pom2::Via6522::VIA_ACR, 0x40);
    via.write(pom2::Via6522::VIA_IER, 0xC0);   // enable T1 interrupt

    // Arm: low latch then high latch — the high write starts the countdown.
    via.write(pom2::Via6522::VIA_T1CL, kLatch & 0xFF);
    via.write(pom2::Via6522::VIA_T1CH, (kLatch >> 8) & 0xFF);

    // First interval after the arming write.
    const int first = cyclesToFire(via, kPeriod * 2);
    std::printf("first fire at %d cycles (latch %u)\n", first, kLatch);
    assert(first > 0 && "T1 never fired");

    // Every SUBSEQUENT interval is the free-run period and must be exact:
    // this is the one that accumulates.
    for (int i = 0; i < 8; ++i) {
        const int n = cyclesToFire(via, kPeriod * 2);
        std::printf("  reload %d: %d cycles\n", i, n);
        assert(n == kPeriod &&
               "T1 continuous period must be latch + 2 — a one-cycle error "
               "here drifts a whole frame per frame and slides beam-raced "
               "effects off the screen");
    }

    // A degenerate tiny latch must not spin the reload loop (the collapse
    // path) and must still respect latch + 2.
    pom2::Via6522 tiny;
    tiny.write(pom2::Via6522::VIA_ACR, 0x40);
    tiny.write(pom2::Via6522::VIA_IER, 0xC0);
    tiny.write(pom2::Via6522::VIA_T1CL, 4);
    tiny.write(pom2::Via6522::VIA_T1CH, 0);
    cyclesToFire(tiny, 64);
    const int small = cyclesToFire(tiny, 64);
    std::printf("tiny latch 4 → period %d\n", small);
    assert(small == 6 && "latch 4 must give a 6-cycle period");

    // ── ACR bit 7: PB7 is the T1 square-wave output ────────────────────
    //
    // MAME `6522via.cpp:85` `T1_SET_PB7(c) = (c & 0x80)`, folded into
    // `input_pb` / `output_pb` / `read_pb` (`:605-630`) as
    // `pb = (pb & 0x7f) | (m_t1_pb7 << 7)` — WITHOUT consulting DDRB, so a
    // port-B pin the guest configured as an input still shows the wave.
    // `t1_tick` (`:536-549`) toggles it on every continuous underflow and
    // forces it to 1 when a one-shot expires; a T1CH write clears it
    // (`:934`); reset sets it (`:347`).
    {
        pom2::Via6522 v;
        v.write(pom2::Via6522::VIA_DDRB, 0x00);   // whole port an INPUT
        assert((v.read(pom2::Via6522::VIA_ORB) & 0x80) == 0x80 &&
               "ACR.7 clear: PB7 must read as the plain pulled-up pin");

        v.write(pom2::Via6522::VIA_ACR, 0xC0);    // T1 continuous + PB7
        v.write(pom2::Via6522::VIA_T1CL, 8);
        v.write(pom2::Via6522::VIA_T1CH, 0);      // arm — and drive PB7 low
        assert((v.read(pom2::Via6522::VIA_ORB) & 0x80) == 0x00 &&
               "T1CH write must clear PB7");

        bool level = false;
        for (int i = 0; i < 6; ++i) {
            cyclesToFire(v, 64);                  // one underflow
            level = !level;
            assert(((v.read(pom2::Via6522::VIA_ORB) & 0x80) != 0) == level &&
                   "PB7 must toggle once per continuous-mode underflow");
        }
        // The collapse path (one advance() spanning several periods) must
        // land on the same parity as stepping one cycle at a time.
        const bool before = (v.read(pom2::Via6522::VIA_ORB) & 0x80) != 0;
        v.advance(10 * 10);                       // period is 10 → 10 toggles
        assert(((v.read(pom2::Via6522::VIA_ORB) & 0x80) != 0) == before &&
               "collapsed reload must apply the underflow parity to PB7");
        v.advance(10);                            // one more → flipped
        assert(((v.read(pom2::Via6522::VIA_ORB) & 0x80) != 0) != before);

        // One-shot: PB7 goes HIGH at the underflow and stays there.
        pom2::Via6522 os;
        os.write(pom2::Via6522::VIA_DDRB, 0xFF);
        os.write(pom2::Via6522::VIA_ORB, 0x00);   // every output pin low
        os.write(pom2::Via6522::VIA_ACR, 0x80);   // one-shot + PB7
        os.write(pom2::Via6522::VIA_T1CL, 8);
        os.write(pom2::Via6522::VIA_T1CH, 0);
        assert((os.read(pom2::Via6522::VIA_ORB) & 0x80) == 0x00);
        cyclesToFire(os, 64);
        assert((os.read(pom2::Via6522::VIA_ORB) & 0x80) == 0x80 &&
               "one-shot underflow drives PB7 high, over an output latch of 0");
        os.advance(1000);
        assert((os.read(pom2::Via6522::VIA_ORB) & 0x80) == 0x80 &&
               "one-shot PB7 must stay high");
    }

    // ── ACR bit 0: port-A input latching on a CA1 edge ─────────────────
    //
    // MAME `6522via.cpp:1107-1110` latches `input_pa()` on an active CA1
    // transition when ACR.0 is set; `:662-671` (ORA) and `:690-700`
    // (ORANH) then return the latch instead of the live pins for as long
    // as IFR.CA1 stays set. (Port-B latching, ACR.1, is NOT modelled —
    // it needs a CB1 this VIA does not have. See Via6522.h.)
    {
        pom2::Via6522 v;
        v.write(pom2::Via6522::VIA_DDRA, 0x00);   // port A all input
        v.write(pom2::Via6522::VIA_PCR,  0x00);   // CA1 negative edge
        v.setPortAInput(0x11);

        // Latching OFF: the read follows the pins.
        v.setCa1NegativeEdge();
        v.setPortAInput(0x22);
        assert(v.read(pom2::Via6522::VIA_ORA) == 0x22);

        v.write(pom2::Via6522::VIA_ACR, 0x01);    // PA latch enable
        v.setPortAInput(0x33);
        v.setCa1NegativeEdge();                   // freezes $33
        v.setPortAInput(0x44);                    // pins move on
        assert(v.read(pom2::Via6522::VIA_ORANH) == 0x33 &&
               "ORANH must return the CA1 latch while IFR.CA1 stands");
        assert(v.read(pom2::Via6522::VIA_ORA) == 0x33 &&
               "ORA must return the CA1 latch while IFR.CA1 stands");
        // That ORA read cleared IFR.CA1 (CLR_PA_INT) — the latch is
        // released and the live pins are visible again.
        assert(v.read(pom2::Via6522::VIA_ORA) == 0x44);
    }

    // ── advance(n) == n × advance(1) ─────────────────────────────────────
    // Bug hunt #8: the continuous-mode reload collapse counted underflows
    // with floor+1, so a slice that ended exactly ON an underflow swallowed a
    // whole period (the next interrupt a period late) and inverted PB7. The
    // lazy sync and the batched disk-turbo path both rest on this identity.
    {
        struct Case { uint8_t acr; int latch; int total; };
        const Case cases[] = {
            {0xC0, 36, 15618},    // 15618 = 411 x 38, exactly on an underflow
            {0xC0, 335, 26623},   // 26623 = 79 x 337
            {0x40, 100, 1020},    // 1020 = 10 x 102
            {0x40, 7, 900},       // 900 = 100 x 9
            {0xC0, 50, 1234},     // not a multiple: the common case
        };
        for (const Case& c : cases) {
            pom2::Via6522 single, batch;
            for (pom2::Via6522* v : {&single, &batch}) {
                v->write(pom2::Via6522::VIA_ACR, c.acr);
                v->write(pom2::Via6522::VIA_T1LL, static_cast<uint8_t>(c.latch & 0xFF));
                v->write(pom2::Via6522::VIA_T1CH, static_cast<uint8_t>(c.latch >> 8));
            }
            for (int i = 0; i < c.total; ++i) single.advance(1);
            batch.advance(c.total);
            if (single.t1Counter != batch.t1Counter || single.t1Pb7 != batch.t1Pb7 ||
                single.t1FireArmed != batch.t1FireArmed || single.ifr != batch.ifr) {
                std::printf("  acr=$%02X latch=%d total=%d: single t1=%d pb7=%d "
                            "armed=%d ifr=%02X / batch t1=%d pb7=%d armed=%d ifr=%02X\n",
                            c.acr, c.latch, c.total, single.t1Counter, single.t1Pb7,
                            single.t1FireArmed, single.ifr, batch.t1Counter,
                            batch.t1Pb7, batch.t1FireArmed, batch.ifr);
                assert(false && "advance(n) diverged from n x advance(1)");
            }
        }
        std::printf("  advance(n) == n x advance(1) on exact-multiple slices: OK\n");
    }

    std::printf("via_t1_continuous_period OK (PB7 + PA latch pinned)\n");
    return 0;
}
