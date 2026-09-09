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

// PrinterSoundDevice implementation — see the header for the grain model and
// why the scheduling cursor's cap is load-bearing.
//
// Values below are the reference's (web-a2e `printer-sound.js`), kept rather
// than re-tuned: they came from spectral measurements of real dot-matrix
// printers, and guessing differently would only make it sound less like one.

#include "PrinterSoundDevice.h"

#include <algorithm>
#include <cmath>

namespace pom2 {

namespace {

constexpr double kPi = 3.14159265358979323846;

// ── Grain shapes ─────────────────────────────────────────────────────────
// A pin strike is a short bright clack; a line feed is the platen, so it is
// longer, lower and narrower-Q. "Wide Q" matters: it is what keeps these
// broadband clicks instead of beeps.
constexpr double kCharDur     = 0.011;   // seconds
constexpr double kCharFreq    = 1500.0;
constexpr double kCharQ       = 0.9;
constexpr double kCharHp      = 600.0;
constexpr double kCharPeak    = 0.70;
constexpr double kCharSpacing = 0.005;   // grain-to-grain on the timeline
constexpr double kCharJitter  = 220.0;

constexpr double kLineDur     = 0.040;
constexpr double kLineFreq    = 950.0;
constexpr double kLineQ       = 1.4;
constexpr double kLineHp      = 300.0;
constexpr double kLinePeak    = 0.85;
constexpr double kLineSpacing = 0.022;
constexpr double kLineJitter  = 80.0;

/// The carriage sweep is one long grain, not a click.
constexpr double kReturnFreq    = 700.0;
constexpr double kReturnQ       = 2.2;
constexpr double kReturnHp      = 250.0;
constexpr double kReturnPeak    = 0.55;
constexpr double kReturnMinDur  = 0.03;
constexpr double kReturnMaxDur  = 0.50;
/// Real return speed: about 15 in/s, so a full 8" line takes just over half
/// a second — which is why the clamp above is where it is.
constexpr double kReturnSecondsPerInch = 1.0 / 15.0;

/// How far ahead of the clock the cursor may run. A dense burst thins past
/// this instead of queueing minutes of buzz.
constexpr double kMaxAheadSeconds = 0.20;

/// Attack of every grain: near-instant, which is what makes it an impact.
constexpr double kAttackSeconds = 0.0008;
/// Envelope floor the decay aims at, matching the reference's exponential
/// ramp target.
constexpr double kEnvFloor = 0.0001;

} // namespace

// ── Biquads (RBJ cookbook) ───────────────────────────────────────────────

void PrinterSoundDevice::Biquad::bandpass(double f0, double q, double fs)
{
    const double w0    = 2.0 * kPi * f0 / fs;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double a0    = 1.0 + alpha;
    // Constant skirt gain — peak gain = Q, which keeps the wide-Q grains
    // from collapsing in level as the jitter moves the centre.
    b0 = static_cast<float>( alpha / a0);
    b1 = 0.0f;
    b2 = static_cast<float>(-alpha / a0);
    a1 = static_cast<float>(-2.0 * std::cos(w0) / a0);
    a2 = static_cast<float>((1.0 - alpha) / a0);
    x1 = x2 = y1 = y2 = 0.0f;
}

void PrinterSoundDevice::Biquad::highpass(double f0, double fs)
{
    const double w0    = 2.0 * kPi * f0 / fs;
    const double alpha = std::sin(w0) / (2.0 * 0.707);
    const double c     = std::cos(w0);
    const double a0    = 1.0 + alpha;
    b0 = static_cast<float>(((1.0 + c) / 2.0) / a0);
    b1 = static_cast<float>((-(1.0 + c)) / a0);
    b2 = b0;
    a1 = static_cast<float>((-2.0 * c) / a0);
    a2 = static_cast<float>((1.0 - alpha) / a0);
    x1 = x2 = y1 = y2 = 0.0f;
}

// ─────────────────────────────────────────────────────────────────────────

PrinterSoundDevice::PrinterSoundDevice() = default;

void PrinterSoundDevice::setSampleRate(uint32_t hz)
{
    std::lock_guard<std::mutex> lk(mtx_);
    sampleRate_ = (hz > 0) ? hz : kAudioSampleRate;
    // Grains in flight were built for the old rate; drop them rather than
    // letting them play at the wrong pitch and length.
    for (auto& g : grains_) g.active = false;
    nextGrainFrame_ = frameCounter_.load(std::memory_order_relaxed);
}

void PrinterSoundDevice::setVolume(float v)
{
    volume_.store(std::clamp(v, 0.0f, 2.0f), std::memory_order_relaxed);
}

float PrinterSoundDevice::noise()
{
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(static_cast<int32_t>(rng_)) * (1.0f / 2147483648.0f);
}

int PrinterSoundDevice::activeGrains() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    int n = 0;
    for (const auto& g : grains_) if (g.active) ++n;
    return n;
}

// ── UI thread: schedule a grain ──────────────────────────────────────────

void PrinterSoundDevice::schedule(double durSeconds, double freqHz, double q,
                                  double hpHz, double peak,
                                  double spacingSeconds, double jitterHz)
{
    if (peak <= 0.0) return;

    std::lock_guard<std::mutex> lk(mtx_);
    if (!powered_) return;

    const double   fs      = static_cast<double>(sampleRate_);
    const uint64_t now     = frameCounter_.load(std::memory_order_relaxed);
    const uint64_t maxAhead = now + static_cast<uint64_t>(kMaxAheadSeconds * fs);

    // The cursor never goes backwards past the clock, and never runs more
    // than kMaxAheadSeconds in front of it. A burst denser than the printer
    // can voice therefore THINS — this early return is that thinning.
    //
    // The test is `>=`, not `>`, and that is load-bearing (bug hunt #13).
    // The clamp below pins the cursor EXACTLY on `maxAhead` once a burst
    // outruns the clock; with a strict `>` the pinned cursor still passed
    // this gate, so every further grain was scheduled on the identical
    // frame and the burst STACKED instead of thinning. A single
    // `carriageReturn(8.0)` was enough — its spacing is the sweep's own
    // length (0.53 s), far past the 0.2 s cap — and the 47 characters that
    // followed it inside one ImageWriter tick all attacked together: a
    // 0.44 full-scale step at the default volume 0.35, four times the
    // crackle bar. With `>=` the cursor is strictly increasing, so no two
    // grains can ever share a start frame.
    uint64_t start = std::max(now, nextGrainFrame_);
    if (start >= maxAhead) return;
    nextGrainFrame_ = std::min(start + static_cast<uint64_t>(spacingSeconds * fs),
                               maxAhead);

    Grain* slot = nullptr;
    for (auto& g : grains_) if (!g.active) { slot = &g; break; }
    if (!slot) return;                  // pool full: thin from the other end

    const double jitter = static_cast<double>(noise()) * jitterHz;
    const double f0     = std::max(60.0, freqHz + jitter);

    slot->start = start;
    slot->end   = start + static_cast<uint64_t>(durSeconds * fs);
    slot->bp.bandpass(f0, q, fs);
    slot->hp.highpass(hpHz, fs);
    slot->peak = static_cast<float>(peak);

    // Near-instant attack, then an exponential decay that lands on the floor
    // exactly at `end` — the reference's two ramps, expressed per frame.
    const double attackFrames = std::max(1.0, kAttackSeconds * fs);
    const double decayFrames  = std::max(1.0, (durSeconds - kAttackSeconds) * fs);
    slot->attackStep = static_cast<float>(peak / attackFrames);
    slot->decayK     = static_cast<float>(
        std::pow(kEnvFloor / std::max(peak, 1e-6), 1.0 / decayFrames));
    slot->env       = 0.0f;
    slot->attacking = true;
    slot->active    = true;
}

void PrinterSoundDevice::strike(int pins)
{
    if (pins <= 0) return;
    // Loudness follows how many wires fired: a full stop and a 'W' are not
    // the same impact.
    const double amp = std::clamp(static_cast<double>(pins) / 9.0, 0.15, 1.0);
    schedule(kCharDur, kCharFreq, kCharQ, kCharHp, kCharPeak * amp,
             kCharSpacing, kCharJitter);
}

void PrinterSoundDevice::paperFeed(double inches)
{
    inches = std::fabs(inches);
    if (inches <= 0.0) return;
    // One line feed = one platen grain. A long feed (form feed) is louder,
    // not longer: the reference's line grain is a fixed 40 ms.
    const double amp = std::clamp(inches / (1.0 / 6.0), 0.4, 1.0);
    schedule(kLineDur, kLineFreq, kLineQ, kLineHp, kLinePeak * amp,
             kLineSpacing, kLineJitter);
}

void PrinterSoundDevice::carriageReturn(double inches)
{
    inches = std::fabs(inches);
    if (inches < 0.05) return;          // a short hop is not a sweep
    const double dur = std::clamp(inches * kReturnSecondsPerInch,
                                  kReturnMinDur, kReturnMaxDur);
    const double amp = std::clamp(inches / 8.0, 0.35, 1.0);
    // Spacing equals the duration: the sweep owns the cursor for its whole
    // length, so characters printed "during" it queue behind rather than
    // stacking on top.
    schedule(dur, kReturnFreq, kReturnQ, kReturnHp, kReturnPeak * amp,
             dur, 40.0);
}

void PrinterSoundDevice::power(bool on)
{
    std::lock_guard<std::mutex> lk(mtx_);
    powered_ = on;
    if (!on) {
        // Switching off silences the mechanism at once — no coasting.
        for (auto& g : grains_) g.active = false;
        nextGrainFrame_ = frameCounter_.load(std::memory_order_relaxed);
    }
}

// ── Audio thread ─────────────────────────────────────────────────────────

void PrinterSoundDevice::fillAudioBuffer(float* output, int frameCount)
{
    if (frameCount <= 0) return;

    const float vol = muted_.load(std::memory_order_relaxed)
                          ? 0.0f
                          : volume_.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lk(mtx_);
    const uint64_t base = frameCounter_.load(std::memory_order_relaxed);

    for (auto& g : grains_) {
        if (!g.active) continue;

        for (int i = 0; i < frameCount; ++i) {
            const uint64_t f = base + static_cast<uint64_t>(i);
            if (f < g.start) continue;          // scheduled ahead of this buffer
            if (f >= g.end) { g.active = false; break; }

            // Attack to the peak, then exponential decay to the floor.
            //
            // The phase is a FLAG, not `env < peak`. Every grain here has an
            // attack step bigger than one decay step (a char grain gains
            // 0.0198 per frame and loses 0.0136), so the comparison flipped
            // straight back to attack the instant a decay step lowered env —
            // the envelope locked into a two-frame oscillation just under the
            // peak and every voice became a flat-top burst cut off dead at
            // `end`. Only a grain shorter than ~7.5 ms would ever have decayed,
            // and the shortest constant here is 11 ms.
            if (g.attacking) {
                g.env += g.attackStep;
                if (g.env >= g.peak) { g.env = g.peak; g.attacking = false; }
            } else {
                g.env *= g.decayK;
            }

            const float n = noise();
            const float s = g.hp.run(g.bp.run(n)) * g.env;
            if (vol > 0.0f) output[i] += s * vol;
        }
    }

    frameCounter_.store(base + static_cast<uint64_t>(frameCount),
                        std::memory_order_relaxed);
}

} // namespace pom2
