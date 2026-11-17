// POM2 — GPL-3.0-or-later
#include "FilePathPicker.h"
#include <cassert>
#include <cstring>

namespace {
pom2::FileDialogResult next = pom2::FileDialogResult::Selected;
std::string picked, initialDir, initialName, filter;
bool available = true, saving = false;
int calls = 0;
}
namespace pom2 {
bool nativeFileDialogAvailable() { return available; }
FileDialogResult nativeFileDialog(bool save, const std::string&, const std::string& extensions,
    const std::string& directory, const std::string& name, std::string& path)
{
    ++calls; saving = save; filter = extensions; initialDir = directory; initialName = name;
    path = picked; // Even a failed backend may have produced output.
    return next;
}
}
int main()
{
    std::string path = "/tmp/old name.po";
    picked = "/tmp/new image.po";
    assert(pom2::pickFilePath(false, "Open", "po,hdv", path) == next);
    assert(path == picked && !saving && filter == "po,hdv");
    assert(initialDir == "/tmp" && initialName == "old name.po");
    for (auto status : {pom2::FileDialogResult::Cancelled, pom2::FileDialogResult::Unavailable}) {
        next = status; picked = "/tmp/discarded.po";
        assert(pom2::pickFilePath(true, "Save", "txt", path) == status);
        assert(path == "/tmp/new image.po" && saving);
    }
    path = std::filesystem::temp_directory_path().u8string();
    next = pom2::FileDialogResult::Selected;
    pom2::pickFilePath(false, "Open", "", path);
    assert(initialName.empty()); // A remembered folder is not a filename.

    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.DisplaySize = ImVec2(1800, 800); io.DeltaTime = 1.0f / 60;
    io.Fonts->AddFontDefault();
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    std::array<char, 64> buffer{};
    std::strcpy(buffer.data(), "manual.po");
    ImVec2 buttonMin, buttonMax;
    bool longInput = false;
    auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1600, 600));
        ImGui::Begin("Paths");
        if (longInput) pom2::filePathInput("Path", path);
        else {
            ImGui::InputText("Path", buffer.data(), buffer.size());
            pom2::browseFilePath(false, "Choose disk", "po,hdv", buffer);
            buttonMin = ImGui::GetItemRectMin(); buttonMax = ImGui::GetItemRectMax();
        }
        ImGui::End(); ImGui::Render();
    };
    auto click = [&] {
        io.AddMousePosEvent((buttonMin.x + buttonMax.x) / 2, (buttonMin.y + buttonMax.y) / 2);
        frame(); io.AddMouseButtonEvent(0, true); frame();
        io.AddMouseButtonEvent(0, false); frame();
    };
    frame(); frame();
    picked = "chosen image.po"; click();
    assert(std::string(buffer.data()) == picked);
    next = pom2::FileDialogResult::Cancelled; picked = "cancelled.po"; click();
    assert(std::string(buffer.data()) == "chosen image.po");
    next = pom2::FileDialogResult::Unavailable; click();
    assert(std::string(buffer.data()) == "chosen image.po");
    next = pom2::FileDialogResult::Selected; picked.assign(100, 'x'); click();
    assert(std::string(buffer.data()) == "chosen image.po"); // Never truncate into a different file.
    // No backend: no button and no request; manual input still renders.
    available = false; const int before = calls; frame(); assert(calls == before);
    path = "/tmp/" + std::string(700, 'x'); const auto longPath = path;
    longInput = true; frame(); frame(); assert(path == longPath);
    ImGui::DestroyContext();
}
