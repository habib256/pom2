// POM2 — GPL-3.0-or-later
#include "DriveRackFilePicker.h"
#include "ResourcePaths.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <utility>

namespace pom2 {
namespace fs = std::filesystem;
void DriveRackFilePicker::browse(const std::string& directory) {
    std::error_code ec;
    auto dir = fs::absolute(directory, ec);
    if (ec || !fs::is_directory(dir, ec)) { error_ = "This folder could not be opened."; return; }
    std::vector<Entry> entries;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) { error_ = "This folder could not be read."; return; }
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const auto& item = *it;
        const bool folder = item.is_directory(ec);
        if (ec) { ec.clear(); continue; }
        const auto name = item.path().filename().string();
        if (name.empty() || (!showHidden_ && name[0] == '.')) continue;
        if (!folder) {
            if (!item.is_regular_file(ec) || ec) { ec.clear(); continue; }
            auto ext = item.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
            if (ext != ".dsk" && ext != ".do" && ext != ".po" && ext != ".nib" &&
                ext != ".woz" && ext != ".2mg" && ext != ".hdv" && ext != ".d13" && ext != ".image") continue;
        }
        entries.push_back({name, item.path().string(), folder});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.directory != b.directory) return a.directory;
        return a.name < b.name;
    });
    directory_ = dir.lexically_normal().string(); entries_ = std::move(entries); error_.clear();
    std::snprintf(folder_.data(), folder_.size(), "%s", directory_.c_str());
    path_[0] = 0;
}
void DriveRackFilePicker::open(const DrivePicture& target) {
    target_ = target; opening_ = true; closing_ = false; path_[0] = 0; error_.clear();
    if (directory_.empty()) {
        auto root = findFirstResource({"bundled_disks", "packaging/disks"});
        browse(root.empty() ? "." : root);
    } else browse(directory_);
}
void DriveRackFilePicker::complete(bool ok, const std::string& error) {
    closing_ = ok; error_ = error;
}
std::optional<DriveRackSelection> DriveRackFilePicker::render() {
    if (opening_) { ImGui::OpenPopup("Insert disk into drive"); opening_ = false; }
    ImGui::SetNextWindowSize(ImVec2(650, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal("Insert disk into drive", nullptr, ImGuiWindowFlags_NoSavedSettings)) return {};
    if (closing_) { ImGui::CloseCurrentPopup(); closing_ = false; ImGui::EndPopup(); return {}; }
    ImGui::TextWrapped("%s", target_.label.c_str());
    ImGui::TextDisabled("%s", target_.kind == DrivePictureKind::DiskII ? "5.25-inch disk image" :
                              target_.kind == DrivePictureKind::Floppy35 ? "800K 3.5-inch disk image" : "ProDOS hard-disk image");
    const auto current = fs::path(directory_);
    ImGui::BeginDisabled(current == current.root_path());
    if (ImGui::Button("Up")) browse(current.parent_path().string());
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Root")) browse(current.root_path().string());
    ImGui::SameLine();
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    ImGui::BeginDisabled(!home || !*home);
    if (ImGui::Button("Home") && home && *home) browse(home);
    ImGui::EndDisabled();
    ImGui::SameLine();
#ifdef _WIN32
    if (ImGui::Button("Drives")) ImGui::OpenPopup("Mounted drives");
    if (ImGui::BeginPopup("Mounted drives")) {
        for (char letter = 'A'; letter <= 'Z'; ++letter) {
            const std::string root = std::string(1, letter) + ":\\";
            std::error_code ec;
            if (fs::is_directory(root, ec) && ImGui::Selectable(root.c_str())) browse(root);
        }
        ImGui::EndPopup();
    }
#elif defined(__APPLE__)
    if (ImGui::Button("Volumes")) browse("/Volumes");
#else
    if (ImGui::Button("Volumes")) browse("/media");
#endif
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) browse(directory_);
    // A dedicated address field makes any accessible folder reachable
    // without climbing out of the bundled disks one parent at a time.
    ImGui::SetNextItemWidth(-ImGui::GetFrameHeightWithSpacing() * 4);
    bool go = ImGui::InputText("##Folder", folder_.data(), folder_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(); go |= ImGui::Button("Go");
    if (go && folder_[0]) {
        auto destination = fs::path(folder_.data());
        if (destination.is_relative()) destination = fs::path(directory_) / destination;
        browse(destination.string());
    }
    if (ImGui::Checkbox("Show hidden folders and files", &showHidden_)) browse(directory_);
    std::string nextDirectory;
    bool insert = false;
    ImGui::BeginChild("files", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 4), ImGuiChildFlags_Borders);
    for (const auto& entry : entries_) {
        const auto label = entry.directory ? entry.name + "/" : entry.name;
        if (ImGui::Selectable(label.c_str(), entry.path == path_.data(), ImGuiSelectableFlags_AllowDoubleClick)) {
            if (entry.directory) nextDirectory = entry.path;
            else {
                std::snprintf(path_.data(), path_.size(), "%s", entry.path.c_str());
                error_.clear();
                insert = ImGui::IsMouseDoubleClicked(0);
            }
        }
    }
    ImGui::EndChild();
    if (!nextDirectory.empty()) browse(nextDirectory);
    insert |= ImGui::InputText("File or folder", path_.data(), path_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    if (!error_.empty()) ImGui::TextWrapped("%s", error_.c_str());
    ImGui::BeginDisabled(path_[0] == 0);
    insert |= ImGui::Button("Insert");
    ImGui::EndDisabled(); ImGui::SameLine();
    if (ImGui::Button("Cancel")) { ImGui::CloseCurrentPopup(); insert = false; }
    std::optional<DriveRackSelection> result;
    if (insert && path_[0]) {
        std::error_code ec;
        auto path = fs::path(path_.data());
        if (path.is_relative()) path = fs::path(directory_) / path;
        if (fs::is_directory(path, ec)) { browse(path.string()); path_[0] = 0; }
        else result = DriveRackSelection{target_, path.string()};
    }
    ImGui::EndPopup();
    return result;
}
}
