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

// Memory Heatmap, beside the map: the inspector, the loupe, the right-click
// menu, the breakpoints it arms, the legend and the PNG.

#include "MemoryHeatmap_ImGui.h"

#include "AppleIoNames.h"
#include "Debugger.h"
#include "EmulationController.h"
#include "ResourcePaths.h"
#include "ScreenshotExport.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace pom2 {

namespace {

ImU32 colour(heatview::Rgb c, int alpha = 255)
{
    return IM_COL32(c.r, c.g, c.b, alpha);
}

ImVec4 colourV(heatview::Rgb c)
{
    return ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f);
}

/// A swatch the height of a text line.
void swatch(ImU32 col)
{
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + h * 0.2f),
                                              ImVec2(p.x + h * 0.6f, p.y + h * 0.8f), col, 2.0f);
    ImGui::Dummy(ImVec2(h * 0.6f, h));
}

std::string addressText(const DebugAddress& at)
{
    char t[24];
    std::snprintf(t, sizeof t, "$%04X", at.addr);
    return at.bank.kind == DebugBank::Any ? std::string(t) : at.text();
}

std::string withThousands(uint64_t v)
{
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), " ");
    return s;
}

/// Buttons laid out left to right, wrapping to the next line when the next
/// one would cross the panel's edge — at any width and UI scale.
class ButtonFlow {
public:
    ButtonFlow() : right_(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x) {}
    bool button(const char* label)
    {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float w = ImGui::CalcTextSize(label, nullptr, true).x + st.FramePadding.x * 2.0f;
        if (!first_ && ImGui::GetItemRectMax().x + st.ItemSpacing.x + w <= right_) ImGui::SameLine();
        first_ = false;
        return ImGui::Button(label);
    }
private:
    float right_;
    bool first_ = true;
};

}  // namespace

// ── Small helpers ──────────────────────────────────────────────────────────

const MemoryHeatmap_ImGui::Column* MemoryHeatmap_ImGui::columnOf(int plane) const
{
    for (const Column& c : columns_)
        if (c.plane == plane) return &c;
    return nullptr;
}

uint8_t MemoryHeatmap_ImGui::valueOf(int plane, uint16_t phys) const
{
    const Column* c = columnOf(plane);
    return c && c->values.size() > phys ? c->values[phys] : 0;
}

void MemoryHeatmap_ImGui::say(std::string text)
{
    status_ = std::move(text);
    statusUntil_ = ImGui::GetTime() + 5.0;
}

void MemoryHeatmap_ImGui::select(int plane, uint16_t phys, bool centre)
{
    selected_ = true;
    ranged_ = false;
    selPlane_ = plane;
    selPhys_ = phys;
    if (!centre || lastW_ <= 0.0f) return;
    float cx = 0, cy = 0;
    if (!heatview::cellOrigin(layout_, plane, phys, cx, cy)) return;
    const float sx = xf_.toScreenX(cx), sy = xf_.toScreenY(cy);
    const float margin = std::max(8.0f, xf_.scale * 2.0f);
    if (sx < margin || sy < margin || sx > lastW_ - margin || sy > lastH_ - margin)
        heatview::centreOn(xf_, layout_, lastW_, lastH_, cx + 0.5f, cy + 0.5f, xf_.scale);
}

HeatData::ByteInfo MemoryHeatmap_ImGui::infoOf(EmulationController& ctrl, int plane, uint16_t phys)
{
    if (frozen_) return frozen_->byteInfo(plane, phys);
    if (!heat_) return {};
    auto st = ctrl.lockState();
    return heat_->byteInfo(plane, phys);
}

std::string MemoryHeatmap_ImGui::describeAge(uint32_t stamp) const
{
    if (stamp == 0) return "never";
    if (stamp > machine_.now) return "-";
    const double cycles = static_cast<double>(machine_.now - stamp) * (1u << HeatData::kStampShift);
    const double frames = cycles / machine_.frameCycles;
    char t[64];
    if (frames < 1.0) std::snprintf(t, sizeof t, "this frame");
    else if (frames < 1000.0) std::snprintf(t, sizeof t, "%.0f frames ago", frames);
    else std::snprintf(t, sizeof t, "%.1f s ago", cycles / machine_.clockHz);
    return t;
}

std::string MemoryHeatmap_ImGui::describeByte(int plane, uint16_t phys, const LabelFn& labelOf) const
{
    const DebugAddress at = HeatData::cpuAddress(plane, phys);
    std::string s = addressText(at);
    s += "  " + heatview::planeTitle(plane, machine_.auxBanks);
    if (const heatview::Region* r = heatview::regionAt(plane, phys)) s += std::string("  \xc2\xb7  ") + r->name;
    if (plane != HeatData::kRom && phys >= 0xC000 && phys < 0xD000) {
        char t[48];
        std::snprintf(t, sizeof t, "\n(the chips' $%04X: bank 1 is kept there)", phys);
        s += t;
    }
    if (labelOf) {
        const std::string label = labelOf(at.addr, at.bank);
        if (!label.empty()) s += "\n" + label;
    }
    if (plane == HeatData::kRom && phys < 0xC090) {
        if (const char* io = motherboardIoName(phys, machine_.iicClass)) s += std::string("\n") + io;
    } else if (plane == HeatData::kRom && phys < 0xC100) {
        char t[48];
        std::snprintf(t, sizeof t, "\nslot %d I/O, register %X", (phys - 0xC080) >> 4, phys & 0x0F);
        s += t;
    }
    if (!(plane == HeatData::kRom && phys < 0xC100)) {
        char t[32];
        std::snprintf(t, sizeof t, "\nvalue $%02X", valueOf(plane, phys));
        s += t;
    }
    return s;
}

// ── The loupe ──────────────────────────────────────────────────────────────

void MemoryHeatmap_ImGui::drawLoupe(int plane, uint16_t phys)
{
    // The map around the pointer, a byte a cell, with its value: the 16
    // bytes of its column group on the 9 pages around it, as on the map.
    const Column* c = columnOf(plane);
    const uint32_t lo = static_cast<uint32_t>(heatview::firstRow(plane)) << 8;
    if (!c || c->cells.size() < 0x10000 - lo) return;
    constexpr int kCols = 16, kRows = 9;
    const float fs = ImGui::GetFontSize();
    const float cw = fs * 1.55f, ch = fs * 1.2f, labelW = fs * 3.4f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int group = phys & 0xF0, row = phys >> 8;
    for (int j = 0; j < kCols; ++j) {
        char t[4];
        std::snprintf(t, sizeof t, "%X", j);
        dl->AddText(ImVec2(p.x + labelW + j * cw + cw * 0.35f, p.y), IM_COL32(140, 146, 162, 255), t);
    }
    for (int i = 0; i < kRows; ++i) {
        const int r = opt_.bottomUp ? row + kRows / 2 - i : row - kRows / 2 + i;
        const float y = p.y + ch + i * ch;
        if (r < heatview::firstRow(plane) || r > 255) continue;
        char t[8];
        std::snprintf(t, sizeof t, "$%02X%02X", r, group);
        dl->AddText(ImVec2(p.x, y + (ch - fs) * 0.5f),
                    r == row ? IM_COL32(255, 255, 255, 255) : IM_COL32(140, 146, 162, 255), t);
        for (int j = 0; j < kCols; ++j) {
            const auto a = static_cast<uint16_t>((r << 8) | (group + j));
            const uint32_t px = heatview::compose(c->cells[a - lo], opt_.style);
            const ImVec2 q(p.x + labelW + j * cw, y);
            dl->AddRectFilled(q, ImVec2(q.x + cw - 1, q.y + ch - 1), px | 0xFF000000u);
            if (!(plane == HeatData::kRom && a < 0xC100)) {
                const int lum = static_cast<int>((px & 0xFF) * 3 + ((px >> 8) & 0xFF) * 6 + ((px >> 16) & 0xFF)) / 10;
                std::snprintf(t, sizeof t, "%02X", c->values[a]);
                dl->AddText(nullptr, fs * 0.85f, ImVec2(q.x + cw * 0.14f, q.y + ch * 0.12f),
                            lum > 140 ? IM_COL32(10, 10, 14, 255) : IM_COL32(226, 230, 240, 230), t);
            }
            if (a == phys)
                dl->AddRect(ImVec2(q.x - 1, q.y - 1), ImVec2(q.x + cw, q.y + ch), IM_COL32(255, 255, 255, 255),
                            0.0f, 0, 2.0f);
        }
    }
    ImGui::Dummy(ImVec2(labelW + kCols * cw, ch * (kRows + 1)));
}

// ── Breakpoints ────────────────────────────────────────────────────────────

void MemoryHeatmap_ImGui::armRange(EmulationController& ctrl, int plane, uint16_t lo, uint16_t hi,
                                   int access)
{
    int armed = 0;
    {
        auto st = ctrl.lockState();
        Debugger& dbg = ctrl.debugger();
        std::map<DebugAddress, int> have;
        for (const Debugger::Watch& w : dbg.watchpoints()) have[{w.addr, w.bank}] = w.access;
        for (uint32_t p = lo; p <= hi; ++p) {
            const DebugAddress at = HeatData::cpuAddress(plane, static_cast<uint16_t>(p));
            const auto it = have.find(at);
            const int now = it == have.end() ? 0 : it->second;
            const int want = access ? (now | access) : 0;
            if (want == now) continue;
            dbg.setWatchpoint(at.addr, static_cast<Debugger::Access>(want), at.bank);
            ++armed;
        }
        ctrl.syncDebugHook();
    }
    const DebugAddress first = HeatData::cpuAddress(plane, lo);
    char t[160];
    if (!access)
        std::snprintf(t, sizeof t, "Removed the memory breakpoints of %u byte(s) from %s.",
                      static_cast<unsigned>(hi - lo + 1), addressText(first).c_str());
    else
        std::snprintf(t, sizeof t, "Break on %s: %u byte(s) from %s (%d new).",
                      access == Debugger::ReadWrite ? "read or write"
                          : access == Debugger::Read ? "read" : "write",
                      static_cast<unsigned>(hi - lo + 1), addressText(first).c_str(), armed);
    say(t);
}

void MemoryHeatmap_ImGui::armPc(EmulationController& ctrl, const DebugAddress& at)
{
    {
        auto st = ctrl.lockState();
        if (at.bank.kind == DebugBank::Any) ctrl.debugger().addBreakpoint(at.addr);
        else ctrl.debugger().addBreakpoint(at.addr, at.bank);
        ctrl.syncDebugHook();
    }
    say("Breakpoint at " + addressText(at) + ".");
}

// ── Right-click menu ───────────────────────────────────────────────────────

void MemoryHeatmap_ImGui::drawContextMenu(EmulationController& ctrl)
{
    if (!ImGui::BeginPopup("##heatmenu")) return;
    const DebugAddress at = HeatData::cpuAddress(menuPlane_, menuPhys_);
    const std::string where = addressText(at);
    ImGui::TextDisabled("%s  %s", where.c_str(), heatview::planeTitle(menuPlane_, machine_.auxBanks).c_str());
    ImGui::Separator();
    char item[160];
    if (menuOnRange_) {
        const std::string from = addressText(HeatData::cpuAddress(selPlane_, rangeLo_));
        const unsigned n = static_cast<unsigned>(rangeHi_ - rangeLo_ + 1);
        std::snprintf(item, sizeof item, "Break on writes to the %u bytes from %s  (BPMW)", n, from.c_str());
        if (ImGui::MenuItem(item)) armRange(ctrl, selPlane_, rangeLo_, rangeHi_, Debugger::Write);
        std::snprintf(item, sizeof item, "Break on reads of the %u bytes  (BPMR)", n);
        if (ImGui::MenuItem(item)) armRange(ctrl, selPlane_, rangeLo_, rangeHi_, Debugger::Read);
        std::snprintf(item, sizeof item, "Break on reads or writes of the %u bytes", n);
        if (ImGui::MenuItem(item)) armRange(ctrl, selPlane_, rangeLo_, rangeHi_, Debugger::ReadWrite);
        if (ImGui::MenuItem("Remove the memory breakpoints of the range"))
            armRange(ctrl, selPlane_, rangeLo_, rangeHi_, 0);
        ImGui::Separator();
    }
    std::snprintf(item, sizeof item, "Break at %s  (BP)", where.c_str());
    if (ImGui::MenuItem(item)) armPc(ctrl, at);
    std::snprintf(item, sizeof item, "Break when %s is read  (BPMR)", where.c_str());
    if (ImGui::MenuItem(item)) armRange(ctrl, menuPlane_, menuPhys_, menuPhys_, Debugger::Read);
    std::snprintf(item, sizeof item, "Break when %s is written  (BPMW)", where.c_str());
    if (ImGui::MenuItem(item)) armRange(ctrl, menuPlane_, menuPhys_, menuPhys_, Debugger::Write);
    std::snprintf(item, sizeof item, "Break when %s is read or written", where.c_str());
    if (ImGui::MenuItem(item)) armRange(ctrl, menuPlane_, menuPhys_, menuPhys_, Debugger::ReadWrite);
    ImGui::Separator();
    std::snprintf(item, sizeof item, "Watch $%04X", at.addr);
    if (ImGui::MenuItem(item)) request_ = {Request::Kind::Watch, at};
    std::snprintf(item, sizeof item, "Watch $%04X as a 16-bit word", at.addr);
    if (ImGui::MenuItem(item)) request_ = {Request::Kind::WatchWord, at};
    if (ImGui::MenuItem("Show in the Debugger's listing", "double-click"))
        request_ = {Request::Kind::Listing, at};
    if (ImGui::MenuItem("Show in the Memory viewer")) request_ = {Request::Kind::MemoryViewer, at};
    ImGui::Separator();
    if (ImGui::MenuItem("Select this byte")) select(menuPlane_, menuPhys_, false);
    if (ImGui::MenuItem("Zoom to this byte")) {
        select(menuPlane_, menuPhys_, false);
        float cx = 0, cy = 0;
        if (heatview::cellOrigin(layout_, menuPlane_, menuPhys_, cx, cy))
            heatview::centreOn(xf_, layout_, lastW_, lastH_, cx + 0.5f, cy + 0.5f, 28.0f);
    }
    if (ImGui::MenuItem("Copy the address")) ImGui::SetClipboardText(where.c_str());
    ImGui::EndPopup();
}

// ── The inspector ──────────────────────────────────────────────────────────

void MemoryHeatmap_ImGui::drawInspector(EmulationController& ctrl, const LabelFn& labelOf)
{
    const bool have = selected_ || hovered_;
    const int plane = selected_ ? selPlane_ : hoverPlane_;
    const uint16_t phys = selected_ ? selPhys_ : hoverPhys_;

    // ── The byte ───────────────────────────────────────────────────────────
    ImGui::SeparatorText(selected_ ? "Selected byte" : "Under the pointer");
    if (!have) {
        ImGui::TextDisabled("Hover a byte, or click one to keep it here.");
    } else {
        const DebugAddress at = HeatData::cpuAddress(plane, phys);
        ImGui::SetWindowFontScale(1.45f);
        ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.36f, 1.0f), "$%04X", at.addr);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", heatview::planeTitle(plane, machine_.auxBanks).c_str());
        if (at.bank.kind != DebugBank::Any) {
            ImGui::TextDisabled("bank");
            ImGui::SameLine();
            ImGui::TextUnformatted(at.text().c_str());
        }
        if (plane != HeatData::kRom && phys >= 0xC000 && phys < 0xD000)
            ImGui::TextDisabled("kept in the chips at $%04X (bank 1 of $D000)", phys);
        if (const heatview::Region* r = heatview::regionAt(plane, phys))
            ImGui::TextUnformatted(r->name);
        const std::string spot = heatview::describeSpot(heatview::screenSpot(plane, phys));
        if (!spot.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.86f, 0.95f, 1.0f));
            ImGui::TextWrapped("%s", spot.c_str());
            ImGui::PopStyleColor();
        }
        if (labelOf) {
            const std::string label = labelOf(at.addr, at.bank);
            if (!label.empty()) ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "%s", label.c_str());
        }
        const bool io = plane == HeatData::kRom && phys < 0xC100;
        if (io && phys < 0xC090) {
            if (const char* n = motherboardIoName(phys, machine_.iicClass)) ImGui::TextWrapped("%s", n);
        } else if (io) {
            ImGui::Text("Slot %d I/O, register %X", (phys - 0xC080) >> 4, phys & 0x0F);
        } else if (plane == HeatData::kRom && phys < 0xC800) {
            ImGui::Text("Slot %d ROM", (phys >> 8) & 7);
        }
        if (!io) {
            const uint8_t v = valueOf(plane, phys);
            char bits[10];
            for (int i = 0; i < 8; ++i) bits[i] = (v & (0x80 >> i)) ? '1' : '0';
            bits[8] = 0;
            const char ch = static_cast<char>(v & 0x7F);
            ImGui::Text("$%02X  %3u  %%%.4s %.4s  %c", v, v, bits, bits + 4,
                        ch >= 0x20 && ch < 0x7F ? ch : '.');
        }

        const HeatData::ByteInfo info = infoOf(ctrl, plane, phys);
        if (ImGui::BeginTable("##kinds", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("last");
            ImGui::TableSetupColumn("count");
            ImGui::TableSetupColumn("by");
            ImGui::TableHeadersRow();
            for (int k : {HeatData::Write, HeatData::Read, HeatData::Exec, HeatData::Dma}) {
                const auto ki = static_cast<std::size_t>(k);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                swatch(colour(heatview::kindColour(k)));
                ImGui::SameLine();
                ImGui::TextUnformatted(heatview::kindName(k));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(describeAge(info.stamp[ki]).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(withThousands(info.count[ki]).c_str());
                ImGui::TableNextColumn();
                if ((k == HeatData::Read || k == HeatData::Write) && info.stamp[ki]) {
                    const DebugAddress by{k == HeatData::Read ? info.readPc : info.writePc,
                                          k == HeatData::Read ? info.readPcBank : info.writePcBank};
                    std::string text = addressText(by);
                    if (labelOf) {
                        const std::string l = labelOf(by.addr, by.bank);
                        if (!l.empty()) text += " " + l;
                    }
                    ImGui::PushID(k);
                    if (ImGui::SmallButton(text.c_str())) request_ = {Request::Kind::Listing, by};
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("The instruction that made the last access: show it in the listing.");
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        if (selected_) {
            ButtonFlow flow;
            if (flow.button("Listing")) request_ = {Request::Kind::Listing, at};
            if (flow.button("Memory")) request_ = {Request::Kind::MemoryViewer, at};
            if (flow.button("BP")) armPc(ctrl, at);
            if (flow.button("BPMR")) armRange(ctrl, plane, phys, phys, Debugger::Read);
            if (flow.button("BPMW")) armRange(ctrl, plane, phys, phys, Debugger::Write);
            if (flow.button("Watch")) request_ = {Request::Kind::Watch, at};
        }
    }

    // ── The range ──────────────────────────────────────────────────────────
    if (ranged_) {
        ImGui::SeparatorText("Selected range");
        const std::string from = addressText(HeatData::cpuAddress(selPlane_, rangeLo_));
        const std::string to = addressText(HeatData::cpuAddress(selPlane_, rangeHi_));
        ImGui::Text("%s - %s", from.c_str(), to.c_str());
        ImGui::TextDisabled("%u bytes, %s", static_cast<unsigned>(rangeHi_ - rangeLo_ + 1),
                            heatview::planeTitle(selPlane_, machine_.auxBanks).c_str());
        if (--rangeSummaryAge_ <= 0) {
            rangeSummaryAge_ = 15;
            rangeSummary_ = {};
            auto st = ctrl.lockState();
            const HeatData& src = frozen_ ? static_cast<const HeatData&>(*frozen_)
                                          : static_cast<const HeatData&>(*heat_);
            if (const HeatData::PlaneData* d = src.plane(selPlane_))
                for (uint32_t a = rangeLo_; a <= rangeHi_; ++a)
                    for (int k = 0; k < HeatData::kKinds; ++k) {
                        const auto ki = static_cast<std::size_t>(k);
                        if (d->stamp[ki][a]) ++rangeSummary_.touched[k];
                        rangeSummary_.total[k] += d->count[ki][a];
                    }
        }
        for (int k : {HeatData::Write, HeatData::Read, HeatData::Exec, HeatData::Dma}) {
            swatch(colour(heatview::kindColour(k)));
            ImGui::SameLine();
            ImGui::Text("%s: %u bytes, %s accesses", heatview::kindName(k), rangeSummary_.touched[k],
                        withThousands(rangeSummary_.total[k]).c_str());
        }
        ButtonFlow flow;
        if (flow.button("Break on write")) armRange(ctrl, selPlane_, rangeLo_, rangeHi_, Debugger::Write);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Stop at the first write into the range, in this bank:\n"
                              "\"does my code write where it must not\".");
        if (flow.button("on read")) armRange(ctrl, selPlane_, rangeLo_, rangeHi_, Debugger::Read);
        if (flow.button("Remove")) armRange(ctrl, selPlane_, rangeLo_, rangeHi_, 0);
        if (flow.button("Unselect")) selected_ = ranged_ = false;
    } else {
        ImGui::Spacing();
        ImGui::TextDisabled("Shift-drag on the map selects a range.");
    }

    // ── Hot spots ──────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Hottest bytes since Clear", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* const kinds[] = {"read", "written", "executed", "card DMA"};
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::Combo("##hotkind", &hotKind_, kinds, 4)) hotAge_ = 0;
        if (--hotAge_ <= 0 || (hot_.empty() && hotAge_ % 4 == 0)) {
            hotAge_ = 12;
            hot_.clear();
            auto st = ctrl.lockState();
            const HeatData& src = frozen_ ? static_cast<const HeatData&>(*frozen_)
                                          : static_cast<const HeatData&>(*heat_);
            for (const Column& c : columns_) {
                auto top = heatview::hottest(src, c.plane, hotKind_, 12);
                hot_.insert(hot_.end(), top.begin(), top.end());
            }
            std::sort(hot_.begin(), hot_.end(), [](const heatview::Hot& a, const heatview::Hot& b) {
                return a.count > b.count;
            });
            if (hot_.size() > 12) hot_.resize(12);
        }
        if (hot_.empty()) ImGui::TextDisabled("Nothing yet.");
        if (!hot_.empty() &&
            ImGui::BeginTable("##hot", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            for (std::size_t i = 0; i < hot_.size(); ++i) {
                const heatview::Hot& h = hot_[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string text = addressText(HeatData::cpuAddress(h.plane, h.phys));
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(text.c_str(), selected_ && selPlane_ == h.plane && selPhys_ == h.phys,
                                      ImGuiSelectableFlags_SpanAllColumns))
                    jumpTo(h.plane, h.phys);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go there: select it and zoom until its value shows.");
                ImGui::PopID();
                ImGui::TableNextColumn();
                const heatview::Region* r = heatview::regionAt(h.plane, h.phys);
                std::string label = labelOf ? labelOf(HeatData::cpuAddress(h.plane, h.phys).addr,
                                                      HeatData::cpuAddress(h.plane, h.phys).bank)
                                            : std::string();
                ImGui::TextDisabled("%s", !label.empty() ? label.c_str() : r ? r->tag : "");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(withThousands(h.count).c_str());
            }
            ImGui::EndTable();
        }
    }

    // ── French Touch's monitoring marks (AppleWin FT's CPU and IRQ bars) ──
    if (ImGui::CollapsingHeader("Cycle monitor ($C066-$C069)", ImGuiTreeNodeFlags_DefaultOpen))
        drawMonitorSection();

    // ── Interrupt load, measured by POM2 ───────────────────────────────────
    if (ImGui::CollapsingHeader("Interrupt load per frame")) {
        const auto& f = machine_.irq;
        if (f.empty()) {
            ImGui::TextDisabled("No frame finished since the map opened.");
        } else {
            const std::size_t n = std::min<std::size_t>(f.size(), 160);
            std::vector<float> share(n);
            for (std::size_t i = 0; i < n; ++i) share[i] = static_cast<float>(f[f.size() - n + i].share() * 100.0);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, colourV(heatview::kindColour(HeatData::Read)));
            ImGui::PlotHistogram("##irq", share.data(), static_cast<int>(n), 0, nullptr, 0.0f, 100.0f,
                                 ImVec2(ImGui::GetContentRegionAvail().x, 56.0f));
            ImGui::PopStyleColor();
            ImGui::Text("last %.1f %%  \xc2\xb7  mean %.1f %%  \xc2\xb7  peak %.1f %%",
                        machine_.irqSummary.last * 100.0, machine_.irqSummary.mean * 100.0,
                        machine_.irqSummary.peak * 100.0);
            ImGui::TextWrapped("Share of each frame's cycles inside IRQ / NMI handlers, measured by "
                               "POM2 from the interrupt's entry to the RTI that restores the stack - "
                               "every handler, every frame, no marker needed. The cycle monitor's IRQ "
                               "bar (AppleWin FT's) times only what the program brackets with "
                               "$C069 / $C068, and shows the last span.");
        }
    }

    // ── Legend and help ────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Legend and controls")) drawLegend();
}

void MemoryHeatmap_ImGui::drawLegend()
{
    for (int k : {HeatData::Write, HeatData::Read, HeatData::Exec, HeatData::Dma}) {
        swatch(colour(heatview::kindColour(k)));
        ImGui::SameLine();
        ImGui::TextUnformatted(heatview::kindName(k));
    }
    swatch(colour(heatview::railColour(heatview::RailRead | heatview::RailWrite, opt_.style.palette)));
    ImGui::SameLine();
    ImGui::TextUnformatted("rail: the CPU reads AND writes this page here");
    swatch(IM_COL32(236, 236, 240, 255));
    ImGui::SameLine();
    ImGui::TextUnformatted("rail: the video shows this page");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "One pixel a byte, one row a page; the language card's $D000 bank 1 is in the $C0-$CF rows "
        "of MAIN and AUX, its bank 2 in $D0-$DF (the chips' own layout, as AppleWin FT draws it). "
        "The grey under the colours is the bits set in each byte. Smaller than a pixel, a byte is "
        "max-pooled: the hottest byte of each pixel wins, per kind.");
    ImGui::Spacing();
    ImGui::TextDisabled("Mouse");
    ImGui::BulletText("wheel or two-finger pinch: zoom where the pointer is; drag: pan; the minimap: move");
    ImGui::BulletText("click: select; shift-drag: a range");
    ImGui::BulletText("double-click: the listing (on empty space: fit)");
    ImGui::BulletText("right click: BP / BPMR / BPMW / Watch, in the byte's bank");
    ImGui::TextDisabled("Keys (the window focused)");
    ImGui::BulletText("arrows, Page Up / Down: move the selection; Esc: unselect");
    ImGui::BulletText("Enter: the listing; Ctrl+G: go to; Alt+Left / Right: back / forward");
    ImGui::BulletText("Home: fit; + / -: zoom; F: freeze; C: clear");
    ImGui::Spacing();
    ImGui::TextDisabled("After fenarinarsa's heatmap in French Touch's AppleWin.");
}

// ── PNG ────────────────────────────────────────────────────────────────────

void MemoryHeatmap_ImGui::exportPng()
{
    // Two pixels a byte, the columns and their rails as on screen.
    constexpr int kPx = 2, kMargin = 16;
    const int width = static_cast<int>(layout_.width) * kPx + 2 * kMargin;
    const int height = 256 * kPx + 2 * kMargin;
    std::vector<uint32_t> px(static_cast<std::size_t>(width) * height, 0xFF110E0Du);
    auto fill = [&](int x0, int y0, int w, int h, uint32_t c) {
        for (int y = std::max(0, y0); y < std::min(height, y0 + h); ++y)
            for (int x = std::max(0, x0); x < std::min(width, x0 + w); ++x)
                px[static_cast<std::size_t>(y) * width + x] = c;
    };
    auto abgr = [](ImU32 c) { return static_cast<uint32_t>(c); };   // ImU32 is ABGR already
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const Column& c = columns_[i];
        const int x0 = kMargin + static_cast<int>(layout_.columns[i].x) * kPx;
        const int r0 = heatview::firstRow(c.plane);
        if (c.cells.size() < static_cast<std::size_t>(256 - r0) * 256u) continue;
        for (int r = r0; r < 256; ++r) {
            const int y = kMargin + static_cast<int>(heatview::rowY(layout_, r)) * kPx;
            for (int b = 0; b < 256; ++b)
                fill(x0 + b * kPx, y, kPx, kPx,
                     heatview::compose(c.cells[static_cast<std::size_t>(((r - r0) << 8) | b)], opt_.style));
            const uint8_t bank = c.rails.bank[static_cast<std::size_t>(r)];
            if (opt_.rails && bank)
                fill(x0 + 257 * kPx, y, 3 * kPx, kPx, abgr(colour(heatview::railColour(bank, opt_.style.palette))));
            if (opt_.rails && c.rails.shown[static_cast<std::size_t>(r)])
                fill(x0 + 261 * kPx, y, 3 * kPx, kPx, abgr(IM_COL32(236, 236, 240, 255)));
        }
    }
    ScreenshotMetadata meta;
    meta.software = "POM2 Memory Heatmap";
    meta.monitor = "Memory Heatmap";
    meta.machine = machine_.iie ? "Apple //e class" : "Apple ][ class";
    meta.videoStandard = machine_.frameCycles == 20280 ? "PAL" : "NTSC";
    std::filesystem::path out;
    std::string error;
    if (saveScreenshotPng(userDataDir() / "screenshots", width, height, px, meta, out, error))
        say("Saved " + out.string());
    else
        say("PNG: " + error);
}

}  // namespace pom2
