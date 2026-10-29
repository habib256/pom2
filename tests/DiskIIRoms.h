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

// DiskIIRoms.h — the Disk II PROMs a hand-built DiskIICard needs.
//
// The card embedded Apple's 341-0027-A boot PROM and 341-0028-A sequencer
// until 2026-10-29; they are copyrighted, so they now come from roms/ (CI
// fetches them, tools/fetch_test_media.py). A test that boots a hand-built
// card, or reads a WOZ on one, loads them here — and fails loudly when they
// are missing rather than testing an empty PROM.
#pragma once

#include "DiskIICard.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace pom2test {

// roms/<name> from the repo root, build/ or build/tests (ctest's cwd).
inline std::string diskIIRomPath(const char* name)
{
    for (const char* root : { "", "../", "../../" }) {
        const std::string p = std::string(root) + "roms/" + name;
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p;
    }
    std::fprintf(stderr, "missing roms/%s — run tools/fetch_test_media.py\n", name);
    std::exit(1);
}

// The boot PROM, and the sequencer table WITHOUT selecting the bit-level
// LSS for nibble media — exactly the card's old embedded default.
inline void loadDiskIIDefaults(DiskIICard& card)
{
    if (!card.loadBootRom(diskIIRomPath("disk2.rom")) ||
        !card.loadLssRom(diskIIRomPath("diskii_p6.rom"), /*selectBitLss=*/false)) {
        std::fprintf(stderr, "Disk II PROM load failed\n");
        std::exit(1);
    }
}

} // namespace pom2test
