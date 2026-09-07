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

// The three snapshot defects from hunt #4 that a `machineId` match cannot
// catch, because pom2::snapshotMachineId() hashes the PROFILE KEY alone:
//
//   #13  a snapshot captured with N RamWorks banks, restored into a machine
//        configured for M != N. Memory::loadSnapshotState's best-effort
//        branch lifted only the saved CURRENT bank and returned true, so up
//        to 8 MB of guest aux RAM vanished and the load reported success.
//   #14  a 65C02 snapshot restored onto an NMOS core (or the reverse). The
//        cpuMode byte is deliberately NOT applied — it is configuration, not
//        state — so the machine resumed at a PC pointing into 65C02-only
//        opcodes, several of which are KIL on NMOS. Silent freeze.
//   #29  SnapshotIO's memory writer starts at offset 0 without clearing its
//        sink, so reusing one buffer for a shorter snapshot left the previous
//        blob's tail behind and the reader rejected it as corrupt.
//
// All three are pinned as REFUSALS with a named error (or, for #29, as a
// clean round-trip), not as best-effort restores.

#include "M6502.h"
#include "MachineSnapshot.h"
#include "Memory.h"
#include "SnapshotIO.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
}

bool mentions(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Capture a machine into a blob the same way the file path does.
std::vector<uint8_t> capture(M6502& cpu, Memory& mem)
{
    std::vector<uint8_t> blob;
    pom2::SnapshotWriter w(blob);
    pom2::captureMachineState(w, cpu, mem, /*includeSlots=*/false);
    const bool ok = w.finish();
    assert(ok);
    (void)ok;
    return blob;
}

pom2::RestoreResult restore(const std::vector<uint8_t>& blob,
                            M6502& cpu, Memory& mem)
{
    pom2::SnapshotReader r(blob.data(), blob.size());
    return pom2::restoreMachineState(r, cpu, mem, /*transactional=*/true);
}

}  // namespace

int main()
{
    // ── #13: RamWorks bank-count mismatch ───────────────────────────────
    {
        Memory big;
        big.setIIEMode(true);
        big.setRamWorksBanks(8);
        M6502 cpuBig(&big);
        cpuBig.setCpuMode(M6502::CpuMode::CMOS);
        // Something identifiable in the aux banks so a "restore" that keeps
        // one of them is not confused with a machine that never had them.
        big.writeRamUnchecked(0x4000, 0xA5);
        const std::vector<uint8_t> fat = capture(cpuBig, big);

        Memory stock;
        stock.setIIEMode(true);
        M6502 cpuStock(&stock);
        cpuStock.setCpuMode(M6502::CpuMode::CMOS);
        check(stock.ramWorksBanks() == 1, "stock machine has 1 aux bank");

        const pom2::RestoreResult r = restore(fat, cpuStock, stock);
        check(!r.ok, "#13: an 8-bank snapshot is REFUSED by a 1-bank machine");
        check(mentions(r.error, "RamWorks"),
              "#13: the refusal names RamWorks");
        check(mentions(r.error, "ramworks_banks"),
              "#13: the refusal names the setting that fixes it");

        // Same geometry both ways round: still refused.
        Memory other;
        other.setIIEMode(true);
        other.setRamWorksBanks(4);
        M6502 cpuOther(&other);
        cpuOther.setCpuMode(M6502::CpuMode::CMOS);
        check(!restore(fat, cpuOther, other).ok,
              "#13: 8 banks into 4 banks is refused too");

        // And the matching case still loads.
        Memory same;
        same.setIIEMode(true);
        same.setRamWorksBanks(8);
        M6502 cpuSame(&same);
        cpuSame.setCpuMode(M6502::CpuMode::CMOS);
        const pom2::RestoreResult ok8 = restore(fat, cpuSame, same);
        check(ok8.ok, "#13: a matching bank count still restores");
    }

    // ── #14: CPU core mismatch ──────────────────────────────────────────
    {
        Memory cmosMem;
        cmosMem.setIIEMode(true);
        M6502 cmos(&cmosMem);
        cmos.setCpuMode(M6502::CpuMode::CMOS);
        cmos.setProgramCounter(0x0300);
        const std::vector<uint8_t> cmosBlob = capture(cmos, cmosMem);

        Memory nmosMem;
        nmosMem.setIIEMode(true);
        M6502 nmos(&nmosMem);
        nmos.setCpuMode(M6502::CpuMode::NMOS);
        nmos.setProgramCounter(0x1234);

        const pom2::RestoreResult r = restore(cmosBlob, nmos, nmosMem);
        check(!r.ok, "#14: a 65C02 snapshot is REFUSED by an NMOS machine");
        check(mentions(r.error, "65C02"), "#14: the refusal names the core");
        // Nothing must have been applied: the transactional wrapper rolls the
        // machine back, and the CPU-mode check fires before any register move.
        check(nmos.getProgramCounter() == 0x1234,
              "#14: the refused load left the live PC alone");
        check(nmos.getCpuMode() == M6502::CpuMode::NMOS,
              "#14: the refused load did not change the core");

        // The reverse direction is refused as well.
        const std::vector<uint8_t> nmosBlob = capture(nmos, nmosMem);
        const pom2::RestoreResult r2 = restore(nmosBlob, cmos, cmosMem);
        check(!r2.ok, "#14: an NMOS snapshot is REFUSED by a 65C02 machine");
        check(mentions(r2.error, "NMOS"), "#14: the reverse refusal names it");

        // Matching cores still round-trip.
        Memory twinMem;
        twinMem.setIIEMode(true);
        M6502 twin(&twinMem);
        twin.setCpuMode(M6502::CpuMode::CMOS);
        check(restore(cmosBlob, twin, twinMem).ok,
              "#14: a matching core still restores");
        check(twin.getProgramCounter() == 0x0300,
              "#14: ...and actually applied the snapshot");
    }

    // ── #29: the memory writer REPLACES its sink ────────────────────────
    {
        Memory mem;
        mem.setIIEMode(true);
        M6502 cpu(&mem);
        cpu.setCpuMode(M6502::CpuMode::CMOS);

        // A long blob first (8 RamWorks banks), then a short one into the
        // SAME buffer. Before the fix the second write left the first one's
        // tail behind and the reader saw a corrupt file.
        std::vector<uint8_t> scratch;
        {
            Memory fatMem;
            fatMem.setIIEMode(true);
            fatMem.setRamWorksBanks(8);
            M6502 fatCpu(&fatMem);
            fatCpu.setCpuMode(M6502::CpuMode::CMOS);
            pom2::SnapshotWriter w(scratch);
            pom2::captureMachineState(w, fatCpu, fatMem, false);
            check(w.finish(), "#29: the long capture finished");
        }
        const std::size_t longSize = scratch.size();

        {
            pom2::SnapshotWriter w(scratch);
            pom2::captureMachineState(w, cpu, mem, false);
            check(w.finish(), "#29: the short capture finished");
        }
        check(scratch.size() < longSize,
              "#29: the reused sink shrank to the short snapshot's length");

        Memory dest;
        dest.setIIEMode(true);
        M6502 destCpu(&dest);
        destCpu.setCpuMode(M6502::CpuMode::CMOS);
        const pom2::RestoreResult r = restore(scratch, destCpu, dest);
        check(r.ok, "#29: the reused-sink snapshot loads cleanly");
        if (!r.ok) std::fprintf(stderr, "       (error: %s)\n", r.error.c_str());
    }

    if (g_failures) {
        std::fprintf(stderr, "snapshot_identity_guard: %d failures\n",
                     g_failures);
        return 1;
    }
    std::printf("snapshot_identity_guard: OK\n");
    return 0;
}
