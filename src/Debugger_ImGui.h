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
#include "DebuggerSymbols.h"
#include "DebuggerSourceMap.h"

#include <cstdint>
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
        bool     cmos    = false;
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
        bool traceMli = false, traceBdos = false;
        /// 64 KiB of main RAM, copied so the disassembly can be laid out
        /// after the lock is released.
        std::vector<uint8_t> memory;
    };

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
