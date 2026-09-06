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

// DIX "press RETURN at the menu" crash probe — diagnostic, not a pinned
// test.
//
// User report: boot DIX (disks_3.5/DIX.po) on //e PAL, press RETURN, and
// the demo dies. This probe rebuilds the DIX target machine headlessly
// (//e PAL + slot-4 Mockingboard + slot-5 Liron-class SmartPort holding
// the 800K .po), boots it, injects a key sequence at a chosen emulated
// time, and watches for the death signatures (video checksum frozen, PC
// pinned, BRK executed = PC ran away into unwritten RAM).
//
// VERDICT (2026-08-08): **not a POM2 bug — a DIX off-by-one.** Confirmed
// against DIX's own GPLv3 sources (Fr3nchT0uch/DIX): `MENU/main.a:178-222`
// and `loader.a:117-153`, both using `CurrentChoice = $DFFF`. The menu
// keeps the highlighted entry in $DFFF (LC bank 2) and initialises it to
// 0 at $D026 (`LDA #$00 / STA CurrentChoice`, loader.a:119-121). Only the
// arrow keys ever give it a valid value:
//
//   $E10F  LDA $C000 / BPL ...  / STA $C010
//   $E117  CMP #$88  BEQ $E129     ; LEFT  : 0 -> 17, then DEC -> 16
//   $E11B  CMP #$95  BEQ $E13E     ; RIGHT : INC -> 1
//   $E11F  CMP #$8D  BEQ $E153     ; RETURN: launch
//   $E123  CMP #$A0  BEQ $E153     ; SPACE : launch
//
// and the launcher indexes a 16-entry jump table one-based:
//
//   $D02C  LDX $DFFF
//   $D02F  DEX                     ; 0 -> $FF  <-- underflow
//   $D030  LDA $D076,X / STA $D03D ; -> $D175 = $E1
//   $D036  LDA $D086,X / STA $D03E ; -> $D185 = $17
//   $D03C  JSR $17E1               ; garbage -> BRK storm in empty RAM
//
// Index 0 is a deliberate display state (the "use arrows to select demo"
// prompt, MENU/main.a:639), but the accept path has no matching guard:
// `.return` (MENU/main.a:213-222) loads CurrentChoice into a dead A — its
// only test, `CMP #16 / BNE`, is commented out upstream — and falls
// straight through to teardown + RTS.
//
// So RETURN or SPACE *before any arrow key* jumps to $17E1. The main
// thread then grinds BRK-by-BRK through unwritten RAM while the 50 Hz
// Mockingboard IRQ music engine keeps running — screen frozen, music on.
// Pressing LEFT or RIGHT first makes RETURN load and run the part
// normally (verified here: SmartPort reads at $C58B, part runs at $7xxx
// with video moving). "Right after launch" is a red herring: the key
// simply sits in the hardware latch until the menu polls $C000.
//
// Usage:
//   dix_return_crash_probe [--key-at <emulated seconds>] [--run <seconds>]
//                          [--keys 15,0D] [--no-key] [--trace]
//                          [--disk <path.po>]
//
//   --disk   the 800K .po to boot (default disks_3.5/DIX.po). Point it at
//            disks_3.5/DIX-fix.po (built by tools/make_dix_fix.py) to see
//            the same RETURN reach AUTOMODE instead of the BRK storm.
//   --keys   comma-separated hex Apple II key codes, 0.5 s apart
//            (08 = LEFT, 15 = RIGHT, 0D = RETURN, 20 = SPACE).
//            Default: a single RETURN.
//
// Run from build/ or the repo root (paths probe both prefixes).

#include "M6502.h"
#include "Memory.h"
#include "Mockingboard.h"
#include "SlotBus.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string firstExisting(const std::vector<std::string>& candidates)
{
    namespace fs = std::filesystem;
    for (const auto& p : candidates) {
        std::error_code ec;
        if (fs::exists(p, ec)) return p;
        const std::string up1 = "../" + p;
        if (fs::exists(up1, ec)) return up1;
        const std::string up2 = "../../" + p;
        if (fs::exists(up2, ec)) return up2;
    }
    return {};
}

void dumpText(Memory& mem)
{
    static const uint16_t kRowBase[24] = {
        0x400,0x480,0x500,0x580,0x600,0x680,0x700,0x780,
        0x428,0x4A8,0x528,0x5A8,0x628,0x6A8,0x728,0x7A8,
        0x450,0x4D0,0x550,0x5D0,0x650,0x6D0,0x750,0x7D0,
    };
    for (int r = 0; r < 24; ++r) {
        std::printf("R%02d: '", r);
        for (int c = 0; c < 40; ++c) {
            const uint8_t b = mem.memRead(static_cast<uint16_t>(kRowBase[r] + c));
            const char ch = static_cast<char>(b & 0x7F);
            std::putchar(ch >= 0x20 && ch < 0x7F ? ch : '.');
        }
        std::printf("'\n");
    }
}

}  // namespace

int main(int argc, char** argv)
{
    std::vector<uint8_t> keys;      // extra keys sent before the RETURN
    double keyAtSec = 1.0;
    double runSec   = 25.0;
    bool   sendKey  = true;
    bool   trace    = false;
    std::string diskArg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a == "--key-at" && i + 1 < argc) keyAtSec = std::atof(argv[++i]);
        else if (a == "--run"    && i + 1 < argc) runSec   = std::atof(argv[++i]);
        else if (a == "--no-key")                 sendKey  = false;
        else if (a == "--disk"   && i + 1 < argc) diskArg  = argv[++i];
        else if (a == "--keys" && i + 1 < argc) {
            // Comma-separated hex Apple II key codes sent 0.5 s apart,
            // starting at --key-at. e.g. --keys 15,0D = RIGHT then RETURN.
            const std::string list = argv[++i];
            size_t pos = 0;
            while (pos < list.size()) {
                size_t comma = list.find(',', pos);
                if (comma == std::string::npos) comma = list.size();
                keys.push_back(uint8_t(std::stoul(list.substr(pos, comma - pos),
                                                  nullptr, 16)));
                pos = comma + 1;
            }
        }
        else if (a == "--trace")                  trace    = true;
        else { std::printf("unknown arg %s\n", a.c_str()); return 2; }
    }

    const std::string rom = firstExisting({"roms/apple2e.rom"});
    const std::string po  = firstExisting(diskArg.empty()
                                          ? std::vector<std::string>{"disks_3.5/DIX.po"}
                                          : std::vector<std::string>{diskArg});
    if (rom.empty() || po.empty()) {
        std::printf("SKIP: missing apple2e.rom or disks_3.5/DIX.po\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    Memory mem;
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    mem.setVideoStandard(VideoStandard::PAL);
    if (!mem.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/false)) {
        std::printf("FAIL: load %s\n", rom.c_str());
        return 1;
    }

    M6502 cpu(&mem);
    mem.setCpu(&cpu);

    // Slot 4: Mockingboard A/C — DIX probes it at boot ("BADGUY").
    auto mb = std::make_unique<MockingboardCard>(4, MockingboardCard::Variant::AC);
    mb->setCpu(&cpu);
    mem.slotBus().plug(4, std::move(mb));

    // Slot 5: Liron-class SmartPort, unit 0 = the DIX 800K .po.
    auto card = std::make_unique<pom2::SmartPortCard>(5);
    auto u0   = std::make_unique<pom2::SmartPort35Unit>();
    if (!u0->loadImage(po)) {
        std::printf("FAIL: %s: %s\n", po.c_str(), u0->lastError().c_str());
        return 1;
    }
    std::printf("Mounted %s (%u blocks)\n", po.c_str(), u0->blockCount());
    card->setUnit(0, std::move(u0));
    mem.slotBus().plug(5, std::move(card));

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    mem.slotBus().reset();

    const int kClock = POM2_TIMING_PAL.cpuClockHz;
    const long long keyAtCycle = static_cast<long long>(keyAtSec * kClock);
    const long long endCycle   = static_cast<long long>(runSec   * kClock);

    bool keySent = false;
    long long cycles = 0;

    // Rolling death detectors.
    uint16_t lastPc = 0;
    long long sameWindowSince = 0;
    uint16_t windowLo = 0xFFFF, windowHi = 0;
    bool reportedHang = false;

    // Per-second bucket: which PC page dominated, and whether the video
    // memory moved at all (a frozen demo = identical checksums second
    // after second).
    std::map<uint16_t, long long> pcPage;
    uint64_t prevMainCk = 0, prevAuxCk = 0;
    int frozenRun = 0;
    long long nextSampleCycle = kClock;

    auto checksum = [](const uint8_t* p, size_t lo, size_t hi) {
        uint64_t h = 1469598103934665603ULL;
        for (size_t i = lo; i < hi; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
        return h;
    };
    std::vector<uint8_t> mainCopy(0x10000);

    // Ring buffer of the last N instructions, dumped the first time the
    // CPU executes a BRK ($00) after the key — i.e. the instant the PC
    // ran away into unwritten RAM.
    struct Rec { uint16_t pc; uint8_t op, a, x, y, sp, p; };
    constexpr size_t kRing = 4000000;
    std::vector<Rec> ring(kRing);
    size_t ringN = 0;
    bool dumpedRunaway = false;

    const long long kStepChunk = 1000;
    while (cycles < endCycle) {
        for (int i = 0; i < kStepChunk; ++i) {
            const uint16_t pc = cpu.getProgramCounter();
            pcPage[static_cast<uint16_t>(pc & 0xFF00)]++;

            if (!dumpedRunaway) {
                const uint8_t op = mem.memRead(pc);
                ring[ringN % kRing] = { pc, op, cpu.getAccumulator(),
                                        cpu.getXRegister(), cpu.getYRegister(),
                                        cpu.getStackPointer(),
                                        cpu.getStatusRegister() };
                ++ringN;
                if (keySent && op == 0x00) {
                    dumpedRunaway = true;
                    std::printf("\n=== RUNAWAY: BRK at $%04X, %.3fs "
                                "(%.3fs after the key) ===\n",
                                pc, double(cycles) / kClock,
                                double(cycles - keyAtCycle) / kClock);
                    const size_t show = std::min<size_t>(ringN, 400000);
                    for (size_t k = ringN - show; k < ringN; ++k) {
                        const Rec& r = ring[k % kRing];
                        std::printf("  %5zu $%04X op=%02X A=%02X X=%02X "
                                    "Y=%02X SP=%02X P=%02X\n",
                                    ringN - k, r.pc, r.op, r.a, r.x, r.y,
                                    r.sp, r.p);
                    }
                    auto hex = [&](uint16_t lo, uint16_t hi, const char* tag) {
                        std::printf("  %s $%04X:", tag, lo);
                        for (uint16_t a = lo; a < hi; ++a) {
                            if ((a - lo) % 16 == 0) std::printf("\n    $%04X ", a);
                            std::printf("%02X ", mem.memRead(a));
                        }
                        std::printf("\n");
                    };
                    hex(0xD000, 0xD060, "code");
                    hex(0xD060, 0xD0C6, "tables");
                    hex(0xD0C6, 0xD190, "menu");
                    hex(0xE100, 0xE170, "kbd");
                    // Who writes $DFFF? Scan every visible byte for
                    // STA/INC/DEC $DFFF, in each LC bank.
                    for (int bank = 2; bank >= 1; --bank) {
                        (void)mem.memRead(bank == 1 ? 0xC08B : 0xC083);
                        (void)mem.memRead(bank == 1 ? 0xC08B : 0xC083);
                        std::printf("  --- LC bank %d: refs to $DFFF ---\n", bank);
                        for (uint32_t a = 0x0800; a < 0xFFFD; ++a) {
                            if (a >= 0xC000 && a < 0xC100) continue;
                            const uint8_t o = mem.memRead(uint16_t(a));
                            if (mem.memRead(uint16_t(a+1)) != 0xFF ||
                                mem.memRead(uint16_t(a+2)) != 0xDF) continue;
                            const char* n = o == 0x8D ? "STA" : o == 0xAD ? "LDA"
                                          : o == 0xEE ? "INC" : o == 0xCE ? "DEC"
                                          : o == 0xAE ? "LDX" : o == 0xAC ? "LDY"
                                          : o == 0x8E ? "STX" : o == 0x8C ? "STY"
                                          : nullptr;
                            if (n) std::printf("    $%04X  %s $DFFF\n", a, n);
                        }
                    }
                    // $DFFF is DIX's "selected part" byte. Which LC bank
                    // does it actually live in?
                    std::printf("  $DFFF (current bank) = %02X\n",
                                mem.memRead(0xDFFF));
                    (void)mem.memRead(0xC08B); (void)mem.memRead(0xC08B);
                    std::printf("  $DFFF (LC bank 1)    = %02X\n",
                                mem.memRead(0xDFFF));
                    (void)mem.memRead(0xC083); (void)mem.memRead(0xC083);
                    std::printf("  $DFFF (LC bank 2)    = %02X\n",
                                mem.memRead(0xDFFF));
                    std::fflush(stdout);
                }
            }

            // Track a small PC window to spot hard hangs.
            if (pc < windowLo) windowLo = pc;
            if (pc > windowHi) windowHi = pc;
            if (windowHi - windowLo > 64) {
                windowLo = windowHi = pc;
                sameWindowSince = cycles;
            } else if (!reportedHang && cycles - sameWindowSince > 3LL * kClock) {
                std::printf("[%.3fs] HANG: PC pinned in $%04X-$%04X for 3s\n",
                            double(cycles) / kClock, windowLo, windowHi);
                reportedHang = true;
            }

            lastPc = pc;
            cpu.step();
            cycles = static_cast<long long>(mem.getCycleCounter());
        }

        if (sendKey && !keySent && cycles >= keyAtCycle) {
            if (keys.empty()) keys.push_back(0x0D);
            // Deliver the sequence 0.5 s apart so the menu's poll loop
            // sees each keystroke separately.
            for (size_t k = 0; k < keys.size(); ++k) {
                std::printf("[%.3fs] >>> injecting key $%02X (PC=$%04X)\n",
                            double(mem.getCycleCounter()) / kClock, keys[k],
                            cpu.getProgramCounter());
                std::fflush(stdout);
                mem.queueKey(keys[k]);
                if (k + 1 < keys.size()) cpu.run(kClock / 2);
            }
            cycles = static_cast<long long>(mem.getCycleCounter());
            keySent = true;
            windowLo = windowHi = lastPc;
            sameWindowSince = cycles;
            reportedHang = false;
        }

        if (cycles >= nextSampleCycle) {
            nextSampleCycle += kClock;
            for (int a = 0; a < 0x10000; ++a)
                mainCopy[a] = mem.peekMainRam(static_cast<uint16_t>(a));
            const uint64_t mck = checksum(mainCopy.data(), 0x0400, 0x6000);
            const uint64_t ack = checksum(mem.auxData(), 0x0400, 0x6000);
            const bool frozen = (mck == prevMainCk && ack == prevAuxCk);
            frozenRun = frozen ? frozenRun + 1 : 0;
            prevMainCk = mck; prevAuxCk = ack;

            // Dominant PC page for this second.
            uint16_t topPage = 0; long long topN = 0;
            for (auto& kv : pcPage) if (kv.second > topN) { topN = kv.second; topPage = kv.first; }
            pcPage.clear();

            // $C000 without touching $C010: is a keystroke still latched?
            const uint8_t kb = mem.memRead(0xC000);

            // ExpoMode (zp $04) = 1 once the loader's AUTOMODE entry is
            // running, i.e. "ALL DEMOS IN AUTOMATIC MODE" was launched.
            std::printf("[%2.0fs] pc=$%04X top=$%02Xxx  video=%s  kbd=%s  "
                        "expo=%u%s\n",
                        double(cycles) / kClock, lastPc, topPage >> 8,
                        frozen ? "FROZEN" : "moving",
                        (kb & 0x80) ? "LATCHED" : "-",
                        unsigned(mem.peekMainRam(0x04)),
                        frozenRun >= 3 ? "   <<< STUCK" : "");

            // Once wedged for 4 s, capture the loop: every distinct PC it
            // visits over 200k instructions, with the opcode byte under it.
            if (trace && keySent && frozenRun == 4) {
                // Separate the two threads: SP >= $F0 means we're on the
                // main thread's stack, below that we're inside the IRQ
                // handler (it pushes P/PC/A/X/Y).
                std::printf("--- stuck-state census (200k instructions) ---\n");
                std::map<uint16_t, long long> mainHits, irqHits;
                long long inIrq = 0, inMain = 0;
                for (int n = 0; n < 200000; ++n) {
                    const uint16_t pc = cpu.getProgramCounter();
                    if (cpu.getStackPointer() >= 0xF4) { mainHits[pc]++; ++inMain; }
                    else                               { irqHits[pc]++;  ++inIrq;  }
                    cpu.step();
                }
                std::printf("  main-thread instrs=%lld  irq-thread instrs=%lld\n",
                            inMain, inIrq);
                std::printf("  --- MAIN thread PCs ---\n");
                for (auto& kv : mainHits)
                    std::printf("    $%04X  %02X %02X %02X   x%lld\n", kv.first,
                                mem.memRead(kv.first),
                                mem.memRead(uint16_t(kv.first + 1)),
                                mem.memRead(uint16_t(kv.first + 2)), kv.second);
                std::printf("--- iieMemMode=%04X  zp: F4=%02X F5=%02X FC=%02X "
                            "FD=%02X FF=%02X  vec($FE)=%02X%02X\n",
                            mem.iieModeFlags(),
                            mem.peekMainRam(0xF4), mem.peekMainRam(0xF5),
                            mem.peekMainRam(0xFC), mem.peekMainRam(0xFD),
                            mem.peekMainRam(0xFF),
                            mem.peekMainRam(0xFF), mem.peekMainRam(0xFE));
                cycles = static_cast<long long>(mem.getCycleCounter());
            }
        }
    }

    std::printf("\nFinal: PC=$%04X  A=%02X X=%02X Y=%02X SP=%02X  cycles=%lld (%.2fs)\n",
                cpu.getProgramCounter(), cpu.getAccumulator(), cpu.getXRegister(),
                cpu.getYRegister(), cpu.getStackPointer(), cycles,
                double(cycles) / kClock);
    if (trace) { std::printf("--- text page ---\n"); dumpText(mem); }
    return 0;
}
