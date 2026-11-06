// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// AudioDeviceWatch — WHEN to re-open the host's audio output.
//
// `AudioDevice` opened its device once, in its constructor, and that was it
// for the session: unplug the headphones the sound was going to, switch the
// system's output, dock the laptop, and POM2 stayed silent until it was
// restarted (DEV.md recorded it as a known gap). The fix is a re-open, and
// the re-open already existed — `reconfigurePeriod` does one to climb the
// buffer ladder — so what was missing is only the decision, which is this
// file: policy with no miniaudio in it, same shape and same reason as
// `AudioBufferPolicy.h`, `MouseGrab.h` and `KeyChord.h`. A test can reach it.
//
// Three signals, because no backend gives all of them:
//
//   * REROUTED — the backend followed a change of default output by itself
//     (CoreAudio does). The device plays, but miniaudio keeps the format it
//     was opened with and converts behind POM2's back with its linear
//     resampler ("fastest, lowest quality"), which is exactly what opening
//     at the device's own rate exists to avoid (`AudioDevice::initAudio`).
//     So a reroute is re-opened too.
//   * STOPPED — the backend reports the device stopped and POM2 did not ask
//     for it (WASAPI invalidates a removed device).
//   * STALLED — no data callback for `kStallSec`. The catch-all: ALSA and
//     PulseAudio do not always say anything when a device goes away, but a
//     device that is not asking for audio is not playing any.
//
// A device that cannot be opened (nothing plugged in) is retried with a
// back-off, so sound comes back when an output does. Not in the browser:
// miniaudio's WebAudio re-open rebuilds the AudioContext under the autoplay
// policy (see `AudioDevice::appliesAtNextLoad`).
//
// [D] Casso `AudioEndpointNotifier.cpp` / `WasapiAudio.h` (kReinitRetryMs)
//     @ 2d899adf — a notification sets a flag, the emulation thread re-opens
//     at most once per second. The idea is theirs; the three signals and the
//     back-off are POM2's.
// [F] `kStallSec` and the retry delays are judgement, not measurement.
//     Settled by: unplug tests on each backend (CoreAudio, WASAPI, ALSA,
//     PulseAudio / PipeWire).

#ifndef POM2_AUDIO_DEVICE_WATCH_H
#define POM2_AUDIO_DEVICE_WATCH_H

#include <algorithm>
#include <cstdint>

namespace pom2 {

class AudioDeviceWatch {
public:
    /// A device that has asked for nothing this long is gone. Well above a
    /// UI hitch or the largest buffer on the ladder (4096 frames x 3 at
    /// 44.1 kHz is 0.28 s), well below what a user waits before restarting.
    static constexpr double kStallSec    = 2.0;
    /// Delay before the next attempt: after a success (so a burst of route
    /// changes is one re-open, not five) and the first one after a failure.
    static constexpr double kRetryMinSec = 2.0;
    /// Ceiling of the back-off while no device can be opened at all.
    static constexpr double kRetryMaxSec = 30.0;

    enum class Reason : std::uint8_t { None, Rerouted, Stopped, Stalled, Unavailable };

    struct Input {
        double        nowSec    = 0.0;
        bool          hostMode  = false;  ///< false: offline device, never re-opened
        bool          available = false;  ///< a device is open and started
        std::uint64_t callbacks = 0;      ///< data callbacks so far (monotonic)
        bool          rerouted  = false;  ///< since the last poll
        bool          stopped   = false;  ///< since the last poll, not by POM2
    };

    /// Once per UI frame. Returns why the device should be re-opened NOW, or
    /// `None`. The caller re-opens and reports back through `reopened`.
    Reason poll(const Input& in)
    {
        if (!in.hostMode) return Reason::None;
        Reason why = Reason::None;
        if (!in.available) {
            why = Reason::Unavailable;
        } else {
            if (!tracking_ || in.callbacks != lastCallbacks_) {
                tracking_        = true;
                lastCallbacks_   = in.callbacks;
                lastProgressSec_ = in.nowSec;
            }
            if (in.rerouted)      pending_ = Reason::Rerouted;
            else if (in.stopped)  pending_ = Reason::Stopped;
            if (pending_ != Reason::None)                        why = pending_;
            else if (in.nowSec - lastProgressSec_ > kStallSec)   why = Reason::Stalled;
        }
        if (why == Reason::None || in.nowSec < nextAttemptSec_) return Reason::None;
        return why;
    }

    /// The outcome of the re-open `poll` asked for.
    void reopened(bool ok, double nowSec)
    {
        pending_  = Reason::None;
        tracking_ = false;                 // the new device counts from zero
        if (ok) {
            failures_       = 0;
            nextAttemptSec_ = nowSec + kRetryMinSec;
            return;
        }
        double delay = kRetryMinSec;
        for (int i = 0; i < failures_ && delay < kRetryMaxSec; ++i) delay *= 2.0;
        nextAttemptSec_ = nowSec + std::min(delay, kRetryMaxSec);
        ++failures_;
    }

    int failures() const { return failures_; }

    static const char* describe(Reason r)
    {
        switch (r) {
            case Reason::Rerouted:    return "the system's output device changed";
            case Reason::Stopped:     return "the output device stopped";
            case Reason::Stalled:     return "the output device stopped asking for audio";
            case Reason::Unavailable: return "no output device was open";
            case Reason::None:        break;
        }
        return "";
    }

private:
    bool          tracking_        = false;
    std::uint64_t lastCallbacks_   = 0;
    double        lastProgressSec_ = 0.0;
    double        nextAttemptSec_  = 0.0;
    int           failures_        = 0;
    Reason        pending_         = Reason::None;
};

}  // namespace pom2

#endif  // POM2_AUDIO_DEVICE_WATCH_H
