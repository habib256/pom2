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

// Mockingboard AY-3-8910 audio-QUALITY regression test.
//
// The existing `mockingboard_smoke_test` asserts that the synthesiser is
// not silent and that a tone lands within 6 % of its nominal pitch. That
// left every audio-fidelity defect free to regress unnoticed. This file
// pins the properties the 2026-08-01/02 rendering work bought, each of
// which was measurably broken before it:
//
//   1. SPECTRAL PURITY. The renderer used to advance the AY's counters in
//      integer clock/8 ticks inside a per-output-sample loop and then
//      point-sample the mixer once at the end, throwing away the
//      sub-sample edge position it had just computed. Every square-wave
//      edge snapped to the 44.1 kHz grid and every harmonic above Nyquist
//      folded back into the audible band. Box-integrating the mixer over
//      the ticks each output sample spans fixes both. MAME avoids the
//      problem by construction: it renders on the chip's own clock/8 grid
//      (`ay8910.cpp:1298`) and decimates with a real resampler
//      (`src/emu/resampler.cpp`).
//
//   2. NO DC OFFSET. The AY channel model is unipolar — `level += table[]`
//      only ever adds — so a channel with tone and noise both masked off
//      in R7 is pure DC, and every volume write steps that DC. MAME
//      high-passes each speaker at 20 Hz by default
//      (`src/emu/audio_effects/filter.cpp:39-44,63-68`); POM2 now does the
//      same on the card output.
//
//   3. SUB-BUFFER WRITE PLACEMENT UNDER A BURSTY PRODUCER. This is the
//      one that mattered most in practice. The CPU worker publishes a
//      whole video frame of register writes in one burst (~17045 cycles)
//      while an audio callback only covers ~5937 cycles, so a burst
//      always contains ~3 callbacks' worth of future writes. The renderer
//      used to drain the entire queue every callback, dump everything it
//      could not place at the buffer edge — where only the last value
//      written to each register survived — and then park the cursor on
//      the newest event. Volume-register PWM was destroyed. Test 3
//      reproduces exactly that technique: Digidream 2's channel-A "SID
//      voice", which is 54 % of that demo's entire register traffic.
//
//   4. LOW-FREQUENCY RESPONSE. The DC blocker of (2) started life as a
//      1-pole filter at MAME's 20 Hz corner. MAME's is a 2-pole
//      Butterworth, which is flat where a 1-pole is already drooping, so
//      POM2 was throwing away up to 0.8 dB of the AY's bottom two octaves.
//      Test 5 measures the fundamental against the analytic filter.
//
// The two ways the cycle-stamped timeline itself can break — a rewind, and
// an out-of-band bank wipe — are pinned separately in
// `mockingboard_timeline_test.cpp`.

#include "AyPsgSynth.h"
#include "M6502.h"
#include "Memory.h"
#include "Mockingboard.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kSr = 44100;
// PAL phase-0 clock — what the AY's pin 22 really sees on a European
// machine, and what Digidream 1 / 2 and the rest of DIX run at.
constexpr double kPalClockHz = 1015625.0;

// ─── VIA → AY bus helpers (same idiom as mockingboard_smoke_test) ────────

constexpr uint8_t kPbInactive = 0x04;   // {BC1=0, BDIR=0, /RESET=1}
constexpr uint8_t kPbWrite    = 0x06;   // {BC1=0, BDIR=1, /RESET=1}
constexpr uint8_t kPbLatch    = 0x07;   // {BC1=1, BDIR=1, /RESET=1}

void writeVia(MockingboardCard& card, int chip, uint8_t reg, uint8_t v)
{
    const uint8_t base = (chip == 0) ? 0x00 : 0x80;
    card.slotRomWrite(static_cast<uint8_t>(base | (reg & 0x0F)), v);
}

void ayWrite(MockingboardCard& card, int chip, uint8_t reg, uint8_t v)
{
    writeVia(card, chip, 0x03, 0xFF);         // DDRA = $FF
    writeVia(card, chip, 0x02, 0x07);         // DDRB = PB0..PB2 out
    writeVia(card, chip, 0x01, reg);          // ORA = reg
    writeVia(card, chip, 0x00, kPbLatch);
    writeVia(card, chip, 0x00, kPbInactive);
    writeVia(card, chip, 0x01, v);            // ORA = value
    writeVia(card, chip, 0x00, kPbWrite);
    writeVia(card, chip, 0x00, kPbInactive);
}

// ─── Minimal iterative radix-2 FFT ───────────────────────────────────────
// Only used by test 1; N is a power of two by construction.
void fft(std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k];
                const std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k]             = u + v;
                a[i + k + len / 2]   = u - v;
                w *= wl;
            }
        }
    }
}

// Fraction of total spectral energy that does NOT sit on a harmonic of
// `f0`. A perfect band-limited square wave scores ~0; a point-sampled one
// scores high because its fold-back products land between the harmonics.
double inharmonicFraction(const std::vector<float>& x, double f0, double sr)
{
    const size_t n = x.size();
    std::vector<std::complex<double>> a(n);
    // Hann window — without it the non-integer-bin fundamental smears
    // across the whole spectrum and swamps the measurement.
    for (size_t i = 0; i < n; ++i) {
        const double w =
            0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) /
                                  static_cast<double>(n - 1)));
        a[i] = std::complex<double>(static_cast<double>(x[i]) * w, 0.0);
    }
    fft(a);
    const size_t half = n / 2;
    const double binHz = sr / static_cast<double>(n);
    std::vector<double> mag2(half, 0.0);
    for (size_t k = 0; k < half; ++k) mag2[k] = std::norm(a[k]);

    // Mark every bin within +/-3 of a harmonic as "harmonic". A Hann main
    // lobe is 4 bins wide, so 3 covers it with margin.
    std::vector<bool> isHarm(half, false);
    for (int h = 1; ; ++h) {
        const double f = f0 * h;
        if (f >= sr * 0.5) break;
        const long c = std::lround(f / binHz);
        for (long k = c - 3; k <= c + 3; ++k)
            if (k >= 0 && k < static_cast<long>(half))
                isHarm[static_cast<size_t>(k)] = true;
    }
    // DC and the first few bins belong to the window/DC-blocker, not to
    // the aliasing we are measuring.
    for (size_t k = 0; k < 6 && k < half; ++k) isHarm[k] = true;

    double tot = 0.0, bad = 0.0;
    for (size_t k = 0; k < half; ++k) {
        tot += mag2[k];
        if (!isHarm[k]) bad += mag2[k];
    }
    return (tot > 0.0) ? (bad / tot) : 0.0;
}

// ─── Test 1: a plain tone must be spectrally clean ───────────────────────
void testTonalPurity()
{
    // TP = 16 -> 1022727/(16*16) = 3995 Hz. High enough that a
    // point-sampler folds a lot of energy back (the pre-fix renderer
    // measured about -11 dB of inharmonic energy here, i.e. ~7 % of total
    // power), low enough to stay musically ordinary.
    constexpr int kPeriod = 16;
    MockingboardCard card(4);
    card.setSampleRate(kSr);
    card.setVolume(1.0f);
    card.setMuted(false);

    ayWrite(card, 0, 0, kPeriod & 0xFF);
    ayWrite(card, 0, 1, (kPeriod >> 8) & 0x0F);
    ayWrite(card, 0, 7, 0x3E);          // tone A only
    ayWrite(card, 0, 8, 0x0F);          // amplitude 15

    constexpr size_t N = 16384;
    std::vector<float> buf(N);
    AudioSource* src = card.audioSource();
    assert(src);
    // Discard one buffer so the DC blocker has settled.
    src->fillAudioBuffer(buf.data(), static_cast<int>(N));
    src->fillAudioBuffer(buf.data(), static_cast<int>(N));

    const double f0 = (1022727.0 / 8.0) / (2.0 * kPeriod);   // 3995.0 Hz
    const double frac = inharmonicFraction(buf, f0, kSr);
    std::printf("  tone TP=%d (%.0f Hz): inharmonic energy = %.2f %% (%.1f dB)\n",
                kPeriod, f0, frac * 100.0, 10.0 * std::log10(frac));
    // The pre-fix renderer sat at roughly 7 % here. Box integration takes
    // it well below 1 %; assert 2 % so the test pins the improvement
    // without being brittle about the exact filter.
    assert(frac < 0.02);
}

// ─── Test 2: no DC offset, even on a pure-DC channel configuration ───────
void testNoDcOffset()
{
    MockingboardCard card(4);
    card.setSampleRate(kSr);
    card.setVolume(1.0f);
    card.setMuted(false);

    // R7 = $3F masks tone AND noise on every channel, so channel A's
    // mixer output is stuck high and the channel emits a constant equal
    // to its amplitude. This is the configuration volume-register digi
    // playback uses. Unipolar and unfiltered, it would sit at a large
    // positive DC forever.
    ayWrite(card, 0, 7, 0x3F);
    ayWrite(card, 0, 8, 0x0F);

    constexpr size_t N = 16384;
    std::vector<float> buf(N);
    AudioSource* src = card.audioSource();
    assert(src);
    src->fillAudioBuffer(buf.data(), static_cast<int>(N));   // settle
    src->fillAudioBuffer(buf.data(), static_cast<int>(N));

    double mean = 0.0;
    for (float s : buf) mean += static_cast<double>(s);
    mean /= static_cast<double>(N);
    std::printf("  constant-level channel: residual DC = %.6f\n", mean);
    // Without the blocker this is the full channel level (~0.33 after the
    // /3 normalisation). One pole at 20 Hz kills it to the noise floor.
    assert(std::fabs(mean) < 0.005);
}

// ─── Test 3: volume-register PWM survives a bursty producer ──────────────
// Digidream 2's channel-A "SID voice": the CPU toggles the amplitude
// register between 0 and a level at an audio rate, while the register
// writes reach the audio thread in once-per-video-frame bursts.
void testVolumePwmUnderBurstyProducer()
{
    Memory mem;
    M6502  cpu(&mem);
    MockingboardCard card(4);
    card.setCpu(&cpu);
    card.setSampleRate(kSr);
    card.setVolume(1.0f);
    card.setMuted(false);

    // Mask tone and noise so the channel level IS the amplitude register:
    // the output waveform is then exactly the PWM the CPU writes.
    ayWrite(card, 0, 7, 0x3F);
    ayWrite(card, 0, 8, 0x00);

    // Toggle every 1000 CPU cycles -> a square wave of period 2000
    // cycles = 1022727/2000 = 511.4 Hz.
    constexpr int kHalfPeriodCycles = 1000;
    const double kExpectedHz = 1022727.0 / (2.0 * kHalfPeriodCycles);

    // One NTSC video frame of production, then exactly the matching
    // amount of consumption — the real producer/consumer cadence.
    constexpr int kFrameCycles  = 17045;
    constexpr int kFrameSamples = 735;      // 17045 / (1022727/44100)
    constexpr int kChunk        = 245;      // 3 chunks per frame
    constexpr int kFrames       = 40;

    AudioSource* src = card.audioSource();
    assert(src);
    std::vector<float> all;
    all.reserve(static_cast<size_t>(kFrames) * kFrameSamples);
    std::vector<float> chunk(kChunk);

    int  cyclesIntoHalf = 0;
    bool high = false;
    for (int f = 0; f < kFrames; ++f) {
        // ── Producer: one frame's worth of writes, published as a burst.
        int remaining = kFrameCycles;
        while (remaining > 0) {
            const int step = std::min(remaining, kHalfPeriodCycles - cyclesIntoHalf);
            mem.advanceCycles(step);
            cyclesIntoHalf += step;
            remaining      -= step;
            if (cyclesIntoHalf >= kHalfPeriodCycles) {
                cyclesIntoHalf = 0;
                high = !high;
                ayWrite(card, 0, 8, high ? 0x0F : 0x00);
            }
        }
        // ── Consumer: the matching number of samples.
        for (int c = 0; c < kFrameSamples / kChunk; ++c) {
            src->fillAudioBuffer(chunk.data(), kChunk);
            all.insert(all.end(), chunk.begin(), chunk.end());
        }
    }

    // Analyse the tail only — skip the jitter buffer's start-up latency.
    const size_t skip = all.size() / 2;
    std::vector<float> tail(all.begin() + static_cast<ptrdiff_t>(skip), all.end());
    double mean = 0.0;
    for (float s : tail) mean += static_cast<double>(s);
    mean /= static_cast<double>(tail.size());

    int crossings = 0;
    for (size_t i = 1; i < tail.size(); ++i)
        if (tail[i - 1] <= mean && tail[i] > mean) ++crossings;
    const double measured =
        crossings * static_cast<double>(kSr) / static_cast<double>(tail.size());

    double sumSq = 0.0;
    for (float s : tail) sumSq += static_cast<double>(s) * s;
    const double rms = std::sqrt(sumSq / static_cast<double>(tail.size()));

    std::printf("  volume PWM: measured=%.1f Hz expected=%.1f Hz rms=%.4f\n",
                measured, kExpectedHz, rms);
    // Pre-fix, ~90 % of these writes were dumped at the buffer edge where
    // only the last value of each burst survived, so the PWM collapsed to
    // the ~60 Hz burst rate (or to silence) instead of reproducing 511 Hz.
    assert(rms > 0.02);
    assert(measured > kExpectedHz * 0.90 && measured < kExpectedHz * 1.10);
}

// ─── Test 3b: dense digi stream + both AYs must not glitch ──────────────
// Digidream 1's profile, and the case that caught a bad first attempt at
// the jitter-buffer cursor. DD1 drives BOTH AYs and runs a 4-bit PCM
// "digidrum" into a volume register from a VIA1 T1 interrupt at ~6.8 kHz,
// on top of the 50 Hz pattern player. That is ~40x the write density of
// DD2's SID voice, so any instability in the replay cursor shows up as
// broadband glitch energy rather than as a subtle timing shift.
//
// What it caught: the cursor's re-anchor fought the (then separate)
// backward-jump guard. A forward re-anchor left older queued events
// behind the cursor, the backward guard yanked it back, and the two
// alternated every callback — audible as tempo instability on DD1 while
// DD2, with a far sparser stream, mostly got away with it.
void testDenseDigiStreamBothChips()
{
    Memory mem;
    M6502  cpu(&mem);
    MockingboardCard card(4);
    card.setCpu(&cpu);
    card.setSampleRate(kSr);
    card.setVolume(1.0f);
    card.setMuted(false);
    // PAL, like DD1 and the rest of the French Touch corpus. This is NOT
    // incidental: the replay cursor's target lag is sized in PAL frames,
    // so a PAL producer is the case where the lag swings across its full
    // range every frame. An NTSC producer emits a SHORTER burst than the
    // target lag and never exercises the bottom of that swing — which is
    // exactly why the first version of this test passed against the buggy
    // cursor it was written to catch.
    card.setCpuClock(kPalClockHz);

    // Chip 0 carries the digi: tone + noise masked, so the channel level
    // IS the value written to R8 and the output is exactly the PCM.
    ayWrite(card, 0, 7, 0x3F);
    ayWrite(card, 0, 8, 0x00);
    // Chip 1 is driven too (so both VIAs feed the queue) but silent, to
    // keep the spectrum below attributable to the PCM alone.
    ayWrite(card, 1, 7, 0x3F);
    ayWrite(card, 1, 8, 0x00);

    // DD1's own figure: ATARI ST MFP digidrum replayed on the Apple at
    // 1015625/149 (see disks_5.4/demo/digidream/Sources/main.a, DELAYDIGI).
    // A write every 149 cycles = 6864 Hz; 32 writes per PCM period puts
    // the fundamental at 214.5 Hz and every sample-and-hold image exactly
    // on a harmonic of it, so a clean render has NO inharmonic energy.
    constexpr int kWriteIntervalCycles = 149;
    constexpr int kPcmPeriodWrites     = 32;
    const double  kFundamentalHz =
        kPalClockHz / (kWriteIntervalCycles * kPcmPeriodWrites);

    constexpr int    kFrameCycles  = 20313;   // one PAL video frame
    constexpr double kFrameSamples = 882.0;   // 44100 / 50
    // The REAL miniaudio period (AudioDevice.cpp: periodSizeInFrames).
    // Deliberately not a divisor of kFrameSamples: 882/256 = 3.445
    // buffers per producer tick, so the phase between producer and
    // consumer drifts continuously and the replay cursor's lag sweeps its
    // whole range instead of sitting at one value. An earlier version of
    // this test consumed exactly 882 samples per frame in 3 equal chunks;
    // that locks the lag at a constant and made the test pass against the
    // very cursor bug it was written to catch.
    constexpr int    kChunk        = 256;
    constexpr int    kFrames       = 200;

    AudioSource* src = card.audioSource();
    assert(src);
    std::vector<float> all;
    all.reserve(static_cast<size_t>(kFrames * kFrameSamples) + 1024);
    std::vector<float> chunk(kChunk);

    int    cyclesIntoWrite = 0;
    int    pcmIndex        = 0;
    double samplesOwed     = 0.0;
    uint32_t jitter        = 0x1234567u;
    for (int f = 0; f < kFrames; ++f) {
        int remaining = kFrameCycles;
        while (remaining > 0) {
            const int step =
                std::min(remaining, kWriteIntervalCycles - cyclesIntoWrite);
            mem.advanceCycles(step);
            cyclesIntoWrite += step;
            remaining       -= step;
            if (cyclesIntoWrite >= kWriteIntervalCycles) {
                cyclesIntoWrite = 0;
                const double ph =
                    2.0 * kPi * pcmIndex / static_cast<double>(kPcmPeriodWrites);
                const int v = static_cast<int>(std::lround(7.5 + 7.4 * std::sin(ph)));
                ayWrite(card, 0, 8, static_cast<uint8_t>(v & 0x0F));
                pcmIndex = (pcmIndex + 1) % kPcmPeriodWrites;
            }
        }
        // The audio callback is NOT in lockstep with the CPU worker: it
        // runs on the sound device's own thread and clock, so the number
        // of buffers that land between two producer ticks wanders. Model
        // that with a deterministic pseudo-jitter of +/-2 buffers around
        // the rate-matched value, which is what makes the replay cursor's
        // lag actually sweep its range.
        samplesOwed += kFrameSamples;
        jitter = jitter * 1103515245u + 12345u;
        const int extra = static_cast<int>((jitter >> 16) % 5u) - 2;
        int budget = static_cast<int>(samplesOwed / kChunk) + extra;
        for (int c = 0; c < budget; ++c) {
            src->fillAudioBuffer(chunk.data(), kChunk);
            all.insert(all.end(), chunk.begin(), chunk.end());
            samplesOwed -= kChunk;
        }
    }

    // Analyse a power-of-two tail, past the jitter buffer's start-up.
    constexpr size_t kN = 16384;
    assert(all.size() > kN + 4096);
    std::vector<float> tail(all.end() - static_cast<ptrdiff_t>(kN), all.end());

    double sumSq = 0.0;
    for (float v : tail) sumSq += static_cast<double>(v) * v;
    const double rms = std::sqrt(sumSq / static_cast<double>(kN));

    const double frac = inharmonicFraction(tail, kFundamentalHz, kSr);
    std::printf("  dense digi (%.0f Hz PCM @ %.0f Hz writes, 2 chips): "
                "rms=%.4f inharmonic=%.2f %%\n",
                kFundamentalHz, kPalClockHz / kWriteIntervalCycles, rms,
                frac * 100.0);
    assert(rms > 0.01);
    // Cursor thrash scatters broadband energy here. A stable cursor keeps
    // this in the low single digits.
    assert(frac < 0.06);
}

// ─── Test 3c: an IDLE producer must not disturb the replay cursor ───────
// The case a continuous-write harness can never show, and the one that
// actually broke Digidream 1. Music has gaps: part transitions, disk
// loads, silence between patterns. During a gap the CPU keeps running but
// emits no AY writes, so `latestAyEventCycle_` freezes while the audio
// thread keeps consuming and the replay cursor keeps advancing.
//
// The first version of the pacing guard treated that as "the consumer has
// run up onto the producer" and re-anchored — every single callback, for
// the whole gap, dragging the cursor progressively further behind real
// CPU time. When writes resumed, the cursor was hundreds of thousands of
// cycles late, the starvation branch fired, and a buffer's worth of
// events went out in a lump. Audible as a tempo glitch on resumption.
//
// This test plays, goes quiet, then plays again, and looks at the audio
// RIGHT AFTER the resumption.
//
// HONEST SCOPE: it pins that resumption works, and it would catch a cursor
// that failed to recover from a gap at all. It does NOT discriminate the
// specific defect described above — measured against a deliberately
// restored buggy guard it produces identical numbers, because that guard
// still self-heals via the starvation branch within one buffer. The defect
// is real (it was caught from a live run's instrumentation, not from this
// harness) but its blast radius is one mis-placed buffer, so do not read a
// pass here as proof that gap handling is optimal.
void testIdleProducerThenResume()
{
    Memory mem;
    M6502  cpu(&mem);
    MockingboardCard card(4);
    card.setCpu(&cpu);
    card.setSampleRate(kSr);
    card.setVolume(1.0f);
    card.setMuted(false);
    card.setCpuClock(kPalClockHz);

    ayWrite(card, 0, 7, 0x3F);      // tone + noise masked: level == R8
    ayWrite(card, 0, 8, 0x00);

    constexpr int    kHalfPeriodCycles = 1000;
    const double     kExpectedHz = kPalClockHz / (2.0 * kHalfPeriodCycles);
    constexpr int    kFrameCycles  = 20313;
    constexpr double kFrameSamples = 882.0;
    constexpr int    kChunk        = 256;

    AudioSource* src = card.audioSource();
    assert(src);
    std::vector<float> chunk(kChunk);
    std::vector<float> resumed;          // samples captured after the gap
    double samplesOwed   = 0.0;
    int    cyclesIntoHalf = 0;
    bool   high           = false;
    bool   capture        = false;

    // phase 0: 20 frames of PWM | phase 1: 40 silent frames | phase 2: PWM
    for (int f = 0; f < 100; ++f) {
        const bool writing = (f < 20) || (f >= 60);
        if (f == 60) capture = true;     // start capturing at resumption
        int remaining = kFrameCycles;
        while (remaining > 0) {
            const int step =
                std::min(remaining, kHalfPeriodCycles - cyclesIntoHalf);
            mem.advanceCycles(step);
            cyclesIntoHalf += step;
            remaining      -= step;
            if (cyclesIntoHalf >= kHalfPeriodCycles) {
                cyclesIntoHalf = 0;
                high = !high;
                // During the gap the CPU still runs — it just does not
                // touch the AY. That is the whole point of the test.
                if (writing) ayWrite(card, 0, 8, high ? 0x0F : 0x00);
            }
        }
        samplesOwed += kFrameSamples;
        while (samplesOwed >= kChunk) {
            src->fillAudioBuffer(chunk.data(), kChunk);
            if (capture && resumed.size() < 8192)
                resumed.insert(resumed.end(), chunk.begin(), chunk.end());
            samplesOwed -= kChunk;
        }
    }

    assert(resumed.size() >= 8192);
    resumed.resize(8192);
    // Skip the first two buffers: the jitter buffer legitimately needs the
    // target lag to refill before the resumed stream is placeable.
    std::vector<float> win(resumed.begin() + 1024, resumed.end());
    win.resize(4096);

    double sumSq = 0.0;
    for (float v : win) sumSq += static_cast<double>(v) * v;
    const double rms = std::sqrt(sumSq / static_cast<double>(win.size()));
    const double frac = inharmonicFraction(win, kExpectedHz, kSr);
    std::printf("  resume after idle gap: rms=%.4f inharmonic=%.2f %%\n",
                rms, frac * 100.0);
    // A mis-anchored cursor dumps a buffer of edges in a lump, which is
    // broadband. A clean resumption is a plain square wave.
    assert(rms > 0.02);
    assert(frac < 0.10);
}

// ─── Test 4: envelope shape sequences match MAME, all 16 shapes ──────────
// Drives R13 with a slow envelope period and decodes the amplitude
// staircase back into MAME's 4-bit `step ^ attack` sequence. Pins the
// state machine (`ay8910.h:243-259` set_shape + `ay8910.cpp:1126-1144`
// ramp end) against the alternate/hold flags, which is where an
// off-by-one silently breaks shapes $0A / $0E.
void testEnvelopeShapes()
{
    // Reference: first 32 envelope steps for each shape, generated from
    // MAME's state machine. Shapes 0-7 collapse onto two behaviours
    // (continue = 0 maps to hold), which is itself worth pinning.
    static const char* kExpect[16] = {
        "FEDCBA9876543210" "0000000000000000",   // $0 \___
        "FEDCBA9876543210" "0000000000000000",   // $1 \___
        "FEDCBA9876543210" "0000000000000000",   // $2 \___
        "FEDCBA9876543210" "0000000000000000",   // $3 \___
        "0123456789ABCDEF" "0000000000000000",   // $4 /___
        "0123456789ABCDEF" "0000000000000000",   // $5 /___
        "0123456789ABCDEF" "0000000000000000",   // $6 /___
        "0123456789ABCDEF" "0000000000000000",   // $7 /___
        "FEDCBA9876543210" "FEDCBA9876543210",   // $8 saw down, repeating
        "FEDCBA9876543210" "0000000000000000",   // $9 down then hold low
        "FEDCBA9876543210" "0123456789ABCDEF",   // $A triangle, down first
        "FEDCBA9876543210" "FFFFFFFFFFFFFFFF",   // $B down then hold high
        "0123456789ABCDEF" "0123456789ABCDEF",   // $C saw up, repeating
        "0123456789ABCDEF" "FFFFFFFFFFFFFFFF",   // $D up then hold high
        "0123456789ABCDEF" "FEDCBA9876543210",   // $E triangle, up first
        "0123456789ABCDEF" "0000000000000000",   // $F up then hold low
    };

    // Envelope period in base ticks: one step takes 2*EP clock/8 ticks.
    // EP = 64 -> 128 ticks/step -> 128/2.899 = 44.2 output samples/step,
    // plenty to identify each step by its level.
    constexpr int kEnvPer = 64;
    const double ticksPerSample = (1022727.0 / 8.0) / kSr;
    const int samplesPerStep =
        static_cast<int>((2.0 * kEnvPer) / ticksPerSample);

    // Amplitude table (must match Mockingboard.cpp's kAyVolumeTable).
    static const float kTab[16] = {
        0.0000f, 0.0105f, 0.0154f, 0.0223f, 0.0321f, 0.0468f, 0.0635f, 0.1061f,
        0.1319f, 0.2164f, 0.2974f, 0.3909f, 0.5128f, 0.6371f, 0.8186f, 1.0000f
    };

    for (int shape = 0; shape < 16; ++shape) {
        MockingboardCard card(4);
        card.setSampleRate(kSr);
        card.setVolume(1.0f);
        card.setMuted(false);
        // Channel A: tone+noise masked (so the level IS the envelope),
        // amplitude register in envelope mode (bit 4).
        ayWrite(card, 0, 7, 0x3F);
        ayWrite(card, 0, 8, 0x10);
        ayWrite(card, 0, 11, kEnvPer & 0xFF);
        ayWrite(card, 0, 12, (kEnvPer >> 8) & 0xFF);
        ayWrite(card, 0, 13, static_cast<uint8_t>(shape));

        const int nSteps = 32;
        std::vector<float> buf(static_cast<size_t>(samplesPerStep) * nSteps + 64);
        AudioSource* src = card.audioSource();
        assert(src);
        src->fillAudioBuffer(buf.data(), static_cast<int>(buf.size()));

        // The output has been through the DC blocker, so the raw sample
        // is NOT the table level. Decode FORWARD rather than inverting the
        // filter: build the staircase each candidate shape predicts, push
        // it through a fresh blocker of the same type, and see which one
        // the render actually matches.
        //
        // This used to run the blocker backwards instead, which only
        // worked while it was one pole. Inverting the MAME biquad the card
        // now uses means a recursion with a DOUBLE pole at z = 1 driven by
        // float-quantised samples — a double integrator on the
        // quantisation noise, which diverges over the ~45 k samples this
        // test captures. Matching forward is filter-agnostic and, unlike
        // the inverse, also pins the LEVELS rather than just their order.
        const auto reference = [&](int cand) {
            std::vector<float> ref(buf.size(), 0.0f);
            pom2::ay::DcBlocker dc;
            dc.setRate(kSr);
            for (size_t i = 0; i < ref.size(); ++i) {
                const size_t st = i / static_cast<size_t>(samplesPerStep);
                const char c = (st < 32) ? kExpect[cand][st] : '0';
                const int lvl = (c <= '9') ? (c - '0') : (c - 'A' + 10);
                // /6 = the card's per-side /3 followed by the mono
                // fold-down's x0.5.
                ref[i] = dc.process(kTab[lvl & 0x0F] / 6.0f);
            }
            return ref;
        };

        int    bestCand = -1;
        double bestErr  = 1e9, secondErr = 1e9;
        for (int cand = 0; cand < 16; ++cand) {
            const std::vector<float> ref = reference(cand);
            double err = 0.0;
            for (int s = 0; s < nSteps; ++s) {
                const size_t idx =
                    static_cast<size_t>(s) * samplesPerStep + samplesPerStep / 2;
                err += std::fabs(static_cast<double>(buf[idx] - ref[idx]));
            }
            err /= nSteps;
            if (err < bestErr) { secondErr = bestErr; bestErr = err; bestCand = cand; }
            else if (err < secondErr) { secondErr = err; }
        }
        // Shapes 0-7 collapse onto two behaviours, so several candidates
        // are legitimately identical; compare the SEQUENCES, not the
        // indices, and require the winner to be an exact string match.
        if (std::string(kExpect[bestCand]) != std::string(kExpect[shape]) ||
            bestErr > 0.002) {
            std::printf("  shape $%X MISMATCH: best match $%X "
                        "(err %.5f, runner-up %.5f)\n    expect %s\n",
                        shape, bestCand, bestErr, secondErr, kExpect[shape]);
            assert(false && "envelope shape sequence diverges from MAME");
        }
    }
    std::printf("  all 16 envelope shapes match the MAME step sequence\n");
}

// ─── Test 5: low-frequency response matches MAME's speaker high-pass ────
// The AY channel model is unipolar, so the render chain HAS to high-pass;
// the only question is which filter. MAME's answer is its default speaker
// effect: a 2-pole Butterworth at 20 Hz, one instance per output channel
// (`src/emu/audio_effects/filter.cpp` — `reset_highpass_active` defaults
// to `true`, `reset_fh` to 20, `reset_qh` to `DEFAULT_Q = 0.7071067f` from
// `filter.h`; coefficients from `build_highpass`).
//
// POM2 used a 1-POLE blocker at the same corner until 2026-08-02. Same
// -3 dB point, very different passband: a 1-pole is already drooping an
// octave up where the Butterworth is flat, and the AY's bass register is
// exactly there. Measured deficit against MAME, single tone at amplitude
// 15: 0.76 dB at 27.5 Hz, 0.46 dB at 55 Hz, 0.23 dB at 82.5 Hz.
//
// Everything ELSE in the chain measured transparent down to 27.5 Hz, which
// is why this test asserts against the analytic filter response rather
// than a fudge factor: the volume table is Westcott's measurement to
// within 0.0007 (see kVolumeTable's note), the box integrator's sinc is
// 1.0000 below 100 Hz, and the two DC blockers sit in PARALLEL on
// independent channels, so the mono fold-down cannot cancel any of it.
void testLowFrequencyResponse()
{
    // Ideal fundamental of the rendered square. One hard-panned chip at
    // amplitude 15 toggles chipOut between 0 and 1; x1/3 for the per-side
    // normalisation and x0.5 for the mono fold-down gives a square of
    // amplitude 1/12 about its own mean, whose fundamental is 4/pi of that.
    const double ideal = (4.0 / kPi) / 12.0;

    struct Point { int period; double maxDeviationDb; };
    // Tone periods chosen to land on musical pitches: A0, A1, E2, A2, A4.
    static const Point kPoints[] = {
        { 2308, 0.10 },   // 27.50 Hz
        { 1154, 0.10 },   // 55.01 Hz
        {  769, 0.10 },   // 82.54 Hz
        {  577, 0.10 },   // 110.01 Hz
        {  144, 0.10 },   // 440.81 Hz
    };

    for (const Point& p : kPoints) {
        MockingboardCard card(4);
        card.setSampleRate(kSr);
        card.setVolume(1.0f);
        card.setMuted(false);
        card.setCpuClock(kPalClockHz);
        ayWrite(card, 0, 0, p.period & 0xFF);
        ayWrite(card, 0, 1, (p.period >> 8) & 0x0F);
        ayWrite(card, 0, 7, 0x3E);        // tone A only
        ayWrite(card, 0, 8, 0x0F);        // amplitude 15

        // 11.9 s: at 27.5 Hz one period is 1604 samples, and the Goertzel
        // below needs many of them to resolve the fundamental cleanly.
        const size_t N = 1u << 19;
        std::vector<float> buf(N);
        AudioSource* src = card.audioSource();
        assert(src);
        src->fillAudioBuffer(buf.data(), static_cast<int>(N));   // settle
        src->fillAudioBuffer(buf.data(), static_cast<int>(N));

        const double f0 = (kPalClockHz / 8.0) / (2.0 * p.period);
        // Goertzel magnitude at f0.
        const double w = 2.0 * kPi * f0 / kSr;
        const double cw = 2.0 * std::cos(w);
        double s1 = 0.0, s2 = 0.0;
        for (float v : buf) { const double s0 = v + cw * s1 - s2; s2 = s1; s1 = s0; }
        const double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
        const double meas = 2.0 * std::sqrt(re * re + im * im) / double(N);

        // MAME's 2-pole Butterworth high-pass, analytically.
        const double r = f0 / 20.0;
        const double expected = ideal * (r * r / std::sqrt(1.0 + r * r * r * r));
        const double devDb = 20.0 * std::log10(meas / expected);
        // What the old 1-pole would have given, for the record.
        const double onePole = ideal * (f0 / std::sqrt(f0 * f0 + 400.0));
        std::printf("  f0=%7.2f Hz  fund=%.6f  vs MAME 2-pole %+.2f dB "
                    "(1-pole would be %+.2f dB)\n",
                    f0, meas, devDb, 20.0 * std::log10(onePole / expected));
        assert(std::fabs(devDb) < p.maxDeviationDb);
    }
}

}  // namespace

// ─── Test 8: a CPU-driven PWM edge is placed WITHIN the sample ──────────
//
// Bug hunt #19. Tone, noise and envelope edges were always sub-sample placed
// (`stepTick` fires at a fractional `tickPhase`), but a REGISTER-driven edge
// was not: the render loop applied every event stamped anywhere inside a
// sample before integrating any of it, so the edge snapped to the output grid
// — 0 to 22.7 us early at 44.1 kHz, never late, uniformly distributed. That
// is the whole of a volume-PWM digidrum, and it reads as a gritty noise floor
// rather than as a pitch error.
//
// The measurement is the one test 1 uses for tones, applied to a PWM whose
// period is deliberately incommensurate with the sample grid: the sample-and
// -hold jitter shows up as energy off the harmonics of f0.
void testPwmSubSamplePlacement()
{
    Memory mem;
    M6502  cpu(&mem);
    MockingboardCard card(4);
    card.setCpu(&cpu);
    card.setSampleRate(kSr);
    card.setVolume(1.0f);
    card.setMuted(false);

    ayWrite(card, 0, 7, 0x3F);          // tone + noise off: R8 IS the output
    ayWrite(card, 0, 8, 0x00);

    // 167 cycles per half period: 1022727/334 = 3062.1 Hz, and 167 is prime
    // to the 23.19 cycles an output sample spans, so the edges walk across
    // the sample grid instead of landing on it.
    constexpr int kHalfPeriodCycles = 167;
    const double  kF0 = 1022727.0 / (2.0 * kHalfPeriodCycles);
    constexpr int kFrameCycles  = 17045;
    constexpr int kFrameSamples = 735;
    constexpr int kChunk        = 245;
    constexpr int kFrames       = 60;

    AudioSource* src = card.audioSource();
    assert(src);
    std::vector<float> all;
    all.reserve(static_cast<size_t>(kFrames) * kFrameSamples);
    std::vector<float> chunk(kChunk);
    int  cyclesIntoHalf = 0;
    bool high = false;
    for (int f = 0; f < kFrames; ++f) {
        int remaining = kFrameCycles;
        while (remaining > 0) {
            const int step = std::min(remaining, kHalfPeriodCycles - cyclesIntoHalf);
            mem.advanceCycles(step);
            cyclesIntoHalf += step;
            remaining      -= step;
            if (cyclesIntoHalf >= kHalfPeriodCycles) {
                cyclesIntoHalf = 0;
                high = !high;
                ayWrite(card, 0, 8, high ? 0x0F : 0x00);
            }
        }
        for (int c = 0; c < kFrameSamples / kChunk; ++c) {
            src->fillAudioBuffer(chunk.data(), kChunk);
            all.insert(all.end(), chunk.begin(), chunk.end());
        }
    }
    // A power of two from the tail, past the jitter buffer's start-up.
    constexpr size_t kFft = 16384;
    assert(all.size() > kFft);
    std::vector<float> tail(all.end() - static_cast<ptrdiff_t>(kFft), all.end());
    const double frac = inharmonicFraction(tail, kF0, kSr);
    std::printf("  PWM %.0f Hz on the amplitude register: inharmonic energy "
                "= %.2f %% (%.1f dB)\n",
                kF0, frac * 100.0, 10.0 * std::log10(frac));
    // Measured on this exact stream: 5.04 % with the edge snapped to the
    // output grid, 0.36 % with the sample split at the write — a 14x drop,
    // -11.5 dB. The bound pins the improvement without being brittle about
    // the filter.
    assert(frac < 0.01);
}

int main()
{
    testTonalPurity();
    std::printf("tonal purity ......... OK\n");
    testNoDcOffset();
    std::printf("DC offset ............ OK\n");
    testVolumePwmUnderBurstyProducer();
    std::printf("volume PWM placement . OK\n");
    testDenseDigiStreamBothChips();
    std::printf("dense digi stream .... OK\n");
    testIdleProducerThenResume();
    std::printf("idle then resume ..... OK\n");
    testEnvelopeShapes();
    std::printf("envelope shapes ...... OK\n");
    testLowFrequencyResponse();
    std::printf("bass response ........ OK\n");
    testPwmSubSamplePlacement();
    std::printf("PWM sub-sample place . OK\n");
    std::printf("Mockingboard audio quality test passed.\n");
    return 0;
}
