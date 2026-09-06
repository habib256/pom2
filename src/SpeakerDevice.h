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

// SpeakerDevice — 1-bit speaker synthesis. The Apple II speaker is a
// single flip-flop driven by any access in the $C030-$C03F MMIO range.
// Programs make sound by hitting $C030 in tight loops at frequencies
// between ~50 Hz and ~5 kHz; the cone's natural mechanical low-pass
// turns the resulting square wave into recognisable tones.
//
// Reconstruction pipeline (MAME `spkrdev.cpp:74-327` verbatim port):
//
//   CPU thread ─ Memory's $C030 handler calls recordToggle(absoluteCpuCycle)
//                 → push event onto an SPSC-style deque (mutex-guarded).
//   Audio thread ─ fillAudioBuffer():
//                   * For each output sample, fill 4 intermediate samples
//                     (RATE_MULTIPLIER=4 oversampling) by rectangle-area
//                     integration of the latch level over each sub-window.
//                   * Convolve the rolling 64-entry composed_volume ring
//                     with a windowed sinc kernel (FILTER_STEP =
//                     π/(2*RATE_MULTIPLIER), cutoff ≈ sr/4).
//                   * 0.995-pole DC blocker (matches MAME `:280-285`).
//
// This replaces the earlier "snap-to-level + 1-pole LP" reconstruction
// which aliased badly on tight click sequences (Karateka music, click-
// rate effects above sample_rate/4 — pinned by `tests/speaker_smoke`).

#ifndef POM2_SPEAKER_DEVICE_H
#define POM2_SPEAKER_DEVICE_H

#include "AudioSource.h"
#include "CpuClock.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

class SpeakerDevice : public AudioSource, public RateAware
{
public:
    SpeakerDevice();
    ~SpeakerDevice() override = default;

    /// Called from the CPU thread synchronously when $C030-$C03F is
    /// accessed. `cpuCycle` is the absolute CPU cycle of the access
    /// (Memory::cycleCounter + cpu->getCurrentInstructionCycles()).
    void recordToggle(uint64_t cpuCycle);

    /// AudioSource — generates speaker samples mixed by AudioDevice.
    void fillAudioBuffer(float* output, int frameCount) override;

    /// Set the audio output sample rate (negotiated by AudioDevice).
    void setSampleRate(uint32_t hz) override;
    uint32_t getSampleRate() const { return outputSampleRate.load(std::memory_order_relaxed); }

    /// Set the emulated CPU clock (Hz) the cycle→sample reconstruction assumes.
    /// Defaults to the NTSC nominal; EmulationController::setVideoStandard()
    /// pushes the PAL clock (1.0156 MHz) when a PAL profile loads. Without this
    /// the audio path consumed NTSC-many cycles/sec of toggles under PAL — a
    /// ~0.7 % deficit that starved the reconstructor and glitched continuous
    /// speaker music (e.g. H.E.R.O. on the //e-PAL profile).
    void setCpuClock(double hz);

    /// Volume in [0, 2]. UI thread sets, audio thread reads.
    void  setVolume(float v);
    float getVolume() const { return volume.load(std::memory_order_relaxed); }

    /// Mute toggle — separate from volume so the user can flip it without
    /// losing their level setting.
    void setMuted(bool m) { muted.store(m, std::memory_order_relaxed); }
    bool isMuted() const  { return muted.load(std::memory_order_relaxed); }

    /// Drop pending events + reset filter state. Called on hard reset.
    void reset();

    size_t   getQueuedEventCount() const;

private:
    // CPU clock the reconstruction assumes. Runtime (not constexpr) so PAL
    // profiles can retune it; audio thread reads, UI/worker thread writes.
    std::atomic<double>     cpuClockHz_{ static_cast<double>(POM2_CPU_CLOCK_HZ) };
    static constexpr float  kSquareAmp    = 0.18f;     // headroom vs cassette mix
    static constexpr float  kCatchUpSecs  = 0.10f;     // snap forward if behind
    static constexpr size_t kMaxEvents    = 16384;     // ~750 ms at 22 kHz toggles
    // MAME parity: 4× oversampling × 64-tap windowed sinc.
    // RATE_MULTIPLIER must divide FILTER_LENGTH evenly.
    static constexpr int    kRateMultiplier = 4;       // MAME `spkrdev.cpp:74`
    static constexpr int    kFilterLength   = 64;      // MAME `spkrdev_h.txt:28`

    mutable std::mutex   eventMutex;
    std::deque<uint64_t> events;

    // Audio-thread state. Touched only inside fillAudioBuffer + reset.
    /// Toggles pulled out of `events` for the buffer being rendered. A
    /// member so the realtime callback neither allocates nor frees it per
    /// tick (see the note at its use site); its capacity settles at the
    /// busiest buffer seen. Audio thread only.
    std::vector<uint64_t> windowEvents_;
    uint64_t audioCpuCursor   = 0;     // CPU cycle at start of next sample
    double   subSampleAccum   = 0.0;   // fractional CPU cycles into next sub
    double   lastUpdateFrac   = 0.0;   // accumulator since last sub-sample
                                       //   boundary (units: sub-sample
                                       //   periods, range [0, 1)).
    bool     currentLevel     = false;
    // Rolling ring of integrated sub-sample windows. Each slot stores the
    // time-weighted average of `level` over one sub-sample period
    // (= [0..1] given binary level). Indexed by `composedIdx` (write
    // head); the sinc convolution walks the 64 most-recent slots
    // newest-last via `composedIdx + 1 .. composedIdx + 64`.
    std::array<double, kFilterLength> composedVolume{};
    int                               composedIdx = 0;
    // DC blocker state (MAME's y[n] = x[n] - x[n-1] + 0.995 * y[n-1]).
    double dcPrevX = 0.0;
    double dcPrevY = 0.0;

    // Sinc kernel + its abs-sum (used as the convolution normaliser).
    std::array<double, kFilterLength> ampl{};
    double ampSum = 1.0;

    // Producer-published high-water mark.
    std::atomic<uint64_t> latestEventCycle{0};

    // reset() runs on the CPU/UI thread but the integrator state above
    // belongs to the audio thread — clobbering it directly is a data race
    // (caught by TSan). Instead, reset() flips this flag and the audio
    // callback applies the actual reset at the start of its next tick.
    std::atomic<bool>     resetPending_{false};

    std::atomic<float>    volume{1.0f};
    std::atomic<bool>     muted{false};
    std::atomic<uint32_t> outputSampleRate{kAudioSampleRate};

    void buildSincKernel();
};

#endif // POM2_SPEAKER_DEVICE_H
