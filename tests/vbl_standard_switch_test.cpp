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

// VBL frame phase across a RUNTIME video-standard switch.
//
// `Memory::advanceCycles` tracks the start-of-frame cycle incrementally
// (`vblFrameBase_`) instead of taking a runtime-divisor modulo on every
// emulated instruction. That is only correct while the invariant
// `vblFrameBase_ % (65 * scanlinesPerFrame) == 0` holds — and the frame
// period is a live input: `applyProfile` calls `setVideoStandard()` on an
// already-running `Memory` when the user switches from an NTSC profile to
// the //c PAL one.
//
// The rollover branch cannot notice the change on its own: 17030 and 20280
// sit within a factor of two of each other, so `sinceBase` always lands in
// the "ordinary rollover" case and carries the stale residue forward
// forever. Booting NTSC and then switching to //c PAL used to put the VBL
// edge on scanline 252 instead of 192 — permanently, and out of step with
// `$C019`, `pushVideoEventLocked` and `Apple2Display::frameCycleToPos`,
// which all take a true modulo. Residues are multiples of
// gcd(17030, 20280) = 130, so the error is arbitrary rather than benign.
//
// This matters most on the //c PAL profile, whose 50 Hz VBL interrupt is
// what the French Touch / DIX demos use as their frame sync.
//
// The existing `pal_timing`, `vbl_smoke` and `vbl_edge_phase` tests all call
// `setVideoStandard` on a FRESH `Memory` (cycleCounter == 0, base trivially
// aligned to anything), which is why none of them saw this.
//
// Observation channel: a //c-class machine is the only one whose VBL edge is
// externally visible — `advanceCycles` drives the real CPU IRQ line there
// (`IRQ_SRC_VBL`), so the edge cycle can be read off `getIrqSourceMask()`.

#include "CpuClock.h"
#include "M6502.h"
#include "Memory.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kCyclesPerScanline = 65;
constexpr uint64_t kVisibleScanlines  = 192;
constexpr uint64_t kVblEdgeInFrame    = kVisibleScanlines * kCyclesPerScanline;  // 12480

std::string firstExisting(const std::vector<std::string>& candidates)
{
    namespace fs = std::filesystem;
    for (const auto& p : candidates) {
        if (fs::exists(p)) return p;
        const std::string up1 = "../" + p;    if (fs::exists(up1)) return up1;
        const std::string up2 = "../../" + p; if (fs::exists(up2)) return up2;
    }
    return {};
}

void advance(Memory& mem, uint64_t cycles)
{
    while (cycles > 0) {
        const uint64_t slice = cycles < 1000 ? cycles : 1000;
        mem.advanceCycles(static_cast<int>(slice));
        cycles -= slice;
    }
}

uint64_t frameCyclesOf(VideoStandard std)
{
    const VideoTiming& t = pom2VideoTiming(std);
    return static_cast<uint64_t>(t.cyclesPerScanline) *
           static_cast<uint64_t>(t.scanlinesPerFrame);
}

// Step one cycle at a time until the //c VBL interrupt asserts; returns the
// cycle counter at the assertion, or 0 if none within `limit` cycles.
uint64_t nextVblEdge(Memory& mem, M6502& cpu, uint64_t limit)
{
    cpu.setIrqLine(M6502::IRQ_SRC_VBL, false);
    for (uint64_t i = 0; i < limit; ++i) {
        mem.advanceCycles(1);
        // getIrqSourceMask() is a wire-OR BIT mask; IRQ_SRC_VBL is the bit index.
        if (cpu.getIrqSourceMask() & (1u << M6502::IRQ_SRC_VBL))
            return mem.getCycleCounter();
    }
    return 0;
}

int failures = 0;

void expectEdgePhase(const char* what, uint64_t edge, VideoStandard std)
{
    const uint64_t frame = frameCyclesOf(std);
    if (edge == 0) {
        std::printf("FAIL %s: no VBL edge within two frames\n", what);
        ++failures;
        return;
    }
    const uint64_t phase = edge % frame;
    if (phase != kVblEdgeInFrame) {
        std::printf("FAIL %s: VBL edge at cycle %llu -> %llu mod %llu "
                    "(scanline %llu), expected %llu (scanline %llu)\n",
                    what,
                    static_cast<unsigned long long>(edge),
                    static_cast<unsigned long long>(phase),
                    static_cast<unsigned long long>(frame),
                    static_cast<unsigned long long>(phase / kCyclesPerScanline),
                    static_cast<unsigned long long>(kVblEdgeInFrame),
                    static_cast<unsigned long long>(kVisibleScanlines));
        ++failures;
    }
}

// A //c-class machine armed for VBL interrupts through the real IOU decode
// ($C07F clears IOUDIS, $C05B is EnVBL), left running under `from`.
bool bringUp(Memory& mem, M6502& cpu, const std::string& rom, VideoStandard from)
{
    mem.setCpu(&cpu);
    mem.clearRam();
    mem.setIIEMode(true);
    if (!mem.loadAppleIIRom(rom.c_str(), /*pickLower16KFor32K=*/true)) {
        std::printf("FAIL: could not load %s\n", rom.c_str());
        ++failures;
        return false;
    }
    mem.resetSoftSwitches();
    mem.setVideoStandard(from);
    // Run for a while under the ORIGINAL standard so the frame base is
    // aligned to its period, at a cycle counter that is a multiple of
    // neither period (100000 % 17030 = 14850, 100000 % 20280 = 18880).
    advance(mem, 100000);
    mem.memWrite(0xC07F, 0);   // CLRIOUDIS → the IIc IOU switch decode
    mem.memWrite(0xC05B, 0);   // EnVBL
    return true;
}

}  // namespace

int main()
{
    const std::string rom = firstExisting({"roms/apple2c-32Kv0.rom",
                                           "roms/apple2c-16K.rom",
                                           "roms/apple2cp.rom"});
    if (rom.empty()) {
        std::printf("SKIP vbl_standard_switch: no //c-class ROM in roms/\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    // ── Baseline: no switch. Pins that the harness itself is sound. ──
    {
        Memory mem;
        M6502  cpu(&mem);
        if (!bringUp(mem, cpu, rom, VideoStandard::NTSC)) return 1;
        expectEdgePhase("NTSC steady state",
                        nextVblEdge(mem, cpu, 2 * frameCyclesOf(VideoStandard::NTSC)),
                        VideoStandard::NTSC);
    }

    // ── NTSC → PAL on a running machine (boot Apple ][+ / //e, then load
    // the //c PAL profile). The edge must re-align to PAL scanline 192. ──
    {
        Memory mem;
        M6502  cpu(&mem);
        if (!bringUp(mem, cpu, rom, VideoStandard::NTSC)) return 1;
        mem.setVideoStandard(VideoStandard::PAL);
        // Settle one PAL frame so `vblWasActive` reflects PAL geometry, then
        // measure two consecutive edges: the phase must hold, not drift.
        advance(mem, frameCyclesOf(VideoStandard::PAL));
        expectEdgePhase("NTSC->PAL first edge",
                        nextVblEdge(mem, cpu, 2 * frameCyclesOf(VideoStandard::PAL)),
                        VideoStandard::PAL);
        expectEdgePhase("NTSC->PAL second edge",
                        nextVblEdge(mem, cpu, 2 * frameCyclesOf(VideoStandard::PAL)),
                        VideoStandard::PAL);
    }

    // ── PAL → NTSC, the symmetric direction (//c PAL back to //e). ──
    {
        Memory mem;
        M6502  cpu(&mem);
        if (!bringUp(mem, cpu, rom, VideoStandard::PAL)) return 1;
        mem.setVideoStandard(VideoStandard::NTSC);
        advance(mem, frameCyclesOf(VideoStandard::NTSC));
        expectEdgePhase("PAL->NTSC first edge",
                        nextVblEdge(mem, cpu, 2 * frameCyclesOf(VideoStandard::NTSC)),
                        VideoStandard::NTSC);
        expectEdgePhase("PAL->NTSC second edge",
                        nextVblEdge(mem, cpu, 2 * frameCyclesOf(VideoStandard::NTSC)),
                        VideoStandard::NTSC);
    }

    // ── A backwards cycle jump (snapshot restore / rewind) must still
    // self-heal in one division — the unsigned-wrap path in advanceCycles. ──
    {
        Memory mem;
        M6502  cpu(&mem);
        if (!bringUp(mem, cpu, rom, VideoStandard::PAL)) return 1;
        mem.setCycleCounter(4242);
        advance(mem, frameCyclesOf(VideoStandard::PAL));
        expectEdgePhase("rewind under PAL",
                        nextVblEdge(mem, cpu, 2 * frameCyclesOf(VideoStandard::PAL)),
                        VideoStandard::PAL);
    }

    if (failures) {
        std::printf("vbl_standard_switch FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("vbl_standard_switch OK\n");
    return 0;
}
