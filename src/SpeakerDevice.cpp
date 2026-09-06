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

#include "SpeakerDevice.h"

#include <algorithm>
#include <cmath>
#include <vector>

SpeakerDevice::SpeakerDevice()
{
    buildSincKernel();
    // One up-front allocation for the audio thread's per-buffer toggle
    // window (~2048 toggles = a 6 ms buffer at 340 kHz, far past what any
    // real click loop produces). Growth beyond it is still legal, just
    // rare enough not to be a per-tick cost.
    windowEvents_.reserve(2048);
}

void SpeakerDevice::buildSincKernel()
{
    // MAME `spkrdev.cpp:121-132`. The kernel is centred on x=0 (or
    // bisected by an x≈0 pair when length is even). For FILTER_LENGTH=64
    // the loop goes from x = (0.5 - 32) * step = -31.5 * step to
    // +31.5 * step in `step` increments. Step π/(2·R) puts the first
    // zero at half the cutoff frequency = sr/(2·2·R) = sr/(4·R) — for
    // R=4 cutoff ends up at ~sr/4 (~12 kHz @ 48 kHz host).
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kStep = kPi / (2.0 * kRateMultiplier);
    ampSum = 0.0;
    double x = (0.5 - kFilterLength / 2.0) * kStep;
    for (int i = 0; i < kFilterLength; ++i, x += kStep) {
        const double v = (std::abs(x) < 1e-12) ? 1.0 : std::sin(x) / x;
        ampl[i] = v;
        ampSum += std::abs(v);
    }
    if (ampSum == 0.0) ampSum = 1.0;
}

void SpeakerDevice::recordToggle(uint64_t cpuCycle)
{
    std::lock_guard<std::mutex> lk(eventMutex);
    events.push_back(cpuCycle);
    // The speaker is a 1-bit flip-flop: each event is a parity flip, not an
    // absolute level. Dropping a single toggle on overflow would invert the
    // reconstructed level of every later sample, so drop in PAIRS (round the
    // excess up to an even count) to keep the surviving stream's parity.
    if (events.size() > kMaxEvents) {
        size_t excess = events.size() - kMaxEvents;
        if (excess & 1u) ++excess;
        while (excess-- > 0 && !events.empty()) events.pop_front();
    }
    latestEventCycle.store(cpuCycle, std::memory_order_relaxed);
}

void SpeakerDevice::reset()
{
    // Only touch shared state under eventMutex (events deque). The
    // integrator fields (audioCpuCursor, subSampleAccum, currentLevel,
    // composedVolume, …) belong to the audio thread; writing them here
    // would race fillAudioBuffer (caught by TSan). Defer their wipe to
    // the audio thread via resetPending_.
    {
        std::lock_guard<std::mutex> lk(eventMutex);
        events.clear();
        latestEventCycle.store(0, std::memory_order_relaxed);
    }
    resetPending_.store(true, std::memory_order_release);
}

void SpeakerDevice::setSampleRate(uint32_t hz)
{
    if (hz == 0) hz = kAudioSampleRate;
    outputSampleRate.store(hz, std::memory_order_relaxed);
    // Kernel is sample-rate-independent (FILTER_STEP only depends on
    // RATE_MULTIPLIER) — no rebuild needed.
}

void SpeakerDevice::setCpuClock(double hz)
{
    if (hz > 0.0) cpuClockHz_.store(hz, std::memory_order_relaxed);
}

void SpeakerDevice::setVolume(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;
    volume.store(v, std::memory_order_relaxed);
}

size_t SpeakerDevice::getQueuedEventCount() const
{
    std::lock_guard<std::mutex> lk(eventMutex);
    return events.size();
}

void SpeakerDevice::fillAudioBuffer(float* output, int frameCount)
{
    if (frameCount <= 0) return;

    // Apply a deferred reset() request from the CPU/UI thread. Done here
    // (audio thread) so the integrator fields stay single-writer.
    if (resetPending_.exchange(false, std::memory_order_acquire)) {
        audioCpuCursor = 0;
        subSampleAccum = 0.0;
        lastUpdateFrac = 0.0;
        currentLevel   = false;
        composedVolume.fill(0.0);
        composedIdx    = 0;
        dcPrevX = dcPrevY = 0.0;
    }

    const uint32_t sr = outputSampleRate.load(std::memory_order_relaxed);
    if (sr == 0) { std::fill_n(output, frameCount, 0.0f); return; }

    // Emulated CPU clock — NTSC nominal by default, PAL (1.0156 MHz) when a
    // PAL profile has retuned it. Using the WRONG standard here starves the
    // reconstructor (audio consumes more cycles/sec of toggles than the CPU
    // produces) → periodic snap-forward glitches on continuous speaker music.
    const double cpuClockHz = cpuClockHz_.load(std::memory_order_relaxed);

    // CPU cycles per sub-sample (= per intermediate sample). At 1.0227 MHz
    // and 48 kHz output that's ~5.33 cycles/sub. Tracked as `double` so
    // the fractional drift never accumulates over long buffers.
    const double cyclesPerSubSample =
        cpuClockHz /
        (static_cast<double>(sr) * static_cast<double>(kRateMultiplier));

    // Catch-up: snap forward if the producer ran ahead. Avoids 5 s of
    // buffered toggles playing at full speed after a pause+resume.
    const uint64_t latest = latestEventCycle.load(std::memory_order_relaxed);
    const uint64_t catchUpCycles =
        static_cast<uint64_t>(2.0 * kCatchUpSecs * cpuClockHz);
    if (latest > audioCpuCursor + catchUpCycles) {
        const uint64_t snapTo = latest -
            static_cast<uint64_t>(kCatchUpSecs * cpuClockHz);
        std::lock_guard<std::mutex> lk(eventMutex);
        // Each toggle is a parity flip, not an absolute level —
        // recordToggle's overflow trim preserves parity by dropping in
        // PAIRS (see above). Mirror the invariant here: when this purge
        // skips an odd number of toggles, flip currentLevel so the
        // reconstructed level of every surviving toggle stays correct
        // (an odd silent drop would invert all later samples).
        size_t dropped = 0;
        while (!events.empty() && events.front() < snapTo) {
            events.pop_front();
            ++dropped;
        }
        if (dropped & 1u) currentLevel = !currentLevel;
        audioCpuCursor = snapTo;
        subSampleAccum = 0.0;
        lastUpdateFrac = 0.0;
    }

    // Snapshot events that could fire inside this buffer's window.
    // MEMBER, not a local: this is the realtime audio callback, and a local
    // vector allocated once per buffer (up to kMaxEvents entries on a busy
    // speaker) and freed at the end of it is a malloc/free pair per tick —
    // the canonical source of underruns. Cleared here, so the capacity from
    // the busiest previous buffer is reused. Audio thread only.
    std::vector<uint64_t>& windowEvents = windowEvents_;
    windowEvents.clear();
    {
        std::lock_guard<std::mutex> lk(eventMutex);
        // Consumer-ahead re-anchor — the mirror of the forward catch-up
        // above. While the machine is paused (toolbar pause, debugger
        // break: Mode::Stopped parks the worker but nothing stops the
        // ma_device) this callback keeps consuming cycles while
        // Memory::cycleCounter freezes, so on resume every new toggle is
        // stamped far BEHIND the cursor. Without this, the stale purge
        // below ate the whole resumed stream one buffer at a time (a net
        // parity flip per ~6 ms buffer) and speaker audio stayed clicks or
        // silence for the rest of the session. Snap back so the resumed
        // stream keeps the same kCatchUpSecs lead the forward path
        // maintains; the catchUpCycles threshold keeps ordinary
        // rounding-stale events (a few cycles) on the cheap purge path.
        if (!events.empty()
            && events.front() + catchUpCycles < audioCpuCursor) {
            const uint64_t lead =
                static_cast<uint64_t>(kCatchUpSecs * cpuClockHz);
            audioCpuCursor =
                (events.front() > lead) ? events.front() - lead : 0;
            subSampleAccum = 0.0;
            lastUpdateFrac = 0.0;
        }
        const uint64_t windowEndApprox = audioCpuCursor +
            static_cast<uint64_t>(frameCount * cyclesPerSubSample *
                                  kRateMultiplier) + 2;
        // Stale-event purge (timestamps already behind the cursor) —
        // same parity rule as the catch-up purge above: flip the level
        // when an odd count is silently dropped.
        size_t dropped = 0;
        while (!events.empty() && events.front() < audioCpuCursor) {
            events.pop_front();
            ++dropped;
        }
        if (dropped & 1u) currentLevel = !currentLevel;
        while (!events.empty() && events.front() <= windowEndApprox) {
            windowEvents.push_back(events.front());
            events.pop_front();
        }
    }

    const float vol     = volume.load(std::memory_order_relaxed);
    const bool  isMuted = muted.load(std::memory_order_relaxed);
    size_t evIdx = 0;

    // Helper: emit one fully-composed output sample after kRateMultiplier
    // sub-samples have been finalised. Implements MAME `get_filtered_volume`
    // + DC blocker.
    auto emitSample = [&]() -> float {
        // Convolve the 64-sample ring with the sinc kernel. We walk
        // starting one slot AFTER the most-recently-written index, so
        // c=0 corresponds to the oldest sample and c=63 to the newest
        // — matches MAME `spkrdev.cpp:319-322`.
        double filtered = 0.0;
        int i = (composedIdx + 1) & (kFilterLength - 1);
        for (int c = 0; c < kFilterLength; ++c) {
            filtered += composedVolume[i] * ampl[c];
            i = (i + 1) & (kFilterLength - 1);
        }
        filtered /= ampSum;
        // DC blocker (identical filter to MAME).
        const double tempX = filtered;
        filtered = tempX - dcPrevX + 0.995 * dcPrevY;
        dcPrevX = tempX;
        dcPrevY = filtered;
        return isMuted ? 0.0f :
            static_cast<float>(filtered * kSquareAmp * vol);
    };

    // Advance one CPU cycle window of duration `dCycles` against the
    // current sub-sample. Accumulates `currentLevel * timeFraction` into
    // composedVolume[composedIdx]. The fraction is measured against the
    // sub-sample period so each slot ends up in [0, 1] (binary level).
    // When the sub-sample boundary is crossed (lastUpdateFrac reaches 1),
    // advance composedIdx and reset its slot to 0.
    auto integrate = [&](double dCycles) {
        // Time still owed to the current sub-sample, in cycles.
        double remainingInSub =
            (1.0 - lastUpdateFrac) * cyclesPerSubSample;
        while (dCycles >= remainingInSub) {
            // Finish this sub-sample.
            if (currentLevel) {
                composedVolume[composedIdx] += (1.0 - lastUpdateFrac);
            }
            dCycles -= remainingInSub;
            // Move to next sub-sample slot.
            composedIdx = (composedIdx + 1) & (kFilterLength - 1);
            composedVolume[composedIdx] = 0.0;
            lastUpdateFrac = 0.0;
            remainingInSub = cyclesPerSubSample;
        }
        // Partial accumulation: fraction of the *next* (still-open)
        // sub-sample.
        if (dCycles > 0.0) {
            const double frac = dCycles / cyclesPerSubSample;
            if (currentLevel) composedVolume[composedIdx] += frac;
            lastUpdateFrac += frac;
        }
    };

    for (int i = 0; i < frameCount; ++i) {
        // Process exactly RATE_MULTIPLIER sub-samples per output sample.
        // We use sub-sample boundaries (not output-sample boundaries) as
        // the unit so toggles always land precisely.
        for (int sub = 0; sub < kRateMultiplier; ++sub) {
            // Cycles until the next sub-sample boundary.
            subSampleAccum += cyclesPerSubSample;
            // Integer-cycle deadline for the next sub-sample boundary.
            const uint64_t deadline = audioCpuCursor +
                static_cast<uint64_t>(subSampleAccum);
            subSampleAccum -= static_cast<double>(deadline - audioCpuCursor);

            // Process events with cycle ≤ deadline. For each, integrate
            // up to the event, flip the level, continue.
            while (evIdx < windowEvents.size()
                   && windowEvents[evIdx] <= deadline) {
                const uint64_t evCycle = windowEvents[evIdx];
                const double   gap     =
                    (evCycle > audioCpuCursor)
                        ? static_cast<double>(evCycle - audioCpuCursor)
                        : 0.0;
                integrate(gap);
                audioCpuCursor = evCycle;
                currentLevel = !currentLevel;
                ++evIdx;
            }
            // Integrate up to the sub-sample boundary.
            integrate(static_cast<double>(deadline - audioCpuCursor));
            audioCpuCursor = deadline;
        }
        // 4 sub-samples now in place; the most recent one is at
        // composedIdx-3 .. composedIdx (mod 64). emitSample walks the
        // full 64-entry window through the kernel.
        output[i] = emitSample();
    }

    // Push back any events whose timestamps fell beyond the actual cursor
    // (rare, possible at buffer-end rounding). Keeps ordering best-effort.
    if (evIdx < windowEvents.size()) {
        std::lock_guard<std::mutex> lk(eventMutex);
        // Reverse-iterate: push_front of ascending k would REVERSE the tail
        // and break the strictly-ascending-by-cycle invariant the consumer
        // relies on (front() is the earliest pending toggle). Walking k
        // downward leaves windowEvents[evIdx] at the front, ascending.
        for (size_t k = windowEvents.size(); k-- > evIdx; ) {
            events.push_front(windowEvents[k]);
        }
    }
}
