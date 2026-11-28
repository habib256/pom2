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

// Memory Heatmap, what the map always tells: the status line under it (the
// byte under the pointer or the selection — breadcrumb, bank, label, value,
// where it shows on the screen, its last accesses), the minimap while
// zoomed in, and French Touch's monitoring bars (CycleMonitor.h): under the
// map as FT draws them, in figures in the side panel.

#include "MemoryHeatmap_ImGui.h"

#include "EmulationController.h"
#include "IconsFontAwesome6.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace pom2 {

using heatview::Layout;

namespace {

ImVec4 colourV(heatview::Rgb c, float alpha = 1.0f)
{
    return ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, alpha);
}

std::string grouped(uint64_t v)
{
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), " ");
    return s;
}

}  // namespace

// ── The status line ────────────────────────────────────────────────────────

std::string MemoryHeatmap_ImGui::shortAge(uint32_t stamp) const
{
    if (stamp == 0) return "never";
    if (stamp > machine_.now) return "-";
    const double cycles = static_cast<double>(machine_.now - stamp) * (1u << HeatData::kStampShift);
    const double frames = cycles / machine_.frameCycles;
    char t[32];
    if (frames < 1.0) std::snprintf(t, sizeof t, "now");
    else if (frames < 1000.0) std::snprintf(t, sizeof t, "%.0f fr ago", frames);
    else std::snprintf(t, sizeof t, "%.1f s ago", cycles / machine_.clockHz);
    return t;
}

void MemoryHeatmap_ImGui::drawStatusLine(EmulationController& ctrl, float x, float y, float w,
                                         const LabelFn& labelOf)
{
    ImGui::SetCursorScreenPos(ImVec2(x + 2.0f, y));
    ImGui::PushClipRect(ImVec2(x - 2.0f, y), ImVec2(x + w, y + ImGui::GetTextLineHeightWithSpacing() * 2.0f + 4.0f), true);
    const bool have = hovered_ || selected_;
    const int plane = hovered_ ? hoverPlane_ : selPlane_;
    const uint16_t phys = hovered_ ? hoverPhys_ : selPhys_;
    const ImVec4 dim = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
    if (!have) {
        ImGui::TextColored(dim, "%.1f x %.1f px a byte%s", static_cast<double>(xf_.scale),
                           static_cast<double>(xf_.scaleY), frozen_ ? "   \xc2\xb7   FROZEN" : "");
        ImGui::SetCursorScreenPos(ImVec2(x + 2.0f, y + ImGui::GetTextLineHeightWithSpacing()));
        if (!status_.empty() && ImGui::GetTime() < statusUntil_)
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "%s", status_.c_str());
        else
            ImGui::TextColored(dim, "wheel zoom \xc2\xb7 drag pan \xc2\xb7 click select \xc2\xb7 shift-drag a range "
                                    "\xc2\xb7 right-click break / watch \xc2\xb7 Ctrl+G go to \xc2\xb7 Home fit");
        ImGui::PopClipRect();
        return;
    }
    // Line 1 — the breadcrumb: bank › region › address, then the bank as
    // the debugger names it, the label, the value.
    const DebugAddress at = HeatData::cpuAddress(plane, phys);
    const heatview::Region* region = heatview::regionAt(plane, phys);
    ImGui::TextUnformatted(heatview::planeTitle(plane, machine_.auxBanks).c_str());
    if (region) {
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextColored(dim, ICON_FA_ANGLE_RIGHT);
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextUnformatted(*region->tag ? region->tag : region->name);
    }
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextColored(dim, ICON_FA_ANGLE_RIGHT);
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.36f, 1.0f), "$%04X", at.addr);
    if (at.bank.kind != DebugBank::Any) {
        ImGui::SameLine(0.0f, 14.0f);
        ImGui::TextColored(dim, "%s", at.text().c_str());
    }
    if (labelOf) {
        const std::string label = labelOf(at.addr, at.bank);
        if (!label.empty()) {
            ImGui::SameLine(0.0f, 14.0f);
            ImGui::TextColored(ImVec4(0.55f, 0.82f, 1.0f, 1.0f), "%s", label.c_str());
        }
    }
    const bool io = plane == HeatData::kRom && phys < 0xC100;
    if (!io) {
        const uint8_t v = valueOf(plane, phys);
        const char ch = static_cast<char>(v & 0x7F);
        ImGui::SameLine(0.0f, 14.0f);
        ImGui::Text("$%02X  %u  '%c'", v, v, ch >= 0x20 && ch < 0x7F ? ch : '.');
    }
    if (selected_ && !hovered_) {
        ImGui::SameLine(0.0f, 14.0f);
        ImGui::TextColored(dim, "(selected)");
    }
    if (frozen_) {
        ImGui::SameLine(0.0f, 14.0f);
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), ICON_FA_SNOWFLAKE " frozen");
    }

    // Line 2 — a message if one is fresh; else where the byte shows on the
    // screen and its last access of each kind.
    ImGui::SetCursorScreenPos(ImVec2(x + 2.0f, y + ImGui::GetTextLineHeightWithSpacing()));
    if (!status_.empty() && ImGui::GetTime() < statusUntil_) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "%s", status_.c_str());
        ImGui::PopClipRect();
        return;
    }
    statusInfo_ = infoOf(ctrl, plane, phys);
    bool first = true;
    for (int k : {HeatData::Write, HeatData::Read, HeatData::Exec, HeatData::Dma}) {
        const auto ki = static_cast<std::size_t>(k);
        if (!first) ImGui::SameLine(0.0f, 12.0f);
        first = false;
        static const char* const tag[] = {"R", "W", "X", "D"};
        const bool seen = statusInfo_.stamp[ki] != 0;
        ImGui::TextColored(seen ? colourV(heatview::kindColour(k)) : dim, "%s %s", tag[k],
                           shortAge(statusInfo_.stamp[ki]).c_str());
    }
    const std::string spot = heatview::describeSpot(heatview::screenSpot(plane, phys));
    if (!spot.empty()) {
        ImGui::SameLine(0.0f, 18.0f);
        ImGui::TextColored(dim, "%s", spot.c_str());
    }
    ImGui::PopClipRect();
}

// ── The minimap ────────────────────────────────────────────────────────────

bool MemoryHeatmap_ImGui::drawMinimap(ImDrawList* dl, float ox, float oy, float w, float h)
{
    (void)ox; (void)oy;
    const ImVec2 m = ImGui::GetIO().MousePos;
    auto over = [&](float x0, float y0, float x1, float y1) { return m.x >= x0 && m.x < x1 && m.y >= y0 && m.y < y1; };
    if (mini_.pill) {   // collapsed: a pill that brings it back
        const bool hot = over(mini_.bx0, mini_.by0, mini_.bx1, mini_.by1);
        dl->AddRectFilled(ImVec2(mini_.bx0, mini_.by0), ImVec2(mini_.bx1, mini_.by1),
                          IM_COL32(28, 31, 40, hot ? 235 : 190), 6.0f);
        dl->AddRect(ImVec2(mini_.bx0, mini_.by0), ImVec2(mini_.bx1, mini_.by1), IM_COL32(255, 255, 255, hot ? 150 : 80), 6.0f);
        dl->AddText(ImVec2(mini_.bx0 + 7.0f, mini_.by0 + 4.0f), IM_COL32(230, 233, 242, 255), ICON_FA_MAP " minimap");
        return false;
    }
    if (!mini_.shown) return false;
    // Translucent: the map under it shows through, dimmed enough that its
    // values do not fight the overview (the inset also moves out of the
    // selection's way, see paint()).
    const ImVec2 a(mini_.x0 - 5.0f, mini_.y0 - 5.0f), b(mini_.x1 + 5.0f, mini_.y1 + 5.0f);
    dl->AddRectFilled(a, b, IM_COL32(30, 33, 42, 212), 6.0f);
    const ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    if (pio.DrawCallback_SetSamplerNearest) dl->AddCallback(pio.DrawCallback_SetSamplerNearest, nullptr);
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const Column& c = columns_[i];
        if (!c.mini.id) continue;
        const int r0 = heatview::firstRow(c.plane);
        const float top = std::min(heatview::rowY(layout_, r0), heatview::rowY(layout_, 255));
        const float x0 = mini_.x0 + layout_.columns[i].x * mini_.s;
        const float y0 = mini_.y0 + top * mini_.sY;
        const ImVec2 uvTop(0.0f, layout_.bottomUp ? 1.0f : 0.0f), uvBottom(1.0f, layout_.bottomUp ? 0.0f : 1.0f);
        dl->AddImage(static_cast<ImTextureID>(c.mini.id), ImVec2(x0, y0),
                     ImVec2(x0 + Layout::kPlane * mini_.s, y0 + static_cast<float>(256 - r0) * mini_.sY),
                     uvTop, uvBottom, IM_COL32(255, 255, 255, 238));
    }
    if (pio.DrawCallback_SetSamplerLinear) dl->AddCallback(pio.DrawCallback_SetSamplerLinear, nullptr);
    // Each column's outline and initial, so the banks read apart at this size.
    const float tf = ImGui::GetFontSize() * 0.7f;
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const int r0 = heatview::firstRow(columns_[i].plane);
        const float top = std::min(heatview::rowY(layout_, r0), heatview::rowY(layout_, 255));
        const float x0 = mini_.x0 + layout_.columns[i].x * mini_.s, y0 = mini_.y0 + top * mini_.sY;
        dl->AddRect(ImVec2(x0 - 0.5f, y0 - 0.5f),
                    ImVec2(x0 + Layout::kPlane * mini_.s + 0.5f, y0 + static_cast<float>(256 - r0) * mini_.sY + 0.5f),
                    IM_COL32(255, 255, 255, 70));
        const int p = columns_[i].plane;
        const char* tag = p == HeatData::kRom ? "ROM" : p == HeatData::kMain ? "MAIN" : "AUX";
        dl->AddText(nullptr, tf, ImVec2(x0 + 2.0f, y0 + 1.0f), IM_COL32(235, 238, 246, 170), tag);
    }
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 110), 6.0f, 0, 1.2f);
    // What the view shows: a bright rectangle, outlined dark for any ground.
    const float vx0 = mini_.x0 + std::max(0.0f, xf_.toCanvasX(0.0f)) * mini_.s;
    const float vx1 = std::max(vx0 + 3.0f, mini_.x0 + std::min(layout_.width, xf_.toCanvasX(w)) * mini_.s);
    const float vy0 = mini_.y0 + std::max(0.0f, xf_.toCanvasY(0.0f)) * mini_.sY;
    const float vy1 = std::max(vy0 + 3.0f, mini_.y0 + std::min(Layout::kPlane, xf_.toCanvasY(h)) * mini_.sY);
    dl->AddRectFilled(ImVec2(vx0, vy0), ImVec2(vx1, vy1), IM_COL32(255, 214, 90, 46));
    dl->AddRect(ImVec2(vx0 - 2, vy0 - 2), ImVec2(vx1 + 2, vy1 + 2), IM_COL32(0, 0, 0, 200), 2.0f, 0, 1.5f);
    dl->AddRect(ImVec2(vx0 - 1, vy0 - 1), ImVec2(vx1 + 1, vy1 + 1), IM_COL32(255, 222, 100, 255), 2.0f, 0, 2.0f);
    // The collapse button, top right.
    const bool hot = over(mini_.bx0, mini_.by0, mini_.bx1, mini_.by1);
    dl->AddRectFilled(ImVec2(mini_.bx0, mini_.by0), ImVec2(mini_.bx1, mini_.by1),
                      IM_COL32(40, 44, 56, hot ? 240 : 200), 4.0f);
    const ImVec2 ms = ImGui::CalcTextSize(ICON_FA_MINUS);
    dl->AddText(ImVec2((mini_.bx0 + mini_.bx1 - ms.x) * 0.5f, (mini_.by0 + mini_.by1 - ms.y) * 0.5f),
                IM_COL32(235, 238, 246, hot ? 255 : 190), ICON_FA_MINUS);
    return true;
}

// ── French Touch's monitoring bars ─────────────────────────────────────────

float MemoryHeatmap_ImGui::monitorStripHeight() const
{
    if (machine_.monitor.marks() == 0) return 0.0f;
    return ImGui::GetTextLineHeightWithSpacing() * 2.0f + 4.0f;
}

void MemoryHeatmap_ImGui::drawMonitorStrip(ImDrawList* dl, float x, float y, float w)
{
    // [D] AppleWin FT @ 52ddd021 Debugger_Heatmap.cpp, "CPU monitoring":
    // two 256-px bars under the map, white-bordered — the last section
    // (green / orange / red over 1 / 4 / 8 frames, a tick at each frame)
    // and the last IRQ span (light blue over one frame).
    const CycleMonitor& m = machine_.monitor;
    const float lh = ImGui::GetTextLineHeightWithSpacing();
    const float labelW = ImGui::CalcTextSize("section").x + 10.0f;
    const float barW = std::min(320.0f, std::max(120.0f, w * 0.35f));
    const float barH = std::min(12.0f, lh - 5.0f);
    const int frame = machine_.frameCycles;
    auto row = [&](float ry, const char* name, const CycleMonitor::Stats& st, bool open,
                   const CycleMonitor::Bar& bar, int scaleFrames) {
        dl->AddText(ImVec2(x, ry), IM_COL32(150, 156, 172, 255), name);
        const float bx = x + labelW, by = ry + (lh - 4.0f - barH) * 0.5f;
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + barW, by + barH), IM_COL32(24, 26, 32, 255), 2.0f);
        if (st.count)
            dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + barW * bar.fill, by + barH),
                              IM_COL32(bar.r, bar.g, bar.b, 255), 2.0f);
        for (int f = 1; f < scaleFrames; ++f) {
            const float tx = bx + barW * static_cast<float>(f) / static_cast<float>(scaleFrames);
            dl->AddLine(ImVec2(tx, by), ImVec2(tx, by + barH), IM_COL32(255, 255, 255, 200));
        }
        dl->AddRect(ImVec2(bx - 1, by - 1), ImVec2(bx + barW + 1, by + barH + 1), IM_COL32(255, 255, 255, 120), 2.0f);
        char t[160];
        if (!st.count)
            std::snprintf(t, sizeof t, "%s", open ? "started, not stopped yet" : "no span yet");
        else
            std::snprintf(t, sizeof t, "%s cycles  \xc2\xb7  %.1f %% of a frame  \xc2\xb7  %.0f lines%s%s",
                          grouped(st.last).c_str(), 100.0 * static_cast<double>(st.lastVideo) / frame,
                          CycleMonitor::lines(st.lastVideo), scaleFrames > 1 ? "  \xc2\xb7  scale " : "",
                          scaleFrames == 4 ? "4 frames" : scaleFrames == 8 ? "8 frames" : "");
        dl->AddText(ImVec2(bx + barW + 10.0f, ry), IM_COL32(220, 224, 234, 255), t);
    };
    const CycleMonitor::Bar sb = CycleMonitor::sectionBar(m.section().lastVideo, frame);
    row(y, "section", m.section(), m.sectionOpen(), sb, sb.frames);
    row(y + lh, "IRQ", m.irq(), m.irqOpen(), CycleMonitor::irqBar(m.irq().lastVideo, frame), 1);
}

void MemoryHeatmap_ImGui::drawMonitorSection()
{
    const CycleMonitor& m = machine_.monitor;
    const int frame = machine_.frameCycles;
    if (m.marks() == 0) {
        ImGui::TextWrapped("Nothing yet. A program times a stretch of its code by writing $C067 "
                           "before it and $C066 after it, and its interrupt handler by writing "
                           "$C069 / $C068 around it - French Touch's \"Monitoring Card\" markers, "
                           "drawn as AppleWin FT draws them. The machine ignores those writes.");
        return;
    }
    if (ImGui::BeginTable("##mon", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("cycles");
        ImGui::TableSetupColumn("last");
        ImGui::TableSetupColumn("min");
        ImGui::TableSetupColumn("max");
        ImGui::TableSetupColumn("mean");
        ImGui::TableHeadersRow();
        auto row = [&](const char* name, const CycleMonitor::Stats& st) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(st.count ? grouped(st.last).c_str() : "-");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(st.count ? grouped(st.min).c_str() : "-");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(st.count ? grouped(st.max).c_str() : "-");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(st.count ? grouped(static_cast<uint64_t>(st.average() + 0.5)).c_str() : "-");
        };
        row("section", m.section());
        row("IRQ", m.irq());
        ImGui::EndTable();
    }
    auto share = [&](const char* name, const CycleMonitor::Stats& st) {
        if (!st.count) return;
        ImGui::Text("%s: %.1f %% of a frame, %.0f lines (last); %u spans", name,
                    100.0 * static_cast<double>(st.lastVideo) / frame, CycleMonitor::lines(st.lastVideo), st.count);
    };
    share("section", m.section());
    share("IRQ", m.irq());
    if (m.section().count)
        ImGui::Text("IRQ inside the last section: %s cycles", grouped(m.irqInLastSection()).c_str());
    if (m.orphanStops())
        ImGui::TextDisabled("%s stop(s) with no start ignored", grouped(m.orphanStops()).c_str());
    // The last spans, as a share of a frame.
    const CycleMonitor::History& hs = m.sectionHistory();
    if (hs.size > 1) {
        std::vector<float> v(hs.size);
        for (std::size_t i = 0; i < hs.size; ++i) v[i] = 100.0f * static_cast<float>(hs.at(i)) / static_cast<float>(frame);
        ImGui::TextDisabled("the last %zu sections, %% of a frame", hs.size);
        ImGui::PushStyleColor(ImGuiCol_PlotLines, colourV({0x00, 0xDD, 0x00}));
        ImGui::PlotLines("##sechist", v.data(), static_cast<int>(v.size()), 0, nullptr, 0.0f,
                         std::max(100.0f, *std::max_element(v.begin(), v.end())),
                         ImVec2(ImGui::GetContentRegionAvail().x, 44.0f));
        ImGui::PopStyleColor();
    }
    if (ImGui::SmallButton("Reset figures")) wantMonitorReset_ = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Write $C067 / $C066 around a section, $C069 / $C068 around the IRQ handler.\n"
                          "Each write is timed at its own bus cycle; the frame is this machine's\n"
                          "(%d cycles), not AppleWin FT's NTSC one. A line is 65 cycles.", frame);
}

}  // namespace pom2
