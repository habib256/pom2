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

#include "DiskIICard.h"
#include "DiskImage.h"
#include "Block512Backing.h"
#include "EmulationController.h"
#include "ProDOSBlockCard.h"
#include "RewindBuffer.h"
#include "SmartPortUnit.h"

#include <memory>
#include <mutex>
#include <utility>

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

bool mountDiskII(EmulationController& ctrl, DiskIICard& card, int drive,
                 const std::string& path, std::string& error,
                 bool seekTrack0)
{
    error.clear();

    // Phase 1 — no lock. The read and the nibble decode happen here, so the
    // CPU worker keeps running and the UI keeps painting through all of it.
    // Heap, not stack: a DiskImage is ~242 KB and prepareDisk stacks two more
    // below this frame — see the note in DiskIICard::prepareDisk.
    auto prepared = std::make_unique<DiskImage>();
    if (!DiskIICard::prepareDisk(path, card.isWriteBackEnabled(), *prepared, error))
        return false;

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
    return mountBlockLike(
        ctrl, path, error,
        [&unit](Block512Backing::PreparedImage&& p) {
            return unit.adoptImage(std::move(p));
        },
        [&unit] { return unit.lastError(); },
        [&unit](const std::string& p) { return unit.loadImage(p); });
}

} // namespace pom2
