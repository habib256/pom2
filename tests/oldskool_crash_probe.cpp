// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// OLDSKOOL crash probe — diagnostic, not a pinned test.
//
// The standalone `oldskool.dsk` (French Touch, 8KB intro) plays its SHADOW
// intro for ~10 s then blows the 6502 stack (SP -> $00) and lands in DOS
// ($BA37). The DIX-packaged copy runs fine. This probe boots the disk, types
// BRUN OLDSKOOL, single-steps with a ring buffer of (PC, opcode, SP), and
// dumps the last N instructions + the IRQ vector + an interrupt-entry census
// the moment the stack runs low — to see WHAT fills the stack (an unhandled
// IRQ storm being the prime suspect).
//
// Usage: oldskool_crash_probe [nmos|cmos] [bootSecs]

#include "CpuClock.h"
#include "M6502.h"
#include "Memory.h"
#include "DiskIICard.h"
#include "Mockingboard.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {
std::string firstExisting(std::initializer_list<const char*> cands) {
    namespace fs = std::filesystem;
    for (const char* c : cands) {
        std::error_code ec;
        if (fs::exists(c, ec)) return c;
        const std::string up = std::string("../") + c;
        if (fs::exists(up, ec)) return up;
    }
    return {};
}
}  // namespace

int main(int argc, char** argv) {
    const std::string mode = (argc > 1) ? argv[1] : "nmos";
    const int bootSecs = (argc > 2) ? std::atoi(argv[2]) : 14;
    const bool noMB = (argc > 3) && std::string(argv[3]) == "nomb";

    const std::string rom  = firstExisting({"roms/apple2e.rom"});
    const std::string boot = firstExisting({"roms/disk2.rom"});
    const std::string dsk  = firstExisting({"disks_5.4/demo/oldskool/oldskool.dsk"});
    if (rom.empty() || boot.empty() || dsk.empty()) { std::printf("SKIP: missing files\n"); return 0; }

    Memory mem;
    mem.clearRam(); mem.resetSoftSwitches(); mem.setIIEMode(true);
    mem.setVideoStandard(VideoStandard::PAL);
    if (!mem.loadAppleIIRom(rom.c_str(), false)) { std::printf("FAIL rom\n"); return 1; }
    M6502 cpu(&mem); mem.setCpu(&cpu);
    MockingboardCard* mbp = nullptr;
    if (!noMB) {
        auto mb = std::make_unique<MockingboardCard>(4, MockingboardCard::Variant::AC);
        mb->setCpu(&cpu);
        mbp = mb.get();
        mem.slotBus().plug(4, std::move(mb));
    }
    auto d2 = std::make_unique<DiskIICard>();
    if (!d2->loadBootRom(boot) || !d2->insertDisk(dsk)) { std::printf("FAIL disk\n"); return 1; }
    mem.slotBus().plug(6, std::move(d2));
    cpu.setCpuMode(mode == "cmos" ? M6502::CpuMode::CMOS : M6502::CpuMode::NMOS);
    cpu.hardReset(); mem.slotBus().reset();

    std::printf("cpu=%s MB=%s booting for %d s then BRUN OLDSKOOL...\n",
                mode.c_str(), noMB ? "off" : "on", bootSecs);
    for (int s = 0; s < bootSecs; ++s) cpu.run(POM2_TIMING_PAL.cpuClockHz);
    const char* cmd = "BRUN OLDSKOOL\r";
    for (const char* c = cmd; *c; ++c) {
        mem.queueKey(static_cast<uint8_t>(*c));
        cpu.run(POM2_TIMING_PAL.cpuClockHz / 10);
    }

    // Single-step, ring-buffering (PC, opcode, SP, P). Census interrupt
    // entries: a taken IRQ/BRK pushes 3 bytes and vectors to $FFFE. We detect
    // "PC just became the IRQ handler with SP dropped by 3" as an interrupt.
    struct Rec { uint16_t pc; uint8_t op, sp, p, a; };
    constexpr size_t kRing = 512;
    std::vector<Rec> ring(kRing); size_t rn = 0;
    const uint16_t irqVec = mem.memRead(0xFFFE) | (mem.memRead(0xFFFF) << 8);
    uint64_t irqEntries = 0, lastSp = 0x1FF;
    uint8_t minSp = 0xFF;

    // Track the FIRST time IER.T1 (bit6) becomes enabled on either VIA, and
    // the FIRST time the slot IRQ line asserts — with the PC that caused it.
    auto ier = [&](int chip) { return mbp ? mbp->peekViaRegister(chip, 0x0E) : 0; };
    auto acr = [&](int chip) { return mbp ? mbp->peekViaRegister(chip, 0x0B) : 0; };
    auto ifr = [&](int chip) { return mbp ? mbp->peekViaRegister(chip, 0x0D) : 0; };
    bool ierT1Seen = false, irqSeen = false;
    // Measure the IRQ-entry latency the stable-raster handler depends on:
    // CPU cycles from the T1 underflow (IFR.T1 rising edge) to the handler's
    // SBC $C404 read at $82F7, and the T1CL it reads there.
    uint8_t prevIfrT1 = 0; uint64_t underflowCycle = 0; int measured = 0;

    const long long maxSteps = 60'000'000;  // ~generous
    bool dumped = false;
    for (long long i = 0; i < maxSteps && !dumped; ++i) {
        const uint16_t pc = cpu.getProgramCounter();
        const uint8_t sp = cpu.getStackPointer();
        const uint8_t op = mem.memRead(pc);
        ring[rn % kRing] = { pc, op, sp, cpu.getStatusRegister(), cpu.getAccumulator() };
        ++rn;
        if (sp < minSp) minSp = sp;
        if (mbp && measured < 4) {
            // IFR.T1 rising edge = the underflow that raised the IRQ.
            const uint8_t ifrT1 = ifr(0) & 0x40;
            if (ifrT1 && !prevIfrT1) underflowCycle = cpu.getCycleCountNow();
            prevIfrT1 = ifrT1;
            // The handler's phase read at $82F7 (SBC $C404).
            if (pc == 0x82F7) {
                const uint64_t readCycle = cpu.getCycleCountNow();
                std::printf(">> phase read #%d: underflow=%llu read=%llu "
                            "elapsed=%llu cyc  T1CL($C404 peek)=$%02X  A(mem[$03])=$%02X\n",
                            measured + 1,
                            (unsigned long long)underflowCycle,
                            (unsigned long long)readCycle,
                            (unsigned long long)(readCycle - underflowCycle),
                            mbp->peekViaRegister(0, 0x04), mem.memRead(0x03));
                ++measured;
            }
        }
        if (mbp) {
            if (!ierT1Seen && ((ier(0) | ier(1)) & 0x40)) {
                ierT1Seen = true;
                std::printf(">> IER.T1 first enabled near PC=$%04X: "
                            "VIA1 IER=$%02X ACR=$%02X  VIA2 IER=$%02X ACR=$%02X\n",
                            pc, ier(0), acr(0), ier(1), acr(1));
            }
            if (!irqSeen && mbp->isIrqAsserted()) {
                irqSeen = true;
                std::printf(">> slot IRQ line FIRST asserted near PC=$%04X: "
                            "VIA1 IER=$%02X IFR=$%02X ACR=$%02X | VIA2 IER=$%02X IFR=$%02X ACR=$%02X | I-flag=%d\n",
                            pc, ier(0), ifr(0), acr(0), ier(1), ifr(1), acr(1),
                            (cpu.getStatusRegister() >> 2) & 1);
            }
        }
        // IRQ-entry: SP dropped by exactly 3 in one step (a taken IRQ pushes
        // PCH,PCL,P). Capture the LIVE vector the CPU used ($FFFE reflects the
        // current LC bank) the first few times.
        if (((sp + 3) & 0xFF) == (lastSp & 0xFF)) {
            ++irqEntries;
            if (irqEntries <= 4) {
                const uint16_t liveVec = mem.memRead(0xFFFE) | (mem.memRead(0xFFFF) << 8);
                std::printf(">> IRQ #%llu taken -> PC=$%04X  live $FFFE=$%04X  "
                            "$D000=%02X  P=$%02X\n",
                            (unsigned long long)irqEntries, pc, liveVec,
                            mem.memRead(0xD000), cpu.getStatusRegister());
            }
            if (irqEntries == 1) {
                std::printf("   handler bytes $82EE: ");
                for (int k = 0; k < 0x30; ++k) std::printf("%02X ", mem.memRead(0x82EE + k));
                std::printf("\n   ...$831E: ");
                for (int k = 0; k < 0x30; ++k) std::printf("%02X ", mem.memRead(0x831E + k));
                std::printf("\n   ...$83AE-$83CE: ");
                for (int k = 0; k < 0x21; ++k) std::printf("%02X ", mem.memRead(0x83AE + k));
                std::printf("\n");
            }
        }
        lastSp = sp;

        // trip: stack nearly exhausted, or we reached DOS warm ($BA37), or BRK
        if (sp < 0x08 || pc == 0xBA37 || op == 0x00) {
            dumped = true;
            std::printf("\n=== TRIP at PC=$%04X op=$%02X SP=$%02X (minSp seen $%02X) ===\n",
                        pc, op, sp, minSp);
            std::printf("IRQ vector ($FFFE)=$%04X  interrupt-entries counted=%llu\n",
                        irqVec, (unsigned long long)irqEntries);
            std::printf("IRQ handler bytes @ $%04X: ", irqVec);
            for (int k = 0; k < 12; ++k) std::printf("%02X ", mem.memRead(irqVec + k));
            std::printf("\nCPU I-flag=%d  (P=$%02X)\n",
                        (cpu.getStatusRegister() >> 2) & 1, cpu.getStatusRegister());
            if (mbp)
                std::printf("VIA1 IER=$%02X IFR=$%02X ACR=$%02X | VIA2 IER=$%02X IFR=$%02X ACR=$%02X | irqLine=%d\n",
                            ier(0), ifr(0), acr(0), ier(1), ifr(1), acr(1), mbp->isIrqAsserted());
            std::printf("user IRQ vec $03FE=$%02X%02X\n",
                        mem.memRead(0x03FF), mem.memRead(0x03FE));
            // Mockingboard VIA IRQ state (slot 4)
            const size_t show = std::min<size_t>(rn, 300);
            std::printf("--- last %zu instructions (newest last) ---\n", show);
            for (size_t k = rn - show; k < rn; ++k) {
                const Rec& r = ring[k % kRing];
                std::printf("  $%04X op=%02X SP=%02X P=%02X A=%02X\n", r.pc, r.op, r.sp, r.p, r.a);
            }
            break;
        }
        cpu.step();
    }
    if (!dumped) std::printf("no trip in %lld steps (minSp seen $%02X)\n", maxSteps, minSp);
    return 0;
}
