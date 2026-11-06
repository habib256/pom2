// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// AudioDeviceWatch — when a lost or changed output device is re-opened.
// Pure policy, no audio: the three signals, the back-off, and the cases that
// must NOT trigger a re-open.

#include "AudioDeviceWatch.h"

#include <cstdio>

using pom2::AudioDeviceWatch;
using Reason = AudioDeviceWatch::Reason;

namespace {

int failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

AudioDeviceWatch::Input at(double now, bool available, std::uint64_t callbacks,
                           bool rerouted = false, bool stopped = false)
{
    AudioDeviceWatch::Input in;
    in.nowSec = now; in.hostMode = true; in.available = available;
    in.callbacks = callbacks; in.rerouted = rerouted; in.stopped = stopped;
    return in;
}

}  // namespace

int main()
{
    // An offline device (tests, headless) is never re-opened, whatever it says.
    {
        AudioDeviceWatch w;
        AudioDeviceWatch::Input in = at(100.0, false, 0, true, true);
        in.hostMode = false;
        CHECK(w.poll(in) == Reason::None);
    }

    // A healthy device: callbacks keep coming, nothing happens — for hours.
    {
        AudioDeviceWatch w;
        std::uint64_t n = 0;
        for (double t = 0.0; t < 3600.0; t += 0.016) CHECK(w.poll(at(t, true, n += 3)) == Reason::None);
    }

    // The UI thread stalls (a file dialog) while the audio thread runs on:
    // the first poll after it sees progress, so it is not a stall.
    {
        AudioDeviceWatch w;
        CHECK(w.poll(at(0.0, true, 10)) == Reason::None);
        CHECK(w.poll(at(30.0, true, 90000)) == Reason::None);
    }

    // STALLED: the callbacks stop. Not at once, then after kStallSec.
    {
        AudioDeviceWatch w;
        CHECK(w.poll(at(10.0, true, 500)) == Reason::None);
        CHECK(w.poll(at(11.0, true, 500)) == Reason::None);
        CHECK(w.poll(at(11.9, true, 500)) == Reason::None);
        CHECK(w.poll(at(12.1, true, 500)) == Reason::Stalled);
        // Re-opened: the new device counts from zero, and is left alone.
        w.reopened(true, 12.1);
        CHECK(w.poll(at(12.2, true, 0)) == Reason::None);
        CHECK(w.poll(at(13.0, true, 40)) == Reason::None);
        CHECK(w.failures() == 0);
    }

    // REROUTED and STOPPED act at once…
    {
        AudioDeviceWatch w;
        CHECK(w.poll(at(5.0, true, 1)) == Reason::None);
        CHECK(w.poll(at(5.1, true, 2, /*rerouted=*/true)) == Reason::Rerouted);
        w.reopened(true, 5.1);
        // …but a burst of route changes is one re-open, not one per event:
        // the next is held for kRetryMinSec, and then still honoured even
        // though the flag was only seen once.
        CHECK(w.poll(at(5.3, true, 9, true)) == Reason::None);
        CHECK(w.poll(at(6.0, true, 50)) == Reason::None);
        CHECK(w.poll(at(7.2, true, 90)) == Reason::Rerouted);
        w.reopened(true, 7.2);
        CHECK(w.poll(at(9.5, true, 5, false, /*stopped=*/true)) == Reason::Stopped);
    }

    // No device at all: tried at once, then on a back-off that doubles to a
    // ceiling — a machine without an output is not hammered.
    {
        AudioDeviceWatch w;
        double t = 0.0;
        CHECK(w.poll(at(t, false, 0)) == Reason::Unavailable);
        w.reopened(false, t);
        const double expected[] = { 2.0, 4.0, 8.0, 16.0, 30.0, 30.0 };
        for (double gap : expected) {
            CHECK(w.poll(at(t + gap - 0.1, false, 0)) == Reason::None);
            CHECK(w.poll(at(t + gap + 0.1, false, 0)) == Reason::Unavailable);
            t += gap + 0.1;
            w.reopened(false, t);
        }
        CHECK(w.failures() == 7);
        // An output appears: the retry opens it and the count starts over.
        CHECK(w.poll(at(t + 31.0, false, 0)) == Reason::Unavailable);
        w.reopened(true, t + 31.0);
        CHECK(w.failures() == 0);
        CHECK(w.poll(at(t + 31.5, true, 4)) == Reason::None);
    }

    if (failures) { std::printf("audio_device_watch: %d check(s) failed\n", failures); return 1; }
    std::printf("audio_device_watch: OK\n");
    return 0;
}
