// POM2 — GPL-3.0-or-later
#include "Keyboard_ImGui.h"
#include "KeyboardSkin.h"
#include "SystemProfile.h"
#include "imgui.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cmath>

int main() {
    using pom2::SystemProfile;
    const auto& compact = pom2::keyboardSkin(SystemProfile::AppleIIc);
    assert(&compact == &pom2::keyboardSkin(SystemProfile::AppleIIcPAL));
    assert(&compact == &pom2::keyboardSkin(SystemProfile::AppleIIcPlus));
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1100, 650);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    io.Fonts->SetTexID(2);
    pom2::Keyboard_ImGui keyboard;
    bool open = true;
    auto profile = SystemProfile::AppleIIc;
    auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1000, 520));
        const bool isCompact = pom2::keyboardSkin(profile).compact;
        auto event = keyboard.render(&open, 1, isCompact ? 1774 : 2578,
                                     isCompact ? 887 : 908, "", pom2::keyboardSkin(profile));
        ImGui::Render();
        return event;
    };
    frame(); frame();
    // Measure the textured image in the actual draw output. End() restores
    // the parent window's last item, so GetItemRectMin() cannot identify it.
    ImVec2 origin(1100, 650), end(0, 0);
    const auto* draw = ImGui::GetDrawData();
    for (const auto* list : draw->CmdLists) {
        for (const auto& command : list->CmdBuffer) {
            if (command.GetTexID() != 1) continue;
            for (unsigned i = 0; i < command.ElemCount; ++i) {
                const auto& point = list->VtxBuffer[command.VtxOffset +
                    list->IdxBuffer[command.IdxOffset + i]].pos;
                origin.x = std::min(origin.x, point.x);
                origin.y = std::min(origin.y, point.y);
                end.x = std::max(end.x, point.x);
                end.y = std::max(end.y, point.y);
            }
        }
    }
    assert(end.x > origin.x && end.y > origin.y);
    assert(std::abs((end.x - origin.x) / (end.y - origin.y) - 2578.0f / 908.0f) < 0.001f);
    auto click = [&](float x, float y) {
        io.AddMousePosEvent(origin.x + x * (end.x - origin.x),
                            origin.y + y * (end.y - origin.y));
        frame();
        io.AddMouseButtonEvent(0, true);
        auto event = frame();
        io.AddMouseButtonEvent(0, false);
        frame();
        return event;
    };
    // The current //c photo has distinct Apple and backslash positions.
    click(0.690f, 0.867f);
    assert(keyboard.latches().solidApple && !keyboard.latches().openApple);
    click(0.250f, 0.867f);
    assert(keyboard.latches().openApple);
    auto event = click(0.960f, 0.330f);
    assert(event.key && event.key->base == '\\');
    event = click(0.930f, 0.510f);
    assert(event.key && event.key->action == pom2::KeyAction::Return);
    event = click(0.180f, 0.867f);
    assert(!event.key); // empty recess beside Open-Apple
    // A live machine change selects the other geometry, with the same
    // displayed proportions. This position is Caps Lock on the //e and
    // backquote on the current //c image.
    keyboard.releaseAll();
    profile = SystemProfile::AppleIIe;
    frame();
    click(0.114f, 0.867f);
    assert(!keyboard.latches().caps && !keyboard.latches().solidApple);
    ImGui::DestroyContext();
}
