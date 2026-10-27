// POM2 Apple II Emulator — GPL-3.0-or-later
//
// m6502_undoc_access_timing — an undocumented NMOS opcode touches the bus at
// the same in-flight cycle as the documented opcode with the same addressing
// mode. Devices stamp an access with `cycleCounter +
// getCurrentInstructionCycles()` (Disk II latch, 6522 timers, speaker,
// floating bus), and the undocumented handlers used to add their whole cost
// first: `LAX $C0C0` read one cycle after `LDA $C0C0`, `SLO $C0C0` three
// after `ASL $C0C0`. Cycle totals were right, so Tom Harte could not see it.

#include "M6502.h"
#include "Memory.h"
#include "SlotPeripheral.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr int kSlot = 4;   // $C0C0-$C0CF

struct Recorder : SlotPeripheral {
    M6502* cpu = nullptr;
    std::vector<int> stamps;
    std::string_view name() const override { return "recorder"; }
    uint8_t deviceSelectRead(uint8_t) override
    { stamps.push_back(cpu->getCurrentInstructionCycles()); return 0x00; }
    void deviceSelectWrite(uint8_t, uint8_t) override
    { stamps.push_back(cpu->getCurrentInstructionCycles()); }
};

int failures = 0;

// Run one instruction at $0300 with X = Y = 0 and the zero-page pointers at
// $10/$11 aimed at $C0C0; return the in-flight cycle of each $C0Cx access.
std::vector<int> stampsOf(const std::vector<uint8_t>& insn,
                          M6502::CpuMode mode = M6502::CpuMode::NMOS)
{
    Memory mem;
    M6502 cpu(&mem);
    cpu.setCpuMode(mode);
    cpu.hardReset();
    auto rec = std::make_unique<Recorder>();
    rec->cpu = &cpu;
    Recorder* r = rec.get();
    mem.slotBus().plug(kSlot, std::move(rec));
    mem.memWrite(0x10, 0xC0);
    mem.memWrite(0x11, 0xC0);
    for (std::size_t i = 0; i < insn.size(); ++i)
        mem.memWrite(static_cast<uint16_t>(0x0300 + i), insn[i]);
    cpu.setXRegister(0);
    cpu.setYRegister(0);
    cpu.setProgramCounter(0x0300);
    cpu.step();
    return r->stamps;
}

void same(const char* what, const std::vector<uint8_t>& doc,
          const std::vector<uint8_t>& undoc)
{
    const auto a = stampsOf(doc), b = stampsOf(undoc);
    // A documented RMW stamps its three accesses at its read; so must the
    // undocumented one. Compare the first access (the data read or write).
    const bool ok = !a.empty() && !b.empty() && a.front() == b.front();
    std::printf("  %s %s: documented at %d, undocumented at %d\n",
                ok ? "OK  " : "FAIL", what, a.empty() ? -1 : a.front(),
                b.empty() ? -1 : b.front());
    if (!ok) ++failures;
}

} // namespace

int main()
{
    // Reads: LDA vs LAX.
    same("abs read    LDA/LAX $C0C0",   {0xAD, 0xC0, 0xC0}, {0xAF, 0xC0, 0xC0});
    same("abs,Y read  LDA/LAX $C0C0,Y", {0xB9, 0xC0, 0xC0}, {0xBF, 0xC0, 0xC0});
    same("(zp,X) read LDA/LAX ($10,X)", {0xA1, 0x10},       {0xA3, 0x10});
    same("(zp),Y read LDA/LAX ($10),Y", {0xB1, 0x10},       {0xB3, 0x10});
    // Stores: STA vs SAX / SHA.
    same("abs store   STA/SAX $C0C0",   {0x8D, 0xC0, 0xC0}, {0x8F, 0xC0, 0xC0});
    same("abs,Y store STA/SHA $C0C0,Y", {0x99, 0xC0, 0xC0}, {0x9F, 0xC0, 0xC0});
    same("(zp,X) st.  STA/SAX ($10,X)", {0x81, 0x10},       {0x83, 0x10});
    same("(zp),Y st.  STA/SHA ($10),Y", {0x91, 0x10},       {0x93, 0x10});
    // Read-modify-write: ASL vs SLO, DEC vs DCP.
    same("abs RMW     ASL/SLO $C0C0",   {0x0E, 0xC0, 0xC0}, {0x0F, 0xC0, 0xC0});
    same("abs,X RMW   ASL/SLO $C0C0,X", {0x1E, 0xC0, 0xC0}, {0x1F, 0xC0, 0xC0});
    same("abs RMW     DEC/DCP $C0C0",   {0xCE, 0xC0, 0xC0}, {0xCF, 0xC0, 0xC0});

    // Documented instructions, against MAME's om6502.lst bus order (the
    // device stamps the access on the cycle it is issued, opcode = 0).
    auto exact = [&](const char* what, const std::vector<uint8_t>& insn,
                     const std::vector<int>& want) {
        const auto got = stampsOf(insn);
        const bool ok = got == want;
        std::printf("  %s %s:", ok ? "OK  " : "FAIL", what);
        for (int c : got) std::printf(" %d", c);
        std::printf("\n");
        if (!ok) ++failures;
    };
    // asl_aba: read @3, the old value written back @4, the new one @5. Both
    // writes used to be stamped on the read's cycle.
    exact("ASL $C0C0 read/write/write", {0x0E, 0xC0, 0xC0}, {3, 4, 5});
    // sta_abx: the NMOS always reads base-page+low before the store, on the
    // cycle before it (it shared the store's cycle).
    exact("STA $C0C0,X dummy read, store", {0x9D, 0xC0, 0xC0}, {3, 4});

    // The Apple //e enhanced and //c carry an NCR (or GTE/CMD) 65C02, not a
    // Rockwell: $x7/$xF and $CB/$DB are one-byte, one-cycle NOPs on it
    // (IIe/IIc Technical Reference, Appendix A). They used to run as
    // RMB/BBR/WAI/STP — STP halted the machine.
    {
        Memory mem;
        M6502 cpu(&mem);
        cpu.setCpuMode(M6502::CpuMode::CMOS);
        cpu.setNcr65C02(true);
        cpu.hardReset();
        for (uint8_t op : { uint8_t{0x07}, uint8_t{0x0F}, uint8_t{0xCB}, uint8_t{0xDB} }) {
            mem.memWrite(0x0300, op);
            mem.memWrite(0x0301, 0xEA);
            cpu.setProgramCounter(0x0300);
            const uint64_t before = mem.getCycleCounter();
            cpu.step();
            const bool ok = cpu.getProgramCounter() == 0x0301 &&
                            mem.getCycleCounter() - before == 1;
            std::printf("  %s NCR $%02X: 1 byte, 1 cycle\n", ok ? "OK  " : "FAIL", op);
            if (!ok) ++failures;
        }
    }

    std::printf("m6502_undoc_access_timing: %d failure(s)\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
