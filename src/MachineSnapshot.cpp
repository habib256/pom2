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

// MachineSnapshot — see MachineSnapshot.h. Extracted verbatim from the
// AiControlServer `/snapshot/save|load` handlers so the rewind ring buffer
// and the HTTP API serialize the exact same bytes.

#include "MachineSnapshot.h"

#include "Logger.h"

#include "M6502.h"
#include "Memory.h"
#include "SlotBus.h"
#include "SlotPeripheral.h"
#include "SnapshotIO.h"

#include <cstdint>
#include <vector>

namespace pom2 {

void captureMachineState(SnapshotWriter& w, M6502& cpu, Memory& mem,
                         bool includeSlots)
{
    // CPU: PC(2) A X Y P SP cpuMode (6) + absolute cycle counter (8) +
    // STP halt latch (1) = 17 B. IRQ/NMI lines are transient and
    // self-correct within a frame, so they are not persisted (see
    // SnapshotIO.h); `halted` is NOT transient — only RESET clears it.
    {
        SnapshotWriter::SectionHandle h = w.beginSection("CPU");
        w.writeU16(cpu.getProgramCounter());
        w.writeU8 (cpu.getAccumulator());
        w.writeU8 (cpu.getXRegister());
        w.writeU8 (cpu.getYRegister());
        w.writeU8 (cpu.getStatusRegister());
        w.writeU8 (cpu.getStackPointer());
        w.writeU8 (cpu.getCpuMode() == M6502::CpuMode::CMOS ? 1 : 0);
        w.writeU64(mem.getCycleCounter());
        w.writeU8 (cpu.isHalted() ? 1 : 0);
        w.endSection(h);
    }
    w.writeSection("MEM", mem.data(), 0x10000);
    // MEX (v2): aux RAM + Language-Card RAM + RamWorks banks + paging soft-
    // switches + DisplayState — everything the MEM main-64K misses.
    {
        std::vector<uint8_t> mex;
        mem.appendSnapshotState(mex);
        w.writeSection("MEX", mex.data(), mex.size());
    }
    // SLOT1..SLOT7: per-card volatile runtime state (e.g. DiskIICard's head
    // position + LSS). Opt-in: only the rewind path wants these (the
    // AI-control file snapshot keeps disk/slot state excluded — see header).
    // Cards with nothing rewindable append nothing → no section.
    if (includeSlots) {
        SlotBus& bus = mem.slotBus();
        for (int slot = 1; slot <= 7; ++slot) {
            SlotPeripheral* card = bus.peripheral(slot);
            if (!card) continue;
            std::vector<uint8_t> blob;
            card->appendSnapshotState(blob);
            if (blob.empty()) continue;
            const char name[6] = { 'S', 'L', 'O', 'T',
                                   static_cast<char>('0' + slot), '\0' };
            w.writeSection(name, blob.data(), blob.size());
        }
    }
}

namespace {
// "SLOTn" → n in 1..7, else 0 (not a per-slot section).
int parseSlotSection(const std::string& name)
{
    if (name.size() == 5 && name.compare(0, 4, "SLOT") == 0 &&
        name[4] >= '1' && name[4] <= '7')
        return name[4] - '0';
    return 0;
}
}  // namespace

namespace {

RestoreResult applyMachineState(SnapshotReader& r, M6502& cpu, Memory& mem,
                                bool allowSlots)
{
    // Disarm any live DMA bus master (SoftCard Z80) BEFORE restoring.
    // File snapshots are captured with includeSlots=false, so the incoming
    // blob usually has no SLOTn section to overwrite a claimant's state —
    // without this, loading a snapshot while CP/M runs left the Z80
    // enabled and executing over the freshly restored RAM at a stale PC,
    // and the restored 6502 never ran (2026-07-12 bug hunt). onReset is
    // the bus-accurate verb (MAME reset_from_bus). Snapshots that DO
    // carry a SLOTn blob (rewind ring) re-arm the card right below.
    // LAZY: run this only once we are about to apply a section that
    // actually mutates the machine. A file that turns out to be empty,
    // foreign or truncated-at-the-first-header used to kick a live bus
    // master off the bus before anything was even read.
    bool dmaDisarmed = false;
    auto disarmDmaOnce = [&]() {
        if (dmaDisarmed) return;
        dmaDisarmed = true;
        for (int s = 0; s < SlotBus::kSlotCount; ++s) {
            SlotPeripheral* card = mem.slotBus().peripheral(s);
            if (card && card->dmaActive())
                card->onReset();
        }
    };

    std::string name;
    uint32_t len = 0;
    bool appliedCore = false;   // CPU or MEM actually restored
    while (r.nextSection(name, len)) {
        // Require the FULL 16-byte CPU section. The block below consumes 16
        // bytes unconditionally; a gate of `>= 9` let a crafted/truncated
        // section (9..15 B) read up to 7 bytes past it → garbage cycle
        // counter / CPU mode. A normal save always writes exactly 16.
        if (name == "CPU" && len >= 16) {
            disarmDmaOnce();
            appliedCore = true;
            const uint16_t pc      = r.readU16();
            const uint8_t  a       = r.readU8();
            const uint8_t  x       = r.readU8();
            const uint8_t  y       = r.readU8();
            const uint8_t  p       = r.readU8();
            const uint8_t  sp      = r.readU8();
            const uint8_t  cpuMode = r.readU8();
            const uint64_t cycles  = r.readU64();
            // v1.1 tail: STP halt latch. Legacy 16-byte blobs predate the
            // halted capture and were (almost) always taken while running
            // — clearing is the correct default AND fixes the common
            // rewind-out-of-a-crash case, where the live `halted` used to
            // survive the restore and keep the machine frozen.
            const bool halted = (len >= 17) ? (r.readU8() != 0) : false;
            cpu.setProgramCounter(pc);
            // cpuMode is read to keep the section cursor math intact but
            // NOT applied: CPU mode is machine CONFIGURATION (profile +
            // cpu_mode_override, with resolveCpuMode's soldered-65C02
            // clamp on //c-class), not machine state. Applying a foreign
            // snapshot's byte bypassed that clamp — an NMOS-mode blob
            // loaded on a //c forced its 65C02 ROM onto an NMOS core (KIL
            // freeze), and the override persisted across resets. Same
            // precedent as MEX's iieMode field (Memory.cpp).
            (void)cpuMode;
            cpu.setAccumulator(a);
            cpu.setXRegister(x);
            cpu.setYRegister(y);
            cpu.setStatusRegister(p);
            cpu.setStackPointer(sp);
            cpu.setHalted(halted);
            mem.setCycleCounter(cycles);
        } else if (name == "MEM" && len == 0x10000) {
            // Restore the main 64 KB through writable[] so the ROM mirror in
            // $C000-$FFFF isn't clobbered (LC RAM is restored via MEX).
            disarmDmaOnce();
            appliedCore = true;
            std::vector<uint8_t> buf(0x10000);
            r.readBytes(buf.data(), buf.size());
            mem.restoreMainRam(buf.data(), buf.size());
        } else if (name == "MEX") {
            // Bound the allocation. nextSection() already rejects len > blob
            // size; cap here too (a legit MEX is ≤ ~11 MB: aux + LC + 128
            // RamWorks banks) so even a large crafted file can't OOM us.
            constexpr uint32_t kMaxMexBytes = 16u * 1024u * 1024u;
            if (len > kMaxMexBytes) {
                return { false, "snapshot MEX section too large" };
            }
            disarmDmaOnce();
            std::vector<uint8_t> buf(len);
            if (len) r.readBytes(buf.data(), len);
            // Surface a malformed MEX honestly. The public wrapper restores
            // its pre-load checkpoint if any section fails after mutation.
            if (!mem.loadSnapshotState(buf.data(), len)) {
                return { false, "snapshot MEX section truncated or malformed" };
            }
        } else if (const int slot = parseSlotSection(name)) {
            if (!allowSlots)
                return { false, "snapshot SLOT sections are rewind-only" };
            // Per-card state. Bound the alloc (nextSection already rejects
            // len > blob size; cap again so a crafted file can't OOM us — a
            // real card blob is well under this).
            constexpr uint32_t kMaxSlotBytes = 1u * 1024u * 1024u;
            if (len > kMaxSlotBytes) { r.skipCurrentSection(); continue; }
            disarmDmaOnce();
            std::vector<uint8_t> buf(len);
            if (len) r.readBytes(buf.data(), len);
            // Apply only if a card sits there now; a card type mismatch is
            // handled by each card ignoring foreign (magic-tagged) blobs.
            if (SlotPeripheral* card = mem.slotBus().peripheral(slot))
                card->loadSnapshotState(buf.data(), len);
        } else {
            r.skipCurrentSection();
        }
    }
    // The section loop exits on BOTH clean EOF and mid-file truncation
    // (nextSection returns false either way). Distinguish them: a section
    // whose declared length runs past EOF sets ok=false + errorMsg, and a
    // stream failbit means a torn header — both left the machine
    // HALF-restored while this function reported success. good() is
    // `ok && !fail()`, so a normal EOF (eofbit only) still passes.
    if (!r.good()) {
        return { false,
                 r.error().empty() ? "snapshot truncated or corrupt"
                                   : r.error() };
    }
    if (!appliedCore) {
        // Well-formed but carrying neither CPU nor MEM: nothing was
        // restored, so reporting success would leave the caller (and the
        // user) believing a load happened.
        return { false, "snapshot contains no restorable CPU/MEM sections" };
    }
    // The clock has just moved, possibly backwards, so the beam-race event
    // log is stale by construction. MEX's own restore does this — but SECTION
    // ORDER COMES FROM THE FILE and is not constrained, so a file that puts
    // CPU last applied `setCycleCounter` after that cleanup and left the log
    // holding future-stamped events. Doing it here, once, after everything
    // has been applied, is order-independent. Idempotent, so the MEX path
    // doing it too costs nothing.
    mem.resetVideoEventLogForClockJump();
    return {};
}

}  // namespace

RestoreResult restoreMachineState(SnapshotReader& r, M6502& cpu, Memory& mem,
                                  bool transactional)
{
    if (!transactional)
        return applyMachineState(r, cpu, mem, /*allowSlots=*/true);

    // First consume and frame-check the ENTIRE untrusted stream without
    // touching the machine. File/API snapshots never write SLOT sections;
    // rejecting them here keeps host-backed cards (TCP sockets, FujiNet link)
    // out of rollback entirely — those external sessions cannot be replayed.
    constexpr uint32_t kMaxMexBytes = 16u * 1024u * 1024u;
    std::vector<uint8_t> staged;
    SnapshotWriter stagedWriter(staged);
    std::string name;
    uint32_t len = 0;
    bool haveCpu = false;
    bool haveMem = false;
    bool haveMex = false;
    while (r.nextSection(name, len)) {
        if (parseSlotSection(name))
            return { false, "snapshot SLOT sections are rewind-only" };
        if (name == "CPU") {
            if (haveCpu) return { false, "snapshot contains duplicate CPU sections" };
            if (len != 16 && len != 17)
                return { false, "snapshot CPU section has an invalid length" };
            haveCpu = true;
        } else if (name == "MEM") {
            if (haveMem) return { false, "snapshot contains duplicate MEM sections" };
            if (len != 0x10000)
                return { false, "snapshot MEM section has an invalid length" };
            haveMem = true;
        } else if (name == "MEX") {
            if (haveMex) return { false, "snapshot contains duplicate MEX sections" };
            if (len > kMaxMexBytes)
                return { false, "snapshot MEX section too large" };
            haveMex = true;
        }

        const bool keep = name == "CPU" || name == "MEM" ||
                          name == "MEX";
        if (!keep) {
            r.skipCurrentSection();
            continue;
        }
        std::vector<uint8_t> payload(len);
        if (len) r.readBytes(payload.data(), payload.size());
        stagedWriter.writeSection(name, payload.data(), payload.size());
    }
    if (!r.good()) {
        return { false, r.error().empty() ? "snapshot truncated or corrupt"
                                          : r.error() };
    }
    if (!haveCpu || !haveMem)
        return { false, "snapshot must contain one valid CPU and MEM section" };
    // MEX is the ONLY carrier of the IIe paging mode, the language-card
    // latches, all 64 KB of aux RAM, the RamWorks banks and the display state.
    // Without it, restore replaces main RAM, the CPU and the clock while every
    // soft switch keeps whatever the LIVE session had — and it used to report
    // success. On the default //e profile with ALTZP set, zero page and the
    // stack then resolve to the wrong 64 KB and the machine dies at once, with
    // nothing anywhere to explain why.
    //
    // v1 files predate the section, so they still load — but say so, because
    // the same half-restore applies to them.
    if (!haveMex) {
        if (r.version() >= 2)
            return { false, "snapshot has no MEX section: paging, aux RAM and "
                            "the language card would keep the live machine's "
                            "values while main RAM and the CPU are replaced" };
        pom2::log().warn("Snapshot",
            "v1 snapshot has no MEX section — paging, aux RAM, the language "
            "card and the display mode keep their CURRENT values; only main "
            "RAM and the CPU are restored");
    }
    if (!stagedWriter.finish())
        return { false, "cannot stage snapshot for validation" };

    // Memory's MEX parser includes optional, length-prefixed device state and
    // can therefore reject a blob whose outer section framing is valid.  Run
    // that semantic check before touching CPU state or resetting a live DMA
    // claimant.  The parser is currently apply-oriented, so immediately put
    // back a private MEX checkpoint; this affects no SLOT/host resources and
    // makes a rejected file observationally transactional to the machine.
    if (haveMex) {
        std::vector<uint8_t> candidateMex;
        SnapshotReader mexReader(staged.data(), staged.size());
        while (mexReader.nextSection(name, len)) {
            if (name != "MEX") { mexReader.skipCurrentSection(); continue; }
            candidateMex.resize(len);
            if (len) mexReader.readBytes(candidateMex.data(), len);
            break;
        }
        std::vector<uint8_t> oldMex;
        mem.appendSnapshotState(oldMex);
        const bool valid = mem.loadSnapshotState(candidateMex.data(),
                                                 candidateMex.size());
        const bool restored = mem.loadSnapshotState(oldMex.data(), oldMex.size());
        if (!restored)
            return { false, "snapshot MEX validation checkpoint could not be restored" };
        if (!valid)
            return { false, "snapshot MEX section truncated or malformed" };
    }

    // MEX's nested device payload is semantic rather than framing-only. Keep
    // one reversible memory checkpoint for that final validation/apply. Slot
    // state is deliberately excluded, so rollback cannot hang up a live NIC.
    std::vector<uint8_t> rollback;
    {
        SnapshotWriter w(rollback);
        captureMachineState(w, cpu, mem, /*includeSlots=*/false);
        if (!w.finish())
            return { false, "cannot create snapshot rollback checkpoint" };
    }

    SnapshotReader stagedReader(staged.data(), staged.size());
    RestoreResult result = applyMachineState(stagedReader, cpu, mem,
                                             /*allowSlots=*/false);
    if (result.ok) {
        // Say it out loud (2026-09-06). `allowSlots=false` is the documented
        // FILE contract — an archival snapshot can outlive a media swap, so a
        // stale head position or a primed write block must not come back with
        // it — but nothing ever told the user, and the visible symptom is a
        // restored machine whose disk I/O, printer and sound cards are simply
        // where the live session left them. Logged on the file path only; the
        // rewind ring restores slots and never reaches here.
        pom2::log().info("Snapshot",
            "restored CPU + RAM. Card state (disk heads, sound chips, "
            "printer, network) is NOT part of a .pom2snap file and keeps its "
            "live values — only Rewind restores it.");
        return result;
    }

    SnapshotReader before(rollback.data(), rollback.size());
    const RestoreResult rolledBack = applyMachineState(before, cpu, mem,
                                                       /*allowSlots=*/false);
    if (!rolledBack.ok) {
        return { false, result.error + "; rollback failed: " +
                        rolledBack.error };
    }
    return result;
}

}  // namespace pom2
