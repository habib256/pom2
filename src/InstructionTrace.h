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

// InstructionTrace — every executed 6502 instruction, one line each, to a
// file (UI-017). What a2trace builds by post-processing a MAME trace, here
// from the machine itself: the PHYSICAL bank the instruction ran from
// (main / aux / lc / rom / slot — DebugAddress.h), the registers going IN,
// the cycles it took.
//
// The line is a FORMAT STRING with these tokens:
//
//   %PC     program counter, 4 hex digits      %A %X %Y %P %SP  2 hex digits
//   %BANK   physical bank (main, aux0, lc1…)   %FLAGS  NV-BDIZC, lowercase = clear
//   %BYTES  the instruction's bytes, 8 wide    %CYC    cycles it took (decimal)
//   %DIS    disassembly, 16 wide               %TOTAL  machine cycle at its start
//   %LABEL  symbol at the PC, if one is loaded %%      a percent sign
//
// Registers and bytes are read BEFORE the instruction runs (self-modifying
// code traces as executed); the line is written once it completed, when its
// cycles are known. An instruction a breakpoint stopped is not written; an
// interrupt taken in its place writes a `---- interrupt -> $XXXX` line.

#ifndef POM2_INSTRUCTION_TRACE_H
#define POM2_INSTRUCTION_TRACE_H

#include "DebuggerSymbols.h"
#include "InstructionTraceSink.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

class Memory;

namespace pom2 {

class InstructionTrace : public InstructionTraceSink
{
public:
    static constexpr const char* kDefaultFormat =
        "%PC %BANK  %BYTES  %DIS  A=%A X=%X Y=%Y P=%P SP=%SP  %FLAGS  %CYC";

    /// What one line is made of, already read from the machine.
    struct Fields {
        uint16_t pc = 0;
        DebugBank bank{};
        uint8_t bytes[3] = {};
        int length = 1;
        std::string disassembly, label;
        uint8_t a = 0, x = 0, y = 0, p = 0, sp = 0;
        int cycles = 0;
        uint64_t total = 0;
    };

    ~InstructionTrace() override;

    /// `cpu` and `memory` are the machine's own; they outlive the trace.
    bool open(const std::filesystem::path& path, std::string format,
              const M6502& cpu, const Memory& memory, std::string& error);
    /// The same in two steps, for a caller that must not do file I/O under
    /// `stateMutex`: create the file first, then attach to the machine under
    /// the lock (nothing is written before both are done).
    bool openFile(const std::filesystem::path& path, std::string format, std::string& error);
    void attach(const M6502& cpu, const Memory& memory) { cpu_ = &cpu; memory_ = &memory; }
    void close();
    bool isOpen() const { return out_ != nullptr; }
    /// The trace stopped writing: it reached `maxLines`, or the disk said
    /// no (full, gone). It stays attached and costs nothing from then on;
    /// what it wrote is closed cleanly. Readable from another thread.
    bool stopped() const { return stopped_.load(std::memory_order_relaxed); }
    std::string stopReason() const;
    /// Lines before the trace stops by itself. ~50 bytes a line: the default
    /// 20 000 000 is about 1 GB, a few emulated seconds — the cap a forgotten
    /// trace needs (the CPU writes it under the machine lock, and a disk turbo
    /// runs ~60x). 0 = no cap.
    void setMaxLines(uint64_t lines) { maxLines_ = lines; }
    /// Safe from another thread (the panel shows it while the CPU writes).
    uint64_t lines() const { return lines_.load(std::memory_order_relaxed); }
    const std::string& format() const { return format_; }

    /// Labels for %LABEL (a copy: the panel may reload its own).
    void setSymbols(const DebuggerSymbols& symbols) { symbols_ = symbols; }

    /// The line for `f` under `format`, without a newline. Pure: the tests
    /// pin the format with it, and the GUI previews it.
    static std::string formatLine(const std::string& format, const Fields& f);

    // InstructionTraceSink
    void before(uint16_t pc, DebugBank bank) override;
    void after(const M6502& cpu, int cycles) override;
    void interrupted(const M6502& cpu, int cycles) override;

private:
    std::FILE* out_ = nullptr;
    std::string format_;
    const M6502* cpu_ = nullptr;
    const Memory* memory_ = nullptr;
    DebuggerSymbols symbols_;
    Fields pending_;
    bool havePending_ = false;
    std::atomic<uint64_t> lines_{0};
    uint64_t maxLines_ = 20000000;
    std::atomic<bool> stopped_{false};
    std::atomic<bool> writeFailed_{false};
    void write(const std::string& text);
};

} // namespace pom2

#endif // POM2_INSTRUCTION_TRACE_H
