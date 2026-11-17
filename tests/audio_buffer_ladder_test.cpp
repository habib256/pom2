// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pins AudioBufferLadder.h: the device-buffer ladder's climb, what it
// learns and what a restart starts from. The 2026-11-01 mutation audit found
// none of it reached by a test (the state lived in AudioCoordinator, behind
// the real device and the wall clock), so the Raspberry Pi / phone crackle the
// ladder exists to cure could return with the suite green. Every case below
// drives the ladder with a fake clock and a fake late-callback counter.

#include "AudioBufferLadder.h"

#include <cstdio>

using pom2::AudioBufferLadder;
using Platform = pom2::AudioBufferPolicy::Platform;

namespace {

int failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

constexpr double kWindow = pom2::AudioBufferPolicy::kWindowSec;
constexpr unsigned kLate = pom2::AudioBufferPolicy::kLateToEscalate;

void climbsOnLateCallbacks()
{
    AudioBufferLadder l;
    // The first poll only opens the window.
    CHECK(l.poll(0.0, 0, 256, Platform::Desktop, true, false).frames == 0);
    // Below the threshold inside the window: stay.
    CHECK(l.poll(1.0, kLate - 1, 256, Platform::Desktop, true, false).frames == 0);
    // At the threshold: one rung up — proposed, not yet remembered: only a
    // device that took it makes it the next launch's start.
    const auto c = l.poll(2.0, kLate, 256, Platform::Desktop, true, false);
    CHECK(c.frames == 512);
    CHECK(c.lateInWindow == kLate);
    CHECK(c.windowSec == 2.0);
    CHECK(l.learned() == 0 && "a rung the device has not taken is not learned");
    l.reached(c.frames);
    CHECK(l.learned() == 512);
}

void windowExpires()
{
    AudioBufferLadder l;
    l.poll(0.0, 0, 256, Platform::Desktop, true, false);
    l.poll(1.0, kLate - 1, 256, Platform::Desktop, true, false);
    // Past the window the count starts again from the counter NOW: the
    // late callbacks of the old window do not add up with the new one's.
    CHECK(l.poll(kWindow + 1.0, kLate - 1, 256, Platform::Desktop, true, false).frames == 0);
    CHECK(l.poll(kWindow + 2.0, 2 * (kLate - 1), 256, Platform::Desktop, true, false).frames == 0);
    CHECK(l.poll(kWindow + 3.0, kLate - 1 + kLate, 256, Platform::Desktop, true, false).frames == 512);
}

void restartedWindowForgetsTheOldDevice()
{
    AudioBufferLadder l;
    l.poll(0.0, 10, 256, Platform::Desktop, true, false);
    // A re-open: what came before is the old device's. The next poll opens
    // the window and is its baseline, however many late callbacks the
    // counter shows by then (12 + kLate here: none of them may count).
    l.restartWindow();
    CHECK(l.poll(1.0, 12 + kLate, 256, Platform::Desktop, true, false).frames == 0);
    CHECK(l.poll(2.0, 12 + kLate + kLate - 1, 256, Platform::Desktop, true, false).frames == 0);
    CHECK(l.poll(3.0, 12 + kLate + kLate, 256, Platform::Desktop, true, false).frames == 512);
}

void stopsAtTheTop()
{
    AudioBufferLadder l;
    l.poll(0.0, 0, 2048, Platform::Desktop, true, false);
    CHECK(l.poll(1.0, 100, 2048, Platform::Desktop, true, false).frames == 0);
    AudioBufferLadder b;
    b.poll(0.0, 0, 4096, Platform::Browser, true, false);
    CHECK(b.poll(1.0, 100, 4096, Platform::Browser, true, false).frames == 8192);
}

void noClimbWhilePinnedOrClosed()
{
    AudioBufferLadder l;
    CHECK(l.pin(512, 256, Platform::Desktop));
    l.poll(0.0, 0, 512, Platform::Desktop, true, false);
    CHECK(l.poll(1.0, 100, 512, Platform::Desktop, true, false).frames == 0);

    AudioBufferLadder closed;
    closed.poll(0.0, 0, 256, Platform::Desktop, false, false);
    CHECK(closed.poll(1.0, 100, 256, Platform::Desktop, false, false).frames == 0);
}

void browserClimbsOncePerLoad()
{
    // A browser rung applies at the NEXT load: the late callbacks after the
    // climb are still the old buffer's, so a second climb would be on noise.
    AudioBufferLadder l;
    l.poll(0.0, 0, 2048, Platform::Browser, true, true);
    CHECK(l.poll(1.0, kLate, 2048, Platform::Browser, true, true).frames == 4096);
    l.reached(4096);
    l.poll(kWindow + 5.0, kLate, 2048, Platform::Browser, true, true);
    CHECK(l.poll(kWindow + 6.0, 10 * kLate, 2048, Platform::Browser, true, true).frames == 0);
    CHECK(l.learned() == 4096);
    // A desktop climbs again.
    AudioBufferLadder d;
    d.poll(0.0, 0, 256, Platform::Desktop, true, false);
    CHECK(d.poll(1.0, kLate, 256, Platform::Desktop, true, false).frames == 512);
    d.reached(512);
    d.restartWindow();
    d.poll(2.0, kLate, 512, Platform::Desktop, true, false);
    CHECK(d.poll(3.0, 2 * kLate, 512, Platform::Desktop, true, false).frames == 1024);
    d.reached(1024);
    CHECK(d.learned() == 1024);
}

void restoreStartsWhereItLearned()
{
    // A Pi that climbed to 2048 yesterday starts there, not at 1024.
    AudioBufferLadder l;
    auto r = l.restore(0, 2048, 1024, Platform::LinuxArm);
    CHECK(r.frames == 2048 && !r.pinned);
    CHECK(l.learned() == 2048);
    // Learned at or below the current rung: nothing to apply.
    AudioBufferLadder same;
    CHECK(same.restore(0, 1024, 1024, Platform::LinuxArm).frames == 0);
    // A pin wins over what was learned.
    AudioBufferLadder pinned;
    r = pinned.restore(512, 2048, 256, Platform::Desktop);
    CHECK(r.frames == 512 && r.pinned);
    CHECK(pinned.pinned() == 512);
    // Not a rung of this platform (a browser value on a desktop, a
    // non-power of two, negative): dropped, not applied.
    AudioBufferLadder stale;
    CHECK(stale.restore(8192, 3000, 256, Platform::Desktop).frames == 0);
    CHECK(stale.pinned() == 0 && stale.learned() == 0);
    AudioBufferLadder neg;
    CHECK(neg.restore(-1, -5, 256, Platform::Desktop).frames == 0);
}

void persistRoundTrip()
{
    // What persist writes (pinned(), learned()) is what restore reads back.
    AudioBufferLadder run1;
    run1.poll(0.0, 0, 256, Platform::Desktop, true, false);
    const auto c = run1.poll(1.0, kLate, 256, Platform::Desktop, true, false);
    CHECK(c.frames == 512);
    run1.reached(c.frames);
    AudioBufferLadder run2;
    const auto r = run2.restore(static_cast<int>(run1.pinned()),
                                static_cast<int>(run1.learned()), 256,
                                Platform::Desktop);
    CHECK(r.frames == 512 && !r.pinned);
}

void unpinKeepsTheCurrentRung()
{
    AudioBufferLadder l;
    CHECK(!l.pin(300, 256, Platform::Desktop) && "not a rung");
    CHECK(l.pinned() == 0);
    CHECK(l.pin(1024, 256, Platform::Desktop));
    // Back to automatic at 1024: that is the learned floor now, and the
    // window starts from the counter at the unpin (7).
    CHECK(l.pin(0, 1024, Platform::Desktop));
    CHECK(l.pinned() == 0 && l.learned() == 1024);
    l.poll(0.0, 7, 1024, Platform::Desktop, true, false);
    CHECK(l.poll(1.0, 7 + kLate, 1024, Platform::Desktop, true, false).frames == 2048);
}

}  // namespace

int main()
{
    climbsOnLateCallbacks();
    windowExpires();
    restartedWindowForgetsTheOldDevice();
    stopsAtTheTop();
    noClimbWhilePinnedOrClosed();
    browserClimbsOncePerLoad();
    restoreStartsWhereItLearned();
    persistRoundTrip();
    unpinKeepsTheCurrentRung();
    if (failures) {
        std::printf("audio_buffer_ladder: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("audio_buffer_ladder OK\n");
    return 0;
}
