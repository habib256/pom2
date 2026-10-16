// POM2 — GPL-3.0-or-later
#include "DriveRackMount.h"
#include "DiskIICard.h"
#include "DiskImage.h"
#include "EmulationController.h"
#include "Memory.h"
#include "MountableMediaCard.h"
#include "StorageCoordinator.h"

namespace pom2 {
bool mountEmptyDrive(EmulationController& controller, Settings& settings,
                     StorageCoordinator& storage, const DrivePicture& target,
                     const std::string& path, std::string& error) {
    error.clear();
    // Inspect fresh state: a file picker may remain open across media changes.
    if (target.slot < 0) {
        if (target.kind != DrivePictureKind::Floppy35 || target.bay < 0 || target.bay > 2) {
            error = "This drive is no longer available."; return false;
        }
        const auto snapshot = storage.captureDisk35(controller, true);
        if (!snapshot.drives[target.bay].connected) {
            error = "This drive is no longer connected."; return false;
        }
        if (snapshot.drives[target.bay].loaded) {
            error = "Eject the current disk before inserting another."; return false;
        }
    } else {
        auto state = controller.lockState();
        auto* card = state.memory().slotBus().peripheral(target.slot);
        if (target.kind == DrivePictureKind::DiskII) {
            auto* disk = dynamic_cast<DiskIICard*>(card);
            if (!disk || target.bay < 0 || target.bay >= DiskIICard::kDriveCount) {
                error = "This drive is no longer available."; return false;
            }
            if (disk->isDiskLoaded(target.bay)) {
                error = "Eject the current disk before inserting another."; return false;
            }
        } else {
            auto* media = dynamic_cast<MountableMediaCard*>(card);
            if (!media || target.bay < 0 || target.bay >= media->bayCount()) {
                error = "This drive is no longer available."; return false;
            }
            const auto info = media->bayInfo(target.bay);
            const bool floppy = info.typeKey == "35" || info.kindLabel.find("3.5") != std::string::npos;
            const bool hardDisk = info.typeKey == "hdv" || info.kindLabel.find("HD") != std::string::npos;
            if ((target.kind == DrivePictureKind::Floppy35 && !floppy) ||
                (target.kind == DrivePictureKind::HardDisk && !hardDisk) ||
                (target.kind == DrivePictureKind::Generic && !info.acceptsSony35OrHdv)) {
                error = "The drive type changed. Select the drive again."; return false;
            }
            if (info.loaded) {
                error = "Eject the current disk before inserting another."; return false;
            }
        }
    }
    // Classification does file I/O, outside the machine lock. Loaders validate
    // the actual payload; a bad image cannot be redirected to another drive.
    const auto kind = classifyDiskForSlot(path);
    const auto expected = target.kind == DrivePictureKind::DiskII ? DiskSlotClass::Floppy525 :
                          target.kind == DrivePictureKind::Floppy35 ? DiskSlotClass::Sony35 : DiskSlotClass::Hdv;
    // A bay that takes either kind lets the file decide (mountMediaBay routes).
    const bool either = target.kind == DrivePictureKind::Generic &&
                        (kind == DiskSlotClass::Sony35 || kind == DiskSlotClass::Hdv);
    if (kind != expected && !either) {
        error = target.kind == DrivePictureKind::DiskII ? "Choose a 5.25-inch disk image for this drive." :
                target.kind == DrivePictureKind::Floppy35 ? "Choose an 800K 3.5-inch disk image for this drive." :
                target.kind == DrivePictureKind::Generic ? "Choose an 800K 3.5-inch or ProDOS hard-disk image for this drive." :
                "Choose a ProDOS hard-disk image for this drive.";
        return false;
    }
    StorageCoordinator::MediaCommandResult result;
    if (target.slot < 0)
        result = storage.mountDisk35(controller, settings, target.bay, path, true);
    else if (target.kind == DrivePictureKind::DiskII)
        result = storage.mountDiskII(controller, settings, target.slot, target.bay, path, false);
    else
        result = storage.mountMediaBay(controller, settings, target.slot, target.bay, path);
    error = result.error;
    return result.ok;
}
}
