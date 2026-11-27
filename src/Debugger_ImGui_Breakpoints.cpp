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

// Debugger_ImGui_Breakpoints — the breakpoint list, the "when" editor and the
// right-click menus that set breakpoints (French Touch's requests of
// 2026-11-27). Part of Debugger_ImGui; see its header for the locking rule.
//
// One list, three kinds, named as AppleWin names them: BP stops when the PC
// reaches an address, BPMR when an address is read, BPMW when it is written.
// POM2 called the last two "watches" until that day; a watch is now what the
// word says — a value looked at, in the Watch window — and never stops
// anything. A breakpoint is CREATED bare (the gutter, a right click, the add
// row) and its conditions are added afterwards, on its row.

#include "Debugger_ImGui.h"

#include "Disassembler6502.h"
#include "EmulationController.h"
#include "M6502.h"
#include "Memory.h"
#include "Pom2Theme.h"

#include "imgui.h"

#include <cstdio>
#include <string>

namespace pom2 {

namespace {

const ImVec4 kDim  {0.55f, 0.55f, 0.60f, 1.0f};
const ImVec4 kBreak{0.85f, 0.30f, 0.25f, 1.0f};

const char* const kRegisters[] = {"A", "X", "Y", "P", "SP", "PC"};
const char* const kOperators[] = {"==", "!=", "<", "<=", ">", ">="};

const char* kindName(int access)       // 0 = on the PC, else Debugger::Access
{
    switch (access) {
        case Debugger::Read:      return "BPMR";
        case Debugger::Write:     return "BPMW";
        case Debugger::ReadWrite: return "BPMRW";
        default:                  return "BP";
    }
}

/// "X == $03, beam V 100-110 H 25-64, after 3" — or nothing for a plain one.
std::string whenText(const Debugger::Condition* condition, const Debugger::Gate* gate)
{
    std::string out;
    char part[64];
    const auto add = [&](const char* text) {
        if (!out.empty()) out += ", ";
        out += text;
    };
    if (condition) {
        std::snprintf(part, sizeof part, "%s %s $%02X", kRegisters[static_cast<int>(condition->reg)],
                      kOperators[static_cast<int>(condition->compare)], condition->value);
        add(part);
    }
    if (gate && gate->valueOn) {
        std::snprintf(part, sizeof part, "value %s $%02X",
                      kOperators[static_cast<int>(gate->valueCompare)], gate->value);
        add(part);
    }
    if (gate && gate->beam) {
        std::snprintf(part, sizeof part, "beam V %u-%u H %u-%u", gate->vMin, gate->vMax,
                      gate->hMin, gate->hMax);
        add(part);
    }
    if (gate && gate->ignore) {
        std::snprintf(part, sizeof part, "after %u", gate->ignore);
        add(part);
    }
    return out;
}

}  // namespace

void Debugger_ImGui::showInListing(uint16_t addr, DebugBank bank)
{
    viewAddr_ = addr;
    cursorAddr_ = addr;
    cursorBank_ = {};
    cursorValid_ = true;
    lookingAround_ = true;
    showRequest_ = true;            // asked from another window, perhaps:
    showPending_ = true;            // render() re-applies it once it draws
    showAddr_ = addr;
    showBank_ = bank;
}

// ── The list ─────────────────────────────────────────────────────────────

void Debugger_ImGui::drawBreakpointList(EmulationController& ctrl, const Snapshot& snap)
{
    ImGui::SetNextItemWidth(86);
    ImGui::Combo("##bpkind", &bpKind_, "BP\0BPMR\0BPMW\0BPMRW\0");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("BP     stop when the PC reaches the address\n"
                          "BPMR   stop when the address is READ\n"
                          "BPMW   stop when the address is WRITTEN\n"
                          "BPMRW  either\n"
                          "A write breakpoint costs nothing. While a READ one is armed,\n"
                          "every bus read goes out of line (roughly the speed of the\n"
                          "pre-2026-08 core). Opcode fetches and soft switches count.\n"
                          "To LOOK at a value without stopping: the Watch window.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(190);
    const bool entered = ImGui::InputTextWithHint("##bpaddr", "0800, aux0:0800 or label", bpEntry_,
                                                  sizeof(bpEntry_), ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("An address or a label applies in every bank;\n"
                          "bank:address in one (aux0:0800, lc2:D000).\n"
                          "Or: click the gutter of a line, right-click a line.");
    ImGui::SameLine();
    if ((ImGui::Button("Add") || entered) && bpEntry_[0]) {
        DebugAddress location;
        if (symbols_.resolveLocation(bpEntry_, location)) {
            auto st = ctrl.lockState();
            Debugger& dbg = ctrl.debugger();
            // ADDED to what is there, as the right-click menus do: adding a
            // BP over one that has a condition would drop the condition,
            // and a BPMR over a BPMW would replace it.
            if (bpKind_ == 0) {
                bool there = false;
                for (const DebugAddress& b : dbg.bankBreakpoints()) there |= b == location;
                if (!there) dbg.addBreakpoint(location.addr, location.bank);
            } else {
                int already = Debugger::None;
                for (const Debugger::Watch& w : dbg.watchpoints())
                    if (w.addr == location.addr && w.bank == location.bank) already = w.access;
                dbg.setWatchpoint(location.addr, static_cast<Debugger::Access>(already | bpKind_),
                                  location.bank);
            }
            ctrl.syncDebugHook();          // installs Memory's diversions too
            bpStatus_.clear();
            bpEntry_[0] = '\0';
        } else {
            bpStatus_ = "Unknown address or label.";
        }
    }
    if (!snap.breakpoints.empty() || !snap.watchpoints.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Clear all")) {
            auto st = ctrl.lockState();
            ctrl.debugger().clearBreakpoints();
            ctrl.debugger().clearWatchpoints();
            ctrl.syncDebugHook();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset passes")) {
            auto st = ctrl.lockState();
            ctrl.debugger().resetPassCounts();
        }
    }
    if (!bpStatus_.empty()) ImGui::TextColored(kBreak, "%s", bpStatus_.c_str());

    if (snap.breakpoints.empty() && snap.watchpoints.empty()) {
        ImGui::TextColored(kDim, "none - click the gutter of a line, or right-click a line");
        return;
    }
    if (!ImGui::BeginTable("##bplist", 6, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
        return;
    ImGui::TableSetupColumn("##x");
    ImGui::TableSetupColumn("Kind");
    ImGui::TableSetupColumn("Address");
    ImGui::TableSetupColumn("Value");
    ImGui::TableSetupColumn("Passes");
    ImGui::TableSetupColumn("When", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    // One row. `access` 0 = a breakpoint on the PC.
    const auto row = [&](int access, const DebugAddress& at) {
        const bool memory = access != 0;
        const Debugger::GateMap& gates = memory ? snap.wpGates : snap.bpGates;
        const auto gate = gates.find(at);
        const auto condition = memory ? snap.conditions.end() : snap.conditions.find(at);
        ImGui::PushID(at.text().c_str());
        ImGui::PushID(access);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::SmallButton("x")) {
            auto st = ctrl.lockState();
            if (memory) ctrl.debugger().setWatchpoint(at.addr, Debugger::None, at.bank);
            else        ctrl.debugger().removeBreakpoint(at.addr, at.bank);
            ctrl.syncDebugHook();
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(kindName(access));
        ImGui::TableNextColumn();
        const std::string label = symbols_.label(at.addr, at.bank);
        char text[160];
        std::snprintf(text, sizeof text, "%s%s%s", at.text().c_str(), label.empty() ? "" : "  ",
                      label.c_str());
        if (ImGui::Selectable(text)) showInListing(at.addr, at.bank);
        ImGui::TableNextColumn();
        // The byte there NOW, as the CPU sees it — French Touch: "the WATCH
        // window does not show the value even when it stops the code".
        if (!memory) ImGui::TextUnformatted("");
        else if (at.bank.kind == DebugBank::Any || at.bank == snap.banks[at.addr])
            ImGui::Text("$%02X", snap.memory[at.addr]);
        else {
            ImGui::TextColored(kDim, "--");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("That bank is not the one mapped at this address now.");
        }
        ImGui::TableNextColumn();
        ImGui::Text("%llu", static_cast<unsigned long long>(gate == gates.end() ? 0 : gate->second.passes));
        ImGui::TableNextColumn();
        const std::string when = whenText(condition == snap.conditions.end() ? nullptr : &condition->second,
                                          gate == gates.end() ? nullptr : &gate->second.gate);
        if (when.empty()) ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        if (ImGui::Selectable(when.empty() ? "always (click to add a condition)" : when.c_str()))
            openGateEditor(snap, memory, at);
        if (when.empty()) ImGui::PopStyleColor();
        ImGui::PopID();
        ImGui::PopID();
    };
    for (const DebugAddress& at : snap.breakpoints) row(0, at);
    for (const Debugger::Watch& w : snap.watchpoints) row(w.access, {w.addr, w.bank});
    ImGui::EndTable();
}

// ── "When": the conditions of one breakpoint ─────────────────────────────

void Debugger_ImGui::openGateEditor(const Snapshot& snap, bool memory, DebugAddress at)
{
    edit_ = {};
    edit_.memory = memory;
    edit_.at = at;
    edit_.request = true;
    const Debugger::GateMap& gates = memory ? snap.wpGates : snap.bpGates;
    const auto gate = gates.find(at);
    if (gate != gates.end()) { edit_.gate = gate->second.gate; edit_.passes = gate->second.passes; }
    if (memory) {
        for (const Debugger::Watch& w : snap.watchpoints)
            if (w.addr == at.addr && w.bank == at.bank) edit_.access = w.access;
    } else {
        const auto condition = snap.conditions.find(at);
        edit_.conditionOn = condition != snap.conditions.end();
        if (edit_.conditionOn) edit_.condition = condition->second;
    }
}

void Debugger_ImGui::drawGateEditor(EmulationController& ctrl)
{
    if (edit_.request) { ImGui::OpenPopup("##bpwhen"); edit_.request = false; }
    if (!ImGui::BeginPopup("##bpwhen")) return;
    {
    ScopedMonoFont mono;          // this block: the font goes before EndPopup()
    Debugger::Gate& g = edit_.gate;
    ImGui::Text("%s  %s", edit_.memory ? "Memory breakpoint" : "Breakpoint", edit_.at.text().c_str());
    ImGui::TextColored(kDim, "Stops when ALL the ticked conditions hold.");
    ImGui::Separator();

    const auto hex = [](const char* id, ImGuiDataType type, void* value, const char* format) {
        ImGui::SetNextItemWidth(60);
        ImGui::InputScalar(id, type, value, nullptr, nullptr, format, ImGuiInputTextFlags_CharsHexadecimal);
    };
    const auto number = [](const char* id, ImGuiDataType type, void* value, float width = 52.0f) {
        ImGui::SetNextItemWidth(width);
        ImGui::InputScalar(id, type, value, nullptr, nullptr, "%u");
    };
    const auto comparison = [](const char* id, Debugger::Compare& how) {
        int index = static_cast<int>(how);
        ImGui::SetNextItemWidth(56);
        if (ImGui::Combo(id, &index, "==\0!=\0<\0<=\0>\0>=\0")) how = static_cast<Debugger::Compare>(index);
    };

    if (edit_.memory) {
        ImGui::TextUnformatted("On a");
        ImGui::SameLine(); ImGui::RadioButton("read (BPMR)", &edit_.access, Debugger::Read);
        ImGui::SameLine(); ImGui::RadioButton("write (BPMW)", &edit_.access, Debugger::Write);
        ImGui::SameLine(); ImGui::RadioButton("either", &edit_.access, Debugger::ReadWrite);
        ImGui::Checkbox("The byte read or written", &g.valueOn);
        ImGui::BeginDisabled(!g.valueOn);
        ImGui::SameLine(); comparison("##vcmp", g.valueCompare);
        ImGui::SameLine(); hex("##vval", ImGuiDataType_U8, &g.value, "%02X");
        ImGui::EndDisabled();
    } else {
        ImGui::Checkbox("Register", &edit_.conditionOn);
        ImGui::BeginDisabled(!edit_.conditionOn);
        int reg = static_cast<int>(edit_.condition.reg);
        ImGui::SameLine(); ImGui::SetNextItemWidth(56);
        if (ImGui::Combo("##reg", &reg, "A\0X\0Y\0P\0SP\0PC\0"))
            edit_.condition.reg = static_cast<Debugger::Register>(reg);
        ImGui::SameLine(); comparison("##rcmp", edit_.condition.compare);
        ImGui::SameLine(); hex("##rval", ImGuiDataType_U16, &edit_.condition.value, "%X");
        ImGui::EndDisabled();
    }

    ImGui::Checkbox("The beam is within", &g.beam);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("V = line in the frame (0-191 the picture, then vertical blanking,\n"
                          "to 261 NTSC / 311 PAL). H = cycle in the 65-cycle line (0-24\n"
                          "horizontal blanking, 25-64 the 40 bytes) - the Debugger's Beam row.\n"
                          "%s\n"
                          "A minimum above its maximum wraps (V 300 to 10).",
                          edit_.memory ? "Judged at the cycle of the ACCESS itself."
                                       : "Judged at the instruction's first cycle.");
    ImGui::BeginDisabled(!g.beam);
    ImGui::TextUnformatted("  V"); ImGui::SameLine(); number("##vmin", ImGuiDataType_U16, &g.vMin);
    ImGui::SameLine(); ImGui::TextUnformatted("to"); ImGui::SameLine(); number("##vmax", ImGuiDataType_U16, &g.vMax);
    ImGui::SameLine(); ImGui::TextUnformatted("  H"); ImGui::SameLine(); number("##hmin", ImGuiDataType_U8, &g.hMin);
    ImGui::SameLine(); ImGui::TextUnformatted("to"); ImGui::SameLine(); number("##hmax", ImGuiDataType_U8, &g.hMax);
    ImGui::TextUnformatted("  ");
    ImGui::SameLine();
    if (ImGui::SmallButton("the picture"))        { g.vMin = 0;   g.vMax = 191; g.hMin = 25; g.hMax = 64; }
    ImGui::SameLine();
    if (ImGui::SmallButton("horizontal blanking")) { g.vMin = 0;   g.vMax = 311; g.hMin = 0;  g.hMax = 24; }
    ImGui::SameLine();
    if (ImGui::SmallButton("vertical blanking"))   { g.vMin = 192; g.vMax = 311; g.hMin = 0;  g.hMax = 64; }
    ImGui::EndDisabled();
    if (g.vMin > 311) g.vMin = 311;
    if (g.vMax > 311) g.vMax = 311;
    if (g.hMin > 64)  g.hMin = 64;
    if (g.hMax > 64)  g.hMax = 64;

    ImGui::TextUnformatted("Let the first"); ImGui::SameLine();
    number("##ignore", ImGuiDataType_U32, &g.ignore, 70.0f);
    ImGui::SameLine(); ImGui::TextUnformatted("passes go by");
    ImGui::TextColored(kDim, "%llu passes so far; applying starts the count over.",
                       static_cast<unsigned long long>(edit_.passes));
    ImGui::Separator();
    if (ImGui::Button("Apply")) {
        auto st = ctrl.lockState();
        Debugger& dbg = ctrl.debugger();
        // An 8-bit register never holds more than $FF: "A == 100" typed for
        // a hundred would be a breakpoint that can never stop.
        // Clamped, not masked: "$100" must not turn into "$00".
        if (edit_.condition.reg != Debugger::Register::PC && edit_.condition.value > 0xFF)
            edit_.condition.value = 0xFF;
        if (edit_.memory) {
            // Access first: it re-creates the entry; then its gate.
            dbg.setWatchpoint(edit_.at.addr, static_cast<Debugger::Access>(edit_.access), edit_.at.bank);
            dbg.setWatchGate(edit_.at, g);
        } else {
            dbg.setCondition(edit_.at, edit_.conditionOn, edit_.condition, st.cpu());
            dbg.setBreakpointGate(edit_.at, g);
        }
        ctrl.syncDebugHook();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ── Right click ──────────────────────────────────────────────────────────

bool Debugger_ImGui::operandAddress(const Snapshot& snap, uint16_t addr, int len,
                                    const std::string& mnemonic, uint16_t& out)
{
    const uint8_t op = snap.memory[addr];
    const uint8_t b1 = snap.memory[(addr + 1) & 0xFFFF], b2 = snap.memory[(addr + 2) & 0xFFFF];
    if (len == 3) {
        // BBR / BBS: zero page + relative. The zero-page byte is the operand.
        if ((op & 0x0F) == 0x0F && snap.isa == Cpu6502Set::Superset65C02) { out = b1; return true; }
        out = static_cast<uint16_t>(b1 | (b2 << 8));
        return true;
    }
    if (len != 2 || mnemonic.find('#') != std::string::npos) return false;
    const bool branch = (op & 0x1F) == 0x10 || (op == 0x80 && snap.isa != Cpu6502Set::Nmos6502);
    // A branch names the place it goes to; anything else, a zero-page byte
    // (direct, indexed or the pointer of an indirect).
    out = branch ? static_cast<uint16_t>(addr + 2 + static_cast<int8_t>(b1)) : b1;
    return true;
}

// Right click on a line of the listing. The line's own address first, with
// the three kinds of breakpoint side by side — a line is code to stop on, and
// in self-modifying code it is also a byte somebody writes; then where to run
// to; then the same choices for the address its operand names.
void Debugger_ImGui::drawLineMenu(EmulationController& ctrl, const Snapshot& snap, uint16_t addr,
                                  int len, const std::string& mnemonic, bool isBp, bool wildcard)
{
    const DebugBank bank = snap.banks[addr];
    char item[96];
    std::snprintf(item, sizeof item, isBp ? "Remove the breakpoint at $%04X" : "Break at $%04X  (BP)", addr);
    if (ImGui::MenuItem(item)) {
        auto st = ctrl.lockState();
        if (isBp) ctrl.debugger().removeBreakpoint(addr, wildcard ? DebugBank{} : bank);
        else      ctrl.debugger().addBreakpoint(addr, bank);
        ctrl.syncDebugHook();
    }
    std::snprintf(item, sizeof item, "Break when $%04X is read  (BPMR)", addr);
    if (ImGui::MenuItem(item)) applyAddressAction(ctrl, DebugAddressAction::BreakOnRead, addr);
    std::snprintf(item, sizeof item, "Break when $%04X is written  (BPMW)", addr);
    if (ImGui::MenuItem(item)) applyAddressAction(ctrl, DebugAddressAction::BreakOnWrite, addr);
    if (isBp && ImGui::MenuItem("Conditions of the breakpoint..."))
        openGateEditor(snap, false, {addr, wildcard ? DebugBank{} : bank});
    ImGui::Separator();
    if (ImGui::MenuItem("Run to here")) ctrl.debugRunToCursor(addr, bank);
    if (ImGui::MenuItem("Set the PC here", nullptr, false, !snap.running)) {
        auto st = ctrl.lockState();
        st.cpu().setProgramCounter(addr);
        ctrl.noteChangedByHand();           // not something a replay reproduces
    }
    uint16_t target = 0;
    if (!operandAddress(snap, addr, len, mnemonic, target)) return;
    ImGui::Separator();
    const std::string name = symbols_.label(target, snap.banks[target]);
    std::snprintf(item, sizeof item, "Operand $%04X %.40s", target, name.c_str());
    if (ImGui::BeginMenu(item)) {
        // Asked from the listing: the Watch window opens, the listing stays.
        applyAddressAction(ctrl, debugAddressMenuItems(target), target, "Debugger");
        ImGui::EndMenu();
    }
}

void Debugger_ImGui::applyAddressAction(EmulationController& ctrl, DebugAddressAction action,
                                        uint16_t addr, const char* requester)
{
    const auto arm = [&](int access) {
        auto st = ctrl.lockState();
        const int already = ctrl.debugger().watchpointAt(addr);
        ctrl.debugger().setWatchpoint(addr, static_cast<Debugger::Access>(already | access));
        ctrl.syncDebugHook();
    };
    const auto watch = [&](WatchEntry::Format format) {
        if (watches_.add({{addr, {}}, format})) watchRefs_ = {};
        watchOpenRequest_ = true;
        // The Watch window appears; the window the user asked from keeps
        // its tab (three frames: a new tab is selected the frame after it
        // appears, and the dock node follows the focus one frame later).
        returnFocusTo_ = requester ? requester : "";
        returnFocusFrames_ = returnFocusTo_.empty() ? 0 : 3;
    };
    switch (action) {
        case DebugAddressAction::BreakAtPc: {   // in every bank: the menu knows no bank
            auto st = ctrl.lockState();
            // Not over one that is there — in ANY bank: the gutter adds a
            // bank-qualified one, and an every-bank breakpoint on top of it
            // would stop on every pass whatever its condition says.
            // (One qualified for ANOTHER bank does not cover this address
            // as it is mapped now: the breakpoint asked for is then added.)
            const DebugBank mapped = st.memory().debugBank(addr);
            bool there = false;
            for (const DebugAddress& b : ctrl.debugger().bankBreakpoints())
                there |= b.addr == addr && (b.bank.kind == DebugBank::Any || b.bank == mapped);
            if (!there) ctrl.debugger().addBreakpoint(addr);
            ctrl.syncDebugHook();
            break;
        }
        case DebugAddressAction::BreakOnRead:   arm(Debugger::Read);  break;
        case DebugAddressAction::BreakOnWrite:  arm(Debugger::Write); break;
        case DebugAddressAction::BreakOnAccess: arm(Debugger::ReadWrite); break;
        case DebugAddressAction::Watch:         watch(WatchEntry::Format::Byte); break;
        case DebugAddressAction::WatchWord:     watch(WatchEntry::Format::Word); break;
        case DebugAddressAction::ShowInListing: showInListing(addr); break;
        case DebugAddressAction::None:          break;
    }
}

} // namespace pom2
