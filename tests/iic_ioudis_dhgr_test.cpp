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

// //c-class: IOUDIS gates $C05E/$C05F, not just the VBL pair.
//
// `vbl_ioudis_annunciator` pins the VBL half of the same IOU decode. This
// pins the DISPLAY half, which had been carried in the TODO as an open MAME
// deviation long after the 2026-07-30 IOUDIS work actually closed it.
//
// IOUDIS CLEAR — $C058-$C05F are the IIc IOU's switches (DisXY/EnbXY,
// DisVBL/EnVBL, and the X0/Y0 edge selects at $C05C-$C05F). MAME's
// `(m_isiic || m_isace500) && !m_ioudis` branch swallows the whole range and
// returns WITHOUT reaching an3_w, so the mouse firmware walking that switch
// protocol must not flip the display into double-hi-res.
//
// IOUDIS SET — $C05E/$C05F are SETDHIRES / CLRDHIRES again. The IIc Technical
// Reference reserves only $C058-$C05D in that state (MAME quotes it in the
// fall-through), which is why 80-column software keeps working at the reset
// default.
//
// RDIOUDIS ($C078/$C07A/$C07C/$C07E) — the READ side of the same decode, and
// the second thing pinned here. MAME answers it in `c000_iic_r` ONLY
// (`apple2e.cpp:2336-2338`): `(m_ioudis ? 0x80 : 0x00) | uFloatingBus7`.
// A plain //e has no case for it in `c000_r` at all and drops out to
// `return uFloatingBus;`. POM2 answered on every IIe-class machine and
// clamped bits 0-6 to zero.
//
// $C060 gets the same treatment (`:2177-2185`, `case 0x60: case 0x68:`):
// the cassette comparator in bit 7, the floating bus in bits 0-6, on BOTH
// halves of the `.mirror(0x8)` pair — POM2's $C060 branch dropped it while
// its $C068 twin kept it.

#include "M6502.h"
#include "Memory.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool cond, const std::string& what)
{
    if (!cond) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

std::string firstExisting(const std::string& rel)
{
    namespace fs = std::filesystem;
    for (const std::string& p : { rel, "../" + rel, "../../" + rel })
        if (fs::exists(p)) return p;
    return {};
}

} // namespace

int main()
{
    // Every //c-class dump POM2 ships a profile for. CI stays ROM-free, so a
    // missing dump skips rather than fails.
    const std::vector<std::string> candidates = {
        "roms/apple2c-32Kv0.rom", "roms/apple2c-16K.rom", "roms/apple2cp.rom",
    };

    int tested = 0;
    for (const std::string& candidate : candidates) {
        const std::string rom = firstExisting(candidate);
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
        mem.resetSoftSwitches();          // IOUDIS → true, the reset default

        // ── IOUDIS clear: the IOU owns the range, DHIRES is unreachable ──
        mem.memWrite(0xC07F, 0);          // CLRIOUDIS
        const bool before = mem.getDisplayState().dhgr;

        (void)mem.memRead(0xC05E);        // SETDHIRES on a IIe
        expect(mem.getDisplayState().dhgr == before,
               rom + ": $C05E with IOUDIS clear reached DHGR");
        (void)mem.memRead(0xC05F);        // CLRDHIRES on a IIe
        expect(mem.getDisplayState().dhgr == before,
               rom + ": $C05F with IOUDIS clear reached DHGR");

        // Writes take the same decode — a guest storing to the switch must
        // not sneak past a read-only gate.
        mem.memWrite(0xC05E, 0);
        expect(mem.getDisplayState().dhgr == before,
               rom + ": writing $C05E with IOUDIS clear reached DHGR");

        // ── IOUDIS set: they are the display switches again ──
        mem.memWrite(0xC07E, 0);          // SETIOUDIS
        (void)mem.memRead(0xC05E);
        expect(mem.getDisplayState().dhgr,
               rom + ": $C05E with IOUDIS set did not set DHGR");
        (void)mem.memRead(0xC05F);
        expect(!mem.getDisplayState().dhgr,
               rom + ": $C05F with IOUDIS set did not clear DHGR");

        // ── RDIOUDIS on a //c: bit 7 = state, bits 0-6 = floating bus ──
        // Park the scanner where it fetches a byte we planted, so "bits 0-6
        // carry the bus" is provable and not 0 == 0.
        for (uint16_t a = 0x0400; a < 0x0800; ++a) mem.memWrite(a, 0x6D);
        mem.setCycleCounter(25);
        const uint8_t bus7 = static_cast<uint8_t>(mem.peekFloatingBus() & 0x7F);
        expect(bus7 != 0x00, rom + ": floating bus sample is degenerate (0)");

        mem.memWrite(0xC07E, 0);          // SETIOUDIS
        for (uint16_t a : { 0xC078, 0xC07A, 0xC07C, 0xC07E }) {
            const uint8_t v = mem.memRead(a);
            expect(v == static_cast<uint8_t>(0x80 | bus7),
                   rom + ": RDIOUDIS set — wrong value");
        }
        mem.memWrite(0xC07F, 0);          // CLRIOUDIS
        for (uint16_t a : { 0xC078, 0xC07A, 0xC07C, 0xC07E }) {
            const uint8_t v = mem.memRead(a);
            expect(v == bus7, rom + ": RDIOUDIS clear — wrong value");
        }

        // ── $C060 and its $C068 mirror agree, and both carry the bus ──
        expect(mem.memRead(0xC060) == bus7, rom + ": $C060 lost the bus");
        expect(mem.memRead(0xC068) == bus7, rom + ": $C068 lost the bus");
    }

    // ── A plain //e must NOT answer RDIOUDIS at all ────────────────────
    {
        const std::string iie = firstExisting("roms/apple2e.rom");
        if (!iie.empty()) {
            Memory mem;
            M6502  cpu(&mem);
            mem.setCpu(&cpu);
            mem.clearRam();
            mem.setIIEMode(true);
            if (mem.loadAppleIIRom(iie.c_str(), /*pickLower16KFor32K=*/true)) {
                mem.resetSoftSwitches();
                for (uint16_t a = 0x0400; a < 0x0800; ++a) mem.memWrite(a, 0x6D);
                mem.setCycleCounter(25);
                const uint8_t bus = mem.peekFloatingBus();
                expect((bus & 0x7F) != 0, "//e: floating bus sample is degenerate");
                // Whatever IOUDIS happens to hold, the //e read is pure bus —
                // no bit-7 override either way.
                expect(mem.memRead(0xC07E) == bus, "//e: $C07E answered RDIOUDIS");
                mem.memWrite(0xC07E, 0);      // write is //c-only too
                expect(mem.memRead(0xC07E) == bus, "//e: $C07E answered RDIOUDIS");
                expect(mem.memRead(0xC060) ==
                           static_cast<uint8_t>(bus & 0x7F),
                       "//e: $C060 lost the floating bus");
                ++tested;
            }
        }
    }

    if (tested == 0) {
        std::printf("iic_ioudis_dhgr SKIPPED (no //c ROM present)\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }
    if (failures) {
        std::printf("iic_ioudis_dhgr FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("iic_ioudis_dhgr OK (%d ROM%s)\n", tested, tested == 1 ? "" : "s");
    return 0;
}
