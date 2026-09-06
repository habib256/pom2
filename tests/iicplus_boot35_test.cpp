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

// The //c+ boots an 800K 3.5" disk with its OWN firmware.
//
// Not through POM2's host-served SmartPort block device at slot 5 — that
// substitute has worked for a year and is what `iic_onboard_smartport_smoke`
// pins. This is the real path, the one TODO § Storage called "the largest
// remaining fidelity gap in the storage stack": the //c+ ROM drives the MIG
// gate array, the MIG selects the drive, the IWM walks the bit cells, and the
// Sony 3.5" drive turns its motor and steps its head, all of it firmware-
// driven, with nothing in the machine but the disk in the internal bay.
//
// It did not work until 2026-09-01, and the failure was not in the firmware
// or the MIG: the IWM's bit-cell walker could not recover a sector (see
// `sony35_iwm_read_path` and the CHANGELOG entries for that day). Fixing the
// controller's time base and its flux query turned this exact test from
//
//     UNABLE TO FIND A BOOTABLE DISK ONLINE.
//
// into ProDOS 8 booting off the Sony drive. Both halves were run on the same
// harness against the same image; the diff between them was the controller.
//
// What is asserted, and why each one:
//   * ProDOS's boot banner reaches the text page — the machine did not merely
//     read *some* bytes, it read the boot block, loaded the loader, and the
//     loader ran.
//   * The firmware's own "no bootable disk" message does NOT appear. Without
//     this, a future regression that boots something else (or nothing) while
//     the screen happens to hold stale text would still pass.
//   * The motor ran and the head left track 0. A boot that somehow came from
//     anywhere but the Sony drive would not move it — and there is nothing
//     else mounted.

#include "Disk35Image.h"
#include "DiskIICard.h"
#include "IWMDevice.h"
#include "M6502.h"
#include "Memory.h"
#include "ResourcePaths.h"
#include "SmartPortHub.h"
#include "Sony35Drive.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace {

/// Apple II text page 1: row Y at $0400 + $80*(Y%8) + $28*(Y/8).
std::string scrapeTextPage(const uint8_t* ram)
{
    std::string out;
    for (int row = 0; row < 24; ++row) {
        const int base = 0x0400 + 0x80 * (row % 8) + 0x28 * (row / 8);
        for (int col = 0; col < 40; ++col) {
            const char c = static_cast<char>(ram[base + col] & 0x7F);
            out.push_back((c >= 0x20 && c < 0x7F) ? c : ' ');
        }
        out.push_back('\n');
    }
    return out;
}

}  // namespace

int main()
{
    const std::string rom  = pom2::findResource("roms/apple2cp.rom");
    const std::string disk =
        pom2::findResource("disks_3.5/A2DeskTop-1.5-en_800k.2mg");
    if (rom.empty() || disk.empty()) {
        std::printf("SKIP iicplus_boot35: need roms/apple2cp.rom and "
                    "disks_3.5/A2DeskTop-1.5-en_800k.2mg\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    Memory mem;
    M6502  cpu(&mem);
    mem.setCpu(&cpu);

    // The 3.5" stack, wired the way EmulationController wires it.
    pom2::IWMDevice    iwm;
    pom2::SmartPortHub hub;
    pom2::Disk35Image  imgInt, imgExt;
    pom2::Sony35Drive  drvInt, drvExt;
    drvInt.setImage(&imgInt);
    drvExt.setImage(&imgExt);
    hub.attach(&iwm);
    hub.setSony35(&drvInt, &drvExt);
    mem.setIWM(&iwm);
    mem.setSmartPortHub(&hub);

    // The //c+'s on-board 5.25" drive. Load-bearing even with no 5.25" media:
    // `IIcClassProfile::ioReadIWM` hands the CPU the IWM's byte only while a
    // 3.5" Sony is selected and falls through to this card otherwise. With no
    // card the fall-through returns the floating bus — $FF, whose bit 5 reads
    // as "drive enabled" — and the firmware takes a branch the real machine
    // never takes. (That cost an hour of reading the wrong loop.)
    mem.slotBus().plug(6, std::make_unique<DiskIICard>(6));

    mem.setIIEMode(true);
    mem.clearRam();
    mem.resetSoftSwitches();
    // `pickLower16KFor32K = true`: a //c-class 32 KB dump is two firmware
    // banks, bank 0 low. The //e layout is the opposite slicing of the same
    // file size, and getting it wrong runs the CPU through the char ROM.
    if (!mem.loadAppleIIRom(rom.c_str(), /*pickLower16KFor32K=*/true)) {
        std::printf("SKIP iicplus_boot35: cannot load %s\n", rom.c_str());
        return 77;   // ctest SKIP_RETURN_CODE
    }
    if (!imgInt.loadFile(disk)) {
        std::printf("SKIP iicplus_boot35: cannot load %s\n", disk.c_str());
        return 77;   // ctest SKIP_RETURN_CODE
    }
    drvInt.notifyMediaChange();
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();

    // ~90 emulated seconds at 1 MHz. ProDOS appears around 40 M on this
    // machine; the margin is for a slower host or a slightly different
    // firmware path, and the whole run is about 1.5 s of wall clock.
    constexpr long kBudget = 90'000'000;
    long total     = 0;
    long bootCycle = -1;
    bool motorSeen = false;
    int  maxTrack  = 0;
    std::string screen;
    while (total < kBudget) {
        total += cpu.run(4096);
        // EmulationController ticks the IWM once per frame so its drive-
        // disable timer drains even when the firmware stops poking $C0Ex.
        iwm.tick(mem.getCycleCounter());
        if (drvInt.isMotorOn())        motorSeen = true;
        if (drvInt.track() > maxTrack) maxTrack  = drvInt.track();
        if (bootCycle < 0 && (total % (1 << 20)) < 4096) {
            screen = scrapeTextPage(mem.data());
            if (screen.find("ProDOS 8") != std::string::npos) bootCycle = total;
        }
    }
    screen = scrapeTextPage(mem.data());

    int failures = 0;
    auto fail = [&](const char* what) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    };

    if (screen.find("UNABLE TO FIND A BOOTABLE DISK") != std::string::npos)
        fail("the //c+ firmware reported no bootable disk — the 3.5\" read "
             "path is broken again (run sony35_iwm_read_path first: it tells "
             "you whether the controller or the format regressed)");
    if (bootCycle < 0 && screen.find("ProDOS 8") == std::string::npos)
        fail("ProDOS never reached the text page");
    if (!motorSeen)
        fail("the Sony drive's motor never ran — the firmware never got as "
             "far as selecting the drive (MIG / IWM handshake)");
    if (maxTrack == 0)
        fail("the head never left track 0 — the firmware read no catalogue, "
             "so at best it read the boot block");

    if (failures) {
        std::printf("--- text page ---\n%s---\n", screen.c_str());
        std::printf("drive: motor=%d maxTrack=%d ; IWM mode=$%02X "
                    "control=$%02X status=$%02X ; 3.5\" selected=%d\n",
                    motorSeen ? 1 : 0, maxTrack, iwm.mode(), iwm.control(),
                    iwm.status(), hub.active35Selected() ? 1 : 0);
        return 1;
    }
    std::printf("iicplus_boot35: OK — ProDOS 8 booted from the internal Sony "
                "3.5\" at %ld cycles, head reached track %d\n",
                bootCycle, maxTrack);
    return 0;
}
