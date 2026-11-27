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

// Run-control debugger — see Debugger.h for the design and the locking rule.

#include "Debugger.h"
#include "Memory.h"

#include <algorithm>

namespace pom2 {

namespace {
constexpr std::size_t kAddressSpace = 0x10000;
constexpr std::size_t kBpBytes      = kAddressSpace / 8;   // one bit each
}  // namespace

void Debugger::ensureBpBits()
{
    if (bpBits_.empty()) bpBits_.assign(kBpBytes, 0);
}

void Debugger::ensureWpBits()
{
    if (wpBits_.empty()) wpBits_.assign(kAddressSpace, 0);
}

// ── Breakpoints ──────────────────────────────────────────────────────────

void Debugger::addBreakpoint(uint16_t addr)
{
    conditions_.erase(addr);
    ensureBpBits();
    uint8_t& byte = bpBits_[addr >> 3];
    const uint8_t bit = static_cast<uint8_t>(1u << (addr & 7));
    if (byte & bit) return;                    // already set — keep the count honest
    byte = static_cast<uint8_t>(byte | bit);
    ++bpCount_;
}

void Debugger::removeBreakpoint(uint16_t addr)
{
    conditions_.erase(addr);
    bpGates_.erase({addr, {}});
    if (bpBits_.empty()) return;
    uint8_t& byte = bpBits_[addr >> 3];
    const uint8_t bit = static_cast<uint8_t>(1u << (addr & 7));
    if (!(byte & bit)) return;
    byte = static_cast<uint8_t>(byte & ~bit);
    --bpCount_;
}

void Debugger::toggleBreakpoint(uint16_t addr)
{
    if (hasBreakpoint(addr)) removeBreakpoint(addr);
    else                     addBreakpoint(addr);
}

void Debugger::clearBreakpoints()
{
    conditions_.clear();
    bankBreakpoints_.clear();
    bankConditions_.clear();
    bpGates_.clear();
    // Freed rather than zeroed: an un-armed debugger should hold no memory,
    // and `armed()` keying off the count means the CPU goes back to its fast
    // loop the moment the last breakpoint goes.
    bpBits_.clear();
    bpBits_.shrink_to_fit();
    bpCount_ = 0;
}

bool Debugger::hasBreakpoint(uint16_t addr) const
{
    if (bpBits_.empty()) return false;
    return (bpBits_[addr >> 3] >> (addr & 7)) & 1u;
}

void Debugger::addConditionalBreakpoint(uint16_t addr, Condition condition,
                                        const M6502& cpu)
{
    addBreakpoint(addr);
    conditionCpu_ = &cpu;
    conditions_[addr] = condition;
}

bool Debugger::conditionMatches(uint16_t addr) const
{
    const auto found = conditions_.find(addr);
    if (found == conditions_.end()) return true;
    return conditionMatches(found->second);
}

void Debugger::setCondition(DebugAddress at, bool on, Condition condition, const M6502& cpu)
{
    conditionCpu_ = &cpu;
    if (at.bank.kind == DebugBank::Any) {
        if (!hasBreakpoint(at.addr)) return;
        if (on) conditions_[at.addr] = condition; else conditions_.erase(at.addr);
    } else {
        if (!bankBreakpoints_.count(at)) return;
        if (on) bankConditions_[at] = condition; else bankConditions_.erase(at);
    }
}

bool Debugger::compare(Compare how, unsigned value, unsigned against)
{
    switch (how) {
        case Compare::Equal:        return value == against;
        case Compare::NotEqual:     return value != against;
        case Compare::Less:         return value <  against;
        case Compare::LessEqual:    return value <= against;
        case Compare::Greater:      return value >  against;
        case Compare::GreaterEqual: return value >= against;
    }
    return false;
}

bool Debugger::beamInside(const Gate& g, int line, int h)
{
    const auto within = [](int v, int lo, int hi) {
        return lo <= hi ? (v >= lo && v <= hi) : (v >= lo || v <= hi);   // wraps
    };
    return within(line, g.vMin, g.vMax) && within(h, g.hMin, g.hMax);
}

void Debugger::setBreakpointGate(DebugAddress at, const Gate& gate)
{
    const bool there = at.bank.kind == DebugBank::Any ? hasBreakpoint(at.addr)
                                                      : bankBreakpoints_.count(at) != 0;
    if (there) bpGates_[at] = GateState{gate};
}

void Debugger::setWatchGate(DebugAddress at, const Gate& gate)
{
    const bool there = at.bank.kind == DebugBank::Any ? watchpointAt(at.addr) != None
                                                      : bankWatches_.count(at) != 0;
    if (there) wpGates_[at] = GateState{gate};
}

void Debugger::resetPassCounts()
{
    for (auto& g : bpGates_) g.second.passes = g.second.countedAt = 0;
    for (auto& g : wpGates_) g.second.passes = g.second.countedAt = 0;
}

// One breakpoint, one pass: is the beam in its window, does the byte match,
// and has it let enough passes go by. `value` is -1 for a PC breakpoint.
//
// `mayRepeat`: one instruction can put the SAME kind of access on the SAME
// address twice — the dummy cycle of a read-modify-write. An NMOS part
// writes the original value back before the result; a 65C02 reads the
// operand a second time. Both reach here, and the program made ONE read and
// ONE write: the earlier of two same-kind accesses has its pass taken back
// before the later is judged — else `INC $10` counted two write passes on a
// 6502 and two read passes on a 65C02, and a "written == $05" gate stopped
// on a byte that ends up $06. A read and a write are two accesses: on a
// read-and-write breakpoint each keeps its pass.
void Debugger::uncountPass(GateMap& gates, const DebugAddress& key)
{
    const auto found = gates.find(key);
    if (found == gates.end()) return;
    GateState& s = found->second;
    if (s.countedAt != serial_ || !s.passes) return;
    --s.passes;
    s.countedAt = 0;
}

bool Debugger::gateOpens(GateMap& gates, const DebugAddress& key, uint64_t videoCycle,
                         int value, bool write, bool mayRepeat)
{
    GateState& s = gates[key];
    if (!replaying_ && mayRepeat && s.countedWrite == write) uncountPass(gates, key);
    const Gate& g = s.gate;
    if (g.beam && memory_) {
        const VideoTiming& t = pom2VideoTiming(memory_->videoStandard());
        const uint64_t perLine = static_cast<uint64_t>(t.cyclesPerScanline);
        const uint64_t inFrame = videoCycle % (perLine * static_cast<uint64_t>(t.scanlinesPerFrame));
        if (!beamInside(g, static_cast<int>(inFrame / perLine), static_cast<int>(inFrame % perLine)))
            return false;
    }
    if (g.valueOn && value >= 0 && !compare(g.valueCompare, static_cast<unsigned>(value), g.value))
        return false;
    if (replaying_) return true;          // the past, read again: nothing counts
    s.countedAt = serial_;
    s.countedWrite = write;
    return ++s.passes > g.ignore;
}

bool Debugger::conditionMatches(const Condition& c) const
{
    if (!conditionCpu_) return false;
    uint16_t value = 0;
    switch (c.reg) {
        case Register::A: value = conditionCpu_->getAccumulator(); break;
        case Register::X: value = conditionCpu_->getXRegister(); break;
        case Register::Y: value = conditionCpu_->getYRegister(); break;
        case Register::P: value = conditionCpu_->getStatusRegister(); break;
        case Register::SP: value = conditionCpu_->getStackPointer(); break;
        case Register::PC: value = conditionCpu_->getProgramCounter(); break;
    }
    return compare(c.compare, value & c.mask, c.value);
}

std::vector<uint16_t> Debugger::breakpoints() const
{
    std::vector<uint16_t> out;
    if (bpBits_.empty()) return out;
    out.reserve(bpCount_);
    // A plain bit loop, not a count-trailing-zeros intrinsic: `__builtin_ctz`
    // is GCC/Clang only and MSVC rejects it outright (caught by the Windows CI
    // job, on a push rather than at tag time — which is what that job is for).
    // There is no portability cost to pay here anyway: this runs once per UI
    // frame over 8 KiB, never on the CPU's path.
    for (std::size_t i = 0; i < kBpBytes; ++i) {
        const uint8_t byte = bpBits_[i];
        if (!byte) continue;
        for (int bit = 0; bit < 8; ++bit)
            if (byte & (1u << bit))
                out.push_back(static_cast<uint16_t>(i * 8 + static_cast<std::size_t>(bit)));
    }
    return out;
}

// ── Watchpoints ──────────────────────────────────────────────────────────

void Debugger::setWatchpoint(uint16_t addr, Access access)
{
    if (access == None) {
        wpGates_.erase({addr, {}});
        if (wpBits_.empty()) return;
        if (wpBits_[addr] != None) --wpCount_;
        wpBits_[addr] = None;
        if (wpCount_ == 0) { wpBits_.clear(); wpBits_.shrink_to_fit(); }
        return;
    }
    ensureWpBits();
    if (wpBits_[addr] == None) ++wpCount_;
    wpBits_[addr] = static_cast<uint8_t>(access);
}

Debugger::Access Debugger::watchpointAt(uint16_t addr) const
{
    if (wpBits_.empty()) return None;
    return static_cast<Access>(wpBits_[addr]);
}

void Debugger::clearWatchpoints()
{
    bankWatches_.clear();
    wpGates_.clear();
    wpBits_.clear();
    wpBits_.shrink_to_fit();
    wpCount_ = 0;
}

std::vector<Debugger::Watch> Debugger::watchpoints() const
{
    std::vector<Watch> out;
    for (const auto& w : bankWatches_) out.push_back({w.first.addr, w.second, w.first.bank});
    if (wpBits_.empty()) return out;
    out.reserve(wpCount_);
    for (std::size_t a = 0; a < kAddressSpace; ++a)
        if (wpBits_[a] != None)
            out.push_back({ static_cast<uint16_t>(a), static_cast<Access>(wpBits_[a]) });
    return out;
}

// ── Transient stops ──────────────────────────────────────────────────────

void Debugger::setTransient(uint16_t addr, Reason reason, DebugBank bank)
{
    stepCpu_ = nullptr;
    transientAddr_   = addr;
    transientBank_ = bank;
    transientReason_ = reason;
    // Released last, so a CPU worker that observes the flag set also observes
    // the address and reason that go with it.
    transientArmed_.store(true, std::memory_order_release);
}

void Debugger::setRunToBeam(uint64_t videoCycle)
{
    stepCpu_ = nullptr;
    beamTarget_      = videoCycle;
    transientReason_ = Reason::RunToBeam;
    transientArmed_.store(true, std::memory_order_release);
}

void Debugger::setStepOver(M6502& cpu, const Memory& memory)
{
    stepCpu_ = &cpu;
    stepMemory_ = &memory;
    stepStack_ = cpu.getStackPointer();
    stepReturning_ = false;
    stepFrames_.clear();
    stepSkipFirst_ = true;        // armed AT the JSR being stepped over
    stepJsrPc_     = cpu.getProgramCounter();
    stepOutMode_   = false;
    stepStartSp_   = stepStack_;
    transientReason_ = Reason::StepOver;
    transientArmed_.store(true, std::memory_order_release);
}

void Debugger::setStepOut(M6502& cpu, const Memory& memory)
{
    setStepOver(cpu, memory);
    // Not SP+2: a routine stopped after PHA (a register save) returns from
    // higher up, and its RTS was never matched — the machine ran on to some
    // unrelated RTS (bug hunt 2026-10-23). The return is the first outer RTS
    // that pops ABOVE where the step started; see onInstruction.
    stepSkipFirst_ = false;
    stepOutMode_   = true;
    stepStartSp_   = cpu.getStackPointer();
    for (int slot = stepStartSp_ + 1; slot <= 0xFF; ++slot)
        stepStackAtStart_[static_cast<std::size_t>(slot)] =
            memory.peekCpuView(static_cast<uint16_t>(0x0100 + slot));
    transientReason_ = Reason::StepOut;
}

// Is the address an outer RTS / RTI is about to pop the routine's RETURN, or
// a target the routine pushed to jump through (PHA PHA RTS)? Both land above
// the start level, and with the same SP trace: begun at the entry, an
// inline-parameter routine does PLA PLA … PHA PHA RTS (start s, up to s+2,
// down to s, RTS to s+2); begun after two register pushes, a dispatch does
// exactly that too. The stack level cannot separate them (bug hunt
// 2026-11-18). What the stack HELD when the step began can:
//   - the address lies partly BELOW the start level: it did not exist when
//     the step began, so it is not this routine's return. Always a jump.
//     (Stopped after one PHA, then PLA … PHA PHA RTS — the audit's case.)
//   - the two slots are unchanged: a plain return.
//   - they were rewritten, and what they held followed a JSR: the routine
//     moved its own return address (inline parameters, the ProDOS MLI).
//   - they were rewritten over something that was NOT a return address
//     (saved registers): a dispatch.
// The last test is a judgement, not a proof: a return address pushed by hand
// (PHA PHA JMP) or whose caller is banked out does not look like one, and a
// routine entered that way that ALSO rewrites it is taken for a dispatch —
// Step Out then stops one return later. Two rare things at once, against a
// stop inside the routine every time a pushed register met a dispatch.
bool Debugger::stepOutPopIsAReturn(int after) const
{
    if (!stepMemory_) return true;
    const int hiSlot = after;                 // an RTS / RTI pops PCL then PCH
    const int loSlot = hiSlot - 1;
    if (hiSlot > 0xFF) return true;           // wrapped stack: no opinion
    if (loSlot <= static_cast<int>(stepStartSp_)) return false;
    const uint8_t lo0 = stepStackAtStart_[static_cast<std::size_t>(loSlot)];
    const uint8_t hi0 = stepStackAtStart_[static_cast<std::size_t>(hiSlot)];
    const uint8_t lo1 = stepMemory_->peekCpuView(static_cast<uint16_t>(0x0100 + loSlot));
    const uint8_t hi1 = stepMemory_->peekCpuView(static_cast<uint16_t>(0x0100 + hiSlot));
    if (lo0 == lo1 && hi0 == hi1) return true;
    // A JSR pushes the address of its own last byte: the opcode is two back.
    const uint16_t pushed = static_cast<uint16_t>(lo0 | (hi0 << 8));
    return stepMemory_->peekCpuView(static_cast<uint16_t>(pushed - 2)) == 0x20;
}

void Debugger::setProfiling(bool enabled)
{
    profiling_ = enabled;
}

void Debugger::setHistoryEnabled(bool enabled)
{
    if (enabled && history_.empty()) history_.resize(256);
    historyEnabled_ = enabled;
}

uint64_t Debugger::frameOfRetired(int cycles) const
{
    if (!memory_) return 0;
    // The hook runs after Memory::advanceCycles: the counter is past the
    // instruction, so its start is `cycles` back. Beam clock, not CPU clock:
    // an accelerator lengthens neither the line nor the frame.
    const uint64_t start = memory_->getCycleCounter() - static_cast<uint64_t>(cycles);
    const uint64_t frame = static_cast<uint64_t>(
        pom2VideoTiming(memory_->videoStandard()).cyclesPerFrame);
    return memory_->videoCycleAt(start) / frame;
}
void Debugger::clearMeasurements()
{
    profile_.clear();
    interruptLoad_.reset();
    historyHead_ = historyCount_ = 0;
}

void Debugger::onStepCompleted(const M6502& cpu, uint16_t pc, int cycles)
{
    // The stepped JSR has executed (an interrupt entry reports through
    // onInterruptEntered instead): its callee's frame is the one to follow.
    steppedOffAmnesty_ = false;             // the stepped instruction ran
    if (stepSkipFirst_ && pc == stepJsrPc_) stepSkipFirst_ = false;
    // The resumed instruction has executed: its amnesty is spent (DBG-1).
    if (!amnesties_.empty()) takeAmnesty(pc);
    osCalls_.retire6502();                 // ...and so is its trace offer
    if (trace_) trace_->after(cpu, cycles);
    if (cycles <= 0) return;
    if (profiling_) {
        interruptLoad_.stepped(cpu.getStackPointer(), cycles, frameOfRetired(cycles));
        auto& entry = profile_[{pc, curBank_}];
        entry.bank = curBank_;
        entry.pc = pc;
        ++entry.visits;
        entry.cycles += static_cast<uint64_t>(cycles);
    }
    if (historyEnabled_) {
        history_[historyHead_] = {pc, cpu.getProgramCounter(), cpu.getAccumulator(),
            cpu.getXRegister(), cpu.getYRegister(), cpu.getStatusRegister(),
            cpu.getStackPointer(), cycles, curBank_};
        historyHead_ = (historyHead_ + 1) % history_.size();
        historyCount_ = std::min(historyCount_ + 1, history_.size());
    }
}

void Debugger::onInterruptEntered(const M6502& cpu, uint16_t interruptedPc, int cycles)
{
    // The instruction offered at this boundary did NOT run: the interrupt
    // was taken in its place, and it will be offered again after the RTI.
    // A breakpoint there that counted a pass without stopping (its "ignore
    // N" not reached) would count the same pass twice — the shape of the
    // stepSkipFirst_ and resume-amnesty fixes. Take it back.
    uncountPass(bpGates_, {interruptedPc, {}});
    uncountPass(bpGates_, {interruptedPc, curBank_});
    if (steppedOffAmnesty_) { amnesties_.push_back(steppedOff_); steppedOffAmnesty_ = false; }
    // The interrupted instruction is offered again after the RTI.
    osCalls_.interrupted6502();
    if (trace_) trace_->interrupted(cpu, cycles);
    // A step follows the handler as a nested frame, closed when SP climbs
    // back to the interrupted level: its RTI is not the call's return.
    if (stepCpu_ && transientArmed_.load(std::memory_order_relaxed) && stepFrames_.size() < 256)
        stepFrames_.push_back(static_cast<uint8_t>(cpu.getStackPointer() + 3));
    // The entry's cycles belong to the handler it enters; it is not a visit
    // (the handler's first instruction counts its own when it runs), and
    // nothing executed, so history records nothing.
    // Keyed by the HANDLER's bank, resolved at its PC: `curBank_` is still
    // the interrupted instruction's (aux, LC RAM…), which filed the entry
    // under a phantom zero-visit row beside the real handler's.
    if (!profiling_ || cycles <= 0) return;
    interruptLoad_.entered(cpu.getStackPointer(), cycles, frameOfRetired(cycles));
    const uint16_t pc = cpu.getProgramCounter();
    const DebugBank bank = resolveBank(pc);
    auto& entry = profile_[{pc, bank}];
    entry.bank = bank;
    entry.pc = pc;
    entry.cycles += static_cast<uint64_t>(cycles);
}

std::vector<Debugger::ProfileEntry> Debugger::profile(std::size_t limit) const
{
    std::vector<ProfileEntry> out;
    for (const auto& entry : profile_)
        if (entry.second.visits) out.push_back(entry.second);
    std::sort(out.begin(), out.end(), [](const ProfileEntry& a, const ProfileEntry& b) {
        return a.cycles != b.cycles ? a.cycles > b.cycles : DebugAddress{a.pc,a.bank} < DebugAddress{b.pc,b.bank};
    });
    if (out.size() > limit) out.resize(limit);
    return out;
}

std::vector<Debugger::TraceEntry> Debugger::history() const
{
    std::vector<TraceEntry> out;
    out.reserve(historyCount_);
    if (!historyCount_) return out;
    const std::size_t first = (historyHead_ + history_.size() - historyCount_) % history_.size();
    for (std::size_t i = 0; i < historyCount_; ++i)
        out.push_back(history_[(first + i) % history_.size()]);
    return out;
}

void Debugger::clearTransient()
{
    // The flag only. `transientReason_` is read exclusively while the flag is
    // true, and this is the one method callable WITHOUT `stateMutex` (the Stop
    // verb; see the header) — a plain write to the reason here would be a race
    // for no gain.
    transientArmed_.store(false, std::memory_order_relaxed);
}

void Debugger::dropHistoryOf(uint16_t pc)
{
    if (!historyCount_ || history_.empty()) return;
    const std::size_t newest = (historyHead_ + history_.size() - 1) % history_.size();
    if (history_[newest].pc != pc) return;
    historyHead_ = newest;
    --historyCount_;
}

bool Debugger::beamStopDue(uint16_t parkedPc)
{
    if (!beamStopArmed() || !memory_ || memory_->videoNow() < beamTarget_) return false;
    clearTransient();
    hit_ = { Reason::RunToBeam, parkedPc, 0, 0, resolveBank(parkedPc) };
    hit_.busMaster = true;                  // the parked 6502's PC, not the card's
    hit_.videoCycle = memory_->videoNow();
    return true;
}

void Debugger::armResumeFrom(uint16_t pc, int sp)
{
    const DebugBank bank = hit_.valid() && hit_.pc == pc ? hit_.pcBank : resolveBank(pc);
    // Keep only the amnesties an interrupt is holding up: armed at a
    // SHALLOWER stack than this one (a higher SP — the 6502's stack grows
    // down, and the interrupt pushed its return on top). Anything else is a
    // resume that is over or abandoned, and would skip a later real stop.
    amnesties_.erase(std::remove_if(amnesties_.begin(), amnesties_.end(),
        [sp](const Amnesty& a) { return sp < 0 || a.sp < 0 || a.sp <= sp; }), amnesties_.end());
    if (amnesties_.size() >= 8) amnesties_.erase(amnesties_.begin());   // nesting, not a log
    amnesties_.push_back({pc, bank, sp});
}

bool Debugger::hasAmnesty(uint16_t pc) const
{
    for (const Amnesty& a : amnesties_) if (a.pc == pc) return true;
    return false;
}

bool Debugger::takeAmnesty(uint16_t pc, Amnesty* taken)
{
    for (auto it = amnesties_.begin(); it != amnesties_.end(); ++it)
        if (it->pc == pc) {
            if (taken) *taken = *it;
            amnesties_.erase(it);
            return true;
        }
    return false;
}

void Debugger::clearForTimeJump(bool keepMeasurements)
{
    // See the header. Unlike clearTransient() this one is lock-protected, so
    // the plain members can be written directly.
    transientArmed_.store(false, std::memory_order_relaxed);
    amnesties_.clear();
    steppedOffAmnesty_ = false;
    curPc_           = 0;
    hit_             = {};
    osCalls_.dropPending();
    if (!keepMeasurements) clearMeasurements();
}

// ── The hot hooks ────────────────────────────────────────────────────────

void Debugger::beginStep(uint16_t pc, bool explicitStep)
{
    // An explicit step (F7) passes a breakpoint like any other arrival: its
    // pass counts — unless this step is the one leaving the stop that
    // already counted it (the resume amnesty names that instruction).
    // Nor a step that leaves a stop still LATCHED at this pc: the toolbar's
    // Step, the palette's and the CLI's `--step` do not go through
    // debugResume, so no amnesty names the instruction — the hit does.
    const bool leavingStop = hasAmnesty(pc) ||
        (hit_.valid() && hit_.pc == pc && !hit_.busMaster &&
         hit_.reason != Reason::WatchRead && hit_.reason != Reason::WatchWrite);
    const bool arriving = explicitStep && !replaying_ && !leavingStop;
    // ...and that stop is then over, whichever Step verb made the step: left
    // latched, it made a LATER arrival at the same pc (a loop coming back
    // round under the toolbar's Step) look like "leaving" again, pass missed.
    // Only THAT stop: a memory breakpoint tripped by an earlier step of a
    // queue (`--step N`) is a hit still to be shown, and stays.
    if (explicitStep && !replaying_ && hit_.valid() && hit_.pc == pc &&
        hit_.reason != Reason::WatchRead && hit_.reason != Reason::WatchWrite) hit_ = {};
    if (explicitStep) {
        // A deliberate step consumes the amnesty (a branch to itself must
        // stop again on its breakpoint) — unless the step turns out to take
        // an interrupt instead of running the instruction, which then comes
        // back after the RTI as the same arrival: onInterruptEntered gives
        // the amnesty back (steppedOffAmnesty_).
        // Only the amnesty's OWN instruction: a step made inside the handler
        // that interrupted it must leave the amnesty for the return.
        steppedOffAmnesty_ = takeAmnesty(pc, &steppedOff_);
        // The toolbar's Step leaves a stop through the latched hit, with no
        // amnesty to take: if an interrupt is taken instead, the instruction
        // comes back after the RTI and must not stop or count again either.
        if (!steppedOffAmnesty_ && leavingStop) {
            steppedOff_ = {pc, resolveBank(pc), -1};
            steppedOffAmnesty_ = true;
        }
        // F7 / Step goes through here, not onInstruction: without the offer a
        // stepped $BF00 was never decoded and a stepped-over return left its
        // call open, later closed as "(no return)" (bug hunt 2026-11-07).
        if (osCalls_.any6502()) osCalls_.offer6502(pc);
    }
    curPc_ = pc;
    curBank_ = resolveBank(pc);
    ++serial_;
    if (arriving && (bpCount_ || !bankBreakpoints_.empty())) {
        const Hit kept = hit_;
        (void)breakpointStops(pc);          // counted; a step never stops on it
        hit_ = kept;
    }
    // Both entries reach here — the run loop's onInstruction and F7's
    // explicit step — so a tracer sees every instruction going IN.
    if (trace_) trace_->before(pc, curBank_);
}

bool Debugger::onInstruction(uint16_t pc)
{
    // A watchpoint fired inside the PREVIOUS instruction. The access is done
    // and cannot be un-done, so its stop lands here, at the first boundary
    // after it — which is what makes noteAccess() a run-control stop rather
    // than a note in a log. Checked before everything else: the hit is only
    // ever clear when the controller has consumed it (debugResume), so this
    // cannot swallow a resume.
    if (hit_.valid()) return true;
    if (osCalls_.any6502()) osCalls_.offer6502(pc);
    // Latched for noteAccess(), which fires mid-instruction and would
    // otherwise have no way to name the instruction responsible.
    beginStep(pc);

    bool transientHit = false;
    if (transientArmed_.load(std::memory_order_acquire)) {
        if (stepCpu_) {
            const uint8_t sp = stepCpu_->getStackPointer();
            // The OUTER return, not any RTS at the right stack depth. SP alone
            // was the old test, and an inline-string routine (PLA PLA … JSR
            // COUT … PHA PHA RTS) sits at the caller's level while it calls
            // COUT, so COUT's RTS looked like the outer one and Step Over stopped
            // inside the routine (bug hunt 2026-10-23). Nested JSRs open a
            // frame; one closes when SP climbs back to its JSR's level.
            // Keep the latch through an IRQ/NMI between RTS and its target.
            while (!stepFrames_.empty() && sp >= stepFrames_.back())
                stepFrames_.pop_back();
            transientHit = stepReturning_ && sp == stepStack_;
            if (!transientHit) {
                const uint8_t op = stepMemory_->peekCpuView(pc);
                // The stepped JSR, at ITS pc and stack level, is not a
                // nested call. The skip is RETIRED in onStepCompleted, once
                // that JSR has really executed — not here: an IRQ/NMI taken
                // at this boundary runs the handler first and the JSR is
                // offered to this hook again after the RTI, and consuming the
                // skip on the first offer counted the stepped JSR as nested
                // the second time, so its RTS never ended the step and Step Over ran
                // away (bug hunt 2026-11-01). The handler's own instructions
                // sit below `stepStack_` and cannot match.
                if (stepSkipFirst_ && pc == stepJsrPc_ && sp == stepStack_) {
                    // the stepped JSR — nothing to track yet
                } else if (op == 0x20 || op == 0x00) {   // JSR, BRK: a nested call
                    if (stepFrames_.size() < 256) stepFrames_.push_back(sp);
                } else if ((op == 0x60 || op == 0x40) && stepFrames_.empty()) {
                    // RTI pops P as well: Step Out of an IRQ/NMI/BRK handler
                    // returns to the interrupted pc (bug hunt 2026-11-07).
                    // In Step Over too: ProDOS 8's MLI leaves a `JSR $BF00`
                    // by PHA PHA PHA … RTI, so Step Over on any MLI call never
                    // stopped (bug hunt 2026-11-18). An interrupt's own RTI
                    // cannot match: its entry opened a frame.
                    const int after = static_cast<int>(sp) + (op == 0x40 ? 3 : 2);
                    // An RTS used as a jump (push target-1, RTS) pops back to
                    // where the callee was, not to the caller: not a return.
                    //
                    // Step Out adds what the stack HELD when the step began
                    // (stepOutPopIsAReturn): the level alone cannot tell a
                    // dispatch from an inline-parameter return.
                    if (stepOutMode_ ? (after > static_cast<int>(stepStartSp_) &&
                                        stepOutPopIsAReturn(after))
                                     : static_cast<uint8_t>(after) == stepStack_) {
                        stepReturning_ = true;
                        stepStack_ = static_cast<uint8_t>(after);
                    }
                }
            }
        } else if (transientReason_ == Reason::RunToBeam) {
            transientHit = memory_ && memory_->videoNow() >= beamTarget_;
        } else {
            transientHit = pc == transientAddr_ && (transientBank_.kind == DebugBank::Any || transientBank_ == curBank_);
        }
    }

    // Resuming from a stop: the PC still points at the instruction that
    // stopped us, so without this the same breakpoint fires forever and Run
    // does nothing. One instruction of amnesty — RETIRED in onStepCompleted,
    // once that instruction has really executed, not here on the offer. An
    // IRQ/NMI taken at this boundary runs its handler first and the
    // instruction is offered again after the RTI; consuming the amnesty on
    // the first offer re-stopped Run at the same breakpoint having executed
    // nothing, and with a level-triggered IRQ still asserted Run could never
    // get past it (bug hunt 2026-11-01 round 2, DBG-1 — the mirror of the
    // stepSkipFirst_ fix above). The handler's own instructions sit at other
    // addresses and leave it armed.
    for (const Amnesty& a : amnesties_)
        if (a.pc == pc && a.bank == curBank_) return false;

    if (transientHit) {
        // Transients are one-shot by construction — step-over must not leave
        // a breakpoint behind at the return address.
        const Reason why = transientReason_;
        clearTransient();
        // Landing on a breakpoint's address is a pass of that breakpoint,
        // though it is the transient that stops: the Run that follows gets
        // the amnesty and would never count it.
        (void)breakpointStops(pc);
        hit_ = { why, pc, 0, 0, curBank_ };
        hit_.videoCycle = memory_ ? memory_->videoNow() : 0;
        return true;
    }
    return breakpointStops(pc);
}

bool Debugger::probeInstruction(uint16_t pc)
{
    if (hit_.valid()) return true;        // a memory breakpoint, one instruction ago
    curPc_ = pc;
    curBank_ = resolveBank(pc);
    ++serial_;
    return breakpointStops(pc);
}

bool Debugger::breakpointStops(uint16_t pc)
{

    // Both forms are judged (no short-circuit): each counts its own pass.
    const bool wild   = bpCount_ && hasBreakpoint(pc);
    const bool banked = !bankBreakpoints_.empty() && bankBreakpoints_.count({pc,curBank_});
    if (!wild && !banked) return false;
    const uint64_t now = memory_ ? memory_->videoNow() : 0;
    bool stop = false;
    if (wild && conditionMatches(pc))
        stop |= gateOpens(bpGates_, {pc, {}}, now, -1, false, false);
    if (banked && (!bankConditions_.count({pc,curBank_}) ||
                   conditionMatches(bankConditions_.at({pc,curBank_}))))
        stop |= gateOpens(bpGates_, {pc, curBank_}, now, -1, false, false);
    if (!stop) return false;
    hit_ = { Reason::Breakpoint, pc, 0, 0, curBank_ };
    hit_.videoCycle = now;
    return true;
}

void Debugger::noteAccess(uint16_t addr, uint8_t value, bool write)
{
    noteBankAccess(addr, value, write, resolveBank(addr,write));
}

void Debugger::noteBankAccess(uint16_t addr, uint8_t value, bool write, DebugBank bank)
{
    // The debugger's own write (an edit in the Memory viewer, the assembler's
    // patch) is not the guest's: no stop, no pass, and a stop that is
    // pending stays exactly as it is.
    if (ownWrite_) return;
    const uint8_t want = write ? Write : Read;
    const auto found = bankWatches_.find({addr,bank});
    const bool wild   = watchpointAt(addr) & want;
    const bool banked = found != bankWatches_.end() && (found->second & want);
    if (!wild && !banked) return;
    // The gates (Debugger.h § Gates), at the cycle of the access itself. A
    // card processor's slice has no instruction serial of its own: none of
    // its writes is taken for an RMW's second.
    // (A card processor's access: the beam clock as it stands. The 6502's
    // sub-instruction offset is whatever its last instruction left behind.)
    const uint64_t cycle = !memory_ ? 0 : busMaster_ ? memory_->videoNow()
                                                     : memory_->videoAccessCycle();
    const bool repeat = !busMaster_;
    bool open = false;
    if (wild)   open |= gateOpens(wpGates_, {addr, {}},   cycle, value, write, repeat);
    if (banked) open |= gateOpens(wpGates_, {addr, bank}, cycle, value, write, repeat);
    // Do NOT overwrite an existing un-consumed hit: the first stop wins, and
    // an instruction that touches two watched addresses should report the one
    // that stopped it rather than the last one it happened to reach.
    if (hit_.valid()) {
        // ...except the SAME instruction writing the SAME byte again: an NMOS
        // read-modify-write writes the original value back first (the dummy
        // cycle, rmwSecondBusCycle), so `INC $10` reported "$0010 was written
        // ($05)" while memory held $06. The last write is the one that stays.
        // And if the gate does not open on that last write, the stop the
        // dummy cycle latched is taken back.
        // SAME instruction: its serial, not just its pc — a queue of steps
        // round a loop comes back to that pc, and a later turn's write must
        // not rewrite or erase the stop an earlier turn latched.
        if (write && !busMaster_ && hit_.reason == Reason::WatchWrite && hit_.addr == addr &&
            hit_.bank == bank && hit_.pc == curPc_ && hitSerial_ == serial_) {
            if (open) { hit_.value = value; hit_.videoCycle = cycle; }
            else      hit_ = {};
        }
        return;
    }
    if (!open) return;
    hit_ = { write ? Reason::WatchWrite : Reason::WatchRead,
             curPc_, addr, value, curBank_, bank, busMaster_ };
    hit_.videoCycle = cycle;
    hitSerial_ = serial_;
    // curPc_, not the CPU's programCounter: the access is mid-instruction and
    // that register has already advanced past the opcode and its operands, so
    // it names neither the writing instruction nor anything stable. curPc_ is
    // what onInstruction was handed for the instruction now executing — the
    // one that performed this write.
}

DebugBank Debugger::resolveBank(uint16_t addr, bool write) const
{
    return memory_ ? memory_->debugBank(addr,write) : DebugBank{};
}
void Debugger::addBreakpoint(uint16_t addr, DebugBank bank)
{
    if (bank.kind == DebugBank::Any) addBreakpoint(addr);
    else { bankBreakpoints_[{addr,bank}] = true; bankConditions_.erase({addr,bank}); }
}
void Debugger::removeBreakpoint(uint16_t addr, DebugBank bank)
{
    if (bank.kind == DebugBank::Any) removeBreakpoint(addr);
    else { bankBreakpoints_.erase({addr,bank}); bankConditions_.erase({addr,bank}); bpGates_.erase({addr,bank}); }
}
void Debugger::addConditionalBreakpoint(uint16_t addr, Condition condition, const M6502& cpu, DebugBank bank)
{
    if (bank.kind == DebugBank::Any) { addConditionalBreakpoint(addr,condition,cpu); return; }
    addBreakpoint(addr,bank);
    conditionCpu_ = &cpu;
    bankConditions_[{addr,bank}] = condition;
}
bool Debugger::hasBreakpoint(uint16_t addr, DebugBank bank) const
{
    return hasBreakpoint(addr) || bankBreakpoints_.count({addr,bank});
}
std::vector<DebugAddress> Debugger::bankBreakpoints() const
{
    std::vector<DebugAddress> out;
    for (auto addr : breakpoints()) out.push_back({addr,{}});
    for (const auto& entry : bankBreakpoints_) out.push_back(entry.first);
    std::sort(out.begin(),out.end());
    return out;
}
void Debugger::setWatchpoint(uint16_t addr, Access access, DebugBank bank)
{
    if (bank.kind == DebugBank::Any) setWatchpoint(addr,access);
    else if (access == None) { bankWatches_.erase({addr,bank}); wpGates_.erase({addr,bank}); }
    else bankWatches_[{addr,bank}] = access;
}

void Debugger::clearHit()
{
    hit_ = {};
}

void Debugger::patchMemory(Memory& memory, uint16_t addr, const uint8_t* bytes, std::size_t count)
{
    // noteTimeJump() was the assembler's old call: it wiped the stop, so the
    // next Run had no amnesty and re-stopped on the same breakpoint having
    // executed nothing (bug hunt 2026-11-07). A memory breakpoint on a
    // patched byte must neither latch, nor count a pass, nor touch a pending
    // hit (noteBankAccess returns at once while ownWrite_ is set).
    ownWrite_ = true;
    memory.setUnattributedAccess(true);         // a switch it throws names no instruction
    for (std::size_t i = 0; i < count; ++i)
        memory.memWrite(static_cast<uint16_t>(addr + i), bytes[i]);
    memory.setUnattributedAccess(false);
    ownWrite_ = false;
}

}  // namespace pom2
