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

// Smoke test: no-disk power-on boot via the on-board IWM (//c + //c+).
//
// Companion to iic_boot_trace.cpp (which boots a real disk). Here NO disk
// is inserted, mirroring a real machine powered on with an empty drive.
//
// On real hardware an empty, spinning 5.25" drive feeds the IWM a stream
// of noise flux, so the read shift register keeps assembling garbage bytes
// with bit-7 ("byte ready") set. The //c boot firmware relies on that: its
// wait-for-byte loop ($C0EC bit 7) must keep advancing so the per-read
// retry counter can drain and the machine falls through to its "Check Disk
// Drive." screen.
//
// POM2's IWM collapsed MAME's window timer into IWMDevice::nextTransition().
// Pre-fix that returned INT64_MAX when no media was present, so the read FSM
// only ever shifted in 0-bits, bit-7 never asserted, and the //c spun
// forever in F8 firmware ($C661) never reaching "Check Disk Drive."
// IWMDevice::noiseTransition() restores the noise stream; this test pins it.

#include "M6502.h"
#include "Memory.h"
#include "SlotBus.h"
#include "DiskIICard.h"
#include "IWMDevice.h"

#include <cctype>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::string firstExisting(const std::vector<std::string>& candidates)
{
    namespace fs = std::filesystem;
    for (const auto& p : candidates) {
        if (fs::exists(p)) return p;
        const std::string up1 = "../" + p;   if (fs::exists(up1)) return up1;
        const std::string up2 = "../../" + p; if (fs::exists(up2)) return up2;
    }
    return {};
}

// Linearise the interleaved 24x40 text page into an upper-cased blob so we
// can substring-search for firmware messages.
std::string scrapeTextUpper(const uint8_t* ram)
{
    std::string out;
    for (int row = 0; row < 24; ++row) {
        const int base = 0x0400 + 0x80 * (row % 8) + 0x28 * (row / 8);
        for (int col = 0; col < 40; ++col) {
            const char c = static_cast<char>(ram[base + col] & 0x7F);
            out.push_back((c >= 0x20 && c < 0x7F)
                          ? static_cast<char>(std::toupper(c)) : ' ');
        }
        out.push_back('\n');
    }
    return out;
}

// Run a //c-class ROM with NO disk and assert `needle` appears on the text
// page within the instruction budget. Returns 0 on pass, 1 on fail, 2 on skip
// (kept out of the 0/1 bit so `rc |= …` cannot let a skip mask a failure).
constexpr int kCaseSkipped = 2;

int runNoDiskCase(const char* tag, const std::string& romPath,
                  const char* needle, int instrs)
{
    if (romPath.empty()) {
        std::printf("SKIP iic_nodisk_boot_trace[%s]: ROM not present\n", tag);
        return kCaseSkipped;
    }

    Memory mem;
    M6502  cpu(&mem);
    mem.setCpu(&cpu);
    pom2::IWMDevice iwm;
    mem.setIWM(&iwm);
    mem.setIWMAuthoritative(true);

    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);

    if (!mem.loadAppleIIRom(romPath.c_str(), /*pickLowerHalf=*/true)) {
        std::printf("FAIL[%s]: could not load %s\n", tag, romPath.c_str());
        return 1;
    }

    auto card = std::make_unique<DiskIICard>(6);
    const std::string diskRom = firstExisting({"roms/disk2.rom"});
    if (!diskRom.empty()) card->loadBootRom(diskRom);
    const std::string lssRom = firstExisting({"roms/diskii_p6.rom"});
    if (!lssRom.empty()) card->loadLssRom(lssRom);
    // Deliberately NO insertDisk() — empty drive.
    card->setIWM(&iwm);
    mem.slotBus().plug(6, std::move(card));
    mem.slotBus().reset();

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();

    bool sawC600 = false;
    std::string screen;
    bool found = false;
    for (int i = 0; i < instrs && !found; ++i) {
        const uint16_t pc = cpu.getProgramCounter();
        if (pc >= 0xC600 && pc < 0xC700) sawC600 = true;
        cpu.step();
        if ((i & 0x3FFFF) == 0) {           // poll the screen periodically
            screen = scrapeTextUpper(mem.data());
            if (screen.find(needle) != std::string::npos) found = true;
        }
    }
    if (!found) screen = scrapeTextUpper(mem.data());
    found = found || screen.find(needle) != std::string::npos;

    std::printf("[%s] sawC600=%d found(\"%s\")=%d finalPC=$%04X\n",
                tag, sawC600, needle, found, cpu.getProgramCounter());
    if (!found) {
        std::printf("FAIL[%s]: firmware never displayed \"%s\" "
                    "(IWM no-disk noise flux regressed?)\nScreen:\n%s",
                    tag, needle, screen.c_str());
        return 1;
    }
    return 0;
}

}  // namespace

int main()
{
    int rc = 0, ran = 0;
    auto tally = [&](int r) { if (r == kCaseSkipped) return; ++ran; rc |= r; };

    // //c (32 KB): empty internal 5.25" drive -> "Check Disk Drive."
    tally(runNoDiskCase("//c-32k",
                        firstExisting({"roms/apple2c-32Kv0.rom"}),
                        "CHECK DISK", 20'000'000));
    // //c+: scans 3.5"/SmartPort + 5.25", then the no-online-disk banner.
    tally(runNoDiskCase("//c+",
                        firstExisting({"roms/apple2cp.rom",
                                       "roms/apple2c-plus.rom"}),
                        "UNABLE TO FIND A BOOTABLE DISK", 25'000'000));

    if (rc != 0) return rc;
    if (ran == 0) {
        std::printf("SKIP iic_nodisk_boot_trace: no //c-class ROM present\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }
    std::printf("PASS\n");
    return 0;
}
