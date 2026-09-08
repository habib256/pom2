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

// RewindBuffer — the storage layer of the MicroM8-style rewind feature:
// a ring of machine-state snapshots captured one-per-frame so the user can
// step / scrub / replay back through emulated time.
//
// Storage model (Phase 2): periodic *keyframes* hold a full POM2 state
// snapshot (CPU + main RAM + MEX, ~175 KB on stock IIe); the frames between
// them hold an XOR *delta* vs the previous frame — typically a few KB, since
// an Apple II only touches a little RAM per frame. Reconstructing frame i
// copies its nearest keyframe-at-or-below and XORs the intervening deltas
// forward. A 30 s ring drops from ~315 MB of full snapshots to ~10 MB.
//
// Eviction is oldest-first but rebases as it goes: the front is always a
// keyframe, so when it is dropped the next frame (if a delta) is first
// promoted to a keyframe — the delta chain never dangles.
//
// Threading contract: `enabled()` is atomic and may be toggled from any
// thread. Every other method touches `frames_` and MUST be called with
// exclusive access to cpu+mem — i.e. from the CPU worker thread, or from
// another thread holding the EmulationController state mutex while the
// worker is parked. The buffer does no locking of its own.

#ifndef POM2_REWIND_BUFFER_H
#define POM2_REWIND_BUFFER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

class M6502;
class Memory;

namespace pom2 {

class RewindBuffer {
public:
    /// Metadata describing one stored frame (no payload).
    struct FrameInfo {
        uint64_t cycle    = 0;       ///< emuCycles stamp when captured
        size_t   bytes    = 0;       ///< stored size (full blob or delta)
        bool     keyframe = false;   ///< true = full snapshot, false = delta
    };

    /// ~30 s at 60 Hz. Tunable via setMaxFrames / a UI setting.
    static constexpr size_t kDefaultMaxFrames = 1800;
    /// Hard cap on retained payload (256 MiB). The frame count alone doesn't
    /// bound memory once RamWorks blows the per-frame blob up to ~10 MB, so a
    /// byte budget keeps the footprint sane — RamWorks just buys fewer frames
    /// of history. Whichever cap (frames or bytes) binds first wins.
    static constexpr size_t kDefaultMaxBytes = static_cast<size_t>(256) * 1024 * 1024;
    /// One full keyframe every 2 s @ 60 Hz — bounds random-seek cost to
    /// ≤ this many delta applies while keeping keyframe overhead small.
    static constexpr size_t kDefaultKeyframeInterval = 120;
    /// Sentinel returned by restoreToCycle when the buffer is empty.
    static constexpr size_t kNoFrame = static_cast<size_t>(-1);

    explicit RewindBuffer(size_t maxFrames = kDefaultMaxFrames);

    /// Turning recording off and on again starts a NEW timeline, so the ring
    /// must not delta the first frame back against the last one before the
    /// pause. The blob size does not change across a pause (it depends only on
    /// the RamWorks bank count and the card blob shapes), so the keyframe test
    /// used to pass and the resumed frame was stored as a delta against a base
    /// minutes old, spliced straight onto the stale tail: the timeline claimed
    /// a contiguous history, and scrubbing one frame left across the join
    /// teleported the machine back to the pause. Since 2026-09-08 re-enabling
    /// DROPS the ring outright: restarting only the delta base left the old
    /// frames in the deque with no hole marker, so the panel's span read
    /// `newest - oldest` across the pause and one slider notch still jumped
    /// the length of it.
    void setEnabled(bool on);
    bool enabled() const     { return enabled_.load(); }

    /// Cap on retained frames. Shrinking drops the oldest immediately.
    ///
    /// Both caps are honoured as CEILINGS, not as exact counts: eviction
    /// drops a keyframe together with the deltas that hang off it, in one
    /// step, so the ring holds between `maxFrames - keyframeInterval + 1` and
    /// `maxFrames` frames. See evictToCap() for why the exact-count variant
    /// was a stall rather than a nicety.
    void   setMaxFrames(size_t n);
    size_t maxFrames() const { return maxFrames_; }

    /// Cap on retained payload bytes (0 = unlimited). Shrinking evicts the
    /// oldest immediately. At least one frame is always kept, even if a lone
    /// keyframe exceeds the budget.
    void   setMaxBytes(size_t n);
    size_t maxBytes() const { return maxBytes_; }

    /// Frames between full keyframes (>= 1). Lower = faster random seek,
    /// more memory. Takes effect from the next capture.
    void   setKeyframeInterval(size_t n) { keyframeInterval_ = n ? n : 1; }
    size_t keyframeInterval() const { return keyframeInterval_; }

    /// Drop every retained frame (e.g. on cold boot / profile switch).
    void clear();

    /// Append a snapshot of the current machine state (keyframe or delta).
    /// No-op when disabled. Evicts the oldest frame(s) when over the cap.
    ///
    /// The ring's cycle stamps are STRICTLY INCREASING, and this is where
    /// that is enforced rather than assumed: a capture whose stamp is not
    /// newer than the tail means the machine jumped back in time (a scrub
    /// resumed without `truncateAfter`, a snapshot load), so the abandoned
    /// future is dropped first. Every seek helper — `indexForCycle` and the
    /// span readout above it — walks the deque expecting that order and
    /// silently lies without it.
    void capture(M6502& cpu, Memory& mem);

    size_t size()  const { return frames_.size(); }
    bool   empty() const { return frames_.empty(); }
    size_t bytes() const { return totalBytes_; }   ///< total payload held
    /// Capacity of the serialize scratch between captures — a test seam. It
    /// must stay hot: a capture that starts it at 0 re-grows the whole blob
    /// under stateMutex every frame.
    size_t scratchCapacity() const { return captureScratch_.capacity(); }
    size_t keyframeCount() const;

    FrameInfo infoAt(size_t index) const;

    /// Restore the frame at `index` (0 = oldest) into cpu+mem.
    /// Returns false if the index is out of range or the blob is corrupt.
    bool restore(size_t index, M6502& cpu, Memory& mem);

    /// Restore the newest frame whose cycle stamp is <= `cycle` (clamped to
    /// the oldest frame if all are newer). Returns the restored index, or
    /// kNoFrame if the buffer is empty.
    size_t restoreToCycle(uint64_t cycle, M6502& cpu, Memory& mem);

    /// Drop every frame *after* `index`, making it the newest. Used by the
    /// UI when the user resumes from a rewound point — the abandoned future
    /// is discarded so new captures append from here. No-op if out of range.
    void truncateAfter(size_t index);

    uint64_t oldestCycle() const { return frames_.empty() ? 0 : frames_.front().cycle; }
    uint64_t newestCycle() const { return frames_.empty() ? 0 : frames_.back().cycle; }

    /// Index of the newest frame whose cycle is <= `cycle` (clamped to the
    /// oldest). kNoFrame when empty. Pure lookup — does not restore.
    size_t indexForCycle(uint64_t cycle) const;

private:
    struct Frame {
        uint64_t             cycle = 0;
        bool                 isKeyframe = false;
        std::vector<uint8_t> data;   ///< full snapshot (keyframe) or XOR delta
    };

    void evictToCap();
    // Drop every frame stamped at-or-after `cycle`: the future the machine
    // has just abandoned by jumping back. One compare on the hot path.
    void dropAbandonedFuture(uint64_t cycle);
    // Rebuild the full snapshot blob for frame `index` into `out`. False iff
    // a delta in the chain did not apply — `out` is then partially XORed and
    // must not be restored into the live machine.
    bool reconstruct(size_t index, std::vector<uint8_t>& out) const;
    void resyncSinceKeyframe();

    std::deque<Frame> frames_;
    size_t            maxFrames_;
    size_t            maxBytes_ = kDefaultMaxBytes;
    size_t            keyframeInterval_ = kDefaultKeyframeInterval;
    size_t            sinceKeyframe_ = 0;   ///< delta frames since last keyframe (tail)
    size_t            totalBytes_ = 0;
    std::atomic<bool> enabled_{false};

    // Scratch reused across calls to avoid per-frame allocation churn.
    std::vector<uint8_t> prevBlob_;           ///< full blob of the newest frame
    std::vector<uint8_t> captureScratch_;     ///< serialize target
    std::vector<uint8_t> reconstructScratch_; ///< restore target
};

}  // namespace pom2

#endif  // POM2_REWIND_BUFFER_H
