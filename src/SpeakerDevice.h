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
// Reconstruction pipeline (based on MAME's oversampled speaker):
//
//   CPU thread ─ Memory's $C030 handler calls recordToggle(absoluteCpuCycle)
//                 → push event onto an SPSC-style deque (mutex-guarded).
//   Audio thread ─ fillAudioBuffer():
//                   * For each output sample, fill 4 intermediate samples
//                     (RATE_MULTIPLIER=4 oversampling) by rectangle-area
//                     integration of the latch level over each sub-window.
//                   * Convolve the rolling 64-entry composed_volume ring
//                     with a Blackman-windowed sinc kernel (FILTER_STEP =
//                     π/(2*RATE_MULTIPLIER), cutoff ≈ sr/4).
//                   * DC blocker (0.995 pole at 44.1 kHz, rate-adjusted).
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
#include "EventRing.h"
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
    /// Clock used by the audio reconstructor (stock time after an accelerator transition).
    double cpuClockHz() const { return cpuClockHz_.load(std::memory_order_relaxed); }
    /// An accelerator changed the CPU clock to `hz` at CPU cycle `cycle`; the
    /// machine's stock clock is `stockHz`. Toggles are stamped in STOCK time
    /// from here on (see recordToggle) and the reconstruction runs at
    /// `stockHz`, so the queued backlog plays at the speed it was MADE at.
    /// Converting at playback with the clock then in force played a backlog
    /// made at 4 MHz as if made at 1 MHz and back again — the tail of a
    /// //c+ bell 4x sharp (bug hunt 2026-10-23).
    void setCpuClockAt(uint64_t cycle, double hz, double stockHz);

    /// Volume in [0, 2]. UI thread sets, audio thread reads.
    void  setVolume(float v);
    float getVolume() const { return volume.load(std::memory_order_relaxed); }

    /// Mute toggle — separate from volume so the user can flip it without
    /// losing their level setting.
    void setMuted(bool m) { muted.store(m, std::memory_order_relaxed); }
    bool isMuted() const  { return muted.load(std::memory_order_relaxed); }

    /// The replay lead: how far the audio cursor trails the toggles it plays,
    /// in seconds — the margin a stalled CPU worker has before its toggles
    /// fall behind the cursor and are purged (an audible scratch). 0.10 s by
    /// default and for the synchronous SDK, which never calls this.
    ///
    /// A REAL-TIME host (the desktop and the browser, through
    /// AudioCoordinator's start-up warm-up, AudioWarmup.h) calls it, and that
    /// also turns on the lead's upkeep: at the first toggle of a sound that
    /// follows a silence (kIdleSecs with no toggle), a lead under half the
    /// target or over one and a half times it is set back to the target —
    /// by inserting or skipping silence, which nobody hears. Without it the
    /// lead only ever ERODED: a worker stall that the worker resyncs instead
    /// of catching up costs the lead its length for good, and it was only
    /// re-established once it had gone negative, i.e. after a purge.
    void setRealtimeLead(double seconds);
    /// The CPU's present cycle, from the worker as each slice ends. The lead
    /// is `this - the audio cursor`. It cannot be read off the toggles: in a
    /// rest between two notes the newest toggle is old, and the first one of
    /// the next note is as near the cursor as the rest was short — measured
    /// that way (the first version of the upkeep), every note of Micro
    /// Sokoban's music looked 95 ms late, was pushed back, and the backlog
    /// was then purged in a lump: the tempo slowed, then jumped. The
    /// Mockingboard learned the same on Digidream 1 ("pace against CPU-NOW,
    /// never against the last WRITE", Mockingboard.cpp).
    void noteProducerNow(uint64_t cpuCycle);
    double playbackLeadSeconds() const { return leadSecs_.load(std::memory_order_relaxed); }

    /// Drop pending events + reset filter state. Called on hard reset.
    void reset();
    /// The cycle counter jumped (snapshot load, rewind): the cycle→stock
    /// mapping an accelerator left behind is re-anchored on `cycle`, slope
    /// kept. Call after reset().
    void rebaseStamps(uint64_t cycle);

    size_t   getQueuedEventCount() const;

    /// Generation of the toggle queue — bumped by every `reset()`. The audio
    /// callback snapshots it while it drains the queue and refuses to push
    /// its leftovers back if the value moved meanwhile (see fillAudioBuffer).
    /// Public so a test can pin the counter itself, the way
    /// `MockingboardCard::ayQueueGen_` is pinned through its own queue.
    uint32_t queueGeneration() const;

    /// Every toggle the audio thread did NOT play, whatever the reason (the
    /// forward catch-up, the stale purge, a far-future front, a reset's
    /// leftovers), and every time it moved its cursor other than by playing
    /// (a re-anchor, the lead's upkeep). Both are ZERO in a healthy steady
    /// state — a tune keeps its tempo exactly when the cursor free-runs and
    /// nothing is dropped — so both are what a test, and the Audio panel,
    /// watch: the first upkeep moved the cursor at every note of Micro
    /// Sokoban's music and purged the backlog every few notes, and nothing
    /// counted it.
    uint64_t droppedToggleCount() const { return droppedToggles_.load(std::memory_order_relaxed); }
    uint64_t cursorMoveCount() const { return cursorMoves_.load(std::memory_order_relaxed); }

    /// Toggles the audio thread dropped because they were stale: either a
    /// leftover whose generation had been reset underneath it, or a queue
    /// front stamped in the far future. Diagnostic/test hook only.
    uint64_t staleDropCount() const;

    // The synchronous SDK owns both timelines and can capture their full
    // phase between calls. Desktop audio may be running on another thread.
    void appendSdkState(std::vector<uint8_t>& out) const;
    bool loadSdkState(const uint8_t* data, std::size_t size, bool apply);

private:
    // CPU clock the reconstruction assumes. Runtime (not constexpr) so PAL
    // profiles can retune it; audio thread reads, UI/worker thread writes.
    std::atomic<double>     cpuClockHz_{ static_cast<double>(POM2_CPU_CLOCK_HZ) };
    // CPU cycle → stock time, piecewise linear from the last clock change.
    // Identity (and exact) until an accelerator changes the clock. Under
    // eventMutex. See setCpuClockAt.
    uint64_t stampBaseCycle_ = 0;
    double   stampBaseStock_ = 0.0;
    double   stampRatio_     = 1.0;
    bool     stampIdentity_  = true;
    double   toStockLocked(uint64_t cycle) const;
    // MAME's Apple II speaker route is 0.4 (apple2e.cpp / apple2.cpp).
    // Keep the same nominal level so the AY-to-speaker balance is measurable.
    static constexpr float  kSquareAmp    = 0.4f;
    // The default lead (playbackLeadSeconds). [F] Tuned, not measured.
    static constexpr double kDefaultLeadSecs = 0.10;
    // How much MORE than the lead the producer may get ahead before its
    // backlog is purged (the forward catch-up, for a resume after the device
    // was suspended). At least 0.25 s, so a lead lowered from the start-up
    // 0.30 s to 0.10 s while a sound plays is trimmed at its next silence,
    // not purged in the middle of the sound. Without a real-time host the
    // threshold stays the former 2 x the lead; see fillAudioBuffer.
    static constexpr double kSnapMarginSecs = 0.25;
    // A silence long enough to re-set the lead behind (realtime only).
    static constexpr double kIdleSecs = 0.05;
    std::atomic<double>     leadSecs_{ kDefaultLeadSecs };
    std::atomic<bool>       realtimeLead_{ false };
    std::atomic<uint64_t>   producerNow_{ 0 };   // stock time; 0 = not published
    std::atomic<uint64_t>   droppedToggles_{ 0 };
    std::atomic<uint64_t>   cursorMoves_{ 0 };
    static constexpr size_t kMaxEvents    = 16384;     // ~750 ms at 22 kHz toggles
    // 4× oversampling × 64-tap Blackman-windowed sinc.
    // RATE_MULTIPLIER must divide FILTER_LENGTH evenly.
    static constexpr int    kRateMultiplier = 4;       // [D] MAME spkrdev.cpp:85 @ a2b6ba2d
    static constexpr int    kFilterLength   = 64;      // [D] MAME spkrdev.h:39 @ a2b6ba2d

    mutable std::mutex   eventMutex;
    pom2::EventRing<uint64_t> events;   // EventRing.h: none allocates on the audio thread
    /// Guarded by `eventMutex`. `reset()` bumps it; `fillAudioBuffer`
    /// snapshots it under the lock it takes to drain the queue and compares
    /// under the lock it takes to push the leftovers back. Mirrors
    /// `MockingboardCard::ayQueueGen_`, and exists for the same reason: the
    /// two locks are NOT one critical section, so a reset() landing between
    /// them used to re-insert pre-reset toggles into a queue that had just
    /// been emptied — stamps from the abandoned future, at the FRONT of a
    /// front-ordered consumer, which silenced every live toggle behind them.
    uint32_t queueGen_ = 0;
    /// Count of toggles dropped as stale (generation mismatch or far-future
    /// front). Guarded by `eventMutex`; read by tests.
    uint64_t staleDrops_ = 0;

    // Audio-thread state. Touched only inside fillAudioBuffer + reset.
    /// Toggles pulled out of `events` for the buffer being rendered. A
    /// member so the realtime callback neither allocates nor frees it per
    /// tick (see the note at its use site); its capacity settles at the
    /// busiest buffer seen. Audio thread only.
    std::vector<uint64_t> windowEvents_;
    uint64_t audioCpuCursor   = 0;     // CPU cycle at start of next sample
    uint64_t lastToggleCycle_ = 0;     // stamp of the last toggle played (realtime lead upkeep)
    double   subSampleAccum   = 0.0;   // fractional CPU cycles into next sub
    bool     currentLevel     = false;
    // Rolling ring of integrated sub-sample windows. Each slot stores the
    // time-weighted average of `level` over one complete sub-sample period
    // (= [0..1] given binary level). Indexed by `composedIdx` (write
    // head); the sinc convolution walks the 64 most-recent slots
    // newest-last via `composedIdx + 1 .. composedIdx + 64`.
    std::array<double, kFilterLength> composedVolume{};
    int                               composedIdx = 0;
    // DC blocker state (y[n] = x[n] - x[n-1] + pole * y[n-1]).
    double dcPrevX = 0.0;
    double dcPrevY = 0.0;

    // Sinc kernel + its signed sum (unity-gain convolution normaliser).
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
