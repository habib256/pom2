// POM2 — GPL-3.0-or-later
#include "DiskController_ImGui.h"
#include "imgui.h"
#include <algorithm>
#include <array>
#include <cfloat>

namespace pom2 {
void DiskController_ImGui::renderFlux(const DriveSnapshot& snap)
{
    if (!ImGui::CollapsingHeader("Magnetic transitions and head")) return;
    ImGui::RadioButton("Drive 1##flux", &fluxDrive_, 0); ImGui::SameLine();
    ImGui::RadioButton("Drive 2##flux", &fluxDrive_, 1);
    const auto& flux = snap.flux[fluxDrive_];
    if (!flux.loaded || flux.period <= 0) { ImGui::TextDisabled("No track loaded."); return; }
    ImGui::Text("Head: track %.2f | %zu transitions per revolution", flux.quarterTrack / 4.0f, flux.transitions.size());
    ImGui::ProgressBar(float(flux.quarterTrack) / 159.0f, ImVec2(-1, 12), "Head: track 0 to 39.75");
    std::array<float, 512> density{};
    for (int event : flux.transitions) {
        if (event >= 0 && event < flux.period)
            density[size_t(int64_t(event) * density.size() / flux.period)] += 1;
    }
    const auto origin = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImGui::PlotHistogram("##flux-density", density.data(), int(density.size()), 0,
        "One revolution; yellow = angular cursor", 0, FLT_MAX, ImVec2(width, 64));
    const float cursorX = origin.x + width * float(flux.position) / flux.period;
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(cursorX, origin.y), ImVec2(cursorX, origin.y + 64), IM_COL32(255, 220, 80, 255));
    ImGui::Checkbox("Follow angular cursor", &fluxFollow_);
    ImGui::BeginDisabled(fluxFollow_);
    ImGui::SliderFloat("Revolution offset", &fluxOffset_, 0, 1, "%.4f");
    ImGui::EndDisabled();
    constexpr int window = 512;
    const int start = fluxFollow_ ? flux.position : int(fluxOffset_ * (flux.period - 1));
    const int span = std::min(window, flux.period);
    const auto p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##flux-transitions", ImVec2(width, 50));
    draw->AddRectFilled(p, ImVec2(p.x + width, p.y + 50), IM_COL32(20, 24, 30, 255));
    // lower_bound keeps zoom rendering bounded; a window can cross index 0.
    for (int pass = 0; pass < 2; ++pass) {
        const int from = pass ? 0 : start;
        const int to = pass ? start + span - flux.period : std::min(start + span, flux.period);
        if (to <= from) continue;
        auto it = std::lower_bound(flux.transitions.begin(), flux.transitions.end(), from);
        for (; it != flux.transitions.end() && *it < to; ++it) {
            const int offset = *it - start + (pass ? flux.period : 0);
            const float x = p.x + width * float(offset) / span;
            draw->AddLine(ImVec2(x, p.y + 8), ImVec2(x, p.y + 42), IM_COL32(100, 220, 190, 255));
        }
    }
    ImGui::Text("Zoom: %d LSS cycles from %d (wraps at %d)", span, start, flux.period);
    ImGui::TextWrapped("Transitions used by the controller; sector images show reconstructed flux. One LSS cycle = 500 ns. This view does not measure analog signal strength.");
    ImGui::Separator();
}
}
