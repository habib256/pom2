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

#include "StorageCoordinator.h"

#include "CffaCard.h"
#include "Disk35Image.h"
#include "DiskIICard.h"
#include "DiskImage.h"
#include "EmulationController.h"
#include "MediaMount.h"
#include "MountableMediaCard.h"
#include "ProDOSBlockCard.h"
#include "ProDOSHardDiskCard.h"
#include "Settings.h"
#include "SlotBus.h"
#include "SlotPeripheral.h"
#include "SmartPortCard.h"
#include "SmartPort35Unit.h"
#include "SmartPortHdvUnit.h"
#include "SmartPortUnit.h"
#include "Sony35Drive.h"

#include <algorithm>
#include <filesystem>
#include <memory>
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

namespace {

SmartPortCard* smartPortAt(SlotBus& bus, int requestedSlot = -1)
{
    if (requestedSlot >= 1 && requestedSlot < SlotBus::kSlotCount)
        return dynamic_cast<SmartPortCard*>(bus.peripheral(requestedSlot));
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        if (auto* card = dynamic_cast<SmartPortCard*>(bus.peripheral(slot)))
            return card;
    }
    return nullptr;
}

struct SettingUpdate {
    std::string key;
    std::string stringValue;
    bool boolValue = false;
    bool isBool = false;
};

void appendStringSetting(std::vector<SettingUpdate>& updates,
                         std::string key, std::string value)
{
    updates.push_back({std::move(key), std::move(value), false, false});
}

void appendBoolSetting(std::vector<SettingUpdate>& updates,
                       std::string key, bool value)
{
    updates.push_back({std::move(key), {}, value, true});
}

void applySettingUpdates(Settings& settings,
                         const std::vector<SettingUpdate>& updates)
{
    for (const auto& update : updates) {
        if (update.isBool)
            settings.setBool(update.key, update.boolValue);
        else
            settings.setString(update.key, update.stringValue);
    }
}

void appendDiskIIDriveSettingUpdates(
    std::vector<SettingUpdate>& updates, const SlotBus& bus,
    const DiskIICard& card, int drive)
{
    const std::string path = card.isDiskLoaded(drive)
        ? std::string(card.getDiskPath(drive)) : std::string();
    appendStringSetting(
        updates, diskIIPathSettingKey(card.getSlot(), drive), path);
    appendBoolSetting(
        updates, "disk_writeback_slot" + std::to_string(card.getSlot()),
        card.isWriteBackEnabled());

    const auto cards = StorageCoordinator{}.topology(bus);
    if (drive == 0 && cards.primaryDiskII == &card) {
        appendStringSetting(updates, "disk_path", path);
        appendBoolSetting(updates, "disk_writeback",
                          card.isWriteBackEnabled());
    }
}

/// A MountableMediaCard with no keyspace of its own (today: `LironCard`).
/// The SmartPort, CFFA and HDV cards keep theirs — those keys are on disk in
/// every user's settings — and this generic one only covers what they do
/// not, so a card never answers under two names.
MountableMediaCard* genericMediaCard(SlotPeripheral* peripheral)
{
    if (!peripheral) return nullptr;
    if (dynamic_cast<SmartPortCard*>(peripheral) ||
        dynamic_cast<CffaCard*>(peripheral) ||
        dynamic_cast<ProDOSHardDiskCard*>(peripheral))
        return nullptr;
    return dynamic_cast<MountableMediaCard*>(peripheral);
}

std::string genericBayKey(int slot, int bay)
{
    return "media_slot" + std::to_string(slot) + "_bay" + std::to_string(bay);
}

bool appendMediaBaySettingUpdates(
    std::vector<SettingUpdate>& updates, SlotPeripheral& peripheral,
    int slot, int bay, int autoHdvSlot, int autoSmartPortSlot)
{
    if (auto* card = dynamic_cast<SmartPortCard*>(&peripheral)) {
        if (slot == autoSmartPortSlot) return false;
        if (bay < 0 || bay >= static_cast<int>(SmartPortCard::kMaxUnits))
            return false;
        const std::string base = "smartport_slot" + std::to_string(slot) +
                                 "_unit" + std::to_string(bay);
        const SmartPortUnit* unit = card->unit(static_cast<std::size_t>(bay));
        appendStringSetting(
            updates, base + "_type",
            unit ? std::string(unit->kindKey()) : std::string());
        appendStringSetting(
            updates, base + "_path", unit ? unit->path() : std::string());
        appendBoolSetting(
            updates, base + "_writeback",
            unit ? unit->isWriteBackEnabled() : false);
        return true;
    }
    if (auto* card = dynamic_cast<CffaCard*>(&peripheral)) {
        const std::string base = "cffa_slot" + std::to_string(slot);
        appendStringSetting(updates, base + "_path", card->getImagePath());
        appendBoolSetting(
            updates, base + "_writeback", card->isWriteBackEnabled());
        return true;
    }
    if (auto* card = dynamic_cast<ProDOSHardDiskCard*>(&peripheral)) {
        // Session-only auto-provisioned cards and synthetic host-folder
        // volumes must not overwrite the user's configured HDV.
        if (slot == autoHdvSlot ||
            card->getImagePath().rfind("[host folder] ", 0) == 0) {
            return false;
        }
        appendStringSetting(updates, "hdv_path", card->getImagePath());
        appendBoolSetting(
            updates, "hdv_writeback", card->isWriteBackEnabled());
        return true;
    }
    if (auto* media = genericMediaCard(&peripheral)) {
        if (bay < 0 || bay >= media->bayCount()) return false;
        const MediaBayInfo info = media->bayInfo(bay);
        const std::string base = genericBayKey(slot, bay);
        appendStringSetting(updates, base + "_path",
                            info.loaded ? info.path : std::string());
        appendBoolSetting(updates, base + "_writeback", info.writeBackEnabled);
        return true;
    }
    return false;
}

/// `prepared` non-null = the caller already read the image with no lock held
/// (phase 1); the unit adopts those bytes instead of reading the file a second
/// time under `stateMutex`. Leave it null for a unit kind with no block
/// backing (the 3.5" unit), whose phase 1 would be wasted.
bool mountSmartPortUnitAs(
    SmartPortCard& card, int bay, std::string_view kind,
    const std::string& path, std::vector<SettingUpdate>& updates,
    std::string& error, int autoSmartPortSlot,
    Block512Backing::PreparedImage* prepared = nullptr)
{
    if (bay < 0 || bay >= static_cast<int>(SmartPortCard::kMaxUnits)) {
        error = "invalid SmartPort unit " + std::to_string(bay + 1);
        return false;
    }
    const auto index = static_cast<std::size_t>(bay);
    SmartPortUnit* unit = card.unit(index);
    if (!unit || unit->kindKey() != kind) {
        // Replacing the type destroys the current unit. Flush explicitly so
        // a failed write-back leaves the only dirty copy mounted for retry.
        if (unit && !unit->saveDirty()) {
            error = "unsaved changes on SmartPort unit " +
                    std::to_string(bay + 1) +
                    " could not be written: " + unit->lastError();
            return false;
        }
        auto replacement = makeSmartPortUnit(kind);
        if (!replacement) {
            error = "unsupported SmartPort media type '" +
                    std::string(kind) + "'";
            return false;
        }
        card.setUnit(index, std::move(replacement));
        unit = card.unit(index);
    }
    if (!unit) {
        error = "SmartPort unit creation failed";
        return false;
    }
    // Phase 2 when the caller ran phase 1: adopting costs no file I/O, where
    // loadImage() would re-read the whole image — up to 32 MiB — with the lock
    // held. An empty error back from adoptImage is the "this unit kind has no
    // block backing" answer, not a failure, so fall back to the inline read
    // for those (same rule as pom2::mountSmartPortUnit).
    bool loaded = false;
    if (prepared) {
        loaded = unit->adoptImage(std::move(*prepared));
        if (!loaded && !unit->lastError().empty()) {
            error = unit->lastError();
            return false;
        }
    }
    if (!loaded && !unit->loadImage(path)) {
        error = unit->lastError();
        return false;
    }
    (void)appendMediaBaySettingUpdates(
        updates, card, card.getSlot(), bay, -1, autoSmartPortSlot);
    return true;
}

std::string freePoNameFor(const std::string& wozPath)
{
    if (wozPath.empty()) return {};
    const std::filesystem::path source(wozPath);
    const auto directory = source.parent_path();
    const std::string stem = source.stem().string();
    std::error_code error;
    for (int number = 1; number <= 99; ++number) {
        const std::string name = number == 1
            ? stem + ".po"
            : stem + " (" + std::to_string(number) + ").po";
        const auto candidate = directory / name;
        if (!std::filesystem::exists(candidate, error))
            return candidate.string();
        error.clear();
    }
    return {};
}

void copyDisk35ImageState(
    StorageCoordinator::Disk35DriveSnapshot& target,
    const Disk35Image& image)
{
    target.loaded = image.isLoaded();
    target.writeProtected = image.isWriteProtected();
    target.path = image.path();
    target.lastError = image.lastError();
    target.hasUnsavedChanges = image.hasUnsavedChanges();
    target.writeBackEnabled = image.isWriteBackEnabled();
    target.isWoz = image.kind() == Disk35Image::ImageKind::Woz35;
}

StorageCoordinator::MediaCommandResult commandError(std::string error)
{
    StorageCoordinator::MediaCommandResult result;
    result.error = std::move(error);
    return result;
}

} // namespace

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

StorageCoordinator::InventorySnapshot
StorageCoordinator::captureInventory(EmulationController& controller) const
{
    InventorySnapshot snapshot;
    auto state = controller.lockState();
    const auto cards = topology(state.memory().slotBus());
    for (auto* card : cards.diskIICards)
        snapshot.diskIISlots.push_back(card->getSlot());
    for (auto* card : cards.blockCards)
        snapshot.blockSlots.push_back(card->getSlot());
    for (auto* card : cards.smartPortCards)
        snapshot.smartPortSlots.push_back(card->getSlot());
    if (cards.primaryDiskII) {
        snapshot.primaryDiskIISlot = cards.primaryDiskII->getSlot();
        snapshot.primaryDiskUsingBitLss = cards.primaryDiskII->usingBitLss();
        snapshot.primaryDiskLoaded = cards.primaryDiskII->isDiskLoaded();
    }
    if (cards.primaryHdv) {
        snapshot.primaryHdvSlot = cards.primaryHdv->getSlot();
        snapshot.primaryHdvLoaded = cards.primaryHdv->isImageLoaded();
    }
    if (cards.primaryCffa)
        snapshot.primaryCffaSlot = cards.primaryCffa->getSlot();
    if (cards.primarySmartPort) {
        snapshot.primarySmartPortSlot = cards.primarySmartPort->getSlot();
        snapshot.primarySmartPortLironRomLoaded =
            cards.primarySmartPort->isLironRomLoaded();
    }
    return snapshot;
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

    if (snapshot.primaryHdv && snapshot.primaryHdv->slot != autoHdvSlot_) {
        const bool persistable = snapshot.primaryHdv->loaded &&
            snapshot.primaryHdv->path.rfind("[host folder] ", 0) ==
                std::string::npos;
        settings.setString("hdv_path",
                           persistable ? snapshot.primaryHdv->path
                                       : std::string());
        settings.setBool("hdv_writeback",
                         snapshot.primaryHdv->writeBackEnabled);
    } else {
        settings.setString("hdv_path", "");
    }
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

StorageCoordinator::MediaCommandResult StorageCoordinator::mountDiskII(
    EmulationController& controller, Settings& settings, int slot, int drive,
    const std::string& path, bool seekTrackZero) const
{
    MediaCommandResult result;
    std::vector<SettingUpdate> updates;
    if (!DiskIICard::validDrive(drive))
        return commandError("invalid Disk II drive " +
                            std::to_string(drive + 1));

    // Phase 1, NO lock: read the file and run the nibble decode. Doing this
    // under stateMutex is what MediaMount.h exists to prevent — the worker
    // takes that lock every 4096 cycles and the UI takes it to paint, so a
    // 12.8 ms warm-cache read freezes the machine and the window together.
    //
    // The write-back flag is read unlocked on purpose: prepareDisk only needs
    // it to decide whether the decode keeps its write buffers, and phase 2
    // re-checks the card it actually installs into.
    bool writeBack = false;
    {
        auto state = controller.lockState();
        auto* card = dynamic_cast<DiskIICard*>(
            state.memory().slotBus().peripheral(slot));
        if (!card)
            return commandError("no Disk II card in slot " +
                                std::to_string(slot));
        writeBack = card->isWriteBackEnabled();
    }
    auto prepared = std::make_unique<DiskImage>();
    if (!DiskIICard::prepareDisk(path, writeBack, *prepared, result.error))
        return result;

    // Phase 2 — re-resolve by SLOT, never through a pointer carried across
    // the gap. This is the caller contract MediaMount.h spells out for
    // anything that is not plain UI-thread code: the bus can have been
    // rebuilt while the read was running.
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* card = dynamic_cast<DiskIICard*>(bus.peripheral(slot));
        if (!card)
            return commandError("Disk II card in slot " +
                                std::to_string(slot) +
                                " went away during the mount");
        if (!card->installDisk(drive, std::move(*prepared)))
            return commandError(card->getLastError(drive));
        if (seekTrackZero) card->seekTrack0();
        appendDiskIIDriveSettingUpdates(updates, bus, *card, drive);
        result.ok = true;
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();
    return result;
}

StorageCoordinator::MediaCommandResult StorageCoordinator::ejectDiskII(
    EmulationController& controller, Settings& settings, int slot,
    int drive) const
{
    MediaCommandResult result;
    std::vector<SettingUpdate> updates;
    // Two-phase, exactly as `mountDiskII` above: `DiskIICard::ejectDisk` does
    // a full sector re-encode, a write and two fsyncs, and doing that with
    // `stateMutex` held froze the CPU worker and the window together (the
    // eject side of the v0.8.5 mount split — CLAUDE.md, MediaMount.h). Phase 1
    // lifts the medium out under the lock; phase 2 writes it with the lock
    // released; a failed phase 2 puts the medium back so the user can retry.
    std::unique_ptr<DiskImage> pending;
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* card = dynamic_cast<DiskIICard*>(bus.peripheral(slot));
        if (!card)
            return commandError("no Disk II card in slot " +
                                std::to_string(slot));
        if (!DiskIICard::validDrive(drive))
            return commandError("invalid Disk II drive " +
                                std::to_string(drive + 1));
        pending = card->takeEjectWriteBack(drive);
        appendDiskIIDriveSettingUpdates(updates, bus, *card, drive);
        result.ok = true;
    }
    if (pending) {
        std::string error;
        if (!DiskIICard::commitEjectWriteBack(*pending, error)) {
            auto state = controller.lockState();
            auto* card = dynamic_cast<DiskIICard*>(
                state.memory().slotBus().peripheral(slot));
            if (card) (void)card->restoreEjected(drive, std::move(pending));
            return commandError(error);
        }
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();
    return result;
}

StorageCoordinator::MediaCommandResult
StorageCoordinator::setDiskIIWriteBack(
    EmulationController& controller, Settings& settings, int slot,
    bool enabled) const
{
    MediaCommandResult result;
    std::vector<SettingUpdate> updates;
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* card = dynamic_cast<DiskIICard*>(bus.peripheral(slot));
        if (!card)
            return commandError("no Disk II card in slot " +
                                std::to_string(slot));
        card->setWriteBackEnabled(enabled);
        appendDiskIIDriveSettingUpdates(updates, bus, *card, 0);
        result.ok = true;
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();
    return result;
}

StorageCoordinator::MediaCommandResult StorageCoordinator::mountMediaBay(
    EmulationController& controller, Settings& settings, int slot, int bay,
    const std::string& path) const
{
    MediaCommandResult result;
    std::vector<SettingUpdate> updates;

    // Phase 1, NO lock: read the image. An HDV is up to 32 MiB and this ran
    // under stateMutex before — 25.8 ms with the machine and the window both
    // stopped, against a 20 ms PAL frame.
    Block512Backing::PreparedImage prepared;
    std::string prepareError;
    const bool preparedOk =
        Block512Backing::readImageFile(path, prepared, prepareError);

    // Phase 2: re-resolve by SLOT — the bus can have been rebuilt while the
    // read ran — then adopt. A bay with no block backing (the 3.5" SmartPort
    // unit) reports that with an empty error, and falls back to the one-phase
    // mountBay(), which is the only form it has.
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* peripheral = bus.peripheral(slot);
        auto* media = dynamic_cast<MountableMediaCard*>(peripheral);
        if (!media)
            return commandError("slot " + std::to_string(slot) +
                                " has no mountable media");
        if (bay < 0 || bay >= media->bayCount())
            return commandError("invalid media bay " +
                                std::to_string(bay + 1));

        bool mounted = false;
        if (preparedOk) {
            std::string adoptError;
            if (media->adoptBay(bay, std::move(prepared), adoptError))
                mounted = true;
        }
        if (!mounted) {
            // Fall back to the one-phase path on ANY adopt miss, not only on
            // "this bay has no block backing". adoptBay takes the plain
            // 512-byte-block route, while mountBay goes through each card's
            // own loader, which accepts more than that (a 2MG header, an odd
            // trailing partial block). Treating an adopt failure as fatal here
            // would reject images the one-phase path mounts happily, so the
            // fast path is an optimisation and mountBay stays authoritative.
            if (!media->mountBay(bay, path, result.error)) {
                if (result.error.empty()) result.error = prepareError;
                return result;
            }
        }
        (void)appendMediaBaySettingUpdates(
            updates, *peripheral, slot, bay,
            autoHdvSlot_, autoSmartPortSlot_);
        result.ok = true;
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();
    return result;
}

StorageCoordinator::MediaCommandResult StorageCoordinator::mountBlockBytes(
    EmulationController& controller, Settings& settings, int slot,
    std::vector<std::uint8_t> bytes, const std::string& label,
    const std::string& hostFolder) const
{
    MediaCommandResult result;
    std::vector<SettingUpdate> updates;
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* peripheral = bus.peripheral(slot);
        auto* card = dynamic_cast<ProDOSBlockCard*>(peripheral);
        if (!card)
            return commandError("slot " + std::to_string(slot) +
                                " has no ProDOS block device");
        if (!card->loadImageFromBytes(std::move(bytes), label, hostFolder))
            return commandError(card->getLastError());
        (void)appendMediaBaySettingUpdates(
            updates, *peripheral, slot, 0,
            autoHdvSlot_, autoSmartPortSlot_);
        result.ok = true;
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();
    return result;
}

StorageCoordinator::MediaCommandResult StorageCoordinator::ejectMediaBay(
    EmulationController& controller, Settings& settings, int slot,
    int bay) const
{
    MediaCommandResult result;
    std::vector<SettingUpdate> updates;

    // Three critical sections, because save-on-eject is a whole-file
    // read-modify-write + rename and doing it under `stateMutex` froze the
    // CPU worker and the paint thread together (CLAUDE.md's standing rule).
    //   1. locked   — MOVE the payload out (dirty flags retired at capture),
    //                 medium left MOUNTED
    //   2. unlocked — commit it (restore the captured dirty set on failure)
    //   3. locked   — drop the medium
    // The medium survives phase 1 on purpose: a commit can fail (disk full,
    // read-only parent) and the one-phase code kept it mounted so the user
    // could retry — the failure path re-marks the captured blocks so the
    // retry re-captures them. The CPU worker keeps RUNNING through phase 2,
    // so the guest can dirty more blocks against the still-mounted medium:
    // phase 1 having cleared only what it captured, those stragglers keep
    // their flags and phase 3's `ejectBay` flushes them inline (normally
    // nothing). The pre-fix blanket clear in phase 3 wiped them instead —
    // guest writes racing the commit were silently dropped from the user's
    // only host copy, the exact hazard DEV.md ranks above latency.
    Block512Backing::PendingWriteBack pending;
    bool twoPhase = false;
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* peripheral = bus.peripheral(slot);
        auto* media = dynamic_cast<MountableMediaCard*>(peripheral);
        if (!media)
            return commandError("slot " + std::to_string(slot) +
                                " has no mountable media");
        if (bay < 0 || bay >= media->bayCount())
            return commandError("invalid media bay " +
                                std::to_string(bay + 1));
        std::string prepareError;
        twoPhase = media->prepareEjectBay(bay, pending, prepareError);
        if (!twoPhase && !prepareError.empty())
            return commandError(prepareError);
    }

    if (twoPhase && pending.valid) {
        const std::vector<std::uint32_t> captured = pending.dirtyIndices;
        std::string error;
        if (!Block512Backing::commitWriteBack(std::move(pending), error)) {
            // Put the captured dirty set back so the still-mounted medium is
            // dirty again and a retry re-captures it (pre-split behaviour: a
            // failed save loses nothing). Re-resolved: the bus can be
            // rebuilt while the commit runs unlocked.
            auto state = controller.lockState();
            auto& bus = state.memory().slotBus();
            if (auto* media =
                    dynamic_cast<MountableMediaCard*>(bus.peripheral(slot)))
                media->restoreBayDirty(bay, captured);
            return commandError(error.empty()
                ? "the image could not be saved" : error);
        }
    }

    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* peripheral = bus.peripheral(slot);
        auto* media = dynamic_cast<MountableMediaCard*>(peripheral);
        // Re-resolved: the bus can be rebuilt while the commit runs unlocked.
        if (!media)
            return commandError("slot " + std::to_string(slot) +
                                " has no mountable media");
        if (bay < 0 || bay >= media->bayCount())
            return commandError("invalid media bay " +
                                std::to_string(bay + 1));
        // No phase-3 dirty bookkeeping: phase 1 retired the flags it
        // captured, and any block the guest dirtied while phase 2 ran
        // unlocked kept its flag — this ejectBay flushes that (normally
        // empty) remainder inline before dropping the medium.
        if (!media->ejectBay(bay)) {
            const auto info = media->bayInfo(bay);
            return commandError(info.lastError.empty()
                ? "the image could not be saved" : info.lastError);
        }
        (void)appendMediaBaySettingUpdates(
            updates, *peripheral, slot, bay,
            autoHdvSlot_, autoSmartPortSlot_);
        result.ok = true;
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();
    return result;
}

StorageCoordinator::MediaCommandResult
StorageCoordinator::setMediaBayWriteBack(
    EmulationController& controller, Settings& settings, int slot, int bay,
    bool enabled) const
{
    MediaCommandResult result;
    std::vector<SettingUpdate> updates;
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* peripheral = bus.peripheral(slot);
        auto* media = dynamic_cast<MountableMediaCard*>(peripheral);
        if (!media)
            return commandError("slot " + std::to_string(slot) +
                                " has no mountable media");
        if (bay < 0 || bay >= media->bayCount())
            return commandError("invalid media bay " +
                                std::to_string(bay + 1));
        media->setBayWriteBack(bay, enabled);
        (void)appendMediaBaySettingUpdates(
            updates, *peripheral, slot, bay,
            autoHdvSlot_, autoSmartPortSlot_);
        result.ok = true;
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();
    return result;
}

StorageCoordinator::MediaCommandResult StorageCoordinator::setMediaBayType(
    EmulationController& controller, Settings& settings, int slot, int bay,
    const std::string& kind) const
{
    MediaCommandResult result;
    std::vector<SettingUpdate> updates;
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* peripheral = bus.peripheral(slot);
        auto* media = dynamic_cast<MountableMediaCard*>(peripheral);
        if (!media)
            return commandError("slot " + std::to_string(slot) +
                                " has no mountable media");
        if (bay < 0 || bay >= media->bayCount())
            return commandError("invalid media bay " +
                                std::to_string(bay + 1));
        const auto options = media->bayTypeOptions(bay);
        const bool supported = std::any_of(
            options.begin(), options.end(), [&](const auto& option) {
                return option.first == kind;
            });
        if (!supported)
            return commandError("unsupported media type '" + kind + "'");
        media->setBayType(bay, kind);
        (void)appendMediaBaySettingUpdates(
            updates, *peripheral, slot, bay,
            autoHdvSlot_, autoSmartPortSlot_);
        result.ok = true;
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();
    return result;
}

StorageCoordinator::Disk35Snapshot StorageCoordinator::captureDisk35(
    EmulationController& controller) const
{
    Disk35Snapshot snapshot;
    {
        auto state = controller.lockState();
        const auto cards = topology(state.memory().slotBus());
        if (cards.primarySmartPort) {
            snapshot.smartPortSlot = cards.primarySmartPort->getSlot();
            for (int drive = 0; drive < 2; ++drive) {
                const auto* unit = dynamic_cast<const SmartPort35Unit*>(
                    cards.primarySmartPort->unit(
                        static_cast<std::size_t>(drive)));
                if (!unit) continue;
                copyDisk35ImageState(snapshot.drives[drive], unit->image());
            }
        } else {
            const Sony35Drive* drives[2] = {
                &controller.sony35Internal(), &controller.sony35External(),
            };
            const Disk35Image* images[2] = {
                &controller.disk35Internal(), &controller.disk35External(),
            };
            for (int drive = 0; drive < 2; ++drive) {
                auto& target = snapshot.drives[drive];
                copyDisk35ImageState(target, *images[drive]);
                target.loaded = drives[drive]->isInserted();
                target.motorOn = drives[drive]->isMotorOn();
                target.track = drives[drive]->track();
                target.side1 = drives[drive]->side1();
                target.writeProtected = drives[drive]->isWriteProtected();
            }
        }
    }
    for (auto& drive : snapshot.drives) {
        if (drive.isWoz) drive.convertTargetPath = freePoNameFor(drive.path);
    }
    return snapshot;
}

StorageCoordinator::RoutedMediaCommandResult
StorageCoordinator::mountDisk35(
    EmulationController& controller, Settings& settings, int drive,
    const std::string& path) const
{
    RoutedMediaCommandResult result;
    if (drive < 0 || drive >= 2) {
        result.error = "invalid 3.5-inch drive " +
                       std::to_string(drive + 1);
        return result;
    }

    std::vector<SettingUpdate> updates;
    {
        auto state = controller.lockState();
        const auto cards = topology(state.memory().slotBus());
        if (cards.primarySmartPort) {
            result.usesSmartPort = true;
            result.bootSlot = cards.primarySmartPort->getSlot();
            result.ok = mountSmartPortUnitAs(
                *cards.primarySmartPort, drive, SmartPort35Unit::kKindKey,
                path, updates, result.error, autoSmartPortSlot_);
        }
    }
    if (result.usesSmartPort) {
        applySettingUpdates(settings, updates);
        if (!updates.empty()) (void)settings.save();
        return result;
    }

    result.ok = controller.mount35(drive, path);
    if (!result.ok) {
        auto state = controller.lockState();
        const auto& image = drive == 0
            ? controller.disk35Internal() : controller.disk35External();
        result.error = image.lastError();
        if (result.error.empty()) result.error = "3.5-inch mount failed";
    }
    return result;
}

StorageCoordinator::MediaCommandResult StorageCoordinator::ejectDisk35(
    EmulationController& controller, Settings& settings, int drive) const
{
    MediaCommandResult result;
    if (drive < 0 || drive >= 2)
        return commandError("invalid 3.5-inch drive " +
                            std::to_string(drive + 1));

    bool usesSmartPort = false;
    std::vector<SettingUpdate> updates;
    {
        auto state = controller.lockState();
        const auto cards = topology(state.memory().slotBus());
        if (cards.primarySmartPort) {
            usesSmartPort = true;
            auto* unit = dynamic_cast<SmartPort35Unit*>(
                cards.primarySmartPort->unit(
                    static_cast<std::size_t>(drive)));
            if (!unit)
                return commandError("SmartPort unit " +
                    std::to_string(drive + 1) +
                    " is not a 3.5-inch drive");
            if (!unit->eject()) return commandError(unit->lastError());
            (void)appendMediaBaySettingUpdates(
                updates, *cards.primarySmartPort,
                cards.primarySmartPort->getSlot(), drive,
                autoHdvSlot_, autoSmartPortSlot_);
            result.ok = true;
        }
    }
    if (usesSmartPort) {
        applySettingUpdates(settings, updates);
        if (!updates.empty()) (void)settings.save();
        return result;
    }

    result.ok = controller.eject35(drive);
    if (!result.ok) {
        auto state = controller.lockState();
        const auto& image = drive == 0
            ? controller.disk35Internal() : controller.disk35External();
        result.error = image.lastError();
        if (result.error.empty()) result.error = "3.5-inch eject failed";
    }
    return result;
}

StorageCoordinator::MediaCommandResult
StorageCoordinator::setDisk35WriteBack(
    EmulationController& controller, Settings& settings, int drive,
    bool enabled) const
{
    MediaCommandResult result;
    if (drive < 0 || drive >= 2)
        return commandError("invalid 3.5-inch drive " +
                            std::to_string(drive + 1));

    std::vector<SettingUpdate> updates;
    {
        auto state = controller.lockState();
        const auto cards = topology(state.memory().slotBus());
        if (cards.primarySmartPort) {
            auto* unit = dynamic_cast<SmartPort35Unit*>(
                cards.primarySmartPort->unit(
                    static_cast<std::size_t>(drive)));
            if (!unit)
                return commandError("SmartPort unit " +
                    std::to_string(drive + 1) +
                    " is not a 3.5-inch drive");
            unit->setWriteBackEnabled(enabled);
            (void)appendMediaBaySettingUpdates(
                updates, *cards.primarySmartPort,
                cards.primarySmartPort->getSlot(), drive,
                autoHdvSlot_, autoSmartPortSlot_);
        } else {
            auto& image = drive == 0
                ? controller.disk35Internal() : controller.disk35External();
            image.setWriteBackEnabled(enabled);
        }
        result.ok = true;
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();
    return result;
}

StorageCoordinator::RoutedMediaCommandResult
StorageCoordinator::convertDisk35WozToPo(
    EmulationController& controller, Settings& settings, int drive) const
{
    RoutedMediaCommandResult result;
    if (drive < 0 || drive >= 2) {
        result.error = "invalid 3.5-inch drive " +
                       std::to_string(drive + 1);
        return result;
    }

    Disk35Image imageSnapshot;
    {
        auto state = controller.lockState();
        const auto cards = topology(state.memory().slotBus());
        if (cards.primarySmartPort) {
            const auto* unit = dynamic_cast<const SmartPort35Unit*>(
                cards.primarySmartPort->unit(
                    static_cast<std::size_t>(drive)));
            if (!unit) {
                result.error = "SmartPort unit " +
                    std::to_string(drive + 1) +
                    " is not a 3.5-inch drive";
                return result;
            }
            imageSnapshot = unit->image();
            result.usesSmartPort = true;
            result.bootSlot = cards.primarySmartPort->getSlot();
        } else {
            imageSnapshot = drive == 0
                ? controller.disk35Internal() : controller.disk35External();
        }
    }
    if (imageSnapshot.kind() != Disk35Image::ImageKind::Woz35) {
        result.error = "that drive does not hold a 3.5-inch WOZ";
        return result;
    }
    result.outputPath = freePoNameFor(imageSnapshot.path());
    if (result.outputPath.empty()) {
        result.error = "no free .po filename beside the WOZ";
        return result;
    }
    if (!imageSnapshot.exportRawTo(result.outputPath, result.error))
        return result;

    const auto mounted = mountDisk35(
        controller, settings, drive, result.outputPath);
    if (!mounted.ok) {
        result.error = "converted, but mounting failed: " + mounted.error;
        return result;
    }
    const auto writeBack = setDisk35WriteBack(
        controller, settings, drive, true);
    if (!writeBack.ok) {
        result.error = "converted and mounted, but write-back failed: " +
                       writeBack.error;
        return result;
    }
    result.ok = true;
    result.bootSlot = mounted.bootSlot;
    result.usesSmartPort = mounted.usesSmartPort;
    return result;
}

StorageCoordinator::RoutedMediaCommandResult StorageCoordinator::mountHdv(
    EmulationController& controller, Settings& settings,
    const std::string& path, bool smartPortOnly) const
{
    RoutedMediaCommandResult result;
    std::vector<SettingUpdate> updates;

    // Phase 1, NO lock — this is the 32 MiB case, the largest single stall the
    // tree had (25.8 ms under the lock before v0.8.5 split it).
    Block512Backing::PreparedImage prepared;
    std::string prepareError;
    const bool preparedOk =
        Block512Backing::readImageFile(path, prepared, prepareError);

    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        const auto cards = topology(bus);
        if (!smartPortOnly) {
            if (auto* card = cards.preferredBlock()) {
                auto* peripheral = bus.peripheral(card->getSlot());
                bool mounted = false;
                if (preparedOk) {
                    std::string adoptError;
                    if (card->adoptBay(0, std::move(prepared), adoptError))
                        mounted = true;   // else fall through, see mountMediaBay
                }
                if (!mounted && !card->mountBay(0, path, result.error)) {
                    if (result.error.empty()) result.error = prepareError;
                    return result;
                }
                if (peripheral) {
                    (void)appendMediaBaySettingUpdates(
                        updates, *peripheral, card->getSlot(), 0,
                        autoHdvSlot_, autoSmartPortSlot_);
                }
                result.bootSlot = card->getSlot();
                result.ok = true;
            }
        }
        if (!result.ok && cards.primarySmartPort) {
            result.usesSmartPort = true;
            result.bootSlot = cards.primarySmartPort->getSlot();
            // `prepared` is still untouched here: the block-card branch above
            // either takes it and succeeds or returns, so reaching this means
            // it never ran. Handing it over is what keeps the //c shape (no
            // block card, built-in SmartPort) from throwing phase 1 away and
            // reading the whole image a second time under the lock.
            result.ok = mountSmartPortUnitAs(
                *cards.primarySmartPort, 0, SmartPortHdvUnit::kKindKey,
                path, updates, result.error, autoSmartPortSlot_,
                preparedOk ? &prepared : nullptr);
        }
        if (!result.ok && result.error.empty())
            result.error = "no HDV or SmartPort card plugged";
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();
    return result;
}

StorageCoordinator::EjectAllResult StorageCoordinator::ejectAllMedia(
    EmulationController& controller, Settings& settings) const
{
    EjectAllResult result;
    std::vector<SettingUpdate> updates;
    std::array<bool, 2> onboardDisk35Loaded{};
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        const auto cards = topology(bus);

        for (auto* card : cards.diskIICards) {
            if (!card) continue;
            for (int drive = 0; drive < DiskIICard::kDriveCount; ++drive) {
                if (!card->isDiskLoaded(drive)) continue;
                if (card->ejectDisk(drive)) {
                    result.changed = true;
                    appendDiskIIDriveSettingUpdates(
                        updates, bus, *card, drive);
                } else {
                    result.failures.push_back(
                        "Disk II slot " + std::to_string(card->getSlot()) +
                        " drive " + std::to_string(drive + 1) + ": " +
                        card->getLastError(drive));
                }
            }
        }
        for (auto* card : cards.blockCards) {
            if (!card || !card->isImageLoaded()) continue;
            auto* peripheral = bus.peripheral(card->getSlot());
            if (card->ejectImage()) {
                result.changed = true;
                if (peripheral) {
                    (void)appendMediaBaySettingUpdates(
                        updates, *peripheral, card->getSlot(), 0,
                        autoHdvSlot_, autoSmartPortSlot_);
                }
            } else {
                result.failures.push_back(
                    "block device slot " + std::to_string(card->getSlot()) +
                    ": " + card->getLastError());
            }
        }
        for (auto* card : cards.smartPortCards) {
            if (!card) continue;
            for (std::size_t bay = 0; bay < SmartPortCard::kMaxUnits; ++bay) {
                auto* unit = card->unit(bay);
                if (!unit || !unit->isLoaded()) continue;
                if (unit->eject()) {
                    result.changed = true;
                    (void)appendMediaBaySettingUpdates(
                        updates, *card, card->getSlot(),
                        static_cast<int>(bay),
                        autoHdvSlot_, autoSmartPortSlot_);
                } else {
                    result.failures.push_back(
                        "SmartPort slot " +
                        std::to_string(card->getSlot()) + " bay " +
                        std::to_string(bay + 1) + ": " + unit->lastError());
                }
            }
        }
        for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
            auto* peripheral = bus.peripheral(slot);
            auto* media = genericMediaCard(peripheral);
            if (!media) continue;
            for (int bay = 0; bay < media->bayCount(); ++bay) {
                if (!media->bayInfo(bay).loaded) continue;
                if (media->ejectBay(bay)) {
                    result.changed = true;
                    (void)appendMediaBaySettingUpdates(
                        updates, *peripheral, slot, bay,
                        autoHdvSlot_, autoSmartPortSlot_);
                } else {
                    result.failures.push_back(
                        "slot " + std::to_string(slot) + " bay " +
                        std::to_string(bay + 1) + ": " +
                        media->bayInfo(bay).lastError);
                }
            }
        }
        onboardDisk35Loaded[0] = controller.disk35Internal().isLoaded();
        onboardDisk35Loaded[1] = controller.disk35External().isLoaded();
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();

    // EmulationController::eject35 owns the same state lock internally, so
    // on-board media must be handled after the slot-media critical section.
    for (int drive = 0; drive < 2; ++drive) {
        if (!onboardDisk35Loaded[drive]) continue;
        if (controller.eject35(drive)) {
            result.changed = true;
            continue;
        }
        auto state = controller.lockState();
        const auto& image = drive == 0
            ? controller.disk35Internal() : controller.disk35External();
        result.failures.push_back(
            "on-board 3.5-inch drive " + std::to_string(drive + 1) +
            ": " + image.lastError());
    }
    return result;
}

StorageCoordinator::RestoreSettingsResult
StorageCoordinator::restoreMediaFromSettings(
    SlotBus& bus, const Settings& settings) const
{
    RestoreSettingsResult result;
    const auto cards = topology(bus);
    for (auto* card : cards.diskIICards) {
        if (!card) continue;
        const bool isPrimary = card == cards.primaryDiskII;
        card->setWriteBackEnabled(settings.getBool(
            "disk_writeback_slot" + std::to_string(card->getSlot()),
            isPrimary ? settings.getBool("disk_writeback", false) : false));
        for (std::size_t drive = 0; drive < kDiskIIDriveCount; ++drive) {
            const std::string path = settings.getString(
                diskIIPathSettingKey(card->getSlot(), drive),
                drive == 0 && isPrimary
                    ? settings.getString("disk_path", "") : std::string());
            std::error_code ec;
            if (!path.empty() && std::filesystem::is_regular_file(path, ec) &&
                !card->insertDisk(static_cast<int>(drive), path)) {
                result.warnings.push_back(
                    "Disk II slot " + std::to_string(card->getSlot()) +
                    " drive " + std::to_string(drive + 1) + ": " +
                    card->getLastError(static_cast<int>(drive)));
            }
        }
    }

    if (cards.primaryHdv) {
        const std::string path = settings.getString("hdv_path", "");
        std::error_code ec;
        if (!path.empty() && std::filesystem::is_regular_file(path, ec) &&
            !cards.primaryHdv->loadImage(path)) {
            result.warnings.push_back(
                "HDV slot " + std::to_string(cards.primaryHdv->getSlot()) +
                ": " + cards.primaryHdv->getLastError());
        }
        cards.primaryHdv->setWriteBackEnabled(
            settings.getBool("hdv_writeback", false));
    }

    for (auto* block : cards.blockCards) {
        auto* card = dynamic_cast<CffaCard*>(block);
        if (!card) continue;
        const std::string key =
            "cffa_slot" + std::to_string(card->getSlot());
        const std::string path = settings.getString(key + "_path", "");
        std::error_code ec;
        if (!path.empty() && std::filesystem::is_regular_file(path, ec) &&
            !card->loadImage(path)) {
            result.warnings.push_back(
                "CFFA slot " + std::to_string(card->getSlot()) + ": " +
                card->getLastError());
        }
        card->setWriteBackEnabled(
            settings.getBool(key + "_writeback", false));
    }

    for (auto* card : cards.smartPortCards) {
        if (!card) continue;
        const std::string slotKey =
            "smartport_slot" + std::to_string(card->getSlot());
        for (std::size_t bay = 0; bay < SmartPortCard::kMaxUnits; ++bay) {
            const std::string base =
                slotKey + "_unit" + std::to_string(bay);
            const std::string kind =
                settings.getString(base + "_type", "");
            if (kind.empty()) continue;
            auto unit = makeSmartPortUnit(kind);
            if (!unit) {
                result.warnings.push_back(
                    "SmartPort slot " + std::to_string(card->getSlot()) +
                    " bay " + std::to_string(bay + 1) +
                    ": unknown media type '" + kind + "'");
                continue;
            }
            unit->setWriteBackEnabled(
                settings.getBool(base + "_writeback", false));

            const std::string path =
                settings.getString(base + "_path", "");
            std::string resolved;
            if (!path.empty()) {
                std::error_code error;
                for (const std::string& candidate : {
                         path, std::string("../") + path,
                         std::string("../../") + path}) {
                    if (std::filesystem::is_regular_file(candidate, error)) {
                        resolved = candidate;
                        break;
                    }
                    error.clear();
                }
            }
            if (!resolved.empty() && !unit->loadImage(resolved)) {
                result.warnings.push_back(
                    "SmartPort slot " + std::to_string(card->getSlot()) +
                    " bay " + std::to_string(bay + 1) + ": " +
                    unit->lastError());
            } else if (resolved.empty() && !path.empty()) {
                result.warnings.push_back(
                    "SmartPort slot " + std::to_string(card->getSlot()) +
                    " bay " + std::to_string(bay + 1) +
                    ": persisted path not found: " + path);
            }
            card->setUnit(bay, std::move(unit));
        }
    }

    // Cards with bays but no keyspace of their own (the Liron): the same
    // cwd anchors as the SmartPort units, the same warnings.
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        auto* media = genericMediaCard(bus.peripheral(slot));
        if (!media) continue;
        for (int bay = 0; bay < media->bayCount(); ++bay) {
            const std::string base = genericBayKey(slot, bay);
            media->setBayWriteBack(bay, settings.getBool(base + "_writeback", false));
            const std::string path = settings.getString(base + "_path", "");
            if (path.empty()) continue;
            std::string resolved;
            std::error_code error;
            for (const std::string& candidate : {
                     path, std::string("../") + path,
                     std::string("../../") + path}) {
                if (std::filesystem::is_regular_file(candidate, error)) {
                    resolved = candidate;
                    break;
                }
                error.clear();
            }
            std::string err;
            if (resolved.empty()) {
                result.warnings.push_back(
                    "slot " + std::to_string(slot) + " bay " +
                    std::to_string(bay + 1) + ": persisted path not found: " + path);
            } else if (!media->mountBay(bay, resolved, err)) {
                result.warnings.push_back(
                    "slot " + std::to_string(slot) + " bay " +
                    std::to_string(bay + 1) + ": " + err);
            }
        }
    }
    return result;
}

void StorageCoordinator::restoreRebuildSnapshot(
    SlotBus& bus, const RebuildSnapshot& snapshot) const
{
    for (const auto& disk : snapshot.diskII) {
        if (disk.slot < 1 || disk.slot >= SlotBus::kSlotCount) continue;
        auto* card = dynamic_cast<DiskIICard*>(bus.peripheral(disk.slot));
        if (!card) continue;

        // DiskImage snapshots the card policy during insert, so this order is
        // observable by the guest's write-protect probe.
        card->setWriteBackEnabled(disk.writeBackEnabled);
        for (std::size_t drive = 0; drive < disk.drives.size(); ++drive) {
            const auto& medium = disk.drives[drive];
            if (!medium.loaded || medium.path.empty()) {
                // The settings phase may have mounted a path last saved
                // before an in-session eject. A live snapshot entry proves
                // this card/drive existed and was empty, so it overrides that
                // stale setting. Cards absent from the snapshot are untouched.
                if (card->isDiskLoaded(static_cast<int>(drive)))
                    (void)card->ejectDisk(static_cast<int>(drive));
                continue;
            }
            std::error_code ec;
            if (std::filesystem::is_regular_file(medium.path, ec)) {
                (void)card->insertDisk(static_cast<int>(drive), medium.path);
            }
        }
    }

    const auto rebuilt = topology(bus);
    if (rebuilt.primaryHdv && snapshot.primaryHdv) {
        const auto& medium = *snapshot.primaryHdv;
        bool restored = false;
        if (medium.loaded && !medium.path.empty()) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(medium.path, ec))
                restored = rebuilt.primaryHdv->loadImage(medium.path);
        }
        if (!restored && rebuilt.primaryHdv->isImageLoaded())
            (void)rebuilt.primaryHdv->ejectImage();
        rebuilt.primaryHdv->setWriteBackEnabled(medium.writeBackEnabled);
    }

    for (const auto& medium : snapshot.cffa) {
        if (medium.slot < 1 || medium.slot >= SlotBus::kSlotCount) {
            continue;
        }
        auto* card = dynamic_cast<CffaCard*>(bus.peripheral(medium.slot));
        if (!card) continue;
        bool restored = false;
        if (medium.loaded && !medium.path.empty()) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(medium.path, ec))
                restored = card->loadImage(medium.path);
        }
        if (!restored && card->isImageLoaded()) (void)card->ejectImage();
        // Preserve the historical ordering for mounted CFFA images: apply
        // the opt-in after loading. Empty live state still owns the policy.
        card->setWriteBackEnabled(medium.writeBackEnabled);
    }
}

bool StorageCoordinator::flushAll(const SlotBus& bus,
                                  std::string& error) const
{
    error.clear();
    bool allSaved = true;
    const auto recordFailure = [&](std::string message) {
        if (!error.empty()) error += "; ";
        error += std::move(message);
        allSaved = false;
    };

    const auto cards = topology(bus);
    for (auto* card : cards.diskIICards) {
        if (card && !card->flushPendingWrites()) {
            recordFailure("Disk II slot " + std::to_string(card->getSlot()) +
                          ": " + card->getLastError());
        }
    }
    for (auto* card : cards.blockCards) {
        if (card && !card->saveDirty()) {
            recordFailure("block device slot " +
                          std::to_string(card->getSlot()) + ": " +
                          card->getLastError());
        }
    }
    for (auto* card : cards.smartPortCards) {
        for (size_t bay = 0; bay < SmartPortCard::kMaxUnits; ++bay) {
            auto* unit = card->unit(bay);
            if (unit && !unit->saveDirty()) {
                recordFailure("SmartPort slot " +
                              std::to_string(card->getSlot()) + " bay " +
                              std::to_string(bay + 1) + ": " +
                              unit->lastError());
            }
        }
    }
    // Every OTHER mountable-media card — today the Liron, whose bays are
    // `Disk35Image`s that no branch above reaches. `topology()` only knows the
    // three historical card families, so a card outside them was flushed by
    // nothing at all: quit and profile-switch both destroyed its medium with
    // the session's writes still in RAM, and the remount read the untouched
    // file back. `genericMediaCard` is the same "cards with no keyspace of
    // their own" filter the settings path uses, so nothing is flushed twice.
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        auto* media = genericMediaCard(bus.peripheral(slot));
        if (!media) continue;
        for (int bay = 0; bay < media->bayCount(); ++bay) {
            std::string err;
            if (!media->flushBay(bay, err)) {
                recordFailure("slot " + std::to_string(slot) + " bay " +
                              std::to_string(bay + 1) + ": " +
                              (err.empty() ? std::string("write-back failed")
                                           : err));
            }
        }
    }
    return allSaved;
}

SmartPort_ImGui::CardSnapshot
StorageCoordinator::captureSmartPortPanel(
    EmulationController& controller) const
{
    SmartPort_ImGui::CardSnapshot snapshot;
    auto state = controller.lockState();
    auto& bus = state.memory().slotBus();
    const auto* card = smartPortAt(bus);
    if (!card) return snapshot;

    snapshot.plugged = true;
    snapshot.slot = card->getSlot();
    for (std::size_t unitIndex = 0;
         unitIndex < snapshot.units.size(); ++unitIndex) {
        const SmartPortUnit* unit = card->unit(unitIndex);
        auto& unitSnapshot = snapshot.units[unitIndex];
        if (!unit) continue;
        unitSnapshot.kind = std::string(unit->kindKey());
        unitSnapshot.kindLabel = std::string(unit->kindLabel());
        unitSnapshot.path = unit->path();
        unitSnapshot.lastError = unit->lastError();
        unitSnapshot.blockCount = unit->blockCount();
        unitSnapshot.loaded = unit->isLoaded();
        unitSnapshot.writeProtected = unit->isWriteProtected();
        unitSnapshot.writeBackEnabled = unit->isWriteBackEnabled();
    }
    return snapshot;
}

StorageCoordinator::PanelCommandStatus
StorageCoordinator::applySmartPortPanel(
    EmulationController& controller, Settings& settings, int slot,
    const SmartPort_ImGui::Result& command) const
{
    PanelCommandStatus status;
    std::vector<SettingUpdate> settingUpdates;
    const std::string slotKey = "smartport_slot" + std::to_string(slot);

    // Mounts are collected under the lock and performed after it: an HDV is up
    // to 32 MiB and this panel runs every frame with the CPU worker live, so
    // reading the image inline froze the machine and the window together.
    struct PendingMount {
        std::size_t    unitIndex = 0;
        SmartPortUnit* unit      = nullptr;
        std::string    path;
        std::string    base;
    };
    std::vector<PendingMount> pendingMounts;

    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* card = smartPortAt(bus, slot);
        if (!card) return status;

        for (std::size_t unitIndex = 0;
             unitIndex < command.units.size(); ++unitIndex) {
            const auto& action = command.units[unitIndex];
            const std::string base = slotKey + "_unit" +
                                     std::to_string(unitIndex);
            const auto rememberString = [&](std::string key,
                                            std::string value) {
                settingUpdates.push_back(
                    {std::move(key), std::move(value), false, false});
            };
            const auto rememberBool = [&](std::string key, bool value) {
                settingUpdates.push_back(
                    {std::move(key), {}, value, true});
            };

            if (action.clearType || !action.setType.empty()) {
                if (action.clearType) {
                    card->setUnit(unitIndex, nullptr);
                    rememberString(base + "_type", "");
                    rememberString(base + "_path", "");
                    rememberBool(base + "_writeback", false);
                    status.message = "SmartPort unit " +
                        std::to_string(unitIndex) + ": cleared";
                } else {
                    auto unit = makeSmartPortUnit(action.setType);
                    if (unit) {
                        card->setUnit(unitIndex, std::move(unit));
                        rememberString(base + "_type", action.setType);
                        rememberString(base + "_path", "");
                        rememberBool(base + "_writeback", false);
                        status.message = "SmartPort unit " +
                            std::to_string(unitIndex) + ": type = " +
                            action.setType;
                    } else {
                        status.message = "SmartPort unit " +
                            std::to_string(unitIndex) + ": unknown type '" +
                            action.setType + "'";
                    }
                }
                status.visibleSeconds = 3.0;
                continue;
            }

            SmartPortUnit* unit = card->unit(unitIndex);
            if (!unit) continue;

            if (action.writeBackChanged) {
                unit->setWriteBackEnabled(action.writeBackOn);
                rememberBool(base + "_writeback", action.writeBackOn);
                status.message = "SmartPort unit " +
                    std::to_string(unitIndex) + ": write-back " +
                    (action.writeBackOn ? "ON" : "OFF");
                status.visibleSeconds = 3.0;
            }
            if (!action.mountPath.empty()) {
                // Deferred, not mounted here: mountSmartPortUnit takes
                // stateMutex itself for phase 2, and stateMutex is
                // non-recursive — doing it inside this scope would deadlock.
                pendingMounts.push_back(
                    {unitIndex, unit, action.mountPath, base});
            }
            if (action.eject) {
                const bool ok = unit->eject();
                if (ok) rememberString(base + "_path", "");
                status.message = "SmartPort unit " +
                    std::to_string(unitIndex) +
                    (ok ? ": ejected" : ": eject failed: " +
                                           unit->lastError());
                status.visibleSeconds = 4.0;
            }
        }
    }

    // Two phases, both outside the scope above: the read runs with no lock
    // held, the swap takes stateMutex on its own. The unit pointers stay valid
    // across the gap because the unit table is UI-thread-confined and this is
    // the UI thread (same contract as pom2::mountDiskII — MediaMount.h).
    for (auto& mount : pendingMounts) {
        std::string mountError;
        if (mountSmartPortUnit(controller, *mount.unit, mount.path,
                               mountError)) {
            appendStringSetting(settingUpdates, mount.base + "_path",
                                mount.path);
            status.message = "SmartPort unit " +
                std::to_string(mount.unitIndex) + ": mounted " + mount.path;
        } else {
            status.message = "SmartPort unit " +
                std::to_string(mount.unitIndex) + ": mount failed: " +
                mountError;
        }
        status.visibleSeconds = 4.0;
    }

    // Settings storage is deliberately outside the machine critical section.
    for (const auto& update : settingUpdates) {
        if (update.isBool)
            settings.setBool(update.key, update.boolValue);
        else
            settings.setString(update.key, update.stringValue);
    }
    if (!settingUpdates.empty()) settings.save();
    return status;
}

} // namespace pom2
