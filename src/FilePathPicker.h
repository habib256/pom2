// POM2 — GPL-3.0-or-later
#ifndef POM2_FILE_PATH_PICKER_H
#define POM2_FILE_PATH_PICKER_H
#include "NativeFileDialog.h"
#include "imgui.h"
#include <array>
#include <cstdio>
#include <system_error>
#include <vector>
#include <filesystem>
#include <string>

namespace pom2 {
// Retain long UTF-8 paths from the OS chooser when editing a manual field.
inline void filePathInput(const char* id, std::string& path)
{
    std::vector<char> buffer(path.size() + 1024, 0);
    std::snprintf(buffer.data(), buffer.size(), "%s", path.c_str());
    if (ImGui::InputText(id, buffer.data(), buffer.size())) path = buffer.data();
}
// UI thread, with no machine lock held. Only a successful selection changes
// the caller's path; Cancelled and Unavailable preserve manual input.
inline FileDialogResult pickFilePath(bool save, const char* title,
    const char* extensions, std::string& path)
{
    const auto previous = std::filesystem::u8path(path);
    std::error_code ec;
    const bool directory = std::filesystem::is_directory(previous, ec);
    std::string picked;
    const auto result = nativeFileDialog(save, title, extensions,
        (directory ? previous : previous.parent_path()).u8string(),
        directory ? "" : previous.filename().u8string(), picked);
    if (result == FileDialogResult::Selected) path = picked;
    return result;
}
inline void browseFilePath(bool save, const char* title,
    const char* extensions, std::string& path)
{
    if (!nativeFileDialogAvailable()) return;
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) pickFilePath(save, title, extensions, path);
}
inline void browseFilePath(bool save, const char* title,
    const char* extensions, char* buffer, size_t capacity)
{
    if (!nativeFileDialogAvailable()) return;
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        std::string path(buffer);
        if (pickFilePath(save, title, extensions, path) == FileDialogResult::Selected) {
            if (path.size() < capacity) std::snprintf(buffer, capacity, "%s", path.c_str());
            else ImGui::OpenPopup("Path too long");
        }
    }
    if (ImGui::BeginPopup("Path too long")) {
        ImGui::TextUnformatted("The selected path is too long. Choose a shorter path.");
        ImGui::EndPopup();
    }
}
template <size_t N> inline void browseFilePath(bool save, const char* title,
    const char* extensions, char (&buffer)[N])
{ browseFilePath(save, title, extensions, buffer, N); }
template <size_t N> inline void browseFilePath(bool save, const char* title,
    const char* extensions, std::array<char, N>& buffer)
{ browseFilePath(save, title, extensions, buffer.data(), N); }
}
#endif
