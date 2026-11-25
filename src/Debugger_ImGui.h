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

// Debugger_ImGui — the window you interrogate a running Apple II from.
//
// Registers, a disassembly that follows the PC, a breakpoint gutter you click,
// and the five verbs that matter: Run, Stop, Step, Step Over, Run To Cursor.
// The stop banner says WHY the machine is stopped, because "it stopped" and
// "it stopped because you asked it to break at $C600" are different facts and
// only one of them is useful.
//
// Everything lives here. `MainWindow` owns the object and calls `render()`;
// it holds no debugger state and makes no debugger decisions, which is the
// rule the file-size ratchet (tools/check_file_sizes.sh) exists to keep.
//
// ── Locking ──────────────────────────────────────────────────────────────
// The panel runs on the UI thread and every emulator read or write it makes
// goes through `stateMutex` — the debugger's bitmaps are consulted by the CPU
// worker inside that same lock (Debugger.h § Threading). The snapshot pattern
// is the one the rest of POM2's panels use: take a VALUE copy of everything
// under the lock, release it, then draw. Holding it across ImGui calls would
// freeze the machine for the length of a repaint.

#ifndef POM2_DEBUGGER_IMGUI_H
#define POM2_DEBUGGER_IMGUI_H

#include "Debugger.h"
#include "Memory.h"
#include "DebuggerSymbols.h"
#include "DebuggerSourceMap.h"
#include "InstructionTrace.h"
#include "Opcode6502.h"
#include "CpuClock.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class EmulationController;

namespace pom2 {

class Debugger_ImGui
{
public:
    /// Draw the window. `open` is the caller's visibility flag, wired to the
    /// window's close button in the usual ImGui way; when false this returns
    /// immediately having taken no lock.
    void render(EmulationController& ctrl, bool* open);

private:
    /// A value snapshot of everything the panel draws, taken under the lock
    /// in one go. Nothing below is a pointer into live emulator state — that
    /// was the 2026-08-02 disk-path race, and it applies here for the same
    /// reason (see tests/disk_path_snapshot_test.cpp).
    struct Snapshot {
        uint8_t  a = 0, x = 0, y = 0, p = 0, sp = 0;
        uint16_t pc = 0;
        /// The core's instruction set (`M6502::instructionSet()`), NCR
        /// included — the listing and the assembler decode this.
        Cpu6502Set isa   = Cpu6502Set::Nmos6502;
        bool     running = false;
        bool     halted  = false;
        Debugger::Hit hit{};
        std::vector<DebugAddress> breakpoints;
        std::vector<DebugBank> banks;
        DebugBank pcBank;
        std::vector<Debugger::Watch> watchpoints;
        std::map<DebugAddress, Debugger::Condition> conditions;
        bool profiling = false, historyEnabled = false;
        std::vector<Debugger::ProfileEntry> profile;
        std::vector<Debugger::TraceEntry> history;
        /// Interrupt share of each completed video frame, oldest first, in
        /// percent (the histogram's input), and its summary.
        std::vector<float> interruptLoad;
        InterruptLoad::Summary interruptSummary;
        uint32_t interruptEntriesLast = 0;
        pom2::OsTraceKinds trace;
        /// 64 KiB of main RAM, copied so the disassembly can be laid out
        /// after the lock is released.
        std::vector<uint8_t> memory;
        /// CPU cycles since power-on (`Memory::getCycleCounter`) — the
        /// cycle counter's clock — and the BEAM's clock (`videoNow`), which
        /// an accelerator leaves at the stock rate, for the H/V readout.
        uint64_t cycles = 0;
        uint64_t videoCycle = 0;
        VideoStandard standard = VideoStandard::NTSC;
        bool beamView = false;
        /// For the listing's built-in I/O names (AppleIoNames.h): what sits
        /// in each slot (0 nothing known, 1 a 6522 sound card, 2 a Disk II),
        /// and whether this is a //c-class machine.
        Memory::DisplayState video;      ///< the live video switches
        uint8_t slotKind[8] = {};
        bool iicClass = false;
    };
    /// "; PAGE2 on (page 2)" for the instruction at `addr`: the operand's
    /// label from the loaded symbols, else the machine's own I/O name.
    std::string operandNote(const Snapshot& snap, uint16_t addr, int len) const;
    bool ioNames_ = true;

    void drawControls(EmulationController& ctrl, const Snapshot& snap);
    void drawRegisters(const Snapshot& snap);
    void drawDisassembly(EmulationController& ctrl, const Snapshot& snap);
    void drawBreakpointList(EmulationController& ctrl, const Snapshot& snap);
    void drawWatchpointList(EmulationController& ctrl, const Snapshot& snap);
    void drawStopBanner(const Snapshot& snap);
    void drawSource(EmulationController& ctrl, const Snapshot& snap);
    void drawAssembler(EmulationController& ctrl, const Snapshot& snap);

    /// The OS-call trace, re-copied only when it has grown.
    std::vector<std::string> osLines_;
    uint64_t osEmitted_ = ~uint64_t(0);
    void drawOsCalls(EmulationController& ctrl, Snapshot& snap);

    /// Disassembly origin. Follows the PC unless the user has scrolled away.
    uint16_t  viewAddr_    = 0;
    bool      followPc_    = true;
    /// The machine was running at the previous frame: a stop then brings the
    /// view back to the PC even after the user scrolled away.
    bool      wasRunning_  = false;
    /// "Go to" box: a hex address, or a label from the loaded symbols.
    char      gotoEntry_[64] = {0};
    void drawNavigation(EmulationController& ctrl, const Snapshot& snap);
    /// AppleWin's debugger keys, while this window has the focus.
    void handleKeys(EmulationController& ctrl, const Snapshot& snap);
    /// Addresses of the lines the listing drew last frame (cursor keys keep
    /// the cursor among them).
    std::vector<uint16_t> listed_;
    /// Scroll the listing by `instructions` (negative = up) and stop
    /// following the PC.
    void scrollView(const Snapshot& snap, int instructions);

    bool      gotoFailed_  = false;
    /// Fractional wheel travel (trackpads send fractions of a notch).
    float     wheelAccum_  = 0.0f;

    /// The resettable cycle counter: cycles counted from `cycleMark_`.
    uint64_t  cycleMark_ = 0;
    /// Cycles of the last run or step: the counter where the machine last
    /// stood still, and how far it moved before standing still again —
    /// "break at A, Run, break at B" reads B - A with no click.
    uint64_t  stopCycles_   = 0;
    uint64_t  lastRunCycles_ = 0;
    bool      haveStop_ = false, haveLastRun_ = false;
    void drawBeamAndCycles(const Snapshot& snap);
    /// Address typed into the "add breakpoint" box, as text so a half-typed
    /// value does not jump the view.
    char      bpEntry_[256]  = {0};
    bool conditionEnabled_ = false;
    int conditionRegister_ = 0;
    int conditionCompare_ = 0;
    char conditionValue_[8] = {"00"};
    char symbolPath_[1024] = {};
    DebuggerSymbols symbols_;
    DebuggerSourceMap sourceMap_;
    char sourceMapPath_[1024] = {};
    std::string sourceStatus_, sourceRoot_, sourceFile_;
    std::vector<std::string> sourceLines_;
    bool sourceLoaded_ = false;
    std::string symbolStatus_;
    /// UI-017: the instruction trace this panel attached (null = none) and
    /// what the last trace / callgrind export said.
    std::unique_ptr<InstructionTrace> trace_;
    std::string traceStatus_;
    void drawTraceAndExport(EmulationController& ctrl);
    char assemblyAddress_[8] = {"0300"};
    char assemblySource_[128] = {"LDA #$00"};
    std::string assemblyStatus_;
    /// Same, for the watchpoint box. Separate storage so typing in one does
    /// not clear the other.
    char      wpEntry_[256]  = {0};
    /// Access the next watchpoint is armed with (Debugger::Access bits).
    /// Defaults to Write: it is free while armed, a read watch is not.
    int       wpAccess_    = 2;
    /// The address the user last clicked in the disassembly — the target for
    /// Run To Cursor.
    uint16_t  cursorAddr_  = 0;
    DebugBank cursorBank_{};
    bool      cursorValid_ = false;
};

} // namespace pom2

#endif // POM2_DEBUGGER_IMGUI_H
