// POM2 — GPL-3.0-or-later
#include "MainWindow.h"
#include "DiskIICard.h"
#include "DriveRack_ImGui.h"
#include "EmulationController.h"
#include "Memory.h"
#include "MountableMediaCard.h"
#include "StorageCoordinator.h"
#include "SystemProfile.h"
#include <algorithm>

void MainWindow::renderDriveRackWindow() {
    using pom2::DrivePicture;
    using Kind = pom2::DrivePictureKind;
    const bool compact = activeProfile == pom2::SystemProfile::AppleIIc ||
                         activeProfile == pom2::SystemProfile::AppleIIcPAL ||
                         activeProfile == pom2::SystemProfile::AppleIIcPlus;
    std::vector<DrivePicture> drives;
    // Copy values under the machine lock. Card pointers never survive it;
    // artwork decode/upload and drawing take place afterwards.
    {
        auto state = controller->lockState();
        auto& bus = state.memory().slotBus();
        for (int slot = 0; slot < SlotBus::kSlotCount; ++slot) {
            auto* card = bus.peripheral(slot);
            if (auto* disk = dynamic_cast<DiskIICard*>(card)) {
                for (int bay = 0; bay < DiskIICard::kDriveCount; ++bay) {
                    DrivePicture row;
                    row.kind = Kind::DiskII; row.slot = slot; row.bay = bay;
                    row.label = "Slot " + std::to_string(slot) + " · Disk II " + std::to_string(bay + 1);
                    if (compact) {
                        row.label = "Slot " + std::to_string(slot) + " · Disk //c 5.25\" " + std::to_string(bay + 1);
                        row.pictureAspect = 0.43f;
                    }
                    row.loaded = disk->isDiskLoaded(bay);
                    row.writeProtected = disk->isFileWriteProtected(bay);
                    row.busy = disk->isMotorOn() && disk->getActiveDrive() == bay;
                    row.path = disk->getDiskPath(bay);
                    drives.push_back(std::move(row));
                }
            } else if (auto* media = dynamic_cast<pom2::MountableMediaCard*>(card)) {
                for (int bay = 0; bay < media->bayCount(); ++bay) {
                    const auto info = media->bayInfo(bay);
                    // SmartPort's unassigned unit positions have no drive
                    // yet; configured mechanisms still appear when empty.
                    if (info.kindLabel.empty() && info.typeKey.empty() && !info.loaded) continue;
                    DrivePicture row;
                    row.slot = slot; row.bay = bay;
                    row.kind = info.typeKey == "35" || info.kindLabel.find("3.5") != std::string::npos
                        ? Kind::Floppy35 : info.typeKey == "hdv" || info.blockCount > 1600 ||
                          info.kindLabel.find("HD") != std::string::npos ? Kind::HardDisk : Kind::Generic;
                    if (row.kind == Kind::Floppy35) row.pictureAspect = 0.45f;
                    if (row.kind == Kind::HardDisk) row.pictureAspect = 0.40f;
                    const char* name = row.kind == Kind::Floppy35 ? "UniDisk 3.5\"" :
                                       row.kind == Kind::HardDisk ? "ProDOS HD" : "Drive";
                    row.label = "Slot " + std::to_string(slot) + " · " + name + " " + std::to_string(bay + 1);
                    row.loaded = info.loaded; row.busy = info.busy; row.path = info.path;
                    row.writeProtected = info.fileWriteProtected;
                    drives.push_back(std::move(row));
                }
            }
        }
    }
    // Physical Sony drives aren't slot-card bays. Include only connected
    // mechanisms, so //c+ external connection changes update the stack live.
    if (activeProfile == pom2::SystemProfile::AppleIIcPlus) {
        const auto sony = storageCoordinator_->captureDisk35(*controller, true);
        std::vector<DrivePicture> onboard;
        for (int bay = 0; bay < 3; ++bay) {
            const auto& info = sony.drives[bay];
            if (!info.connected) continue;
            DrivePicture row;
            row.kind = Kind::Floppy35; row.slot = -1; row.bay = bay;
            row.pictureAspect = 0.45f;
            row.label = bay == 0 ? "Apple 3.5\" · Internal" : "Apple 3.5\" · External " + std::to_string(bay);
            row.loaded = info.loaded; row.busy = info.motorOn; row.path = info.path;
            row.writeProtected = info.fileWriteProtected;
            onboard.push_back(std::move(row));
        }
        drives.insert(drives.begin(), onboard.begin(), onboard.end());
    }
    const auto order = [](Kind kind) {
        switch (kind) {
        case Kind::DiskII: return 0;
        case Kind::Floppy35: return 1;
        case Kind::HardDisk: return 2;
        case Kind::Generic: return 3;
        }
        return 3;
    };
    std::stable_sort(drives.begin(), drives.end(), [&](const DrivePicture& a, const DrivePicture& b) {
        return order(a.kind) < order(b.kind);
    });
    for (auto& drive : drives)
        if (drive.kind == Kind::DiskII)
            drive.texture = compact ? retroArtwork_.driveIIc(drive.loaded, drive.busy)
                                    : retroArtwork_.drive(drive.bay, drive.loaded, drive.busy);
        else if (drive.kind == Kind::Floppy35)
            drive.texture = retroArtwork_.drive35(drive.busy);
        else if (drive.kind == Kind::HardDisk)
            drive.texture = retroArtwork_.hardDisk(drive.busy);
    const auto result = pom2::renderDriveRack(show(pom2::PanelId::Drives), drives);
    if (result.manage)
        show(pom2::PanelId::Media) = true;
    for (const auto& action : result.actions) {
        const auto& drive = drives.at(action.driveIndex);
        pom2::StorageCoordinator::MediaCommandResult command;
        if (action.type == pom2::DriveRackAction::Type::ToggleWrite)
            command = storageCoordinator_->setMediaNotch(*controller, drive.path, !drive.writeProtected);
        else if (drive.slot < 0)
            command = storageCoordinator_->ejectDisk35(*controller, *settings, drive.bay, true);
        else if (drive.kind == Kind::DiskII)
            command = storageCoordinator_->ejectDiskII(*controller, *settings, drive.slot, drive.bay);
        else
            command = storageCoordinator_->ejectMediaBay(*controller, *settings, drive.slot, drive.bay);
        tapeStatusMessage = command.ok
            ? (action.type == pom2::DriveRackAction::Type::Eject ? "Disk ejected" : "Disk write protection updated")
            : command.error;
        tapeStatusUntil = lastFrameTime + 4.0;
    }
}
