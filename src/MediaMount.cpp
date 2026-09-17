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

// Two-phase media mount — see MediaMount.h for why this exists.

#include "MediaMount.h"

#include "Logger.h"

#include "DiskIICard.h"
#include "SlotPeripheral.h"
#include "SlotBus.h"
#include "MountableMediaCard.h"
#include "Disk35Image.h"
#include "DiskImage.h"
#include "Block512Backing.h"
#include "EmulationController.h"
#include "ProDOSBlockCard.h"
#include "RewindBuffer.h"
#include "SmartPortUnit.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace pom2 {

namespace {

/// A HOST-side media swap makes every frame already in the rewind ring
/// describe a machine with DIFFERENT media in it — restoring one puts the old
/// disk's in-flight controller state on top of whatever is in the bay now, and
/// the next commit writes it into the new image. `StorageCoordinator` has
/// cleared the ring on its own mounts since the ring existed
/// (`invalidateRewindForMediaChange`), but the RAW helpers here did not, and
/// they are what the AI control server, the CLI and eleven GUI buttons call
/// (bug hunt 4 #7/#8). Putting it here is the fix that covers all of them at
/// once, rather than at each call site.
///
/// Its own lock scope, after phase 2 has released: `lockState()` /
/// `stateMutex()` is NON-RECURSIVE (CLAUDE.md), and the worker captures
/// frames with that mutex held, so clearing under it cannot land mid-frame.
/// Bare `stateMutex()` is the right handle — this touches neither Memory nor
/// the CPU, which is exactly the case the rule reserves it for.
void noteHostMediaSwap(EmulationController& ctrl)
{
    std::lock_guard<std::mutex> lk(ctrl.stateMutex());
    ctrl.rewind().clear();
}

}  // namespace

/// Every image file mounted anywhere on the machine, as its leaf reports the
/// path: both Disk II drives of every card, every bay of every mountable card
/// (Liron, SmartPort, HDV / CFFA), and the //c+'s two on-board 3.5" drives.
/// Strings only, copied under the state lock — the file comparison happens
/// after, unlocked.
std::vector<std::string> mountedImagePaths(EmulationController& ctrl)
{
    std::vector<std::string> out;
    auto st = ctrl.lockState();
    const SlotBus& bus = st.memory().slotBus();
    for (int s = 0; s < SlotBus::kSlotCount; ++s) {
        SlotPeripheral* p = bus.peripheral(s);
        if (!p) continue;
        if (auto* d = dynamic_cast<DiskIICard*>(p)) {
            for (int dr = 0; dr < DiskIICard::kDriveCount; ++dr)
                if (d->isDiskLoaded(dr)) out.push_back(d->driveImage(dr).getPath());
            continue;
        }
        if (auto* m = dynamic_cast<MountableMediaCard*>(p)) {
            for (int b = 0; b < m->bayCount(); ++b) {
                const MediaBayInfo info = m->bayInfo(b);
                if (info.loaded && !info.path.empty()) out.push_back(info.path);
            }
        }
    }
    for (Disk35Image* img : { &ctrl.disk35Internal(), &ctrl.disk35External() })
        if (img->isLoaded() && !img->path().empty()) out.push_back(img->path());
    return out;
}

namespace {

bool refusedAsSecondMount(EmulationController& ctrl, const std::string& path,
                          const std::string& targetPath, std::string& error)
{
    return imageMountedElsewhere(ctrl, path, targetPath, error);
}

/// The body both Disk II mounts share. `eraseAfterPrepare` makes the mounted
/// medium an UNFORMATTED diskette — done here, between the phases, because it
/// is part of preparing the image and has no business inside the lock.
bool mountDiskIICommon(EmulationController& ctrl, DiskIICard& card, int drive,
                       const std::string& path, std::string& error,
                       bool seekTrack0, bool eraseAfterPrepare)
{
    error.clear();
    std::string current;
    {
        auto st = ctrl.lockState();
        if (card.isDiskLoaded(drive)) current = card.driveImage(drive).getPath();
    }
    if (refusedAsSecondMount(ctrl, path, current, error)) return false;

    // Phase 1 — no lock. The read and the nibble decode happen here, so the
    // CPU worker keeps running and the UI keeps painting through all of it.
    // Heap, not stack: a DiskImage is ~242 KB and prepareDisk stacks two more
    // below this frame — see the note in DiskIICard::prepareDisk.
    auto prepared = std::make_unique<DiskImage>();
    if (!DiskIICard::prepareDisk(path, card.isWriteBackEnabled(), *prepared, error))
        return false;
    if (eraseAfterPrepare) {
        prepared->eraseSurface();
        // Refuse rather than install a FORMATTED disk where the caller asked
        // for a blank one: a user who then finds the disk readable goes
        // looking for the bug in their format code, not in ours.
        if (!prepared->isSurfaceBlank()) {
            error = path + ": the medium refused the erase (write-protected, "
                           "or a WOZ — say it with a TMAP of $FF)";
            return false;
        }
    }

    // Phase 2 — the lock, held only for the swap. Same mutex the CPU worker
    // takes around softSwitchAccess: installing rebuilds the drive's track
    // buffers, so it must not race the LSS.
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        ok = card.installDisk(drive, std::move(*prepared));
        if (ok) {
            if (seekTrack0) card.seekTrack0();
        } else {
            error = card.getLastError(drive);
        }
    }
    if (ok) noteHostMediaSwap(ctrl);   // see noteHostMediaSwap
    if (!ok && error.empty()) error = "insert failed";
    return ok;
}

}  // namespace

bool sameImageFile(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty()) return false;
    std::error_code ec;
    if (std::filesystem::equivalent(a, b, ec) && !ec) return true;
    return a == b;
}

bool imageMountedElsewhere(EmulationController& ctrl, const std::string& path,
                           const std::string& targetPath, std::string& error)
{
    int held = 0;
    for (const std::string& p : mountedImagePaths(ctrl))
        if (sameImageFile(p, path)) ++held;
    // Putting a disk back into the drive it is already in is fine.
    if (!targetPath.empty() && sameImageFile(targetPath, path)) --held;
    if (held <= 0) return false;
    error = path + " is already mounted in another drive — two copies of one "
                   "image would each write their own view of the disk into it";
    return true;
}

// One image, one drive, on the coordinator's own mount paths too (bug hunt
// 2026-09-17): the Disk Library's "insert only" and "mount into the next free
// unit" came through here and mounted a second copy of an image already in
// another drive — two views of one disk, each written back over the other.
// `slot < 0` names an on-board //c+ 3.5" drive (`index` 0/1); otherwise
// `index` is the Disk II drive or the media bay. Returns true, with `error`
// set, when the mount must be refused.
bool imageMountedElsewhereAt(EmulationController& controller, int slot, int index,
                       const std::string& path, std::string& error)
{
    std::string current;
    {
        auto state = controller.lockState();
        if (slot < 0) {
            const auto& img = index == 0 ? controller.disk35Internal()
                                         : controller.disk35External();
            if (img.isLoaded()) current = img.path();
        } else if (auto* p = state.memory().slotBus().peripheral(slot)) {
            if (auto* d = dynamic_cast<DiskIICard*>(p)) {
                if (DiskIICard::validDrive(index) && d->isDiskLoaded(index))
                    current = d->driveImage(index).getPath();
            } else if (auto* m = dynamic_cast<MountableMediaCard*>(p)) {
                if (index >= 0 && index < m->bayCount()) {
                    const MediaBayInfo info = m->bayInfo(index);
                    if (info.loaded) current = info.path;
                }
            }
        }
    }
    return imageMountedElsewhere(controller, path, current, error);
}

bool mountDiskII(EmulationController& ctrl, DiskIICard& card, int drive,
                 const std::string& path, std::string& error,
                 bool seekTrack0)
{
    return mountDiskIICommon(ctrl, card, drive, path, error, seekTrack0,
                             /*eraseAfterPrepare=*/false);
}

bool mountBlankDiskII(EmulationController& ctrl, DiskIICard& card, int drive,
                      const std::string& path, std::string& error,
                      bool seekTrack0)
{
    return mountDiskIICommon(ctrl, card, drive, path, error, seekTrack0,
                             /*eraseAfterPrepare=*/true);
}

// One image, one drive — at restore too (bug hunt 2026-09-16). The restore
// loads every persisted path inline, card by card, so a settings file naming
// one image for two drives (hand-edited, or left by a build without this
// rule) mounted two copies that each wrote their own view of the disk back
// into one file. Run after the loads: the first holder in slot order keeps
// the image, a later one is ejected — it was loaded a moment ago and holds
// nothing to write back — and the user is told. The //c+ on-board 3.5"
// drives are not on the bus and are not covered here.
void dropDuplicateMounts(SlotBus& bus, std::vector<std::string>* warnings)
{
    std::vector<std::pair<std::string, std::string>> seen;   // path, where
    auto claim = [&](const std::string& path, const std::string& where) {
        for (const auto& held : seen) {
            if (sameImageFile(held.first, path)) {
                if (warnings)
                    warnings->push_back(where + ": " + path + " is already mounted in " +
                                        held.second + " — left empty (one image, one drive)");
                log().warn("Storage", where + ": duplicate of " + held.second +
                                            " (" + path + ") left empty");
                return false;
            }
        }
        seen.emplace_back(path, where);
        return true;
    };
    for (int slot = 0; slot < SlotBus::kSlotCount; ++slot) {
        SlotPeripheral* p = bus.peripheral(slot);
        if (!p) continue;
        if (auto* d = dynamic_cast<DiskIICard*>(p)) {
            for (int drive = 0; drive < DiskIICard::kDriveCount; ++drive) {
                if (!d->isDiskLoaded(drive)) continue;
                const std::string where = "Disk II slot " + std::to_string(slot) +
                                          " drive " + std::to_string(drive + 1);
                if (!claim(d->getDiskPath(drive), where)) (void)d->ejectDisk(drive);
            }
            continue;
        }
        if (auto* m = dynamic_cast<MountableMediaCard*>(p)) {
            for (int bay = 0; bay < m->bayCount(); ++bay) {
                const MediaBayInfo info = m->bayInfo(bay);
                if (!info.loaded || info.path.empty()) continue;
                const std::string where = "slot " + std::to_string(slot) +
                                          " bay " + std::to_string(bay + 1);
                if (!claim(info.path, where)) (void)m->ejectBay(bay);
            }
        }
    }
}

// ── Block devices: the 32 MiB case ──────────────────────────────────────

namespace {

/// Shared by both block-device helpers: phase 1 without the lock, then a
/// caller-supplied phase 2 with it. Templated on the adopt step because
/// ProDOSBlockCard and SmartPortUnit are unrelated types that happen to
/// expose the same two-phase shape.
template <class AdoptFn, class ErrFn, class InlineFn>
bool mountBlockLike(EmulationController& ctrl, const std::string& path,
                    std::string& error, AdoptFn adopt, ErrFn lastError,
                    InlineFn inlineLoad)
{
    error.clear();

    // Phase 1 — no lock. The whole file read and the size gates happen here,
    // so the CPU worker keeps running and the UI keeps painting through all
    // of it. Static: it touches no card state at all.
    Block512Backing::PreparedImage prepared;
    if (!Block512Backing::readImageFile(path, prepared, error)) return false;

    // Phase 2 — the lock, held for the 2IMG parse and the adopt. No file I/O
    // in there, except in the one documented same-file-still-dirty case where
    // correctness costs a re-read.
    bool ok          = false;
    bool unsupported = false;
    {
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        ok = adopt(std::move(prepared));
        if (!ok) {
            error = lastError();
            // An empty error from adoptImage is the "this unit kind has no
            // block backing" answer, not a failure — see SmartPortUnit.h.
            unsupported = error.empty();
        }
    }
    if (ok) { noteHostMediaSwap(ctrl); return true; }
    if (!unsupported) return false;

    // Fall back to the inline form for a unit that cannot do phase 2. It
    // costs the stall, and it is the honest behaviour: refusing the mount
    // because the fast path does not apply would be worse.
    {
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        if (inlineLoad(path)) {
            error.clear();
        } else {
            error = lastError();
            if (error.empty()) error = "mount failed";
        }
    }
    if (error.empty()) { noteHostMediaSwap(ctrl); return true; }
    return false;
}

}  // namespace

bool mountBlockCard(EmulationController& ctrl, ProDOSBlockCard& card,
                    const std::string& path, std::string& error)
{
    error.clear();
    std::string current;
    {
        auto st = ctrl.lockState();
        // adoptImage lands in the card's first drive.
        const MediaBayInfo info = card.bayInfo(0);
        if (info.loaded) current = info.path;
    }
    if (refusedAsSecondMount(ctrl, path, current, error)) return false;
    return mountBlockLike(
        ctrl, path, error,
        [&card](Block512Backing::PreparedImage&& p) {
            return card.adoptImage(std::move(p));
        },
        [&card] { return card.getLastError(); },
        [&card](const std::string& p) { return card.loadImage(p); });
}

bool mountSmartPortUnit(EmulationController& ctrl, SmartPortUnit& unit,
                        const std::string& path, std::string& error)
{
    error.clear();
    std::string current;
    {
        auto st = ctrl.lockState();
        if (unit.isLoaded()) current = unit.path();
    }
    if (refusedAsSecondMount(ctrl, path, current, error)) return false;
    return mountBlockLike(
        ctrl, path, error,
        [&unit](Block512Backing::PreparedImage&& p) {
            return unit.adoptImage(std::move(p));
        },
        [&unit] { return unit.lastError(); },
        [&unit](const std::string& p) { return unit.loadImage(p); });
}

} // namespace pom2
