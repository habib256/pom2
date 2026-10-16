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

// Dac8Audio — an unsigned 8-bit DAC as an AudioSource on the stereo bus,
// fed by CPU-cycle-stamped writes (CLAUDE.md "emuCycles everywhere").
//
// Level mapping is MAME's `DAC_8BIT_R2R` (src/devices/sound/dac.h:237 —
// `dac_mapper_unsigned`, 8 bits, `dac_gain_r2r` = 1.0): code / 256 maps onto
// the default output range [-1, +1] (dac.cpp:28-33 and :77-78, :113), so
//
//     level(code) = -1 + 2 * code / 256
//
// and the S.A.M. card routes it to its speaker at 0.5 (a2sam.cpp:56), which
// is folded into `levelFor`. A DAC holds its last code — a zero-order hold —
// so each output sample is the EXACT time-average of the held level over
// the CPU cycles that sample spans (a box filter, no interpolation).
//
// The real card's output stage is AC-coupled; an idle DAC sits at code $00,
// i.e. -0.5 on MAME's scale. A first-order DC blocker (same 44.1 kHz pole as
// SpeakerDevice, rate-independent) keeps that offset off the mix; tests turn
// it off to read the raw levels.
//
// Threading: `write()` runs on the CPU thread (under stateMutex),
// `fillAudioBuffer()` on the audio thread; they share only the event queue
// (own mutex) and atomics. The consumer cursor follows SpeakerDevice's
// rules: it trails the producer by kLeadSecs, snaps forward when the
// producer runs more than two leads ahead (a resume after pause), and snaps
// back when every queued stamp is behind it (a worker stall that resynced).

#ifndef POM2_DAC8_AUDIO_H
#define POM2_DAC8_AUDIO_H

#include "AudioSource.h"
#include "CpuClock.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace pom2 {

class Dac8Audio : public AudioSource, public RateAware
{
public:
    Dac8Audio() { window_.reserve(4096); }

    /// MAME DAC_8BIT_R2R level × the a2sam 0.5 route.
    static float levelFor(uint8_t code)
    {
        return 0.5f * (-1.0f + 2.0f * static_cast<float>(code) / 256.0f);
    }

    /// Latch `code` at CPU cycle `cycle`. Stamps must be non-decreasing.
    void write(uint64_t cycle, uint8_t code);

    void fillAudioBuffer(float* output, int frameCount) override;
    void setSampleRate(uint32_t hz) override;
    void setCpuClock(double hz);

    void  setVolume(float v);
    float getVolume() const { return volume_.load(std::memory_order_relaxed); }
    void  setMuted(bool m)  { muted_.store(m, std::memory_order_relaxed); }
    bool  isMuted() const   { return muted_.load(std::memory_order_relaxed); }
    void  setDcBlock(bool on) { dcBlock_.store(on, std::memory_order_relaxed); }

    /// Drop every queued write; the audio thread re-anchors on the next one.
    void reset();

    uint8_t lastCode() const { return lastCode_.load(std::memory_order_relaxed); }
    std::size_t queuedWrites() const;

private:
    static constexpr double      kLeadSecs  = 0.10;
    static constexpr std::size_t kMaxEvents = 65536;   // ~1.4 s at 44 kHz-rate writes

    using Event = std::pair<uint64_t, uint8_t>;

    mutable std::mutex mutex_;
    std::deque<Event>  events_;
    uint32_t           gen_ = 0;
    std::atomic<uint64_t> latest_{0};
    std::atomic<bool>     resetPending_{false};

    // Audio thread only.
    std::vector<Event> window_;
    uint64_t cursor_   = 0;      // CPU cycle at the start of the next sample
    double   frac_     = 0.0;    // fractional cycles into it
    bool     anchored_ = false;
    float    level_    = levelFor(0x00);
    double   dcX_ = 0.0, dcY_ = 0.0;

    std::atomic<uint8_t>  lastCode_{0};
    std::atomic<double>   cpuClockHz_{static_cast<double>(POM2_CPU_CLOCK_HZ)};
    std::atomic<uint32_t> sampleRate_{kAudioSampleRate};
    std::atomic<float>    volume_{1.0f};
    std::atomic<bool>     muted_{false};
    std::atomic<bool>     dcBlock_{true};
};

} // namespace pom2

#endif // POM2_DAC8_AUDIO_H
