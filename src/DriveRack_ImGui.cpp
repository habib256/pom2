// POM2 — GPL-3.0-or-later
#include "DriveRack_ImGui.h"
#include "IconsFontAwesome6.h"
#include "imgui.h"
#include <algorithm>

namespace pom2 {
namespace {
// Original vector illustrations for mechanisms without bundled artwork.
// ImGui draws them at the current scale, retaining sharp edges at any DPI.
void mechanismPicture(const DrivePicture& drive, float width) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float height = width * 0.62f;
    auto point = [&](float x, float y) { return ImVec2(origin.x + x * width, origin.y + y * height); };
    auto* draw = ImGui::GetWindowDrawList();
    const auto beige = IM_COL32(203, 194, 171, 255);
    const auto light = IM_COL32(233, 225, 204, 255);
    const auto dark = IM_COL32(68, 64, 57, 255);
    draw->AddRectFilled(point(0.04f, 0.06f), point(0.98f, 0.96f), IM_COL32(33, 31, 28, 200), 7);
    draw->AddRectFilled(point(0.02f, 0.02f), point(0.96f, 0.91f), beige, 7);
    draw->AddRectFilled(point(0.04f, 0.04f), point(0.94f, 0.16f), light, 4);
    draw->AddRect(point(0.02f, 0.02f), point(0.96f, 0.91f), dark, 7);
    if (drive.kind == DrivePictureKind::HardDisk) {
        for (int i = 0; i < 7; ++i)
            draw->AddLine(point(0.12f, 0.30f + i * 0.05f), point(0.68f, 0.30f + i * 0.05f), dark, 2);
        draw->AddRectFilled(point(0.78f, 0.31f), point(0.87f, 0.49f), dark, 3);
        draw->AddCircleFilled(point(0.825f, 0.40f), width * 0.017f, light);
    } else {
        draw->AddRectFilled(point(0.11f, 0.38f), point(0.78f, 0.46f), dark, 2);
        if (drive.loaded)
            draw->AddRectFilled(point(0.18f, 0.37f), point(0.70f, 0.43f), IM_COL32(40, 45, 55, 255), 2);
        draw->AddRectFilled(point(0.81f, 0.37f), point(0.89f, 0.47f), light, 2);
        draw->AddRect(point(0.81f, 0.37f), point(0.89f, 0.47f), dark, 2);
    }
    draw->AddCircleFilled(point(0.12f, 0.73f), width * 0.014f,
        drive.busy ? IM_COL32(255, 65, 28, 255) : IM_COL32(77, 44, 33, 255));
    const char* badge = drive.kind == DrivePictureKind::HardDisk ? "HARD DISK" :
                        drive.kind == DrivePictureKind::Floppy35 ? "3.5\" DRIVE" : "DRIVE";
    draw->AddText(point(0.20f, 0.68f), dark, badge);
    ImGui::InvisibleButton("##drive-picture", ImVec2(width, height));
}
} // namespace

DriveRackResult renderDriveRack(bool& open, const std::vector<DrivePicture>& drives) {
    DriveRackResult result;
    ImGui::SetNextWindowSize(ImVec2(370, 720), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Drives", &open)) { ImGui::End(); return result; }
    if (ImGui::Button("Manage disks...")) result.manage = true;
    if (drives.empty()) ImGui::TextDisabled("No drives configured.");
    for (std::size_t index = 0; index < drives.size(); ++index) {
        const auto& drive = drives[index];
        ImGui::PushID(drive.slot);
        ImGui::PushID(drive.bay);
        ImGui::Separator();
        ImGui::TextWrapped("%s", drive.label.c_str());
        const float width = std::min(320.0f, std::max(1.0f, ImGui::GetContentRegionAvail().x));
        if (drive.texture) {
            ImGui::InvisibleButton("##drive-picture", ImVec2(width, width * drive.pictureAspect));
            ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(drive.texture),
                ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        } else mechanismPicture(drive, width);
        const auto pictureMin = ImGui::GetItemRectMin(), pictureMax = ImGui::GetItemRectMax();
        if (ImGui::IsItemClicked() && !drive.loaded)
            result.actions.push_back({DriveRackAction::Type::Insert, index, {}});
        // Clip OS drop targets to the visible picture, including scrolled rows.
        if (ImGui::IsItemVisible() && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
            const auto clipMin = ImGui::GetWindowDrawList()->GetClipRectMin();
            const auto clipMax = ImGui::GetWindowDrawList()->GetClipRectMax();
            result.hits.push_back({drive, std::max(pictureMin.x, clipMin.x),
                std::max(pictureMin.y, clipMin.y), std::min(pictureMax.x, clipMax.x),
                std::min(pictureMax.y, clipMax.y)});
        }
        if (!drive.loaded && ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload("POM2_DISK_IMAGE"))
                if (payload->DataSize > 0 && static_cast<const char*>(payload->Data)[payload->DataSize - 1] == 0)
                    result.actions.push_back({DriveRackAction::Type::Mount, index,
                        static_cast<const char*>(payload->Data)});
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemHovered()) {
            if (!drive.loaded) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("Click to choose a disk, or drop an image here");
                ImGui::GetWindowDrawList()->AddRect(pictureMin, pictureMax,
                    ImGui::GetColorU32(ImGuiCol_ButtonHovered), 4.0f, 2.0f);
            } else ImGui::SetTooltip("Eject this disk before inserting another");
        }
        ImGui::BeginDisabled(!drive.loaded || drive.path.empty());
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, ImGui::GetStyle().FramePadding.y));
        if (ImGui::SmallButton(ICON_FA_EJECT))
            result.actions.push_back({DriveRackAction::Type::Eject, index, {}});
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Eject disk");
        ImGui::SameLine(0.0f, 3.0f);
        if (ImGui::SmallButton(drive.writeProtected ? ICON_FA_LOCK : ICON_FA_LOCK_OPEN))
            result.actions.push_back({DriveRackAction::Type::ToggleWrite, index, {}});
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(drive.writeProtected
            ? "Allow writing to disk" : "Protect disk from writing");
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (drive.loaded) {
            const auto filename = drive.path.find_last_of("/\\");
            ImGui::TextWrapped("%s", drive.path.c_str() + (filename == std::string::npos ? 0 : filename + 1));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", drive.path.c_str());
        } else ImGui::TextDisabled("Empty");
        ImGui::PopID(); ImGui::PopID();
    }
    ImGui::End();
    return result;
}
} // namespace pom2
