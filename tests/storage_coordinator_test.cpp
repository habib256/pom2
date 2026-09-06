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

// StorageCoordinator topology contract.
//
// SlotBus owns every card. The coordinator may return an ephemeral view to a
// caller holding the machine lock, but must never cache it across a re-plug.

#include "CffaCard.h"
#include "Disk35Image.h"
#include "DiskIICard.h"
#include "EmulationController.h"
#include "LironCard.h"
#include "ProDOSHardDiskCard.h"
#include "Settings.h"
#include "SlotBus.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "StorageCoordinator.h"
#include "Woz35Fixture.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string writeImage(const std::string& name, std::size_t bytes,
                       std::uint8_t fill)
{
    const auto path = std::filesystem::temp_directory_path() / name;
    const std::vector<std::uint8_t> contents(bytes, fill);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(contents.data()),
                 static_cast<std::streamsize>(contents.size()));
    assert(stream.good());
    return path.string();
}

} // namespace

int main()
{
    EmulationController controller;
    pom2::StorageCoordinator storage;

    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        bus.plug(6, std::make_unique<DiskIICard>(6));
        bus.plug(4, std::make_unique<DiskIICard>(4));
        bus.plug(5, std::make_unique<ProDOSHardDiskCard>(5));
        bus.plug(7, std::make_unique<pom2::CffaCard>(7));
        bus.plug(3, std::make_unique<pom2::SmartPortCard>(3));

        const auto topology = storage.topology(bus);
        assert(topology.diskIICards.size() == 2);
        assert(topology.diskIICards[0]->getSlot() == 4);
        assert(topology.diskIICards[1]->getSlot() == 6);
        assert(topology.primaryDiskII == topology.diskIIAt(4));
        assert(topology.diskIIAt(6) != nullptr);
        assert(topology.primaryHdv != nullptr);
        assert(topology.primaryHdv->getSlot() == 5);
        assert(topology.primaryCffa != nullptr);
        assert(topology.primaryCffa->getSlot() == 7);
        assert(topology.preferredBlock() == topology.primaryCffa);
        assert(topology.primarySmartPort != nullptr);
        assert(topology.primarySmartPort->getSlot() == 3);
    }

    const auto initial = storage.captureInventory(controller);
    assert(initial.diskIISlots == std::vector<int>({4, 6}));
    assert(initial.blockSlots == std::vector<int>({5, 7}));
    assert(initial.smartPortSlots == std::vector<int>({3}));
    assert(initial.primaryDiskIISlot == 4);
    assert(initial.primaryHdvSlot == 5);
    assert(initial.primaryCffaSlot == 7);
    assert(initial.primarySmartPortSlot == 3);

    // Destroy the old primary cards and reuse their slots for different card
    // types. A cached alias would now either report stale data or trip ASan.
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        (void)bus.unplug(4);
        (void)bus.unplug(5);
        (void)bus.unplug(7);
        bus.plug(4, std::make_unique<pom2::SmartPortCard>(4));
        bus.plug(7, std::make_unique<ProDOSHardDiskCard>(7));
    }

    const auto rebuilt = storage.captureInventory(controller);
    assert(rebuilt.diskIISlots == std::vector<int>({6}));
    assert(rebuilt.blockSlots == std::vector<int>({7}));
    assert(rebuilt.smartPortSlots == std::vector<int>({3, 4}));
    assert(rebuilt.primaryDiskIISlot == 6);
    assert(rebuilt.primaryHdvSlot == 7);
    assert(rebuilt.primaryCffaSlot == -1);
    assert(rebuilt.primarySmartPortSlot == 3);

    std::string flushError;
    {
        auto state = controller.lockState();
        assert(storage.flushAll(state.memory().slotBus(), flushError));
    }
    assert(flushError.empty());

    const std::string diskPath = writeImage(
        "pom2_storage_rebuild.dsk", 35u * 16u * 256u, 0x00);
    const std::string disk2Path = writeImage(
        "pom2_storage_rebuild_drive2.dsk", 35u * 16u * 256u, 0x44);
    const std::string hdvPath = writeImage(
        "pom2_storage_rebuild_hdv.hdv", 8u * 512u, 0x11);
    const std::string cffaPath = writeImage(
        "pom2_storage_rebuild_cffa.hdv", 8u * 512u, 0x22);
    const std::string stalePath = writeImage(
        "pom2_storage_rebuild_stale.hdv", 8u * 512u, 0x33);
    const std::string invalidDiskPath = writeImage(
        "pom2_storage_rebuild_invalid.dsk", 3u, 0x55);
    const std::string invalidBlockPath = writeImage(
        "pom2_storage_rebuild_invalid.hdv", 3u, 0x66);
    const std::string disk35Path = writeImage(
        "pom2_storage_disk35.po", pom2::Disk35Image::kBytesPerImage, 0x77);

    // A rebuild snapshot contains values, never aliases into the old cards.
    // Destroy the complete topology, then prove the copied media state can be
    // applied to replacement cards (including an HDV which moved slots).
    {
        EmulationController mediaController;
        pom2::StorageCoordinator mediaStorage;

        {
            auto state = mediaController.lockState();
            auto& bus = state.memory().slotBus();

            auto disk4 = std::make_unique<DiskIICard>(4);
            disk4->setWriteBackEnabled(true);
            assert(disk4->insertDisk(0, diskPath));
            assert(disk4->insertDisk(1, disk2Path));
            bus.plug(4, std::move(disk4));

            auto disk6 = std::make_unique<DiskIICard>(6);
            disk6->setWriteBackEnabled(true);
            bus.plug(6, std::move(disk6));

            auto hdv5 = std::make_unique<ProDOSHardDiskCard>(5);
            assert(hdv5->loadImage(hdvPath));
            hdv5->setWriteBackEnabled(true);
            bus.plug(5, std::move(hdv5));

            auto cffa7 = std::make_unique<pom2::CffaCard>(7);
            assert(cffa7->loadImage(cffaPath));
            cffa7->setWriteBackEnabled(true);
            bus.plug(7, std::move(cffa7));
        }

        pom2::StorageCoordinator::RebuildSnapshot mediaSnapshot;
        {
            auto state = mediaController.lockState();
            mediaSnapshot = mediaStorage.captureRebuildSnapshot(
                state.memory().slotBus());
        }
        assert(mediaSnapshot.diskII.size() == 2);
        assert(mediaSnapshot.diskII[0].slot == 4);
        assert(mediaSnapshot.diskII[0].drives[0].loaded);
        assert(mediaSnapshot.diskII[0].drives[0].path == diskPath);
        assert(mediaSnapshot.diskII[0].drives[1].loaded);
        assert(mediaSnapshot.diskII[0].drives[1].path == disk2Path);
        assert(mediaSnapshot.diskII[0].writeBackEnabled);
        assert(mediaSnapshot.diskII[1].slot == 6);
        assert(!mediaSnapshot.diskII[1].drives[0].loaded);
        assert(!mediaSnapshot.diskII[1].drives[1].loaded);
        assert(mediaSnapshot.diskII[1].writeBackEnabled);
        assert(mediaSnapshot.primaryHdv.has_value());
        assert(mediaSnapshot.primaryHdv->slot == 5);
        assert(mediaSnapshot.primaryHdv->path == hdvPath);
        assert(mediaSnapshot.cffa.size() == 1);
        assert(mediaSnapshot.cffa[0].slot == 7);
        assert(mediaSnapshot.cffa[0].path == cffaPath);
        assert(mediaSnapshot.cffa[0].writeBackEnabled);

        // Slot-configuration persistence keeps per-slot Disk II/CFFA state,
        // while a session-only HDV must not overwrite the configured image.
        pom2::Settings settings;
        settings.setString("hdv_path", "configured.hdv");
        settings.setBool("hdv_writeback", true);
        mediaStorage.markAutoProvisionedHdv(5);
        mediaStorage.persistRebuildSettings(settings, mediaSnapshot);
        assert(settings.getString("hdv_path") == "configured.hdv");
        assert(settings.getString("disk_path_slot4") == diskPath);
        assert(settings.getString("disk_path_slot4_drive2") == disk2Path);
        assert(settings.getBool("disk_writeback_slot4"));
        assert(settings.getString("disk_path_slot6").empty());
        assert(settings.getBool("disk_writeback_slot6"));
        assert(settings.getString("cffa_slot7_path") == cffaPath);
        assert(settings.getBool("cffa_slot7_writeback"));

        mediaStorage.clearAutoProvisioned();
        mediaStorage.persistRebuildSettings(settings, mediaSnapshot);
        assert(settings.getString("hdv_path") == hdvPath);

        // Shutdown persistence adds legacy primary aliases and HDV policy to
        // the same per-slot/two-drive values. Auto-provisioned HDV media are
        // explicitly cleared and do not overwrite the configured opt-in.
        pom2::Settings sessionSettings;
        sessionSettings.setString("hdv_path", "configured.hdv");
        sessionSettings.setBool("hdv_writeback", false);
        mediaStorage.markAutoProvisionedHdv(5);
        mediaStorage.persistSessionSettings(sessionSettings, mediaSnapshot);
        assert(sessionSettings.getString("hdv_path").empty());
        assert(!sessionSettings.getBool("hdv_writeback"));
        mediaStorage.clearAutoProvisioned();
        mediaStorage.persistSessionSettings(sessionSettings, mediaSnapshot);
        assert(sessionSettings.getString("hdv_path") == hdvPath);
        assert(sessionSettings.getBool("hdv_writeback"));
        assert(sessionSettings.getString("disk_path") == diskPath);
        assert(sessionSettings.getBool("disk_writeback"));
        assert(sessionSettings.getString("disk_path_slot4_drive2") ==
               disk2Path);
        assert(sessionSettings.getString("cffa_slot7_path") == cffaPath);

        {
            auto state = mediaController.lockState();
            auto& bus = state.memory().slotBus();
            bus.clear();
            bus.plug(4, std::make_unique<DiskIICard>(4));
            bus.plug(6, std::make_unique<DiskIICard>(6));
            bus.plug(3, std::make_unique<ProDOSHardDiskCard>(3));
            bus.plug(7, std::make_unique<pom2::CffaCard>(7));

            // Simulate the settings phase resurrecting an image which was
            // ejected during the live session. The following live snapshot
            // overlay must make the old card's empty drive authoritative.
            auto* staleDisk6 =
                dynamic_cast<DiskIICard*>(bus.peripheral(6));
            assert(staleDisk6 && staleDisk6->insertDisk(diskPath));

            mediaStorage.restoreRebuildSnapshot(bus, mediaSnapshot);

            auto* rebuiltDisk4 = dynamic_cast<DiskIICard*>(bus.peripheral(4));
            auto* rebuiltDisk6 = dynamic_cast<DiskIICard*>(bus.peripheral(6));
            auto* rebuiltHdv =
                dynamic_cast<ProDOSHardDiskCard*>(bus.peripheral(3));
            auto* rebuiltCffa =
                dynamic_cast<pom2::CffaCard*>(bus.peripheral(7));
            assert(rebuiltDisk4 && rebuiltDisk4->isDiskLoaded());
            assert(rebuiltDisk4->getDiskPath() == diskPath);
            assert(rebuiltDisk4->isDiskLoaded(1));
            assert(rebuiltDisk4->getDiskPath(1) == disk2Path);
            assert(rebuiltDisk4->isWriteBackEnabled());
            assert(rebuiltDisk6 && !rebuiltDisk6->isDiskLoaded());
            assert(rebuiltDisk6->isWriteBackEnabled());
            assert(rebuiltHdv && rebuiltHdv->isImageLoaded());
            assert(rebuiltHdv->getImagePath() == hdvPath);
            assert(rebuiltCffa && rebuiltCffa->isImageLoaded());
            assert(rebuiltCffa->getImagePath() == cffaPath);
            assert(rebuiltCffa->isWriteBackEnabled());

            // Empty live block-card state is authoritative too. This prevents
            // settings saved before an in-session eject from resurrecting an
            // HDV/CFFA during a profile switch.
            auto emptyBlocks = mediaSnapshot;
            emptyBlocks.primaryHdv->loaded = false;
            emptyBlocks.primaryHdv->path.clear();
            emptyBlocks.primaryHdv->writeBackEnabled = false;
            emptyBlocks.cffa[0].loaded = false;
            emptyBlocks.cffa[0].path.clear();
            emptyBlocks.cffa[0].writeBackEnabled = false;
            mediaStorage.restoreRebuildSnapshot(bus, emptyBlocks);
            assert(!rebuiltHdv->isImageLoaded());
            assert(!rebuiltHdv->isWriteBackEnabled());
            assert(!rebuiltCffa->isImageLoaded());
            assert(!rebuiltCffa->isWriteBackEnabled());
            mediaStorage.restoreRebuildSnapshot(bus, mediaSnapshot);
            assert(rebuiltHdv->getImagePath() == hdvPath);
            assert(rebuiltCffa->getImagePath() == cffaPath);

            // Eager drive-2 persistence must clear only drive 2; the old
            // implementation accidentally cleared drive 1's key here.
            assert(rebuiltDisk4->ejectDisk(1));
            assert(mediaStorage.persistDiskIIDrive(
                settings, bus, 4, 1));
            assert(settings.getString("disk_path_slot4") == diskPath);
            assert(settings.getString("disk_path_slot4_drive2").empty());
            mediaStorage.persistRebuildSettings(settings, mediaSnapshot);

            // Settings restoration is one coordinator operation performed
            // only after every replacement card exists.
            assert(rebuiltDisk4->ejectDisk());
            rebuiltDisk4->setWriteBackEnabled(false);
            assert(rebuiltHdv->loadImage(stalePath));
            assert(rebuiltCffa->loadImage(stalePath));
            rebuiltCffa->setWriteBackEnabled(false);
            const auto restored =
                mediaStorage.restoreMediaFromSettings(bus, settings);
            assert(restored.ok());
            assert(rebuiltDisk4->isDiskLoaded());
            assert(rebuiltDisk4->getDiskPath() == diskPath);
            assert(rebuiltDisk4->isDiskLoaded(1));
            assert(rebuiltDisk4->getDiskPath(1) == disk2Path);
            assert(rebuiltDisk4->isWriteBackEnabled());
            assert(rebuiltHdv->getImagePath() == hdvPath);
            assert(rebuiltHdv->isWriteBackEnabled());
            assert(rebuiltCffa->getImagePath() == cffaPath);
            assert(rebuiltCffa->isWriteBackEnabled());

            // A newly-added Disk II did not exist in the live snapshot. It
            // must still honour an older persisted image for its new slot.
            settings.setString("disk_path_slot2", diskPath);
            settings.setString("disk_path_slot2_drive2", disk2Path);
            settings.setBool("disk_writeback_slot2", true);
            bus.plug(2, std::make_unique<DiskIICard>(2));
            assert(mediaStorage.restoreMediaFromSettings(bus, settings).ok());
            auto* newDisk2 = dynamic_cast<DiskIICard*>(bus.peripheral(2));
            assert(newDisk2 && newDisk2->isDiskLoaded());
            assert(newDisk2->getDiskPath() == diskPath);
            assert(newDisk2->isDiskLoaded(1));
            assert(newDisk2->getDiskPath(1) == disk2Path);
            assert(newDisk2->isWriteBackEnabled());

            // A rejected image is diagnostic, not transactional: continue
            // restoring every other card and report each concrete target.
            settings.setString("disk_path_slot2", invalidDiskPath);
            settings.setString("hdv_path", invalidBlockPath);
            settings.setString("cffa_slot7_path", invalidBlockPath);
            const auto rejected =
                mediaStorage.restoreMediaFromSettings(bus, settings);
            assert(!rejected.ok());
            const auto hasWarning = [&](const std::string& prefix) {
                for (const auto& warning : rejected.warnings) {
                    if (warning.rfind(prefix, 0) == 0) return true;
                }
                return false;
            };
            assert(hasWarning("Disk II slot 2 drive 1:"));
            assert(hasWarning("HDV slot 3:"));
            assert(hasWarning("CFFA slot 7:"));
        }

        // Immediate frontend commands are addressed by slot/drive or
        // slot/bay. They own locking, mutation and persistence routing, so a
        // caller never needs to retain a concrete card alias. Read-only keeps
        // this deterministic test from touching the user's state.cfg.
        pom2::Settings commandSettings;
        commandSettings.setReadOnly(true);

        auto command = mediaStorage.mountDiskII(
            mediaController, commandSettings, 4, 1, disk2Path, true);
        assert(command.ok);
        assert(commandSettings.getString("disk_path_slot4_drive2") ==
               disk2Path);
        command = mediaStorage.setDiskIIWriteBack(
            mediaController, commandSettings, 4, true);
        assert(command.ok);
        assert(commandSettings.getBool("disk_writeback_slot4"));
        // Ejecting drive 2 clears drive 2's key AND LEAVES DRIVE 1's ALONE.
        // The second half is the regression that matters: every hand-rolled
        // eject in the frontend built `disk_path_slot<N>` for both drives, so
        // ejecting drive 2 cleared the path of the disk still sitting in
        // drive 1 while leaving `_drive2` set — after a crash or a kill (a
        // clean quit rewrites both keys from live state and hides it), drive 1
        // came back empty and the disk the user had just ejected came back
        // mounted. Those call sites now delegate here; this asserts the
        // contract they depend on.
        command = mediaStorage.mountDiskII(
            mediaController, commandSettings, 4, 0, diskPath, true);
        assert(command.ok);
        assert(commandSettings.getString("disk_path_slot4") == diskPath);
        command = mediaStorage.ejectDiskII(
            mediaController, commandSettings, 4, 1);
        assert(command.ok);
        assert(commandSettings.getString("disk_path_slot4_drive2").empty());
        assert(commandSettings.getString("disk_path_slot4") == diskPath);
        command = mediaStorage.ejectDiskII(
            mediaController, commandSettings, 4, 0);
        assert(command.ok);
        assert(commandSettings.getString("disk_path_slot4").empty());
        command = mediaStorage.mountDiskII(
            mediaController, commandSettings, 1, 0, diskPath);
        assert(!command.ok);
        assert(!command.error.empty());

        // ── Two-phase Disk II eject (bug hunt 2026-09-06 #13) ───────────
        // `ejectDisk` re-encodes the dirty tracks, rewrites the image and
        // fsyncs twice; doing that under `stateMutex` froze the CPU worker
        // and the window together. The split mirrors `prepareDisk` +
        // `installDisk` on the mount side: phase 1 lifts the medium out
        // (a move, no syscall), phase 2 writes it with the lock released,
        // phase 3 puts it back when the write failed.
        {
            // A DiskImage is ~242 KB: heap it, never a stack local (macOS
            // gives a thread 512 KB — see DiskIICard::prepareDisk).
            const auto stageDirty = [&](int track, int nibble) {
                auto staged = std::make_unique<DiskImage>();
                std::string err;
                assert(DiskIICard::prepareDisk(diskPath, true, *staged, err));
                const std::uint8_t cur = staged->nibbleAt(track, nibble);
                staged->writeNibbleAt(
                    track, nibble, static_cast<std::uint8_t>(cur ^ 0x01));
                assert(staged->hasUnsavedChanges());
                return staged;
            };

            std::unique_ptr<DiskImage> pending;
            {
                auto state = mediaController.lockState();
                auto* card = dynamic_cast<DiskIICard*>(
                    state.memory().slotBus().peripheral(4));
                assert(card);
                card->setWriteBackEnabled(true);
                assert(card->installDisk(0, std::move(*stageDirty(6, 80))));
                assert(card->hasUnsavedChanges(0));

                pending = card->takeEjectWriteBack(0);
                assert(pending && "phase 1 hands out the dirty medium");
                assert(!card->isDiskLoaded(0) &&
                       "phase 1 empties the bay with no file I/O");
            }
            std::string error;
            assert(DiskIICard::commitEjectWriteBack(*pending, error));
            assert(error.empty());

            // Phase 3: a failed commit re-mounts the medium so the writes
            // are not lost — what the inline path did by never ejecting.
            {
                auto state = mediaController.lockState();
                auto* card = dynamic_cast<DiskIICard*>(
                    state.memory().slotBus().peripheral(4));
                assert(card);
                assert(card->installDisk(0, std::move(*stageDirty(7, 90))));
                std::unique_ptr<DiskImage> doomed = card->takeEjectWriteBack(0);
                assert(doomed);
                assert(!card->isDiskLoaded(0));
                // Truncating the source is the deterministic, portable save
                // failure (same trick as disk_writeback_smoke_test).
                std::error_code ec;
                std::filesystem::resize_file(diskPath, 1, ec);
                assert(!ec);
                std::string err2;
                assert(!DiskIICard::commitEjectWriteBack(*doomed, err2));
                assert(!err2.empty());
                assert(card->restoreEjected(0, std::move(doomed)));
                assert(card->isDiskLoaded(0) &&
                       "a failed commit puts the medium back for a retry");
                assert(card->hasUnsavedChanges(0));
            }
            // Put the image back so the rest of the suite still has it.
            (void)writeImage("pom2_storage_rebuild.dsk", 35u * 16u * 256u,
                             0x00);
            command = mediaStorage.ejectDiskII(
                mediaController, commandSettings, 4, 0);
            assert(command.ok);
        }

        command = mediaStorage.mountMediaBay(
            mediaController, commandSettings, 3, 0, hdvPath);
        assert(command.ok);
        assert(commandSettings.getString("hdv_path") == hdvPath);
        command = mediaStorage.setMediaBayWriteBack(
            mediaController, commandSettings, 3, 0, true);
        assert(command.ok);
        assert(commandSettings.getBool("hdv_writeback"));

        command = mediaStorage.mountMediaBay(
            mediaController, commandSettings, 7, 0, cffaPath);
        assert(command.ok);
        assert(commandSettings.getString("cffa_slot7_path") == cffaPath);
        command = mediaStorage.setMediaBayWriteBack(
            mediaController, commandSettings, 7, 0, true);
        assert(command.ok);
        assert(commandSettings.getBool("cffa_slot7_writeback"));

        // Auto-provisioned and synthetic HDV media are deliberately
        // session-only: mounting either must preserve the configured path.
        commandSettings.setString("hdv_path", "configured-command.hdv");
        mediaStorage.markAutoProvisionedHdv(3);
        command = mediaStorage.mountMediaBay(
            mediaController, commandSettings, 3, 0, hdvPath);
        assert(command.ok);
        assert(commandSettings.getString("hdv_path") ==
               "configured-command.hdv");
        mediaStorage.clearAutoProvisioned();
        std::vector<std::uint8_t> syntheticBytes(2u * 512u, 0);
        command = mediaStorage.mountBlockBytes(
            mediaController, commandSettings, 3, std::move(syntheticBytes),
            "[host folder] deterministic", "deterministic");
        assert(command.ok);
        assert(commandSettings.getString("hdv_path") ==
               "configured-command.hdv");

        // Generic SmartPort bay commands share the same persistence seam.
        {
            auto state = mediaController.lockState();
            state.memory().slotBus().plug(
                5, std::make_unique<pom2::SmartPortCard>(5));
        }
        command = mediaStorage.setMediaBayType(
            mediaController, commandSettings, 5, 0, "hdv");
        assert(command.ok);
        assert(commandSettings.getString("smartport_slot5_unit0_type") ==
               "hdv");
        command = mediaStorage.mountMediaBay(
            mediaController, commandSettings, 5, 0, stalePath);
        assert(command.ok);
        assert(commandSettings.getString("smartport_slot5_unit0_path") ==
               stalePath);

        // Eject-all visits both Disk II drives and every block/SmartPort
        // image. One command saves the cleared settings after releasing the
        // machine lock.
        // A card with bays but no keyspace of its own — the Liron — goes
        // through the generic media_slotN_bayK_* keys, so what the user
        // mounted is back on the next launch (bug hunt 3 left this open).
        {
            const std::string lironPath =
                writeImage("pom2_storage_liron.po", 1600u * 512u, 0x00);
            bool lironPresent = false;
            {
                auto state = mediaController.lockState();
                auto& bus = state.memory().slotBus();
                (void)bus.unplug(1);
                auto liron = std::make_unique<pom2::LironCard>(1);
                lironPresent = liron->romLoaded();
                if (lironPresent) bus.plug(1, std::move(liron));
            }
            if (lironPresent) {
                command = mediaStorage.mountMediaBay(
                    mediaController, commandSettings, 1, 0, lironPath);
                assert(command.ok);
                assert(commandSettings.getString("media_slot1_bay0_path") ==
                       lironPath);
                command = mediaStorage.setMediaBayWriteBack(
                    mediaController, commandSettings, 1, 0, true);
                assert(command.ok);
                assert(commandSettings.getBool("media_slot1_bay0_writeback"));
                command = mediaStorage.ejectMediaBay(
                    mediaController, commandSettings, 1, 0);
                assert(command.ok);
                assert(commandSettings.getString("media_slot1_bay0_path").empty());

                commandSettings.setString("media_slot1_bay0_path", lironPath);
                {
                    auto state = mediaController.lockState();
                    auto& bus = state.memory().slotBus();
                    assert(mediaStorage.restoreMediaFromSettings(
                        bus, commandSettings).ok());
                    auto* card = dynamic_cast<pom2::LironCard*>(bus.peripheral(1));
                    assert(card);
                    const auto info = card->bayInfo(0);
                    assert(info.loaded && "the Liron's bay came back from settings");
                    assert(info.path == lironPath);
                    assert(info.writeBackEnabled);
                    assert(card->ejectBay(0));
                    (void)bus.unplug(1);
                }
                commandSettings.setString("media_slot1_bay0_path", "");

                // ── flushAll reaches a generic media bay (bug hunt #1) ──
                // `flushAll` walked Disk II / block / SmartPort cards only,
                // so the Liron — a MountableMediaCard outside all three —
                // was flushed by nothing: quit and profile switch destroyed
                // the card with the session's writes still in RAM, and the
                // remount read the untouched file back.
                {
                    {   // the persistence case above unplugged it
                        auto state = mediaController.lockState();
                        state.memory().slotBus().plug(
                            1, std::make_unique<pom2::LironCard>(1));
                    }
                    command = mediaStorage.mountMediaBay(
                        mediaController, commandSettings, 1, 0, lironPath);
                    assert(command.ok);
                    command = mediaStorage.setMediaBayWriteBack(
                        mediaController, commandSettings, 1, 0, true);
                    assert(command.ok);

                    std::vector<std::uint8_t> block(512, 0xC7);
                    {
                        auto state = mediaController.lockState();
                        auto& bus = state.memory().slotBus();
                        auto* card =
                            dynamic_cast<pom2::LironCard*>(bus.peripheral(1));
                        assert(card);
                        auto* image = card->drive(0).image();
                        assert(image && image->isLoaded());
                        assert(image->writeBlock(11, block.data()));
                        assert(card->bayInfo(0).hasUnsavedChanges);

                        std::string flushErr;
                        assert(mediaStorage.flushAll(bus, flushErr));
                        assert(flushErr.empty());
                        assert(!card->bayInfo(0).hasUnsavedChanges);
                    }
                    std::ifstream reread(lironPath, std::ios::binary);
                    reread.seekg(11 * 512);
                    std::vector<char> got(512, 0);
                    reread.read(got.data(), 512);
                    assert(std::memcmp(got.data(), block.data(), 512) == 0 &&
                           "flushAll must reach every mountable media bay");
                }

                // ── a failed save must refuse the eject (bug hunt #2) ────
                // `LironCard::ejectBay` discarded saveDirty()'s return and
                // ejected anyway; `Disk35Image::eject()` then dropped the
                // ONLY copy of everything written since the mount, with a
                // success return. A directory at <image>.pom2tmp is the
                // deterministic, cross-platform save failure.
                {
                    const std::string tmpName = lironPath + ".pom2tmp";
                    std::error_code ec;
                    std::filesystem::remove_all(tmpName, ec);
                    std::filesystem::create_directory(tmpName, ec);
                    assert(!ec);

                    std::vector<std::uint8_t> block(512, 0x3E);
                    auto state = mediaController.lockState();
                    auto& bus = state.memory().slotBus();
                    auto* card =
                        dynamic_cast<pom2::LironCard*>(bus.peripheral(1));
                    assert(card);
                    auto* image = card->drive(0).image();
                    assert(image && image->isLoaded());
                    assert(image->writeBlock(12, block.data()));

                    assert(!card->ejectBay(0) &&
                           "a failed save must refuse the eject");
                    assert(card->bayInfo(0).loaded &&
                           "the medium stays mounted so the user can retry");
                    assert(card->bayInfo(0).hasUnsavedChanges);

                    // …and the retry, once the obstacle is gone, works.
                    std::filesystem::remove_all(tmpName, ec);
                    assert(card->ejectBay(0));
                    assert(!card->bayInfo(0).loaded);
                    (void)bus.unplug(1);
                }
                commandSettings.setString("media_slot1_bay0_path", "");
            } else {
                std::cout << "  (no roms/liron.rom — Liron persistence case skipped)\n";
            }
        }

        assert(mediaStorage.mountDiskII(
            mediaController, commandSettings, 4, 0, diskPath).ok);
        assert(mediaStorage.mountDiskII(
            mediaController, commandSettings, 4, 1, disk2Path).ok);
        assert(mediaStorage.mountMediaBay(
            mediaController, commandSettings, 3, 0, hdvPath).ok);
        assert(mediaStorage.mountMediaBay(
            mediaController, commandSettings, 7, 0, cffaPath).ok);
        const auto ejected = mediaStorage.ejectAllMedia(
            mediaController, commandSettings);
        assert(ejected.ok());
        assert(ejected.changed);
        assert(commandSettings.getString("disk_path_slot4").empty());
        assert(commandSettings.getString("disk_path_slot4_drive2").empty());
        assert(commandSettings.getString("hdv_path").empty());
        assert(commandSettings.getString("cffa_slot7_path").empty());
        assert(commandSettings.getString(
            "smartport_slot5_unit0_path").empty());
        {
            auto state = mediaController.lockState();
            const auto topology = mediaStorage.topology(
                state.memory().slotBus());
            assert(!topology.diskIIAt(4)->isDiskLoaded(0));
            assert(!topology.diskIIAt(4)->isDiskLoaded(1));
            assert(!topology.primaryHdv->isImageLoaded());
            assert(!topology.primaryCffa->isImageLoaded());
            assert(!topology.primarySmartPort->unit(0)->isLoaded());
        }
    }

    // 3.5-inch commands have one authoritative target. With no SmartPort
    // card they operate on the on-board pair; once a card exists they create
    // and persist SmartPort35Unit media instead.
    {
        EmulationController disk35Controller;
        pom2::StorageCoordinator disk35Storage;
        pom2::Settings disk35Settings;
        disk35Settings.setReadOnly(true);

        auto disk35 = disk35Storage.captureDisk35(disk35Controller);
        assert(!disk35.usesSmartPort());
        assert(!disk35.drives[0].loaded);

        auto mounted35 = disk35Storage.mountDisk35(
            disk35Controller, disk35Settings, 0, disk35Path);
        assert(mounted35.ok);
        assert(!mounted35.usesSmartPort);
        assert(mounted35.bootSlot == -1);
        disk35 = disk35Storage.captureDisk35(disk35Controller);
        assert(disk35.drives[0].loaded);
        assert(disk35.drives[0].path == disk35Path);
        assert(disk35Storage.setDisk35WriteBack(
            disk35Controller, disk35Settings, 0, true).ok);
        assert(disk35Storage.captureDisk35(
            disk35Controller).drives[0].writeBackEnabled);
        const auto refusedConversion = disk35Storage.convertDisk35WozToPo(
            disk35Controller, disk35Settings, 0);
        assert(!refusedConversion.ok);
        assert(!refusedConversion.error.empty());
        assert(disk35Storage.ejectDisk35(
            disk35Controller, disk35Settings, 0).ok);
        assert(!disk35Storage.captureDisk35(
            disk35Controller).drives[0].loaded);

        // Exercise the complete WOZ -> writable PO transaction on a SYNTHETIC
        // 800K WOZ2. It used to copy `disks_3.5/The Oregon Trail 800K.woz`;
        // that disk is commercial and left the repository on 2026-09-05
        // (TODO.md § G1), and its absence aborted this test rather than
        // skipping it. A core test must not rest on a disk POM2 is not
        // allowed to redistribute — and the fixture is strictly better here,
        // because it pins the transaction against a known payload instead of
        // whatever one particular image happens to contain.
        const auto tempWoz =
            pom2::test::writeSyntheticWoz35("pom2_storage_convert.woz");
        const auto tempPo = std::filesystem::temp_directory_path() /
            "pom2_storage_convert.po";
        std::error_code fileError;
        std::filesystem::remove(tempPo, fileError);
        fileError.clear();
        assert(disk35Storage.mountDisk35(
            disk35Controller, disk35Settings, 0, tempWoz.string()).ok);
        disk35 = disk35Storage.captureDisk35(disk35Controller);
        assert(disk35.drives[0].isWoz);
        assert(disk35.drives[0].convertTargetPath == tempPo.string());
        const auto converted = disk35Storage.convertDisk35WozToPo(
            disk35Controller, disk35Settings, 0);
        assert(converted.ok);
        assert(converted.outputPath == tempPo.string());
        assert(std::filesystem::file_size(tempPo) ==
               pom2::Disk35Image::kBytesPerImage);
        disk35 = disk35Storage.captureDisk35(disk35Controller);
        assert(disk35.drives[0].path == tempPo.string());
        assert(disk35.drives[0].writeBackEnabled);
        std::filesystem::remove(tempWoz, fileError);
        std::filesystem::remove(tempPo, fileError);

        {
            auto state = disk35Controller.lockState();
            state.memory().slotBus().plug(
                5, std::make_unique<pom2::SmartPortCard>(5));
        }
        mounted35 = disk35Storage.mountDisk35(
            disk35Controller, disk35Settings, 1, disk35Path);
        assert(mounted35.ok);
        assert(mounted35.usesSmartPort);
        assert(mounted35.bootSlot == 5);
        assert(disk35Settings.getString(
            "smartport_slot5_unit1_type") == "35");
        assert(disk35Settings.getString(
            "smartport_slot5_unit1_path") == disk35Path);
        disk35 = disk35Storage.captureDisk35(disk35Controller);
        assert(disk35.usesSmartPort());
        assert(disk35.smartPortSlot == 5);
        assert(disk35.drives[1].loaded);
        assert(disk35Storage.setDisk35WriteBack(
            disk35Controller, disk35Settings, 1, true).ok);
        assert(disk35Settings.getBool(
            "smartport_slot5_unit1_writeback"));
        assert(disk35Storage.ejectDisk35(
            disk35Controller, disk35Settings, 1).ok);
        assert(disk35Settings.getString(
            "smartport_slot5_unit1_path").empty());

        const auto smartPortHdv = disk35Storage.mountHdv(
            disk35Controller, disk35Settings, hdvPath, true);
        assert(smartPortHdv.ok);
        assert(smartPortHdv.usesSmartPort);
        assert(smartPortHdv.bootSlot == 5);
        assert(disk35Settings.getString(
            "smartport_slot5_unit0_type") == "hdv");
        assert(disk35Settings.getString(
            "smartport_slot5_unit0_path") == hdvPath);

        // Settings restoration constructs SmartPort units only after the
        // card exists and reports every invalid type/path through one result.
        EmulationController restoreController;
        pom2::StorageCoordinator restoreStorage;
        pom2::Settings restoreSettings;
        restoreSettings.setReadOnly(true);
        restoreSettings.setString("smartport_slot5_unit0_type", "35");
        restoreSettings.setString(
            "smartport_slot5_unit0_path", disk35Path);
        restoreSettings.setBool(
            "smartport_slot5_unit0_writeback", true);
        {
            auto state = restoreController.lockState();
            auto& bus = state.memory().slotBus();
            bus.plug(5, std::make_unique<pom2::SmartPortCard>(5));
            const auto restored = restoreStorage.restoreMediaFromSettings(
                bus, restoreSettings);
            assert(restored.ok());
            const auto* card = dynamic_cast<const pom2::SmartPortCard*>(
                bus.peripheral(5));
            const auto* unit = dynamic_cast<const pom2::SmartPort35Unit*>(
                card ? card->unit(0) : nullptr);
            assert(unit && unit->isLoaded());
            assert(unit->path() == disk35Path);
            assert(unit->isWriteBackEnabled());
        }
    }

    std::error_code removeError;
    std::filesystem::remove(diskPath, removeError);
    std::filesystem::remove(disk2Path, removeError);
    std::filesystem::remove(hdvPath, removeError);
    std::filesystem::remove(cffaPath, removeError);
    std::filesystem::remove(stalePath, removeError);
    std::filesystem::remove(invalidDiskPath, removeError);
    std::filesystem::remove(invalidBlockPath, removeError);
    std::filesystem::remove(disk35Path, removeError);

    std::cout << "storage coordinator: OK\n";
    return 0;
}
