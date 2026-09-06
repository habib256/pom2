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

// Mockingboard smoke test — pins the four behaviours that any music
// driver running on a real Apple II Mockingboard A/C depends on:
//
//   1. Slot ROM address decode: writes to $Cn0X reach VIA #1, writes to
//      $Cn8X reach VIA #2.
//   2. VIA → AY register addressing. The canonical command sequence
//      LATCH ($Cn00 ← addr ; $Cn02 ← 0x07) → WRITE ($Cn00 ← data ;
//      $Cn02 ← 0x06) → INACTIVE deposits `data` in the addressed AY
//      register. We assert this end-to-end through `slotRomWrite`.
//   3. VIA Timer 1 IRQ generation. Continuous mode plus IER = $C0
//      (T1 enabled) must drive `isIrqAsserted()` true after the first
//      timer underflow, and the IRQ stays asserted until $Cn04 (T1CL)
//      read clears IFR.T1.
//   4. AY synthesis produces a non-silent waveform when a tone period
//      is set and the channel + amplitude are enabled.
//
// Self-contained: doesn't touch Memory or M6502, drives the card's
// SlotPeripheral interface directly. Mirrors the structure of
// `clock_card_smoke_test.cpp`.

#include "Mockingboard.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

// Convenience wrappers — the card's slot-ROM API is byte-indexed within
// a 256-byte window, but writers think in $CnXX addresses. The card
// doesn't care which slot it's in (it only sees the low byte), so we
// just feed the offset directly.

void writeVia(MockingboardCard& card, int chip, uint8_t reg, uint8_t v)
{
    const uint8_t base = (chip == 0) ? 0x00 : 0x80;
    card.slotRomWrite(static_cast<uint8_t>(base | (reg & 0x0F)), v);
}

uint8_t readVia(MockingboardCard& card, int chip, uint8_t reg)
{
    const uint8_t base = (chip == 0) ? 0x00 : 0x80;
    return card.slotRomRead(static_cast<uint8_t>(base | (reg & 0x0F)));
}

// AY command codes carried on VIA Port B bits 0..2, per the Sweet
// Microsystems Mockingboard A/C schematic (and AppleWin
// `Mockingboard.cpp:193`):
//   bit 0 = BC1
//   bit 1 = BDIR
//   bit 2 = /RESET (active low) — 1 = chip running
constexpr uint8_t kPbInactive = 0x04;        // {BC1=0, BDIR=0, /RESET=1}
constexpr uint8_t kPbWrite    = 0x06;        // {BC1=0, BDIR=1, /RESET=1}
constexpr uint8_t kPbLatch    = 0x07;        // {BC1=1, BDIR=1, /RESET=1}

// Drive the canonical 6522→AY bus sequence to set AY register `reg` to
// `value` on chip 0 or 1. Mirrors the AppleWin / WozTracker idiom.
void ayWrite(MockingboardCard& card, int chip, uint8_t reg, uint8_t v)
{
    // Configure DDRs once: PA all output, PB lower 3 bits output. (A
    // real driver does this once at init; we do it on every call here
    // so the test functions are self-contained.)
    writeVia(card, chip, 0x03, 0xFF);   // DDRA = $FF
    writeVia(card, chip, 0x02, 0x07);   // DDRB = $07 (PB0..PB2 output)

    // Latch register address.
    writeVia(card, chip, 0x01, reg);          // ORA = reg
    writeVia(card, chip, 0x00, kPbLatch);     // ORB = LATCH
    writeVia(card, chip, 0x00, kPbInactive);  // ORB = INACTIVE
    // Write data.
    writeVia(card, chip, 0x01, v);            // ORA = v
    writeVia(card, chip, 0x00, kPbWrite);     // ORB = WRITE
    writeVia(card, chip, 0x00, kPbInactive);  // ORB = INACTIVE
}

// ─── Test 1: address decoding ────────────────────────────────────────────
void testAddressDecode()
{
    MockingboardCard card(4);
    // Set DDRA = $FF on both VIAs so reads of ORA reflect the latch
    // (with DDR=0, input lines float high and the read is always $FF).
    card.slotRomWrite(0x03, 0xFF);     // VIA1 DDRA = $FF
    card.slotRomWrite(0x83, 0xFF);     // VIA2 DDRA = $FF
    // Write a sentinel to VIA1 ORA; VIA2 ORA must be unaffected.
    card.slotRomWrite(0x01, 0xAA);     // VIA1 ORA
    card.slotRomWrite(0x81, 0x55);     // VIA2 ORA
    assert(card.peekViaRegister(0, 0x01) == 0xAA);
    assert(card.peekViaRegister(1, 0x01) == 0x55);
    // Mirror behaviour: $Cn10 should also decode to VIA1 (low 4 bits =
    // reg, high bit alone selects chip).
    card.slotRomWrite(0x11, 0x33);
    assert(card.peekViaRegister(0, 0x01) == 0x33);
    // $Cn90 mirrors VIA2.
    card.slotRomWrite(0x91, 0x44);
    assert(card.peekViaRegister(1, 0x01) == 0x44);
}

// ─── Test 2: AY register write via VIA control bus ────────────────────────
void testAyRegisterWrite()
{
    MockingboardCard card(4);
    // Drive the standard sequence: AY chip 0, R0 (ch A tone period low) ← $5A.
    ayWrite(card, 0, /*reg=*/0, /*v=*/0x5A);
    assert(card.getAyRegister(0, 0) == 0x5A);

    // R7 (mixer) ← $38 (tone enabled on all 3 chans, noise off).
    ayWrite(card, 0, /*reg=*/7, /*v=*/0x38);
    assert(card.getAyRegister(0, 7) == 0x38);

    // Different chip / different register.
    ayWrite(card, 1, /*reg=*/8, /*v=*/0x0F);     // chan A amplitude max
    assert(card.getAyRegister(1, 8) == 0x0F);

    // Verify the /RESET line zeros the AY: write ORB with PB2=0
    // (/RESET asserted) — a real driver typically pulses PB=$00
    // during init.
    writeVia(card, 0, 0x00, 0x00);    // ORB = 0 → PB2=0 → /RESET asserted
    assert(card.getAyRegister(0, 0) == 0);
    assert(card.getAyRegister(0, 7) == 0);
}

// ─── Test 3: VIA T1 IRQ generation in continuous mode ────────────────────
void testT1IrqContinuous()
{
    MockingboardCard card(4);
    assert(!card.isIrqAsserted());

    // Set T1 latch to 100 cycles, ACR = $40 (T1 continuous, no PB7),
    // IER = $C0 (set bit 7 + bit 6 = enable T1).
    writeVia(card, 0, 0x06, 100);     // T1LL = 100
    writeVia(card, 0, 0x07, 0);       // T1LH = 0
    writeVia(card, 0, 0x0B, 0x40);    // ACR = continuous
    writeVia(card, 0, 0x05, 0);       // T1CH = 0 (also xfers latch → counter)
    writeVia(card, 0, 0x04, 100);     // T1CL = 100
    writeVia(card, 0, 0x07, 0);       // T1CH must be re-written to actually
    writeVia(card, 0, 0x05, 0);       // start (T1CH write transfers latch).
    writeVia(card, 0, 0x0E, 0xC0);    // IER = enable T1

    // Counter is now ~100. Walk past the underflow.
    card.advanceCycles(150);
    assert(card.isIrqAsserted());
    assert((card.peekViaRegister(0, 0x0D) & 0x40) != 0);  // IFR.T1 set

    // Reading T1CL must clear IFR.T1 and drop the IRQ line.
    (void)readVia(card, 0, 0x04);
    assert((card.peekViaRegister(0, 0x0D) & 0x40) == 0);
    assert(!card.isIrqAsserted());

    // In continuous mode the timer should fire again after another period.
    card.advanceCycles(200);
    assert(card.isIrqAsserted());
}

// ─── Test 4: VIA T1 one-shot fires once and stays low ────────────────────
void testT1IrqOneShot()
{
    MockingboardCard card(4);
    writeVia(card, 0, 0x06, 50);      // T1LL = 50
    writeVia(card, 0, 0x07, 0);       // T1LH = 0
    writeVia(card, 0, 0x0B, 0x00);    // ACR = one-shot
    writeVia(card, 0, 0x05, 0);       // T1CH = 0 → load + start
    writeVia(card, 0, 0x0E, 0xC0);    // IER enable T1

    card.advanceCycles(80);
    assert(card.isIrqAsserted());
    // Clear the flag.
    (void)readVia(card, 0, 0x04);
    assert(!card.isIrqAsserted());
    // One-shot: even after another full period, no fire.
    card.advanceCycles(500);
    assert(!card.isIrqAsserted());
}

// ─── Test 5: AY tone synthesis is non-silent when configured ─────────────
void testAyAudioSynthesis()
{
    MockingboardCard card(4);
    card.setSampleRate(44100);
    card.setVolume(1.0f);
    card.setMuted(false);

    // Configure chip 0, channel A: period 0x100 (~250 Hz), max amplitude,
    // mixer enables tone A only (R7 = 0x3E → tone B/C off, all noise off,
    // tone A on). R7 active-low: bit 0 = 0 enables tone A.
    ayWrite(card, 0, 0,  0x00);    // R0 (period low)
    ayWrite(card, 0, 1,  0x01);    // R1 (period high, 12-bit period = 0x100)
    ayWrite(card, 0, 7,  0x3E);    // R7 mixer
    ayWrite(card, 0, 8,  0x0F);    // R8 channel A amplitude = 15

    // Pull a buffer of audio. Expect a non-silent square-ish waveform.
    constexpr int N = 4096;
    std::vector<float> buf(N);
    AudioSource* src = card.audioSource();
    assert(src);
    src->fillAudioBuffer(buf.data(), N);

    // RMS energy must be appreciably above zero. Silence would give 0.
    double sumSq = 0.0;
    float  vmin = +1e9f, vmax = -1e9f;
    for (float s : buf) {
        sumSq += static_cast<double>(s) * s;
        if (s < vmin) vmin = s;
        if (s > vmax) vmax = s;
    }
    const double rms = std::sqrt(sumSq / N);
    std::printf("  AY ch-A tone @ period $0100: rms=%.4f vmin=%.4f vmax=%.4f\n",
                rms, vmin, vmax);
    // Expect non-trivial energy. The exact value depends on the volume
    // table + the per-sample increment; at amp 15 with one channel
    // active the divide-by-6 in fillAudioBuffer normalises this to
    // ~0.16 mean amplitude.
    assert(rms > 0.05);
    // Mute path silences output completely.
    card.setMuted(true);
    src->fillAudioBuffer(buf.data(), N);
    sumSq = 0.0;
    for (float s : buf) sumSq += static_cast<double>(s) * s;
    assert(sumSq == 0.0);
}

// ─── Test 5b: envelope restarts on a same-value R13 write ────────────────
void testEnvelopeRetriggerOnSameShape()
{
    MockingboardCard card(4);
    card.setSampleRate(44100);
    card.setVolume(1.0f);
    card.setMuted(false);

    // Channel A amplitude = envelope mode (R8 bit4); disable tone+noise so
    // the channel emits only the envelope level. The Mockingboard output is
    // AC-coupled (DC blocked, like the real card's output cap), so a steady
    // level is inaudible — what shows up is the ramp TRANSIENT. Shape 0
    // (\___): ramp 15→0 once, then HOLD at 0. Fast period so the ramp
    // completes well inside one 4096-sample buffer.
    ayWrite(card, 0, 7,  0x3F);    // R7: all tone + noise disabled
    ayWrite(card, 0, 8,  0x10);    // R8: chan A use-envelope
    ayWrite(card, 0, 11, 0x01);    // R11: envelope period low
    ayWrite(card, 0, 12, 0x00);    // R12: envelope period high
    ayWrite(card, 0, 13, 0x00);    // R13: shape 0

    AudioSource* src = card.audioSource();
    constexpr int N = 4096;
    std::vector<float> buf(N);

    auto rms = [&](int from, int to) {
        double s = 0; for (int i = from; i < to; ++i) s += double(buf[i]) * buf[i];
        return std::sqrt(s / (to - from));
    };

    // Buffer 1: envelope ramps down (a transient) then holds at 0 (silence).
    src->fillAudioBuffer(buf.data(), N);
    const double buf1Head = rms(0, N / 4);
    assert(buf1Head > 0.002);            // initial ramp produced a transient
    assert(rms(3 * N / 4, N) < 0.0005);  // then held at 0 (DC → silence)

    // Re-write R13 with the SAME shape value. A real AY-3-8910 restarts the
    // envelope on ANY R13 write; the bug only restarted on a changed value
    // (so buffer 2 would stay held at 0).
    ayWrite(card, 0, 13, 0x00);

    // Buffer 2: the retrigger must reproduce the initial ramp transient.
    src->fillAudioBuffer(buf.data(), N);
    const double buf2Head = rms(0, N / 4);
    assert(buf2Head > 0.5 * buf1Head);   // retriggered (bug → ~0)

    std::printf("  ok: envelope retriggers on same-value R13 write\n");
}

// ─── Test 6: Reset clears VIA + AY state ─────────────────────────────────
void testReset()
{
    MockingboardCard card(4);
    ayWrite(card, 0, 8, 0x0F);
    assert(card.getAyRegister(0, 8) == 0x0F);

    writeVia(card, 0, 0x06, 100);
    writeVia(card, 0, 0x07, 0);
    writeVia(card, 0, 0x0B, 0x40);
    writeVia(card, 0, 0x05, 0);
    writeVia(card, 0, 0x0E, 0xC0);
    card.advanceCycles(200);
    assert(card.isIrqAsserted());

    card.onReset();
    assert(!card.isIrqAsserted());
    assert(card.getAyRegister(0, 8) == 0);
    assert(card.peekViaRegister(0, 0x0E) == 0x80);  // IER reads as ier|0x80
}

}  // namespace

// ─── Test 5c: AY tone counter produces the correct fundamental ──────────
void testAyToneFrequency()
{
    // Pins the AY tone COUNTER math touched by the float→integer refactor
    // (3f42efc; MAME parity ay8910.cpp:998-1015). A tone toggles every
    // `period` tone-ticks, the tick rate is kAyToneStepHz = CPU_CLOCK/8
    // (≈127.84 kHz), so the square-wave fundamental is
    // kAyToneStepHz / (2*period). Render an audible period and measure the
    // fundamental by counting upward mean-crossings — a broken counter (wrong
    // divisor, drift, off-by-one) shifts the frequency well outside tolerance.
    MockingboardCard card(4);
    card.setSampleRate(44100);
    card.setVolume(1.0f);
    card.setMuted(false);

    constexpr int period = 64;                  // 12-bit R0/R1 period
    ayWrite(card, 0, 0, period & 0xFF);         // R0 period low
    ayWrite(card, 0, 1, (period >> 8) & 0x0F);  // R1 period high
    ayWrite(card, 0, 7, 0x3E);                  // R7 mixer: tone A only
    ayWrite(card, 0, 8, 0x0F);                  // R8 chan-A amplitude = 15

    constexpr int N = 16384;
    std::vector<float> buf(N);
    AudioSource* src = card.audioSource();
    assert(src);
    src->fillAudioBuffer(buf.data(), N);

    double mean = 0.0;
    for (float s : buf) mean += s;
    mean /= N;
    int cycles = 0;                             // one upward mean-crossing per period
    for (int i = 1; i < N; ++i)
        if (buf[i - 1] <= mean && buf[i] > mean) ++cycles;

    const double sr       = 44100.0;
    const double measured = cycles * sr / N;
    // kAyToneStepHz = POM2_CPU_CLOCK_HZ(1022727)/8; fundamental = /(2*period).
    const double expected = (1022727.0 / 8.0) / (2.0 * period);   // ≈ 998.8 Hz
    std::printf("  AY tone period %d: measured=%.1f Hz expected=%.1f Hz (%d cyc)\n",
                period, measured, expected, cycles);
    assert(measured > expected * 0.94 && measured < expected * 1.06);
}

// Sound II variant — SSI263 at $Cn40..$Cn4F, A/!R wired to VIA1.CA1.
//
// Pins:
//   (1) AC variant has no SSI263 (snapshotSsi263 → false).
//   (2) SoundII variant has SSI263 + snapshotSsi263 succeeds.
//   (3) Writes to $40..$44 reach the SSI263 (verify via the snapshot
//       after a phoneme write).
//   (4) Writes to $50+ still hit VIA1 (mirror-decode unchanged for
//       the rest of the slot ROM page).
//   (5) After a phoneme runs to completion via advanceCycles, VIA1's
//       IFR.CA1 latches (PCR.0 = 0 default = negative-edge active,
//       matching the SSI263 A/!R-inverted-into-CA1 wiring).
//   (6) Slot IRQ asserts once the host enables IER.CA1.
void testSoundIIVariantSSI263()
{
    // AC variant: no SSI263.
    {
        MockingboardCard ac(4);
        assert(ac.getVariant() == MockingboardCard::Variant::AC);
        assert(!ac.hasSsi263());
        MockingboardCard::Ssi263Snap snap;
        assert(!ac.snapshotSsi263(&snap));
    }

    // Sound II variant: SSI263 present at $40..$44.
    MockingboardCard mb(4, MockingboardCard::Variant::SoundII);
    assert(mb.getVariant() == MockingboardCard::Variant::SoundII);
    assert(mb.hasSsi263());

    // SSI263 register decode: write $03 (CTTRAMP) with CTL=0, amp=15,
    // then $00 (DURPHON) to start a phoneme. The snapshot must reflect.
    mb.slotRomWrite(0x43, 0x0F);          // CTTRAMP: CTL=0, amp=15
    mb.slotRomWrite(0x42, 0xF0);          // RATEINF: rate=15 (fast)
    mb.slotRomWrite(0x40, 0xC1);          // DURPHON: mode=11, phon=1
    MockingboardCard::Ssi263Snap snap;
    assert(mb.snapshotSsi263(&snap));
    assert(snap.regs[0] == 0xC1);
    assert(snap.regs[2] == 0xF0);
    assert(snap.regs[3] == 0x0F);
    assert(snap.currentPhoneme == 1);
    assert(!snap.powerDown);
    assert(snap.phonemeRemainingCycles > 0);
    assert(snap.phonemeWriteCount == 1);

    // $50+ still hits VIA1 mirrors (low4 = 0 → ORB). Write to $50,
    // read back via VIA1 IFR-side-effect-free peek of ORB.
    mb.slotRomWrite(0x52, 0xFF);          // VIA1 DDRB = $FF
    mb.slotRomWrite(0x50, 0xA5);          // VIA1 ORB = $A5 (via mirror)
    // ORB readback observes the latched output.
    assert(mb.peekViaRegister(0, 0x00) == 0xA5);

    // After advanceCycles past the phoneme duration, A/!R fires and
    // VIA1.IFR.CA1 latches (PCR.0 = 0 at reset = negative-edge active).
    // IER.CA1 stays disabled so slot IRQ is still released.
    mb.advanceCycles(snap.phonemeRemainingCycles + 100);
    const uint8_t ifr1 = mb.peekViaRegister(0, 0x0D);     // VIA1 IFR
    assert((ifr1 & 0x02) != 0);                            // IFR.CA1 set
    assert(!mb.isIrqAsserted());

    // Enable IER.CA1 (write $82 = "set" bit 7 + CA1 bit 1). Drive
    // another phoneme so a fresh A/!R edge fires and the slot IRQ
    // line goes high.
    mb.slotRomWrite(0x0D, 0xFF);                           // clear all IFR bits
    mb.slotRomWrite(0x0E, 0x82);                           // IER set CA1
    mb.slotRomWrite(0x40, 0xC2);                           // restart phoneme
    mb.snapshotSsi263(&snap);
    mb.advanceCycles(snap.phonemeRemainingCycles + 100);
    assert(mb.isIrqAsserted());

    std::printf("Sound II variant ...... OK\n");
}

// Sound II SSI263 audio render — the card's AudioSrc must mix
// Ssi263::fillAudio() into its output (same pattern as
// EchoPlusCard::AudioSrc). Pre-fix, fillAudioBuffer synthesised only the
// two AYs and Sound II speech was silent despite the register/IRQ
// emulation being complete and the phoneme PCM blob being linked in.
void testSoundIISpeechAudio()
{
    MockingboardCard mb(4, MockingboardCard::Variant::SoundII);
    mb.setSampleRate(44100);
    mb.setVolume(1.0f);
    mb.setMuted(false);

    // Trigger a phoneme: power up with amp=15, slow rate, phoneme $05.
    mb.slotRomWrite(0x43, 0x0F);          // CTTRAMP: CTL=0, amp=15
    mb.slotRomWrite(0x42, 0x00);          // RATEINF: rate=0 (slow)
    mb.slotRomWrite(0x40, 0x80 | 0x05);   // DURPHON: mode=10, phoneme $05

    constexpr int N = 4096;
    std::vector<float> buf(N, 0.0f);
    AudioSource* src = mb.audioSource();
    assert(src);
    src->fillAudioBuffer(buf.data(), N);

    double sumSq = 0.0;
    for (float s : buf) sumSq += static_cast<double>(s) * s;
    const double rms = std::sqrt(sumSq / N);
    std::printf("  Sound II speech rms=%.4f (AYs silent, SSI263 speaking)\n",
                rms);
    assert(rms > 0.005 &&
           "Sound II AudioSrc must mix SSI263 speech into its output");

    // Mute still silences everything (speech included).
    mb.setMuted(true);
    src->fillAudioBuffer(buf.data(), N);
    sumSq = 0.0;
    for (float s : buf) sumSq += static_cast<double>(s) * s;
    assert(sumSq == 0.0);

    // Control: the same writes on a vanilla AC card (no SSI263) keep the
    // output silent — the $40-$4F window is VIA territory there and no AY
    // strobe sequence ran.
    MockingboardCard ac(4, MockingboardCard::Variant::AC);
    ac.setSampleRate(44100);
    ac.setVolume(1.0f);
    ac.setMuted(false);
    ac.slotRomWrite(0x43, 0x0F);
    ac.slotRomWrite(0x42, 0x00);
    ac.slotRomWrite(0x40, 0x80 | 0x05);
    std::fill(buf.begin(), buf.end(), 1.0f);
    ac.audioSource()->fillAudioBuffer(buf.data(), N);
    sumSq = 0.0;
    for (float s : buf) sumSq += static_cast<double>(s) * s;
    assert(sumSq == 0.0);
}

// ─── AY data bus = port-A PINS, not the output latch ─────────────────────
//
// A port-A bit whose DDRA says "input" is undriven; the pull-ups take it
// HIGH. MAME's `via6522_device::output_pa()` hands its PA handler
// `(m_out_a & m_ddr_a) | ~m_ddr_a`, and `a2bus_ayboard_device` gives the
// chip exactly that byte. POM2 built the bus as `portAOut & ddrA`, so every
// undriven bit reached the AY as 0.
//
// The sequence below is the one that makes the difference visible: latch a
// register address with DDRA = $FF, then RELEASE port A (DDRA = $00 — what a
// driver does to read the chip back) and strobe. Pre-fix the card delivered
// $00; a 6522 delivers $FF.
void testAyBusUndrivenBitsFloatHigh()
{
    MockingboardCard card(4);
    writeVia(card, 0, 0x03, 0xFF);            // DDRA = $FF (all output)
    writeVia(card, 0, 0x02, 0x07);            // DDRB = PB0..PB2 out
    writeVia(card, 0, 0x01, 0x07);            // ORA = AY register 7 (mixer)
    writeVia(card, 0, 0x00, kPbLatch);
    writeVia(card, 0, 0x00, kPbInactive);

    // Release port A, then WRITE. The AY must see $FF on the bus.
    writeVia(card, 0, 0x03, 0x00);            // DDRA = $00 (all input)
    writeVia(card, 0, 0x00, kPbWrite);
    writeVia(card, 0, 0x00, kPbInactive);

    const uint8_t r7 = card.getAyRegister(0, 7);
    if (r7 != 0xFF) {
        std::fprintf(stderr,
            "AY bus: undriven port-A bits delivered $%02X, expected $FF "
            "(MAME output_pa = (out & ddr) | ~ddr)\n", r7);
        std::abort();
    }

    // Partially-driven port A: DDRA = $0F drives the low nibble from the
    // ORA latch and floats the high nibble high, so the byte on the bus is
    // $F_ where the latch says $0_.
    MockingboardCard mixed(4);
    writeVia(mixed, 0, 0x02, 0x07);
    writeVia(mixed, 0, 0x03, 0xFF);           // DDRA = all output
    writeVia(mixed, 0, 0x01, 0x07);           // latch AY register 7
    writeVia(mixed, 0, 0x00, kPbLatch);
    writeVia(mixed, 0, 0x00, kPbInactive);
    writeVia(mixed, 0, 0x03, 0x0F);           // DDRA = low nibble only
    writeVia(mixed, 0, 0x01, 0x0A);           // latch holds $0A
    writeVia(mixed, 0, 0x00, kPbWrite);       // bus = $FA (pre-fix: $0A)
    writeVia(mixed, 0, 0x00, kPbInactive);
    assert(mixed.getAyRegister(0, 7) == 0xFA);
}

int main()
{
    testAddressDecode();        std::printf("address decode ........ OK\n");
    testAyBusUndrivenBitsFloatHigh();
                                std::printf("AY bus float-high ..... OK\n");
    testAyRegisterWrite();      std::printf("AY register write ..... OK\n");
    testT1IrqContinuous();      std::printf("T1 IRQ continuous ..... OK\n");
    testT1IrqOneShot();         std::printf("T1 IRQ one-shot ....... OK\n");
    testAyAudioSynthesis();     std::printf("AY audio synthesis .... OK\n");
    testAyToneFrequency();      std::printf("AY tone frequency ..... OK\n");
    testEnvelopeRetriggerOnSameShape();
                                std::printf("envelope retrigger .... OK\n");
    testReset();                std::printf("reset ................. OK\n");
    testSoundIIVariantSSI263();
    testSoundIISpeechAudio();   std::printf("Sound II speech audio . OK\n");
    std::printf("Mockingboard smoke test passed.\n");
    return 0;
}
