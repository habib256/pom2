// POM2 — GPL-3.0-or-later
#include "DriveRackMount.h"
#include "DiskIICard.h"
#include "DiskImage.h"
#include "EmulationController.h"
#include "Memory.h"
#include "MountableMediaCard.h"
#include "StorageCoordinator.h"
#include <filesystem>

namespace pom2 {
namespace {
// The words a refusal is written in. Whoever reads them is not expected to
// know what a Disk II, a SmartPort or a 2IMG is: say what the FILE is, what
// the DRIVE takes, why that matters, and what to do instead.
std::string quotedName(const std::string& path)
{
    return "\"" + std::filesystem::path(path).filename().string() + "\"";
}
const char* whatTheFileIs(DiskSlotClass k)
{
    switch (k) {
    case DiskSlotClass::Floppy525: return "a 5.25-inch floppy disk";
    case DiskSlotClass::Sony35:    return "a 3.5-inch disk";
    case DiskSlotClass::Hdv:       return "a hard disk";
    default:                       return "not an Apple II disk image";
    }
}
const char* whatTheDriveTakes(DrivePictureKind k)
{
    switch (k) {
    case DrivePictureKind::DiskII:   return "5.25-inch floppy disks only";
    case DrivePictureKind::Floppy35: return "3.5-inch disks only";
    case DrivePictureKind::HardDisk: return "hard disks only";
    default:                         return "3.5-inch disks and hard disks only";
    }
}
const char* whereItGoes(DiskSlotClass k)
{
    switch (k) {
    case DiskSlotClass::Floppy525: return "a 5.25-inch drive (Disk II)";
    case DiskSlotClass::Sony35:    return "a 3.5-inch drive";
    default:                       return "a hard-disk drive";
    }
}
constexpr const char* kDropOnScreen =
    "drop the file onto the emulator screen instead: POM2 then puts it in a "
    "drive that fits and starts it";
} // namespace

std::string explainUnknownDisk(const std::string& path)
{
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(path, ec);
    std::string size = ec ? std::string() : " (" + std::to_string(bytes / 1024) + " KB)";
    return quotedName(path) + size + " is not a disk image POM2 can read: its "
           "name and size match no Apple II disk format.\n\n"
           "Apple II disk images end in .dsk, .do, .po, .nib or .woz (5.25-inch "
           "floppies), .po, .2mg or .woz (3.5-inch disks), or .hdv / .2mg (hard "
           "disks). A .zip or .7z archive has to be unpacked first.";
}

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
        if (snapshot.drives[target.bay].loaded) { error = target.label + " already holds a disk" +
                 (target.path.empty() ? std::string() : " (" + quotedName(target.path) + ")") +
                 ", and a drive takes one disk at a time.\n\nEject it first "
                 "(click the drive, then Eject), or drop your disk onto an empty drive.";
                 return false; }
    } else {
        auto state = controller.lockState();
        auto* card = state.memory().slotBus().peripheral(target.slot);
        if (target.kind == DrivePictureKind::DiskII) {
            auto* disk = dynamic_cast<DiskIICard*>(card);
            if (!disk || target.bay < 0 || target.bay >= DiskIICard::kDriveCount) {
                error = "This drive is no longer available."; return false;
            }
            if (disk->isDiskLoaded(target.bay)) { error = target.label + " already holds a disk" +
                 (target.path.empty() ? std::string() : " (" + quotedName(target.path) + ")") +
                 ", and a drive takes one disk at a time.\n\nEject it first "
                 "(click the drive, then Eject), or drop your disk onto an empty drive.";
                 return false; }
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
            if (info.loaded) { error = target.label + " already holds a disk" +
                 (target.path.empty() ? std::string() : " (" + quotedName(target.path) + ")") +
                 ", and a drive takes one disk at a time.\n\nEject it first "
                 "(click the drive, then Eject), or drop your disk onto an empty drive.";
                 return false; }
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
    if (kind == DiskSlotClass::Unknown) { error = explainUnknownDisk(path); return false; }
    if (kind != expected && !either) {
        // Why, in the user's terms: a disk fits only a drive of its own kind,
        // as on the real machine.
        error = quotedName(path) + " is " + whatTheFileIs(kind) + ", but " +
                target.label + " takes " + whatTheDriveTakes(target.kind) +
                ". As on a real Apple II, a disk only goes into a drive made "
                "for it.\n\nWhat to do: " + kDropOnScreen + ", or drop it onto " +
                whereItGoes(kind) + ".";
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
