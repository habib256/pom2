// POM2 — GPL-3.0-or-later
#include "SessionFilePicker.h"
#include "SessionFiles.h"
#include "EmulationController.h"
#include "NativeFileDialog.h"
#include "SystemProfile.h"
#include "TestTempPath.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {
pom2::FileDialogResult nextResult = pom2::FileDialogResult::Selected;
std::string nextPath;
bool nextSave = true;
int nativeCalls = 0;
}
namespace pom2 {
bool nativeFileDialogAvailable() { return true; }
FileDialogResult nativeFileDialog(bool save, const std::string&, const std::string& extensions,
    const std::string&, const std::string&, std::string& path)
{
    ++nativeCalls;
    assert(save == nextSave && extensions == "pom2session");
    path = nextPath;
    return nextResult;
}
}

int main()
{
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1000, 700); io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr; io.Fonts->AddFontDefault();
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto ctrl = std::make_unique<EmulationController>();
    ctrl->setMachineId(pom2::snapshotMachineId(pom2::SystemProfile::AppleIIPlus));
    { auto state = ctrl->lockState(); state.cpu().setProgramCounter(0x2468); }
    const std::string stem = pom2test::tempPath("pom2-session-picker");
    const std::string file = stem + ".pom2session";
    const std::string broken = pom2test::tempPath("pom2-session-broken.pom2session");
    std::filesystem::remove(file);
    pom2::SessionFilePicker picker;
    int operations = 0;
    bool visible = false;
    ImRect cancel;
    auto frame = [&] {
        ImGui::NewFrame();
        const auto request = picker.render();
        visible = ImGui::IsPopupOpen("Session file");
        if (visible) {
            const auto* window = ImGui::FindWindowByName("Session file");
            if (window) {
                // EndPopup restores LastItemData from the parent. The final
                // line's layout still locates the Cancel button in the modal.
                const auto end = window->DC.CursorPosPrevLine;
                cancel = ImRect(ImVec2(end.x - 120, end.y),
                                ImVec2(end.x, end.y + ImGui::GetFrameHeight()));
            }
        }
        if (request) {
            ++operations;
            std::string error;
            const bool ok = request->save ? pom2::saveSessionFile(*ctrl, request->path, error)
                                          : pom2::loadSessionFile(*ctrl, request->path, error);
            picker.complete(ok, error);
        }
        ImGui::Render();
    };
    auto click = [&](bool accept) {
        // Both action buttons are 120px; the last item is Cancel.
        const ImVec2 pos(accept ? cancel.Min.x - ImGui::GetStyle().ItemSpacing.x - 60
                               : cancel.GetCenter().x, cancel.GetCenter().y);
        io.AddMousePosEvent(pos.x, pos.y); frame();
        io.AddMouseButtonEvent(0, true); frame();
        io.AddMouseButtonEvent(0, false); frame();
    };
    // Native cancellation does not open a fallback dialog or execute anything.
    nextResult = pom2::FileDialogResult::Cancelled;
    picker.open(true); frame();
    assert(!visible && operations == 0 && nativeCalls == 1);
    frame(); assert(nativeCalls == 1);
    // Save through the real controller adapter; add a missing suffix.
    nextResult = pom2::FileDialogResult::Selected; nextPath = stem;
    picker.open(true); frame(); frame();
    assert(!visible && operations == 1 && std::filesystem::exists(file));
    // An existing destination requires an explicit Replace. Cancelling leaves
    // the earlier snapshot available, even though the live PC has changed.
    { auto state = ctrl->lockState(); state.cpu().setProgramCounter(0x5678); }
    nextPath = file; picker.open(true); frame(); frame();
    assert(visible && operations == 1);
    click(false); assert(!visible && operations == 1);
    nextSave = false; picker.open(false); frame(); frame();
    assert(!visible && operations == 2);
    { auto state = ctrl->lockState(); assert(state.cpu().getProgramCounter() == 0x2468); }
    // Explicit overwrite really updates the saved state.
    { auto state = ctrl->lockState(); state.cpu().setProgramCounter(0x369C); }
    nextSave = true; picker.open(true); frame(); frame(); click(true); frame();
    assert(!visible && operations == 3);
    { auto state = ctrl->lockState(); state.cpu().setProgramCounter(0x1111); }
    nextSave = false; picker.open(false); frame(); frame();
    assert(!visible && operations == 4);
    { auto state = ctrl->lockState(); assert(state.cpu().getProgramCounter() == 0x369C); }
    // Corrupt input keeps the dialog open for correction and preserves CPU.
    { std::ofstream f(broken); f << "broken session"; }
    nextPath = broken; picker.open(false); frame(); frame();
    assert(visible && operations == 5);
    { auto state = ctrl->lockState(); assert(state.cpu().getProgramCounter() == 0x369C); }
    click(false); assert(!visible && operations == 5);
    // Backend failure exposes the manual path fallback, with the remembered
    // valid path, and still requires an explicit action.
    nextSave = true; nextPath = file; nextResult = pom2::FileDialogResult::Selected;
    picker.open(true); frame(); click(false);
    nextResult = pom2::FileDialogResult::Unavailable;
    picker.open(true); frame(); frame();
    assert(visible && operations == 5);
    click(true); assert(operations == 5); // first Save only asks to replace
    frame(); click(true); frame(); assert(!visible && operations == 6);
    std::filesystem::remove(file); std::filesystem::remove(broken);
    ImGui::DestroyContext();
}
