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

// SpeakerDevice event-overflow parity regression test.
//
// The Apple II speaker is a 1-bit flip-flop: each recorded toggle is a PARITY
// flip, not an absolute level. When the event queue overflowed, the trim
// dropped a single (oldest) toggle, which inverts the reconstructed level of
// every later sample — an audible click/polarity glitch rather than just lost
// time. The trim now drops toggles in PAIRS so parity is preserved.
//
// Observable difference: push an ODD number of toggles, far more than the
// queue cap. With the single-drop bug the steady-state retained count equals
// the (even) cap; with the pair-drop fix it settles one below the cap (odd).
// We assert the queue actually trimmed (count < pushed) AND the retained
// count is odd — which fails on the old single-drop behaviour.

// Second regression, added with bug hunt #4 — the far-future queue front.
//
// `fillAudioBuffer` drains the queue under one lock and pushes its unconsumed
// leftovers back under a SECOND lock, after the render. A `reset()` (cold
// boot, profile switch, hard reset) landing between the two used to re-seed
// the just-emptied queue with pre-reset stamps. After a reset the CPU cycle
// counter restarts near 0, so those stamps sit in the FAR FUTURE — and the
// consumer is strictly front-ordered, so a single one of them blocks every
// live toggle behind it. The speaker went silent, not glitchy, and stayed
// silent until the audio cursor walked all the way up to the stale stamp.
//
// Two guards, both pinned below: a generation counter bumped by `reset()`
// (leftovers whose generation moved are dropped, mirroring
// `MockingboardCard::ayQueueGen_`), and a self-healing purge of a front
// stamped past `latestEventCycle` so an already-corrupted queue recovers
// within one buffer instead of within a minute.

#include "SpeakerDevice.h"

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

// Render `buffers` buffers and report the peak absolute sample. A healthy
// speaker fed a square wave is well above zero; a queue blocked by a stale
// front produces exact silence.
float renderPeak(SpeakerDevice& spk, int buffers, int frameCount)
{
    std::vector<float> buf(static_cast<size_t>(frameCount), 0.0f);
    float peak = 0.0f;
    for (int b = 0; b < buffers; ++b) {
        spk.fillAudioBuffer(buf.data(), frameCount);
        for (float v : buf) peak = std::max(peak, std::fabs(v));
    }
    return peak;
}

} // namespace

int main()
{
    SpeakerDevice spk;
    spk.reset();

    // Odd, and comfortably larger than kMaxEvents (private constant = 16384).
    constexpr uint64_t kPushed = 200001;
    for (uint64_t i = 0; i < kPushed; ++i)
        spk.recordToggle(i + 1);   // strictly increasing CPU cycle stamps

    const size_t count = spk.getQueuedEventCount();

    // The queue must have actually overflowed and trimmed (guards against a
    // future cap ≥ kPushed silently making the parity check vacuous).
    assert(count < kPushed && "queue should have trimmed on overflow");
    // Pair-drop ⇒ parity preserved ⇒ retained count is odd for an odd push
    // count. The old single-drop trim pinned the count at the even cap.
    assert((count & 1u) == 1u && "overflow trim must drop toggles in pairs");

    std::printf("OK speaker_overflow (pair-drop preserves toggle parity, n=%zu)\n",
                count);

    // ── Far-future front: the queue self-heals within one buffer ─────────
    //
    // Reproduce the exact shape a reset()-vs-push-back race leaves behind,
    // using only the public API: a stamp from the abandoned future queued
    // AHEAD of the live stream, with `latestEventCycle` back down at the live
    // stream's value. On the old code the front (50 M) is never `<= window
    // end` and never `< cursor`, so nothing is ever collected and the render
    // is exact silence for ~50 emulated seconds. With the purge it recovers
    // on the first buffer.
    {
        SpeakerDevice s2;
        s2.setSampleRate(48000);
        s2.reset();
        constexpr uint64_t kFuture = 50'000'000;   // ~49 s ahead at 1 MHz
        s2.recordToggle(kFuture);
        // A ~1 kHz square wave over the first few buffers: toggles every
        // ~511 CPU cycles, all of them BEHIND the stale front.
        for (uint64_t i = 1; i <= 200; ++i) s2.recordToggle(i * 511);
        const float peak = renderPeak(s2, 8, 256);
        assert(peak > 0.0f &&
               "a far-future queue front must not silence the live stream");
        assert(s2.staleDropCount() >= 1 && "the stale front must be purged");
        std::printf("OK speaker_future_front (purged %llu, peak %.4f)\n",
                    static_cast<unsigned long long>(s2.staleDropCount()),
                    static_cast<double>(peak));
    }

    // ── Generation counter: reset() invalidates in-flight leftovers ──────
    {
        SpeakerDevice s3;
        s3.setSampleRate(48000);
        const uint32_t gen0 = s3.queueGeneration();
        s3.reset();
        assert(s3.queueGeneration() == gen0 + 1 &&
               "reset() must bump the queue generation");

        // Race the two halves for real. The invariant is one-sided and holds
        // for every interleaving: once the render thread has joined, a queue
        // whose last operation was reset() must be EMPTY. On the old code the
        // renderer's push-back could refill it after that reset (which is the
        // whole defect); it is a race, so this direction is probabilistic on
        // the old code and deterministic on the new one.
        std::atomic<bool> stop{false};
        std::thread audio([&] {
            std::vector<float> buf(256, 0.0f);
            while (!stop.load(std::memory_order_relaxed))
                s3.fillAudioBuffer(buf.data(), 256);
        });
        uint64_t cycle = 1;
        for (int i = 0; i < 20000; ++i) {
            for (int t = 0; t < 16; ++t) s3.recordToggle(cycle += 337);
            s3.reset();
            cycle = 1;   // a reset restarts the machine's cycle counter
        }
        stop.store(true, std::memory_order_relaxed);
        audio.join();
        assert(s3.getQueuedEventCount() == 0 &&
               "no toggle may survive the reset() that followed it");
        std::printf("OK speaker_reset_generation (gen=%u, stale drops=%llu)\n",
                    s3.queueGeneration(),
                    static_cast<unsigned long long>(s3.staleDropCount()));
    }

    return 0;
}
