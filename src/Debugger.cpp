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
    value &= c.mask;
    switch (c.compare) {
        case Compare::Equal: return value == c.value;
        case Compare::NotEqual: return value != c.value;
        case Compare::Less: return value < c.value;
        case Compare::LessEqual: return value <= c.value;
        case Compare::Greater: return value > c.value;
        case Compare::GreaterEqual: return value >= c.value;
    }
    return false;
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

void Debugger::setStepOver(M6502& cpu, const Memory& memory)
{
    stepCpu_ = &cpu;
    stepMemory_ = &memory;
    stepStack_ = cpu.getStackPointer();
    stepReturning_ = false;
    stepFrames_.clear();
    stepSkipFirst_ = true;        // armed AT the JSR being stepped over
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
    transientReason_ = Reason::StepOut;
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

void Debugger::clearMeasurements()
{
    profile_.clear();
    historyHead_ = historyCount_ = 0;
}

void Debugger::onStepCompleted(const M6502& cpu, uint16_t pc, int cycles)
{
    if (cycles <= 0) return;
    if (profiling_) {
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

void Debugger::onInterruptEntered(const M6502& cpu, uint16_t, int cycles)
{
    // The entry's cycles belong to the handler it enters; it is not a visit
    // (the handler's first instruction counts its own when it runs), and
    // nothing executed, so history records nothing.
    // Keyed by the HANDLER's bank, resolved at its PC: `curBank_` is still
    // the interrupted instruction's (aux, LC RAM…), which filed the entry
    // under a phantom zero-visit row beside the real handler's.
    if (!profiling_ || cycles <= 0) return;
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

void Debugger::clearForTimeJump()
{
    // See the header. Unlike clearTransient() this one is lock-protected, so
    // the plain members can be written directly.
    transientArmed_.store(false, std::memory_order_relaxed);
    resumeSkipArmed_ = false;
    resumeSkip_      = 0;
    curPc_           = 0;
    hit_             = {};
    clearMeasurements();
}

// ── The hot hooks ────────────────────────────────────────────────────────

void Debugger::beginStep(uint16_t pc, bool explicitStep)
{
    if (explicitStep) resumeSkipArmed_ = false;
    curPc_ = pc;
    curBank_ = resolveBank(pc);
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
            // COUT, so COUT's RTS looked like the outer one and F8 stopped
            // inside the routine (bug hunt 2026-10-23). Nested JSRs open a
            // frame; one closes when SP climbs back to its JSR's level.
            // Keep the latch through an IRQ/NMI between RTS and its target.
            while (!stepFrames_.empty() && sp >= stepFrames_.back())
                stepFrames_.pop_back();
            transientHit = stepReturning_ && sp == stepStack_;
            if (!transientHit) {
                const uint8_t op = stepMemory_->peekCpuView(pc);
                if (stepSkipFirst_) {
                    stepSkipFirst_ = false;              // the stepped JSR
                } else if (op == 0x20) {                 // JSR: a nested call
                    if (stepFrames_.size() < 256) stepFrames_.push_back(sp);
                } else if (op == 0x60 && stepFrames_.empty()) {
                    const int after = static_cast<int>(sp) + 2;
                    // An RTS used as a jump (push target-1, RTS) pops back to
                    // where the callee was, not to the caller: not a return.
                    if (stepOutMode_ ? after > static_cast<int>(stepStartSp_)
                                     : static_cast<uint8_t>(after) == stepStack_) {
                        stepReturning_ = true;
                        stepStack_ = static_cast<uint8_t>(after);
                    }
                }
            }
        } else {
            transientHit = pc == transientAddr_ && (transientBank_.kind == DebugBank::Any || transientBank_ == curBank_);
        }
    }

    // Resuming from a stop: the PC still points at the instruction that
    // stopped us, so without this the same breakpoint fires forever and Run
    // does nothing. One instruction of amnesty, cleared on use.
    if (resumeSkipArmed_ && pc == resumeSkip_ && curBank_ == resumeBank_) {
        resumeSkipArmed_ = false;
        return false;
    }
    resumeSkipArmed_ = false;

    if (transientHit) {
        // Transients are one-shot by construction — step-over must not leave
        // a breakpoint behind at the return address.
        const Reason why = transientReason_;
        clearTransient();
        hit_ = { why, pc, 0, 0, curBank_ };
        return true;
    }
    if ((bpCount_ && hasBreakpoint(pc) && conditionMatches(pc)) ||
        (bankBreakpoints_.count({pc,curBank_}) &&
         (!bankConditions_.count({pc,curBank_}) || conditionMatches(bankConditions_.at({pc,curBank_}))))) {
        hit_ = { Reason::Breakpoint, pc, 0, 0, curBank_ };
        return true;
    }
    return false;
}

void Debugger::noteAccess(uint16_t addr, uint8_t value, bool write)
{
    noteBankAccess(addr, value, write, resolveBank(addr,write));
}

void Debugger::noteBankAccess(uint16_t addr, uint8_t value, bool write, DebugBank bank)
{
    const uint8_t want = write ? Write : Read;
    const auto found = bankWatches_.find({addr,bank});
    if (!(watchpointAt(addr) & want) &&
        (found == bankWatches_.end() || !(found->second & want))) return;
    // Do NOT overwrite an existing un-consumed hit: the first stop wins, and
    // an instruction that touches two watched addresses should report the one
    // that stopped it rather than the last one it happened to reach.
    if (hit_.valid()) {
        // ...except the SAME instruction writing the SAME byte again: an NMOS
        // read-modify-write writes the original value back first (the dummy
        // cycle, rmwSecondBusCycle), so `INC $10` reported "$0010 was written
        // ($05)" while memory held $06. The last write is the one that stays.
        if (write && hit_.reason == Reason::WatchWrite && hit_.addr == addr &&
            hit_.bank == bank && hit_.pc == curPc_)
            hit_.value = value;
        return;
    }
    hit_ = { write ? Reason::WatchWrite : Reason::WatchRead,
             curPc_, addr, value, curBank_, bank };
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
    else { bankBreakpoints_.erase({addr,bank}); bankConditions_.erase({addr,bank}); }
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
    else if (access == None) bankWatches_.erase({addr,bank});
    else bankWatches_[{addr,bank}] = access;
}

void Debugger::clearHit()
{
    hit_ = {};
}

}  // namespace pom2
