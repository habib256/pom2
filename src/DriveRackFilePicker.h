// POM2 — GPL-3.0-or-later
#ifndef POM2_DRIVE_RACK_FILE_PICKER_H
#define POM2_DRIVE_RACK_FILE_PICKER_H
#include "DriveRack_ImGui.h"
#include <array>
#include <optional>
#include <string>
#include <vector>
namespace pom2 {
// `boot`: no drive was chosen — the image goes where its type maps and the
// machine boots it (File > Insert disk and boot…, the browser's import twin).
struct DriveRackSelection { DrivePicture target; std::string path; bool boot = false; };
class DriveRackFilePicker {
public:
    void open(const DrivePicture& target);
    void openForBoot();
    std::optional<DriveRackSelection> render();
    void complete(bool ok, const std::string& error);
private:
    void browse(const std::string& directory);
    struct Entry { std::string name, path; bool directory; };
    DrivePicture target_;
    std::string directory_, error_;
    std::array<char, 2048> path_{};
    std::array<char, 2048> folder_{};
    std::vector<Entry> entries_;
    bool showHidden_ = false;
    bool opening_ = false, closing_ = false, boot_ = false;
};
}
#endif
