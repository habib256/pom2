// POM2 — GPL-3.0-or-later
// StorageCoordinator_Chains.cpp — the storage chains grown on 2026-09-11:
// the length of a chain the user sizes (the Liron's 2/4/6/8 units on its
// SmartPort bus), and the ProDOS HD card's drive 2. Own translation unit for
// the file-size ratchet: StorageCoordinator.cpp is a god-object already, and
// what it keeps of these is one call per site.

#include "StorageCoordinator.h"

#include "EmulationController.h"
#include "MediaNotch.h"
#include "MediaWritePolicy.h"
#include "MountableMediaCard.h"
#include "ProDOSHardDiskCard.h"
#include "Settings.h"
#include "SlotBus.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace pom2 {

namespace {
StorageCoordinator::MediaCommandResult commandError(std::string error)
{
    StorageCoordinator::MediaCommandResult r;
    r.ok = false;
    r.error = std::move(error);
    return r;
}
}  // namespace

// ── Keys ─────────────────────────────────────────────────────────────────

std::string StorageCoordinator::hdvDriveKey(const char* base, int drive)
{
    // `hdv_path` / `hdv_writeback` for drive 1, as they always were; the
    // Disk II's `_drive2` suffix (`diskIIPathSettingKey`) for drive 2.
    return drive == 1 ? std::string(base) + "_drive2" : std::string(base);
}

std::string StorageCoordinator::bayCountKey(int slot)
{
    // The generic keyspace of the card's bays (`media_slotN_bayK_*`), so a
    // card never answers under two names.
    return "media_slot" + std::to_string(slot) + "_bays";
}

// ── A chain the user sizes ───────────────────────────────────────────────

void StorageCoordinator::restoreBayCount(MountableMediaCard& media, int slot,
                                         const Settings& settings)
{
    // Before the bays: the count decides how many there are to restore. A
    // card that does not offer the choice keeps its own; one with no saved
    // count keeps the default it was built with (the whole chain).
    if (!media.bayCountChoices().empty())
        media.setBayCount(settings.getInt(bayCountKey(slot), media.bayCount()));
}

StorageCoordinator::MediaCommandResult StorageCoordinator::setMediaBayCount(
    EmulationController& controller, Settings& settings, int slot,
    int count) const
{
    MediaCommandResult result;
    {
        auto state = controller.lockState();
        auto* media = dynamic_cast<MountableMediaCard*>(
            state.memory().slotBus().peripheral(slot));
        if (!media)
            return commandError("slot " + std::to_string(slot) +
                                " has no mountable media");
        const auto choices = media->bayCountChoices();
        if (std::find(choices.begin(), choices.end(), count) == choices.end())
            return commandError("slot " + std::to_string(slot) +
                                " cannot carry " + std::to_string(count) +
                                " units");
        // A shrink never ejects on its own: the medium would have to be
        // flushed (a whole-file rewrite, not under this lock) and the user
        // did not ask for its bay to go. Name the bay to empty instead — by
        // the number the media panel shows it under ("Unit 0" is drive 1).
        for (int bay = count; bay < media->bayCount(); ++bay)
            if (media->bayInfo(bay).loaded)
                return commandError("eject unit " + std::to_string(bay) +
                                    " of slot " + std::to_string(slot) +
                                    " before shrinking the chain to " +
                                    std::to_string(count));
        media->setBayCount(count);
        result.ok = true;
    }
    settings.setInt(bayCountKey(slot), count);
    (void)settings.save();
    return result;
}

// ── The ProDOS HD card's drive 2 ─────────────────────────────────────────
// Drive 1 keeps every one of its historical paths (`primaryHdv`, `hdv_path`);
// these run beside each of them for drive 2, under the same rules.

void StorageCoordinator::restoreHdvDrive2(ProDOSHardDiskCard& hdv,
                                          const Settings& settings,
                                          std::vector<std::string>& warnings)
{
    const std::string path = settings.getString(hdvDriveKey("hdv_path", 1), "");
    const std::string where = "HDV slot " + std::to_string(hdv.getSlot()) + " drive 2";
    std::error_code ec;
    if (!path.empty() && !std::filesystem::is_regular_file(path, ec))
        warnings.push_back(where + ": persisted path not found: " + path);
    else if (!path.empty() && !hdv.loadDrive(1, path))
        warnings.push_back(where + ": " + hdv.getLastError());
    const bool legacy = settings.getBool(hdvDriveKey("hdv_writeback", 1),
                                         pom2::mediaWritableByDefault());
    hdv.setBayWriteBack(1, migrateLegacyWriteBack(
        legacy, { hdv.backing(1).path() }, warnings));
    if (hdv.backing(1).isLoaded())
        hdv.setDriveHostWriteProtected(
            1, pom2::mediaFileIsReadOnly(hdv.backing(1).path()));
}

void StorageCoordinator::captureHdvDrive2(const ProDOSHardDiskCard& hdv,
                                          RebuildSnapshot& snapshot)
{
    const auto& drive2 = hdv.backing(1);
    SlotMediumSnapshot medium;
    medium.slot = hdv.getSlot();
    medium.loaded = drive2.isLoaded();
    if (medium.loaded) medium.path = drive2.path();
    medium.writeBackEnabled = drive2.isWriteBackEnabled();
    snapshot.primaryHdvDrive2 = std::move(medium);
}

void StorageCoordinator::persistHdvDrive2(Settings& settings,
                                          const RebuildSnapshot& snapshot) const
{
    // Same exclusions as drive 1: the session-only auto-provisioned slot,
    // and a synthesised "[host folder] " volume.
    if (!snapshot.primaryHdvDrive2 ||
        snapshot.primaryHdvDrive2->slot == autoHdvSlot_)
        return;
    const auto& medium = *snapshot.primaryHdvDrive2;
    const bool persistable = medium.loaded &&
        medium.path.rfind("[host folder] ", 0) == std::string::npos;
    settings.setString(hdvDriveKey("hdv_path", 1),
                       persistable ? medium.path : std::string());
    settings.setBool(hdvDriveKey("hdv_writeback", 1), medium.writeBackEnabled);
}

void StorageCoordinator::restoreHdvDrive2(ProDOSHardDiskCard& hdv,
                                          const RebuildSnapshot& snapshot)
{
    if (!snapshot.primaryHdvDrive2) return;
    const auto& medium = *snapshot.primaryHdvDrive2;
    bool restored = false;
    if (medium.loaded && !medium.path.empty()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(medium.path, ec))
            restored = hdv.loadDrive(1, medium.path);
    }
    if (!restored && hdv.backing(1).isLoaded())
        (void)hdv.ejectDrive(1);
    hdv.setBayWriteBack(1, medium.writeBackEnabled);
}

}  // namespace pom2
