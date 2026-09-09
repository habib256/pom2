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

// The printer grain scheduler must THIN a burst, not STACK it — bug hunt #13.
//
// `PrinterSoundDevice::schedule` places each grain on an audio-frame cursor
// that may lead the clock by at most kMaxAheadSeconds (0.2 s). Past that the
// grain is dropped: that early return is the documented thinning. It did not
// thin, because the cursor was CLAMPED to the cap
// (`nextGrainFrame_ = min(start + spacing, maxAhead)`) while the gate that
// drops was strictly greater (`if (start > maxAhead) return;`). Once a burst
// pushed the cursor onto the cap the gate never fired again and every later
// grain was scheduled on the IDENTICAL frame.
//
// One `carriageReturn(8.0)` was enough on its own: its spacing is the sweep's
// own length (8 in / 15 in·s⁻¹ = 0.53 s), far past the 0.2 s cap, so it parked
// the cursor there. `ImageWriter::tick` then drains a whole line of characters
// in ONE UI frame — `Speed::Instant` and the catch-up path both do — and the
// audio frame counter does not move between those `strike()` calls, so all of
// them landed on that one frame and attacked together.
//
// The observable is a single-frame step in the rendered stream. Measured at
// the shipped default volume (0.35): 0.44 full-scale before the fix, which is
// 4x `AudioSource::kClickThreshold`, the bar the Audio panel's own crackle
// detector uses. After: 0.01.
//
// The fix is the gate: `>=` instead of `>`. With it the cursor is strictly
// increasing, so no two grains can share a start frame.

#include "AudioSource.h"          // kClickThreshold — the app's own bar
#include "PrinterSoundDevice.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
void fail(const std::string& what)
{
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
}

constexpr uint32_t kRate  = 44100;
constexpr int      kBlock = 256;
/// The device's shipped default (AudioCoordinator::restore).
constexpr float    kVol   = 0.35f;

struct Render { float maxStep = 0.0f; float peak = 0.0f; double atSec = 0.0; };

/// Render `seconds` with no further input and report the worst single-frame
/// step. A pile-up shows up here and nowhere else: the grains are noise, so
/// the sum is only loud, but their attacks coincide to the frame.
Render drain(pom2::PrinterSoundDevice& d, double seconds)
{
    Render r;
    std::vector<float> buf(kBlock);
    float prev = 0.0f;
    const int blocks = static_cast<int>(seconds * kRate / kBlock);
    for (int b = 0; b < blocks; ++b) {
        std::fill(buf.begin(), buf.end(), 0.0f);
        d.fillAudioBuffer(buf.data(), kBlock);
        for (int i = 0; i < kBlock; ++i) {
            const float v  = buf[static_cast<size_t>(i)];
            const float dv = std::fabs(v - prev);
            if (dv > r.maxStep) {
                r.maxStep = dv;
                r.atSec = (b * static_cast<double>(kBlock) + i) / kRate;
            }
            if (std::fabs(v) > r.peak) r.peak = std::fabs(v);
            prev = v;
        }
    }
    return r;
}

void check(const char* what, const Render& r)
{
    std::printf("  %-46s maxStep %.4f @%.3f s  peak %.4f\n",
                what, static_cast<double>(r.maxStep), r.atSec,
                static_cast<double>(r.peak));
    if (r.maxStep > AudioSource::kClickThreshold)
        fail(std::string(what) + ": single-frame step " +
             std::to_string(r.maxStep) + " past the crackle bar " +
             std::to_string(AudioSource::kClickThreshold));
    if (r.peak > 0.5f)
        fail(std::string(what) + ": peak " + std::to_string(r.peak) +
             " — grains are summing, not thinning");
}

}  // namespace

int main()
{
    // 1. THE case: one carriage return parks the cursor on the cap, then a
    //    whole line arrives inside one ImageWriter tick (no audio callback
    //    in between, so the frame counter does not move).
    {
        pom2::PrinterSoundDevice d;
        d.setSampleRate(kRate);
        d.setVolume(kVol);
        d.carriageReturn(8.0);
        for (int c = 0; c < 200; ++c) d.strike(9);
        check("carriage return + 200 strikes, one tick", drain(d, 1.5));
    }
    // 2. A dense burst with no sweep in front of it: the cursor reaches the
    //    cap on its own after ~40 characters at 5 ms spacing.
    {
        pom2::PrinterSoundDevice d;
        d.setSampleRate(kRate);
        d.setVolume(kVol);
        for (int c = 0; c < 200; ++c) d.strike(9);
        check("200 strikes, one tick", drain(d, 1.5));
    }
    // 3. A form feed's worth of platen grains, same shape.
    {
        pom2::PrinterSoundDevice d;
        d.setSampleRate(kRate);
        d.setVolume(kVol);
        for (int l = 0; l < 66; ++l) d.paperFeed(1.0 / 6.0);
        check("66 line feeds, one tick", drain(d, 2.0));
    }

    if (g_failures) return 1;
    std::puts("printer_grain_pileup OK");
    return 0;
}
