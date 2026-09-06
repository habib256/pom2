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
