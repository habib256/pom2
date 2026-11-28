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

// Memory Heatmap, the toolbar: ONE row — the mode as a segmented control,
// the four kinds as coloured chips (they are the legend), the fade, Clear /
// Freeze / PNG, a View popup holding every other choice, Go to with Back /
// Forward and Fit. Also the keys, and the jumps (Go to, the hottest list,
// the history) that all land the same way.

#include "MemoryHeatmap_ImGui.h"

#include "EmulationController.h"
#include "IconsFontAwesome6.h"
#include "Memory.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace pom2 {

namespace {

/// A two-state segmented control; returns the index chosen.
int segmented(const char* a, const char* b, int value, const char* tipA, const char* tipB)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1.0f, st.ItemSpacing.y));
    for (int i = 0; i < 2; ++i) {
        if (i) ImGui::SameLine();
        const bool on = value == i;
        ImGui::PushStyleColor(ImGuiCol_Button, on ? st.Colors[ImGuiCol_ButtonActive]
                                                  : st.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_Text, on ? st.Colors[ImGuiCol_Text] : st.Colors[ImGuiCol_TextDisabled]);
        if (ImGui::Button(i ? b : a)) value = i;
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i ? tipB : tipA);
    }
    ImGui::PopStyleVar();
    return value;
}

/// A kind of access as a coloured chip: a dot and its name, filled when
/// shown, outlined when hidden. The chips ARE the legend.
void chip(const char* label, heatview::Rgb c, bool& on, const char* tip)
{
    const float h = ImGui::GetFrameHeight();
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(ts.x + h * 1.25f, h);
    ImGui::PushID(label);
    if (ImGui::InvisibleButton("##chip", size)) on = !on;
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = IM_COL32(c.r, c.g, c.b, 255);
    const ImVec2 q(p.x + size.x, p.y + size.y);
    if (on) dl->AddRectFilled(p, q, IM_COL32(c.r, c.g, c.b, hov ? 70 : 48), h * 0.5f);
    dl->AddRect(p, q, IM_COL32(c.r, c.g, c.b, on ? 200 : (hov ? 150 : 90)), h * 0.5f, 0, 1.2f);
    dl->AddCircleFilled(ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.2f,
                        on ? col : IM_COL32(c.r, c.g, c.b, 90));
    dl->AddText(ImVec2(p.x + h * 0.85f, p.y + (h - ts.y) * 0.5f),
                on ? IM_COL32(238, 240, 246, 255) : IM_COL32(150, 154, 166, 255), label);
    if (hov) ImGui::SetTooltip("%s\n(click to %s)", tip, on ? "hide" : "show");
}

}  // namespace

void MemoryHeatmap_ImGui::drawToolbar(EmulationController& ctrl)
{
    ImGui::AlignTextToFramePadding();
    const int mode = segmented("Recent", "Cumulative", opt_.style.cumulative ? 1 : 0,
                               "Each access fades with its age (AppleWin FT's map).",
                               "How often each byte was reached since Clear (log scale): a footprint.");
    opt_.style.cumulative = mode == 1;

    static const struct { int kind; const char* label; const char* tip; } kKinds[] = {
        {HeatData::Write, "Write", "Writes by the 6502."},
        {HeatData::Read, "Read", "Data reads by the 6502 (an instruction's own bytes are execution)."},
        {HeatData::Exec, "Exec", "Instruction bytes the 6502 executed."},
        {HeatData::Dma, "DMA", "Accesses made by a card's processor holding the bus (SoftCard Z80)."},
    };
    ImGui::SameLine(0.0f, 14.0f);
    for (const auto& k : kKinds) {
        chip(k.label, heatview::kindColour(k.kind), opt_.style.show[static_cast<std::size_t>(k.kind)], k.tip);
        ImGui::SameLine(0.0f, 4.0f);
    }

    ImGui::SameLine(0.0f, 12.0f);
    ImGui::SetNextItemWidth(130.0f);
    ImGui::BeginDisabled(opt_.style.cumulative);
    ImGui::SliderInt("##fade", &opt_.style.fadeFrames, 1, 1500, "fade %d fr", ImGuiSliderFlags_Logarithmic);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("An access fades to nothing in %d frames, %.2f s of emulated time.",
                          opt_.style.fadeFrames,
                          static_cast<double>(opt_.style.fadeFrames) * machine_.frameCycles / machine_.clockHz);

    ImGui::SameLine(0.0f, 12.0f);
    if (ImGui::Button(ICON_FA_ERASER)) { wantClear_ = true; frozen_.reset(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear: forget every access recorded (C).");
    ImGui::SameLine(0.0f, 4.0f);
    const bool frozen = frozen_ != nullptr;
    if (frozen) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 110, 170, 255));
    if (ImGui::Button(frozen ? ICON_FA_SNOWFLAKE " Frozen" : ICON_FA_SNOWFLAKE)) toggleFreeze(ctrl);
    if (frozen) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Freeze: keep this picture while the machine runs on; the recording goes on (F).");
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button(ICON_FA_CAMERA)) exportPng();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save the map and its rails as a PNG (screenshots folder).");
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button(ICON_FA_SLIDERS " View")) ImGui::OpenPopup("##heatview");
    viewBtn_[0] = ImGui::GetItemRectMin().x; viewBtn_[1] = ImGui::GetItemRectMin().y;
    viewBtn_[2] = ImGui::GetItemRectMax().x; viewBtn_[3] = ImGui::GetItemRectMax().y;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Banks shown, palette, background, trail, glow, regions, labels, rails,\n"
                          "orientation, minimap, loupe, inspector.");
    drawViewPopup();

    // Go to, then Back / Forward and Fit, on the right.
    const float fh = ImGui::GetFrameHeight();
    const float gotoW = 190.0f, tail = gotoW + 3.0f * (fh + 8.0f) + 4.0f;
    ImGui::SameLine(0.0f, 12.0f);
    const float room = ImGui::GetContentRegionAvail().x;
    if (room > tail) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room - tail);
    ImGui::SetNextItemWidth(gotoW);
    if (focusGoto_) { ImGui::SetKeyboardFocusHere(); focusGoto_ = false; }
    if (ImGui::InputTextWithHint("##goto", ICON_FA_MAGNIFYING_GLASS " $addr, bank:$addr, label",
                                 gotoText_.data(), gotoText_.size(), ImGuiInputTextFlags_EnterReturnsTrue))
        gotoPending_ = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Go to an address (2000, $D000), one in a bank (aux:$2000, lc1:$D000,\n"
                          "aux3:$1000) or a label of the loaded symbols. Ctrl+G.");
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::BeginDisabled(historyPos_ <= 0);
    if (ImGui::Button(ICON_FA_ARROW_LEFT)) historyStep(-1);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Back (Alt+Left).");
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::BeginDisabled(historyPos_ < 0 || historyPos_ + 1 >= static_cast<int>(history_.size()));
    if (ImGui::Button(ICON_FA_ARROW_RIGHT)) historyStep(1);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Forward (Alt+Right).");
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button(ICON_FA_EXPAND)) fitPending_ = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fit the whole map (Home).");
}

void MemoryHeatmap_ImGui::drawViewPopup()
{
    if (!ImGui::BeginPopup("##heatview")) return;
    ImGui::SeparatorText("Banks");
    // Same order as the columns: MAIN, AUX, then ROM & I/O at the right.
    ImGui::Checkbox("MAIN", &opt_.showMain);
    if (machine_.iie) {
        ImGui::SameLine();
        ImGui::Checkbox("AUX", &opt_.showAux);
    }
    ImGui::SameLine();
    ImGui::Checkbox("ROM & I/O", &opt_.showRom);
    if (machine_.iie) {
        if (machine_.auxBanks > 1) {
            ImGui::SetNextItemWidth(200.0f);
            char preview[48];
            if (opt_.auxChoice < 0)
                std::snprintf(preview, sizeof preview, "the CPU's (bank %d)", machine_.cpuAuxSlot);
            else
                std::snprintf(preview, sizeof preview, "bank %d", opt_.auxChoice);
            if (ImGui::BeginCombo("AUX bank", preview)) {
                if (ImGui::Selectable("Follow the CPU's bank", opt_.auxChoice < 0)) opt_.auxChoice = -1;
                for (int b = 0; b < machine_.auxBanks; ++b) {
                    const bool touched = heat_ && heat_->hasPlane(HeatData::auxPlane(b));
                    char item[48];
                    std::snprintf(item, sizeof item, "bank %d%s%s", b, b == machine_.cpuAuxSlot ? "  (CPU)" : "",
                                  touched ? "  \xe2\x80\xa2" : "");
                    if (ImGui::Selectable(item, opt_.auxChoice == b)) opt_.auxChoice = b;
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("RamWorks: which aux bank the AUX column shows (\xe2\x80\xa2 = touched).");
        }
    }

    // No "Look" section (owner, 2026-11-28: not useful): the palette, the
    // bits-set background, the trail and the glow are fixed — see
    // MemoryHeatmap_ImGui::setOptionsText.
    ImGui::SeparatorText("Map");
    ImGui::Checkbox("Regions", &opt_.regions);
    ImGui::SameLine();
    ImGui::Checkbox("Labels", &opt_.labels);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The debugger's symbols drawn on their bytes once the zoom leaves room.");
    ImGui::SameLine();
    ImGui::Checkbox("Rails", &opt_.rails);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Beside each bank (AppleWin FT): where the CPU reads (blue) and writes (red)\n"
                          "now, page by page, and in white the pages the video shows.");
    if (ImGui::Checkbox("$0000 at the bottom", &opt_.bottomUp)) fitPending_ = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Apple's memory maps and AppleWin FT put $FFFF on top; untick for a hex dump's order.");
    ImGui::SameLine();
    if (ImGui::Checkbox("Fill", &opt_.fill)) fitPending_ = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Rows fill the window's height (bytes taller than wide until the zoom\n"
                          "makes them square). Untick for square bytes, as in AppleWin FT.");

    ImGui::SeparatorText("Panels");
    ImGui::Checkbox("Minimap", &opt_.minimap);
    ImGui::SameLine();
    ImGui::Checkbox("Loupe", &opt_.loupe);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The bytes around the pointer, with values, while they are small.");
    ImGui::SameLine();
    ImGui::Checkbox("Inspector", &opt_.inspector);
    if (miniCollapsed_ && opt_.minimap) {
        ImGui::SameLine();
        if (ImGui::SmallButton("show the minimap")) miniCollapsed_ = false;
    }
    viewPopup_[0] = ImGui::GetWindowPos().x; viewPopup_[1] = ImGui::GetWindowPos().y;
    viewPopup_[2] = viewPopup_[0] + ImGui::GetWindowSize().x;
    viewPopup_[3] = viewPopup_[1] + ImGui::GetWindowSize().y;
    ImGui::EndPopup();
}

void MemoryHeatmap_ImGui::toggleFreeze(EmulationController& ctrl)
{
    if (frozen_) { frozen_.reset(); return; }
    auto st = ctrl.lockState();
    frozen_ = std::make_unique<HeatData>(heat_->freeze());
}

// ── Go to and the history ──────────────────────────────────────────────────

void MemoryHeatmap_ImGui::jumpTo(int plane, uint16_t phys, bool record)
{
    if (plane == HeatData::kRom) opt_.showRom = true;
    else if (plane == HeatData::kMain) opt_.showMain = true;
    else { opt_.showAux = true; opt_.auxChoice = plane - HeatData::kAux0; }
    if (record) {
        // Back returns to where the user was, the first time too.
        if (history_.empty() && selected_) history_.push_back({selPlane_, selPhys_});
        if (historyPos_ >= 0) history_.resize(static_cast<std::size_t>(historyPos_) + 1);
        if (history_.empty() || history_.back().plane != plane || history_.back().phys != phys)
            history_.push_back({plane, phys});
        if (history_.size() > 64) history_.erase(history_.begin());
        historyPos_ = static_cast<int>(history_.size()) - 1;
    }
    pendingJump_ = true;
    pendingLoc_ = {plane, phys};
}

void MemoryHeatmap_ImGui::historyStep(int delta)
{
    const int next = historyPos_ + delta;
    if (next < 0 || next >= static_cast<int>(history_.size())) return;
    historyPos_ = next;
    jumpTo(history_[static_cast<std::size_t>(next)].plane, history_[static_cast<std::size_t>(next)].phys, false);
}

void MemoryHeatmap_ImGui::gotoText(EmulationController& ctrl, const ResolveFn& resolve)
{
    if (!gotoPending_) return;
    gotoPending_ = false;
    std::string text(gotoText_.data());
    text.erase(0, text.find_first_not_of(" \t"));
    text.erase(text.find_last_not_of(" \t") + 1);
    if (text.empty()) return;
    DebugAddress at;
    bool ok = resolve && resolve(text, at);
    if (!ok) {   // no symbols wired: a bare hex address
        const char* s = text.c_str();
        if (*s == '$') ++s;
        char* end = nullptr;
        const unsigned long v = std::strtoul(s, &end, 16);
        ok = end != s && *end == '\0' && v <= 0xFFFF;
        at = {static_cast<uint16_t>(v), {}};
    }
    if (!ok) { say("Go to: \"" + text + "\" is neither an address nor a label."); return; }
    DebugBank bank = at.bank;
    if (bank.kind == DebugBank::Any) {
        auto st = ctrl.lockState();
        bank = st.memory().debugBank(at.addr);     // where the CPU reaches it now
    }
    const HeatData::Cell cell = HeatData::locate(at.addr, bank);
    if (cell.plane < 0) { say("Go to: " + at.text() + " is in no bank the map draws."); return; }
    jumpTo(cell.plane, cell.phys);
}

// ── Keys ───────────────────────────────────────────────────────────────────

void MemoryHeatmap_ImGui::handleKeys(EmulationController& ctrl, float w, float h)
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    // Ctrl+G: on macOS ImGui swaps Cmd and Ctrl (ConfigMacOSXBehaviors), so
    // the physical Ctrl arrives as Super — both are taken, as the Debugger does.
    const bool ctrlKey = io.KeyCtrl || (io.ConfigMacOSXBehaviors && io.KeySuper);
    if (ctrlKey && ImGui::IsKeyPressed(ImGuiKey_G)) { focusGoto_ = true; return; }
    if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) { historyStep(-1); return; }
    if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) { historyStep(1); return; }
    if (io.KeyCtrl || io.KeyAlt || io.KeySuper) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) fitPending_ = true;
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) toggleFreeze(ctrl);
    if (ImGui::IsKeyPressed(ImGuiKey_C, false)) { wantClear_ = true; frozen_.reset(); }
    const float cxw = w * 0.5f, cyh = h * 0.5f;
    if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd))
        heatview::zoomAt(xf_, layout_, w, h, cxw, cyh, 1.5f);
    if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))
        heatview::zoomAt(xf_, layout_, w, h, cxw, cyh, 1.0f / 1.5f);
    if (!selected_) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { selected_ = ranged_ = false; return; }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        request_ = {Request::Kind::Listing, HeatData::cpuAddress(selPlane_, selPhys_)};
        return;
    }
    // Arrows move as the map is drawn: up is the page above on screen.
    const int up = opt_.bottomUp ? 0x100 : -0x100;
    int delta = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) delta = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) delta = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) delta = up;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) delta = -up;
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) delta = up * 16;
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) delta = -up * 16;
    if (!delta) return;
    const int lo = heatview::firstRow(selPlane_) << 8;
    const int next = std::clamp(static_cast<int>(selPhys_) + delta, lo, 0xFFFF);
    select(selPlane_, static_cast<uint16_t>(next), true);
}

}  // namespace pom2
