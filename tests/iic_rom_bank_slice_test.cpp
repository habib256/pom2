// //c-class 32 KB ROM slicing is a property of the PROFILE — bug hunt #10.
//
// A //c-class dump is two firmware banks with bank 0 (the cold-reset entry)
// in the LOWER half; a //e "system + video" dump keeps its 16 KB firmware in
// the UPPER half. Same file size, opposite slicing, and nothing in the bytes
// says which. `profileUsesLowerRomHalf` is the one spelling; File > Reload
// ROM and the Welcome panel's reload used the //e slicing unconditionally, so
// on a //c they mapped bank 1, killed the $C028 toggle and reset into it.
// Three things pinned: the truth table, what the right flag does to the real
// dump, and that no MainWindow call site is left with the defaulted flag.

#include "Memory.h"
#include "ResourcePaths.h"
#include "SystemProfile.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

int main()
{
    // 1. The truth table.
    for (const pom2::SystemProfile p : pom2::allProfiles()) {
        const bool iic = p == pom2::SystemProfile::AppleIIc ||
                         p == pom2::SystemProfile::AppleIIcPlus ||
                         p == pom2::SystemProfile::AppleIIcPAL;
        assert(pom2::profileUsesLowerRomHalf(p) == iic);
    }

    // 2. The flag against the real dump: bank 0 at $D000, $C028 swaps to bank 1.
    const std::string rom = pom2::findResource("roms/apple2c-32Kv0.rom");
    if (rom.empty()) {
        std::printf("  (no roms/apple2c-32Kv0.rom — the bank check is skipped)\n");
    } else {
        std::ifstream f(rom, std::ios::binary);
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), {});
        assert(file.size() == 0x8000);
        Memory mem;
        mem.setIIEMode(true);
        assert(mem.loadAppleIIRom(rom.c_str(), pom2::profileUsesLowerRomHalf(pom2::SystemProfile::AppleIIc)));
        assert(mem.memRead(0xD000) == file[0x1000] && "bank 0 must be live after reset");
        (void)mem.memRead(0xC028);
        assert(mem.memRead(0xD000) == file[0x5000] && "$C028 must swap to bank 1");
        Memory wrong;
        wrong.setIIEMode(true);
        assert(wrong.loadAppleIIRom(rom.c_str(), /*pickLower16KFor32K=*/false));
        assert(wrong.memRead(0xD000) == file[0x5000] &&
               "(documenting the defect) the //e slicing maps bank 1 on a //c dump");
    }

    // 3. No MainWindow call site may leave the flag to its default.
    int failures = 0;
    for (const char* rel : { "src/MainWindow_Chrome.cpp", "src/MainWindow_MiscPanels.cpp",
                             "src/MainWindow_Slots.cpp" }) {
        std::string path;
        for (const char* prefix : { "", "../", "../../" }) {
            std::ifstream probe(std::string(prefix) + rel);
            if (probe) { path = std::string(prefix) + rel; break; }
        }
        if (path.empty()) { std::printf("FAIL: cannot open %s\n", rel); ++failures; continue; }
        std::ifstream f(path);
        std::string line; int n = 0;
        while (std::getline(f, line)) {
            ++n;
            const auto at = line.find("loadAppleIIRom(");
            if (at == std::string::npos) continue;
            // The call may span lines: gather until the closing paren.
            std::string call = line.substr(at);
            while (call.find(')') == std::string::npos && std::getline(f, line)) { call += line; ++n; }
            if (call.find(',') == std::string::npos) {
                std::printf("FAIL: %s:%d loads a main ROM without saying which half\n", rel, n);
                ++failures;
            }
        }
    }
    assert(failures == 0);
    std::printf("iic_rom_bank_slice OK\n");
    return 0;
}
