// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SSI 263's formant voice (Ssi263Formant.h), through the chip's own
// register interface. What can be pinned without a reference recording:
//
//   * the two data-sheet formulas (inflection frequency, filter clock);
//   * the property that is the reason for the voice — the inflection
//     register IS the pitch, where the recorded phonemes ignore it;
//   * that voiced and fricative phonemes are different kinds of sound;
//   * that the result does not depend on the host's sample rate;
//   * that it is silent when the chip is, bounded, and deterministic.
//
// What cannot: whether it sounds like the chip. Nothing here measures that.

#include "Ssi263.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using pom2::Ssi263;
using pom2::Ssi263FormantVoice;

namespace {

int failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

constexpr double kXck = 1022727.0;      // the Apple cards' bus clock

/// The 12-bit inflection value for a pitch, by the data-sheet formula.
uint16_t inflectionFor(double hz)
{
    return static_cast<uint16_t>(std::lround(4096.0 - kXck / (8.0 * hz)));
}

/// Program a chip as a speech driver would: out of power-down, phoneme,
/// pitch, typical articulation 5 and the given amplitude, a 20 kHz tract.
void program(Ssi263& chip, uint8_t phoneme, double pitchHz, uint8_t amp = 0x0C,
             uint8_t filter = 0xE6)
{
    const uint16_t infl = inflectionFor(pitchHz);
    chip.write(Ssi263::REG_FILFREQ, filter);
    chip.write(Ssi263::REG_INFLECT, static_cast<uint8_t>((infl >> 3) & 0xFF));
    chip.write(Ssi263::REG_RATEINF, static_cast<uint8_t>(0xA0 | ((infl >> 8) & 0x08) | (infl & 0x07)));
    chip.write(Ssi263::REG_CTTRAMP, static_cast<uint8_t>(0x80 | 0x50 | amp));   // CTL high
    chip.write(Ssi263::REG_DURPHON, static_cast<uint8_t>(0xC0 | phoneme));
    chip.write(Ssi263::REG_CTTRAMP, static_cast<uint8_t>(0x50 | amp));          // CTL low: speak
}

std::vector<float> render(Ssi263& chip, int frames, uint32_t rate)
{
    std::vector<float> out(static_cast<size_t>(frames), 0.0f);
    for (int i = 0; i < frames; i += 256)
        chip.fillAudio(out.data() + i, std::min(256, frames - i), rate);
    return out;
}

double rms(const std::vector<float>& v, size_t from = 0)
{
    double s = 0.0;
    for (size_t i = from; i < v.size(); ++i) s += static_cast<double>(v[i]) * v[i];
    return std::sqrt(s / static_cast<double>(v.size() - from));
}

/// Fundamental by autocorrelation over the steady part, searched 60-450 Hz.
double pitchOf(const std::vector<float>& v, uint32_t rate)
{
    const size_t from = v.size() / 2;
    const int lo = static_cast<int>(rate / 450), hi = static_cast<int>(rate / 60);
    double best = -1e30;
    int bestLag = lo;
    for (int lag = lo; lag <= hi; ++lag) {
        double acc = 0.0;
        for (size_t i = from; i + static_cast<size_t>(lag) < v.size(); ++i)
            acc += static_cast<double>(v[i]) * v[i + static_cast<size_t>(lag)];
        if (acc > best) { best = acc; bestLag = lag; }
    }
    return static_cast<double>(rate) / bestLag;
}

/// How periodic the steady part is at `pitchHz`: the autocorrelation at one
/// period over the energy. Near 1 for a voiced sound, near 0 for a hiss.
double periodicity(const std::vector<float>& v, uint32_t rate, double pitchHz)
{
    const size_t from = v.size() / 2;
    const size_t lag = static_cast<size_t>(std::lround(rate / pitchHz));
    double acc = 0.0, energy = 0.0;
    for (size_t i = from; i + lag < v.size(); ++i) {
        acc    += static_cast<double>(v[i]) * v[i + lag];
        energy += static_cast<double>(v[i]) * v[i];
    }
    return energy > 0.0 ? acc / energy : 0.0;
}

/// Sign changes per second — a crude brightness measure.
double zeroCrossingsPerSecond(const std::vector<float>& v, uint32_t rate)
{
    const size_t from = v.size() / 2;
    int n = 0;
    for (size_t i = from + 1; i < v.size(); ++i)
        if ((v[i - 1] < 0.0f) != (v[i] < 0.0f)) ++n;
    return n * static_cast<double>(rate) / static_cast<double>(v.size() - from);
}

constexpr uint8_t kAH = 0x0E, kS = 0x30, kPA = 0x00;

}  // namespace

int main()
{
    // ── The data sheet's two formulas ──────────────────────────────────
    CHECK(std::fabs(Ssi263FormantVoice::inflectionHz(0, kXck) - kXck / 32768.0) < 1e-9);
    CHECK(std::fabs(Ssi263FormantVoice::inflectionHz(inflectionFor(120.0), kXck) - 120.0) < 0.2);
    CHECK(std::fabs(Ssi263FormantVoice::filterClockHz(0xE6, kXck) - kXck / 52.0) < 1e-9);
    CHECK(std::fabs(Ssi263FormantVoice::filterClockHz(0xE6, kXck) - 20000.0) < 400.0);

    // ── The recorded voice does not hear the inflection register ───────
    Ssi263::setVoice(Ssi263::Voice::Recorded);
    {
        Ssi263 low, high;
        program(low, kAH, 100.0);
        program(high, kAH, 200.0);
        const auto a = render(low, 8192, 44100), b = render(high, 8192, 44100);
        CHECK(rms(a) > 0.001);
        CHECK(a == b);                          // the same take, whatever the pitch
    }

    // ── The formant voice is made of it ────────────────────────────────
    Ssi263::setVoice(Ssi263::Voice::Formant);
    for (double want : { 90.0, 120.0, 180.0, 240.0 }) {
        Ssi263 chip;
        program(chip, kAH, want);
        const auto v = render(chip, 22050, 44100);
        const double got = pitchOf(v, 44100);
        if (std::fabs(got - want) > want * 0.03)
            std::printf("  pitch: asked %.1f Hz, heard %.1f Hz\n", want, got);
        CHECK(std::fabs(got - want) <= want * 0.03);
        CHECK(rms(v, v.size() / 2) > 0.005);
        for (float s : v) CHECK(s >= -1.0f && s <= 1.0f && s == s);
    }

    // A melody: one chip, the pitch rewritten between notes, no other change.
    {
        Ssi263 chip;
        program(chip, kAH, 110.0);
        (void)render(chip, 11025, 44100);
        const uint16_t infl = inflectionFor(165.0);
        chip.write(Ssi263::REG_INFLECT, static_cast<uint8_t>((infl >> 3) & 0xFF));
        chip.write(Ssi263::REG_RATEINF, static_cast<uint8_t>(0xA0 | ((infl >> 8) & 0x08) | (infl & 0x07)));
        const auto v = render(chip, 22050, 44100);
        CHECK(std::fabs(pitchOf(v, 44100) - 165.0) <= 5.0);
    }

    // ── A vowel and a sibilant are different kinds of sound ────────────
    {
        Ssi263 vowel, hiss;
        program(vowel, kAH, 120.0);
        program(hiss, kS, 120.0);
        const auto a = render(vowel, 22050, 44100), s = render(hiss, 22050, 44100);
        CHECK(rms(s, s.size() / 2) > 0.001);
        // The vowel repeats every pitch period; the hiss does not repeat.
        const double pv = periodicity(a, 44100, 120.0), ps = periodicity(s, 44100, 120.0);
        if (!(pv > 0.6 && ps < 0.3))
            std::printf("  periodicity: vowel %.3f, sibilant %.3f\n", pv, ps);
        CHECK(pv > 0.6);
        CHECK(ps < 0.3);
    }

    // ── The amplitude register scales the level; zero is silence ───────
    {
        Ssi263 loud, soft, mute;
        program(loud, kAH, 120.0, 0x0F);
        program(soft, kAH, 120.0, 0x05);
        program(mute, kAH, 120.0, 0x00);
        const double l = rms(render(loud, 22050, 44100), 11025);
        const double q = rms(render(soft, 22050, 44100), 11025);
        CHECK(l > 2.0 * q && q > 0.0);
        CHECK(rms(render(mute, 22050, 44100)) == 0.0);
    }

    // ── The filter register moves the whole tract, not the pitch ───────
    {
        Ssi263 nominal, shorter;
        program(nominal, kAH, 120.0, 0x0C, 0xE6);
        program(shorter, kAH, 120.0, 0x0C, 0xF0);      // a faster filter clock
        const auto a = render(nominal, 22050, 44100), b = render(shorter, 22050, 44100);
        CHECK(std::fabs(pitchOf(a, 44100) - pitchOf(b, 44100)) < 3.0);
        CHECK(zeroCrossingsPerSecond(b, 44100) > zeroCrossingsPerSecond(a, 44100) * 1.1);
    }

    // ── Silence when the chip is silent ────────────────────────────────
    {
        Ssi263 chip;                                   // power-on: power-down
        CHECK(rms(render(chip, 4096, 44100)) == 0.0);
        program(chip, kPA, 120.0);                     // the pause phoneme
        CHECK(rms(render(chip, 4096, 44100)) == 0.0);
        program(chip, kAH, 120.0);
        CHECK(rms(render(chip, 8192, 44100)) > 0.001);
        // Power-down releases the voice instead of cutting it: it decays to
        // nothing within a few tens of milliseconds, and stays there.
        chip.write(Ssi263::REG_CTTRAMP, 0x80 | 0x5C);
        (void)render(chip, 4410, 44100);
        CHECK(rms(render(chip, 4096, 44100)) < 1e-4);
        // FILFREQ = $FF is silence too, as for the recorded voice.
        program(chip, kAH, 120.0, 0x0C, 0xFF);
        (void)render(chip, 4410, 44100);
        CHECK(rms(render(chip, 4096, 44100)) < 1e-4);
    }

    // ── The host's sample rate is not part of the sound ────────────────
    {
        Ssi263 a, b;
        program(a, kAH, 130.0);
        program(b, kAH, 130.0);
        const auto at44 = render(a, 22050, 44100), at48 = render(b, 24000, 48000);
        CHECK(std::fabs(pitchOf(at44, 44100) - pitchOf(at48, 48000)) < 3.0);
        const double r44 = rms(at44, at44.size() / 2), r48 = rms(at48, at48.size() / 2);
        if (!(r48 > r44 * 0.7 && r48 < r44 * 1.4))
            std::printf("  level at 44.1 kHz %.4f, at 48 kHz %.4f\n", r44, r48);
        CHECK(r48 > r44 * 0.7 && r48 < r44 * 1.4);
        const double z44 = zeroCrossingsPerSecond(at44, 44100), z48 = zeroCrossingsPerSecond(at48, 48000);
        CHECK(std::fabs(z44 - z48) < 0.25 * z44);
    }

    // ── Deterministic: the same registers give the same samples ────────
    {
        Ssi263 a, b;
        program(a, kS, 120.0);
        program(b, kS, 120.0);
        CHECK(render(a, 8192, 44100) == render(b, 8192, 44100));
    }

    Ssi263::setVoice(Ssi263::Voice::Recorded);
    if (failures) { std::printf("ssi263_formant: %d check(s) failed\n", failures); return 1; }
    std::printf("ssi263_formant: OK\n");
    return 0;
}
