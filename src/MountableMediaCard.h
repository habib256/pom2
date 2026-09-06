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

// MountableMediaCard — host-side capability mix-in for any slot card that
// exposes one or more *mountable media bays* (a hard-disk image, a 3.5"
// volume, a SmartPort unit, …). It lets the GUI — chiefly the consolidated
// Slot Manager panel — render and drive media on ANY such card generically,
// without a per-card-type `if (cardKey == "...")` ladder.
//
// This is the same "orthogonal host-side mix-in, not a bus concern" pattern
// as ProDOSBlockCard.h: implementers are also SlotPeripherals, but the bus
// dispatch path never touches this interface.
//
// Relationship to ProDOSBlockCard: ProDOSBlockCard already exposes a full
// single-image API (loadImage / ejectImage / getBlockCount / write-back …),
// so it *implements* MountableMediaCard here as a single fixed bay. The two
// HDV-class cards (ProDOSHardDiskCard, CffaCard) therefore gain the bay
// interface for free. SmartPortCard implements it directly over its 2 units,
// advertising per-bay type selection (empty / 3.5" / HDV).

#ifndef POM2_MOUNTABLE_MEDIA_CARD_H
#define POM2_MOUNTABLE_MEDIA_CARD_H

#include <cstdint>
#include "Block512Backing.h"

#include <string>
#include <utility>
#include <vector>

namespace pom2 {

/// Read-only view of one media bay, built by the card for the GUI snapshot.
struct MediaBayInfo
{
    std::string kindLabel;        // media kind in this bay ("3.5\" 800K",
                                  // "ProDOS HDV", …); empty = use card label
    std::string path;             // mounted image path ("" = nothing mounted)
    std::string lastError;        // last mount/load error ("" = none)
    uint32_t    blockCount       = 0;
    bool        loaded           = false;
    /// Recent block activity, for an access LED. Hysteretic (a few frames),
    /// not a single-frame edge, so a transfer reads as a lit lamp rather
    /// than a flicker. False on cards that expose no activity signal.
    bool        busy             = false;
    bool        writeProtected   = false;
    bool        writeBackEnabled  = false;
    bool        supportsWriteBack = true;
    /// Guest writes are held in memory and not yet on disk. With write-back
    /// OFF they are DROPPED at eject, which is the one thing a user needs
    /// warning about before pulling a bay — so the status bar's eject menu
    /// asks for it. Cards whose backing cannot report it leave it false.
    bool        hasUnsavedChanges = false;
    // True when the user may CHOOSE the media kind for this bay (SmartPort
    // units: empty / 3.5" / HDV). Block cards have a fixed kind → false.
    bool        supportsTypeSelect = false;
    std::string typeKey;          // current bay type key ("" / "35" / "hdv")
};

class MountableMediaCard
{
public:
    virtual ~MountableMediaCard() = default;

    /// Number of mountable bays. HDV/CFFA = 1, SmartPort = 2.
    virtual int bayCount() const = 0;

    /// Snapshot of bay `bay` (0-based). Out-of-range → default-constructed.
    virtual MediaBayInfo bayInfo(int bay) const = 0;

    /// Mount `path` into `bay`. On failure returns false and fills `errOut`.
    ///
    /// This is the ONE-PHASE form: it reads the file itself, so a caller
    /// holding `stateMutex` blocks the CPU worker and the UI for the whole
    /// read — 25.8 ms for a 32 MiB HDV before v0.8.5 split that path. Prefer
    /// `prepareBay` + `adoptBay` from any locked context; this stays for
    /// bays with no block backing, which cannot be prepared off the lock.
    virtual bool mountBay(int bay, const std::string& path,
                          std::string& errOut) = 0;

    /// Phase 2 of a two-phase mount: adopt an image `Block512Backing::
    /// readImageFile` produced with no lock held.
    ///
    /// Returns false with an **empty** `errOut` when this bay has no block
    /// backing and therefore cannot adopt — the 3.5" SmartPort unit is the
    /// case that exists. That is not an error: the caller falls back to
    /// `mountBay`, exactly as `MediaMount.cpp`'s `mountBlockLike` does. A real
    /// failure fills `errOut`.
    ///
    /// The default says "unsupported", so a card that has not opted in keeps
    /// working through the fallback.
    virtual bool adoptBay(int /*bay*/,
                          Block512Backing::PreparedImage&& /*prepared*/,
                          std::string& errOut)
    {
        errOut.clear();
        return false;
    }

    /// Eject `bay` (saves dirty blocks first when write-back is on).
    ///
    /// ONE-PHASE, like `mountBay`: the save-on-eject rewrite happens inline,
    /// so a caller holding `stateMutex` freezes the machine for a whole
    /// read-modify-write + rename. Prefer the three-step form below from a
    /// locked context.
    virtual bool ejectBay(int bay) = 0;

    /// Write this bay's pending changes back to its file WITHOUT ejecting.
    ///
    /// This is what a shutdown / profile-switch flush needs
    /// (`StorageCoordinator::flushAll`): the machine is about to be torn down
    /// and rebuilt, and a bay whose writes only reach the file on eject loses
    /// everything the guest wrote since the mount — silently, because the
    /// remount reads the unchanged file back.
    ///
    /// The default is deliberately NOT a silent no-op: a bay with nothing
    /// outstanding succeeds, but a bay that HAS unsaved changes and no flush
    /// path reports failure, so a MountableMediaCard added later cannot
    /// quietly drop a user's data at quit the way `LironCard` did.
    virtual bool flushBay(int bay, std::string& errOut)
    {
        errOut.clear();
        if (!bayInfo(bay).hasUnsavedChanges) return true;
        errOut = "this bay has unsaved changes and no flush path";
        return false;
    }

    /// Phase 1 of a two-phase EJECT, with `stateMutex` held: lift out what
    /// the save-on-eject would write, WITHOUT touching the medium. Leaving it
    /// mounted is the point — phase 2 can fail (disk full, read-only parent),
    /// and the pre-split behaviour kept the medium so the user could retry.
    ///
    /// Same opt-in convention as `adoptBay`: false with an EMPTY `errOut`
    /// means "this bay has no block backing", and the caller falls back to
    /// `ejectBay`. A real failure fills `errOut`.
    virtual bool prepareEjectBay(int /*bay*/,
                                 Block512Backing::PendingWriteBack& /*out*/,
                                 std::string& errOut)
    {
        errOut.clear();
        return false;
    }

    /// Phase-2 FAILURE undo, with `stateMutex` held: re-mark the blocks
    /// `prepareEjectBay` lifted out (phase 1 retires their flags as it
    /// captures them) so the still-mounted medium is dirty again and a retry
    /// re-captures them. On phase-2 SUCCESS there is no phase-3 bookkeeping:
    /// the captured flags are already retired, and any block the guest
    /// dirtied while the commit ran unlocked kept its flag — the caller's
    /// `ejectBay` flushes that (normally empty) remainder inline, so a
    /// write racing the commit is never dropped.
    virtual void restoreBayDirty(int /*bay*/,
                                 const std::vector<uint32_t>& /*indices*/) {}

    /// Toggle per-bay write-back (save-on-eject).
    virtual void setBayWriteBack(int bay, bool on) = 0;

    /// Type-select bays only: the (key,label) options the user may pick
    /// (e.g. {{"","(empty)"},{"35","3.5\" 800K"},{"hdv","ProDOS HDV"}}).
    /// Default: none (fixed-kind card).
    virtual std::vector<std::pair<std::string, std::string>>
    bayTypeOptions(int /*bay*/) const { return {}; }

    /// Type-select bays only: swap the media kind in `bay` to `kindKey`
    /// (drops any currently-mounted media). No-op for fixed-kind cards.
    virtual void setBayType(int /*bay*/, const std::string& /*kindKey*/) {}
};

} // namespace pom2

#endif // POM2_MOUNTABLE_MEDIA_CARD_H
