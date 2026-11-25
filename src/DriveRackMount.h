// POM2 — GPL-3.0-or-later
#ifndef POM2_DRIVE_RACK_MOUNT_H
#define POM2_DRIVE_RACK_MOUNT_H
#include "DriveRack_ImGui.h"
#include "DiskImage.h"
#include <string>
#include <vector>
class EmulationController;
namespace pom2 {
class Settings;
class StorageCoordinator;
// Mount only into the named, still-empty mechanism. Never boot or auto-route.
/// Why `path` is no disk image, in words a non-specialist can act on.
std::string explainUnknownDisk(const std::string& path);
/// `fileKind`, when given, receives what the file was read as (Unknown when
/// the mount stopped before reading it), so a refusal can offer the drive
/// that does take it.
bool mountEmptyDrive(EmulationController& controller, Settings& settings,
                     StorageCoordinator& storage, const DrivePicture& target,
                     const std::string& path, std::string& error,
                     DiskSlotClass* fileKind = nullptr);
/// The first EMPTY drive among `drives` that takes a disk of `kind` (a
/// Disk II a 5.25-inch floppy, a 3.5-inch drive a 3.5-inch disk, a hard-disk
/// bay a hard disk, a bay that takes either both), or nullptr.
/// A drive spinning with no disk (the boot PROM waiting on S6D1) counts as
/// empty: it is the drive the machine is waiting on.
const DrivePicture* emptyDriveFor(const std::vector<DrivePicture>& drives, DiskSlotClass kind);
/// Whether a drive of this kind takes a disk of `kind`: the refusal offers
/// another drive only when the file is the wrong kind for the one chosen.
bool driveTakes(DrivePictureKind drive, DiskSlotClass kind);
}
#endif
