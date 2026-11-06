// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ssi263FormantVoice — see Ssi263Formant.h for what this is and how far to
// trust it. The signal path and the phoneme table are a port of Casso's
// `CassoEmuCore/Devices/Ssi263.cpp` @ 2d899adf:
//
//   MIT License — Copyright (c) Robert Elmer
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software and associated documentation files (the
//   "Software"), to deal in the Software without restriction, including
//   without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and/or sell copies of the Software, and to
//   permit persons to whom the Software is furnished to do so, subject to
//   the following conditions: The above copyright notice and this permission
//   notice shall be included in all copies or substantial portions of the
//   Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#include "Ssi263Formant.h"

#include <algorithm>
#include <cmath>

namespace pom2 {

namespace {

constexpr double kPi = 3.14159265358979323846;

/// One phoneme's targets: three formant centres (Hz) and the two source
/// levels, voiced and fricative, kept apart because their RATIO is what
/// tells Z (frication 0.67, voicing 0.07) from L (0 and 0.47) — their
/// formants are nearly the same.
struct Phoneme {
    int   f1, f2, f3;
    float voiced, fric;
};

// [D] Casso's table, copied by script from its source so that no figure was
// retyped. Its provenance (a die-photograph read, an SC-01A frequency curve)
// is in the header. The two closure holds ($2B, $2D) carry no source: the
// closure ramp is not modelled.
constexpr Phoneme kPhonemes[64] = {
    {    0,    0,    0, 0.00f, 0.00f },   // 00 PA   (pause)
    {  314, 2330, 2756, 0.80f, 0.00f },   // 01 E    meet
    {  446, 2330, 2677, 0.67f, 0.00f },   // 02 E1   bent
    {  256, 2257, 2756, 0.73f, 0.00f },   // 03 Y    before
    {  314, 2174, 2511, 0.40f, 0.00f },   // 04 YI   year
    {  365, 2330, 2756, 0.80f, 0.00f },   // 05 AY   please
    {  256, 2408, 2832, 0.60f, 0.00f },   // 06 IE   any
    {  446, 2007, 2598, 0.53f, 0.00f },   // 07 I    six
    {  482, 2096, 2511, 0.53f, 0.00f },   // 08 A    made
    {  482, 1922, 2425, 0.53f, 0.00f },   // 09 AI   care
    {  577, 1823, 2511, 0.53f, 0.00f },   // 0A EH   nest
    {  605, 1922, 2511, 0.53f, 0.00f },   // 0B EH1  belt
    {  683, 1922, 2511, 0.40f, 0.00f },   // 0C AE   dad
    {  731, 1730, 2511, 0.40f, 0.00f },   // 0D AE1  after
    {  731, 1261, 2511, 0.40f, 0.00f },   // 0E AH   got
    {  731, 1387, 2511, 0.47f, 0.00f },   // 0F AH1  father
    {  683, 1106, 2425, 0.40f, 0.00f },   // 10 AW   office
    {  516,  943, 2511, 0.53f, 0.00f },   // 11 O    store
    {  446,  943, 2511, 0.60f, 0.00f },   // 12 OU   boat
    {  546, 1106, 2425, 0.53f, 0.00f },   // 13 OO   look
    {  365, 1620, 2425, 0.67f, 0.00f },   // 14 IU   you
    {  406, 1387, 2425, 0.60f, 0.00f },   // 15 IU1  could
    {  365,  943, 2244, 0.67f, 0.00f },   // 16 U    tune
    {  256,  722, 2142, 0.67f, 0.00f },   // 17 U1   cartoon
    {  546, 1387, 2511, 0.67f, 0.00f },   // 18 UH   wonder
    {  605, 1261, 2511, 0.53f, 0.00f },   // 19 UH1  love
    {  657, 1261, 2511, 0.40f, 0.00f },   // 1A UH2  what
    {  657, 1514, 2511, 0.47f, 0.00f },   // 1B UH3  nut
    {  482, 1387, 1696, 0.53f, 0.00f },   // 1C ER   bird
    {  365,  943, 1424, 0.53f, 0.00f },   // 1D R    roof
    {  314, 1261, 1822, 0.53f, 0.00f },   // 1E R1   rug
    {  516, 1620, 2336, 0.40f, 0.00f },   // 1F R2   mutter
    {  365, 1261, 2756, 0.47f, 0.00f },   // 20 L    lift
    {  256, 1514, 2832, 0.53f, 0.00f },   // 21 L1   play
    {  446,  943, 2756, 0.60f, 0.00f },   // 22 LF   fall
    {  365,  722, 2336, 0.53f, 0.00f },   // 23 W    water
    {  256, 1261, 2598, 0.53f, 0.00f },   // 24 B    bag
    {  256, 1922, 2756, 0.53f, 0.00f },   // 25 D    paid
    {  365, 2007, 2244, 0.67f, 0.00f },   // 26 KV   tag
    {  406, 1106, 2244, 0.00f, 1.00f },   // 27 P    pen
    {  406, 1922, 2756, 0.00f, 1.00f },   // 28 T    tart
    {  365, 2007, 2244, 0.00f, 0.27f },   // 29 K    kit
    {  516, 1922, 2598, 0.40f, 0.00f },   // 2A HV   (hold vocal)
    {  516, 1922, 2598, 0.00f, 0.00f },   // 2B HVC  (hold vocal closure)
    {  516, 1922, 2598, 0.00f, 0.53f },   // 2C HF   heart
    {  516, 1922, 2598, 0.00f, 0.00f },   // 2D HFC  (hold fric closure)
    {  516, 1922, 2598, 0.27f, 0.00f },   // 2E HN   (hold nasal)
    {  365, 1106, 2677, 0.07f, 0.67f },   // 2F Z    zero
    {  176, 1730, 2598, 0.00f, 1.00f },   // 30 S    same
    {  314, 2096, 2756, 0.07f, 0.67f },   // 31 J    measure
    {  314, 2096, 2756, 0.00f, 0.40f },   // 32 SCH  ship
    {  314, 1261, 2336, 0.20f, 0.27f },   // 33 V    very
    {  314, 1261, 2336, 0.00f, 0.27f },   // 34 F    four
    {  365, 1730, 2756, 0.07f, 0.13f },   // 35 THV  there
    {  446, 1823, 2425, 0.00f, 0.13f },   // 36 TH   with
    {  176, 1261, 2336, 0.67f, 0.00f },   // 37 M    more
    {  176, 1823, 2677, 0.53f, 0.00f },   // 38 N    nine
    {  314, 2174, 2756, 0.27f, 0.00f },   // 39 NG   rang
    {  516, 1922, 2425, 0.53f, 0.00f },   // 3A :A   maerchen
    {  314, 1823, 2336, 0.40f, 0.00f },   // 3B :OH  loewe
    {  256, 1730, 2336, 0.67f, 0.00f },   // 3C :U   fuenf
    {  176, 1922, 2425, 0.67f, 0.00f },   // 3D :UH  menu
    {  482, 1730, 2425, 0.47f, 0.00f },   // 3E E2   bitte
    {  256,  943, 2756, 0.53f, 0.00f },   // 3F LB   lube
};

// [F] All of these — Casso's values, tuned by ear and against a recording
// that cannot be consulted. The three corner frequencies are what its bare
// per-sample coefficients (0.07, 0.28, 0.32) came to at 44.1 kHz.
constexpr float  kVoicedGain      = 966.0f;
constexpr float  kNoiseGain       = 0.165f;
constexpr float  kOutputGain      = 5.0f;
constexpr double kSourceBreakHz   = 50.0;
constexpr double kNoiseBreakHz    = 509.0;
constexpr double kFricBreakHz     = 2306.0;
constexpr double kOutputBreakHz   = 2707.0;
constexpr double kBandwidthHz[3]  = { 60.0, 90.0, 120.0 };
constexpr double kFricBandwidthHz = 450.0;
constexpr double kLevelTauSec     = 0.0025;
constexpr double kAttackTauSec    = 0.002;
constexpr double kReleaseTauSec   = 0.004;
/// [B] the data sheet's "typically … about 20KHz" filter clock: the tract's
/// formants are the table's at that clock and scale with it.
constexpr double kNominalFilterHz = 20000.0;
/// [F] limits Casso puts on the two register-driven quantities.
constexpr double kMinPitchHz = 30.0, kMaxPitchHz = 400.0;
constexpr double kMinTract = 0.5, kMaxTract = 2.0;

float onePole(double breakHz, double fs)
{
    return static_cast<float>(1.0 - std::exp(-2.0 * kPi * breakHz / fs));
}
float tauCoef(double tauSec, double fs)
{
    return static_cast<float>(1.0 - std::exp(-1.0 / (tauSec * fs)));
}

}  // namespace

double Ssi263FormantVoice::inflectionHz(uint16_t inflection, double xckHz)
{
    const double divisor = 8.0 * (4096.0 - static_cast<double>(inflection & 0x0FFF));
    return xckHz / divisor;
}

double Ssi263FormantVoice::filterClockHz(uint8_t filter, double xckHz)
{
    return xckHz / (2.0 * (256.0 - static_cast<double>(filter)));
}

void Ssi263FormantVoice::reset()
{
    const uint32_t rate = rate_;
    *this = Ssi263FormantVoice{};
    if (rate) setRate(rate);
}

void Ssi263FormantVoice::setRate(uint32_t sampleRate)
{
    rate_ = sampleRate;
    const double fs = static_cast<double>(sampleRate);
    sourcePole_  = onePole(kSourceBreakHz, fs);
    noisePole_   = onePole(kNoiseBreakHz, fs);
    fricPole_    = onePole(kFricBreakHz, fs);
    outPole_     = onePole(kOutputBreakHz, fs);
    levelCoef_   = tauCoef(kLevelTauSec, fs);
    attackCoef_  = tauCoef(kAttackTauSec, fs);
    releaseCoef_ = tauCoef(kReleaseTauSec, fs);
    for (int i = 0; i < 3; ++i) resR_[i] = std::exp(-kPi * kBandwidthHz[i] / fs);
    fricR_ = std::exp(-kPi * kFricBandwidthHz / fs);
}

// One two-pole resonator, unit gain at DC, so a cascade passes what lies
// below each centre instead of attenuating it.
float Ssi263FormantVoice::resonate(int stage, float in, double centerHz)
{
    const double fs = static_cast<double>(rate_);
    const double fc = std::clamp(centerHz, 50.0, fs * 0.45);
    const double r  = resR_[stage];
    const double b  = 2.0 * r * std::cos(2.0 * kPi * fc / fs);
    const double c  = -(r * r);
    const float  y  = static_cast<float>((1.0 - b - c) * in + b * resY1_[stage] + c * resY2_[stage]);
    resY2_[stage] = resY1_[stage];
    resY1_[stage] = y;
    return y;
}

void Ssi263FormantVoice::render(float* out, int frames, uint32_t sampleRate, const Params& p)
{
    if (frames <= 0 || sampleRate == 0) return;
    if (sampleRate != rate_) setRate(sampleRate);
    const Phoneme& spec = kPhonemes[p.phoneme & 0x3F];
    const bool hasSource = spec.voiced > 0.0f || spec.fric > 0.0f;
    const float envTarget = (p.sounding && hasSource)
        ? static_cast<float>(p.amplitude & 0x0F) / 15.0f : 0.0f;
    // Nothing sounding and nothing left ringing: free.
    if (envTarget <= 0.0f && silent()) return;

    const double fs = static_cast<double>(sampleRate);
    // [B] pitch and tract length straight from the two registers.
    const double pitch = std::clamp(inflectionHz(p.inflection, p.xckHz), kMinPitchHz, kMaxPitchHz);
    const double tract = std::clamp(filterClockHz(p.filter, p.xckHz) / kNominalFilterHz,
                                    kMinTract, kMaxTract);
    // Articulation 0 is the slowest transition, 7 the fastest. [F]: the data
    // sheet says only that the rate is "controlled by the articulation
    // setting" and that the transition is linear; this is Casso's curve.
    const double glide = 1.0 - std::exp(-1.0 /
        ((8.0 - static_cast<double>(p.articulation & 7)) * 0.010 * fs));
    const double target[3] = { static_cast<double>(spec.f1), static_cast<double>(spec.f2),
                               static_cast<double>(spec.f3) };
    const double phaseStep = pitch / fs;
    const float  radScale  = static_cast<float>(fs / 44100.0);

    for (int i = 0; i < frames; ++i) {
        // A pause or a closure has no formants of its own: the tract HOLDS
        // through it, or the next phoneme would sweep up from nothing. A
        // tract starting from silence snaps to its first target.
        if (spec.f1 != 0) {
            if (fCur_[0] <= 0.0) {
                for (int k = 0; k < 3; ++k) fCur_[k] = target[k];
            } else {
                for (int k = 0; k < 3; ++k) fCur_[k] += glide * (target[k] - fCur_[k]);
            }
        }
        // The two source levels glide too, and do NOT hold through a pause.
        vaCur_ += levelCoef_ * (spec.voiced - vaCur_);
        faCur_ += levelCoef_ * (spec.fric   - faCur_);

        // Glottal source: an impulse per period through two one-pole
        // sections. The oscillator runs through unvoiced stretches so a
        // returning vowel resumes mid-cycle.
        float impulse = 0.0f;
        phase_ += phaseStep;
        if (phase_ >= 1.0) { phase_ -= 1.0; impulse = 1.0f; }
        excLp1_ += sourcePole_ * (impulse - excLp1_);
        excLp2_ += sourcePole_ * (excLp1_ - excLp2_);
        float s = excLp2_ * kVoicedGain * vaCur_ *
                  static_cast<float>(731.0 / std::max(fCur_[0], 170.0));

        // The tract: three resonators in cascade, scaled by the filter clock.
        for (int k = 0; k < 3; ++k) s = resonate(k, s, fCur_[k] * tract);

        // Frication beside the tract, not through it: its own broad
        // resonator at F2, three more poles, then summed in.
        if (spec.fric > 0.0f || faCur_ > 0.0001f) {
            const uint32_t bit = lfsr_ & 1u;
            lfsr_ >>= 1;
            if (bit) lfsr_ ^= 0xB400u;
            noiseLp_ += noisePole_ * ((bit ? 1.0f : -1.0f) - noiseLp_);
            const double fc = std::clamp(fCur_[1] * tract, 50.0, fs * 0.45);
            const double b  = 2.0 * fricR_ * std::cos(2.0 * kPi * fc / fs);
            const double c  = -(fricR_ * fricR_);
            const float  y  = static_cast<float>((1.0 - b - c) * noiseLp_ + b * fricY1_ + c * fricY2_);
            fricY2_ = fricY1_;
            fricY1_ = y;
            fricLp_[0] += fricPole_ * (y - fricLp_[0]);
            fricLp_[1] += fricPole_ * (fricLp_[0] - fricLp_[1]);
            fricLp_[2] += fricPole_ * (fricLp_[1] - fricLp_[2]);
            s += fricLp_[2] * kNoiseGain * faCur_;
        }

        // Lip radiation (+6 dB/oct), normalised so its gain at a given
        // frequency does not depend on the host rate; then the output stage.
        const float diffed = (s - radPrev_) * radScale;
        radPrev_ = s;
        outLp_[0] += outPole_ * (diffed - outLp_[0]);
        outLp_[1] += outPole_ * (outLp_[0] - outLp_[1]);

        // Attack and release instead of a gate: every boundary is a short
        // ramp over the resonators' own tail.
        envLevel_ += (envTarget > envLevel_ ? attackCoef_ : releaseCoef_) *
                     (envTarget - envLevel_);
        out[i] += std::clamp(outLp_[1] * envLevel_ * kOutputGain, -1.0f, 1.0f);
    }
}

}  // namespace pom2
