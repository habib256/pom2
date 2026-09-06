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
// The //c's external 3.5" drive, driven by the //c's OWN firmware.
//
// A plain 32 KB //c has one disk port: the internal 5.25" and the rear
// connector share the IWM. Its bank-0 $C500 page is the controller firmware
// (the Liron's, byte for byte) and its bank 1 the SmartPort bus code; an
// external UniDisk 3.5 is an INTELLIGENT device that firmware talks to in
// packets. Until 2026-09-01 nothing in POM2 answered those packets, so the
// firmware reported $28 and ProDOS never saw slot 5 — a mounted 3.5" was
// invisible from a 5.25" boot, and the only way to reach it was to punch a
// host-served page over $C500, which the //c's own boot (`JSR $C5F8` into
// that page) did not survive.
//
// `IIcExternalSmartPort` answers the bus for the units of whatever card sits
// in slot 5, and `Memory` leaves the real $C500 alone while it does. What
// this pins, each of which is one user-visible thing:
//
//   A. boot the internal 5.25" with a 3.5" mounted: ProDOS comes up AND its
//      device list holds slot 5 (both units) next to slot 6 — the drive is
//      a data volume, found by the firmware's enumeration, not by a stub;
//   B. boot slot 5 explicitly (what the GUI's Boot does — `bootFromSlot`):
//      ProDOS 8 comes up off the 3.5", through the real firmware;
//   C. nothing mounted on slot 5: silence on the wire, no slot-5 device,
//      and the 5.25" boot is exactly what it was.
//
// The DiskIICard stays the 5.25" controller throughout — the port carries its
// own register tracker and claims only the bus's accesses. Pinned separately
// by `iic_diskii_no_iwm_conflict`.
//
// Then the //c+, whose shared IWM carries the bus and whose firmware numbers
// the external chain from 2 (its MIG drive is device 1):
//   D. empty internal bay, 3.5" on the rear port: the boot scan finds it and
//      ProDOS 8 comes up off it over the bus;
//   E. internal 3.5" boots (iicplus_boot35's case) with an external device
//      present: ProDOS lists both slot-5 units.

#include "DiskIICard.h"
#include "IIcExternalSmartPort.h"
#include "IWMDevice.h"
#include "M6502.h"
#include "Memory.h"
#include "ResourcePaths.h"
#include "SlotBus.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "SmartPortHub.h"
#include "Sony35Drive.h"
#include "Disk35Image.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
void fail(const char* what) { std::printf("FAIL: %s\n", what); ++g_failures; }

std::string scrapeTextPage(Memory& mem)
{
    static const int rowBase[24] = {
        0x400,0x480,0x500,0x580,0x600,0x680,0x700,0x780,
        0x428,0x4A8,0x528,0x5A8,0x628,0x6A8,0x728,0x7A8,
        0x450,0x4D0,0x550,0x5D0,0x650,0x6D0,0x750,0x7D0 };
    std::string s;
    for (int r = 0; r < 24; ++r) {
        for (int c = 0; c < 40; ++c) {
            const uint8_t b = mem.memRead(static_cast<uint16_t>(rowBase[r] + c)) & 0x7F;
            s += (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : ' ';
        }
        s += '\n';
    }
    return s;
}

struct Outcome {
    uint64_t diskIIFlushes = 0;      // bus traffic must never become 5.25" flux
    bool     diskIIDirty   = false;
    bool     plusBootRule  = false;  // Memory::iicPlusBootsSlot5ByReset()
    bool prodosSeen = false;
    bool slot5Drive1 = false, slot5Drive2 = false;
    bool slot6Drive1 = false;
    int  devcnt = -1;
    pom2::SmartPortBusDevice::Progress bus;
    std::string screen;
};

Outcome run(const std::string& rom, const std::string& disk525,
            const std::string& disk35, bool bootSlot5)
{
    Outcome o;
    Memory mem;
    M6502  cpu(&mem);
    pom2::IWMDevice iwm;                 // the machine's shared IWM (//c+ path)
    mem.setCpu(&cpu);
    mem.setIWM(&iwm);
    mem.setIWMAuthoritative(true);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    pom2::IIcExternalSmartPort port(&mem.slotBus());
    mem.setExternalSmartPort(&port);
    if (!mem.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/true)) return o;

    auto d2 = std::make_unique<DiskIICard>(6);
    DiskIICard* diskII = d2.get();
    const std::string bootRom = pom2::findResource("roms/disk2.rom");
    const std::string lssRom  = pom2::findResource("roms/diskii_p6.rom");
    if (!bootRom.empty()) d2->loadBootRom(bootRom);
    if (!lssRom.empty())  d2->loadLssRom(lssRom);
    d2->insertDisk(disk525);
    // Drive 2 holds a writable scratch copy with write-back ON: on a real //c
    // the bus traffic selects drive 2 (the rear connector) and enables the
    // motor — bytes the Disk II must never see as flux.
    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path() / "pom2_iic_ext_sp_d2.po";
    std::error_code ec;
    std::filesystem::copy_file(disk525, scratch,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (!ec) d2->insertDisk(1, scratch.string());
    d2->setWriteBackEnabled(true);
    d2->setIWM(&iwm);
    mem.slotBus().plug(6, std::move(d2));

    auto sp = std::make_unique<pom2::SmartPortCard>(5);
    sp->setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
    sp->setUnit(1, std::make_unique<pom2::SmartPort35Unit>());
    if (!disk35.empty()) {
        std::string err;
        if (!sp->mountBay(0, disk35, err)) std::printf("mount: %s\n", err.c_str());
    }
    mem.slotBus().plug(5, std::move(sp));

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    if (bootSlot5) {
        mem.setIicSmartPortArmed(true);          // what bootFromSlot does…
        cpu.setProgramCounter(0xC500);           // …and where it jumps
    }

    constexpr long kBudget = 60'000'000;         // cycles
    for (long total = 0; total < kBudget; ) {
        total += cpu.run(4096);
        if (!o.prodosSeen && (total % (1 << 20)) < 4096) {
            if (scrapeTextPage(mem).find("ProDOS") != std::string::npos)
                o.prodosSeen = true;
        }
    }
    o.screen = scrapeTextPage(mem);
    if (!o.prodosSeen && o.screen.find("ProDOS") != std::string::npos)
        o.prodosSeen = true;

    // ProDOS 8 global page: DEVCNT = number of devices - 1, DEVLST follows.
    const uint8_t devcnt = mem.memRead(0xBF31);
    o.devcnt = devcnt;
    if (devcnt < 14) {
        for (int i = 0; i <= devcnt; ++i) {
            const uint8_t d = mem.memRead(static_cast<uint16_t>(0xBF32 + i));
            const int  slot = (d >> 4) & 7;
            const bool dr2  = (d & 0x80) != 0;
            if (slot == 5 && !dr2) o.slot5Drive1 = true;
            if (slot == 5 &&  dr2) o.slot5Drive2 = true;
            if (slot == 6 && !dr2) o.slot6Drive1 = true;
        }
    }
    o.bus = port.device().progress();
    o.diskIIFlushes = diskII->getWriteFlushCount();
    o.diskIIDirty   = diskII->hasUnsavedChanges(0) || diskII->hasUnsavedChanges(1);
    o.plusBootRule  = mem.iicPlusBootsSlot5ByReset();
    return o;
}

// The //c+: the machine's shared IWM carries the bus (MIG-routed Sony
// drives on the same chip), so the port rides along instead of tracking
// registers of its own. `internal35` fills the internal bay the way
// iicplus_boot35 does; `disk35` goes on the slot-5 card = the rear port.
Outcome runPlus(const std::string& rom, const std::string& internal35,
                const std::string& disk35, bool bootSlot5)
{
    Outcome o;
    Memory mem;
    M6502  cpu(&mem);
    mem.setCpu(&cpu);
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
    pom2::IIcExternalSmartPort port(&mem.slotBus());
    mem.setExternalSmartPort(&port);
    auto d2 = std::make_unique<DiskIICard>(6);
    DiskIICard* diskII = d2.get();
    {
        // A writable 5.25" in drive 1 with write-back on: on the //c+ the
        // bus traffic shares this IWM, and the Disk II must not see it.
        const std::string disk525 = pom2::findResource("disks_5.4/dsk/ProDOS_2_4_3.po");
        const std::filesystem::path scratch =
            std::filesystem::temp_directory_path() / "pom2_iicp_ext_sp_d1.po";
        std::error_code ec;
        if (!disk525.empty())
            std::filesystem::copy_file(disk525, scratch,
                                       std::filesystem::copy_options::overwrite_existing, ec);
        if (!disk525.empty() && !ec) d2->insertDisk(0, scratch.string());
        d2->setWriteBackEnabled(true);
    }
    mem.slotBus().plug(6, std::move(d2));
    auto sp = std::make_unique<pom2::SmartPortCard>(5);
    sp->setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
    sp->setUnit(1, std::make_unique<pom2::SmartPort35Unit>());
    if (!disk35.empty()) {
        std::string err;
        if (!sp->mountBay(0, disk35, err)) std::printf("mount: %s\n", err.c_str());
    }
    mem.slotBus().plug(5, std::move(sp));
    mem.setIIEMode(true);
    mem.clearRam();
    mem.resetSoftSwitches();
    if (!mem.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/true)) return o;
    if (!internal35.empty()) {
        if (imgInt.loadFile(internal35)) drvInt.notifyMediaChange();
    }
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    if (bootSlot5) { mem.setIicSmartPortArmed(true); cpu.setProgramCounter(0xC500); }

    constexpr long kBudget = 60'000'000;
    for (long total = 0; total < kBudget; ) {
        total += cpu.run(4096);
        if (!o.prodosSeen && (total % (1 << 20)) < 4096) {
            if (scrapeTextPage(mem).find("ProDOS") != std::string::npos)
                o.prodosSeen = true;
        }
    }
    o.screen = scrapeTextPage(mem);
    if (!o.prodosSeen && o.screen.find("ProDOS") != std::string::npos)
        o.prodosSeen = true;
    const uint8_t devcnt = mem.memRead(0xBF31);
    o.devcnt = devcnt;
    if (devcnt < 14) {
        for (int i = 0; i <= devcnt; ++i) {
            const uint8_t d = mem.memRead(static_cast<uint16_t>(0xBF32 + i));
            const int  slot = (d >> 4) & 7;
            const bool dr2  = (d & 0x80) != 0;
            if (slot == 5 && !dr2) o.slot5Drive1 = true;
            if (slot == 5 &&  dr2) o.slot5Drive2 = true;
            if (slot == 6 && !dr2) o.slot6Drive1 = true;
        }
    }
    o.bus = port.device().progress();
    o.diskIIFlushes = diskII->getWriteFlushCount();
    o.diskIIDirty   = diskII->hasUnsavedChanges(0) || diskII->hasUnsavedChanges(1);
    o.plusBootRule  = mem.iicPlusBootsSlot5ByReset();
    return o;
}

void dump(const char* label, const Outcome& o)
{
    std::printf("--- %s: prodos=%d devcnt=%d S5D1=%d S5D2=%d S6D1=%d bus{tx=%d "
                "read=%d cmd=$%02X} diskII{flushes=%llu dirty=%d} plusRule=%d\n",
                label, o.prodosSeen ? 1 : 0, o.devcnt,
                o.slot5Drive1 ? 1 : 0, o.slot5Drive2 ? 1 : 0,
                o.slot6Drive1 ? 1 : 0, o.bus.transactions, o.bus.blocksRead,
                o.bus.commandByte, static_cast<unsigned long long>(o.diskIIFlushes),
                o.diskIIDirty ? 1 : 0, o.plusBootRule ? 1 : 0);
    std::fflush(stdout);   // keep the dumps in order with a stderr bus trace
}

}  // namespace

int main()
{
    const std::string rom    = pom2::findResource("roms/apple2c-32Kv0.rom");
    const std::string disk525 = pom2::findResource("disks_5.4/dsk/ProDOS_2_4_3.po");
    const std::string disk35  = pom2::findResource("disks_3.5/A2DeskTop-1.5-en_800k.2mg");
    if (rom.empty() || disk525.empty() || disk35.empty()) {
        std::printf("SKIP iic_external_smartport: need roms/apple2c-32Kv0.rom, "
                    "disks_5.4/dsk/ProDOS_2_4_3.po and an 800K image\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    // A. Internal 5.25" boots; the external 3.5" is a data volume.
    {
        const Outcome o = run(rom, disk525, disk35, /*bootSlot5=*/false);
        dump("A 5.25 boot + 3.5 mounted", o);
        if (!o.prodosSeen)  fail("A: ProDOS did not boot from the internal 5.25\"");
        if (!o.slot6Drive1) fail("A: slot 6 drive 1 missing from ProDOS's device list");
        if (!o.slot5Drive1) fail("A: slot 5 drive 1 missing — the firmware's "
                                 "enumeration did not find the external drive");
        if (!o.slot5Drive2) fail("A: slot 5 drive 2 missing — the chain has two units");
        if (o.bus.transactions == 0)
            fail("A: no SmartPort bus transaction — the firmware never spoke to the port");
        if (o.diskIIFlushes || o.diskIIDirty)
            fail("A: the Disk II wrote flux during bus traffic — packet bytes reached "
                 "the slot-6 card (Memory must drop a write the port claimed)");
        if (o.plusBootRule) fail("A: the //c+ reset-boot rule fired on a plain //c");
        if (g_failures) std::printf("%s", o.screen.c_str());
    }
    // B. Boot the 3.5" through the real firmware.
    {
        const Outcome o = run(rom, disk525, disk35, /*bootSlot5=*/true);
        dump("B boot slot 5", o);
        if (!o.prodosSeen) fail("B: ProDOS 8 never reached the text page booting slot 5");
        if (o.bus.blocksRead == 0) fail("B: no block was read over the bus");
        if (o.diskIIFlushes || o.diskIIDirty)
            fail("B: the Disk II wrote flux during the 3.5\" boot");
        if (g_failures) std::printf("%s", o.screen.c_str());
    }
    // C. Nothing on the port: silence, and the 5.25" boot is untouched.
    {
        const Outcome o = run(rom, disk525, /*disk35=*/"", /*bootSlot5=*/false);
        dump("C 5.25 boot, empty port", o);
        if (!o.prodosSeen)  fail("C: ProDOS did not boot with an empty port");
        if (o.slot5Drive1 || o.slot5Drive2)
            fail("C: a slot-5 device appeared with nothing mounted");
        if (o.bus.commandTaken)
            fail("C: the port took a command with no media — an empty chain "
                 "must be silence, or the firmware boots an absent disk");
        // With nothing on the port the firmware's probe still drives the IWM
        // lines behind $C0E0-$C0EF; none of it may reach the 5.25" as flux.
        // (A 2026-07-30 write-back left 20 bytes of sync garbage over a data
        // prologue of a DOS 3.3 .nib at track 0 — Best1a.nib, restored from
        // git; whatever wrote it, this is the rule it broke.)
        if (o.diskIIFlushes || o.diskIIDirty)
            fail("C: the Disk II wrote flux with an empty port — firmware probe "
                 "bytes reached the slot-6 card");
        if (g_failures) std::printf("%s", o.screen.c_str());
    }

    // ── The //c+ ─────────────────────────────────────────────────────────
    const std::string romPlus = pom2::findResource("roms/apple2cp.rom");
    if (romPlus.empty()) {
        std::printf("NOTE: no roms/apple2cp.rom — the //c+ half is skipped\n");
    } else {
        const std::string psDisk =
            pom2::findResource("disks_3.5/The New Print Shop 800K.po");
        // D. Nothing in the internal bay, an 800K on the rear port: the
        //    firmware's boot scan finds the external device and boots it.
        {
            const Outcome o = runPlus(romPlus, "", disk35, /*bootSlot5=*/false);
            dump("D //c+ empty bay + external 3.5, cold boot", o);
            if (!o.prodosSeen)
                fail("D: the //c+ did not boot the external 3.5\" — its $F223 "
                     "scan found no device on the port");
            if (o.bus.blocksRead == 0) fail("D: no block was read over the bus");
            if (o.diskIIFlushes || o.diskIIDirty)
                fail("D: the //c+'s Disk II wrote flux during bus traffic");
            if (!o.plusBootRule)
                fail("D: Memory::iicPlusBootsSlot5ByReset() is false with a live port — "
                     "bootFromSlot(5) would jump into the real $C500 and fail");
            if (g_failures) std::printf("%s", o.screen.c_str());
        }
        // E. Internal 3.5" boots (the MIG path, iicplus_boot35's case) with an
        //    external device present: both must end up in ProDOS's list.
        if (!psDisk.empty()) {
            const Outcome o = runPlus(romPlus, disk35, psDisk, /*bootSlot5=*/false);
            dump("E //c+ internal boot + external 3.5", o);
            if (!o.prodosSeen) fail("E: ProDOS did not boot from the internal bay");
            if (!o.slot5Drive1 || !o.slot5Drive2)
                fail("E: ProDOS lists fewer than two slot-5 units — the external "
                     "drive was not enumerated next to the internal one");
            if (o.bus.transactions == 0)
                fail("E: the firmware never spoke to the rear port");
            if (o.diskIIFlushes || o.diskIIDirty)
                fail("E: the //c+'s boot disk in drive 1 took flux from the bus");
            if (g_failures) std::printf("%s", o.screen.c_str());
        }
    }

    if (g_failures) return 1;
    std::printf("iic_external_smartport: OK — the //c's own firmware enumerates "
                "and boots the external 3.5\" over the SmartPort bus, next to "
                "the internal 5.25\"\n");
    return 0;
}
