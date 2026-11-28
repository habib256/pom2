// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "PinchZoom_ImGui.h"

#include "imgui_internal.h"

#include <cfloat>
#include <cstdint>
#include <vector>

namespace pom2 {

void pinchZoomNewFrame()
{
    ImGuiIO& io = ImGui::GetIO();
    pinch::Router& r = pinch::router();

    // A second finger landed while the first one holds the left button
    // (PinchZoom.h, cancelPress). The widget it pressed lets go now — a
    // button clicks on release only while it is still the active item — and
    // the release itself is queued AWAY from every window, after the pointer
    // is moved off: ImGui processes the two in that order, so a view that
    // watches IsMouseReleased sees it nowhere. GLFW's own release, when the
    // fingers lift, then finds the button already up and changes nothing.
    if (r.takePressCancel() && io.MouseDown[ImGuiMouseButton_Left]) {
        ImGui::ClearActiveID();
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    }

    // Ctrl+wheel is a pinch: some Linux touchpad drivers send a pinch that
    // way, and a mouse user gets the same zoom with Ctrl held (Cmd on macOS,
    // where ImGui swaps the two). ImGui already refused to scroll on it (it
    // never scrolls with its Ctrl down); clearing the wheel here keeps the
    // views that read io.MouseWheel themselves from ALSO acting on it.
    // (Windows' Ctrl wheels never get here: PinchZoomHost.cpp takes them.)
    if (pinch::wheelIsZoom(io.KeyCtrl, io.MouseWheel)) {
        r.push(pinch::wheelToLog(io.MouseWheel));
        io.MouseWheel = 0.0f;
        io.MouseWheelH = 0.0f;
    }

    const ImVec2 mouse = ImGui::IsMousePosValid(&io.MousePos) ? io.MousePos : ImVec2(-1.0f, -1.0f);
    r.beginFrame(pinch::Point{mouse.x, mouse.y});
    if (!r.pending()) return;

    // The window under the focal point, as ImGui sees it (front to back,
    // children included), then its parents. A modal popup owns every
    // gesture that does not land on it.
    const pinch::Point f = r.focal();
    ImGuiWindow* hovered = nullptr;
    ImGui::FindHoveredWindowEx(ImVec2(f.x, f.y), false, &hovered, nullptr);
    if (ImGuiWindow* modal = ImGui::GetTopMostAndVisiblePopupModal())
        if (hovered && !ImGui::IsWindowChildOf(hovered, modal, true, true)) hovered = nullptr;
    std::vector<std::uint64_t> chain;
    for (ImGuiWindow* w = hovered; w; w = w->ParentWindow) chain.push_back(w->ID);
    r.setHitChain(std::move(chain));
}

bool pinchZoomTake(const ImVec2& min, const ImVec2& max, pinch::Gesture& g)
{
    pinch::Router& r = pinch::router();
    if (!r.pending()) return false;
    const ImGuiWindow* w = ImGui::GetCurrentWindowRead();
    if (!w) return false;
    return r.take(w->ID, pinch::Rect{min.x, min.y, max.x, max.y}, g);
}

} // namespace pom2
