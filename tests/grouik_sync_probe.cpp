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

// grouik_sync_probe — French Touch's own silicon measurements, replayed.
//
// DIX's `loader.a` (fenarinarsa) and `CCII_2016/Sources/part1.a` (Grouik)
// write down, in their comments, what the REAL machine does at three sync
// points ("IRL"). This probe boots DIX on a //e PAL + Mockingboard (slot 2),
// selects Crazy Cycles II and reports, in POM2's beam coordinates (line
// 0..311, cycle 0..64 with the first visible byte at 25), what POM2 does at
// the same points:
//
//  1. TIMERS_SYNC's precise $C019 loop: "precise sync here / IIe => START OF
//     DISPLAY + 5 cycles" — the beam position of the read that ends it.
//  2. TIMER_HANDLER: "IRL: $0E <= T1CL <= $15 / AppleWin: $0F..$16" and
//     TIMER2_HANDLER "IRL: $D4 <= TC2L <= $DB / AppleWin: $D8..$DF" — the
//     T1CL / T2CL values the handlers read.
//  3. "IRQ will execute at line 192 (first line of VBLANK), cycle 25" — the
//     beam position of the handler's first opcode fetch, per timer.
//  4. Part I's floating-bus lock: `LDA $C050` "lecture pos $02 / $14 / $26"
//     of the last text line — the beam position of the three reads that
//     matched, and of the next $C050 read.
//
// Usage: grouik_sync_probe [--nmos | --iic] [--seconds N] [--disk DIX.po]
// --nmos: unenhanced //e ROM + NMOS 6502 (default: enhanced, 65C02).
// --iic:  //c PAL + Mockingboard 4c (65C02). loader.a syncs a //c on its
//         VBL INTERRUPT ($C05B / $C019 = VBLINT / $C070), which rises at the
//         END of the display, where a //e syncs on the end of VBL; both must
//         reach "END OF DISPLAY + 11" and launch T1 on the same cycle.

#include "Apple2Display.h"
#include "DiskIICard.h"
#include "IIcExternalSmartPort.h"
#include "IIcMouse.h"
#include "IWMDevice.h"
#include "CpuClock.h"
#include "M6502.h"
#include "Memory.h"
#include "MemoryWatchSink.h"
#include "Mockingboard.h"
#include "ResourcePaths.h"
#include "SlotBus.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "SuperSerialCard.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kLine = 65, kFrame = 312 * 65;

struct Pos { int line, h; };
Pos pos(uint64_t c) { const uint64_t f = c % kFrame; return {int(f / kLine), int(f % kLine)}; }

struct Sink : pom2::MemoryWatchSink {
    Memory* mem = nullptr;
    M6502* cpu = nullptr;
    uint16_t handler = 0;       // TIMER_HANDLER, read off $FFFE once set
    uint16_t t1Addr = 0xC204, t2Addr = 0xC208;   // the MB's T1CL / T2CL (slot 2; //c: $C4xx)
    // 1
    std::vector<std::pair<uint64_t, uint8_t>> c019;
    // 2
    std::map<uint8_t, int> t1cl, t2cl;
    uint16_t lastTimerRead = 0;
    // 3: entry position per timer, keyed by the IFR bit the handler reads
    std::map<std::pair<int,int>, int> entryT1, entryT2;
    uint64_t lastEntry = 0;
    bool entryPending = false;
    Pos entryPos{};
    // 4
    std::vector<std::pair<uint64_t, uint8_t>> fumes;
    // 6: the instruction right after each dejitter loop (loader.a .T1sync /
    //    .T2sync): "perfect sync with display achieved here".
    uint16_t afterT1 = 0, afterT2 = 0;
    std::map<std::pair<int,int>, int> landT1, landT2;
    // 5: soft-switch writes, histogram of the beam cycle within the line
    std::map<uint16_t, std::map<int, int>> writes;
    std::map<uint16_t, std::map<int, int>> switchReads;   // display-time reads of $C050-$C05F
    std::map<int, int> pageReads;   // $C054/$C055 READS (LDA (CELLn),Y), cycle within line
    std::vector<std::pair<uint64_t, bool>> flips;   // (cycle, true = PAGE2)

    void noteAccess(uint16_t addr, uint8_t v, bool write) override {
        if (write) { ++writes[addr][pos(mem->videoAccessCycle()).h]; return; }
        if (addr >= 0xC050 && addr <= 0xC05F && addr != 0xC054 && addr != 0xC055 && pos(mem->videoAccessCycle()).line < 192)
            ++switchReads[addr][pos(mem->videoAccessCycle()).h];
        const uint64_t c = mem->videoAccessCycle();
        if (addr == 0xC019) { if (c019.size() < 200000) c019.push_back({c, v}); return; }
        if (addr == 0xC050) { if (fumes.size() < 400000) fumes.push_back({c, v}); return; }
        if (addr == 0xC054 || addr == 0xC055) {
            const Pos p = pos(c);
            ++pageReads[p.h];
            flips.push_back({c, addr == 0xC055});
            if (flips.size() > 2000000) flips.erase(flips.begin(), flips.begin() + 1000000);
            return;
        }
        if (addr == t1Addr) {           // T1CL
            if (entryPending) { ++t1cl[v]; ++entryT1[{entryPos.line, entryPos.h}]; entryPending = false; }
            return;
        }
        if (addr == t2Addr) {           // T2CL
            if (entryPending) { ++t2cl[v]; ++entryT2[{entryPos.line, entryPos.h}]; entryPending = false; }
            return;
        }
        if (afterT1 && addr == afterT1) { const Pos p = pos(c); ++landT1[{p.line, p.h}]; return; }
        if (afterT2 && addr == afterT2) { const Pos p = pos(c); ++landT2[{p.line, p.h}]; return; }
        if (handler && addr == handler) {   // opcode fetch at TIMER_HANDLER
            entryPending = true;
            entryPos = pos(c);
        }
    }
};

void hist(const char* title, const std::map<uint8_t, int>& h)
{
    std::printf("%s", title);
    for (const auto& [v, n] : h) std::printf(" $%02X:%d", v, n);
    std::printf("\n");
}

void histPos(const char* title, const std::map<std::pair<int,int>, int>& h)
{
    std::printf("%s\n", title);
    int shown = 0;
    for (const auto& [p, n] : h) {
        std::printf("   line %3d cycle %2d : %d\n", p.first, p.second, n);
        if (++shown == 24) { std::printf("   ...\n"); break; }
    }
}

} // namespace

int main(int argc, char** argv)
{
    bool nmos = false, iic = false;
    int choice = 7;   // DIX menu: 7 = Crazy Cycles II, 9 = Mad Effect, 14 = OLDSKOOL
    double seconds = 75;
    std::string diskArg, pngArg;
    std::vector<double> shots;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--nmos") nmos = true;
        else if (a == "--iic") iic = true;
        else if (a == "--choice" && i + 1 < argc) choice = std::atoi(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--disk" && i + 1 < argc) diskArg = argv[++i];
        else if (a == "--png" && i + 1 < argc) pngArg = argv[++i];        // path prefix
        else if (a == "--shot" && i + 1 < argc) shots.push_back(std::atof(argv[++i]));
    }
    const std::string rom = pom2::findResource(
        iic ? "roms/apple2c-32Kv0.rom" : nmos ? "roms/apple2e_unenh.rom" : "roms/apple2e.rom");
    std::string disk = diskArg.empty() ? pom2::findResource("disks_3.5/DIX.po") : diskArg;
    if (disk.empty()) disk = pom2::findResource("disks_3.5/DIX-fix.po");
    if (rom.empty() || disk.empty()) { std::printf("SKIP: missing ROM or DIX\n"); return 77; }
    const double hz = POM2_TIMING_PAL.cpuClockHz;

    Memory mem;
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    mem.setVideoStandard(VideoStandard::PAL);
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    // The 32 KB //c's own $C500 firmware talks to the rear-port 3.5" over the
    // SmartPort bus through the machine's IWM: wire both as
    // iic_external_smartport does (its units are the slot-5 card's).
    pom2::IWMDevice iwm;
    pom2::IIcExternalSmartPort extPort(&mem.slotBus());
    if (iic) {
        mem.setIWM(&iwm);
        mem.setIWMAuthoritative(true);
        mem.setExternalSmartPort(&extPort);
    }
    if (!mem.loadAppleIIRom(rom.c_str(), iic)) return 1;
    auto unit = std::make_unique<pom2::SmartPort35Unit>();
    if (!unit->loadImage(disk)) { std::printf("FAIL: %s\n", unit->lastError().c_str()); return 1; }
    auto sp = std::make_unique<pom2::SmartPortCard>(5);
    sp->setUnit(0, std::move(unit));
    if (iic) {
        // The //c's built-ins (CLAUDE.md § System profiles) and the
        // Mockingboard 4c in the CPU socket: virtual slot 3, dormant at
        // $C400 until DIX's boot writes $C403 (boot_unidisk.a).
        for (int s = 1; s <= 2; ++s)
            mem.slotBus().plug(s, std::make_unique<SuperSerialCard>(s));
        mem.slotBus().plug(4, std::make_unique<IIcMouse>());
        auto d2 = std::make_unique<DiskIICard>(6);
        d2->setIWM(&iwm);
        mem.slotBus().plug(6, std::move(d2));
        auto mb = std::make_unique<MockingboardCard>(3);
        mb->setCpu(&cpu); mb->setCpuClock(hz); mb->setStandardClock(hz);
        mem.slotBus().plug(3, std::move(mb));
        mem.slotBus().plug(5, std::move(sp));
    } else {
        auto mb = std::make_unique<MockingboardCard>(2, MockingboardCard::Variant::AC);
        mb->setCpu(&cpu); mb->setCpuClock(hz); mb->setStandardClock(hz);
        mem.slotBus().plug(2, std::move(mb));
        mem.slotBus().plug(5, std::move(sp));
    }
    cpu.setCpuMode(nmos ? M6502::CpuMode::NMOS : M6502::CpuMode::CMOS);
    cpu.hardReset();
    mem.slotBus().reset();
    if (iic) {
        // Boot slot 5 as POM2's bootFromSlot does (the nox_mb4c probe's
        // shape): the Monitor's text/keyboard setup, then $C500.
        for (int a = 0x400; a < 0x800; ++a) mem.memWrite(uint16_t(a), 0xA0);
        const uint8_t zp[][2] = {{0x21,40}, {0x23,24}, {0x29,4}, {0x32,255},
                                {0x36,0xF0}, {0x37,0xFD}, {0x38,0x1B}, {0x39,0xFD}};
        for (const auto& p : zp) mem.memWrite(p[0], p[1]);
        // Armed AFTER the resets, which clear it (Core::bootSlot's order).
        mem.setIicSmartPortArmed(true);
        cpu.setProgramCounter(0xC500);
    }

    Sink sink;
    sink.mem = &mem; sink.cpu = &cpu;
    if (iic) { sink.t1Addr = 0xC404; sink.t2Addr = 0xC408; }
    mem.setWatchSink(&sink);
    for (uint16_t a : {uint16_t(0xC019), uint16_t(0xC050), uint16_t(0xC054), uint16_t(0xC055),
                       sink.t1Addr, sink.t2Addr}) mem.setReadWatch(a, true);
    for (uint16_t a : {0xC051, 0xC052, 0xC053, 0xC056, 0xC057, 0xC05E, 0xC05F}) mem.setReadWatch(a, true);
    const uint16_t kSwitches[] = {0xC054, 0xC055, 0xC056, 0xC057, 0xC05E, 0xC05F, 0xC00C, 0xC00D};
    for (uint16_t a : kSwitches) mem.setWriteWatch(a, true);

    uint64_t cycles = 0;
    auto run = [&](double s) {
        const uint64_t end = cycles + uint64_t(s * hz);
        while (cycles < end)
            cycles += uint64_t(cpu.run(static_cast<int>(std::min<uint64_t>(4096, end - cycles))));
    };
    // The unenhanced //e's autostart compares $Cn07 too ([C] its F8 ROM at
    // $FAB9: LDY #$07, against the enhanced ROM's LDY #$05) and so boots a
    // Disk II only; on the real machine one types PR#5 at the prompt.
    if (nmos) { run(3); mem.pasteText("PR#5\r", 5); }
    run(25);   // the menu is up at ~17 s
    // The vector the loader installed (LC bank 2 RAM, `+set_ptr TIMER_HANDLER,$FFFE`).
    sink.handler = uint16_t(mem.peekCpuView(0xFFFE) | (mem.peekCpuView(0xFFFF) << 8));
    mem.setReadWatch(sink.handler, true);
    // The two dejitter loops are 10 x (DEX / BPL +) = CA 10 00, followed by
    // LDY #$08 (A0 08) after .T1sync and LDA T2SyncCodePTR+1 (A5 FF) after
    // .T2sync. Find them in the CPU's view (the handler lives in LC RAM).
    for (uint32_t a = 0xD000; a + 32 < 0x10000; ++a) {
        bool loop = true;
        for (int k = 0; k < 10 && loop; ++k)
            loop = mem.peekCpuView(uint16_t(a + 3 * k)) == 0xCA &&
                   mem.peekCpuView(uint16_t(a + 3 * k + 1)) == 0x10 &&
                   mem.peekCpuView(uint16_t(a + 3 * k + 2)) == 0x00;
        if (!loop) continue;
        const uint16_t after = uint16_t(a + 30);
        const uint8_t o0 = mem.peekCpuView(after), o1 = mem.peekCpuView(uint16_t(after + 1));
        if (o0 == 0xA0 && o1 == 0x08) sink.afterT1 = after;
        if (o0 == 0xA5 && o1 == 0xFF) sink.afterT2 = after;
    }
    if (sink.afterT1) mem.setReadWatch(sink.afterT1, true);
    if (sink.afterT2) mem.setReadWatch(sink.afterT2, true);
    std::printf("after .T1sync at $%04X, after .T2sync at $%04X\n", sink.afterT1, sink.afterT2);
    std::printf("%s, TIMER_HANDLER at $%04X, menu choice $%02X\n",
                iic ? "65C02 (//c PAL + Mockingboard 4c)"
                    : nmos ? "NMOS 6502 (unenhanced //e ROM)" : "65C02 (enhanced //e ROM)",
                sink.handler, mem.peekCpuView(0xDFFF));

    // 1. TIMERS_SYNC ran before the menu: the precise loop's reads are the
    //    run of $C019 reads spaced 20279 cycles apart; the last one ends it.
    {
        std::size_t end = 0;
        for (std::size_t i = 1; i < sink.c019.size(); ++i)
            if (sink.c019[i].first - sink.c019[i - 1].first == 20279) end = i;
        if (end) {
            const auto [c, v] = sink.c019[end];
            const Pos p = pos(c);
            std::size_t start = end;
            while (start > 0 && sink.c019[start].first - sink.c019[start - 1].first == 20279) --start;
            std::printf("\n1. TIMERS_SYNC precise loop: %zu iterations; the exiting $C019 read\n"
                        "   is at line %d cycle %d (value $%02X); the one before at line %d cycle %d ($%02X).\n"
                        "   Exit point (BMI not taken, +3) = line %d cycle %d.\n"
                        "   FT: \"IIe => START OF DISPLAY + 5 cycles\", \"IIc => END OF DISPLAY + 5\".\n",
                        end - start, p.line, p.h, v,
                        pos(sink.c019[end - 1].first).line, pos(sink.c019[end - 1].first).h,
                        sink.c019[end - 1].second, pos(c + 3).line, pos(c + 3).h);
        } else {
            std::printf("\n1. TIMERS_SYNC precise loop: not found in %zu $C019 reads\n", sink.c019.size());
        }
    }
    sink.c019.clear();

    // Select the part: RIGHT x choice then RETURN (MENU/main.a: 21 = next).
    for (int i = 0; i < choice; ++i) { mem.pasteRawKeys("\x15", 1); run(0.3); }
    std::printf("menu choice now $%02X\n", mem.peekCpuView(0xDFFF));
    sink.t1cl.clear(); sink.t2cl.clear(); sink.entryT1.clear(); sink.entryT2.clear();
    sink.landT1.clear(); sink.landT2.clear();
    sink.fumes.clear(); sink.writes.clear(); sink.pageReads.clear(); sink.switchReads.clear();
    mem.pasteRawKeys("\r", 1);
    {
        Apple2Display display;
        display.setAuxMemory(mem.auxData());
        double done = 0;
        std::sort(shots.begin(), shots.end());
        for (double t : shots) {
            if (t > done) { run(t - done); done = t; }
            display.render(mem);
            const std::string path = pngArg + "_" + std::to_string(int(t * 10)) + ".png";
            stbi_write_png(path.c_str(), display.width(), display.height(), 4,
                           display.pixels(), display.width() * 4);
            std::printf("frame at +%.1f s -> %s\n", t, path.c_str());
        }
        if (seconds > done) run(seconds - done);
    }

    hist("\n2. T1CL read in TIMER_HANDLER (IRL $0E..$15, AppleWin $0F..$16):", sink.t1cl);
    hist("   T2CL read in TIMER2_HANDLER (IRL $D4..$DB, AppleWin $D8..$DF):", sink.t2cl);
    histPos("\n3. TIMER_HANDLER first fetch, T1 interrupts (FT: \"line 192, cycle 25\" + jitter):", sink.entryT1);
    histPos("   TIMER_HANDLER first fetch, T2 interrupts:", sink.entryT2);

    // 6. Where the dejitter loops land. FT's line coordinates (MADEF2
    //    main.a: "[HBL-L = 0-11 / DISPLAY = 12-51 / HBL-R = 52-64]") put
    //    their cycle 0 at POM2's cycle 13. The synced code is called 25
    //    cycles after .T2sync (LDA zp, BEQ, LDA zp, STA abs, LDA zp, STA abs,
    //    JSR) and must start at "the last line of VBLANK, cycle 0" with the
    //    IRL-tuned DEFAULT_SYNC_TIMER = 7479 (loader.a) — POM2 line 311,
    //    cycle 13. The parts may move it by n*65 (T2Timing).
    std::printf("\nT2Timing ($F6/$F7) now = %u (loader DEFAULT_SYNC_TIMER = 7479, IRL PAL)\n",
                unsigned(mem.peekCpuView(0xF6) | (mem.peekCpuView(0xF7) << 8)));
    histPos("\n6. After .T1sync (\"perfect sync with display achieved here\"):", sink.landT1);
    histPos("   After .T2sync:", sink.landT2);
    {
        int total = 0; std::map<int,int> ftCycle;
        for (const auto& [p, n] : sink.landT2) {
            const int abs = (p.first * 65 + p.second + 25) % int(kFrame);
            ftCycle[((abs - 13) % 65 + 65) % 65] += n; total += n;
        }
        std::printf("   => synced code entry, FT cycle within its line (expect 0):");
        for (const auto& [c, n] : ftCycle) std::printf(" %d:%d", c, n);
        std::printf("\n");
    }

    // 4. Part I's lock: three $C050 reads 18 cycles apart returning $0C,$02,$0C.
    {
        int locks = 0;
        for (std::size_t i = 2; i + 1 < sink.fumes.size() && locks < 3; ++i) {
            const auto& a = sink.fumes[i - 2]; const auto& b = sink.fumes[i - 1]; const auto& c = sink.fumes[i];
            if (a.second == 0x0C && b.second == 0x02 && c.second == 0x0C &&
                b.first - a.first == 18 && c.first - b.first == 18) {
                const Pos pa = pos(a.first), pb = pos(b.first), pc = pos(c.first), pn = pos(sink.fumes[i + 1].first);
                std::printf("%s lock at %.2f s: reads at line %d cycle %d / %d / %d (FT: byte $02/$14/$26 = cycles 27/45/63);"
                            " next $C050 read line %d cycle %d ($%02X)\n",
                            locks ? "  " : "\n4. Part I floating-bus", a.first / hz, pa.line, pa.h, pb.h, pc.h,
                            pn.line, pn.h, sink.fumes[i + 1].second);
                ++locks;
            }
        }
        if (!locks) std::printf("\n4. Part I floating-bus lock: not seen in %zu $C050 reads\n", sink.fumes.size());
    }
    // 5. Where the soft-switch WRITES land in the line (cycle 0..64; the
    //    first visible byte is fetched at 25, so a write at <= 24 changes a
    //    whole line and one at >= 25 shows as an edge inside it).
    std::printf("\n5. Soft-switch writes by cycle within the line (STA = write on the 4th cycle):\n");
    for (const auto& [addr, h] : sink.writes) {
        int total = 0, visible = 0;
        for (const auto& [c, n] : h) { total += n; if (c >= 25) visible += n; }
        std::printf("   $%04X: %6d writes, %5.1f%% at cycle >= 25 |", addr, total,
                    total ? 100.0 * visible / total : 0.0);
        int shown = 0;
        std::vector<std::pair<int,int>> top(h.begin(), h.end());
        std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
        for (const auto& [c, n] : top) { std::printf(" %d:%d", c, n); if (++shown == 10) break; }
        std::printf("\n");
    }
    std::printf("   display-time READS of $C050-$C05F (not $C054/5):\n");
    for (const auto& [addr, h] : sink.switchReads) {
        int total = 0; for (const auto& [c, n] : h) total += n;
        std::vector<std::pair<int,int>> top(h.begin(), h.end());
        std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
        std::printf("   $%04X: %7d reads |", addr, total);
        int shown = 0; for (const auto& [c, n] : top) { std::printf(" %d:%d", c, n); if (++shown == 10) break; }
        std::printf("\n");
    }

    // 7. Part I's cells: 8 page flips 5 cycles apart per displayed line.
    //    Sharp cells need every flip on a 5-byte boundary: POM2 maps a PAGE2
    //    access at cycle h to column h-24 (fetch side), so h = 24,29,..,59.
    std::printf("\n7. $C054/$C055 READS by cycle within the line (sharp cells: h = 24+5k):\n  ");
    {
        std::vector<std::pair<int,int>> top(sink.pageReads.begin(), sink.pageReads.end());
        std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
        int shown = 0;
        for (const auto& [c, n] : top) { std::printf(" %d:%d", c, n); if (++shown == 16) break; }
        std::printf("\n");
        // Grid phase: the column each flip takes effect at under POM2's
        // PAGE2 mapping (byteCol = h - 25, Apple2Display_Beam.cpp), mod 5.
        // Grouik's 8 cells x 5 bytes fill 40 columns: phase 0 is sharp.
        std::map<int, int> phase;
        for (const auto& [c, n] : sink.pageReads)
            if (c >= 25 && c < 65 && n > 1000)
                phase[(c - 25) % 5] += n;
        std::printf("   cell-grid phase (column mod 5 of every flip, expect all at 0):");
        for (const auto& [ph, n] : phase) std::printf(" %d:%d", ph, n);
        std::printf("\n");
    }
    // 8. Window symmetry (MAD EFFECT: each displayed line opens page 1 with
    //    $C054 and closes it with $C055, Table1[i] + Table2[i] = 45, so the
    //    silhouette is symmetric — its axis must be the screen's centre,
    //    column 20 when columns run 0..39 and a window [a, b) is centred on
    //    (a + b) / 2). Columns under POM2's PAGE2 mapping (h - 24).
    {
        std::map<int, int> twiceCentre;
        for (std::size_t i = 1; i < sink.flips.size(); ++i) {
            const auto& o = sink.flips[i - 1]; const auto& c = sink.flips[i];
            if (o.second || !c.second) continue;              // $C054 then $C055
            const Pos po = pos(o.first), pc = pos(c.first);
            if (po.line != pc.line || po.line >= 192) continue;
            const int a = po.h - kVideoAccessColumnOffset, b = pc.h - kVideoAccessColumnOffset;
            if (a < 0 || b > 40 || b <= a) continue;
            ++twiceCentre[a + b];
        }
        // The last complete frame, line by line (first 12 displayed lines).
        if (!sink.flips.empty()) {
            std::map<uint64_t, int> page2PerFrame;
            for (const auto& f : sink.flips)
                if (f.second && pos(f.first).line < 192) ++page2PerFrame[f.first / kFrame];
            uint64_t lastFrame = sink.flips.back().first / kFrame - 1; int best = -1;
            for (const auto& [fr, n] : page2PerFrame) if (n > best) { best = n; lastFrame = fr; }
            int printed = -1;
            std::printf("\n   frame %llu, flips per line (h:page):", (unsigned long long)lastFrame);
            for (const auto& f : sink.flips) {
                if (f.first / kFrame != lastFrame) continue;
                const Pos p = pos(f.first);
                if (p.line > 11) continue;
                if (p.line != printed) { std::printf("\n     line %3d:", p.line); printed = p.line; }
                std::printf(" %d:%c", p.h, f.second ? '2' : '1');
            }
            std::printf("\n");
        }
        // MAD EFFECT's silhouette edge: per displayed line, a $C054 in HBL
        // (h < 25) followed by ONE $C055 later on the same line — the $C055's
        // beam cycle is the edge. All of them must land in the visible
        // window: the feasible PAGE2 offsets k are those with
        // min(h) - k >= 0 and max(h) - k <= 39.
        {
            std::map<int, long> edgeH;
            for (std::size_t i = 1; i < sink.flips.size(); ++i) {
                const auto& o = sink.flips[i - 1]; const auto& c = sink.flips[i];
                if (o.second || !c.second) continue;
                const Pos po = pos(o.first), pc = pos(c.first);
                if (po.line != pc.line || po.line >= 192 || po.h >= 25 || pc.h < 25) continue;
                ++edgeH[pc.h];
            }
            if (!edgeH.empty()) {
                const int lo = edgeH.begin()->first, hi = edgeH.rbegin()->first;
                long total = 0; for (auto& [h, n] : edgeH) total += n;
                std::printf("\n   silhouette edges ($C055 after an HBL $C054): %ld, beam cycle %d..%d"
                            " -> feasible PAGE2 offsets k in [%d, %d] (POM2 uses 25)\n",
                            total, lo, hi, hi - 39, lo);
                std::printf("   edge histogram:");
                for (auto& [h, n] : edgeH) std::printf(" %d:%ld", h, n);
                std::printf("\n");
            }
        }
        std::printf("\n8. page-1 windows per line: 2 x centre column (expect 40 = centred):");
        int shown = 0;
        std::vector<std::pair<int,int>> top(twiceCentre.begin(), twiceCentre.end());
        std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
        for (const auto& [v, n] : top) { std::printf(" %d:%d", v, n); if (++shown == 8) break; }
        std::printf("\n");
    }
    std::printf("\nend %.1f s, PC=$%04X\n", cycles / hz, cpu.getProgramCounter());
    return 0;
}
