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

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

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
        snap.cmos     = cpu.getCpuMode() == M6502::CpuMode::CMOS;
        snap.halted   = cpu.isHalted();
        snap.hit      = ctrl.debugger().lastHit();
        snap.breakpoints = ctrl.debugger().breakpoints();
        snap.watchpoints = ctrl.debugger().watchpoints();
        // The PAGED view, not mem.data(). The flat mirror is main-bank only,
        // so on a //e running out of aux, out of a RamWorks bank or out of
        // language-card RAM the panel disassembled bytes the CPU never
        // fetches — and every breakpoint the user then set from that listing
        // landed at a non-instruction boundary. snapshotCpuView() resolves the
        // paging without touching a soft switch (Memory.h).
        snap.memory.assign(0x10000, 0);
        mem.snapshotCpuView(snap.memory.data());
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

    // Disassembly left, breakpoints right.
    const float rightWidth = 210.0f;
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
    char msg[128];
    switch (snap.hit.reason) {
        case Debugger::Reason::Breakpoint:
            std::snprintf(msg, sizeof(msg),
                          "Stopped: breakpoint at $%04X.", snap.hit.pc);
            break;
        case Debugger::Reason::StepOver:
            std::snprintf(msg, sizeof(msg),
                          "Stopped: stepped over the call, back at $%04X.",
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
    ImGui::BeginDisabled(!cursorValid_);
    if (ImGui::Button("Run To Cursor", ImVec2(120, 0)))
        ctrl.debugRunToCursor(cursorAddr_);
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
        const bool isBp = std::binary_search(snap.breakpoints.begin(),
                                             snap.breakpoints.end(), addr);

        // The gutter IS the control: clicking it toggles the breakpoint, the
        // way every debugger since Turbo Pascal has worked. A separate "add
        // breakpoint" dialog for the common case would be a worse tool.
        ImGui::PushID(static_cast<int>(addr));
        const char* mark = isBp ? "*" : " ";
        if (ImGui::SmallButton(mark)) {
            auto st = ctrl.lockState();
            ctrl.debugger().toggleBreakpoint(addr);
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
        std::snprintf(line, sizeof(line), "$%04X  %-9s %s", addr, bytes, mnem.c_str());

        if (isPc)      ImGui::PushStyleColor(ImGuiCol_Text, kPcColour);
        else if (isBp) ImGui::PushStyleColor(ImGuiCol_Text, kBreakColour);
        if (ImGui::Selectable(line, cursorValid_ && addr == cursorAddr_)) {
            cursorAddr_  = addr;
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
    ImGui::Separator();

    ImGui::SetNextItemWidth(70);
    const bool entered = ImGui::InputText("##bpaddr", bpEntry_, sizeof(bpEntry_),
                                          ImGuiInputTextFlags_CharsHexadecimal |
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool add = ImGui::Button("Add") || entered;
    if (add && bpEntry_[0]) {
        unsigned v = 0;
        if (std::sscanf(bpEntry_, "%x", &v) == 1) {
            auto st = ctrl.lockState();
            ctrl.debugger().addBreakpoint(static_cast<uint16_t>(v & 0xFFFF));
            ctrl.syncDebugHook();
        }
        bpEntry_[0] = '\0';
    }

    if (snap.breakpoints.empty()) {
        ImGui::TextColored(kDimColour, "none");
    } else {
        for (const uint16_t bp : snap.breakpoints) {
            ImGui::PushID(static_cast<int>(bp));
            if (ImGui::SmallButton("x")) {
                auto st = ctrl.lockState();
                ctrl.debugger().removeBreakpoint(bp);
                ctrl.syncDebugHook();
            }
            ImGui::SameLine();
            if (ImGui::Selectable(([bp] {
                    char t[16];
                    std::snprintf(t, sizeof(t), "$%04X", bp);
                    return std::string(t);
                })().c_str())) {
                viewAddr_ = bp;
                followPc_ = false;      // jumping to a breakpoint means "look here"
            }
            ImGui::PopID();
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

    ImGui::SetNextItemWidth(70);
    const bool entered = ImGui::InputText("##wpaddr", wpEntry_, sizeof(wpEntry_),
                                          ImGuiInputTextFlags_CharsHexadecimal |
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::RadioButton("R", &wpAccess_, Debugger::Read);
    ImGui::SameLine();
    ImGui::RadioButton("W", &wpAccess_, Debugger::Write);
    ImGui::SameLine();
    ImGui::RadioButton("RW", &wpAccess_, Debugger::ReadWrite);
    ImGui::SameLine();
    const bool add = ImGui::Button("Watch") || entered;
    if (add && wpEntry_[0]) {
        unsigned v = 0;
        if (std::sscanf(wpEntry_, "%x", &v) == 1) {
            auto st = ctrl.lockState();
            ctrl.debugger().setWatchpoint(static_cast<uint16_t>(v & 0xFFFF),
                                          static_cast<Debugger::Access>(wpAccess_));
            ctrl.syncDebugHook();      // installs Memory's diversions
        }
        wpEntry_[0] = '\0';
    }

    if (snap.watchpoints.empty()) {
        ImGui::TextColored(kDimColour, "none");
        return;
    }
    for (const Debugger::Watch& w : snap.watchpoints) {
        ImGui::PushID(0x10000 + static_cast<int>(w.addr));
        if (ImGui::SmallButton("x")) {
            auto st = ctrl.lockState();
            ctrl.debugger().setWatchpoint(w.addr, Debugger::None);
            ctrl.syncDebugHook();
        }
        ImGui::SameLine();
        char t[16];
        std::snprintf(t, sizeof(t), "$%04X %s", w.addr,
                      w.access == Debugger::ReadWrite ? "RW"
                      : w.access == Debugger::Read    ? "R" : "W");
        if (ImGui::Selectable(t)) {
            viewAddr_ = w.addr;
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

}  // namespace pom2
