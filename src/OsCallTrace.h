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

// OS-call trace: every ProDOS MLI call the 6502 makes and every CP/M BDOS
// call a card Z80 makes, decoded and paired with its result.
//
//   MLI   JSR $BF00 / .byte cmd / .word params, return to the JSR + 6 with
//         A = error and C set on one (ProDOS 8 Technical Reference § 4.1).
//   BDOS  CALL $0005 with the function in C and the argument in DE, return
//         in A (= L) and HL (CP/M 2.2 Interface Guide § 5; 3.x adds HL).
//   SP    JSR dispatch / .byte cmd / .word list (.long list when cmd has
//         bit 6, the extended set), dispatch = $Cn00 + $CnFF + 3 of a slot
//         whose page carries the SmartPort ID ($Cn01/03/05/07 = $20/00/03/00);
//         return past the inline bytes with A = error, C set on one, X/Y =
//         the byte count (Apple IIc Technical Reference ch. 7, IIgs Firmware
//         Reference ch. 7).
//   PASC  the Apple Pascal 1.x BIOS jump table, whose address the system
//         keeps at $EC/$ED: entries only — the BIOS returns through the
//         p-machine, not to the JSR — and console read/write left out, as
//         izapple2's tracePascal.go does. The table must look like one —
//         its first three entries the same JMP or JSR (1.1's is JSR $FF9E
//         ×n in the language card) — so a stray $EC/$ED does not log.
//   MON   the Monitor's character output (COUT, $FDED, while it holds its
//         JMP (CSW)), one line per carriage return, and each GETLN ($FD6A /
//         $FD67 / $FD6F) paired with the line it returns ($0200, X long) —
//         izapple2's traceMonitor.go, with the echo of the typed line kept out.
//   DISK  a card's own lines (CardTraceSink.h): the Disk II's seeks, motor,
//         drive select and writes, hung on the cards by SlotBus::setCardTrace.
//   MOS   Applecorn's BBC Micro MOS calls (bobbimanners/Applecorn): while the
//         CPU reads aux RAM (RAMRD), an entry into the kernel at $D000+ from
//         below $C000 is a call, named through the page-2 vectors and the
//         $FFxx jump block, and the way back out is its result — plus the
//         language ($8000) and service ($8003) entries of the paged ROM.
//         After izapple2's traceApplecorn.go.
//   CPM65 CP/M-65's BDOS (function in Y, parameter in XA, C on error; paired
//         with its return) and BIOS (entries) — David Given's cpm65. The
//         entries move with every build (izapple2 hard-codes one), so they
//         are recognised by their code: the BDOS's STA/STX/JSR/JSR icall/JMP
//         prologue and the BIOS's jmpdispatch over a 16-entry table.
//
// A call is caught at its entry address and closed when the CPU reaches the
// return address at the stack depth the call will return to — so a nested
// call, an interrupt or an inline-argument routine cannot close it early,
// and one that never returns (MLI QUIT, BDOS 0, an error unwind) is reported
// as such when the stack passes beneath it. Idea and call tables after
// izapple2's traceProDOS.go and traceCpm.go.
//
// Cost: the MLI half rides the Debugger's per-instruction hook, so it puts
// the CPU on the debugged loop only while it is on (Debugger::armed). The
// BDOS half is a Z80Observer that SlotBus hangs on the card Z80s only while
// it is on. Off, neither costs anything. Guarded by stateMutex like the
// Debugger that owns it.

#ifndef POM2_OS_CALL_TRACE_H
#define POM2_OS_CALL_TRACE_H

#include "Z80.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

class M6502;
class Memory;

namespace pom2 {

/// Which traces are on — `EmulationController::setOsCallTrace` takes it.
struct OsTraceKinds {
    bool mli = false, bdos = false, smartPort = false, pascalBios = false;
    bool monitor = false, disk = false, applecorn = false, cpm65 = false;
};

class OsCallTrace : public Z80Observer
{
public:
    void bind(const M6502* cpu, const Memory* memory) { cpu_ = cpu; mem_ = memory; }

    void setMli(bool on)  { mli_ = on; if (!on) mliPending_.clear(); }
    void setBdos(bool on) { bdos_ = on; if (!on) bdosPending_.clear(); }
    void setSmartPort(bool on) { sp_ = on; if (!on) spPending_.clear(); }
    void setPascal(bool on) { pascal_ = on; }
    void setMonitor(bool on) { mon_ = on; if (!on) { monPending_.clear(); monLine_.clear(); monEcho_.clear(); } }
    void setApplecorn(bool on) { acorn_ = on; acornInKernel_ = false; }
    void setCpm65(bool on) { cpm65_ = on; if (!on) cpmPending_.clear(); }
    /// Only recorded here: the lines come from the cards, on which the
    /// controller hangs this trace (SlotBus::setCardTrace).
    void setDisk(bool on) { disk_ = on; }
    bool disk() const { return disk_; }
    OsTraceKinds kinds() const
    {
        OsTraceKinds k;
        k.mli = mli_; k.bdos = bdos_; k.smartPort = sp_; k.pascalBios = pascal_;
        k.monitor = mon_; k.disk = disk_; k.applecorn = acorn_; k.cpm65 = cpm65_;
        return k;
    }
    bool mli() const  { return mli_; }
    bool bdos() const { return bdos_; }
    bool smartPort() const { return sp_; }
    bool pascalBios() const { return pascal_; }  // not pascal(): a macro on Windows
    bool monitor() const { return mon_; }
    bool applecorn() const { return acorn_; }
    bool cpm65() const { return cpm65_; }
    /// Anything traced off the 6502 — what keeps the Debugger's hook armed.
    bool any6502() const { return mli_ || sp_ || pascal_ || mon_ || acorn_ || cpm65_; }

    /// Each finished line, as it is made (the headless frontend's stderr).
    using Sink = std::function<void(const std::string&)>;
    void setSink(Sink sink) { sink_ = std::move(sink); }

    /// The 6502 is about to run the instruction at `pc` (Debugger hook).
    void on6502Instruction(uint16_t pc)
    {
        if (mli_ && (pc == 0xBF00 || (!mliPending_.empty() && pc == mliPending_.back().ret)))
            mliStep(pc);
        if (sp_ && ((pc >= 0xC100 && pc < 0xC800) ||
                    (!spPending_.empty() && pc == spPending_.back().ret)))
            spStep(pc);
        if (pascal_) pascalStep(pc);
        if (mon_ && ((pc >= 0xFD00 && pc < 0xFE00) || !monPending_.empty()))
            monStep(pc);
        if (acorn_) acornStep(pc);
        if (cpm65_) cpm65Step(pc);
    }
    void onZ80Instruction(const Z80& z80) override;
    void onCardLine(const char* line) override { emit(line); }

    /// Newest last; at most kCapacity lines.
    static constexpr std::size_t kCapacity = 4000;
    const std::deque<std::string>& lines() const { return lines_; }
    /// Lines ever emitted — a UI notices new ones without copying.
    uint64_t emitted() const { return emitted_; }
    void clear() { lines_.clear(); }
    /// The machine jumped in time: open calls name a timeline that is gone.
    void dropPending()
    {
        mliPending_.clear(); bdosPending_.clear(); spPending_.clear();
        monPending_.clear(); cpmPending_.clear(); acornInKernel_ = false;
    }

    static const char* mliName(uint8_t cmd);
    static const char* prodosError(uint8_t code);
    static const char* bdosName(uint8_t fn);
    static const char* smartPortName(uint8_t cmd);     ///< bit 6 (extended) ignored
    static const char* smartPortError(uint8_t code);
    static const char* pascalName(uint8_t offset);     ///< nullptr off the table
    static const char* cpm65BdosName(uint8_t fn);
    static const char* cpm65BiosName(uint8_t fn);

private:
    struct Pending {
        uint16_t ret;      // where the call comes back to
        uint16_t sp;       // the stack pointer it comes back with
        uint8_t  fn;
        uint16_t arg;      // MLI / SmartPort parameter list, BDOS DE
        std::string text;  // the call, decoded at entry
    };
    void mliStep(uint16_t pc);
    void spStep(uint16_t pc);
    void pascalStep(uint16_t pc);
    void monStep(uint16_t pc);
    void acornStep(uint16_t pc);
    void cpm65Step(uint16_t pc);
    bool isCpm65Bdos(uint16_t pc) const;
    bool isCpm65Bios(uint16_t pc) const;
    std::string terminated(uint16_t a, uint8_t end, std::size_t max = 64) const;
    std::string spCall(uint8_t cmd, uint32_t list) const;
    std::string spResult(const Pending& p, uint8_t a, bool carry) const;
    std::string mliCall(uint8_t cmd, uint16_t params) const;
    std::string mliResult(const Pending& p, uint8_t a, bool carry) const;
    std::string bdosCall(const Z80& z, uint8_t fn, uint16_t de) const;
    void emit(std::string line);
    /// Close every open call the stack has passed beneath (`sp` is where the
    /// new call returns to, or the live SP).
    void abandon(std::vector<Pending>& open, uint16_t sp, uint16_t mask, const char* who);

    uint8_t peek(uint16_t a) const;
    std::string pathAt(uint16_t a) const;

    const M6502* cpu_ = nullptr;
    const Memory* mem_ = nullptr;
    bool mli_ = false, bdos_ = false, sp_ = false, pascal_ = false;
    bool mon_ = false, acorn_ = false, cpm65_ = false, disk_ = false;
    std::vector<Pending> mliPending_, bdosPending_, spPending_, monPending_, cpmPending_;
    std::string monLine_;                  // COUT output since the last CR
    std::string monEcho_;                  // what the open GETLN echoed
    int monEchoState_ = 0;                 // 0 its prompt, 1 the keys, 2 past the CR
    bool acornInKernel_ = false;
    struct AcornCall { std::string text; uint8_t a = 0, x = 0, y = 0; uint16_t api = 0; bool log = false; };
    AcornCall acornCall_;
    uint32_t cpmSector_ = 0;               // the last BIOS SETSEC / SETDMA
    uint16_t cpmDma_ = 0;
    std::deque<std::string> lines_;
    uint64_t emitted_ = 0;
    Sink sink_;
};

} // namespace pom2

#endif // POM2_OS_CALL_TRACE_H
