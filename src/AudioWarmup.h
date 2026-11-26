// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// AudioWarmup — wide at start-up, then adjusted: the device buffer and the
// speaker's replay lead while POM2 starts, and when they come down.
//
// Start-up is when POM2 is least able to keep audio fed and most likely to
// be heard: the window compiles its shaders, uploads its textures, mounts
// and reads the boot disk while the machine beeps and the drive spins. Two
// things scratch then. The CPU worker stalls for longer than the speaker's
// lead (SpeakerDevice::setRealtimeLead), so the audio cursor overtakes its
// toggles and they are purged; and the host's audio thread misses a period
// of a device buffer opened at its smallest (AudioBufferPolicy: 256 frames,
// 16 ms on a desktop), an underrun. Neither was in the steady state — both
// were the first seconds.
//
// So the first seconds run WIDE: a 0.30 s speaker lead, and the device one
// rung up from at least 1024 frames (64 ms at 48 kHz). The late callbacks of
// that phase teach the ladder nothing (AudioCoordinator holds it) — a
// start-up hiccup used to climb a rung and leave it learned for good. Once
// the machine has run STEADY for kSteadySec — no UI frame over
// kSlowFrameSec, no late callback — the lead comes down to its 0.10 s at
// once (the speaker applies it at the next silence, so nothing is heard)
// and the device goes back to its own rung at the first QUIET moment, the
// output under kQuietPeak for kQuietSec: re-opening a device is a short
// hole, and a hole in silence is not one. A machine that never falls
// silent (a demo's music from the first second — DIX on the //c PAL) would
// keep the wide buffer, and its latency, for the whole session: so the
// threshold doubles every kQuietRelaxSec of waiting, up to kLoudestQuiet,
// and after kMaxQuietWaitSec the device comes down at once — one short
// hole, once, rather than 64 ms more latency for good.
//
// The browser cannot re-open its device before the next load
// (AudioDevice::appliesAtNextLoad) and starts at 2048 frames anyway; a
// pinned rung is the user's: neither gets the device half. Both get the
// lead.
//
// [F] Every constant here is judgement, not measurement: 0.30 s covers the
//     start-up stalls the 0.10 s lead lost (c5b38397's diagnosis, and the
//     user's report that it still scratched); 3 s steady and 0.25 s quiet
//     are what a listener would not notice. Settled by: a log of the worker
//     stalls and late callbacks over the first ten seconds on slow hosts
//     (a Pi 4, a phone).

#ifndef POM2_AUDIO_WARMUP_H
#define POM2_AUDIO_WARMUP_H

#include "AudioBufferPolicy.h"

#include <algorithm>
#include <cstdint>

namespace pom2 {

class AudioWarmup
{
public:
    using Policy   = AudioBufferPolicy;
    using Platform = AudioBufferPolicy::Platform;

    static constexpr double kWarmLeadSec   = 0.30;
    static constexpr double kSteadyLeadSec = 0.10;   // SpeakerDevice::kDefaultLeadSecs
    static constexpr double kSteadySec     = 3.0;
    static constexpr double kSlowFrameSec  = 0.05;   // as MainWindow::powerOnWhenReady
    static constexpr double kQuietSec      = 0.25;
    static constexpr float  kQuietPeak     = 0.002f;
    static constexpr double kQuietRelaxSec = 5.0;
    static constexpr float  kLoudestQuiet  = 0.10f;
    static constexpr double kMaxQuietWaitSec = 30.0;

    /// The peak that counts as quiet after `waitedSec` of waiting for it.
    static float quietPeakAfter(double waitedSec)
    {
        float peak = kQuietPeak;
        for (double t = kQuietRelaxSec; t <= waitedSec && peak < kLoudestQuiet; t += kQuietRelaxSec)
            peak *= 2.0f;
        return std::min(peak, kLoudestQuiet);
    }
    static constexpr std::uint32_t kWarmMinFrames = 1024;

    /// The rung the device runs at while warm: one up from its own, and at
    /// least 1024 frames, within the platform's ladder. 0 for the browser,
    /// which cannot re-open its device at run time.
    static constexpr std::uint32_t warmPeriodFrames(Platform p, std::uint32_t target)
    {
        if (p == Platform::Browser || p == Platform::BrowserMobile) return 0;
        const std::uint32_t up = std::max(Policy::nextPeriodFrames(target, p), kWarmMinFrames);
        return std::min(up, Policy::maxPeriodFrames(p));
    }

    /// What `begin` and `poll` ask for. 0 = leave it.
    struct Action {
        double        leadSec      = 0.0;
        std::uint32_t deviceFrames = 0;
    };

    /// The warm phase starts (power-on, a profile switch). `deviceTarget` is
    /// the rung the device belongs at; `deviceWarm` the rung it was opened at
    /// for the start-up (0 = the device is not part of the warm-up: browser,
    /// pin, or already brought down and only the lead re-warms).
    Action begin(double nowSec, std::uint32_t deviceTarget, std::uint32_t deviceWarm)
    {
        lead_         = kWarmLeadSec;
        target_       = deviceTarget;
        if (deviceWarm > deviceTarget) deviceDown_ = true;
        steadySince_  = nowSec;
        quietSince_   = -1.0;
        lastLateSeen_ = false;
        Action a;
        a.leadSec = lead_;
        return a;
    }

    /// The ladder moved the device's rung (a climb, a pin): from now on that
    /// is where it belongs, and nothing here brings it down.
    void deviceSettled(std::uint32_t frames)
    {
        target_     = frames;
        deviceDown_ = false;
    }

    /// Once per UI frame: `frameSec` is the time since the previous poll,
    /// `lateNow` the device's running count of late callbacks, `peak` the
    /// mixed output's peak (AudioDevice::getMasterPeak).
    Action poll(double nowSec, double frameSec, std::uint64_t lateNow, float peak)
    {
        Action a;
        if (!warm()) return a;
        if (!lastLateSeen_) { lastLate_ = lateNow; lastLateSeen_ = true; }
        if (frameSec > kSlowFrameSec || lateNow != lastLate_) steadySince_ = nowSec;
        lastLate_ = lateNow;
        if (lead_ > kSteadyLeadSec && nowSec - steadySince_ >= kSteadySec) {
            lead_ = kSteadyLeadSec;
            a.leadSec = lead_;
            downWaitSince_ = nowSec;
            quietSince_ = -1.0;
        }
        if (!deviceDown_ || lead_ > kSteadyLeadSec) return a;
        const double waited = nowSec - downWaitSince_;
        if (peak < quietPeakAfter(waited)) { if (quietSince_ < 0.0) quietSince_ = nowSec; }
        else quietSince_ = -1.0;
        if ((quietSince_ >= 0.0 && nowSec - quietSince_ >= kQuietSec) ||
            waited >= kMaxQuietWaitSec) {
            deviceDown_ = false;
            a.deviceFrames = target_;
        }
        return a;
    }

    /// Still widened: the lead, or the device waiting for a quiet moment.
    /// The ladder does not climb meanwhile.
    bool warm() const { return lead_ > kSteadyLeadSec || deviceDown_; }
    double leadSec() const { return lead_; }
    std::uint32_t deviceTarget() const { return target_; }
    bool deviceWaitsForQuiet() const { return deviceDown_; }

private:
    double        lead_         = kSteadyLeadSec;
    std::uint32_t target_       = 0;
    bool          deviceDown_   = false;
    double        steadySince_  = 0.0;
    double        quietSince_   = -1.0;
    double        downWaitSince_ = 0.0;   ///< when the device began waiting to come down
    std::uint64_t lastLate_     = 0;
    bool          lastLateSeen_ = false;
};

} // namespace pom2

#endif // POM2_AUDIO_WARMUP_H
