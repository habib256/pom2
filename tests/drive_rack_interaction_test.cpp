// POM2 — GPL-3.0-or-later
#include "DriveRack_ImGui.h"
#include "DriveRackFilePicker.h"
#include "NativeFileDialog.h"

// Keep this ImGui-input harness deterministic and headless. Native chooser
// protocol/backend checks are separate from the in-app browser interaction.
namespace pom2 {
bool nativeFileDialogAvailable() { return false; }
FileDialogResult nativeFileDialog(bool, const std::string&, const std::string&,
    const std::string&, const std::string&, std::string&) {
    return FileDialogResult::Unavailable;
}
}
#include "imgui.h"
#include <cassert>
#include <vector>

int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(900, 900);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    unsigned char* pixels; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    bool open = true;
    pom2::DrivePicture drive;
    drive.slot = 4; drive.bay = 1; drive.label = "Disk II drive 2";
    drive.kind = pom2::DrivePictureKind::DiskII;
    std::vector<pom2::DrivePicture> drives{drive};
    bool dragSource = false, modal = false;
    const char dropped[] = "/tmp/new.dsk";
    auto frame = [&] {
        ImGui::NewFrame();
        if (dragSource && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern)) {
            ImGui::SetDragDropPayload("POM2_DISK_IMAGE", dropped, sizeof(dropped));
            ImGui::TextUnformatted("new.dsk");
            ImGui::EndDragDropSource();
        }
        if (modal) {
            ImGui::OpenPopup("Blocking dialog");
            if (ImGui::BeginPopupModal("Blocking dialog")) {
                ImGui::TextUnformatted("Blocked"); ImGui::EndPopup();
            }
        }
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(370, 720));
        auto result = pom2::renderDriveRack(open, drives);
        ImGui::Render();
        return result;
    };
    frame(); frame();
    io.AddMousePosEvent(150, 140);
    frame();
    io.AddMouseButtonEvent(0, true);
    auto result = frame();
    io.AddMouseButtonEvent(0, false);
    auto release = frame();
    result.actions.insert(result.actions.end(), release.actions.begin(), release.actions.end());
    assert(result.actions.size() == 1 && "clicking an empty drive picture must open file selection");
    assert(result.actions[0].type == pom2::DriveRackAction::Type::Insert);
    assert(result.actions[0].driveIndex == 0);
    drives[0].loaded = true; drives[0].path = "existing.dsk";
    frame();
    io.AddMouseButtonEvent(0, true); result = frame();
    io.AddMouseButtonEvent(0, false); release = frame();
    assert(result.actions.empty() && release.actions.empty() && "occupied drive must not open insertion");
    // Texture-backed pictures use the same click and drag target as fallback drawings.
    drives[0].loaded = false; drives[0].path.clear(); drives[0].texture = 1;
    frame();
    io.AddMouseButtonEvent(0, true); result = frame();
    assert(result.actions.size() == 1 && result.actions[0].type == pom2::DriveRackAction::Type::Insert);
    assert(!result.hits.empty());
    io.AddMouseButtonEvent(0, false); frame();
    // Exercise the actual ImGui payload delivery, rather than a policy mirror.
    io.AddMouseButtonEvent(0, true); frame();
    dragSource = true; frame(); frame();
    io.AddMouseButtonEvent(0, false); result = frame();
    assert(result.actions.size() == 1 && result.actions[0].type == pom2::DriveRackAction::Type::Mount);
    assert(result.actions[0].path == dropped && result.actions[0].driveIndex == 0);
    dragSource = false; frame();
    drives[0].loaded = true;
    io.AddMouseButtonEvent(0, true); frame();
    dragSource = true; frame(); frame();
    io.AddMouseButtonEvent(0, false); result = frame();
    assert(result.actions.empty() && "dragging onto an occupied picture must not replace its disk");
    dragSource = false; frame();
    // Clipped / scrolled pictures must not receive drops outside the window.
    drives.resize(8, drive);
    for (std::size_t i = 0; i < drives.size(); ++i) drives[i].slot = static_cast<int>(i);
    result = frame();
    assert(!result.hits.empty() && result.hits.size() < drives.size());
    for (const auto& hit : result.hits) assert(hit.y0 >= 0 && hit.y1 < 720);
    // File selection errors keep the dialog open for correction; success closes it.
    pom2::DriveRackFilePicker picker;
    picker.open(drive);
    auto pickerFrame = [&] {
        ImGui::NewFrame(); picker.render();
        const bool visible = ImGui::IsPopupOpen("Insert disk into drive");
        ImGui::Render(); return visible;
    };
    assert(pickerFrame());
    picker.complete(false, "Invalid image"); assert(pickerFrame());
    picker.complete(true, ""); assert(!pickerFrame());
    frame(); frame();
    modal = true; frame(); result = frame();
    assert(result.hits.empty() && "a modal dialog blocks drive drop targets");
    ImGui::DestroyContext();
}
