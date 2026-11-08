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

// French Touch's cycle-exact video sync, run on a //e PAL and a //c PAL.
//
// [A] French Touch DIX, loader.a TIMERS_SYNC (code by fenarinarsa / Cyril
// Lambin; github.com/Fr3nchT0uch/DIX @ b7a6f23). The code was tuned on real
// PAL machines, and POM2 takes it as the silicon reference. The same code
// syncs both machines on two different edges:
//   - On a //e, `$C019` is VBLBAR, so the loop finds the END of VBL.
//   - On a //c, `$C019` is VBLINT, the latch of the VBL interrupt
//     (`$C05B` arms it, `$C070` clears it), so the loop finds the START of
//     VBL, which is the end of the display.
// The precise loop is 20279 cycles, one short of a frame, so each read lands
// one cycle earlier than the last until it falls just before the edge:
//   "IIe => START OF DISPLAY + 5 cycles"
//   "IIc => END OF DISPLAY + 5 cycles"
// The //e path then waits 192 lines, so both reach "END OF DISPLAY + 11" and
// launch the Mockingboard's T1 on the same beam cycle. The two edges must
// therefore sit exactly 192 lines apart at the same horizontal phase. A //c
// edge one cycle off would move every French Touch raster on the //c, the
// browser's default machine, by one cycle.
//
// What this pins, in POM2's beam coordinates (line, cycle within the line):
//   - //e: the exiting read is at line 311 cycle 64, the last VBL cycle, and
//     the read before it at line 0 cycle 0 read $80.
//   - //c: the exiting read is at line 191 cycle 64, the last display cycle,
//     and the read before it at line 192 cycle 0 read $80.
// grouik_sync_probe measures the same thing on the real DIX disk, where both
// machines then land the synced code on the same cycle (2026-11-08).
//
// The code below is loader.a's .SyncDetect and its WAIT65 / WAIT59,
// assembled by hand byte for byte (same instructions, same cycle counts,
// same page-aligned loop).

#include "M6502.h"
#include "Memory.h"
#include "MemoryWatchSink.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

constexpr uint64_t kLine = 65, kFrame = 312 * 65;   // PAL

std::string firstExisting(const std::string& p)
{
    namespace fs = std::filesystem;
    for (const std::string& c : {p, "../" + p, "../../" + p})
        if (fs::exists(c)) return c;
    return {};
}

struct Reads : pom2::MemoryWatchSink {
    Memory* mem = nullptr;
    std::vector<std::pair<uint64_t, uint8_t>> c019;
    void noteAccess(uint16_t addr, uint8_t v, bool write) override {
        if (!write && addr == 0xC019) c019.push_back({mem->videoAccessCycle(), v});
    }
};

void poke(Memory& m, uint16_t at, std::initializer_list<uint8_t> bytes)
{
    for (uint8_t b : bytes) m.memWrite(at++, b);
}

// loader.a, transcribed. $0900: arm the //c VBL interrupt; $0A00: .SyncDetect
// (FT `!align 255`s it); $0B00: WAIT65 / WAIT62 / WAIT59 / WAIT57 / WAIT55.
void loadSyncCode(Memory& m)
{
    poke(m, 0x0900, {
        0x8D, 0x79, 0xC0,   // STA _IOUenable   ($C079)
        0x8D, 0x5B, 0xC0,   // STA _VSYNCenable ($C05B)
        0x8D, 0x78, 0xC0,   // STA _IOUdisable  ($C078)
        0xAD, 0x70, 0xC0,   // LDA _VSYNCIRFreset
        0x4C, 0x00, 0x0A,   // JMP .SyncDetect
    });
    poke(m, 0x0A00, {
        0xAD, 0x19, 0xC0,   // .SyncDetect  LDA _VERTBLANK
        0x10, 0xFB,         //              BPL -
        0xAD, 0x70, 0xC0,   //              LDA _VSYNCIRFreset
        0xAD, 0x19, 0xC0,   //              LDA _VERTBLANK
        0x30, 0xFB,         //              BMI -
        0xAD, 0x19, 0xC0,   //              LDA _VERTBLANK
        0x10, 0xFB,         //              BPL -
        0xEA, 0xEA,         //              NOP : NOP
        0xA0, 0xFF,         // $0A14 -      LDY #255
        0x20, 0x00, 0x0B,   //              JSR WAIT65      16575
        0xAD, 0x70, 0xC0,   //              LDA _VSYNCIRFreset   4
        0xA0, 0x37,         //              LDY #55
        0x20, 0x00, 0x0B,   //              JSR WAIT65       3575
        0xA0, 0x00,         //              LDY #00
        0x20, 0x0F, 0x0B,   //              JSR WAIT59         59
        0xA0, 0x00,         //              LDY #00
        0x20, 0x0F, 0x0B,   //              JSR WAIT59         59
        0xAD, 0x19, 0xC0,   //              LDA _VERTBLANK     4
        0x30, 0xE4,         //              BMI -              3 = 20279
        0x4C, 0x30, 0x0A,   // $0A30        JMP *  (precise sync here)
    });
    poke(m, 0x0B00, {
        0xA5, 0xEA,         // WAIT65  LDA $EA
        0x88,               // WAIT62  DEY
        0xF0, 0x12,         //         BEQ .S1
        0xA2, 0x0B,         // .WL1    LDX #$0B
        0xCA,               // .WL2    DEX
        0xD0, 0xFD,         //         BNE .WL2
        0xEA, 0xEA,         //         NOP : NOP
        0x88,               //         DEY
        0xD0, 0xF6,         //         BNE .WL1
        0xA2, 0x08,         // WAIT59  LDX #$08
        0xCA,               // .WL3    DEX
        0xD0, 0xFD,         //         BNE .WL3
        0xEA, 0xEA,         //         NOP : NOP
        0x60,               //         RTS
        0xEA,               // WAIT57/.S1 NOP
        0xA2, 0x08,         // WAIT55  LDX #08
        0xCA,               // .WL4    DEX
        0xD0, 0xFD,         //         BNE .WL4
        0x60,               //         RTS
    });
}

struct Exit { int line, h; uint8_t v; int prevLine, prevH; uint8_t prevV; size_t precise; bool ok; };

Exit runSync(const std::string& rom, bool iic)
{
    Exit out{};
    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    mem.setVideoStandard(VideoStandard::PAL);
    if (!mem.loadAppleIIRom(rom.c_str(), /*pickLower16KFor32K=*/iic)) return out;
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();                     // I set: "6502 interrupts must be previously disabled"
    loadSyncCode(mem);
    Reads reads;
    reads.mem = &mem;
    mem.setWatchSink(&reads);
    mem.setReadWatch(0xC019, true);
    cpu.setProgramCounter(0x0900);
    uint64_t ran = 0;
    // At most a frame's worth of one-cycle steps, plus the coarse sync.
    while (cpu.getProgramCounter() != 0x0A30 && ran < 80 * kFrame) ran += uint64_t(cpu.run(64));
    if (cpu.getProgramCounter() != 0x0A30 || reads.c019.size() < 2) return out;
    // The precise loop's reads are the ones 20279 cycles apart.
    size_t n = reads.c019.size(), precise = 0;
    for (size_t i = n - 1; i > 0 && reads.c019[i].first - reads.c019[i - 1].first == kFrame - 1; --i) ++precise;
    const auto& last = reads.c019[n - 1];
    const auto& prev = reads.c019[n - 2];
    out = {int(last.first % kFrame / kLine), int(last.first % kLine), last.second,
           int(prev.first % kFrame / kLine), int(prev.first % kLine), prev.second, precise, true};
    return out;
}

} // namespace

int main()
{
    int ran = 0;
    const std::string iie = firstExisting("roms/apple2e.rom");
    if (iie.empty()) {
        std::printf("SKIP ft_loader_sync: roms/apple2e.rom not found\n");
    } else {
        ++ran;
        const Exit e = runSync(iie, false);
        CHECK(e.ok, "//e PAL: the sync loop never exited");
        if (e.ok) {
            std::printf("  //e PAL: exit read line %d cycle %d ($%02X), before it line %d cycle %d ($%02X), %zu precise steps\n",
                        e.line, e.h, e.v, e.prevLine, e.prevH, e.prevV, e.precise);
            CHECK(e.precise >= 1, "//e PAL: the precise loop never iterated (transcription?)");
            CHECK(e.line == 311 && e.h == 64 && (e.v & 0x80) == 0,
                  "//e PAL: the exiting $C019 read must be the last VBL cycle (line 311 cycle 64), got line %d cycle %d", e.line, e.h);
            CHECK(e.prevLine == 0 && e.prevH == 0 && (e.prevV & 0x80),
                  "//e PAL: the read before must be the first display cycle (line 0 cycle 0, $80)");
        }
    }
    const std::string iic = firstExisting("roms/apple2c-32Kv0.rom");
    if (iic.empty()) {
        std::printf("SKIP ft_loader_sync: roms/apple2c-32Kv0.rom not found\n");
    } else {
        ++ran;
        const Exit c = runSync(iic, true);
        CHECK(c.ok, "//c PAL: the sync loop never exited");
        if (c.ok) {
            std::printf("  //c PAL: exit read line %d cycle %d ($%02X), before it line %d cycle %d ($%02X), %zu precise steps\n",
                        c.line, c.h, c.v, c.prevLine, c.prevH, c.prevV, c.precise);
            CHECK(c.precise >= 1, "//c PAL: the precise loop never iterated (transcription?)");
            CHECK(c.line == 191 && c.h == 64 && (c.v & 0x80) == 0,
                  "//c PAL: the exiting $C019 read must be the last display cycle (line 191 cycle 64), got line %d cycle %d", c.line, c.h);
            CHECK(c.prevLine == 192 && c.prevH == 0 && (c.prevV & 0x80),
                  "//c PAL: VBLINT must read set from line 192 cycle 0, the //e edge + 192 lines");
        }
    }
    if (ran == 0) return 77;
    if (failures) { std::printf("ft_loader_sync FAILED (%d)\n", failures); return 1; }
    std::printf("ft_loader_sync OK\n");
    return 0;
}
