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

// mb_audit — Tom Charlesworth's mb-audit (github.com/tomcw/mb-audit, the
// AppleWin author's Mockingboard / Phasor test suite;
// disks_5.4/dsk/mb-audit-v1.61.po) on an enhanced //e, menu choice (A), the
// quick audit. It scans slots 7..1
// for 6522s, then tests every card it finds — 6522 timers to the cycle,
// every addressing mode's false reads, IRQs, the AY registers, the speech
// chips — and stops at the first failure with "Mockingboard failed test:"
// and `Test: CC:TT:nn / Expected / Actual`. A clean run prints "All tests
// passed", then asks for Ctrl-Reset (done here as the machine's warm reset,
// for its reset tests) and ends in a manual tone test, left alone.
//
// Configurations, each its own machine: one Mockingboard (A/C) in slot 4;
// a Sound II (Mockingboard with SSI263) in 4; a Phasor in 4; two
// Mockingboards (2 and 4, the multi-card tests).
//
// Every configuration must pass everything, reset tests included. Three
// POM2 bugs were found this way (CHANGELOG 2026-10-31): the 6522's IFR edge
// one cycle late (T6522_F/10/11), the AY bus not floating high (21:14), and
// the 65C02's indexed-store dummy read (T263_1, "Willy Byte"). A config's
// `knownFailure` can park a run at a known first failure while one is open.
//
// POM2_MBAUDIT_ONLY=<name> runs one configuration; POM2_DUMP=1 prints every
// final screen.

#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "Mockingboard.h"
#include "PhasorCard.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
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

// mb-audit prints in 40 columns, mixed case on a //e.
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

struct Config {
    const char* name;
    std::function<void(Memory&, M6502&)> plug;
    /// The first test POM2 fails today (component:test:subtest), "" = none.
    const char* knownFailure = "";
};

void plugMb(Memory& mem, M6502& cpu, int slot, MockingboardCard::Variant v)
{
    auto c = std::make_unique<MockingboardCard>(slot, v);
    c->setSampleRate(48000);
    c->setCpu(&cpu);
    mem.slotBus().plug(slot, std::move(c));
}

void plugPhasor(Memory& mem, M6502& cpu, int slot)
{
    auto c = std::make_unique<PhasorCard>(slot);
    c->setSampleRate(48000);
    c->setCpu(&cpu);
    mem.slotBus().plug(slot, std::move(c));
}

// 0 ran, 77 media missing.
int audit(const Config& cfg, const std::string& disk)
{
    const fs::path tmp = fs::temp_directory_path() / "pom2_mb_audit.po";
    std::error_code ec;
    fs::copy_file(disk, tmp, fs::copy_options::overwrite_existing, ec);

    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(true);
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    auto d2 = std::make_unique<DiskIICard>(6);
    if (!mem.loadAppleIIRom("roms/apple2e.rom") || !d2->loadBootRom("roms/disk2.rom")
        || !d2->insertDisk(tmp.string())) {
        std::fprintf(stderr, "FAIL: %s setup\n", cfg.name);
        ++g_fail;
        return 0;
    }
    mem.slotBus().plug(6, std::move(d2));
    cfg.plug(mem, cpu);
    cpu.hardReset();
    mem.slotBus().reset();
    auto run = [&](int64_t cycles) { for (int64_t t = 0; t < cycles;) t += cpu.run(4096); };
    auto has = [&](const char* t) { return screen(mem).find(t) != std::string::npos; };
    auto until = [&](std::initializer_list<const char*> texts, int seconds) -> const char* {
        for (int i = 0; i <= seconds * 4; ++i) {
            for (const char* t : texts) if (has(t)) return t;
            run(kSecond / 4);
        }
        return nullptr;
    };

    auto count = [&](const char* t) {
        int n = 0;
        const std::string sc = screen(mem);
        for (auto p = sc.find(t); p != std::string::npos; p = sc.find(t, p + 1)) ++n;
        return n;
    };

    // v1.6x opens on a menu: (A) is the quick audit. Then every card's
    // tests, the multi-card tests, a Ctrl-Reset per card (two on a Phasor),
    // the manual tone test (left with Esc), and only then the verdict.
    if (until({ "(A) AUDIT" }, 20)) mem.pasteKeyStream("A", 1);
    const char* r = until({ "Press CTRL+RESET", "failed test", "No Mockingboard" }, 120);
    int resets = 0;
    while (r && std::strcmp(r, "Press CTRL+RESET") == 0 && resets < 8) {
        const int prompts = count("Press CTRL+RESET");
        mem.resetSoftSwitchesWarm();          // Ctrl-Reset, as POM2's F11
        mem.slotBus().reset();
        cpu.softReset();
        ++resets;
        r = nullptr;
        for (int i = 0; i < 30 * 4 && !r; ++i) {
            run(kSecond / 4);
            if (count("Press CTRL+RESET") > prompts) r = "Press CTRL+RESET";
            else if (has("KEYS: 1-6"))  r = "KEYS: 1-6";   // "USE KEYS" flashes
            else if (has("failed test")) r = "failed test";
        }
    }
    for (int card = 0; r && std::strcmp(r, "KEYS: 1-6") == 0 && card < 8; ++card) {
        mem.pasteKeyStream("\x1B", 1);       // leave the tone test — one Esc per card
        run(kSecond / 2);
        r = has("All tests passed") ? "All tests passed"
          : has("failed test")      ? "failed test" : "KEYS: 1-6";
    }
    const bool passed = r && std::strcmp(r, "All tests passed") == 0;
    const bool resetOk = passed && has("Reset tests passed");
    const std::string report = screen(mem);
    std::string warnings;
    for (auto p = report.find("Test warning:"); p != std::string::npos;
         p = report.find("Test warning:", p + 1))
        warnings += " [warning " + report.substr(p + 13, report.find('\n', p) - p - 13) + "]";
    std::string failedAt;
    if (const auto t = report.find("Test: "); t != std::string::npos)
        failedAt = report.substr(t + 6, 8);
    const bool known = !passed && *cfg.knownFailure && failedAt == cfg.knownFailure;
    std::printf("  %-28s %s%s\n", cfg.name,
                passed ? "All tests passed" : (r ? ("failed test " + failedAt).c_str() : "no result (timeout)"),
                passed ? (resetOk ? ", reset tests passed" : ", reset tests NOT passed")
                       : known ? " (known, parked)" : "");
    if (!warnings.empty()) std::printf("  %-28s%s\n", "", warnings.c_str());
    CHECK(passed || known);
    CHECK(!passed || resetOk);
    if ((!passed && !known) || (passed && !resetOk) || std::getenv("POM2_DUMP"))
        std::fprintf(stderr, "---- %s ----\n%s", cfg.name, report.c_str());
    return 0;
}

} // namespace

int main()
{
    const char* disk = "disks_5.4/dsk/mb-audit-v1.61.po";
    if (!fs::is_regular_file(disk) || !fs::is_regular_file("roms/apple2e.rom")
        || !fs::is_regular_file("roms/disk2.rom")) {
        std::printf("mb_audit: SKIP — needs %s, roms/apple2e.rom, roms/disk2.rom\n", disk);
        return 77;
    }
    using V = MockingboardCard::Variant;
    const Config configs[] = {
        { "Mockingboard A/C (slot 4)", [](Memory& m, M6502& c) { plugMb(m, c, 4, V::AC); } },
        { "Sound II + SSI263 (slot 4)", [](Memory& m, M6502& c) { plugMb(m, c, 4, V::SoundII); } },
        { "Phasor (slot 4)",            [](Memory& m, M6502& c) { plugPhasor(m, c, 4); } },
        { "2x Mockingboard (2, 4)",     [](Memory& m, M6502& c) {
              plugMb(m, c, 2, V::AC); plugMb(m, c, 4, V::AC); } },
    };
    const char* only = std::getenv("POM2_MBAUDIT_ONLY");
    for (const Config& c : configs)
        if (!only || std::strstr(c.name, only)) audit(c, disk);
    std::error_code ec;
    fs::remove(fs::temp_directory_path() / "pom2_mb_audit.po", ec);
    if (g_fail) {
        std::fprintf(stderr, "mb_audit: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("mb_audit: PASS\n");
    return 0;
}
