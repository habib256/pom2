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

// Smoke test: //c with 32 KB ROM boots a 5.25" disk via the on-board
// IWM. POM2 pre-fix had `$C0E0-$C0EF` routed through DiskIICard's LSS
// path on plain //c; MAME wires `A2BUS_IWM` for the 32 KB //c dumps
// (`apple2c0` UniDisk-3.5, `apple2c3`/`apple2c4` Memory Expansion)
// per `apple2e.cpp:5249-5254` + `5263-5272` + `6281-6291` — only the
// 16 KB rev-255 `apple2c` keeps `A2BUS_DISKIING`. The //c's bank-0
// $C600 firmware drives the disk through IWM-style register sequences
// the LSS doesn't satisfy → boot never reached DOS, the //c trace
// PC was stuck cycling F8 firmware.
//
// This test loads the 32 KB //c ROM, plugs a slot-6 DiskIICard +
// wires the IWM (mirroring EmulationController), boots a known disk
// image, and asserts that the boot landed inside DOS code (PC outside
// the $C000-$FFFF ROM range) within a generous cycle budget. If the
// IWM gate regresses back to `isIIcPlus`-only, the boot stalls in F8
// and PC stays high.

#include "M6502.h"
#include "Memory.h"
#include "SystemProfile.h"
#include "SlotBus.h"
#include "DiskIICard.h"
#include "IWMDevice.h"
#include "Logger.h"

#include <cstdio>
#include <cstdlib>
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

}

int main()
{
    Memory mem;
    M6502  cpu(&mem);
    pom2::IWMDevice iwm;
    mem.setIWM(&iwm);
    mem.setIWMAuthoritative(true);

    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);

    const std::string romPath = firstExisting({"roms/apple2c-32Kv0.rom"});
    if (romPath.empty()) {
        std::printf("SKIP iic_boot_trace: roms/apple2c-32Kv0.rom not present\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }
    if (!mem.loadAppleIIRom(romPath.c_str(), /*pickLowerHalf=*/true)) {
        std::printf("FAIL: could not load %s\n", romPath.c_str());
        return 1;
    }

    auto card = std::make_unique<DiskIICard>(6);
    const std::string diskRom = firstExisting({"roms/disk2.rom"});
    if (!diskRom.empty()) card->loadBootRom(diskRom);
    const std::string lssRom = firstExisting({"roms/diskii_p6.rom"});
    if (!lssRom.empty()) card->loadLssRom(lssRom);
    const std::string disk = firstExisting({"disks_5.4/dsk/dos33_master.dsk"});
    if (disk.empty()) {
        std::printf("SKIP iic_boot_trace: disks_5.4/dsk/dos33_master.dsk not present\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }
    card->insertDisk(disk);
    card->setIWM(&iwm);
    mem.slotBus().plug(6, std::move(card));

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();

    // ~5M instructions covers the //c F8 self-test + slot-6 boot scan
    // + DOS 3.3 sector 0/1 reads. Pre-fix //c never escaped $C000-$FFFF
    // within 50M; post-fix DOS jumps out around 1-2M.
    constexpr int kInstrs = 5'000'000;
    bool sawC600 = false;
    bool escapedRom = false;
    for (int i = 0; i < kInstrs && !escapedRom; ++i) {
        const uint16_t pc = cpu.getProgramCounter();
        if (pc >= 0xC600 && pc < 0xC700) sawC600 = true;
        if (sawC600 && pc < 0xC000) escapedRom = true;
        cpu.step();
    }

    const uint16_t finalPc = cpu.getProgramCounter();
    std::printf("//c boot trace: sawC600=%d escapedRom=%d finalPC=$%04X\n",
                sawC600, escapedRom, finalPc);
    if (!sawC600) {
        std::printf("FAIL: //c never entered slot-6 $C600 boot routine\n");
        return 1;
    }
    if (!escapedRom) {
        std::printf("FAIL: //c boot stayed inside $C000-$FFFF ROM "
                    "(IWM not wired / boot stalled)\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
