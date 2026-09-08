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

// 6522 timer + interrupt-flag timing (Via6522 unit level).
//
// T2: MAME (`6522via.cpp:957-960`) schedules the T2 underflow IRQ at
// `TIMER2_VALUE + IFR_DELAY` cycles after the T2CH write, with IFR_DELAY = 3.
// A Timer-2-synced beam-racer (French Touch DIX: `T2 = 7512 − latency`, where
// the latency folds in this 6522 delay + the 6502 IRQ sequence) lands its
// mid-scanline effect on the intended byte only if T2 fires at exactly N+3 —
// POM2 used to fire at N+1 (counter crosses < 0), two bytes early.
//
// T1: same constant — MAME schedules BOTH the first shot after a T1CH write
// (`6522via.cpp:927-943`, adjust at :941) and every continuous-mode reload
// (t1_tick, `:536-543`) at `TIMER1_VALUE + IFR_DELAY`. POM2 used to fire the
// first shot at N+1 while reloading at N+3 — the first IRQ of a music driver
// landed two cycles early relative to all subsequent ones.
//
// ORA / IFR.CA1: MAME's `CLR_PA_INT()` (`6522via.cpp:99`) runs on ANY access
// to register 1 (ORA) — read (:676) or write (:875) — clearing IFR.CA1 and,
// unless CA2 is in independent-interrupt mode (`CA2_IND_IRQ`, :51), IFR.CA2.
// Register $F (ORANH) is side-effect-free (:690-700, :888-896). Since
// setCa1NegativeEdge() can latch IFR.CA1 (Sound II SSI263 wiring), a driver
// acking via the standard ORA access must not see a stuck IRQ.

#include "Via6522.h"

#include <cassert>
#include <cstdio>

using pom2::Via6522;

namespace {
constexpr uint8_t ORA = 0x1, T1CL = 0x4, T1CH = 0x5, T1LL = 0x6;
constexpr uint8_t T2CL = 0x8, T2CH = 0x9, ACR = 0xB, PCR = 0xC, ORANH = 0xF;
constexpr uint8_t IFR_CA2 = 0x01;
constexpr uint8_t IFR_CA1 = 0x02;
constexpr uint8_t IFR_T1 = 0x40;
constexpr uint8_t IFR_T2 = 0x20;

// Cycles from the T2CH write until IFR.T2 latches, advancing one cycle at a
// time (so the test is independent of any chunk granularity).
int t2FireCycle(int n) {
    Via6522 via;
    via.write(ACR, 0x00);                 // T2 timed phase-2 mode (ACR bit5 = 0)
    via.write(T2CL, n & 0xFF);            // low latch
    via.write(T2CH, (n >> 8) & 0xFF);     // high latch → load counter + arm
    for (int c = 1; c <= n + 16; ++c) {
        via.advance(1);
        if (via.ifr & IFR_T2) return c;
    }
    return -1;
}

// Cycles from the T1CH write until IFR.T1 first latches (one-shot mode).
int t1FirstShotCycle(int n) {
    Via6522 via;
    via.write(ACR, 0x00);                 // T1 one-shot
    via.write(T1LL, n & 0xFF);            // latch low
    via.write(T1CH, (n >> 8) & 0xFF);     // latch high → load counter + arm
    for (int c = 1; c <= n + 16; ++c) {
        via.advance(1);
        if (via.ifr & IFR_T1) return c;
    }
    return -1;
}

void testT1FirstShotTiming()
{
    // First shot at exactly N+3, MAME `TIMER1_VALUE + IFR_DELAY`
    // (`6522via.cpp:941`, IFR_DELAY = 3 at :102).
    for (int n : {0, 1, 10, 100, 1000, 7479, 0x3FFF}) {
        const int c = t1FirstShotCycle(n);
        std::printf("  T1=%d → IFR.T1 at cycle %d (MAME N+IFR_DELAY = %d)\n",
                    n, c, n + 3);
        assert(c == n + 3 && "T1 first-shot timing != MAME TIMER1_VALUE+IFR_DELAY");
    }

    // Continuous mode: first shot at N+3 (the IFR_DELAY latency above
    // applies once), then every **N+2** — the hardware free-run period.
    //
    // This assertion used to demand N+3 for the reload too, copying MAME's
    // `t1_tick` (:536-543), which adds IFR_DELAY on every reload. That is a
    // MAME divergence, not hardware: IFR_DELAY is the one-off underflow→IFR
    // latency, and folding it into the recurring period stretches every
    // interval by a cycle. Harmless for a music tick; fatal when T1 is used
    // as a frame clock, because the error accumulates.
    //
    // Ground truth is French Touch "MAD EFFECT", which arms T1 as its PAL
    // frame clock and states the arithmetic in `Sources/main.a` (GPLv3,
    // shipped in disks_5.4/demo/madef/):
    //     ; PAL delay = 65*(192+70+50) = 20280
    //     ; -2 (6522 takes 2 cycles to generate INT)
    //     ; = 20278 = $4F36
    // period == latch + 2. Under N+3 its 192-line beam-raced loop slid one
    // cycle per frame until scanlines fell into VBL and were dropped — and
    // MAME, which keeps N+3, renders the demo wrong for the same reason.
    // Detailed period coverage lives in `via_t1_continuous_period`.
    {
        constexpr int n = 100;
        Via6522 via;
        via.write(ACR, 0x40);             // T1 continuous
        via.write(T1LL, n);
        via.write(T1CH, 0);
        int fires = 0, last = 0;
        for (int c = 1; c <= 3 * (n + 3); ++c) {
            via.advance(1);
            if (via.ifr & IFR_T1) {
                ++fires;
                std::printf("  T1 continuous fire %d at cycle %d\n", fires, c);
                const int expect = (n + 3) + (fires - 1) * (n + 2);
                assert(c == expect &&
                       "T1 continuous: first shot N+3, then period N+2");
                last = c;
                (void)via.read(T1CL);     // clear IFR.T1, keep counting
            }
        }
        (void)last;
        assert(fires == 3);
    }
    std::printf("OK t1 timing (first shot N+3, continuous period N+2)\n");
}

void testOraAccessClearsCa1()
{
    // PCR.0 = 0 (negative edge active — Sound II default). Latch CA1.
    Via6522 via;
    via.write(PCR, 0x00);
    via.ifr |= IFR_CA2;                   // pre-set CA2 too (no setter — POM2
                                          // never raises it; pin the clear)
    via.setCa1NegativeEdge();
    assert(via.ifr & IFR_CA1);

    // Reading ORANH ($F) must NOT clear (MAME 6522via.cpp:690-700).
    (void)via.read(ORANH);
    assert(via.ifr & IFR_CA1);
    assert(via.ifr & IFR_CA2);
    via.write(ORANH, 0x55);               // writes either (:888-896)
    assert(via.ifr & IFR_CA1);
    assert(via.ifr & IFR_CA2);

    // Reading ORA clears CA1 + CA2 (CA2 not independent, PCR=0) —
    // MAME CLR_PA_INT() at 6522via.cpp:676.
    (void)via.read(ORA);
    assert(!(via.ifr & IFR_CA1) && "ORA read must clear IFR.CA1");
    assert(!(via.ifr & IFR_CA2) && "ORA read must clear IFR.CA2 (dependent mode)");

    // Writing ORA clears too (MAME :875).
    via.setCa1NegativeEdge();
    via.ifr |= IFR_CA2;
    via.write(ORA, 0xAA);
    assert(!(via.ifr & IFR_CA1) && "ORA write must clear IFR.CA1");
    assert(!(via.ifr & IFR_CA2));

    // CA2 independent-interrupt mode: PCR[3:1] = 001 → (pcr & 0x0A) ==
    // 0x02 (MAME CA2_IND_IRQ, 6522via.cpp:51). ORA access still clears
    // CA1 but leaves CA2 latched.
    via.write(PCR, 0x02);
    via.setCa1NegativeEdge();
    via.ifr |= IFR_CA2;
    (void)via.read(ORA);
    assert(!(via.ifr & IFR_CA1));
    assert((via.ifr & IFR_CA2) && "independent CA2 must survive ORA access");

    std::printf("OK ora access clears CA1 (+CA2 unless independent)\n");
}
}  // namespace

// Bug hunt #8: an ACR write in the two cycles before a T1 underflow used to
// throw the interrupt 65536 cycles into the future. The re-arm un-biased a
// RUNNING counter by 2 and masked to 16 bits, so counters 0 and 1 wrapped to
// $FFFE / $FFFF and the fire landed 64 ms late — once per ~period/2 ACR
// writes for any driver that re-writes ACR while T1 free-runs. Whatever cycle
// the ACR write lands on, and whether T1 started one-shot or continuous, the
// first IFR.T1 must still latch at N+3.
void testAcrWriteNeverMovesALiveT1()
{
    const int N = 100;
    for (const uint8_t start : {uint8_t{0x00}, uint8_t{0x40}}) {
        for (int k = 1; k <= N + 6; ++k) {
            Via6522 via;
            via.write(ACR, start);
            via.write(T1LL, N & 0xFF);
            via.write(T1CH, (N >> 8) & 0xFF);
            int fired = -1;
            for (int c = 1; c <= N + 8; ++c) {
                if (c == k) via.write(ACR, 0x40);
                via.advance(1);
                if (via.ifr & IFR_T1) { fired = c; break; }
            }
            if (fired != N + 3) {
                std::printf("  ACR<-$40 at cycle %d (start ACR=$%02X): IFR.T1 "
                            "at %d, want %d\n", k, start, fired, N + 3);
                assert(false && "an ACR write moved a live T1's underflow");
            }
        }
    }
    std::printf("OK acr write never moves a live T1\n");
}

int main()
{
    testAcrWriteNeverMovesALiveT1();
    for (int n : {0, 1, 10, 100, 1000, 7479, 0x3FFF}) {
        const int c = t2FireCycle(n);
        std::printf("  T2=%d → IFR.T2 at cycle %d (MAME N+IFR_DELAY = %d)\n",
                    n, c, n + 3);
        assert(c == n + 3 && "T2 underflow IRQ timing != MAME TIMER2_VALUE+IFR_DELAY");
    }

    // One-shot: only ONE interrupt per T2CH write — after firing, further
    // counting must NOT re-raise IFR.T2 (MAME `m_t2_active = 0` after fire).
    {
        Via6522 via;
        via.write(ACR, 0x00);
        via.write(T2CL, 5);
        via.write(T2CH, 0);
        for (int c = 0; c < 8; ++c) via.advance(1);   // fire at cyc 8 (=5+3)
        assert(via.ifr & IFR_T2);
        via.read(T2CL);                                // T2CL read clears IFR.T2
        assert(!(via.ifr & IFR_T2));
        for (int c = 0; c < 0x20000; ++c) via.advance(1);  // counter wraps fully
        assert(!(via.ifr & IFR_T2) && "T2 one-shot re-fired (should arm once per T2CH)");
    }

    // ── T2 read-back: the hardware line `written + 1 - elapsed` ─────────
    // Same rule as T1 (commit c58528a, measured on real hardware via
    // TRIBU): the counter loads on the phi2 AFTER the CxH write, and T2 is
    // the identical silicon. MAME's -2 bias read one LOW — the drift class
    // that made DIX rasters crawl on T1. The line must also be continuous
    // through the one-shot underflow (free-running counter afterwards).
    {
        Via6522 via;
        via.write(ACR, 0x00);
        via.write(T2CL, 100);
        via.write(T2CH, 0);                    // N = 100
        int elapsed = 0;
        auto rb = [&]() {
            const int lo = via.read(T2CL);
            const int hi = via.read(T2CH);
            return (hi << 8) | lo;
        };
        for (int e : {1, 2, 50, 100, 101, 102}) {
            while (elapsed < e) { via.advance(1); ++elapsed; }
            const int expect = (100 + 1 - e) & 0xFFFF;
            const int got = rb();
            std::printf("  T2 rb at e=%d → %04X (want %04X)\n", e, got, expect);
            assert(got == expect && "T2 read-back != written+1-elapsed");
        }
        while (elapsed < 105) { via.advance(1); ++elapsed; }   // fired at 103
        assert(rb() == ((100 + 1 - 105) & 0xFFFF) &&
               "T2 read-back line must be continuous through the underflow");
    }

    testT1FirstShotTiming();
    testOraAccessClearsCa1();

    std::printf("OK via_t2_timing\n");
    return 0;
}
