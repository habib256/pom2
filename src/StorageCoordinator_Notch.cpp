// StorageCoordinator_Notch.cpp — the notch, as the coordinator applies it
//
// The write-protect half of StorageCoordinator, in its own translation unit
// (the file-size ratchet: StorageCoordinator.cpp is a god-object already).
// See MediaNotch.h for the model — the protection is the disk's, carried by
// the image file's host read-only bit — and DEV § The notch for the why.

#include "StorageCoordinator.h"

#include "CffaCard.h"
#include "Disk35Image.h"
#include "DiskIICard.h"
#include "EmulationController.h"
#include "Logger.h"
#include "MediaNotch.h"
#include "MediaWritePolicy.h"
#include "ProDOSBlockCard.h"
#include "ProDOSHardDiskCard.h"
#include "SlotBus.h"
#include "SmartPortCard.h"
#include "SmartPortUnit.h"

#include <string>
#include <utility>
#include <vector>

namespace pom2 {

namespace {
StorageCoordinator::MediaCommandResult notchError(std::string error)
{
    StorageCoordinator::MediaCommandResult r;
    r.ok = false;
    r.error = std::move(error);
    return r;
}
}  // namespace

StorageCoordinator::MediaCommandResult
StorageCoordinator::setMediaNotch(EmulationController& controller,
                                  const std::string& path, bool protect) const
{
    MediaCommandResult result;
    if (path.empty()) return notchError("no image to write-protect");
    // Phase 1: the file, unlocked. This is the whole of the persistence.
    std::string error;
    if (!pom2::setMediaNotch(path, protect, error)) return notchError(error);
    // Phase 2: every mounted copy of that file, under the lock. A string
    // compare, not fs::equivalent — that is a stat per leaf under stateMutex,
    // and every mount path in POM2 is the string the user or settings gave.
    {
        auto state = controller.lockState();
        const auto cards = topology(state.memory().slotBus());
        for (auto* card : cards.diskIICards) {
            if (!card) continue;
            for (int d = 0; d < DiskIICard::kDriveCount; ++d)
                if (card->isDiskLoaded(d) && card->getDiskPath(d) == path)
                    card->setDriveHostWriteProtected(d, protect);
        }
        for (auto* block : cards.blockCards)
            if (block && block->isImageLoaded() && block->getImagePath() == path)
                block->setHostWriteProtected(protect);
        if (cards.primaryHdv && cards.primaryHdv->isImageLoaded() &&
            cards.primaryHdv->getImagePath() == path)
            cards.primaryHdv->setHostWriteProtected(protect);
        for (auto* card : cards.smartPortCards) {
            if (!card) continue;
            for (std::size_t u = 0; u < SmartPortCard::kMaxUnits; ++u) {
                auto* unit = card->unit(u);
                if (unit && unit->isLoaded() && unit->path() == path)
                    unit->setHostWriteProtected(protect);
            }
        }
        for (Disk35Image* img : { &controller.disk35Internal(),
                                  &controller.disk35External() })
            if (img->isLoaded() && img->path() == path)
                img->setHostWriteProtected(protect);
    }
    pom2::log().info("Media", std::string(protect ? "Write-protected: "
                                                  : "Write-enabled: ") + path);
    result.ok = true;
    return result;
}

/// A `*_writeback = false` key is the pre-notch per-drive opt-out
/// (2026-09-08 and earlier). The protection it expressed now lives on the
/// disk (MediaNotch.h), so it is applied ONCE to the file(s) that key was
/// protecting, and the flag reverts to the process default — the key is
/// rewritten `true` at the next save. When the host refuses the chmod the
/// old flag is kept for this session, so nothing becomes writable by
/// accident; the warning says so. A process running protected by default
/// (the test suite, a kiosk) has nothing to migrate.
bool StorageCoordinator::migrateLegacyWriteBack(
    bool keyValue, const std::vector<std::string>& paths,
    std::vector<std::string>& warnings)
{
    if (keyValue || !pom2::mediaWritableByDefault()) return keyValue;
    bool allNotched = true;
    for (const auto& p : paths) {
        if (p.empty() || pom2::mediaFileIsReadOnly(p)) continue;
        std::string err;
        if (pom2::setMediaNotch(p, true, err)) {
            pom2::log().info("Media",
                "write-back was off in settings — the disk is now "
                "write-protected itself: " + p);
        } else {
            allNotched = false;
            warnings.push_back("could not write-protect " + p + " (" + err +
                               "); keeping write-back off for this session");
        }
    }
    return allNotched;
}

}  // namespace pom2
