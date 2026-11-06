// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ssi263FormantVoice — the SSI 263's sound SYNTHESISED from its registers,
// as a second voice beside the recorded phonemes (`Ssi263PhonemeData`).
//
// Why a second voice. The recorded phonemes are 62 fixed takes: what the
// chip's rate, inflection, articulation and filter registers were when they
// were recorded is baked in, and writing those registers changes nothing but
// the A/R cadence. A driver that sets a pitch per syllable — that SINGS —
// plays a monotone. This voice computes the sound from the registers, so the
// 12-bit inflection value is the pitch, the filter register moves the whole
// vocal tract, and one phoneme glides into the next.
//
// What it is, and how far to trust it — graded, because it is a mix:
//
// [B] SSI 263A / Votrax SC-02 data sheet (Silicon Systems, 1985), p. 2 —
//     "Inflection Frequency = XCK Frequency / (8 x (4096 - I))" and "Filter
//     Frequency = XCK frequency / (2 x (256 - FF))", "typically … about
//     20KHz". Both used as written. The same page says the tract is "five
//     cascaded programmable low pass filter sections" and that a new phoneme
//     is reached by "a linear transition"; this model has THREE resonators
//     and an EXPONENTIAL glide. Those are the two places it knowingly leaves
//     the document.
// [D] Casso `CassoEmuCore/Devices/Ssi263.cpp` @ 2d899adf (MIT, © Robert
//     Elmer) — the signal path is a port of its `GenerateSample`: impulse
//     train through two one-pole sections, three DC-normalised two-pole
//     resonators in cascade, a parallel fricative branch at F2, a radiation
//     differentiator, an output low-pass, an attack/release envelope. The
//     per-phoneme targets (`kPhonemes`) are Casso's table, which it derives
//     from a read of the chip's parameter ROM off a visual6502 die
//     photograph, with the code-to-Hz curve borrowed from the Votrax SC-01A
//     as MAME models it — two inferences deep.
// [F] Every gain, bandwidth and time constant below. Casso tuned them by ear
//     and against a recording of a real chip that is NOT in its repository,
//     so none of it can be re-derived. Settled by: a recording of a real
//     SSI 263 speaking known phonemes at a known register state (the probe
//     programs under Casso's `Apple2/Demos/Mockingboard/Speech/` are made
//     for that), compared band by band.
//
// Three things changed in the port. Casso's three one-pole coefficients were
// bare per-sample constants, so the timbre moved with the host's sample
// rate; they are corner frequencies here (the values those constants gave at
// 44.1 kHz). The chip PROTOCOL stays `Ssi263`'s: this class is told which
// phoneme is sounding and at what register values, and decides nothing about
// requests, durations or power-down. And the state is audio-only: nothing
// here is in a snapshot, a rewind simply restarts the filters.
//
// Not the default voice. The recorded phonemes are the real chip's output;
// this is a model of it nobody has validated.

#ifndef POM2_SSI263_FORMANT_H
#define POM2_SSI263_FORMANT_H

#include <cstdint>

namespace pom2 {

class Ssi263FormantVoice {
public:
    /// The register state one stretch of samples is rendered with.
    struct Params {
        uint8_t  phoneme      = 0;      ///< $00-$3F
        uint8_t  amplitude    = 0;      ///< A3-A0
        uint8_t  articulation = 0;      ///< T2-T0
        uint16_t inflection   = 0;      ///< I11-I0
        uint8_t  filter       = 0xE6;   ///< FF7-FF0
        bool     sounding     = false;  ///< false: release to silence
        double   xckHz        = 1022727.0;
    };

    void reset();
    /// ADD `frames` samples at `sampleRate` to `out`.
    void render(float* out, int frames, uint32_t sampleRate, const Params& p);
    /// True once the envelope and the filters have rung out.
    bool silent() const { return envLevel_ < 1e-5f; }

    /// [B] the two data-sheet formulas, exposed for the tests.
    static double inflectionHz(uint16_t inflection, double xckHz);
    static double filterClockHz(uint8_t filter, double xckHz);

private:
    void   setRate(uint32_t sampleRate);
    float  resonate(int stage, float in, double centerHz);

    uint32_t rate_ = 0;
    // Per-rate coefficients.
    float  sourcePole_ = 0, noisePole_ = 0, fricPole_ = 0, outPole_ = 0;
    float  levelCoef_ = 0, attackCoef_ = 0, releaseCoef_ = 0;
    double resR_[3] = {0, 0, 0}, fricR_ = 0;

    // State.
    double fCur_[3] = {0, 0, 0};        ///< glided formant centres, Hz
    float  vaCur_ = 0, faCur_ = 0;      ///< glided source levels
    double phase_ = 0;                  ///< glottal oscillator, 0..1
    float  excLp1_ = 0, excLp2_ = 0;
    uint32_t lfsr_ = 0xACE1u;
    float  noiseLp_ = 0;
    float  resY1_[3] = {0, 0, 0}, resY2_[3] = {0, 0, 0};
    float  fricY1_ = 0, fricY2_ = 0;
    float  fricLp_[3] = {0, 0, 0};
    float  radPrev_ = 0;
    float  outLp_[2] = {0, 0};
    float  envLevel_ = 0;
};

}  // namespace pom2

#endif  // POM2_SSI263_FORMANT_H
