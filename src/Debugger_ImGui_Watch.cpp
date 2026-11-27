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

// Debugger_ImGui_Watch — the Watch window: addresses, and the values they
// hold right now (French Touch's request, 2026-11-27; DebuggerWatchList.h).
//
// It LOOKS and never stops: no breakpoint is involved, so nothing here arms
// the debugger or takes the CPU off its fast loop. One short lock per frame
// reads each entry through the CPU's view (`Memory::peekCpuView`, which
// throws no soft switch).
//
// A value is highlighted when it changed: while the machine runs, since the
// previous frame of the interface; once it is stopped, since the stop BEFORE
// this one — "break at A, Run, break at B" shows what moved between A and B,
// and a step shows what that instruction changed.

#include "Debugger_ImGui.h"

#include "EmulationController.h"
#include "Memory.h"
#include "Pom2Theme.h"

#include "imgui.h"
#include "imgui_internal.h"     // FindWindowByName / DockId: where the Debugger is docked

#include <cstdio>

namespace pom2 {

void Debugger_ImGui::renderWatch(EmulationController& ctrl, bool* open)
{
    // Hand the focus back to the window "Watch $xxxx" was asked from (see
    // applyAddressAction). Before anything else: it must run on the frames
    // AFTER the one that opened this window, whether or not it draws.
    if (returnFocusFrames_ > 0 && !watchOpenRequest_) {
        --returnFocusFrames_;
        ImGui::SetWindowFocus(returnFocusTo_.c_str());
    }
    const bool byRequest = watchOpenRequest_;
    if (watchOpenRequest_ && open) *open = true;
    watchOpenRequest_ = false;
    if (open && !*open) { watchWasOpen_ = false; return; }
    // Reopened: "changed since the previous stop" must not mean "since the
    // stop before the window was closed".
    if (!watchWasOpen_) watchRefs_ = {};
    watchWasOpen_ = true;

    // The values are read, and the stops recorded, BEFORE the window is
    // begun: where it is a tab behind the Debugger its Begin() says "not
    // visible", and a stop that went by unrecorded made the highlight
    // compare with a stop of long ago.
    const std::vector<WatchEntry>& entries = watches_.entries();
    std::vector<WatchSeen> now(entries.size());
    uint64_t cycles = 0;
    {
        auto st = ctrl.lockState();
        const Memory& mem = st.memory();
        cycles = mem.getCycleCounter();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const DebugAddress& at = entries[i].at;
            now[i].lo = mem.peekCpuView(at.addr);
            now[i].hi = mem.peekCpuView(static_cast<uint16_t>(at.addr + 1));
            now[i].mapped = at.bank.kind == DebugBank::Any || mem.debugBank(at.addr) == at.bank;
        }
    }
    const bool running = ctrl.getMode() == EmulationController::Mode::Running;
    WatchRefs& refs = watchRefs_;
    if (refs.frame.size() != now.size()) {          // the list changed: start over
        refs.frame = refs.before = refs.stopped = now;
        refs.stopCycles = cycles;
    }
    if (!running && cycles != refs.stopCycles) {    // stopped somewhere new
        refs.before = refs.stopped;
        refs.stopped = now;
        refs.stopCycles = cycles;
    }
    const std::vector<WatchSeen>& reference = running ? refs.frame : refs.before;

    ImGui::SetNextWindowSize(ImVec2(440, 300), ImGuiCond_FirstUseEver);
    // A layout saved before this window existed has no place for it, and it
    // would open floating over the screen. First use only (no saved
    // settings for "Watch"): it joins the Debugger's dock node as a tab.
    if (const ImGuiWindow* debuggerWindow = ImGui::FindWindowByName("Debugger"))
        if (debuggerWindow->DockId != 0)
            ImGui::SetNextWindowDockID(debuggerWindow->DockId, ImGuiCond_FirstUseEver);
    // Opened by "Watch $xxxx" from a menu elsewhere: it does not take the
    // focus; the tab a dock node gives a new window all the same is handed
    // back by the lines at the top of this function.
    if (byRequest) keepBehindFrames_ = 4;
    const bool shown = ImGui::Begin("Watch", open, byRequest ? ImGuiWindowFlags_NoFocusOnAppearing : 0);
    // ...and before it is shown at all. A dock node selects a tab that
    // appears in it (ImGuiTabBarFlags_AutoSelectNewTabs) by queuing it in
    // NextSelectedTabId, applied the NEXT frame; the focus handed back then
    // took it away again, so the Watch tab flashed over the Memory viewer
    // where they share a node. The queue is emptied here, after this frame's
    // tab bar ran and before the next one applies it — only where the node
    // shows something else: alone in its node, Watch must be shown.
    if (keepBehindFrames_ > 0) {
        --keepBehindFrames_;
        if (ImGuiDockNode* node = ImGui::GetWindowDockNode();
            node && node->TabBar && node->Windows.Size > 1 &&
            node->TabBar->NextSelectedTabId == ImGui::GetCurrentWindow()->TabId)
            node->TabBar->NextSelectedTabId = 0;
    }
    if (!shown) {
        refs.frame = now;
        ImGui::End();
        return;
    }

    // ── Add ──────────────────────────────────────────────────────────────
    ImGui::SetNextItemWidth(190);
    const bool entered = ImGui::InputTextWithHint("##watchaddr", "10, C054, aux0:0800 or label",
                                                  watchEntry_, sizeof(watchEntry_),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(92);
    ImGui::Combo("##watchformat", &watchFormat_, "byte\0word\0binary\0");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("byte    one byte, in hexadecimal\n"
                          "word    16 bits, low byte first: a zero-page pointer\n"
                          "binary  one byte, bit 7 on the left");
    ImGui::SameLine();
    if ((ImGui::Button("Add") || entered) && watchEntry_[0]) {
        DebugAddress at;
        if (!symbols_.resolveLocation(watchEntry_, at)) watchStatus_ = "Unknown address or label.";
        else if (!watches_.add({at, static_cast<WatchEntry::Format>(watchFormat_)}))
            watchStatus_ = "Already in the list (or the list is full).";
        else { watchStatus_.clear(); watchEntry_[0] = '\0'; }
        // The list just grew under the values read for THIS frame (`now`):
        // draw it from the next one.
        watchRefs_ = {};
        ImGui::End();
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The values the CPU sees at these addresses, live. Nothing here\n"
                          "stops the machine: to stop when an address is read or written,\n"
                          "right-click its address (or a line of the listing) for a memory\n"
                          "breakpoint. Highlighted: changed since the previous stop, or,\n"
                          "while running, since the previous frame.\n"
                          "Right-click a line of the Debugger's listing to add its operand.");
    if (!watchStatus_.empty()) ImGui::TextColored(ImVec4(0.85f, 0.30f, 0.25f, 1.0f), "%s", watchStatus_.c_str());

    if (entries.empty()) {
        ImGui::TextDisabled("Nothing watched. Type an address above, or right-click a line of\n"
                            "the Debugger's listing: \"Watch $xxxx\".");
        ImGui::End();
        return;
    }

    std::size_t removeAt = entries.size();
    int reformat = -1;
    std::size_t reformatAt = 0;
    {
    ScopedMonoFont mono;          // this block: the font goes before ImGui::End()
    if (ImGui::BeginTable("##watches", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("##x");
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("Decimal");
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const WatchEntry& e = entries[i];
            const WatchSeen& v = now[i];
            const bool word = e.format == WatchEntry::Format::Word;
            const bool changed = i < reference.size() && v.mapped &&
                (v.lo != reference[i].lo || (word && v.hi != reference[i].hi));
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) removeAt = i;
            ImGui::TableNextColumn();
            // Every bank ("*") is the common case: print the bare address.
            char where[32];
            if (e.at.bank.kind == DebugBank::Any) std::snprintf(where, sizeof where, "$%04X", e.at.addr);
            else std::snprintf(where, sizeof where, "%s", e.at.text().c_str());
            if (ImGui::Selectable(where)) showInListing(e.at.addr);
            if (ImGui::BeginPopupContextItem("##watchrow")) {
                if (ImGui::MenuItem("Byte", nullptr, e.format == WatchEntry::Format::Byte))     { reformat = 0; reformatAt = i; }
                if (ImGui::MenuItem("16-bit word", nullptr, word))                              { reformat = 1; reformatAt = i; }
                if (ImGui::MenuItem("Binary", nullptr, e.format == WatchEntry::Format::Binary)) { reformat = 2; reformatAt = i; }
                ImGui::Separator();
                const auto arm = [&](Debugger::Access access) {
                    auto st = ctrl.lockState();
                    // Added to what is armed there already, in THAT bank.
                    int already = Debugger::None;
                    for (const Debugger::Watch& w : ctrl.debugger().watchpoints())
                        if (w.addr == e.at.addr && w.bank == e.at.bank) already = w.access;
                    ctrl.debugger().setWatchpoint(e.at.addr,
                        static_cast<Debugger::Access>(already | access), e.at.bank);
                    ctrl.syncDebugHook();
                };
                if (ImGui::MenuItem("Break when it is written  (BPMW)")) arm(Debugger::Write);
                if (ImGui::MenuItem("Break when it is read  (BPMR)"))    arm(Debugger::Read);
                if (ImGui::MenuItem("Show in the listing")) showInListing(e.at.addr);
                ImGui::Separator();
                if (ImGui::MenuItem("Remove")) removeAt = i;
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            if (!v.mapped) {
                ImGui::TextDisabled("--");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("That bank is not the one mapped at this address now.");
            } else if (changed) {
                ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1.0f), "%s",
                                   DebuggerWatchList::value(e.format, v.lo, v.hi).c_str());
            } else {
                ImGui::TextUnformatted(DebuggerWatchList::value(e.format, v.lo, v.hi).c_str());
            }
            ImGui::TableNextColumn();
            if (v.mapped) ImGui::Text("%u", word ? static_cast<unsigned>(v.lo | (v.hi << 8)) : v.lo);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(symbols_.label(e.at.addr, e.at.bank).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    }
    refs.frame = now;
    if (reformat >= 0) watches_.setFormat(reformatAt, static_cast<WatchEntry::Format>(reformat));
    if (removeAt < entries.size()) { watches_.remove(removeAt); watchRefs_ = {}; }
    ImGui::End();
}

} // namespace pom2
