// POM2 — GPL-3.0-or-later
#include "SessionFilePicker.h"
#include "NativeFileDialog.h"
#include "BrowserFiles.h"
#include "imgui.h"

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace pom2 {
void SessionFilePicker::open(bool save)
{
    save_ = save; opening_ = true; closing_ = false; replacing_ = false;
    error_.clear();
#ifdef __EMSCRIPTEN__
    if (!save) {
        opening_ = false;
        chooseBrowserFile(BrowserFileKind::Session);
        return;
    }
    if (!path_[0]) setPath("/tmp/session.pom2session");
#else
    if (!path_[0]) setPath("session.pom2session");
#endif
}
bool SessionFilePicker::setPath(const std::string& path)
{
    if (path.size() >= path_.size()) {
        path_[0] = 0;
        error_ = "The selected path is too long. Choose a shorter path.";
        return false;
    }
    std::snprintf(path_.data(), path_.size(), "%s", path.c_str());
    return true;
}
void SessionFilePicker::complete(bool ok, const std::string& error)
{
    closing_ = ok; error_ = error; replacing_ = false;
}
std::optional<SessionFileSelection> SessionFilePicker::submit(bool replace)
{
    if (!path_[0]) return {};
    auto path = std::filesystem::u8path(path_.data());
    // Native save panels do not all append the selected extension.
    if (save_ && path.extension().empty()) {
        path += ".pom2session";
        if (!setPath(path.u8string())) return {};
    }
    std::error_code ec;
    if (save_ && !replace && std::filesystem::exists(path, ec)) {
        replacing_ = true;
        return {};
    }
    return SessionFileSelection{save_, path.u8string()};
}
std::optional<SessionFileSelection> SessionFilePicker::render()
{
#ifdef __EMSCRIPTEN__
    if (opening_) {
        opening_ = false;
        return SessionFileSelection{true, "/tmp/session.pom2session"};
    }
    if (error_.empty()) return {};
    ImGui::OpenPopup("Session export");
    std::optional<SessionFileSelection> selected;
    if (ImGui::BeginPopupModal("Session export", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", error_.c_str());
        if (ImGui::Button("Retry")) {
            selected = SessionFileSelection{true, "/tmp/session.pom2session"};
            error_.clear(); ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { error_.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    return selected;
#else
    bool submitPicked = false;
    if (opening_) {
        opening_ = false;
        if (nativeFileDialogAvailable()) {
            const auto previous = std::filesystem::u8path(path_.data());
            std::string picked;
            const auto result = nativeFileDialog(save_, save_ ? "Save session" : "Open session",
                "pom2session", previous.parent_path().u8string(),
                save_ ? previous.filename().u8string() : "", picked);
            if (result == FileDialogResult::Cancelled) return {};
            if (result == FileDialogResult::Selected) submitPicked = setPath(picked);
        }
        ImGui::OpenPopup("Session file");
    }
    ImGui::SetNextWindowSize(ImVec2(590, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Session file", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return {};
    if (closing_) {
        closing_ = false; ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return {};
    }
    ImGui::TextUnformatted(save_ ? "Save session" : "Open session");
    ImGui::TextWrapped("Supported II/II+/IIe and //c-class machines, cards, floppy/block media "
        "and program tapes. Built-in serial ports must be idle. "
        "Opening restores the recorded profile, embedded firmware and supported cards automatically.");
    ImGui::TextWrapped("Restored writable disks keep changes in RAM; save a session to retain them.");
    if (!save_) ImGui::TextWrapped("Loading replaces the current CPU, memory and supported device state.");
    std::optional<SessionFileSelection> selection;
    if (submitPicked) selection = submit();
    ImGui::BeginDisabled(replacing_);
    ImGui::SetNextItemWidth(-1);
    const bool enter = ImGui::InputText("##SessionPath", path_.data(), path_.size(),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::EndDisabled();
    if (!error_.empty()) ImGui::TextWrapped("%s", error_.c_str());
    if (replacing_) {
        ImGui::TextWrapped("A file already exists at this path. Replace it with this session?");
        if (ImGui::Button("Replace", ImVec2(120, 0))) selection = submit(true);
    } else {
        ImGui::BeginDisabled(!path_[0]);
        if (ImGui::Button(save_ ? "Save" : "Open", ImVec2(120, 0)) || enter)
            selection = submit();
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        selection.reset(); replacing_ = false; ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return selection;
#endif
}
}
