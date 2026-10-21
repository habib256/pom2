// POM2 — GPL-3.0-or-later
#ifndef POM2_DRIVE_RACK_MOUNT_H
#define POM2_DRIVE_RACK_MOUNT_H
#include "DriveRack_ImGui.h"
#include <string>
class EmulationController;
namespace pom2 {
class Settings;
class StorageCoordinator;
// Mount only into the named, still-empty mechanism. Never boot or auto-route.
/// Why `path` is no disk image, in words a non-specialist can act on.
std::string explainUnknownDisk(const std::string& path);
bool mountEmptyDrive(EmulationController& controller, Settings& settings,
                     StorageCoordinator& storage, const DrivePicture& target,
                     const std::string& path, std::string& error);
}
#endif
