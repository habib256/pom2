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

// PhasorCard smoke test — pins the observable surfaces of the
// dual-mode Phasor card:
//
//   1. VIA address decode — MAME `a2bus_phasor_device::read_cnxx` /
//      `write_cnxx` (`a2mockingboard.cpp:312-337` / `:365-390`): decode
//      is gated to $00-$1F and $80-$9F in BOTH modes. Mockingboard mode
//      selects VIA1 below $80 and VIA2 above; Phasor-native mode uses
//      `via_sel = ((off & 0x80) >> 6) | ((off & 0x10) >> 4)` so
//      $00-$0F → none, $10-$1F → VIA1, $80-$8F → VIA2, $90-$9F → BOTH
//      (write broadcast; read ORs the two bytes). Undecoded reads
//      return 0.
//   2. Mode soft-switch ($C0(8+s)X) — boots in PH_Mockingboard, a
//      device-select read/write at $C0(8+s)D transitions to PH_Phasor
//      (mode=5), $C0(8+s)8 clears back to PH_Mockingboard (mode=0).
//      Both reads AND writes trigger the switch (AppleWin behaviour).
//   3. MB-compat routing — in PH_Mockingboard the primary AY of each
//      VIA pair (AY0 for VIA1, AY2 for VIA2) receives the LATCH+WRITE
//      strobes; the secondary AYs (AY1, AY3) stay untouched even when
//      the chip-select bits (PB3/PB4) say otherwise.
//   4. Phasor-native routing — in PH_Phasor the active-low chip-select
//      decode `chip_sel = (~(pb >> 3)) & 3` honours PB3 (primary) and
//      PB4 (secondary): primary-only writes hit AY0 (not AY1);
//      secondary-only hit AY1 (not AY0); broadcast hits both. VIA2's
//      strobes land on the AY2/AY3 pair. (Strobed through the VIA1
//      window at $10-$1F per the native decode above.)

#include "AudioDevice.h"
#include "CpuClock.h"
#include "M6502.h"
#include "Memory.h"
#include "PhasorCard.h"
#include "SlotBus.h"
#include "Via6522.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <vector>

namespace {

// Build a Port B byte: BC1=bc1, BDIR=bdir, /RESET=1 (chip running),
// PB3=cs0 (primary select, active LOW so pass 0 to select), PB4=cs1
// (secondary select), other bits = 1.
uint8_t makePb(bool bc1, bool bdir, bool selPrimary, bool selSecondary)
{
    uint8_t pb = 0xFF;                            // start all high
    if (bc1)        pb |= 0x01; else pb &= ~0x01;
    if (bdir)       pb |= 0x02; else pb &= ~0x02;
    pb |= 0x04;                                   // /RESET=1 (no reset)
    if (selPrimary)   pb &= ~0x08; else pb |= 0x08; // PB3 active-low
    if (selSecondary) pb &= ~0x10; else pb |= 0x10; // PB4 active-low
    return pb;
}

// Slot-ROM window that reaches `viaIdx` EXCLUSIVELY under the current
// mode (MAME decode): VIA1 = $00 in MB-compat mode but $10 in native
// mode ($00-$0F is undecoded there); VIA2 = $80 in both ($90-$9F would
// broadcast to BOTH VIAs in native mode).
uint8_t viaBase(const PhasorCard& card, int viaIdx)
{
    if (viaIdx != 0) return 0x80;
    return (card.mode() == PhasorCard::PH_Mockingboard) ? 0x00 : 0x10;
}

// Drive one LATCH+WRITE strobe through `viaIdx`: latch AY register
// `regAddr`, then write `data`. Both AYs in the pair will receive
// these depending on the mode + chip-select bits in `pb`.
void doLatchWrite(PhasorCard& card, int viaIdx, uint8_t pb,
                  uint8_t regAddr, uint8_t data)
{
    const uint8_t base = viaBase(card, viaIdx);
    // DDRA = all output, DDRB = all output.
    card.slotRomWrite(base + pom2::Via6522::VIA_DDRA, 0xFF);
    card.slotRomWrite(base + pom2::Via6522::VIA_DDRB, 0xFF);
    // LATCH ADDR phase: BDIR=1, BC1=1, plus chip-select bits.
    card.slotRomWrite(base + pom2::Via6522::VIA_ORA,  regAddr);
    card.slotRomWrite(base + pom2::Via6522::VIA_ORB,
                      static_cast<uint8_t>((pb & ~0x03) | 0x03));
    // INACTIVE (BDIR=0, BC1=0) — drop the strobe so the next one is an edge.
    card.slotRomWrite(base + pom2::Via6522::VIA_ORB,
                      static_cast<uint8_t>(pb & ~0x03));
    // WRITE phase: BDIR=1, BC1=0, plus chip-select bits.
    card.slotRomWrite(base + pom2::Via6522::VIA_ORA,  data);
    card.slotRomWrite(base + pom2::Via6522::VIA_ORB,
                      static_cast<uint8_t>((pb & ~0x03) | 0x02));
    // INACTIVE again so a subsequent LATCH is an edge.
    card.slotRomWrite(base + pom2::Via6522::VIA_ORB,
                      static_cast<uint8_t>(pb & ~0x03));
}

void testViaLayout()
{
    PhasorCard card(4);
    // MB-compat mode (power-up). MAME decode (`a2mockingboard.cpp:
    // 312-337`): VIA1 at $00-$1F (reg = low4, $10-$1F mirror), VIA2 at
    // $80-$9F, everything else ($20-$7F, $A0-$FF) undecoded in BOTH
    // modes. Distinct T1 latches written to VIA1 vs VIA2 must come
    // back separate.
    card.slotRomWrite(pom2::Via6522::VIA_T1LL,        0x34);
    card.slotRomWrite(pom2::Via6522::VIA_T1LH,        0x12);
    card.slotRomWrite(0x80 + pom2::Via6522::VIA_T1LL, 0x78);
    card.slotRomWrite(0x80 + pom2::Via6522::VIA_T1LH, 0x56);
    assert(card.peekViaRegister(0, pom2::Via6522::VIA_T1LL) == 0x34);
    assert(card.peekViaRegister(0, pom2::Via6522::VIA_T1LH) == 0x12);
    assert(card.peekViaRegister(1, pom2::Via6522::VIA_T1LL) == 0x78);
    assert(card.peekViaRegister(1, pom2::Via6522::VIA_T1LH) == 0x56);
    // In-range mirror: low8 = $16 reaches VIA1 T1LL ($10-$1F window).
    card.slotRomWrite(0x10 + pom2::Via6522::VIA_T1LL, 0xAB);
    assert(card.peekViaRegister(0, pom2::Via6522::VIA_T1LL) == 0xAB);
    // $90-$9F mirrors VIA2 in MB mode.
    card.slotRomWrite(0x90 + pom2::Via6522::VIA_T1LL, 0xCD);
    assert(card.peekViaRegister(1, pom2::Via6522::VIA_T1LL) == 0xCD);
    // OUT of range: $40-$4F was a VIA1 mirror in the pre-MAME-parity
    // decode — MAME gates it off. Write dropped, read returns 0.
    card.slotRomWrite(0x40 + pom2::Via6522::VIA_T1LL, 0x99);
    assert(card.peekViaRegister(0, pom2::Via6522::VIA_T1LL) == 0xAB);
    assert(card.slotRomRead(0x40 + pom2::Via6522::VIA_T1LL) == 0x00);
    assert(card.slotRomRead(0xA0 + pom2::Via6522::VIA_T1LL) == 0x00);

    std::printf("  ok: dual-VIA register layout + MAME range-gated decode\n");
}

void testNativeViaDecode()
{
    PhasorCard card(4);
    card.deviceSelectWrite(0xD, 0);    // → PH_Phasor (native)
    assert(card.mode() == PhasorCard::PH_Phasor);

    // Native via_sel = ((off & 0x80) >> 6) | ((off & 0x10) >> 4)
    // (MAME `a2mockingboard.cpp:319,373`):
    //   $00-$0F → no VIA. Write dropped, read 0.
    card.slotRomWrite(0x00 + pom2::Via6522::VIA_T1LL, 0x11);
    assert(card.peekViaRegister(0, pom2::Via6522::VIA_T1LL) == 0xFF);
    assert(card.peekViaRegister(1, pom2::Via6522::VIA_T1LL) == 0xFF);
    assert(card.slotRomRead(0x00 + pom2::Via6522::VIA_T1LL) == 0x00);

    //   $10-$1F → VIA1 only.
    card.slotRomWrite(0x10 + pom2::Via6522::VIA_T1LL, 0x22);
    assert(card.peekViaRegister(0, pom2::Via6522::VIA_T1LL) == 0x22);
    assert(card.peekViaRegister(1, pom2::Via6522::VIA_T1LL) == 0xFF);

    //   $80-$8F → VIA2 only.
    card.slotRomWrite(0x80 + pom2::Via6522::VIA_T1LL, 0x44);
    assert(card.peekViaRegister(0, pom2::Via6522::VIA_T1LL) == 0x22);
    assert(card.peekViaRegister(1, pom2::Via6522::VIA_T1LL) == 0x44);

    //   $90-$9F → BOTH: write broadcasts...
    card.slotRomWrite(0x90 + pom2::Via6522::VIA_T1LL, 0x55);
    assert(card.peekViaRegister(0, pom2::Via6522::VIA_T1LL) == 0x55);
    assert(card.peekViaRegister(1, pom2::Via6522::VIA_T1LL) == 0x55);
    //   ...and read ORs the two bytes (MAME `ret |= via->read(...)`).
    card.slotRomWrite(0x10 + pom2::Via6522::VIA_T1LL, 0x0F);
    card.slotRomWrite(0x80 + pom2::Via6522::VIA_T1LL, 0xF0);
    assert(card.slotRomRead(0x90 + pom2::Via6522::VIA_T1LL) == 0xFF);

    std::printf("  ok: native-mode MAME via_sel decode (none/VIA1/VIA2/both)\n");
}

void testModeSoftSwitch()
{
    PhasorCard card(4);
    // Power-up = PH_Mockingboard.
    assert(card.mode() == PhasorCard::PH_Mockingboard);
    assert(card.clockScale() == 1);

    // Write to $C0(8+s)D: low4 = 0xD = 0b1101 → bit3 set (clear mode),
    // low3 = 0b101 = 5 → PH_Phasor.
    card.deviceSelectWrite(0xD, 0x00);
    assert(card.mode() == PhasorCard::PH_Phasor);
    assert(card.clockScale() == 2);

    // Write to $C0(8+s)8: low4 = 0x8 = 0b1000 → clear mode, low3 = 0
    // → PH_Mockingboard.
    card.deviceSelectWrite(0x8, 0x00);
    assert(card.mode() == PhasorCard::PH_Mockingboard);

    // Read also triggers the switch (AppleWin behaviour). Read $C0(8+s)F
    // → low4 = 0xF → clear + OR 7 → PH_EchoPlus.
    const uint8_t status = card.deviceSelectRead(0xF);
    assert(card.mode() == PhasorCard::PH_EchoPlus);
    // MAME read_c0nx returns $FF (open bus) — the mode is NOT readable
    // on real hardware, only settable by the address bits.
    assert(status == 0xFF);

    // Read $C0(8+s)0 (no bit3, no OR) keeps current mode unchanged.
    (void)card.deviceSelectRead(0x0);
    assert(card.mode() == PhasorCard::PH_EchoPlus);

    // Read $C0(8+s)5 (no bit3, OR 5) ORs bits in: 7 | 5 = 7, still EP.
    (void)card.deviceSelectRead(0x5);
    assert(card.mode() == PhasorCard::PH_EchoPlus);

    std::printf("  ok: mode soft-switch ($C0(8+s)X bit3 clears, low3 ORs)\n");
}

void testMockingboardCompatRouting()
{
    PhasorCard card(4);
    // PH_Mockingboard at reset.
    assert(card.mode() == PhasorCard::PH_Mockingboard);

    // VIA1 LATCH+WRITE to AY register 7 (mixer control), data $1F.
    // Pass chip-select bits that "would" select BOTH in Phasor mode
    // (PB3=0, PB4=0) — MB compat must ignore them and only hit AY0.
    const uint8_t pbBoth = makePb(/*bc1*/0, /*bdir*/0, /*pri*/1, /*sec*/1);
    doLatchWrite(card, /*via*/0, pbBoth, /*reg*/7, /*data*/0x1F);

    // AY0 received the write; AY1 stayed silent.
    assert(card.getAyRegister(0, 7) == 0x1F);
    assert(card.getAyRegister(1, 7) == 0x00);

    // Same for VIA2: AY2 receives, AY3 untouched.
    doLatchWrite(card, /*via*/1, pbBoth, /*reg*/8, /*data*/0x0F);
    assert(card.getAyRegister(2, 8) == 0x0F);
    assert(card.getAyRegister(3, 8) == 0x00);

    std::printf("  ok: MB-compat mode routes to primary AY only (PB3/PB4 ignored)\n");
}

void testPhasorNativeRouting()
{
    PhasorCard card(4);
    // Switch to PH_Phasor.
    card.deviceSelectWrite(0xD, 0);
    assert(card.mode() == PhasorCard::PH_Phasor);

    // VIA1 → AY0 only (PB3 low, PB4 high).
    const uint8_t pbPri = makePb(0, 0, /*pri*/1, /*sec*/0);
    doLatchWrite(card, 0, pbPri, /*reg*/0, /*data*/0xAA);
    assert(card.getAyRegister(0, 0) == 0xAA);
    assert(card.getAyRegister(1, 0) == 0x00);

    // VIA1 → AY1 only (PB3 high, PB4 low).
    const uint8_t pbSec = makePb(0, 0, /*pri*/0, /*sec*/1);
    doLatchWrite(card, 0, pbSec, /*reg*/1, /*data*/0xBB);
    assert(card.getAyRegister(0, 1) == 0x00);
    assert(card.getAyRegister(1, 1) == 0xBB);

    // VIA1 → BOTH (PB3 low, PB4 low).
    const uint8_t pbBoth = makePb(0, 0, /*pri*/1, /*sec*/1);
    doLatchWrite(card, 0, pbBoth, /*reg*/2, /*data*/0xCC);
    assert(card.getAyRegister(0, 2) == 0xCC);
    assert(card.getAyRegister(1, 2) == 0xCC);

    // VIA1 → NONE (PB3 high, PB4 high).
    const uint8_t pbNone = makePb(0, 0, /*pri*/0, /*sec*/0);
    doLatchWrite(card, 0, pbNone, /*reg*/3, /*data*/0xDD);
    assert(card.getAyRegister(0, 3) == 0x00);
    assert(card.getAyRegister(1, 3) == 0x00);

    // VIA2 → AY3 only (secondary).
    doLatchWrite(card, 1, pbSec, /*reg*/4, /*data*/0xEE);
    assert(card.getAyRegister(2, 4) == 0x00);
    assert(card.getAyRegister(3, 4) == 0xEE);

    std::printf("  ok: Phasor-native chip-select PB3/PB4 active-low decode\n");
}

void testAudioSynth4Chips()
{
    PhasorCard card(4);
    card.setSampleRate(44100);
    card.setVolume(1.0f);
    card.setMuted(false);
    card.deviceSelectWrite(0xD, 0);    // → PH_Phasor (4 active chips)

    // Drive each of the 4 chips with a tone via the appropriate VIA +
    // chip-select. AY0 + AY1 reached via VIA1 with primary / secondary
    // selects; AY2 + AY3 via VIA2 similarly.
    auto configChannelA = [&](int viaIdx, bool selPrimary,
                              uint8_t periodLo, uint8_t periodHi)
    {
        const uint8_t pb = makePb(0, 0, selPrimary, !selPrimary);
        // R0/R1 = channel A period (12-bit).
        doLatchWrite(card, viaIdx, pb, /*reg*/0, periodLo);
        doLatchWrite(card, viaIdx, pb, /*reg*/1, periodHi);
        // R7 = mixer: enable tone A only (bit 0 = 0), tone B/C off,
        // all noise off. R7 = 0b00111110 = 0x3E.
        doLatchWrite(card, viaIdx, pb, /*reg*/7, 0x3E);
        // R8 = chan A amplitude = 15.
        doLatchWrite(card, viaIdx, pb, /*reg*/8, 0x0F);
    };
    // Different periods per chip so each plays a distinct pitch.
    configChannelA(0, /*pri*/true,  0x00, 0x01);   // AY0: period $100
    configChannelA(0, /*pri*/false, 0x80, 0x01);   // AY1: period $180
    configChannelA(1, /*pri*/true,  0x00, 0x02);   // AY2: period $200
    configChannelA(1, /*pri*/false, 0x80, 0x02);   // AY3: period $280

    constexpr int N = 8192;
    std::vector<float> buf(N);
    AudioSource* src = card.audioSource();
    assert(src);
    src->fillAudioBuffer(buf.data(), N);

    // RMS energy must be non-trivial — all 4 chips contribute amplitude.
    double sumSq = 0.0;
    float vmin = +1e9f, vmax = -1e9f;
    for (float s : buf) {
        sumSq += static_cast<double>(s) * s;
        if (s < vmin) vmin = s;
        if (s > vmax) vmax = s;
    }
    const double rms = std::sqrt(sumSq / N);
    std::printf("  4-AY Phasor mix rms=%.4f vmin=%.4f vmax=%.4f\n",
                rms, vmin, vmax);
    // 4 chips × amp 15 × peak 1.0 / 12 (mix div) → ≥ 0.10 RMS easily.
    // Set the floor low enough to survive minor synth tweaks.
    assert(rms > 0.05);

    // Mute path silences everything.
    card.setMuted(true);
    src->fillAudioBuffer(buf.data(), N);
    sumSq = 0.0;
    for (float s : buf) sumSq += static_cast<double>(s) * s;
    assert(sumSq == 0.0);

    std::printf("  ok: 4-AY mix produces non-silent waveform; mute path silences\n");
}

// Estimate dominant tone frequency from a buffer via zero-crossings.
// Mockingboard / Phasor square-wave outputs are DC-offset (not centred);
// count crossings of the running mean for a robust estimate.
double estimateFreqHz(const std::vector<float>& buf, uint32_t sr)
{
    double mean = 0;
    for (float s : buf) mean += s;
    mean /= buf.size();
    int crossings = 0;
    bool above = (buf[0] - mean) >= 0.0f;
    for (size_t i = 1; i < buf.size(); ++i) {
        const bool now = (buf[i] - mean) >= 0.0f;
        if (now != above) ++crossings;
        above = now;
    }
    // 2 crossings per full cycle.
    return (crossings * 0.5) * sr / buf.size();
}

void testClockScaleDoublesPitch()
{
    // Same register values in MB vs Phasor mode must produce 2x pitch
    // in Phasor mode (clockScale=2 doubles the AY input clock).
    constexpr int N = 16384;
    constexpr uint32_t SR = 44100;

    auto playOneAyToneAndMeasure = [](PhasorCard::Mode mode) -> double {
        PhasorCard card(4);
        card.setSampleRate(SR);
        card.setVolume(1.0f);
        card.setMuted(false);
        // Switch into target mode. PH_Mockingboard is the power-up
        // state, so we only need to write for PH_Phasor.
        if (mode == PhasorCard::PH_Phasor) card.deviceSelectWrite(0xD, 0);
        assert(card.mode() == mode);

        // Drive AY0 via VIA1 primary. In MB-compat mode chip-select bits
        // are ignored so primary always wins → AY0; in Phasor mode we
        // explicitly select primary for the same target chip.
        const uint8_t pb = makePb(0, 0, /*pri*/true, /*sec*/false);
        // Period = $200 → tone freq at clock/16/$200 = ~125 Hz @ MB rate,
        // ~250 Hz @ Phasor-native rate.
        doLatchWrite(card, 0, pb, 0, 0x00);
        doLatchWrite(card, 0, pb, 1, 0x02);
        doLatchWrite(card, 0, pb, 7, 0x3E);   // enable tone A only
        doLatchWrite(card, 0, pb, 8, 0x0F);   // amp 15

        std::vector<float> buf(N);
        card.audioSource()->fillAudioBuffer(buf.data(), N);
        return estimateFreqHz(buf, SR);
    };

    const double mbFreq = playOneAyToneAndMeasure(PhasorCard::PH_Mockingboard);
    const double phFreq = playOneAyToneAndMeasure(PhasorCard::PH_Phasor);

    std::printf("  tone @ period $200: MB=%.1f Hz  Phasor=%.1f Hz  ratio=%.3f\n",
                mbFreq, phFreq, (mbFreq > 0 ? phFreq / mbFreq : 0));
    // Both must be in the audible range and the ratio must be roughly 2.
    // Allow a generous tolerance (±15%) — zero-crossing estimation is
    // noisy on a single-channel square wave and the AY counter's
    // sub-tick aliasing wobbles the apparent freq.
    assert(mbFreq > 50.0 && mbFreq < 200.0);
    assert(phFreq > 100.0 && phFreq < 400.0);
    assert(phFreq / mbFreq > 1.7 && phFreq / mbFreq < 2.3);

    std::printf("  ok: clockScale() in Phasor mode doubles AY tone frequency\n");
}

void testTelemetry()
{
    PhasorCard card(4);
    card.deviceSelectWrite(0xD, 0);    // → PH_Phasor

    const uint8_t pbPri = makePb(0, 0, /*pri*/1, /*sec*/0);
    doLatchWrite(card, 0, pbPri, /*reg*/5, /*data*/0x11);
    doLatchWrite(card, 0, pbPri, /*reg*/6, /*data*/0x22);

    // Two writes landed on AY0 (regs 5, 6), zero on AY1/AY2/AY3.
    assert(card.getAyWriteCount(0) == 2);
    assert(card.getAyWriteCount(1) == 0);
    assert(card.getAyWriteCount(2) == 0);
    assert(card.getAyWriteCount(3) == 0);
    // VIA0 saw a bunch of MMIO writes (DDR setup + 6 strobes per
    // doLatchWrite × 2 = 12+), VIA1 saw none.
    assert(card.getViaWriteCount(0) > 0);
    assert(card.getViaWriteCount(1) == 0);

    std::printf("  ok: per-AY + per-VIA telemetry counters\n");
}

// Contract: onReset() BUMPS ayResetCount_, it does not zero it.
//
// The audio thread re-seeds a chip's tone/noise/envelope generators only
// when this counter CHANGES against its own `lastSeenResetCount`. The
// pre-fix onReset assigned 0, which is a no-op on every reset that is not
// preceded by an AY reset strobe — a second F12, or a cold boot on a driver
// that never strobes PB2 low. The CPU-side ay_[i]->reset() then cleared the
// register bank while the audio thread kept the old tone, and the card held
// its last note through the reset: verbatim the symptom
// MockingboardCard::onReset was fixed for, reintroduced in the sibling.
//
// Asserted from a counter that is ALREADY 0, because that is precisely the
// state in which zeroing was invisible.
void testResetBumpsAyResetCount()
{
    // The constructor resets the card, so a fresh one is already at 1 with
    // the fix and would sit at 0 forever without it. What the audio thread
    // needs is not a particular value but a CHANGE on every reset, so assert
    // exactly that — and assert it twice, since the pre-fix assignment was
    // stable at 0 and would satisfy no step of this.
    PhasorCard card(4);
    std::uint32_t before[4];
    for (int chip = 0; chip < 4; ++chip) {
        before[chip] = card.getAyResetCount(chip);
        assert(before[chip] != 0);       // ctor's own onReset already counted
    }

    card.onReset();
    for (int chip = 0; chip < 4; ++chip)
        assert(card.getAyResetCount(chip) == before[chip] + 1);

    // Monotonic across further resets: a second reset with no AY strobe in
    // between is exactly the case the pre-fix zeroing made invisible.
    card.onReset();
    for (int chip = 0; chip < 4; ++chip)
        assert(card.getAyResetCount(chip) == before[chip] + 2);

    std::printf("  ok: onReset bumps ayResetCount (never zeroes it)\n");
}

// Regression for the end-of-step overshoot. Memory::advanceCycles folds
// the slice into cycleCounter BEFORE dispatching to the card, yet
// M6502::step() still holds cpu->cycles == that slice, so
// getCycleCountNow() (= cycleCounter + cpu->cycles) overshoots "now" by
// one instruction. Pre-fix PhasorCard::advanceCycles compensated with
// syncToCpuCycleAt(now - cycles) but THEN also called via_->advance(cycles)
// a second time, double-charging T1 by one slice per call. Drive the
// same elapsed cycles two ways and require the T1 counter to match.
void testNoEndOfStepOvershoot()
{
    constexpr uint16_t kLatch  = 1000;
    constexpr uint64_t kCycles = 40;

    auto t1Counter = [](PhasorCard& c) -> uint16_t {
        return static_cast<uint16_t>(c.peekViaRegister(0, 0x04)) |
               static_cast<uint16_t>(c.peekViaRegister(0, 0x05) << 8);
    };
    auto armT1 = [](PhasorCard& c, uint16_t latch) {
        c.slotRomWrite(pom2::Via6522::VIA_T1LL,
                       static_cast<uint8_t>(latch & 0xFF));
        c.slotRomWrite(pom2::Via6522::VIA_T1LH,
                       static_cast<uint8_t>((latch >> 8) & 0xFF));
        c.slotRomWrite(pom2::Via6522::VIA_ACR, 0x40);    // continuous
        c.slotRomWrite(pom2::Via6522::VIA_T1CH,
                       static_cast<uint8_t>((latch >> 8) & 0xFF));
        c.slotRomWrite(pom2::Via6522::VIA_IER, 0xC0);    // enable T1
    };

    // Reference: legacy batched advance (no CPU back-pointer).
    PhasorCard ref(4);
    ref.onReset();
    armT1(ref, kLatch);
    ref.advanceCycles(static_cast<int>(kCycles));
    const uint16_t refCtr = t1Counter(ref);

    // CPU-driven: same elapsed cycles via real M6502 NOP stepping through
    // the Memory::advanceCycles -> slotBus -> card path.
    Memory mem;
    M6502  cpu(&mem);
    auto cardp = std::make_unique<PhasorCard>(4);
    cardp->setCpu(&cpu);
    PhasorCard* card = cardp.get();
    mem.slotBus().plug(4, std::move(cardp));
    cpu.hardReset();
    mem.slotBus().reset();
    for (uint16_t a = 0x0300; a < 0x0360; ++a) mem.memWrite(a, 0xEA); // NOPs
    armT1(*card, kLatch);
    cpu.setProgramCounter(0x0300);
    const uint64_t start = mem.getCycleCounter();
    while (mem.getCycleCounter() - start < kCycles) cpu.step();
    const uint64_t elapsed = mem.getCycleCounter() - start;
    const uint16_t cpuCtr = t1Counter(*card);

    if (elapsed != kCycles) {
        std::fprintf(stderr, "Phasor overshoot test: NOP stepping landed at "
                     "%llu cycles (want %llu)\n",
                     (unsigned long long)elapsed,
                     (unsigned long long)kCycles);
        std::abort();
    }
    // The two paths differ by exactly ONE count, and only because the arm
    // itself is an MMIO access on the CPU-driven card: `syncToCpuCycle` adds
    // +1 for the instruction's in-flight data cycle (see testT1MmioDataCycle),
    // so T1 starts one cycle later there and has one fewer cycle to run. The
    // reference card has no CPU and no MMIO clock at all. The regression this
    // test exists for — a second `via_->advance(cycles)` after the corrected
    // sync — double-charges a WHOLE slice (40 counts here), so pinning the
    // offset at exactly 1 still catches it.
    if (cpuCtr != static_cast<uint16_t>(refCtr + 1)) {
        std::fprintf(stderr, "Phasor end-of-step overshoot: CPU-driven T1=%u "
                     "!= batch-driven+1=%u after %llu cycles\n",
                     cpuCtr, static_cast<unsigned>(refCtr + 1),
                     (unsigned long long)elapsed);
        std::abort();
    }
    std::printf("  ok: CPU-driven advance matches batch +1 MMIO data cycle "
                "(T1=%u, %llu cycles)\n",
                cpuCtr, (unsigned long long)elapsed);
}

// ── MMIO access lands on the instruction's DATA cycle (the `+1`) ────────
//
// Port of mockingboard_t1_irq_phase to the Phasor, which runs the same two
// 6522s behind the same lazy sync. `getCycleCountNow()` = cycleCounter +
// cpu.cycles, and cpu.cycles does NOT yet count the in-flight data cycle of
// the access, so without the `+1` in PhasorCard::syncToCpuCycle every MMIO
// read samples its VIA counter one too high — the OLDSKOOL FORT ET VERT
// phase-wrap class of bug ($00 -> $FF), on a Phasor instead of a
// Mockingboard. Reverting the +1 fails here.
void testT1MmioDataCycle()
{
    Memory mem;
    M6502  cpu(&mem);
    auto cardp = std::make_unique<PhasorCard>(4);
    cardp->setCpu(&cpu);
    PhasorCard* card = cardp.get();
    mem.slotBus().plug(4, std::move(cardp));
    cpu.hardReset();
    mem.slotBus().reset();

    // Program at $0300 — the Phasor boots in MB-compat mode, so VIA1 sits
    // at $C400-$C41F exactly where a Mockingboard's does.
    uint16_t p = 0x0300;
    auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) mem.memWrite(p++, b);
    };
    emit({0xA9, 0x00, 0x8D, 0x06, 0xC4});   // LDA #$00 ; STA $C406 (T1LL)
    emit({0xA9, 0x20, 0x8D, 0x07, 0xC4});   // LDA #$20 ; STA $C407 (T1LH)
    emit({0xA9, 0x40, 0x8D, 0x0B, 0xC4});   // LDA #$40 ; STA $C40B (ACR cont.)
    emit({0xA9, 0x20, 0x8D, 0x05, 0xC4});   // LDA #$20 ; STA $C405 (T1CH arms)
    for (int i = 0; i < 4; ++i) emit({0xEA});                // settle
    const uint16_t ldaPc = p;
    emit({0xAD, 0x04, 0xC4});               // LDA $C404 (the phase read)
    emit({0xEA});

    cpu.setProgramCounter(0x0300);
    while (cpu.getProgramCounter() != ldaPc) cpu.step();

    // Raw counter as of the pre-instruction cycle (peek: no read-back
    // bias, no sync).
    const uint16_t peekBefore =
        static_cast<uint16_t>(card->peekViaRegister(0, 0x04)) |
        static_cast<uint16_t>(card->peekViaRegister(0, 0x05) << 8);

    cpu.step();                                  // execute LDA $C404
    const uint8_t got = cpu.getAccumulator();

    // LDA abs = 4 cycles; the data-cycle sync lands on cycleCounter+4, then
    // the VIA applies its -1 read-back bias: got == (peekBefore - 5) low.
    const uint8_t expected = static_cast<uint8_t>((peekBefore - 5) & 0xFF);
    if (got != expected) {
        std::fprintf(stderr,
            "Phasor T1 MMIO phase: LDA $C404 read $%02X, expected $%02X "
            "(peekBefore=$%04X). The access-cycle +1 sync is missing.\n",
            got, expected, peekBefore);
        std::abort();
    }
    std::printf("  ok: $C404 read reflects the access data cycle "
                "(got $%02X = peek $%04X - 5)\n", got, peekBefore);
}

// A zero or negative slice must be a no-op. On the CPU-attached path
// `advanceCycles` computes `getCycleCountNow() - static_cast<uint64_t>
// (cycles)`, so a NEGATIVE slice becomes a huge unsigned subtrahend and
// the sync target jumps |cycles| cycles into the FUTURE: both VIAs run
// through however many T1 periods that is, and `lastSyncCycle_` is pinned
// ahead of the CPU so every later sync early-outs until the machine
// catches up. `Via6522::advance` has its own `cycles <= 0` guard, so the
// headless path never showed this — the card-level guard is what the
// CPU path needs.
void testAdvanceCyclesGuard()
{
    auto t1 = [](PhasorCard& c) -> uint16_t {
        return static_cast<uint16_t>(c.peekViaRegister(0, 0x04)) |
               static_cast<uint16_t>(c.peekViaRegister(0, 0x05) << 8);
    };

    Memory mem;
    M6502  cpu(&mem);
    auto cardp = std::make_unique<PhasorCard>(4);
    cardp->setCpu(&cpu);
    PhasorCard* card = cardp.get();
    mem.slotBus().plug(4, std::move(cardp));
    cpu.hardReset();
    mem.slotBus().reset();

    card->slotRomWrite(pom2::Via6522::VIA_T1LL, 0x00);
    card->slotRomWrite(pom2::Via6522::VIA_T1LH, 0x20);
    card->slotRomWrite(pom2::Via6522::VIA_ACR,  0x40);   // continuous
    card->slotRomWrite(pom2::Via6522::VIA_T1CH, 0x20);   // arm

    const uint16_t before = t1(*card);
    card->advanceCycles(0);
    assert(t1(*card) == before);
    card->advanceCycles(-1);
    assert(t1(*card) == before);
    card->advanceCycles(-100000);
    if (t1(*card) != before) {
        std::fprintf(stderr,
            "Phasor advanceCycles guard: a negative slice moved T1 from %u "
            "to %u (the uint64 wrap)\n", before, t1(*card));
        std::abort();
    }
    // Still ticks normally afterwards (the headless fall-back path).
    PhasorCard plain(4);
    plain.slotRomWrite(pom2::Via6522::VIA_T1LL, 0x00);
    plain.slotRomWrite(pom2::Via6522::VIA_T1LH, 0x20);
    plain.slotRomWrite(pom2::Via6522::VIA_ACR,  0x40);
    plain.slotRomWrite(pom2::Via6522::VIA_T1CH, 0x20);
    const uint16_t plainBefore = t1(plain);
    plain.advanceCycles(-5);
    assert(t1(plain) == plainBefore);
    plain.advanceCycles(10);
    assert(t1(plain) == static_cast<uint16_t>(plainBefore - 10));

    std::printf("  ok: advanceCycles(<=0) is a no-op, timers survive it\n");
}

// setCpuClock must retune the four AYs' input clock (their pin-22 CLOCK is
// the slot's phase-0 line, so a PAL machine really does run them slower).
// Before this the synth derived its step rate from the compile-time NTSC
// constant and PAL music came out 0.7 % sharp. A 0.7 % delta is below the
// zero-crossing estimator's noise, so drive the mechanism with a 2x clock
// and require a 2x tone — the same shape as testClockScaleDoublesPitch.
void testSetCpuClockRetunesAy()
{
    constexpr int N = 16384;
    constexpr uint32_t SR = 44100;

    auto measureAt = [](double cpuHz) -> double {
        PhasorCard card(4);
        card.setSampleRate(SR);
        card.setVolume(1.0f);
        card.setMuted(false);
        card.setCpuClock(cpuHz);
        const uint8_t pb = makePb(0, 0, /*pri*/true, /*sec*/false);
        doLatchWrite(card, 0, pb, 0, 0x00);
        doLatchWrite(card, 0, pb, 1, 0x02);   // period $200
        doLatchWrite(card, 0, pb, 7, 0x3E);   // tone A only
        doLatchWrite(card, 0, pb, 8, 0x0F);   // amp 15
        std::vector<float> buf(N);
        card.audioSource()->fillAudioBuffer(buf.data(), N);
        return estimateFreqHz(buf, SR);
    };

    const double base = static_cast<double>(POM2_CPU_CLOCK_HZ);
    const double f1 = measureAt(base);
    const double f2 = measureAt(base * 2.0);
    std::printf("  tone @ period $200: %.1f Hz @1x clock, %.1f Hz @2x "
                "(ratio %.3f)\n", f1, f2, (f1 > 0 ? f2 / f1 : 0));
    assert(f1 > 50.0 && f1 < 200.0);
    assert(f2 / f1 > 1.7 && f2 / f1 < 2.3);

    std::printf("  ok: setCpuClock retunes the AY clock (PAL follows)\n");
}

// The AY data bus is the VIA's port-A PINS: a bit whose DDRA says "input"
// is undriven and floats HIGH (MAME `via6522.cpp output_pa()` hands its
// handler `(m_out_a & m_ddr_a) | ~m_ddr_a`). Masking with ddrA alone fed
// the chip zeros for every undriven bit.
void testAyBusUndrivenBitsFloatHigh()
{
    PhasorCard card(4);
    // DDRA = all output, latch register 7 (mixer) with a known value so a
    // later strobe that re-reads the bus is observable.
    card.slotRomWrite(pom2::Via6522::VIA_DDRA, 0xFF);
    card.slotRomWrite(pom2::Via6522::VIA_DDRB, 0xFF);
    const uint8_t pb = makePb(0, 0, /*pri*/true, /*sec*/false);
    card.slotRomWrite(pom2::Via6522::VIA_ORA, 0x07);          // reg 7
    card.slotRomWrite(pom2::Via6522::VIA_ORB,
                      static_cast<uint8_t>((pb & ~0x03) | 0x03));  // LATCH
    card.slotRomWrite(pom2::Via6522::VIA_ORB,
                      static_cast<uint8_t>(pb & ~0x03));           // inactive

    // Now the driver releases port A (DDRA = 0) — every pin floats high —
    // and strobes a WRITE. The chip must see $FF, not $00.
    card.slotRomWrite(pom2::Via6522::VIA_DDRA, 0x00);
    card.slotRomWrite(pom2::Via6522::VIA_ORB,
                      static_cast<uint8_t>((pb & ~0x03) | 0x02));  // WRITE
    card.slotRomWrite(pom2::Via6522::VIA_ORB,
                      static_cast<uint8_t>(pb & ~0x03));

    const uint8_t r7 = card.getAyRegister(0, 7);
    if (r7 != 0xFF) {
        std::fprintf(stderr,
            "Phasor AY bus: undriven port-A bits delivered $%02X, expected "
            "$FF (MAME output_pa = (out & ddr) | ~ddr)\n", r7);
        std::abort();
    }
    std::printf("  ok: undriven port-A bits reach the AY as 1s\n");
}

} // namespace

int main()
{
    std::printf("PhasorCard smoke test\n");
    testViaLayout();
    testNativeViaDecode();
    testModeSoftSwitch();
    testMockingboardCompatRouting();
    testPhasorNativeRouting();
    testAudioSynth4Chips();
    testClockScaleDoublesPitch();
    testTelemetry();
    testNoEndOfStepOvershoot();
    testResetBumpsAyResetCount();
    testT1MmioDataCycle();
    testAdvanceCyclesGuard();
    testSetCpuClockRetunesAy();
    testAyBusUndrivenBitsFloatHigh();
    std::printf("PASS\n");
    return 0;
}
