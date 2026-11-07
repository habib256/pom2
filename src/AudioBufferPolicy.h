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

// AudioBufferPolicy — how big the host audio device's buffer is, and when
// it grows.
//
// The sources already ride out a late producer: the speaker's replay cursor
// trails the CPU by 100 ms, the Mockingboard's by two frames plus the
// callback's own span. What they cannot ride out is a late CONSUMER — the
// host's audio thread (or, in the browser, the main thread's
// ScriptProcessorNode) not being scheduled before the device's buffer runs
// dry. That is a device underrun, it is audible as a crackle whatever the
// sources did, and the only lever against it is the size of that buffer.
//
// POM2 opened the device with 3 × 256 frames (16 ms at 48 kHz) everywhere.
// A desktop keeps that. A Raspberry Pi does not: its audio thread shares
// four cores with the CPU worker, the GL thread and the compositor, and
// Raspberry Pi OS's PipeWire runs a 1024-frame quantum (21 ms) on top — a
// 16 ms buffer underruns on every hitch, which users were papering over
// with scheduler priorities. A phone running the WASM build is worse
// again: there the callback waits behind the whole emulated frame.
//
// So: a LADDER of period sizes, a platform-specific first rung, and a rule
// that climbs one rung when the device is caught underrunning. The device
// measures that itself, with no backend help, as a LATE CALLBACK — a gap
// between two data callbacks longer than the whole buffer, which can only
// mean the buffer emptied before the next request came (`callbackIsLate`).
// Three of those inside ten seconds climb a rung; the climb is remembered
// in the settings so the next launch starts where this one ended, and the
// Audio panel can pin a rung by hand. Nothing here touches the sources: a
// bigger callback is just a bigger `frameCount`, which they size from.
//
// Pure functions, so `audio_buffer_policy` pins them with no device.

#ifndef POM2_AUDIO_BUFFER_POLICY_H
#define POM2_AUDIO_BUFFER_POLICY_H

#include <cstdint>

namespace pom2 {

struct AudioBufferPolicy
{
    enum class Platform : std::uint8_t {
        Desktop,        ///< x86/ARM desktop and laptop: a real audio thread
        LinuxArm,       ///< Raspberry Pi and kin: PipeWire's 1024 quantum
        Browser,        ///< WASM: the callback runs on the main thread
        BrowserMobile,  ///< the same, behind a phone's emulated frame
    };

    /// Buffers per device: miniaudio's `periods`. Three is the usual
    /// triple-buffer; the policy scales the PERIOD, not this.
    static constexpr std::uint32_t kPeriods = 3;

    /// The rungs. Powers of two because the browser's ScriptProcessorNode
    /// accepts nothing else (256 … 16384), and the desktop backends are
    /// happiest with them too.
    static constexpr std::uint32_t kMinPeriodFrames = 256;
    static constexpr std::uint32_t kMaxDesktopPeriodFrames = 2048;   // 128 ms
    static constexpr std::uint32_t kMaxBrowserPeriodFrames = 8192;   // 512 ms

    /// The first rung. A desktop starts at the 5.3 ms it always had; a
    /// Linux ARM board above PipeWire's quantum; the browser at the 2048
    /// frames the 2026-10-23 entry measured, a phone one rung above that.
    static constexpr std::uint32_t defaultPeriodFrames(Platform p)
    {
        switch (p) {
            case Platform::Desktop:       return 256;
            case Platform::LinuxArm:      return 1024;
            case Platform::Browser:       return 2048;
            case Platform::BrowserMobile: return 4096;
        }
        return 256;
    }

    static constexpr std::uint32_t maxPeriodFrames(Platform p)
    {
        return (p == Platform::Browser || p == Platform::BrowserMobile)
                   ? kMaxBrowserPeriodFrames
                   : kMaxDesktopPeriodFrames;
    }

    /// The rung a device opens on, from what an earlier load remembered.
    /// A LEARNED rung below the default is stale (the default moved up) and
    /// is dropped; a PINNED one is the user's choice and is kept, even below
    /// the default — the clamp used to drop it too, so a pin below the
    /// browser's 2048 was accepted, shown, persisted and never applied
    /// ("(512 frames at the next load)" on every load; WASM-5).
    static constexpr std::uint32_t startRung(Platform p, std::uint32_t remembered,
                                             bool pinned)
    {
        const std::uint32_t def = defaultPeriodFrames(p);
        if (!isRung(remembered, p)) return def;
        if (pinned) return remembered;
        return remembered > def ? remembered : def;
    }

    /// A rung of the ladder: a power of two within the platform's range.
    static constexpr bool isRung(std::uint32_t frames, Platform p)
    {
        return frames >= kMinPeriodFrames && frames <= maxPeriodFrames(p) &&
               (frames & (frames - 1)) == 0;
    }

    /// The next rung up, or `current` at the top.
    static constexpr std::uint32_t nextPeriodFrames(std::uint32_t current,
                                                    Platform p)
    {
        const std::uint32_t next = current * 2;
        return next <= maxPeriodFrames(p) ? next : current;
    }

    /// Seconds of audio the device holds at that period.
    static constexpr double bufferSeconds(std::uint32_t periodFrames,
                                          std::uint32_t sampleRate)
    {
        return sampleRate == 0
                   ? 0.0
                   : static_cast<double>(periodFrames) * kPeriods /
                         static_cast<double>(sampleRate);
    }

    /// A callback is LATE when the wall-clock gap since the previous one is
    /// longer than the whole buffer: the device cannot have had anything
    /// left to play, so the listener heard a hole. The factor absorbs the
    /// backends that ask in bursts (PulseAudio's `minreq`), and anything
    /// past a second is a laptop lid or a background tab, not an underrun
    /// worth a rung.
    static constexpr double kLateFactor    = 1.25;
    static constexpr double kLateGapMaxSec = 1.0;
    static constexpr bool callbackIsLate(double gapSec,
                                         std::uint32_t periodFrames,
                                         std::uint32_t sampleRate)
    {
        const double buffer = bufferSeconds(periodFrames, sampleRate);
        return buffer > 0.0 && gapSec > buffer * kLateFactor &&
               gapSec < kLateGapMaxSec;
    }

    /// The climb: `lateInWindow` late callbacks inside one window of
    /// `kWindowSec` move up one rung. Returns the new period, or `current`
    /// when nothing changes (too few, or already at the top).
    static constexpr double        kWindowSec     = 10.0;
    static constexpr std::uint32_t kLateToEscalate = 3;
    static constexpr std::uint32_t escalatedPeriodFrames(
        std::uint32_t current, std::uint64_t lateInWindow, Platform p)
    {
        if (lateInWindow < kLateToEscalate) return current;
        return nextPeriodFrames(current, p);
    }
};

} // namespace pom2

#endif // POM2_AUDIO_BUFFER_POLICY_H
