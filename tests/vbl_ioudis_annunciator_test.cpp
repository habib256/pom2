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

// $C058-$C05F on //c-class with IOUDIS at its reset default: annunciators,
// NOT the VBL interrupt.
//
// POM2 keeps a deliberate non-MAME overlay on IIe where $C05A/$C05B double
// as the VBL IRQ mask (`vbl_smoke_test.cpp` pins it). That overlay used to
// be gated on `iieMode` alone, which is also true for //c / //c+ / //c PAL —
// so on a //c the MAME-faithful IOU decode (gated `iicProfile_ && !ioudis`)
// was bypassed at the reset default (`ioudis == true`) and a plain
// `LDA $C05B` fell through into the overlay and armed the mask. Unlike IIe,
// the //c-class edge in `advanceCycles` really does drive the CPU IRQ line,
// so the guest then took an unhandled 50/60 Hz IRQ storm through $FFFE.
// The mirror case: `LDA $C05A` silently acknowledged a VBL interrupt the
// guest had legitimately armed with IOUDIS clear.
//
// MAME `apple2e.cpp do_io` (:1802-1876) splits exactly this way — the
// `(m_isiic || m_isace500) && !m_ioudis` branch (:1811) owns DisVBL/EnVBL
// (:1823-1830) and returns (:1848); the else branch, commented "IIe does
// not have IOUDIS" (:1851), handles only SETDHIRES/CLRDHIRES and falls
// through to the plain AN0/AN1/AN2 cases (:1975-1983) which touch `m_an1`
// and the gameio pin and nothing else. `m_ioudis = true` at reset (:1234).
//
// Observation channel: `Memory::appendSnapshotState` ends with a
// length-prefixed IOU section — intC8Rom, ioudis, vblIrqMask, vblIrqPending,
// then AN0/AN1/AN2 (added 2026-09-06) — so two fixed offsets from the end of
// the blob expose the mask and the latch directly, on IIe as well as on //c.
// Indexed from the section's own size below rather than from `blob.back()`,
// which is what silently re-aimed these probes at the annunciators.

#include "M6502.h"
#include "Memory.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kFrameCyclesNtsc = 65 * 262;

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

// Tail of appendSnapshotState:
//   [len=7][intC8Rom][ioudis][vblIrqMask][vblIrqPending][AN0][AN1][AN2]
// The section's length is the last four bytes before its payload, so read it
// rather than assuming: the payload has grown once already.
constexpr size_t kIouSectionLen = 7;

uint8_t iouByte(Memory& mem, size_t index)
{
    std::vector<uint8_t> blob;
    mem.appendSnapshotState(blob);
    // The section is the tail of the blob, so its 4-byte little-endian length
    // prefix sits just before it. Check the prefix against what this test
    // assumes: if the section ever grows again, that mismatch is a loud
    // failure here instead of two probes that silently read the wrong bytes.
    const size_t at = blob.size() - kIouSectionLen - 4;
    const size_t len = static_cast<size_t>(blob[at]) |
                       (static_cast<size_t>(blob[at + 1]) << 8) |
                       (static_cast<size_t>(blob[at + 2]) << 16) |
                       (static_cast<size_t>(blob[at + 3]) << 24);
    if (len != kIouSectionLen) {
        std::printf("FAIL: the IOU snapshot section is no longer %zu bytes "
                    "— update kIouSectionLen and the indices below\n",
                    kIouSectionLen);
        std::exit(1);
    }
    return blob[blob.size() - kIouSectionLen + index];
}
bool vblMask(Memory& mem)    { return iouByte(mem, 2) != 0; }
bool vblPending(Memory& mem) { return iouByte(mem, 3) != 0; }
bool irqAsserted(M6502& cpu)
{
    // getIrqSourceMask() is a wire-OR BIT mask; IRQ_SRC_VBL is the bit index.
    return (cpu.getIrqSourceMask() & (1u << M6502::IRQ_SRC_VBL)) != 0;
}

int failures = 0;

void expect(bool cond, const char* what)
{
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

}  // namespace

int main()
{
    // ── //c-class: every dump POM2 ships a profile for. ──
    const std::vector<std::string> iicRoms = {
        "roms/apple2c-32Kv0.rom", "roms/apple2c-16K.rom", "roms/apple2cp.rom",
    };
    int tested = 0;
    for (const auto& candidate : iicRoms) {
        const std::string rom = firstExisting({candidate});
        if (rom.empty()) continue;
        ++tested;

        Memory mem;
        M6502  cpu(&mem);
        mem.setCpu(&cpu);
        mem.clearRam();
        mem.setIIEMode(true);
        if (!mem.loadAppleIIRom(rom.c_str(), /*pickLower16KFor32K=*/true)) {
            std::printf("FAIL: could not load %s\n", rom.c_str());
            ++failures;
            continue;
        }
        mem.resetSoftSwitches();   // IOUDIS → true, the reset default

        // 1. The legacy annunciator idiom must be inert.
        (void)mem.memRead(0xC05B);          // LDA $C05B — AN1 on
        expect(!vblMask(mem), "//c: $C05B with IOUDIS set armed the VBL mask");
        advance(mem, 2 * kFrameCyclesNtsc);
        expect(!vblPending(mem), "//c: $C05B with IOUDIS set latched a VBL IRQ");
        expect(!irqAsserted(cpu), "//c: $C05B with IOUDIS set asserted IRQ_SRC_VBL");

        // 2. The real IOU decode must still work: IOUDIS clear → EnVBL arms,
        //    and the frame edge drives the CPU IRQ line (the //c PAL frame
        //    sync a French Touch demo depends on).
        mem.memWrite(0xC07F, 0);            // CLRIOUDIS
        mem.memWrite(0xC05B, 0);            // EnVBL
        expect(vblMask(mem), "//c: EnVBL with IOUDIS clear did not arm the mask");
        advance(mem, 2 * kFrameCyclesNtsc);
        expect(vblPending(mem), "//c: no VBL latch after two frames");
        expect(irqAsserted(cpu), "//c: VBL edge did not assert IRQ_SRC_VBL");

        // 3. With IOUDIS set again, an annunciator poke must NOT acknowledge
        //    the interrupt the guest armed while IOUDIS was clear.
        //    ($C07E is also the //c "any $C07x strobe acknowledges VBL"
        //    decode, so re-run a frame to get a fresh latch first.)
        mem.memWrite(0xC07E, 0);            // SETIOUDIS
        advance(mem, 2 * kFrameCyclesNtsc);
        expect(vblPending(mem), "//c: no VBL latch after re-setting IOUDIS");
        expect(irqAsserted(cpu), "//c: no IRQ_SRC_VBL after re-setting IOUDIS");
        (void)mem.memRead(0xC05A);          // LDA $C05A — AN1 off
        expect(vblMask(mem), "//c: $C05A with IOUDIS set disarmed the VBL mask");
        expect(vblPending(mem), "//c: $C05A with IOUDIS set cleared the VBL latch");
        expect(irqAsserted(cpu), "//c: $C05A with IOUDIS set released IRQ_SRC_VBL");

        // 4. DisVBL through the real decode still works. (The $C07F strobe
        //    already dropped the latch; DisVBL has to drop the mask.)
        mem.memWrite(0xC07F, 0);            // CLRIOUDIS
        mem.memWrite(0xC05A, 0);            // DisVBL
        expect(!vblMask(mem), "//c: DisVBL with IOUDIS clear left the mask armed");
        expect(!vblPending(mem), "//c: DisVBL with IOUDIS clear left the latch set");
        expect(!irqAsserted(cpu), "//c: DisVBL with IOUDIS clear left IRQ_SRC_VBL up");
    }
    if (tested == 0) {
        std::printf("SKIP vbl_ioudis_annunciator: no //c-class ROM in roms/\n");
        return failures ? 1 : 0;
    }

    // ── IIe: the POM2 overlay is UNCHANGED. $C05A/$C05B still drive the
    // mask (what `vbl_smoke_test.cpp` relies on), and the CPU IRQ line is
    // still deliberately never driven there — asserting it would resurrect
    // the original "ProDOS crashes on an annunciator poke" bug. ──
    {
        Memory mem;
        M6502  cpu(&mem);
        mem.setCpu(&cpu);
        mem.setIIEMode(true);
        mem.resetSoftSwitches();

        mem.memWrite(0xC05B, 0);
        expect(vblMask(mem), "//e: $C05B no longer arms the overlay mask");
        advance(mem, 2 * kFrameCyclesNtsc);
        expect(vblPending(mem), "//e: no VBL latch after two frames");
        expect(!irqAsserted(cpu), "//e: VBL edge drove the CPU IRQ line");

        mem.memWrite(0xC05A, 0);
        expect(!vblMask(mem), "//e: $C05A no longer disarms the overlay mask");
        expect(!vblPending(mem), "//e: $C05A did not clear the VBL latch");
    }

    // ── II/II+: no IIe mode, no mask at all (vbl_smoke case 1). ──
    {
        Memory mem;
        M6502  cpu(&mem);
        mem.setCpu(&cpu);
        mem.resetSoftSwitches();

        mem.memWrite(0xC05B, 0);
        expect(!vblMask(mem), "II+: $C05B armed the VBL mask");
        advance(mem, 2 * kFrameCyclesNtsc);
        expect(!vblPending(mem), "II+: $C05B latched a VBL IRQ");
        expect(!irqAsserted(cpu), "II+: $C05B asserted IRQ_SRC_VBL");
    }

    if (failures) {
        std::printf("vbl_ioudis_annunciator FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("vbl_ioudis_annunciator OK\n");
    return 0;
}
