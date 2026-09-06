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

// DIGIDREAM 2 (GROUiK / French Touch, 2020) Mockingboard AY trace — diagnostic,
// NOT a pinned test (needs disks_5.4/demo/digidream/DD2.woz + roms/).
//
// Why
// ---
// DD2's music is a PYM ("Pattern YM") replay of an Atari ST YM2149 tune with
// digidrums (Sources/main.a, shipped GPLv3 with the demo). It is a *buzzer*
// tune: channel A has tone AND noise masked off and R8 = $10, so the ONLY
// sound source is the hardware envelope generator run at note frequency, and
// R13 is re-stored on every tracker row to restart it. That is precisely the
// AY feature a point-sampled renderer degrades most, so before touching the
// audio path we want the register traffic measured, not guessed.
//
// What it does
// ------------
// Boots DD2.woz on //e Enhanced PAL (128 K) with a Disk II in slot 6 and a
// MockingboardCard in slot 4, then single-steps the CPU and decodes the AY
// control bus by polling the two VIAs' Port A / Port B through the card's
// side-effect-free `peekViaRegister` test hook:
//
//   ORB (PB0=BC1, PB1=BDIR, PB2=/RESET) transition to %111 ($07) → LATCH addr
//   from Port A; transition to %110 ($06) → WRITE Port A into the latched
//   register. This is exactly `Ay3_8910::applyControl`'s decode, replayed on
//   the observer side so the trace carries a CPU-cycle stamp per write
//   without reaching into the card's private emuCycles event queue.
//
// Output: per-register histograms, R13/R11 (envelope) statistics, mixer and
// amplitude statistics, and the measured replay-tick + digidrum-IRQ rates.
//
// Usage: build/tests/dd2_ay_trace [seconds] [disk.woz]

#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "Mockingboard.h"
#include "CpuClock.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string findFirst(std::initializer_list<const char*> cands)
{
    std::error_code ec;
    for (const char* c : cands)
        if (std::filesystem::is_regular_file(c, ec)) return c;
    return {};
}

struct AyWrite { uint64_t cycle; uint8_t chip; uint8_t reg; uint8_t val; };

}  // namespace

int main(int argc, char** argv)
{
    const double  secs = (argc > 1) ? std::atof(argv[1]) : 30.0;
    const std::string rom  = findFirst({"../roms/apple2e.rom", "roms/apple2e.rom"});
    const std::string boot = findFirst({"../roms/disk2.rom",   "roms/disk2.rom"});
    const std::string p6   = findFirst({"../roms/diskii_p6.rom", "roms/diskii_p6.rom"});
    const std::string dsk  = (argc > 2) ? std::string(argv[2]) : findFirst({
        "../disks_5.4/demo/digidream/DD2.woz",
        "disks_5.4/demo/digidream/DD2.woz"});
    if (rom.empty() || boot.empty() || dsk.empty()) {
        std::printf("dd2_ay_trace SKIP: missing apple2e.rom / disk2.rom / DD2.woz\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    Memory mem;
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    mem.setVideoStandard(VideoStandard::PAL);
    if (!mem.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/false)) {
        std::fprintf(stderr, "loadAppleIIRom failed\n"); return 1;
    }

    auto disk = std::make_unique<DiskIICard>();
    if (!disk->loadBootRom(boot) || !disk->insertDisk(dsk)) {
        std::fprintf(stderr, "Disk II setup failed\n"); return 1;
    }
    if (!p6.empty()) disk->loadLssRom(p6);
    mem.slotBus().plug(6, std::move(disk));

    auto mbOwned = std::make_unique<MockingboardCard>(4);
    MockingboardCard* mb = mbOwned.get();
    mem.slotBus().plug(4, std::move(mbOwned));

    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    cpu.hardReset();
    mem.slotBus().reset();

    const double cpuHz = pom2VideoTiming(VideoStandard::PAL).cpuClockHz;
    const uint64_t stopCycle = static_cast<uint64_t>(secs * cpuHz);

    std::vector<AyWrite> log;
    log.reserve(2u << 20);
    uint8_t prevPb[2] = {0xFF, 0xFF};
    uint8_t latch[2]  = {0, 0};

    uint64_t nextViaDump = static_cast<uint64_t>(5.0 * cpuHz);
    while (cpu.getCycleCountNow() < stopCycle) {
        cpu.step();
        if (cpu.getCycleCountNow() >= nextViaDump) {
            nextViaDump += static_cast<uint64_t>(5.0 * cpuHz);
            for (int ci = 0; ci < 2; ++ci) {
                if (ci == 1 && mb->getViaWriteCount(1) == 0) continue;
                std::printf("  t=%5.0fs VIA%d ACR=$%02X IER=$%02X "
                            "T1latch=%u T2=%u\n",
                            cpu.getCycleCountNow() / cpuHz, ci + 1,
                            mb->peekViaRegister(ci, 11), mb->peekViaRegister(ci, 14),
                            mb->peekViaRegister(ci, 6) |
                                (mb->peekViaRegister(ci, 7) << 8),
                            mb->peekViaRegister(ci, 8) |
                                (mb->peekViaRegister(ci, 9) << 8));
            }
        }
        for (int ci = 0; ci < 2; ++ci) {
            const uint8_t pb = static_cast<uint8_t>(mb->peekViaRegister(ci, 0) & 0x07);
            if (pb == prevPb[ci]) continue;
            prevPb[ci] = pb;
            if (pb == 0x07) {                       // LATCH ADDR
                latch[ci] = static_cast<uint8_t>(mb->peekViaRegister(ci, 1) & 0x0F);
            } else if (pb == 0x06) {                // WRITE
                log.push_back({cpu.getCycleCountNow(), static_cast<uint8_t>(ci),
                               latch[ci], mb->peekViaRegister(ci, 1)});
            }
        }
    }

    std::printf("DD2 AY trace: %.1f emulated s on //e PAL (%.0f Hz CPU), PC=$%04X\n",
                secs, cpuHz, cpu.getProgramCounter());
    std::printf("AY writes seen: %zu  (card AY counters: chip0=%u chip1=%u;"
                " VIA MMIO writes: via0=%u via1=%u)\n",
                log.size(), mb->getAyWriteCount(0), mb->getAyWriteCount(1),
                mb->getViaWriteCount(0), mb->getViaWriteCount(1));
    if (log.empty()) {
        std::printf("no AY traffic — did the demo boot? text row0 follows:\n");
        for (int c = 0; c < 40; ++c)
            std::putchar(static_cast<char>(mem.memRead(0x400 + c) & 0x7F));
        std::putchar('\n');
        return 0;
    }

    // Optional raw dump: argv[3] = CSV path (cycle,chip,reg,val).
    if (argc > 3) {
        if (FILE* f = std::fopen(argv[3], "w")) {
            for (const auto& w : log)
                std::fprintf(f, "%llu,%u,%u,%u\n", (unsigned long long)w.cycle,
                             w.chip, w.reg, w.val);
            std::fclose(f);
            std::printf("raw writes dumped to %s\n", argv[3]);
        }
    }

    // ── per-register histogram, per chip ────────────────────────────────
    uint32_t hist[2][16] = {};
    for (const auto& w : log) hist[w.chip][w.reg]++;
    std::printf("\nregister write histogram\n  reg   AY1($C400)   AY2($C480)\n");
    for (int r = 0; r < 16; ++r)
        if (hist[0][r] || hist[1][r])
            std::printf("  R%-2d %10u %12u\n", r, hist[0][r], hist[1][r]);

    // ── envelope: R13 shapes, R11/R12 periods, R8/9/10 env bit ──────────
    std::map<int, uint32_t> shapes, envPer, mixer, noisePer;
    std::map<int, uint32_t> amp[3];
    uint32_t envBitSet[3] = {}, envBitTot[3] = {};
    int lastEnvBit[3] = {-1, -1, -1};
    uint32_t envBitToggle[3] = {};
    std::vector<uint64_t> r13Cycles;
    uint8_t regs[16] = {};
    int minTone[3] = {4096, 4096, 4096}, maxTone[3] = {0, 0, 0};
    for (const auto& w : log) {
        if (w.chip != 0) continue;                  // AY1 = the reference stream
        regs[w.reg] = w.val;
        switch (w.reg) {
        case 13: shapes[w.val & 0x0F]++; r13Cycles.push_back(w.cycle); break;
        case 11: envPer[regs[11] | (regs[12] << 8)]++; break;
        case  6: noisePer[w.val & 0x1F]++; break;
        case  7: mixer[w.val]++; break;
        case  1: case 3: case 5: {
            const int ch = (w.reg - 1) / 2;
            const int tp = regs[ch * 2] | ((regs[ch * 2 + 1] & 0x0F) << 8);
            if (tp) { minTone[ch] = std::min(minTone[ch], tp);
                      maxTone[ch] = std::max(maxTone[ch], tp); }
            break; }
        case 8: case 9: case 10: {
            const int ch = w.reg - 8;
            amp[ch][w.val]++;
            const int b = (w.val >> 4) & 1;
            envBitTot[ch]++; if (b) envBitSet[ch]++;
            if (lastEnvBit[ch] >= 0 && b != lastEnvBit[ch]) envBitToggle[ch]++;
            lastEnvBit[ch] = b;
            break; }
        default: break;
        }
    }
    std::printf("\nR13 (envelope shape) writes on AY1: %zu\n", r13Cycles.size());
    for (auto& [s, c] : shapes)
        std::printf("   shape $%X : %u\n", s, c);
    std::printf("R11/R12 envelope period (full-cycle freq = clock/(256*EP)):\n");
    int minEp = 1 << 20;
    for (auto& [e, c] : envPer) {
        if (e) minEp = std::min(minEp, e);
        std::printf("   EP=%-5d %6u writes   %8.1f Hz\n", e, c,
                    e ? cpuHz / (256.0 * e) : 0.0);
    }
    std::printf("   min non-zero EP = %d -> %.1f Hz\n", minEp,
                minEp < (1 << 20) ? cpuHz / (256.0 * minEp) : 0.0);
    for (int ch = 0; ch < 3; ++ch) {
        std::printf("R%d (ch%c amp): %u writes, env-bit set %u (%.1f%%), toggles %u\n",
                    8 + ch, 'A' + ch, envBitTot[ch], envBitSet[ch],
                    envBitTot[ch] ? 100.0 * envBitSet[ch] / envBitTot[ch] : 0.0,
                    envBitToggle[ch]);
    }
    std::printf("\ntone periods (freq = clock/(16*TP)):\n");
    for (int ch = 0; ch < 3; ++ch)
        if (maxTone[ch])
            std::printf("   ch%c TP %d..%d -> %.1f..%.1f Hz\n", 'A' + ch,
                        minTone[ch], maxTone[ch],
                        cpuHz / (16.0 * maxTone[ch]), cpuHz / (16.0 * minTone[ch]));
    std::printf("\nnoise R6 periods: ");
    for (auto& [n, c] : noisePer) std::printf("NP=%d(%u) ", n, c);
    std::printf("\nmixer R7 values: ");
    for (auto& [m, c] : mixer) std::printf("$%02X(%u) ", m, c);
    std::putchar('\n');

    // ── per-window summary (the demo has several parts / tunes) ─────────
    {
        const uint64_t win = static_cast<uint64_t>(10.0 * cpuHz);   // 10 s
        std::printf("\nper-10s window   AY1w   AY2w   R13   envbitA/B/C   EPmin..EPmax\n");
        size_t i = 0;
        for (uint64_t w0 = 0; w0 < stopCycle; w0 += win) {
            uint32_t c0 = 0, c1 = 0, r13 = 0, eb[3] = {}, tot[3] = {};
            int epLo = 1 << 20, epHi = 0;
            uint8_t rr[16] = {};
            for (; i < log.size() && log[i].cycle < w0 + win; ++i) {
                const auto& w = log[i];
                (w.chip ? c1 : c0)++;
                if (w.chip) continue;
                rr[w.reg] = w.val;
                if (w.reg == 13) r13++;
                if (w.reg == 11) {
                    const int e = rr[11] | (rr[12] << 8);
                    if (e) { epLo = std::min(epLo, e); epHi = std::max(epHi, e); }
                }
                if (w.reg >= 8 && w.reg <= 10) {
                    tot[w.reg - 8]++; if (w.val & 0x10) eb[w.reg - 8]++;
                }
            }
            std::printf("  %5.0fs %8u %6u %5u   %5u/%u/%u   %d..%d\n",
                        double(w0) / cpuHz, c0, c1, r13, eb[0], eb[1], eb[2],
                        epLo == (1 << 20) ? 0 : epLo, epHi);
        }
    }

    // ── steady-state tick: median gap between consecutive R7 writes ─────
    {
        std::vector<uint64_t> gaps;
        uint64_t prevR7 = 0;
        for (const auto& w : log)
            if (w.chip == 0 && w.reg == 7) {
                if (prevR7) gaps.push_back(w.cycle - prevR7);
                prevR7 = w.cycle;
            }
        if (!gaps.empty()) {
            std::sort(gaps.begin(), gaps.end());
            const uint64_t med = gaps[gaps.size() / 2];
            std::printf("\nreplay tick (median R7→R7 gap): %llu cycles -> %.2f Hz"
                        "  [min %llu max %llu, n=%zu]\n",
                        (unsigned long long)med, cpuHz / double(med),
                        (unsigned long long)gaps.front(),
                        (unsigned long long)gaps.back(), gaps.size());
        }
        // Digidrum IRQ: median gap between consecutive amplitude-register
        // writes that are closer together than one replay tick.
        for (int rg = 8; rg <= 10; ++rg) {
            std::vector<uint64_t> dg;
            uint64_t p = 0;
            for (const auto& w : log)
                if (w.chip == 0 && w.reg == rg) {
                    if (p && w.cycle - p < 1000) dg.push_back(w.cycle - p);
                    p = w.cycle;
                }
            if (dg.size() > 50) {
                std::sort(dg.begin(), dg.end());
                std::printf("digidrum on R%d: n=%zu, median gap %llu cycles"
                            " -> %.0f Hz sample rate\n", rg, dg.size(),
                            (unsigned long long)dg[dg.size() / 2],
                            cpuHz / double(dg[dg.size() / 2]));
            }
        }
    }

    // ── measured replay tick + digidrum IRQ rate ────────────────────────
    // Bursts are separated by long gaps; anything > 2000 cycles apart starts
    // a new burst. Digidrum ISR writes land as 1-2 writes at ~150-cycle
    // spacing between bursts, so we histogram both gap classes.
    uint32_t nBurst = 0; uint64_t prevBurst = 0, sumBurstGap = 0;
    std::map<uint64_t, uint32_t> smallGaps;
    uint64_t prev = 0;
    for (const auto& w : log) {
        if (w.chip != 0) continue;
        const uint64_t gap = w.cycle - prev;
        if (prev && gap > 2000) {
            if (prevBurst) { sumBurstGap += w.cycle - prevBurst; ++nBurst; }
            prevBurst = w.cycle;
        } else if (prev && gap > 100 && gap < 400) {
            smallGaps[(gap / 10) * 10]++;
        }
        prev = w.cycle;
    }
    if (nBurst)
        std::printf("\nreplay tick: %u bursts, mean %.0f cycles -> %.2f Hz\n",
                    nBurst, double(sumBurstGap) / nBurst,
                    cpuHz / (double(sumBurstGap) / nBurst));
    std::printf("digidrum-class gaps (100-400 cycles, bucketed by 10):\n");
    for (auto& [g, c] : smallGaps)
        if (c > 20) std::printf("   ~%llu cycles (%u) -> %.0f Hz\n",
                                (unsigned long long)g, c, cpuHz / double(g ? g : 1));
    return 0;
}
