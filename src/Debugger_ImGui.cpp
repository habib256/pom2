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

// Debugger_ImGui — see the header for the design and the locking rule.

#include "Debugger_ImGui.h"

#include "Disassembler6502.h"
#include "EmulationController.h"
#include "M6502.h"
#include "Memory.h"
#include "NativeFileDialog.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>

namespace pom2 {

namespace {

/// The 6502 status byte, spelled the way a 6502 programmer reads it: an
/// upper-case letter for a set flag, a dash for a clear one. NV-BDIZC.
std::string flagsText(uint8_t p)
{
    static const char* kNames = "NV-BDIZC";
    std::string out(8, '-');
    for (int bit = 7; bit >= 0; --bit) {
        const int i = 7 - bit;
        if (kNames[i] == '-') { out[i] = '-'; continue; }
        out[i] = (p & (1u << bit)) ? kNames[i]
                                   : static_cast<char>(std::tolower(kNames[i]));
    }
    return out;
}

ImVec4 kBreakColour{0.85f, 0.30f, 0.25f, 1.0f};
ImVec4 kPcColour   {0.95f, 0.78f, 0.25f, 1.0f};
ImVec4 kDimColour  {0.55f, 0.55f, 0.60f, 1.0f};

}  // namespace

void Debugger_ImGui::render(EmulationController& ctrl, bool* open)
{
    if (open && !*open) return;

    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Debugger", open)) { ImGui::End(); return; }

    // ── One snapshot, one lock, then draw ────────────────────────────────
    // Everything the panel needs is copied out here. The 64 KiB memcpy is the
    // expensive part and it is deliberate: laying out the disassembly under
    // the lock would hold the CPU worker still for the length of a repaint.
    Snapshot snap;
    {
        // lockState(), not a bare stateMutex: this reads the CPU and Memory,
        // and CLAUDE.md's rule is that those are reached through the RAII
        // handle so they cannot be touched without the lock having been taken.
        auto st       = ctrl.lockState();
        M6502& cpu    = st.cpu();
        Memory& mem   = st.memory();
        snap.a        = cpu.getAccumulator();
        snap.x        = cpu.getXRegister();
        snap.y        = cpu.getYRegister();
        snap.p        = cpu.getStatusRegister();
        snap.sp       = cpu.getStackPointer();
        snap.pc       = cpu.getProgramCounter();
        snap.cmos     = cpu.effectiveCpuMode() == M6502::CpuMode::CMOS;
        snap.halted   = cpu.isHalted();
        snap.hit      = ctrl.debugger().lastHit();
        snap.breakpoints = ctrl.debugger().bankBreakpoints();
        snap.pcBank = mem.debugBank(snap.pc);
        snap.watchpoints = ctrl.debugger().watchpoints();
        for (const auto& c : ctrl.debugger().conditions()) snap.conditions[{c.first,{}}]=c.second;
        for (const auto& c : ctrl.debugger().bankConditions()) snap.conditions[c.first]=c.second;
        snap.profiling = ctrl.debugger().profiling();
        snap.historyEnabled = ctrl.debugger().historyEnabled();
        snap.profile = ctrl.debugger().profile();
        snap.history = ctrl.debugger().history();
        const pom2::OsCallTrace& os = ctrl.debugger().osCalls();
        snap.trace = os.kinds();
        if (os.emitted() != osEmitted_) {
            osEmitted_ = os.emitted();
            osLines_.assign(os.lines().begin(), os.lines().end());
        }
        // The PAGED view, not mem.data(). The flat mirror is main-bank only,
        // so on a //e running out of aux, out of a RamWorks bank or out of
        // language-card RAM the panel disassembled bytes the CPU never
        // fetches — and every breakpoint the user then set from that listing
        // landed at a non-instruction boundary. snapshotCpuView() resolves the
        // paging without touching a soft switch (Memory.h).
        snap.memory.assign(0x10000, 0);
        mem.snapshotCpuView(snap.memory.data());
        snap.banks.resize(0x10000);
        for (unsigned a=0; a<0x10000; ++a) snap.banks[a]=mem.debugBank(static_cast<uint16_t>(a));
    }
    snap.running = ctrl.getMode() == EmulationController::Mode::Running;

    if (followPc_) viewAddr_ = snap.pc;

    // F7 / F8 while the window is open. Handled HERE rather than in
    // MainWindow's global key routing for two reasons: the shortcuts belong
    // to this panel and should not fire when it is closed, and MainWindow is
    // the god-object the file-size ratchet is holding still — a feature that
    // can keep its own keys out of it should.
    if (!ImGui::GetIO().WantCaptureKeyboard || ImGui::IsWindowFocused()) {
        if (ImGui::IsKeyPressed(ImGuiKey_F7, /*repeat=*/false))
            ctrl.debugStepInstruction();
        else if (ImGui::IsKeyPressed(ImGuiKey_F8, /*repeat=*/false))
            ctrl.debugStepOver();
    }

    drawStopBanner(snap);
    drawControls(ctrl, snap);
    ImGui::Separator();
    drawRegisters(snap);
    ImGui::Separator();
    drawAssembler(ctrl, snap);

    if (ImGui::CollapsingHeader("Symbols")) {
        ImGui::SetNextItemWidth(420);
        ImGui::InputText("File##symbols", symbolPath_, sizeof(symbolPath_));
        ImGui::SameLine();
        if (nativeFileDialogAvailable()) {
            if (ImGui::Button("Browse##symbols")) {
                std::string picked;
                if (nativeFileDialog(false, "Load debugger symbols", "sym,lbl,txt", "", "", picked) == FileDialogResult::Selected) {
                    if (picked.size() < sizeof(symbolPath_))
                        std::snprintf(symbolPath_, sizeof(symbolPath_), "%s", picked.c_str());
                    else symbolStatus_ = "The selected path is too long.";
                }
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Load symbols")) {
            // File I/O stays outside stateMutex. Bound allocation before read.
            std::ifstream input(std::filesystem::u8path(symbolPath_), std::ios::binary | std::ios::ate);
            const auto size = input ? input.tellg() : std::streampos{-1};
            if (size < 0 || size > 4 * 1024 * 1024) {
                symbolStatus_ = "Cannot read symbol file (maximum 4 MiB).";
            } else {
                std::string text(static_cast<size_t>(size), '\0');
                input.seekg(0);
                if (!text.empty()) input.read(text.data(), static_cast<std::streamsize>(text.size()));
                if (!input) symbolStatus_ = "Symbol file read failed.";
                else if (symbols_.load(text, symbolStatus_))
                    symbolStatus_ = std::to_string(symbols_.size()) + " symbols loaded.";
            }
        }
        ImGui::TextWrapped("AppleWin .sym, VICE/cc65 labels and NAME = $address. Bank-qualified symbols are supported (entry = aux1:$0800). Use labels in the breakpoint field.");
        ImGui::TextWrapped("%s", symbolStatus_.c_str());
    }

    drawSource(ctrl,snap);

    if (ImGui::CollapsingHeader("Profiler and execution history")) {
        if (ImGui::Checkbox("Profile CPU steps", &snap.profiling)) {
            auto st = ctrl.lockState();
            ctrl.debugger().setProfiling(snap.profiling);
            ctrl.syncDebugHook();
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Keep last 256 steps", &snap.historyEnabled)) {
            auto st = ctrl.lockState();
            ctrl.debugger().setHistoryEnabled(snap.historyEnabled);
            ctrl.syncDebugHook();
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear measurements")) {
            auto st = ctrl.lockState();
            ctrl.debugger().clearMeasurements();
        }
        ImGui::TextWrapped("Cycles include IRQ/NMI entry steps. History shows registers after each step. Reset and rewind clear measurements.");
        ImGui::BeginChild("##measurements", ImVec2(0, 140), true);
        if (ImGui::BeginTable("##profile", 4)) {
            ImGui::TableSetupColumn("Address"); ImGui::TableSetupColumn("Symbol");
            ImGui::TableSetupColumn("Visits"); ImGui::TableSetupColumn("Cycles");
            ImGui::TableHeadersRow();
            for (const auto& entry : snap.profile) {
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::TextUnformatted(DebugAddress{entry.pc,entry.bank}.text().c_str()); ImGui::TableNextColumn();
                ImGui::TextUnformatted(symbols_.label(entry.pc,entry.bank).c_str()); ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(entry.visits)); ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(entry.cycles));
            }
            ImGui::EndTable();
        }
        for (const auto& entry : snap.history)
            ImGui::Text("%s -> $%04X A:%02X X:%02X Y:%02X P:%02X SP:%02X (%d cycles)",
                DebugAddress{entry.pc,entry.bank}.text().c_str(), entry.nextPc, entry.a, entry.x, entry.y, entry.p, entry.sp, entry.cycles);
        ImGui::EndChild();
    }

    drawOsCalls(ctrl, snap);

    // Disassembly left, breakpoints right.
    const float rightWidth = 270.0f;
    ImGui::BeginChild("##disasm",
                      ImVec2(ImGui::GetContentRegionAvail().x - rightWidth - 8.0f, 0),
                      true);
    drawDisassembly(ctrl, snap);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##bplist", ImVec2(rightWidth, 0), true);
    drawBreakpointList(ctrl, snap);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    drawWatchpointList(ctrl, snap);
    ImGui::EndChild();

    ImGui::End();
}

// ── ProDOS MLI / SmartPort / Pascal BIOS / CP/M BDOS calls ───────────────

void Debugger_ImGui::drawOsCalls(EmulationController& ctrl, Snapshot& snap)
{
    if (!ImGui::CollapsingHeader("OS calls and traces")) return;
    pom2::OsTraceKinds& k = snap.trace;
    bool changed = ImGui::Checkbox("ProDOS MLI", &k.mli);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("SmartPort", &k.smartPort);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Disk II head", &k.disk);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Monitor", &k.monitor);
    changed |= ImGui::Checkbox("Pascal BIOS", &k.pascalBios);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Applecorn MOS", &k.applecorn);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("CP/M-65", &k.cpm65);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("CP/M BDOS (card Z80)", &k.bdos);
    if (changed) {
        auto st = ctrl.lockState();
        ctrl.setOsCallTrace(k);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear##oscalls")) {
        auto st = ctrl.lockState();
        ctrl.debugger().osCalls().clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy##oscalls")) {
        std::string all;
        for (const auto& l : osLines_) all += l + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::TextWrapped("Each call with its result, at its caller's address (Pascal: the BIOS entry only — it returns through the p-machine). The 6502 traces run the CPU on its debugged loop while one is on.");
    ImGui::BeginChild("##oscalls", ImVec2(0, 160), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(osLines_.size()));
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
            ImGui::TextUnformatted(osLines_[static_cast<std::size_t>(i)].c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

// ── Why are we stopped? ──────────────────────────────────────────────────

void Debugger_ImGui::drawStopBanner(const Snapshot& snap)
{
    if (snap.halted) {
        ImGui::TextColored(kBreakColour,
            "CPU halted by STP ($DB) — only a reset clears it.");
        return;
    }
    if (!snap.hit.valid()) {
        ImGui::TextColored(kDimColour, snap.running ? "Running." : "Stopped.");
        return;
    }
    ImGui::Text("Bank: %s", snap.hit.pcBank.text().c_str());
    if (snap.hit.reason == Debugger::Reason::WatchRead || snap.hit.reason == Debugger::Reason::WatchWrite)
        ImGui::Text("Access bank: %s", snap.hit.bank.text().c_str());
    char msg[128];
    switch (snap.hit.reason) {
        case Debugger::Reason::Breakpoint:
            std::snprintf(msg, sizeof(msg),
                          "Stopped: breakpoint at $%04X.", snap.hit.pc);
            break;
        case Debugger::Reason::StepOver:
        case Debugger::Reason::StepOut:
            std::snprintf(msg, sizeof(msg),
                          "Stopped: returned from the call to $%04X.",
                          snap.hit.pc);
            break;
        case Debugger::Reason::RunToCursor:
            std::snprintf(msg, sizeof(msg),
                          "Stopped: reached $%04X.", snap.hit.pc);
            break;
        case Debugger::Reason::WatchRead:
        case Debugger::Reason::WatchWrite:
            // Two addresses, and both matter: WHAT was written, and WHO wrote
            // it. The machine's PC is neither — it is the instruction after
            // the store, because the access cannot be un-done.
            std::snprintf(msg, sizeof(msg),
                          "Stopped: $%04X was %s ($%02X) by the instruction at $%04X.",
                          snap.hit.addr,
                          snap.hit.reason == Debugger::Reason::WatchWrite
                              ? "written" : "read",
                          snap.hit.value, snap.hit.pc);
            break;
        default:
            std::snprintf(msg, sizeof(msg), "Stopped.");
            break;
    }
    ImGui::TextColored(kPcColour, "%s", msg);
}

// ── The five verbs ───────────────────────────────────────────────────────

void Debugger_ImGui::drawControls(EmulationController& ctrl, const Snapshot& snap)
{
    // Run and Stop are one button in two states: the machine is either going
    // or it is not, and a pair of buttons where one is always dead reads as
    // broken. Same reasoning as a media transport.
    if (snap.running) {
        if (ImGui::Button("Stop", ImVec2(90, 0)))
            ctrl.setMode(EmulationController::Mode::Stopped);
    } else {
        if (ImGui::Button("Run", ImVec2(90, 0))) {
            {
                auto st = ctrl.lockState();
                ctrl.debugResume();
            }
            ctrl.setMode(EmulationController::Mode::Running);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Step", ImVec2(90, 0)))     ctrl.debugStepInstruction();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Execute one instruction (F7)");
    ImGui::SameLine();
    if (ImGui::Button("Step Over", ImVec2(90, 0))) ctrl.debugStepOver();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Run a JSR to completion; otherwise a plain step (F8)");
    ImGui::SameLine();
    if (ImGui::Button("Step Out")) ctrl.debugStepOut();
    ImGui::SameLine();
    ImGui::BeginDisabled(!cursorValid_);
    if (ImGui::Button("Run To Cursor", ImVec2(120, 0)))
        ctrl.debugRunToCursor(cursorAddr_, cursorBank_);
    ImGui::EndDisabled();
    if (!cursorValid_ && ImGui::IsItemHovered())
        ImGui::SetTooltip("Click a line in the disassembly first");

    ImGui::SameLine();
    ImGui::Checkbox("Follow PC", &followPc_);
}

// ── Registers ────────────────────────────────────────────────────────────

void Debugger_ImGui::drawRegisters(const Snapshot& snap)
{
    ImGui::Text("PC $%04X    A $%02X   X $%02X   Y $%02X   SP $%02X   P $%02X  [%s]",
                snap.pc, snap.a, snap.x, snap.y, snap.sp, snap.p,
                flagsText(snap.p).c_str());
    ImGui::TextColored(kDimColour, "%s",
                       snap.cmos ? "65C02 (CMOS) decoding" : "6502 (NMOS) decoding");
}

// ── Disassembly, with a clickable breakpoint gutter ─────────────────────

void Debugger_ImGui::drawDisassembly(EmulationController& ctrl, const Snapshot& snap)
{
    if (snap.memory.size() < 0x10000) {
        ImGui::TextDisabled("memory not attached");
        return;
    }

    // Start a couple of instructions above the PC so the user sees where the
    // machine came from, not just where it is. 6502 instructions are variable
    // length and cannot be walked backwards reliably, so this is an honest
    // approximation rather than a claim.
    uint16_t addr = static_cast<uint16_t>(viewAddr_ - 6);
    const int rows = std::max(8, static_cast<int>(
        ImGui::GetContentRegionAvail().y / ImGui::GetTextLineHeightWithSpacing()) - 1);

    for (int i = 0; i < rows; ++i) {
        int len = 1;
        const std::string mnem =
            disassemble6502(snap.memory.data(), addr, len, snap.cmos);
        const bool isPc = (addr == snap.pc);
        const DebugAddress location{addr,snap.banks[addr]};
        const bool wildcard = std::binary_search(snap.breakpoints.begin(),snap.breakpoints.end(),DebugAddress{addr,{}});
        const bool isBp = wildcard || std::binary_search(snap.breakpoints.begin(),snap.breakpoints.end(),location);

        // The gutter IS the control: clicking it toggles the breakpoint, the
        // way every debugger since Turbo Pascal has worked. A separate "add
        // breakpoint" dialog for the common case would be a worse tool.
        ImGui::PushID(static_cast<int>(addr));
        const char* mark = isBp ? "*" : " ";
        if (ImGui::SmallButton(mark)) {
            auto st = ctrl.lockState();
            if (isBp) ctrl.debugger().removeBreakpoint(addr,wildcard ? DebugBank{} : location.bank);
            else ctrl.debugger().addBreakpoint(addr,location.bank);
            ctrl.syncDebugHook();          // touches the CPU — hence lockState
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(isBp ? "Remove breakpoint at $%04X"
                                   : "Break at $%04X", addr);
        ImGui::PopID();
        ImGui::SameLine();

        char bytes[12] = {0};
        for (int b = 0; b < len && b < 3; ++b)
            std::snprintf(bytes + b * 3, sizeof(bytes) - static_cast<std::size_t>(b) * 3,
                          "%02X ", snap.memory[(addr + b) & 0xFFFF]);

        char line[96];
        const std::string label = symbols_.label(addr,location.bank);
        std::snprintf(line, sizeof(line), "%s  %-9s %s %s", location.text().c_str(), bytes, mnem.c_str(), label.c_str());

        if (isPc)      ImGui::PushStyleColor(ImGuiCol_Text, kPcColour);
        else if (isBp) ImGui::PushStyleColor(ImGuiCol_Text, kBreakColour);
        if (ImGui::Selectable(line, cursorValid_ && addr == cursorAddr_)) {
            cursorAddr_  = addr;
            cursorBank_ = location.bank;
            cursorValid_ = true;
        }
        if (isPc || isBp) ImGui::PopStyleColor();

        addr = static_cast<uint16_t>(addr + len);
    }
}

// ── Breakpoint list ──────────────────────────────────────────────────────

void Debugger_ImGui::drawBreakpointList(EmulationController& ctrl,
                                        const Snapshot& snap)
{
    ImGui::TextUnformatted("Breakpoints");
    ImGui::TextWrapped("bank:address or address for all banks (aux0:0800, lc2:D000).");
    ImGui::Separator();

    ImGui::SetNextItemWidth(150);
    const bool entered = ImGui::InputText("##bpaddr", bpEntry_, sizeof(bpEntry_),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool add = ImGui::Button("Add") || entered;
    if (add && bpEntry_[0]) {
        DebugAddress location; uint16_t value = 0;
        if (symbols_.resolveLocation(bpEntry_, location) &&
            (!conditionEnabled_ || DebuggerSymbols::address(conditionValue_, value))) {
            auto st = ctrl.lockState();
            if (conditionEnabled_) {
                Debugger::Condition condition;
                condition.reg = static_cast<Debugger::Register>(conditionRegister_);
                condition.compare = static_cast<Debugger::Compare>(conditionCompare_);
                condition.value = value;
                ctrl.debugger().addConditionalBreakpoint(location.addr, condition, st.cpu(), location.bank);
            } else {
                ctrl.debugger().addBreakpoint(location.addr,location.bank);
            }
            ctrl.syncDebugHook();
            symbolStatus_.clear();
            bpEntry_[0] = '\0';
        } else {
            symbolStatus_ = "Unknown address/label or invalid condition value.";
        }
    }
    ImGui::Checkbox("Conditional", &conditionEnabled_);
    if (conditionEnabled_) {
        ImGui::SetNextItemWidth(55);
        ImGui::Combo("##conditionRegister", &conditionRegister_, "A\0X\0Y\0P\0SP\0PC\0");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(55);
        ImGui::Combo("##conditionCompare", &conditionCompare_, "==\0!=\0<\0<=\0>\0>=\0");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(55);
        ImGui::InputText("##conditionValue", conditionValue_, sizeof(conditionValue_), ImGuiInputTextFlags_CharsHexadecimal);
    }
    if (!symbolStatus_.empty()) ImGui::TextWrapped("%s", symbolStatus_.c_str());

    if (snap.breakpoints.empty()) {
        ImGui::TextColored(kDimColour, "none");
    } else {
        for (const auto& location : snap.breakpoints) {
            const uint16_t bp = location.addr;
            ImGui::PushID(location.text().c_str());
            if (ImGui::SmallButton("x")) {
                auto st = ctrl.lockState();
                ctrl.debugger().removeBreakpoint(bp,location.bank);
                ctrl.syncDebugHook();
            }
            ImGui::SameLine();
            if (ImGui::Selectable(location.text().c_str())) {
                viewAddr_ = bp;
                cursorAddr_ = bp; cursorBank_ = location.bank.kind == DebugBank::Any ? snap.banks[bp] : location.bank;
                cursorValid_ = true;
                followPc_ = false;      // jumping to a breakpoint means "look here"
            }
            ImGui::PopID();
            const auto condition = snap.conditions.find(location);
            if (condition != snap.conditions.end()) {
                static const char* registers[] = {"A", "X", "Y", "P", "SP", "PC"};
                static const char* operators[] = {"==", "!=", "<", "<=", ">", ">="};
                const auto& c = condition->second;
                ImGui::Text("  %s %s $%04X", registers[static_cast<int>(c.reg)],
                    operators[static_cast<int>(c.compare)], c.value);
            }
        }
        ImGui::Separator();
        if (ImGui::Button("Clear all")) {
            auto st = ctrl.lockState();
            ctrl.debugger().clearBreakpoints();
            ctrl.syncDebugHook();
        }
    }
}

// ── Watchpoint list ──────────────────────────────────────────────────────
//
// Write watches are free armed or not (`memWrite`'s fast path carries a
// per-address `writable[]` byte a watch hides inside); a read watch is free
// only while NONE is armed — `memRead` has no such table, so arming one
// flips a flag that sends every read out of line (Memory.h § Read
// watchpoints, docs/PERFORMANCE.md § 8.5). The default is W for that reason.

void Debugger_ImGui::drawWatchpointList(EmulationController& ctrl,
                                        const Snapshot& snap)
{
    ImGui::TextUnformatted("Watch");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stops the machine when the address is accessed.\n"
                          "W: a write watch costs nothing, armed or not — the\n"
                          "address is diverted off memWrite's fast path.\n"
                          "R: a read watch costs nothing while none is armed,\n"
                          "but WHILE one is, every bus read goes out of line\n"
                          "(roughly the speed of the pre-2026-08 core).\n"
                          "Fires on the access, with the value read/written;\n"
                          "opcode fetches and soft-switch reads included.");
    ImGui::Separator();

    ImGui::SetNextItemWidth(150);
    const bool entered = ImGui::InputText("##wpaddr", wpEntry_, sizeof(wpEntry_),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::RadioButton("R", &wpAccess_, Debugger::Read);
    ImGui::SameLine();
    ImGui::RadioButton("W", &wpAccess_, Debugger::Write);
    ImGui::SameLine();
    ImGui::RadioButton("RW", &wpAccess_, Debugger::ReadWrite);
    ImGui::SameLine();
    const bool add = ImGui::Button("Watch") || entered;
    if (add && wpEntry_[0]) {
        DebugAddress location;
        if (symbols_.resolveLocation(wpEntry_, location)) {
            auto st = ctrl.lockState();
            ctrl.debugger().setWatchpoint(location.addr, static_cast<Debugger::Access>(wpAccess_), location.bank);
            ctrl.syncDebugHook();      // installs Memory's diversions
        }
        wpEntry_[0] = '\0';
    }

    if (snap.watchpoints.empty()) {
        ImGui::TextColored(kDimColour, "none");
        return;
    }
    for (const Debugger::Watch& w : snap.watchpoints) {
        ImGui::PushID(DebugAddress{w.addr,w.bank}.text().c_str());
        if (ImGui::SmallButton("x")) {
            auto st = ctrl.lockState();
            ctrl.debugger().setWatchpoint(w.addr, Debugger::None, w.bank);
            ctrl.syncDebugHook();
        }
        ImGui::SameLine();
        char t[48];
        std::snprintf(t, sizeof(t), "%s %s", DebugAddress{w.addr,w.bank}.text().c_str(),
                      w.access == Debugger::ReadWrite ? "RW"
                      : w.access == Debugger::Read    ? "R" : "W");
        if (ImGui::Selectable(t)) {
            viewAddr_ = w.addr;
            cursorAddr_ = w.addr; cursorBank_ = w.bank.kind == DebugBank::Any ? snap.banks[w.addr] : w.bank;
            cursorValid_ = true;
            followPc_ = false;
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    if (ImGui::Button("Clear watches")) {
        auto st = ctrl.lockState();
        ctrl.debugger().clearWatchpoints();
        ctrl.syncDebugHook();
    }
}

// Source files are read on the UI thread, after the machine snapshot lock.
void Debugger_ImGui::drawSource(EmulationController& ctrl, const Snapshot& snap)
{
    if (!ImGui::CollapsingHeader("Source")) return;
    ImGui::SetNextItemWidth(420);
    ImGui::InputText("Map file", sourceMapPath_, sizeof(sourceMapPath_));
    ImGui::SameLine();
    if (nativeFileDialogAvailable() && ImGui::Button("Browse##source")) {
        std::string picked;
        if (nativeFileDialog(false,"Load source map","p2map,txt","","",picked)==FileDialogResult::Selected) {
            if (picked.size()<sizeof(sourceMapPath_))
                std::snprintf(sourceMapPath_,sizeof(sourceMapPath_),"%s",picked.c_str());
            else sourceStatus_="The selected path is too long.";
        }
    }
    auto readBounded = [](const std::filesystem::path& path, std::string& text) {
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        const auto size=file ? file.tellg() : std::streampos{-1};
        if (size<0 || size>4*1024*1024) return false;
        std::string value(static_cast<size_t>(size),'\0');
        file.seekg(0);
        if (!value.empty()) file.read(value.data(),static_cast<std::streamsize>(value.size()));
        if (!file) return false;
        text=std::move(value); return true;
    };
    if (ImGui::Button("Load source map")) {
        std::string text;
        if (!readBounded(std::filesystem::u8path(sourceMapPath_),text))
            sourceStatus_="Cannot read source map (maximum 4 MiB).";
        else if (sourceMap_.load(text,sourceStatus_)) {
            sourceRoot_=std::filesystem::u8path(sourceMapPath_).parent_path().u8string();
            sourceStatus_=std::to_string(sourceMap_.size())+" source ranges loaded.";
            sourceFile_.clear(); sourceLoaded_=false;
        }
    }
    ImGui::TextWrapped("Map rows: aux0:$0800-$0802 \"src/main.s\" 42. Paths are relative to the map. Ranges use inclusive hexadecimal addresses; lines start at 1.");
    ImGui::TextWrapped("%s",sourceStatus_.c_str());
    const DebugAddress location{followPc_ || !cursorValid_ ? snap.pc : cursorAddr_,
        followPc_ || !cursorValid_ ? snap.pcBank : cursorBank_};
    const auto* entry=sourceMap_.lookup(location);
    if (!entry) { ImGui::TextDisabled("No source mapping for %s",location.text().c_str()); return; }
    ImGui::Text("%s -> %s:%u",location.text().c_str(),entry->file.c_str(),entry->line);
    if (ImGui::Button("Break at source line")) {
        auto st=ctrl.lockState();
        ctrl.debugger().addBreakpoint(entry->start.addr,entry->start.bank);
        ctrl.syncDebugHook();
    }
    const auto path=(std::filesystem::u8path(sourceRoot_)/std::filesystem::u8path(entry->file)).lexically_normal();
    auto loadSource = [&] {
        std::string text;
        sourceLoaded_=readBounded(path,text);
        sourceLines_.clear();
        if (!sourceLoaded_) return;
        std::istringstream input(text); std::string line;
        while (std::getline(input,line)) sourceLines_.push_back(std::move(line));
    };
    if (sourceFile_!=path.u8string()) {
        sourceFile_=path.u8string();
        loadSource();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload source")) loadSource();
    if (!sourceLoaded_) { ImGui::TextDisabled("Cannot read source (maximum 4 MiB)."); return; }
    ImGui::BeginChild("##sourceLines",ImVec2(0,125),true,ImGuiWindowFlags_HorizontalScrollbar);
    const unsigned first=entry->line > 4 ? entry->line-4 : 1;
    const unsigned last=std::min(entry->line+4,static_cast<unsigned>(sourceLines_.size()));
    for (unsigned number=first; number<=last; ++number) {
        if (number==entry->line) ImGui::PushStyleColor(ImGuiCol_Text,kPcColour);
        ImGui::Text("%c %5u  %s",number==entry->line ? '>' : ' ',number,sourceLines_[number-1].c_str());
        if (number==entry->line) ImGui::PopStyleColor();
    }
    if (sourceLines_.size()<entry->line) ImGui::TextDisabled("Source line is beyond end of file.");
    ImGui::EndChild();
}

}  // namespace pom2
