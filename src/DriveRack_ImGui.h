// POM2 — GPL-3.0-or-later
#ifndef POM2_DRIVE_RACK_IMGUI_H
#define POM2_DRIVE_RACK_IMGUI_H

#include <string>
#include <vector>

namespace pom2 {
enum class DrivePictureKind { DiskII, Floppy35, HardDisk, Generic };
struct DrivePicture {
    DrivePictureKind kind = DrivePictureKind::Generic;
    int slot = -1, bay = 0;
    std::string label, path;
    bool loaded = false, busy = false;
    bool writeProtected = false;
    unsigned int texture = 0;
    float pictureAspect = 0.62f;
};
// Value-only renderer: no machine access, file I/O or texture ownership.
struct DriveRackAction {
    enum class Type { Eject, ToggleWrite, Insert, Mount };
    Type type;
    std::size_t driveIndex;
    std::string path;
};
struct DriveRackHit {
    DrivePicture drive;
    float x0, y0, x1, y1;
};
struct DriveRackResult {
    bool manage = false;
    std::vector<DriveRackHit> hits;
    std::vector<DriveRackAction> actions;
};
DriveRackResult renderDriveRack(bool& open, const std::vector<DrivePicture>& drives);
} // namespace pom2
#endif
