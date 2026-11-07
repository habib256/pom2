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

// rom_errata — known dump defects are put right in memory, and only on the
// exact dump (RomErrata.h). The ][+ dump POM2 pins carries $FF59 at $FFFE
// where Apple's 341-0020-00 has $FA40: every IRQ and BRK fell into the
// Monitor's reset entry. The file on disk must stay byte-identical (it is
// SHA-256-pinned); the machine must see $FA40.

#include "Memory.h"
#include "RomErrata.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool ok, const char* what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

}  // namespace

int main()
{
    // The CRC is the IEEE one MAME prints: the standard check value.
    const char* check = "123456789";
    expect(pom2::romCrc32(reinterpret_cast<const std::uint8_t*>(check), 9) == 0xCBF43926u,
           "romCrc32 is CRC-32/IEEE");

    // Anything but the exact dump is left alone, even with $59 $FF at $FFFE:
    // a user's own ROM, a hack, a ROM that wants that vector.
    std::vector<std::uint8_t> mem(0x10000, 0xEA);
    mem[0xFFFE] = 0x59;
    mem[0xFFFF] = 0xFF;
    const auto before = mem;
    expect(pom2::applyRomErrata(mem.data()).empty(), "no note for a foreign F8 page");
    expect(mem == before, "a foreign F8 page is not touched");

    // The pinned dump itself (CI fetches it; absent elsewhere is a skip of
    // this half only, said aloud).
    const char* path = "roms/apple2p.rom";
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::printf("SKIP rom_errata: %s absent (tools/fetch_test_media.py)\n", path);
    } else {
        const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(f)),
                                             std::istreambuf_iterator<char>());
        Memory m;
        m.setIIEMode(false);
        expect(m.loadAppleIIRom(path) == 1, "apple2p.rom loads");
        expect(m.peekMainRam(0xFFFE) == 0x40 && m.peekMainRam(0xFFFF) == 0xFA,
               "the ][+ IRQ/BRK vector reads $FA40, the Autostart Monitor's IRQ");
        std::uint8_t page[0x800];
        for (int i = 0; i < 0x800; ++i) page[i] = m.peekMainRam(static_cast<uint16_t>(0xF800 + i));
        expect(pom2::romCrc32(page, sizeof page) == 0x079589C4u,
               "the F8 page is 341-0020-00 (MAME CRC 079589c4)");
        std::ifstream again(path, std::ios::binary);
        const std::vector<std::uint8_t> after((std::istreambuf_iterator<char>(again)),
                                              std::istreambuf_iterator<char>());
        expect(after == file, "the pinned file is not modified");

        // A .pom2snap or session saved BEFORE the erratum carries $59 $FF in
        // its ENV section: it must still name this machine's firmware, and a
        // session installed from it must run the corrected vector (bug hunt
        // 2026-11-07 — it was refused as "firmware differs", and the session
        // path brought $FF59 back).
        std::vector<std::uint8_t> live;
        m.appendSnapshotEnvironment(live);
        const std::size_t at = 8 + (0xFFFE - 0xD000) * 2 + 1;   // (flag, byte) pairs
        auto old = live;
        old[at] = 0x59; old[at + 2] = 0xFF;
        expect(old != live, "the old ENV differs before normalisation");
        auto normalised = old;
        pom2::applyRomErrataToEnvironment(normalised);
        expect(normalised == live, "an ENV saved before the erratum compares equal");
        Memory n;
        std::string err;
        expect(n.installSessionEnvironment(old, err), "an old session ENV installs");
        expect(n.peekMainRam(0xFFFE) == 0x40 && n.peekMainRam(0xFFFF) == 0xFA,
               "a session installed from an old ENV runs the corrected vector");
    }

    if (failures) return 1;
    std::puts("rom_errata: OK");
    return 0;
}
