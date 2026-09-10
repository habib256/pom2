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

// SmartPortUnit — abstraction for a single block-level device plugged into
// a `SmartPortCard` (Liron-class). Each card holds up to N units (we cap
// at 2 for v1 since ProDOS 8's direct slot driver only sees drives 1/2;
// SmartPort extended protocol can reach further units but isn't wired
// here yet). The card dispatches READBLOCK / WRITEBLOCK / STATUS to the
// unit the ProDOS `$43` unit byte selects.
//
// Each concrete unit type owns its own storage. Two concrete units ship:
//   - `SmartPort35Unit`   — wraps a `Disk35Image` (800 K Sony 3.5")
//   - `SmartPortHdvUnit`  — wraps a raw / 2MG ProDOS HDV file (up to 32 MB)
//
// Adding more types later (e.g. a generic block-level disk that takes a
// .po image as a 280-block ProDOS volume) means subclassing this and
// listing it in `SmartPortUnit::kindKeyToInstance` so the slot config
// + settings persistence can recreate it after a restart.

#ifndef POM2_SMARTPORT_UNIT_H
#define POM2_SMARTPORT_UNIT_H

#include "Block512Backing.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace pom2 {

class SmartPortUnit
{
public:
    virtual Block512Backing* blockBacking() { return nullptr; }
    virtual const Block512Backing* blockBacking() const { return nullptr; }
    static constexpr size_t kBlockBytes = 512;

    virtual ~SmartPortUnit() = default;

    // ── Access activity ──────────────────────────────────────────────────
    //
    // Same shape as `Block512Backing`'s: a block access sets a hysteresis
    // count, the host bleeds it off one step per frame, and `isBusy()` is
    // true while it holds. Without the hysteresis the light would only be
    // lit on the exact frames a transfer happened to land in, which for a
    // block device is a flicker rather than a read.
    //
    // It lives on the BASE, not on each unit kind, and `SmartPortCard`
    // bumps it where it dispatches READ/WRITE — one site covers 3.5" and
    // HDV alike. `SmartPortHdvUnit` does wrap a `Block512Backing` that
    // keeps a counter of its own, but nothing decays that one (the host's
    // decay loop walks `ProDOSBlockCard` implementers and a SmartPort card
    // is not one), so reading it would latch the light on forever after
    // the first access.
    bool isBusy() const
    {
        return activityTicks_.load(std::memory_order_relaxed) > 0;
    }
    void tickActivityDecay()
    {
        uint32_t v = activityTicks_.load(std::memory_order_relaxed);
        if (v) activityTicks_.store(v - 1, std::memory_order_relaxed);
    }
    void bumpActivity() const
    {
        activityTicks_.store(kBusyHysteresisFrames, std::memory_order_relaxed);
    }

    /// Short stable key for slot-config + settings persistence
    /// (`"35"`, `"hdv"`, …). Must be unique per concrete subclass.
    virtual std::string_view kindKey() const = 0;

    /// Human-readable label for the UI ("3.5\" 800K", "ProDOS HDV", …).
    virtual std::string_view kindLabel() const = 0;

    /// True when media is present. Empty drives still respond to STATUS
    /// (no-media bit set); READ/WRITE return false.
    virtual bool isLoaded() const = 0;

    /// Either the media is physically write-protected (e.g. WOZ flag,
    /// 2MG header) OR the user has not opted into write-back. Matches
    /// `Disk35Image::isWriteProtected` semantics.
    virtual bool isWriteProtected() const = 0;

    /// Block count for loaded media (0 when no media). 800K 3.5" = 1600;
    /// HDV varies. Used by the panel UI; the card's slot ROM doesn't
    /// surface this directly (ProDOS asks the volume itself).
    virtual uint32_t blockCount() const = 0;

    /// Copy block `idx` into `out`. Returns false on no-media or
    /// out-of-range.
    virtual bool readBlock (uint32_t idx, uint8_t* out) const = 0;

    /// Write `in` to block `idx`. Returns false on no-media,
    /// write-protected, or out-of-range. Writes are buffered in RAM
    /// and only persisted by `saveDirty` (or on `eject` via the
    /// owning card's wrapper).
    virtual bool writeBlock(uint32_t idx, const uint8_t* in) = 0;

    /// Mount media from `path`. Returns false on parse error / missing
    /// file; `lastError()` then has a human-readable diagnostic.
    /// Replaces any currently-loaded media (saves dirty first when
    /// write-back is on — same UX as the Disk II / HDV panels).
    virtual bool loadImage(const std::string& path) = 0;

    /// Phase 2 of the two-phase mount, for the unit kinds that have a
    /// Block512Backing under them (today: the HDV unit). Phase 1 is the static
    /// `Block512Backing::readImageFile`, run by the caller WITHOUT
    /// `stateMutex` — which is the point, since an HDV can be 32 MiB.
    ///
    /// Default is "not supported", not a pure virtual: a 3.5" unit has no
    /// block backing and nothing sensible to do here, and forcing it to
    /// implement a stub would be a worse abstraction than letting the caller
    /// fall back to the inline `loadImage`. Callers MUST honour false.
    virtual bool adoptImage(Block512Backing::PreparedImage&& /*prepared*/)
    { return false; }

    /// Persist + clear media. Auto-saves dirty blocks when write-back
    /// is enabled. Idempotent on empty drives.
    ///
    /// Does the save INLINE, so prefer the two-phase pair below when a lock
    /// is held (an HDV unit rewrites the whole file).
    virtual bool eject() = 0;

    /// Phase 1 / failure-undo of a two-phase eject, mirroring `adoptImage`'s
    /// opt-in shape: the default says "no block backing here" and the caller
    /// falls back to the inline `eject()`. Phase 1 must NOT drop the medium —
    /// the commit can fail and the pre-split behaviour kept it mounted. It
    /// MOVES the dirty set out (flags retired at capture, so a block the
    /// guest dirties while the commit runs unlocked keeps its flag for the
    /// eject's own inline flush); `restoreDirtyBlocks` re-marks the captured
    /// set when the commit fails.
    virtual bool detachImage(Block512Backing::PendingWriteBack& /*out*/)
    { return false; }
    virtual void restoreDirtyBlocks(const std::vector<uint32_t>& /*indices*/) {}

    /// Current image path; empty when nothing mounted.
    virtual const std::string& path() const = 0;

    /// Last load error (or empty when none). Cleared by `loadImage`.
    virtual const std::string& lastError() const = 0;

    /// Write-back toggle (save dirty blocks on eject / explicit save).
    /// Default off — the user opts in per-unit via the panel.
    virtual bool isWriteBackEnabled() const = 0;
    virtual void setWriteBackEnabled(bool on) = 0;

    /// The medium's own protection (2IMG lock, WOZ, or the notch — the host
    /// file's read-only bit, MediaNotch.h), without the write-back default
    /// folded in. `setHostWriteProtected` re-applies a flipped notch on a
    /// mounted image. Units with no file behind them ignore both.
    virtual bool isFileWriteProtected() const { return isWriteProtected(); }
    virtual void setHostWriteProtected(bool /*on*/) {}

    /// Persist dirty blocks now. No-op when write-back is off or
    /// nothing is dirty. Returns false on I/O failure.
    virtual bool saveDirty() = 0;

    /// Blocks written by the guest that `saveDirty` has not yet committed.
    /// With write-back off they never will be — they are dropped at eject —
    /// which is what the status bar's eject menu warns about.
    virtual bool hasUnsavedChanges() const = 0;

private:
    // Matches Block512Backing::kBusyHysteresisFrames so a SmartPort volume
    // and an HDV card light for the same duration on the same transfer.
    static constexpr uint32_t kBusyHysteresisFrames = 8;
    mutable std::atomic<uint32_t> activityTicks_{0};
};

/// Factory: create a fresh empty unit for the given kind key. Returns
/// nullptr for unknown keys. Concrete kinds live in their own .h/.cpp
/// pairs and the factory just lists them — keeps the dispatch
/// table in one place for slot config + settings restore.
std::unique_ptr<SmartPortUnit> makeSmartPortUnit(std::string_view kindKey);

} // namespace pom2

#endif // POM2_SMARTPORT_UNIT_H
