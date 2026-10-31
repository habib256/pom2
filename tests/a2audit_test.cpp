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

// a2audit — Zellyn Hunter's Apple II audit (github.com/zellyn/a2audit, MIT;
// disks_5.4/dsk/a2audit.dsk) on the ][+, the unenhanced //e and the
// enhanced //e. The audit detects the model, then runs its language card,
// auxiliary memory and soft-switch tests; the first failure prints
// "ZELLYN.COM/A2AUDIT/V0#Ennnnnn" and the error's details. Its keyboard check
// is answered with a space, and its video tests — which ask a person whether
// two modes look the same — are entered with a space and skipped with Esc,
// so the run ends on the line "END".
//
// Pins, per machine: the model line the audit prints, no E-code anywhere,
// and the END line.

#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr int64_t kSecond = 1'022'727;

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

uint16_t textRowBase(int row) { return uint16_t(0x0400 + (row % 8) * 0x80 + (row / 8) * 0x28); }

std::string screen(Memory& mem)
{
    std::string out;
    for (int r = 0; r < 24; ++r) {
        std::string s;
        for (int c = 0; c < 40; ++c) {
            uint8_t ch = uint8_t(mem.peekMainRam(uint16_t(textRowBase(r) + c)) & 0x7F);
            if (ch < 0x20) ch = uint8_t(ch + 0x40);
            s.push_back(ch < 0x7F ? char(ch) : '.');
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        out += s + "\n";
    }
    return out;
}

struct Machine {
    const char* name;
    const char* rom;
    bool iie;
    bool cmos;
    const char* modelLine;   // what detect.asm prints for it
};

std::string findFirst(std::initializer_list<const char*> c)
{
    for (const char* p : c) if (fs::is_regular_file(p)) return p;
    return {};
}

// 0 ran, 77 media missing.
int audit(const Machine& m, const std::string& disk)
{
    const std::string rom  = findFirst({ m.rom });
    const std::string prom = findFirst({ "roms/disk2.rom" });
    if (rom.empty() || prom.empty()) return 77;
    const fs::path tmp = fs::temp_directory_path() / "pom2_a2audit.dsk";
    std::error_code ec;
    fs::copy_file(disk, tmp, fs::copy_options::overwrite_existing, ec);

    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(m.iie);
    cpu.setCpuMode(m.cmos ? M6502::CpuMode::CMOS : M6502::CpuMode::NMOS);
    auto d2 = std::make_unique<DiskIICard>(6);
    if (!mem.loadAppleIIRom(rom.c_str()) || !d2->loadBootRom(prom)
        || !d2->insertDisk(tmp.string())) {
        std::fprintf(stderr, "FAIL: %s setup\n", m.name);
        ++g_fail;
        return 0;
    }
    mem.slotBus().plug(6, std::move(d2));
    cpu.hardReset();
    mem.slotBus().reset();
    auto run = [&](int64_t cycles) { for (int64_t t = 0; t < cycles;) t += cpu.run(4096); };
    // "\n" + screen: a match on a whole line can anchor on the first row.
    auto until = [&](const char* text, int seconds) {
        for (int i = 0; i < seconds * 4; ++i) {
            if (("\n" + screen(mem)).find(text) != std::string::npos) return true;
            run(kSecond / 4);
        }
        return ("\n" + screen(mem)).find(text) != std::string::npos;
    };

    bool ended = false;
    std::string text;   // the report, before the video tests clear it
    if (until("PRESS 'Y', 'N', SPACE, OR ESC", 30)) {
        text = screen(mem);
        mem.pasteKeyStream(" ", 1);
        if (until("HIT SPACE TO START", 10)) {
            mem.pasteKeyStream(" ", 1);            // start the video tests,
            run(kSecond);
            mem.pasteKeyStream("\x1B", 1);         // then skip them all
            ended = until("\nEND\n", 10);   // the line, not "SKIP TO END"
        }
    }
    if (text.empty()) text = screen(mem);
    const bool clean = text.find("A2AUDIT/V0#E") == std::string::npos;
    std::printf("  %-22s %s\n", m.name, clean && ended ? "all automatic tests passed"
                                                   : "FAILED");
    CHECK(text.find(m.modelLine) != std::string::npos);
    CHECK(clean);
    CHECK(ended);
    if (!clean || !ended || std::getenv("POM2_DUMP"))
        std::fprintf(stderr, "---- %s ----\n%s", m.name, text.c_str());
    return 0;
}

} // namespace

int main()
{
    const std::string disk = findFirst({ "disks_5.4/dsk/a2audit.dsk" });
    if (disk.empty()) {
        std::printf("a2audit: SKIP — needs disks_5.4/dsk/a2audit.dsk\n");
        return 77;
    }
    const Machine machines[] = {
        { "Apple ][+",          "roms/apple2p.rom",       false, false, "APPLE II PLUS" },
        { "Apple //e",          "roms/apple2e_unenh.rom", true,  false, "APPLE IIE" },
        { "Apple //e enhanced", "roms/apple2e.rom",       true,  true,  "APPLE IIE (ENHANCED)" },
    };
    int skipped = 0;
    for (const Machine& m : machines) if (audit(m, disk) == 77) ++skipped;
    std::error_code ec;
    fs::remove(fs::temp_directory_path() / "pom2_a2audit.dsk", ec);
    if (g_fail) {
        std::fprintf(stderr, "a2audit: %d failure(s)\n", g_fail);
        return 1;
    }
    if (skipped) {
        std::printf("a2audit: SKIP %d machine(s) — ROM missing\n", skipped);
        return 77;
    }
    std::printf("a2audit: PASS\n");
    return 0;
}
