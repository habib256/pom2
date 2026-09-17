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

// StorageCoordinator_Persist.cpp — the PURE half of the storage coordinator
// (TODO G5-4): which media the slot bus holds, captured as a snapshot, and
// the settings keys that snapshot is written to. Nothing here reaches
// EmulationController, so `storage_rebuild_persist` links this file and the
// cards it inspects instead of the whole emulator. Callers hold the state
// lock (or own a bus nobody else reaches); the commands that take the lock
// themselves stay in StorageCoordinator.cpp.

#include "StorageCoordinator.h"

#include "CffaCard.h"
#include "DiskIICard.h"
#include "ProDOSBlockCard.h"
#include "ProDOSHardDiskCard.h"
#include "Settings.h"
#include "SlotBus.h"
#include "SlotPeripheral.h"
#include "SmartPortCard.h"

#include <cstddef>
#include <string>
#include <utility>

namespace pom2 {

// Exported deliberately: this is the ONE definition of a Disk II path key,
// and the profile-switch remount in MainWindow_Slots.cpp has to build the
// same key inline (it runs inside the stateMutex scope that makes the
// SlotBus rebuild atomic, so it cannot call a coordinator command, which
// takes that lock itself). Duplicating the "_drive2" rule there is what
// produced the family of drive-2 bugs this function now prevents.
std::string diskIIPathSettingKey(int slot, std::size_t drive)
{
    std::string key = "disk_path_slot" + std::to_string(slot);
    if (drive > 0) key += "_drive" + std::to_string(drive + 1);
    return key;
}

DiskIICard* StorageCoordinator::Topology::diskIIAt(int slot) const noexcept
{
    for (auto* card : diskIICards) {
        if (card && card->getSlot() == slot) return card;
    }
    return nullptr;
}

ProDOSBlockCard*
StorageCoordinator::Topology::preferredBlock() const noexcept
{
    if (primaryCffa) return static_cast<ProDOSBlockCard*>(primaryCffa);
    return static_cast<ProDOSBlockCard*>(primaryHdv);
}

StorageCoordinator::Topology
StorageCoordinator::topology(const SlotBus& bus) const
{
    Topology result;
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        auto* peripheral = bus.peripheral(slot);
        if (auto* card = dynamic_cast<DiskIICard*>(peripheral)) {
            result.diskIICards.push_back(card);
            if (!result.primaryDiskII) result.primaryDiskII = card;
        }
        if (auto* card = dynamic_cast<ProDOSBlockCard*>(peripheral))
            result.blockCards.push_back(card);
        if (auto* card = dynamic_cast<ProDOSHardDiskCard*>(peripheral)) {
            if (!result.primaryHdv) result.primaryHdv = card;
        }
        if (auto* card = dynamic_cast<CffaCard*>(peripheral)) {
            if (!result.primaryCffa) result.primaryCffa = card;
        }
        if (auto* card = dynamic_cast<SmartPortCard*>(peripheral)) {
            result.smartPortCards.push_back(card);
            if (!result.primarySmartPort) result.primarySmartPort = card;
        }
    }
    return result;
}

StorageCoordinator::RebuildSnapshot
StorageCoordinator::captureRebuildSnapshot(const SlotBus& bus) const
{
    static_assert(DiskIICard::kDriveCount ==
                  static_cast<int>(kDiskIIDriveCount));
    RebuildSnapshot snapshot;
    const auto cards = topology(bus);

    snapshot.diskII.reserve(cards.diskIICards.size());
    for (const auto* card : cards.diskIICards) {
        if (!card) continue;
        DiskIISnapshot disk;
        disk.slot = card->getSlot();
        disk.writeBackEnabled = card->isWriteBackEnabled();
        for (std::size_t drive = 0; drive < disk.drives.size(); ++drive) {
            auto& medium = disk.drives[drive];
            medium.loaded = card->isDiskLoaded(static_cast<int>(drive));
            if (medium.loaded)
                medium.path = card->getDiskPath(static_cast<int>(drive));
        }
        snapshot.diskII.push_back(std::move(disk));
    }

    if (cards.primaryHdv) {
        SlotMediumSnapshot medium;
        medium.slot = cards.primaryHdv->getSlot();
        medium.loaded = cards.primaryHdv->isImageLoaded();
        if (medium.loaded) medium.path = cards.primaryHdv->getImagePath();
        medium.writeBackEnabled = cards.primaryHdv->isWriteBackEnabled();
        snapshot.primaryHdv = std::move(medium);
        captureHdvDrive2(*cards.primaryHdv, snapshot);
    }

    snapshot.cffa.reserve(cards.blockCards.size());
    for (const auto* block : cards.blockCards) {
        const auto* card = dynamic_cast<const CffaCard*>(block);
        if (!card) continue;
        SlotMediumSnapshot medium;
        medium.slot = card->getSlot();
        medium.loaded = card->isImageLoaded();
        if (medium.loaded) medium.path = card->getImagePath();
        medium.writeBackEnabled = card->isWriteBackEnabled();
        snapshot.cffa.push_back(std::move(medium));
    }

    return snapshot;
}

void StorageCoordinator::persistRebuildSettings(
    Settings& settings, const RebuildSnapshot& snapshot) const
{
    for (const auto& disk : snapshot.diskII) {
        for (std::size_t drive = 0; drive < disk.drives.size(); ++drive) {
            const auto& medium = disk.drives[drive];
            settings.setString(diskIIPathSettingKey(disk.slot, drive),
                               medium.loaded ? medium.path : std::string());
        }
        settings.setBool("disk_writeback_slot" + std::to_string(disk.slot),
                         disk.writeBackEnabled);
    }

    // Same shape as the Disk II and CFFA branches above, and for the same
    // reason: this function exists to RESYNC the live state into the keys
    // before a rebuild reads them back, because the keys are not trusted —
    // that distrust is why it was written (Apply once dropped whatever was
    // mounted in Disk II drive 2). The HDV branch only ever resynced the path
    // when an image happened to be loaded, and never resynced the write-back
    // opt-in at all, so an ejected image left a stale path behind and the
    // opt-in survived only because every mutation path persists it
    // separately. Masked, not correct: write what the card ACTUALLY holds,
    // including "nothing".
    //
    // Still excluded, as before: the session-local auto-provisioned slot and a
    // synthesised "[host folder] " volume, neither of which may reach hdv_path
    // or it returns as a real mount. And note this deliberately does NOT clear
    // the key when there is no HDV card at all — persistSessionSettings does
    // (a one-shot auto-plug must not survive a quit), but a rebuild that
    // merely has no card yet must not wipe a path one is about to be given.
    if (snapshot.primaryHdv && snapshot.primaryHdv->slot != autoHdvSlot_) {
        const bool persistable =
            snapshot.primaryHdv->loaded &&
            snapshot.primaryHdv->path.rfind("[host folder] ", 0) ==
                std::string::npos;
        settings.setString("hdv_path",
                           persistable ? snapshot.primaryHdv->path
                                       : std::string());
        settings.setBool("hdv_writeback",
                         snapshot.primaryHdv->writeBackEnabled);
    }
    persistHdvDrive2(settings, snapshot);

    for (const auto& medium : snapshot.cffa) {
        const std::string key =
            "cffa_slot" + std::to_string(medium.slot);
        settings.setString(key + "_path",
                           medium.loaded ? medium.path : std::string());
        settings.setBool(key + "_writeback", medium.writeBackEnabled);
    }
}

void StorageCoordinator::persistSessionSettings(
    Settings& settings, const RebuildSnapshot& snapshot) const
{
    persistRebuildSettings(settings, snapshot);

    // The lowest-slot Disk II is the legacy primary. Keep the unsuffixed
    // aliases for older settings consumers; drive 2 never had such an alias.
    const DiskIISnapshot* primaryDisk = nullptr;
    for (const auto& disk : snapshot.diskII) {
        if (!primaryDisk || disk.slot < primaryDisk->slot)
            primaryDisk = &disk;
    }
    if (primaryDisk) {
        const auto& drive1 = primaryDisk->drives[0];
        settings.setString("disk_path",
                           drive1.loaded ? drive1.path : std::string());
        settings.setBool("disk_writeback", primaryDisk->writeBackEnabled);
    }

    // The HDV keys were written by persistRebuildSettings above, under the
    // one contract every writer now shares (TODO G5-8): an auto-provisioned
    // card or NO card is SKIPPED, never cleared. This used to clear both
    // `hdv_path` keys in that case — on the shipped default map and both //c
    // profiles, which carry no HDV card, that wiped the configured path on
    // every quit, and after a one-shot `POM2 image.hdv` it replaced the
    // user's configured disk with nothing. The one-shot image cannot survive
    // a quit without it: no writer ever records the auto-provisioned slot.
}

bool StorageCoordinator::persistDiskIIDrive(
    Settings& settings, const SlotBus& bus, int slot, int drive) const
{
    if (slot < 1 || slot >= SlotBus::kSlotCount || drive < 0 ||
        drive >= DiskIICard::kDriveCount) {
        return false;
    }
    auto* card = dynamic_cast<DiskIICard*>(bus.peripheral(slot));
    if (!card) return false;

    const std::string path = card->isDiskLoaded(drive)
        ? std::string(card->getDiskPath(drive)) : std::string();
    settings.setString(
        diskIIPathSettingKey(slot, static_cast<std::size_t>(drive)), path);
    settings.setBool("disk_writeback_slot" + std::to_string(slot),
                     card->isWriteBackEnabled());

    if (drive == 0 && topology(bus).primaryDiskII == card) {
        settings.setString("disk_path", path);
        settings.setBool("disk_writeback", card->isWriteBackEnabled());
    }
    return true;
}

std::string StorageCoordinator::hdvDriveKey(const char* base, int drive)
{
    // `hdv_path` / `hdv_writeback` for drive 1, as they always were; the
    // Disk II's `_drive2` suffix (`diskIIPathSettingKey`) for drive 2.
    return drive == 1 ? std::string(base) + "_drive2" : std::string(base);
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

} // namespace pom2
