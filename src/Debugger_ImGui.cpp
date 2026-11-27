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
#include "AppleIoNames.h"
#include "SlotBus.h"
#include "SlotPeripheral.h"
#include "BeamPosition.h"
#include "DisasmNavigation.h"
#include "Pom2Theme.h"

#include "BrowserFiles.h"
#include "Cc65DebugInfo.h"
#include "Disassembler6502.h"
#include "EmulationController.h"
#include "M6502.h"
#include "Memory.h"
#include "NativeFileDialog.h"
#include "AtomicFileReplace.h"
#include "ProfileExport.h"
#include "ResourcePaths.h"

#include "imgui.h"
#include "imgui_internal.h"     // NavWindow: is the Apple II Screen the focused window?

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>

namespace pom2 {

namespace {

/// A cc65 .dbg names every line and span of a program and its libraries, so
/// it outgrows the 4 MiB a hand-written map is allowed; the CONVERTED map
/// still goes through DebuggerSourceMap's own 4 MiB and 65536-range bounds.
constexpr std::streamoff kCc65DebugFileLimit = 16 * 1024 * 1024;

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
    // "Show $xxxx in the listing", asked from another window: come forward.
    if (showRequest_) {
        if (open) *open = true;
        ImGui::SetNextWindowFocus();
        showRequest_ = false;
    }
    if (open && !*open) return;

    // Docked by the layout presets (MainWindow::applyDockLayout: the Debug
    // preset puts it on the right, the Screen Inspector on the left). This
    // size only serves a window the user has pulled out to float.
    ImGui::SetNextWindowSize(ImVec2(860, 760), ImGuiCond_FirstUseEver);
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
        snap.isa      = cpu.instructionSet();
        snap.halted   = cpu.isHalted();
        snap.hit      = ctrl.debugger().lastHit();
        snap.canGoBack = ctrl.debugCanStepBack(&snap.whyNoBack);
        snap.recording = ctrl.rewind().enabled();
        snap.scrubbing = ctrl.rewindScrubbing();
        snap.breakpoints = ctrl.debugger().bankBreakpoints();
        snap.pcBank = mem.debugBank(snap.pc);
        snap.watchpoints = ctrl.debugger().watchpoints();
        for (const auto& c : ctrl.debugger().conditions()) snap.conditions[{c.first,{}}]=c.second;
        for (const auto& c : ctrl.debugger().bankConditions()) snap.conditions[c.first]=c.second;
        snap.bpGates = ctrl.debugger().breakpointGates();
        snap.wpGates = ctrl.debugger().watchGates();
        snap.profiling = ctrl.debugger().profiling();
        snap.historyEnabled = ctrl.debugger().historyEnabled();
        snap.profile = ctrl.debugger().profile();
        snap.history = ctrl.debugger().history();
        {
            const InterruptLoad& load = ctrl.debugger().interruptLoad();
            const auto frames = load.frames();
            snap.interruptLoad.reserve(frames.size());
            for (const auto& f : frames)
                snap.interruptLoad.push_back(static_cast<float>(100.0 * f.share()));
            snap.interruptSummary = load.summary();
            snap.interruptEntriesLast = frames.empty() ? 0 : frames.back().entries;
        }
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
        snap.cycles     = mem.getCycleCounter();
        snap.videoCycle = mem.videoNow();
        snap.standard   = mem.videoStandard();
        snap.beamView   = mem.beamView();
        snap.iicClass   = mem.isIIcClass();
        snap.video      = mem.getDisplayState();
        for (int slot = 1; slot <= 7; ++slot) {
            const SlotPeripheral* card = mem.slotBus().peripheral(slot);
            const std::string_view name = card ? card->name() : std::string_view{};
            snap.slotKind[slot] = (name == "Mockingboard" || name == "Phasor") ? 1
                                : name == "Disk II" ? 2 : 0;
        }
    }
    snap.running = ctrl.getMode() == EmulationController::Mode::Running;

    // A stop brings the listing back to the PC, even after the user scrolled
    // away: the wheel is for looking around, and the machine stopping is the
    // event the user was waiting for.
    // "Follow PC" is the user's switch and nothing else writes it: the wheel,
    // Go to and the other ways of looking elsewhere only SUSPEND it
    // (lookingAround_), until the next step, stop, run or Tab. They used to
    // untick the box — one brush of a trackpad and it was off ("Follow PC
    // does not hold", 2026-11-25).
    if (wasRunning_ != snap.running) {
        lookingAround_ = false;
        if (!snap.running) viewAddr_ = snap.pc;
    }
    wasRunning_ = snap.running;
    if (following()) viewAddr_ = snap.pc;
    // "Show $xxxx in the listing", asked while this panel was closed or a
    // hidden tab: applied HERE, after the lines above — which, on the frame
    // the panel comes back, would send the listing to the PC again.
    if (showPending_) {
        showPending_ = false;
        viewAddr_ = cursorAddr_ = showAddr_;
        // The bank asked for (a bank-qualified breakpoint's row), else the
        // one mapped there now: the Source pane and Run To Cursor need one.
        cursorBank_ = showBank_.kind == DebugBank::Any ? snap.banks[showAddr_] : showBank_;
        cursorValid_ = true;
        lookingAround_ = true;
    }

    // F7 steps while the window is open. Handled HERE rather than in
    // MainWindow's global key routing: the shortcut belongs to this panel and
    // should not fire when it is closed. (F8 was step-over until 2026-11-25;
    // it is now the window / kiosk toggle everywhere. Step over is
    // Ctrl+Space, as in AppleWin.)
    handleKeys(ctrl, snap);
    // RootAndChildWindows: the listing and the breakpoint list are CHILD
    // windows, and a click in one makes it the focused window — with a bare
    // IsWindowFocused() the F7 keys died exactly when the panel had focus.
    // Not while a text field is being typed in: Shift+F7 would rewind the
    // machine from the "Go to" box.
    // Nor while the Apple II Screen has the focus: the user is typing to the
    // machine there, and F7 stepped it — Shift+F7 rewound it — from a
    // keystroke meant for the emulated keyboard.
    const ImGuiWindow* focused = ImGui::GetCurrentContext()->NavWindow;
    const bool typingToMachine = !focused ||
        std::strcmp(focused->RootWindow->Name, "Apple II Screen") == 0;
    if (!ImGui::GetIO().WantTextInput && !typingToMachine &&
        (!ImGui::GetIO().WantCaptureKeyboard ||
         ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))) {
        // Shift+F7 steps BACK, Ctrl+F7 runs back. (Not Backspace: it is the
        // key that corrects a value just typed in one of this panel's
        // fields, and it would have rewound the machine.)
        const ImGuiIO& keys = ImGui::GetIO();
        if (ImGui::IsKeyPressed(ImGuiKey_F7, /*repeat=*/keys.KeyShift)) {
            lookingAround_ = false;
            // KeySuper too, on a Mac only: ImGui swaps Ctrl and Cmd there
            // (ConfigMacOSXBehaviors), so the physical Ctrl arrives as Super
            // — and fell through to a plain step, moving the machine FORWARD
            // when it was asked to go back. Elsewhere Super is the Windows key.
            const bool ctrlHeld = keys.KeyCtrl || (keys.ConfigMacOSXBehaviors && keys.KeySuper);
            if (keys.KeyShift)      goBack(ctrl, snap, false);
            else if (ctrlHeld)      goBack(ctrl, snap, true);
            else if (!keys.KeyAlt)                  ctrl.debugStepInstruction();
        }
    }

    drawStopBanner(snap);
    // The refusal is about ONE attempt: gone as soon as the machine moves.
    if (snap.running || snap.cycles != backStatusCycles_) backStatus_.clear();
    if (!backStatus_.empty()) ImGui::TextColored(kBreakColour, "%s", backStatus_.c_str());
    drawControls(ctrl, snap);
    drawNavigation(ctrl, snap);
    ImGui::Separator();
    drawRegisters(snap);
    drawBeamAndCycles(snap);
    ImGui::Separator();
    // The listing is what the panel is FOR: it comes first, across the whole
    // width (an operand's note needs the room) and takes the height, leaving
    // space for the tool headers below it (closed). An open tool grows
    // downward and the window scrolls.
    // NoScrollWithMouse + NoScrollbar: the listing is not a scrolled region
    // (it is laid out from viewAddr_ each frame), so the wheel must neither
    // scroll it nor fall through to the Debugger window. drawDisassembly
    // reads the wheel itself and moves viewAddr_ by instructions.
    const float mainHeight = std::max(220.0f,
        ImGui::GetContentRegionAvail().y - 6.0f * (ImGui::GetFrameHeightWithSpacing() + 4.0f) - 18.0f);
    ImGui::BeginChild("##disasm", ImVec2(0, mainHeight), true,
                      ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
    {
        ScopedMonoFont mono;
        drawDisassembly(ctrl, snap);
    }
    ImGui::EndChild();

    // Breakpoints fold like the other tools: the gutter of the listing sets
    // one and a right click sets the others, so the list is for reviewing
    // them and giving them conditions. One list, three kinds — on the PC
    // (BP), on a read (BPMR), on a write (BPMW) — named as AppleWin names
    // them. Values that are only LOOKED at live in the Watch window.
    {
        char title[96];
        std::snprintf(title, sizeof title, "Breakpoints (%zu on the PC, %zu on memory)###bpwatch",
                      snap.breakpoints.size(), snap.watchpoints.size());
        if (ImGui::CollapsingHeader(title)) {
            ScopedMonoFont mono;
            ImGui::BeginChild("##bpcol", ImVec2(0, 200), true);
            drawBreakpointList(ctrl, snap);
            ImGui::EndChild();
        }
    }
    drawGateEditor(ctrl);
    drawAssembler(ctrl, snap);

    if (ImGui::CollapsingHeader("Your program's symbols (optional file)")) {
        ImGui::Checkbox("Name the machine's own addresses in the listing", &ioNames_);
        ImGui::TextWrapped("Built in, no file needed: soft switches, language card, monitor ROM entry points, "
                           "ProDOS MLI, vectors, Mockingboard and Disk II registers. A symbol file adds YOUR "
                           "program's labels, and its names win.");
        ImGui::SetNextItemWidth(420);
        ImGui::InputText("File##symbols", symbolPath_, sizeof(symbolPath_));
        ImGui::SameLine();
        if (nativeFileDialogAvailable()) {
            if (ImGui::Button("Browse##symbols")) {
                std::string picked;
                if (nativeFileDialog(false, "Load debugger symbols", "sym,lbl,txt,dbg", "", "", picked) == FileDialogResult::Selected) {
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
            const bool cc65 = cc65dbg::isDebugFilePath(symbolPath_);
            if (size < 0 || size > (cc65 ? kCc65DebugFileLimit : 4 * 1024 * 1024)) {
                symbolStatus_ = cc65 ? "Cannot read cc65 debug file (maximum 16 MiB)."
                                     : "Cannot read symbol file (maximum 4 MiB).";
            } else {
                std::string text(static_cast<size_t>(size), '\0');
                input.seekg(0);
                if (!text.empty()) input.read(text.data(), static_cast<std::streamsize>(text.size()));
                cc65dbg::Conversion converted;
                if (!input) symbolStatus_ = "Symbol file read failed.";
                else if (cc65 && !cc65dbg::toSymbols(text, converted, symbolStatus_)) {}
                else if (symbols_.load(cc65 ? converted.text : text, symbolStatus_)) {
                    symbolStatus_ = std::to_string(symbols_.size()) + " symbols loaded";
                    if (cc65) symbolStatus_ += " from cc65 debug info (" +
                        std::to_string(converted.skipped) + " skipped)";
                    symbolStatus_ += ".";
                }
            }
        }
        ImGui::TextWrapped("AppleWin .sym, VICE/cc65 labels, cc65 .dbg (labels) and NAME = $address. Bank-qualified symbols are supported (entry = aux1:$0800). Use labels in the breakpoint field.");
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
        drawTraceAndExport(ctrl);
        // Interrupt load per video frame (InterruptLoad.h): how much of each
        // frame the IRQ/NMI handlers took, e.g. a Mockingboard player.
        if (snap.profiling || !snap.interruptLoad.empty()) {
            const auto& s = snap.interruptSummary;
            if (s.frames)
                ImGui::Text("Interrupt load per frame: last %.1f %%  mean %.1f %%  peak %.1f %%  (%u entries last frame, %zu frames)",
                            100.0 * s.last, 100.0 * s.mean, 100.0 * s.peak,
                            snap.interruptEntriesLast, s.frames);
            else
                ImGui::TextUnformatted("Interrupt load per frame: waiting for the first complete frame");
            if (!snap.interruptLoad.empty())
                ImGui::PlotHistogram("##irqload", snap.interruptLoad.data(),
                                     static_cast<int>(snap.interruptLoad.size()), 0,
                                     "IRQ/NMI share of each frame, 0-100 %", 0.0f, 100.0f,
                                     ImVec2(-1.0f, 60.0f));
        }
        ImGui::BeginChild("##measurements", ImVec2(0, 140), true);
        {
        ScopedMonoFont mono;
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
        }
        ImGui::EndChild();
    }

    drawOsCalls(ctrl, snap);

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
    {
        ScopedMonoFont mono;
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(osLines_.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
                ImGui::TextUnformatted(osLines_[static_cast<std::size_t>(i)].c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

// ── Why are we stopped? ──────────────────────────────────────────────────

void Debugger_ImGui::drawStopBanner(const Snapshot& snap)
{
    if (snap.halted) {
        ImGui::TextColored(kBreakColour,
            "CPU halted (a JAM opcode on a 6502; STP or WAI on a 65C02) - a reset clears it.");
        return;
    }
    if (!snap.hit.valid()) {
        ImGui::TextColored(kDimColour, snap.running ? "Running." : "Stopped.");
        return;
    }
    ImGui::Text("Bank: %s", snap.hit.pcBank.text().c_str());
    if (snap.hit.reason == Debugger::Reason::WatchRead || snap.hit.reason == Debugger::Reason::WatchWrite)
        ImGui::Text("Access bank: %s", snap.hit.bank.text().c_str());
    char msg[160];
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
        case Debugger::Reason::StepBack:
            std::snprintf(msg, sizeof(msg),
                          "Stepped back: one instruction earlier, at $%04X.", snap.hit.pc);
            break;
        case Debugger::Reason::RunBackStart:
            std::snprintf(msg, sizeof(msg),
                          "Ran back: no breakpoint in the history - this is its oldest point.");
            break;
        case Debugger::Reason::RunBackPaused:
            std::snprintf(msg, sizeof(msg),
                          "Ran back: no breakpoint met so far - Run Back again to go further.");
            break;
        case Debugger::Reason::RunToBeam:
            std::snprintf(msg, sizeof(msg),
                          "Stopped: the beam reached the position asked for.");
            break;
        case Debugger::Reason::WatchRead:
        case Debugger::Reason::WatchWrite:
            // Two addresses, and both matter: WHAT was written, and WHO wrote
            // it. The machine's PC is neither — it is the instruction after
            // the store, because the access cannot be un-done.
            // A DMA card's processor (SoftCard Z80) held the bus: the parked
            // 6502's PC is not the one that made the access.
            if (snap.hit.busMaster)
                std::snprintf(msg, sizeof(msg),
                              "Stopped: $%04X was %s ($%02X) by the card processor holding the bus (DMA).",
                              snap.hit.addr,
                              snap.hit.reason == Debugger::Reason::WatchWrite ? "written" : "read",
                              snap.hit.value);
            else
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
    // Where the beam was: at the instruction's first cycle for a stop on
    // the PC, at the cycle of the ACCESS for a memory breakpoint — which is
    // the figure a program racing the beam is after.
    const BeamPosition at = beamPositionAt(snap.hit.videoCycle, snap.standard);
    const bool access = snap.hit.reason == Debugger::Reason::WatchRead ||
                        snap.hit.reason == Debugger::Reason::WatchWrite;
    ImGui::SameLine();
    ImGui::TextColored(kDimColour, "%s V %d  H %d%s", access ? "Access at" : "Beam at",
                       at.line, at.h, at.vbl ? " (VBL)" : at.hbl ? " (HBL)" : "");
}

// ── The five verbs ───────────────────────────────────────────────────────

void Debugger_ImGui::drawControls(EmulationController& ctrl, const Snapshot& snap)
{
    // Run and Stop are one button in two states: the machine is either going
    // or it is not, and a pair of buttons where one is always dead reads as
    // broken. Same reasoning as a media transport.
    if (snap.running) {
        if (ImGui::Button("Stop", ImVec2(76, 0)))
            ctrl.setMode(EmulationController::Mode::Stopped);
    } else {
        if (ImGui::Button("Run", ImVec2(76, 0))) {
            {
                auto st = ctrl.lockState();
                ctrl.debugResume();
            }
            ctrl.setMode(EmulationController::Mode::Running);
        }
    }
    // Each button stays on the row while it fits, else starts a new one: the
    // panel is often half a window wide.
    const auto next = [](float width) {
        ImGui::SameLine();
        if (ImGui::GetCursorPosX() + width > ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x)
            ImGui::NewLine();
    };
    next(76.0f);
    if (ImGui::Button("Step", ImVec2(76, 0)))     { lookingAround_ = false; ctrl.debugStepInstruction(); }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Execute one instruction (Space, F7)");
    next(90.0f);
    if (ImGui::Button("Step Over", ImVec2(90, 0))) { lookingAround_ = false; ctrl.debugStepOver(); }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Run a JSR to completion; otherwise a plain step (Ctrl+Space)");
    next(82.0f);
    if (ImGui::Button("Step Out", ImVec2(82, 0))) { lookingAround_ = false; ctrl.debugStepOut(); }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Run until the current routine returns (Shift+Space)");
    // Backwards, through the rewind ring. Greyed when there is no way back:
    // the rewind is not recording, its history ends here, or the machine is
    // running — the tooltip says which.
    const auto backward = [&](const char* label, float width, const char* what) {
        next(width);
        ImGui::BeginDisabled(!snap.canGoBack);
        const bool pressed = ImGui::Button(label, ImVec2(width, 0));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (snap.canGoBack) ImGui::SetTooltip("%s", what);
            else ImGui::SetTooltip("%s\n\n%s", what, snap.whyNoBack ? snap.whyNoBack : "");
        }
        return pressed && snap.canGoBack;
    };
    if (backward("Step Back", 90.0f,
                 "Go back one instruction (Shift+F7).\n"
                 "Restores the rewind's previous frame and re-executes up to the\n"
                 "instruction before this one. Not across a key, a mouse movement\n"
                 "or bytes from outside that reached the machine while it ran.")) {
        goBack(ctrl, snap, false);
    }
    if (backward("Run Back", 84.0f,
                 "Go back to the previous breakpoint (Ctrl+F7): the latest\n"
                 "point, before this one, where a breakpoint on the PC or on memory\n"
                 "would have stopped - with its conditions, not its \"ignore N\".\n"
                 "With none in the history: its oldest point.")) {
        goBack(ctrl, snap, true);
    }
    // The switch the two buttons depend on, where they are. Off by default,
    // like the Rewind panel's Record it is the same switch as (owner's
    // ruling, 2026-11-27): recording costs memory and a capture per frame,
    // and is the user's to turn on.
    next(70.0f);
    // Not during a Rewind-panel scrub: that panel ends its scrub before it
    // stops recording, and switching from here would leave the machine on
    // a scrubbed frame of a ring that is gone.
    bool recording = snap.recording;
    ImGui::BeginDisabled(snap.scrubbing);
    if (ImGui::Checkbox("Rec", &recording)) {
        auto st = ctrl.lockState();         // enabling clears the ring the worker captures into
        ctrl.rewind().setEnabled(recording);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Record the machine's history (the Rewind panel's Record).\n"
                          "Step Back and Run Back need it: tick it, then run. It keeps\n"
                          "about 30 s, a few megabytes on a stock machine.");
    next(118.0f);
    ImGui::BeginDisabled(!cursorValid_);
    if (ImGui::Button("Run To Cursor", ImVec2(118, 0)))
        ctrl.debugRunToCursor(cursorAddr_, cursorBank_);
    ImGui::EndDisabled();
    if (!cursorValid_ && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Click a line in the disassembly first (then Ctrl+Down)");
    next(110.0f);
    if (ImGui::Checkbox("Follow PC", &followPc_)) lookingAround_ = false;
    if (followPc_ && lookingAround_) {
        ImGui::SameLine();
        ImGui::TextDisabled("(looking elsewhere: Tab = back to PC)");
    }
}

// Step Back / Run Back, and what to say when the machine refused.
void Debugger_ImGui::goBack(EmulationController& ctrl, const Snapshot& snap, bool toBreakpoint)
{
    if (!snap.canGoBack) {
        // By key there is no greyed button under the pointer to say why.
        backStatus_ = std::string("Cannot go back: ") + (snap.whyNoBack ? snap.whyNoBack : "no history.");
        for (char& c : backStatus_) if (c == '\n') c = ' ';
        backStatusCycles_ = snap.cycles;
        return;
    }
    lookingAround_ = false;
    const bool went = toBreakpoint ? ctrl.debugRunBack() : ctrl.debugStepBack();
    // False covers several things — the history ended in the meantime, or a
    // replay was given up — so the line says what is true of all of them.
    backStatus_ = went ? std::string()
        : "Could not go back: the machine is as it was (the buttons' tooltip, or the log, says why).";
    backStatusCycles_ = snap.cycles;
}

// ── Registers ────────────────────────────────────────────────────────────

namespace {
// Every header row starts with the same 8-character label, so the values of
// CPU / Video / Memory / Beam / Cycles line up in one column. A row is a run
// of tokens: one that does not fit the panel's width continues on the next
// line, under the values and not under the label — the Debugger is often
// docked in half a window, and a row cut at the edge is a row unread.
float g_rowIndent = 0.0f;

void rowLabel(const char* label)
{
    ImGui::TextColored(kDimColour, "%-8s", label);
    ImGui::SameLine(0.0f, 0.0f);
    g_rowIndent = ImGui::GetCursorPosX();
}
// Make room for an item `width` wide: stay on the row, or wrap under it.
void flowTo(float width)
{
    const float right = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    if (ImGui::GetCursorPosX() + width > right && ImGui::GetCursorPosX() > g_rowIndent + 1.0f) {
        ImGui::NewLine();
        ImGui::SetCursorPosX(g_rowIndent);
    }
}
void token(const ImVec4& colour, const char* fmt, ...) IM_FMTARGS(2);
void token(const ImVec4& colour, const char* fmt, ...)
{
    char text[96];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    flowTo(ImGui::CalcTextSize(text).x);
    ImGui::TextColored(colour, "%s", text);
    ImGui::SameLine(0.0f, 0.0f);
}
void endRow() { ImGui::NewLine(); }

const ImVec4 kValue(0.92f, 0.92f, 0.92f, 1.0f);
const ImVec4 kOn(0.45f, 0.90f, 0.50f, 1.0f);
const ImVec4 kNote(0.95f, 0.85f, 0.35f, 1.0f);
}  // namespace

void Debugger_ImGui::drawRegisters(const Snapshot& snap)
{
    ScopedMonoFont mono;
    rowLabel("CPU");
    token(kValue, "PC $%04X  ", snap.pc);
    token(kValue, "A $%02X  ", snap.a);
    token(kValue, "X $%02X  ", snap.x);
    token(kValue, "Y $%02X  ", snap.y);
    token(kValue, "SP $%02X  ", snap.sp);
    token(kValue, "P $%02X [%s]  ", snap.p, flagsText(snap.p).c_str());
    token(kDimColour, "%s",
          snap.isa == Cpu6502Set::Ncr65C02      ? "65C02 (NCR)" :
          snap.isa == Cpu6502Set::Superset65C02 ? "65C02 (Rockwell/WDC)" : "6502 (NMOS)");
    endRow();

    // The video switches as they stand NOW (the live latches, not a frame):
    // bright = on. Every name keeps its width, so the row does not move
    // when the program flips one.
    const Memory::DisplayState& v = snap.video;
    rowLabel("Video");
    token(kOn, "%-9s", v.textMode ? "TEXT" : "GRAPHICS");
    token(v.textMode ? kDimColour : kOn, "%-6s", v.hiRes ? "HIRES" : "LORES");
    token(kOn, "%-6s", v.page2 ? "PAGE2" : "PAGE1");
    token(v.mixedMode ? kOn : kDimColour, "MIXED ");
    token(v.eightyCol ? kOn : kDimColour, "80COL ");
    token(v.dhgr ? kOn : kDimColour, "DHGR ");
    token(v.eightyStore ? kOn : kDimColour, "80STORE ");
    token(v.altChar ? kOn : kDimColour, "ALTCHAR");
    endRow();

    // What the CPU reads where: the bank mapped at each region. Anything
    // but plain main RAM or ROM is paging at work, and stands out.
    rowLabel("Memory");
    const auto region = [&](const char* name, uint16_t at) {
        const DebugBank bank = snap.banks[at];
        const bool plain = bank.kind == DebugBank::Main || bank.kind == DebugBank::Rom ||
                           bank.kind == DebugBank::Io;
        token(plain ? kDimColour : kNote, "%s %-5s", name, bank.text().c_str());
    };
    region("ZP", 0x0000);
    region("$0200", 0x0200);
    region("$0400", 0x0400);
    region("$2000", 0x2000);
    region("$C300", 0xC300);
    region("$D000", 0xD000);
    region("$E000", 0xE000);
    endRow();
}

// ── Beam position and cycle counter ─────────────────────────────────────
// French Touch's request (2026-11-25): where the beam is at instant T, and a
// counter to read the cycles spent between two points of the code.

void Debugger_ImGui::drawBeamAndCycles(const Snapshot& snap)
{
    // The last run or step. Measured only while stopped: the counter where
    // the machine stands is compared with where it stood the previous time.
    // A counter that went BACK (rewind, snapshot load, cold boot) starts the
    // measure over rather than printing a wrapped 64-bit number.
    if (!snap.running) {
        if (haveStop_ && snap.cycles > stopCycles_) {
            lastRunCycles_ = snap.cycles - stopCycles_;
            haveLastRun_ = true;
        } else if (haveStop_ && snap.cycles < stopCycles_) {
            haveLastRun_ = false;
        }
        stopCycles_ = snap.cycles;
        haveStop_ = true;
    }

    ScopedMonoFont mono;
    const BeamPosition b = beamPositionAt(snap.videoCycle, snap.standard);
    // Green while the beam draws, yellow in blanking — AppleWin's colours.
    const ImVec4 beamColour = (b.vbl || b.hbl) ? kNote : kOn;
    rowLabel("Beam");
    token(beamColour, "V %3d ($%03X)  ", b.line, b.line);
    token(beamColour, "H %2d ($%02X)  ", b.h, b.h);
    if (b.vbl)      token(beamColour, "vertical blanking  ");
    else if (b.hbl) token(beamColour, "horizontal blanking  ");
    else            token(beamColour, "on byte %d  ", b.column);
    token(kDimColour, "cycle %u of the %s frame", b.frameCycle,
          snap.standard == VideoStandard::PAL ? "PAL" : "NTSC");
    endRow();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("V = line in the frame (0-191 visible, then vertical blanking).\n"
                          "H = cycle in the 65-cycle line (0-24 horizontal blanking,\n"
                          "25-64 the 40 bytes of the line).\n"
                          "Numbered like AppleWin's v: and h: (its default, not its \"real\" H).");

    const bool wentBack = snap.cycles < cycleMark_;
    rowLabel("Cycles");
    if (wentBack) token(kValue, "since mark --  ");
    else          token(kValue, "since mark %llu ", static_cast<unsigned long long>(snap.cycles - cycleMark_));
    flowTo(ImGui::CalcTextSize("Reset mark").x + 12.0f);
    if (ImGui::SmallButton("Reset mark")) cycleMark_ = snap.cycles;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Count from here: stop at the start of the code to time,\n"
                          "Reset mark, then run to its end.");
    ImGui::SameLine(0.0f, 0.0f);
    if (haveLastRun_) token(kValue, "  last run or step %llu", static_cast<unsigned long long>(lastRunCycles_));
    else              token(kDimColour, "  last run or step --");
    token(kDimColour, "  total %llu", static_cast<unsigned long long>(snap.cycles));
    endRow();
}

// ── Navigation: wheel, Page Up/Down, Go to ──────────────────────────────

void Debugger_ImGui::scrollView(const Snapshot& snap, int instructions)
{
    if (snap.memory.size() < 0x10000 || instructions == 0) return;
    // Running and following: the listing is pinned to a moving PC, and a
    // stray wheel event must not freeze it. Untick the box to look around.
    if (snap.running && following()) return;
    lookingAround_ = true;
    viewAddr_ = disasmStep(snap.memory.data(), viewAddr_, instructions, snap.isa);
}

void Debugger_ImGui::drawNavigation(EmulationController& ctrl, const Snapshot& snap)
{
    ImGui::SetNextItemWidth(160);
    const bool entered = ImGui::InputTextWithHint("##goto", "$C600 or label", gotoEntry_,
                                                  sizeof(gotoEntry_),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Go to") || entered) && gotoEntry_[0]) {
        DebugAddress location;
        gotoFailed_ = !symbols_.resolveLocation(gotoEntry_, location);
        if (!gotoFailed_) {
            viewAddr_    = location.addr;
            lookingAround_ = true;
            cursorAddr_  = location.addr;
            cursorBank_  = location.bank.kind == DebugBank::Any ? snap.banks[location.addr]
                                                                : location.bank;
            cursorValid_ = true;
        }
    }
    ImGui::SameLine();
    bool beamView = snap.beamView;
    if (ImGui::Checkbox("Beam view", &beamView)) {
        auto st = ctrl.lockState();
        st.memory().setBeamView(beamView);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show the screen as the tube has it at THIS cycle: the frame in\n"
                          "progress above the beam, the previous frame from the beam down.\n"
                          "Off: the last completed frame. Use it when stopped in the middle\n"
                          "of a frame, or in slow motion (speed menu).");
    ImGui::SameLine();
    if (gotoFailed_) ImGui::TextColored(kBreakColour, "unknown address or label");
    else {
        ImGui::TextColored(kDimColour, "Keys (?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("AppleWin's keys, while this panel has the focus\n"
                              "(Ctrl is Ctrl or Cmd on a Mac):\n"
                              "  Space, F7        step\n"
                              "  Ctrl+Space       step over\n"
                              "  Shift+Space      step out\n"
                              "  Shift+F7         step BACK (needs the rewind recording)\n"
                              "  Ctrl+F7          run back to the previous breakpoint\n"
                              "  Up / Down        move the cursor (Shift: by one byte)\n"
                              "  Page Up / Down   a screenful; the wheel scrolls too\n"
                              "  Ctrl+Down        run to the cursor\n"
                              "  Right            follow the operand's target\n"
                              "  Ctrl+Right       set PC to the cursor\n"
                              "  Shift+Left       jump to the caller\n"
                              "  Tab              back to the PC\n"
                              "Right-click a line: break here, break when its operand is\n"
                              "read or written, watch it, run to here.\n"
                              "Click the Apple II screen to type into the machine again.");
    }
}

// ── Keyboard, with AppleWin's reflexes ──────────────────────────────────
// [D] AppleWin @ 7fd7fd7c, Debug.cpp DebuggerProcessKey: Space = trace,
// Ctrl+Space = step over, Shift+Space = step out; Up / Down move the cursor,
// with Shift by one byte; Ctrl+Down = run until the cursor; Page Up / Down;
// Right = follow the operand's target, Ctrl+Right = set PC to the cursor,
// Shift+Right and Tab = back to the PC; Shift+Left = jump to the caller.
// Only while this window has the focus, and the keys are then kept from the
// Apple II (in AppleWin the debugger owns the keyboard too).

void Debugger_ImGui::handleKeys(EmulationController& ctrl, const Snapshot& snap)
{
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextFrameWantCaptureKeyboard(true);
    if (io.WantTextInput || snap.memory.size() < 0x10000) return;
    const uint8_t* mem = snap.memory.data();
    const auto moveCursor = [&](uint16_t to) {
        cursorAddr_ = to; cursorBank_ = snap.banks[to]; cursorValid_ = true;
    };
    const auto show = [&](uint16_t to) { viewAddr_ = to; lookingAround_ = true; moveCursor(to); };
    const uint16_t cursor = cursorValid_ ? cursorAddr_ : snap.pc;
    // On macOS ImGui swaps Ctrl and Cmd: the physical Ctrl arrives as Super.
    // Only there — elsewhere Super is the Windows key, and Win+Down must not
    // run to the cursor.
    const bool ctrlKey = io.KeyCtrl || (io.ConfigMacOSXBehaviors && io.KeySuper);

    if (ImGui::IsKeyPressed(ImGuiKey_Space, /*repeat=*/true)) {
        lookingAround_ = false;
        if (io.KeyShift)     ctrl.debugStepOut();
        else if (ctrlKey)    ctrl.debugStepOver();
        else                 ctrl.debugStepInstruction();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) { lookingAround_ = false; moveCursor(snap.pc); }

    const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow), down = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
    if (down && ctrlKey) {
        if (cursorValid_) ctrl.debugRunToCursor(cursorAddr_, cursorBank_);
    } else if (up || down) {
        const int dir = down ? 1 : -1;
        if (io.KeyShift) {            // one BYTE: resynchronise a listing by hand
            viewAddr_ = static_cast<uint16_t>(viewAddr_ + dir);
            lookingAround_ = true;
        } else {
            const uint16_t to = disasmStep(mem, cursor, dir, snap.isa);
            moveCursor(to);
            // Keep the cursor on screen: scroll when it leaves the lines drawn.
            if (std::find(listed_.begin(), listed_.end(), to) == listed_.end())
                scrollView(snap, dir);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
        if (ctrlKey) {
            if (cursorValid_ && !snap.running) {
                auto st = ctrl.lockState();
                st.cpu().setProgramCounter(cursorAddr_);
                ctrl.noteChangedByHand();
            }
        } else if (io.KeyShift || io.KeyAlt) {
            lookingAround_ = false; moveCursor(snap.pc);
        } else {
            // The operand's target: an absolute address, or a branch's.
            int len = 1;
            (void)disassemble6502(mem, cursor, len, snap.isa);
            const uint8_t op = mem[cursor];
            const uint8_t b1 = mem[(cursor + 1) & 0xFFFF], b2 = mem[(cursor + 2) & 0xFFFF];
            const bool branch = (op & 0x1F) == 0x10 ||
                                (op == 0x80 && snap.isa != Cpu6502Set::Nmos6502);
            if (len == 3 && (op & 0x0F) == 0x0F && snap.isa == Cpu6502Set::Superset65C02)
                show(static_cast<uint16_t>(cursor + 3 + static_cast<int8_t>(b2)));   // BBR / BBS
            else if (len == 3)
                show(static_cast<uint16_t>(b1 | (b2 << 8)));
            else if (len == 2 && branch)
                show(static_cast<uint16_t>(cursor + 2 + static_cast<int8_t>(b1)));
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) && io.KeyShift) {
        // The caller: the return address on top of the stack, plus one.
        const uint16_t lo = mem[0x0100 + ((snap.sp + 1) & 0xFF)];
        const uint16_t hi = mem[0x0100 + ((snap.sp + 2) & 0xFF)];
        show(static_cast<uint16_t>((lo | (hi << 8)) + 1));
    }
}

// ── Disassembly, with a clickable breakpoint gutter ─────────────────────

void Debugger_ImGui::drawDisassembly(EmulationController& ctrl, const Snapshot& snap)
{
    if (snap.memory.size() < 0x10000) {
        ImGui::TextDisabled("memory not attached");
        return;
    }

    const int rows = std::max(8, static_cast<int>(
        ImGui::GetContentRegionAvail().y / ImGui::GetTextLineHeightWithSpacing()) - 1);

    // The wheel moves the listing by instructions (three a notch, as a text
    // editor scrolls three lines); Page Up / Page Down by a screenful. Ctrl +
    // wheel is left alone: it is POM2's interface zoom.
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowHovered() && io.MouseWheel != 0.0f && !io.KeyCtrl) {
        wheelAccum_ -= io.MouseWheel * 3.0f;
        const int whole = static_cast<int>(wheelAccum_);   // toward zero
        if (whole != 0) {
            wheelAccum_ -= static_cast<float>(whole);
            scrollView(snap, whole);
        }
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) scrollView(snap, rows - 4);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp))   scrollView(snap, -(rows - 4));
    }

    // Start three instructions above the anchor so the user sees where the
    // machine came from, not just where it is. Walking back through variable-
    // length code is a vote (DisasmNavigation.h): exact on real code, a guess
    // on data.
    uint16_t addr = disasmStep(snap.memory.data(), viewAddr_, -3, snap.isa);
    listed_.clear();

    for (int i = 0; i < rows; ++i) {
        int len = 1;
        const std::string mnem =
            disassemble6502(snap.memory.data(), addr, len, snap.isa);
        listed_.push_back(addr);
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
                                   : "Break at $%04X (right-click the line for more)", addr);
        ImGui::PopID();
        ImGui::SameLine();

        char bytes[12] = {0};
        for (int b = 0; b < len && b < 3; ++b)
            std::snprintf(bytes + b * 3, sizeof(bytes) - static_cast<std::size_t>(b) * 3,
                          "%02X ", snap.memory[(addr + b) & 0xFFFF]);

        // address  bytes  mnemonic   [own label]   ; what the operand is
        char line[192];
        const std::string label = symbols_.label(addr,location.bank);
        const std::string note  = operandNote(snap, addr, len);
        std::snprintf(line, sizeof(line), "%-11s %-9s %-12s %s%s%s%s", location.text().c_str(), bytes,
                      mnem.c_str(), label.c_str(), label.empty() || note.empty() ? "" : "  ",
                      note.empty() ? "" : "; ", note.c_str());

        if (isPc)      ImGui::PushStyleColor(ImGuiCol_Text, kPcColour);
        else if (isBp) ImGui::PushStyleColor(ImGuiCol_Text, kBreakColour);
        if (ImGui::Selectable(line, cursorValid_ && addr == cursorAddr_)) {
            cursorAddr_  = addr;
            cursorBank_ = location.bank;
            cursorValid_ = true;
        }
        if (isPc || isBp) ImGui::PopStyleColor();
        // Right click: the breakpoints and the watch this line can give.
        // (After the colour is popped: the menu is not the line's colour.)
        ImGui::PushID(static_cast<int>(addr) + 0x10000);
        if (ImGui::BeginPopupContextItem("##line")) {
            cursorAddr_ = addr; cursorBank_ = location.bank; cursorValid_ = true;
            // Hold the listing still under the menu: following a running PC
            // would take this line, and the menu with it, off the list.
            lookingAround_ = true;
            drawLineMenu(ctrl, snap, addr, len, mnem, isBp, wildcard);
            ImGui::EndPopup();
        }
        ImGui::PopID();

        addr = static_cast<uint16_t>(addr + len);
    }
}

// ── What the operand is ─────────────────────────────────────────────────

std::string Debugger_ImGui::operandNote(const Snapshot& snap, uint16_t addr, int len) const
{
    if (len != 3) return {};
    const uint8_t op = snap.memory[addr];
    // BBR / BBS are zero page + relative: no absolute operand.
    if ((op & 0x0F) == 0x0F && snap.isa == Cpu6502Set::Superset65C02) return {};
    const uint16_t target = static_cast<uint16_t>(snap.memory[(addr + 1) & 0xFFFF] |
                                                  (snap.memory[(addr + 2) & 0xFFFF] << 8));
    // The program's own name for it first: a symbol file is its vocabulary.
    std::string own = symbols_.label(target, snap.banks[target]);
    if (!own.empty() || !ioNames_) return own;
    if (const char* rom = monitorEntryName(target)) return rom;
    if (target < 0xC000 || target > 0xC7FF) return {};
    if (target < 0xC090) {
        const char* name = motherboardIoName(target, snap.iicClass);
        return name ? name : std::string{};
    }
    char text[64];
    if (target < 0xC100) {                       // $C0nX: slot n-8's switches
        const int slot = ((target >> 4) & 0x0F) - 8;
        if (snap.slotKind[slot] != 2) return {};
        std::snprintf(text, sizeof text, "Disk II slot %d: %s", slot, diskIISwitchName(target & 0x0F));
        return text;
    }
    const int slot = (target >> 8) & 0x07;        // $Cn00-$CnFF: slot n's page
    if (snap.slotKind[slot] != 1) return {};
    std::snprintf(text, sizeof text, "Mockingboard slot %d: VIA %d %s", slot,
                  (target & 0x80) ? 2 : 1, via6522RegisterName(target & 0x0F));
    return text;
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
        if (nativeFileDialog(false,"Load source map","p2map,dbg,txt","","",picked)==FileDialogResult::Selected) {
            if (picked.size()<sizeof(sourceMapPath_))
                std::snprintf(sourceMapPath_,sizeof(sourceMapPath_),"%s",picked.c_str());
            else sourceStatus_="The selected path is too long.";
        }
    }
    auto readBounded = [](const std::filesystem::path& path, std::string& text,
                          std::streamoff limit=4*1024*1024) {
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        const auto size=file ? file.tellg() : std::streampos{-1};
        if (size<0 || size>limit) return false;
        std::string value(static_cast<size_t>(size),'\0');
        file.seekg(0);
        if (!value.empty()) file.read(value.data(),static_cast<std::streamsize>(value.size()));
        if (!file) return false;
        text=std::move(value); return true;
    };
    if (ImGui::Button("Load source map")) {
        // A cc65 .dbg is converted to .p2map text and fed to the same
        // importer (Cc65DebugInfo.h); its file paths are relative to the
        // directory ld65 ran in, normally the .dbg's own.
        std::string text;
        const bool cc65=cc65dbg::isDebugFilePath(sourceMapPath_);
        cc65dbg::Conversion converted;
        if (!readBounded(std::filesystem::u8path(sourceMapPath_),text,
                         cc65 ? kCc65DebugFileLimit : 4*1024*1024))
            sourceStatus_=cc65 ? "Cannot read cc65 debug file (maximum 16 MiB)."
                               : "Cannot read source map (maximum 4 MiB).";
        else if (cc65 && !cc65dbg::toSourceMap(text,converted,sourceStatus_)) {}
        else if (sourceMap_.load(cc65 ? converted.text : text,sourceStatus_)) {
            sourceRoot_=std::filesystem::u8path(sourceMapPath_).parent_path().u8string();
            sourceStatus_=std::to_string(sourceMap_.size())+" source ranges loaded"+
                (cc65 ? " from cc65 debug info." : ".");
            sourceFile_.clear(); sourceLoaded_=false;
        }
    }
    ImGui::TextWrapped("A .p2map, or a cc65 .dbg (ld65 --dbgfile, assembled with -g). Map rows: aux0:$0800-$0802 \"src/main.s\" 42. Paths are relative to the map. Ranges use inclusive hexadecimal addresses; lines start at 1.");
    ImGui::TextWrapped("%s",sourceStatus_.c_str());
    const DebugAddress location{following() || !cursorValid_ ? snap.pc : cursorAddr_,
        following() || !cursorValid_ ? snap.pcBank : cursorBank_};
    const auto* entry=sourceMap_.lookup(location);
    if (!entry) { ImGui::TextDisabled("No source mapping for %s",location.text().c_str()); return; }
    ImGui::Text("%s -> %s:%u",location.text().c_str(),entry->file.c_str(),entry->line);
    if (ImGui::Button("Break at source line")) {
        auto st=ctrl.lockState();
        // Not over one that is there: adding again drops its condition.
        bool there=false;
        for (const DebugAddress& b : ctrl.debugger().bankBreakpoints()) there |= b==entry->start;
        if (!there) ctrl.debugger().addBreakpoint(entry->start.addr,entry->start.bank);
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
    {
        ScopedMonoFont mono;
        const unsigned first=entry->line > 4 ? entry->line-4 : 1;
        const unsigned last=std::min(entry->line+4,static_cast<unsigned>(sourceLines_.size()));
        for (unsigned number=first; number<=last; ++number) {
            if (number==entry->line) ImGui::PushStyleColor(ImGuiCol_Text,kPcColour);
            ImGui::Text("%c %5u  %s",number==entry->line ? '>' : ' ',number,sourceLines_[number-1].c_str());
            if (number==entry->line) ImGui::PopStyleColor();
        }
        if (sourceLines_.size()<entry->line) ImGui::TextDisabled("Source line is beyond end of file.");
    }
    ImGui::EndChild();
}

}  // namespace pom2

namespace pom2 {

// UI-017: every instruction to a file, and the profiler as a callgrind file.
// Both ask for the name; without a dialog they land in the user data folder.
void Debugger_ImGui::drawTraceAndExport(EmulationController& ctrl)
{
    auto pick = [](const char* title, const char* ext, const char* name) {
#ifdef __EMSCRIPTEN__
        // No save dialog in the browser: a scratch file in memory (not the
        // IndexedDB-backed /persistent), handed over as a download.
        (void)title; (void)ext;
        return (std::filesystem::path("/tmp") / name).u8string();
#else
        std::string path;
        const auto r = nativeFileDialogAvailable()
            ? nativeFileDialog(true, title, ext, "", name, path) : FileDialogResult::Unavailable;
        if (r == FileDialogResult::Cancelled) return std::string();
        if (r == FileDialogResult::Unavailable) path = (userDataDir() / name).u8string();
        return path;
#endif
    };
#ifndef __EMSCRIPTEN__
    // Not in the browser: ~50 MB an emulated second, in the page's memory,
    // with nowhere to stream it (bug hunt 2026-11-19).
    if (!trace_ || !trace_->isOpen()) {
        if (ImGui::Button("Trace to file...")) {
            const std::string path = pick("Save instruction trace", "txt", "pom2-trace.txt");
            if (!path.empty()) {
                auto t = std::make_unique<InstructionTrace>();
                t->setSymbols(symbols_);
                std::string why;
                // The file is created BEFORE the machine lock is taken: no
                // file I/O under stateMutex (CLAUDE.md); the trace is then
                // attached to the machine under the lock.
                if (t->openFile(std::filesystem::u8path(path), "", why)) {
                    auto st = ctrl.lockState();
                    t->attach(st.cpu(), st.memory());
                    trace_ = std::move(t);
                    ctrl.debugger().setInstructionTrace(trace_.get());
                    ctrl.syncDebugHook();
                    traceStatus_ = "tracing to " + path;
                } else {
                    traceStatus_ = why;
                }
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Every executed instruction, one line each: PC, physical bank,\n"
                              "bytes, disassembly, the registers going in, cycles.\n"
                              "Large: about 50 bytes per instruction, 50 MB per emulated second.");
    } else {
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "TRACE");
        ImGui::SameLine();
        ImGui::Text("%llu lines", static_cast<unsigned long long>(trace_->lines()));
        if (trace_->stopped()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "(stopped: %s)",
                               trace_->stopReason().c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop trace")) {
            {
                auto st = ctrl.lockState();
                ctrl.debugger().setInstructionTrace(nullptr);
                ctrl.syncDebugHook();
            }
            const auto lines = trace_->lines();
            traceStatus_ = trace_->close()
                ? std::to_string(lines) + " lines written"
                : "the trace could not be written in full (disk full?)";
            trace_.reset();
        }
    }
    ImGui::SameLine();
#endif
    if (ImGui::Button("Export callgrind...")) {
        std::vector<Debugger::ProfileEntry> profile;
        {
            auto st = ctrl.lockState();
            profile = ctrl.debugger().profile(SIZE_MAX);
        }
        if (profile.empty()) {
            traceStatus_ = "nothing profiled yet: tick \"Profile CPU steps\" and run";
        } else {
            const std::string path = pick("Export profile (callgrind)", "callgrind", "callgrind.out.pom2");
            if (!path.empty()) {
                const std::string text = callgrindProfile(profile, symbols_.size() ? &symbols_ : nullptr, "POM2");
                std::error_code ec;
                traceStatus_ = writeFileAtomic(std::filesystem::u8path(path), text.data(), text.size(), ec)
                    ? "profile saved to " + path + " (open it in KCachegrind)"
                    : "cannot write " + path + ": " + ec.message();
#ifdef __EMSCRIPTEN__
                if (!ec) {
                    std::string err;
                    traceStatus_ = downloadBrowserFile(path, "callgrind.out.pom2", err)
                        ? "profile downloaded (callgrind.out.pom2)" : "download failed: " + err;
                    std::filesystem::remove(std::filesystem::u8path(path), ec);
                }
#endif
            }
        }
    }
    if (!traceStatus_.empty()) ImGui::TextWrapped("%s", traceStatus_.c_str());
}

} // namespace pom2
