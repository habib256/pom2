// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// AudioBufferLadder — the device-buffer ladder's STATE: when to climb a rung,
// what was learned, what a restart starts from.
//
// `AudioBufferPolicy.h` holds the rungs and the escalation rule as pure
// functions; the state that drives them — the late-callback window, the
// highest rung reached, the pin, the browser's one-climb-per-load — lived as
// five fields inside `AudioCoordinator`, read through the real `AudioDevice`
// and the wall clock. No test could reach it: the 2026-11-01 mutation audit
// found the climb, `learnedFrames_` and `audio_buffer_learned` touched by
// none, so the crackle on a Raspberry Pi or a phone the ladder exists to cure
// could come back with the suite green. This is that state with no device and
// no clock in it — same shape and same reason as `AudioDeviceWatch.h`,
// `MouseGrab.h` and `KeyChord.h`. `AudioCoordinator` feeds it the device's
// counters and applies the rung it answers.
//
// [F] The window and the threshold (`AudioBufferPolicy::kWindowSec`,
//     `kLateToEscalate`) are judgement, not measurement — see that file.

#ifndef POM2_AUDIO_BUFFER_LADDER_H
#define POM2_AUDIO_BUFFER_LADDER_H

#include "AudioBufferPolicy.h"

#include <algorithm>
#include <cstdint>

namespace pom2 {

class AudioBufferLadder
{
public:
    using Policy   = AudioBufferPolicy;
    using Platform = AudioBufferPolicy::Platform;

    /// A rung to open the device at, and whether it is a pin. frames == 0:
    /// nothing to apply.
    struct Rung {
        std::uint32_t frames = 0;
        bool          pinned = false;
    };

    /// A climb `poll` decided on, with what the log line says about it.
    struct Climb {
        std::uint32_t frames       = 0;   ///< 0 = stay
        std::uint64_t lateInWindow = 0;
        double        windowSec    = 0.0;
    };

    /// From the two settings (`audio_buffer_frames`, `audio_buffer_learned`).
    /// A pin wins; otherwise the rung learned on an earlier run, when it is
    /// above the device's current one, so a Pi that climbed to 2048 frames
    /// yesterday does not crackle through the climb again today. A value
    /// that is not a rung of this platform is stale (the rungs moved) and is
    /// dropped.
    Rung restore(int pinnedSetting, int learnedSetting,
                 std::uint32_t currentFrames, Platform p)
    {
        pinned_  = static_cast<std::uint32_t>(std::max(0, pinnedSetting));
        learned_ = static_cast<std::uint32_t>(std::max(0, learnedSetting));
        if (!Policy::isRung(pinned_, p))  pinned_  = 0;
        if (!Policy::isRung(learned_, p)) learned_ = 0;
        if (pinned_) return {pinned_, true};
        if (learned_ > currentFrames) return {learned_, false};
        return {};
    }

    /// The Audio panel's pin. 0 = back to automatic. Returns false, and
    /// changes nothing, for a value that is not a rung of this platform.
    /// Back to automatic, the ladder starts from the current rung and counts
    /// late callbacks afresh.
    bool pin(std::uint32_t frames, std::uint32_t currentFrames,
             std::uint64_t lateNow, Platform p)
    {
        if (frames != 0 && !Policy::isRung(frames, p)) return false;
        pinned_ = frames;
        if (frames == 0) {
            learned_ = std::max(learned_, currentFrames);
            restartWindow(lateNow);
        }
        return true;
    }

    /// The device was opened again (a new rung, a re-route): its late
    /// callbacks are not the old one's. `lateNow` is the counter now.
    void restartWindow(std::uint64_t lateNow)
    {
        lateAtWindowStart_ = lateNow;
        windowStartSec_    = -1.0;
    }
    /// Same, when the counter itself is not known yet (a re-open in flight).
    void forgetWindow() { windowStartSec_ = -1.0; }

    /// Once per UI frame. `lateNow` is the device's running count of late
    /// callbacks, `nowSec` any monotonic clock. Climbs one rung when the
    /// window saw `kLateToEscalate` of them; nothing while pinned, while no
    /// device is open, or — in the browser, where a rung applies at the next
    /// load — after the first climb of this session, because the late
    /// callbacks that follow are still the OLD buffer's.
    Climb poll(double nowSec, std::uint64_t lateNow, std::uint32_t currentFrames,
               Platform p, bool available, bool appliesAtNextLoad)
    {
        if (pinned_ != 0 || !available || climbDeferred_) return {};
        if (windowStartSec_ < 0.0 || nowSec - windowStartSec_ > Policy::kWindowSec) {
            windowStartSec_    = nowSec;
            lateAtWindowStart_ = lateNow;
            return {};
        }
        const std::uint64_t lateInWindow = lateNow - lateAtWindowStart_;
        const std::uint32_t next =
            Policy::escalatedPeriodFrames(currentFrames, lateInWindow, p);
        if (next == currentFrames) return {};
        learned_ = std::max(learned_, next);
        if (appliesAtNextLoad) climbDeferred_ = true;
        return {next, lateInWindow, nowSec - windowStartSec_};
    }

    std::uint32_t pinned()  const { return pinned_; }
    std::uint32_t learned() const { return learned_; }

private:
    std::uint32_t pinned_            = 0;   ///< 0 = automatic
    std::uint32_t learned_           = 0;   ///< highest rung the ladder reached
    std::uint64_t lateAtWindowStart_ = 0;
    double        windowStartSec_    = -1.0;
    bool          climbDeferred_     = false;
};

} // namespace pom2

#endif // POM2_AUDIO_BUFFER_LADDER_H
