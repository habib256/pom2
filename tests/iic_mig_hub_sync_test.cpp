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

// //c-class: the MIG's drive-select state is ONE state, and the //c-only
// $C0xx status readbacks answer instead of floating.
//
//  1. $C028 ROMSWITCH →bank 0 clears the MIG's "internal 3.5" enabled" bit.
//     MAME `apple2e.cpp:1939-1944` sets `m_migpage = 0; m_intdrive = false;
//     m_35sel = false;` — one variable each. POM2 keeps intdrive twice:
//     IIcClassProfile::migIntDrive_ (what the snapshot serialises) and
//     SmartPortHub::intDrive_ (what routes the IWM to a drive). Only the
//     hub's copy was cleared, so every $C028 toggle back to bank 0 left the
//     profile claiming a drive the hub had already dropped — and the
//     snapshot recorded the stale claim.
//
//  2. Restoring a MIG blob pushes intdrive + head-select back into the hub.
//     SmartPortHub has no snapshot section of its own (it is derived state,
//     the way MAME recomputes recalc_active_device after a state load), so
//     restoring the profile's copy alone left the halves disagreeing. Worse,
//     `SmartPortHub::setMigIntDrive` early-returns on an unchanged value, so
//     the guest's next identical MIG write could never resync it: a //c+
//     snapshot or rewind taken with the internal 3.5" selected came back
//     with `hub.active35() == nullptr` and the drive unreachable.
//
//  3. RDVBLMSK ($C041) — MAME `c000_iic_r` `apple2e.cpp:2312-2313`:
//     `(m_vblmask ? 0x80 : 0x00) | uFloatingBus7`, //c-class only. POM2
//     returned the raw floating bus, so a //c program that armed its frame
//     interrupt with $C05B and read the mask back got bit 7 of the video
//     scanner byte.
//
//  4. RDDHIRES ($C079/$C07B/$C07D/$C07F) — MAME `apple2e.cpp:2341-2343`:
//     `(get_dhires() ? 0x00 : 0x80) | uFloatingBus7`, //c-class only, and
//     bit 7 is the INVERSE of DHGR (the IOU latches AN3). Same failure: the
//     raw bus decided the answer.
//
// A plain //e must keep falling through to the bus for 3 and 4 — `c000_r`
// has no case for either, and the IIe Technical Reference's listing of
// RDIOUDIS/RDDHIRES is the documented error MAME calls out at :2255.

#include "IWMDevice.h"
#include "M6502.h"
#include "Memory.h"
#include "MemoryProfile_IIcClass.h"
#include "SmartPortHub.h"
#include "Sony35Drive.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool cond, const std::string& what)
{
    if (!cond) { std::printf("FAIL: %s\n", what.c_str()); ++failures; }
}

constexpr size_t kMigBytes = 4 + 2 + 0x800;   // magic + page + RAM
constexpr size_t kIntDrive = kMigBytes + 1;   // tail: romBank, intDrive, hdSel

std::string firstExisting(const std::string& rel)
{
    namespace fs = std::filesystem;
    for (const std::string& p : { rel, "../" + rel, "../../" + rel })
        if (fs::exists(p)) return p;
    return {};
}

// A //c+ MIG rig: profile + hub + IWM + the two Sony 3.5" mechanisms.
struct Rig {
    std::vector<uint8_t> rom, alt;
    pom2::IWMDevice      iwm;
    pom2::SmartPortHub   hub;
    pom2::Sony35Drive    internal, external;
    IIcClassProfile      prof;

    Rig()
        : rom(0x4000, 0x00), alt(0x4000, 0x11),
          prof((rom[0x3bbf] = 0x05, rom.data()), rom.size(), alt.data(),
               &iwm, &hub, true)
    {
        hub.attach(&iwm);
        hub.setSony35(&internal, &external);
    }

    // Motor on + drive-2 select: the IWM fires devsel(2) into the hub, which
    // is the state the //c+ firmware is in while it talks to the MIG.
    void selectDrive2()
    {
        iwm.tick(1000);
        iwm.write(0x9, 0);     // motor on
        iwm.write(0xB, 0);     // drive select = 2
        iwm.tick(2000);
    }
    uint8_t blobIntDrive()
    {
        std::vector<uint8_t> b;
        prof.appendSnapshotState(b);
        return b.size() > kIntDrive ? b[kIntDrive] : 0xFF;
    }
};

// ── 1. the $C028 →bank-0 edge clears BOTH copies ──────────────────────
void testRomSwitchClearsIntDrive()
{
    Rig r;
    r.selectDrive2();
    r.prof.romBankToggle();                       // → bank 1
    r.prof.internalRomWrite(0xCC80, 0x00);        // MIG $080: intdrive = on
    expect(r.hub.active35() == &r.internal,
           "MIG enable did not route the hub to the internal 3.5\"");
    expect(r.blobIntDrive() == 1, "MIG enable did not set the profile copy");

    r.prof.romBankToggle();                       // → bank 0: MIG resets
    expect(r.hub.active35() == nullptr,
           "$C028 →bank0 left the hub routed to a drive");
    expect(r.blobIntDrive() == 0,
           "$C028 →bank0 left migIntDrive_ set (MAME apple2e.cpp:1942)");
}

// ── 2. a restored blob reaches the hub ────────────────────────────────
void testRestorePushesToHub()
{
    Rig a;
    a.selectDrive2();
    a.prof.romBankToggle();
    a.prof.internalRomWrite(0xCC80, 0x00);        // intdrive = on
    a.prof.internalRomWrite(0xCC60, 0x00);        // MIG $060: head select 1
    std::vector<uint8_t> blob;
    a.prof.appendSnapshotState(blob);
    expect(a.hub.active35() == &a.internal, "source rig is not on the drive");

    Rig b;                                        // fresh: intDrive_ = false
    b.selectDrive2();
    expect(b.hub.active35() == nullptr, "fresh rig already routed");
    const size_t used = b.prof.loadSnapshotState(blob.data(), blob.size());
    expect(used >= kMigBytes, "MIG blob was rejected");
    expect(b.blobIntDrive() == 1, "restore lost the profile copy");
    expect(b.hub.active35() == &b.internal,
           "restore did not push migIntDrive_ into the SmartPortHub");
}

// ── 3 + 4. the //c-only status readbacks ──────────────────────────────
int testStatusReads()
{
    int tested = 0;
    for (const std::string& cand : { std::string("roms/apple2c-32Kv0.rom"),
                                     std::string("roms/apple2cp.rom") }) {
        const std::string rom = firstExisting(cand);
        if (rom.empty()) continue;
        Memory mem; M6502 cpu(&mem);
        mem.setCpu(&cpu); mem.clearRam(); mem.setIIEMode(true);
        if (!mem.loadAppleIIRom(rom.c_str(), /*pickLower16KFor32K=*/true))
            continue;
        ++tested;
        mem.resetSoftSwitches();
        // Park the scanner on a byte with bit 7 CLEAR, so "bit 7 = the bus"
        // and "bit 7 = the register" are distinguishable in both directions.
        for (uint16_t a = 0x0400; a < 0x0800; ++a) mem.memWrite(a, 0x6D);
        mem.setCycleCounter(25);
        const uint8_t bus7 = static_cast<uint8_t>(mem.peekFloatingBus() & 0x7F);
        expect(bus7 == 0x6D, rom + ": parked floating bus is not $6D");

        // RDDHIRES. IOUDIS set so $C05E/$C05F are the display switches.
        mem.memWrite(0xC07E, 0);                  // SETIOUDIS
        (void)mem.memRead(0xC05E);                // SETDHIRES
        for (uint16_t a : { 0xC079, 0xC07B, 0xC07D, 0xC07F })
            expect(mem.memRead(a) == bus7,
                   rom + ": RDDHIRES with DHGR on — wrong value");
        (void)mem.memRead(0xC05F);                // CLRDHIRES
        for (uint16_t a : { 0xC079, 0xC07B, 0xC07D, 0xC07F })
            expect(mem.memRead(a) == static_cast<uint8_t>(0x80 | bus7),
                   rom + ": RDDHIRES with DHGR off — wrong value");

        // RDVBLMSK, armed through the real IOU decode (IOUDIS clear).
        mem.memWrite(0xC07F, 0);                  // CLRIOUDIS
        (void)mem.memRead(0xC05B);                // EnVBL
        expect(mem.memRead(0xC041) == static_cast<uint8_t>(0x80 | bus7),
               rom + ": RDVBLMSK did not report the armed mask");
        (void)mem.memRead(0xC05A);                // DisVBL
        expect(mem.memRead(0xC041) == bus7,
               rom + ": RDVBLMSK did not report the cleared mask");
    }

    // A plain //e answers neither: no IOU, so both fall out to the bus.
    const std::string iie = firstExisting("roms/apple2e.rom");
    if (!iie.empty()) {
        Memory mem; M6502 cpu(&mem);
        mem.setCpu(&cpu); mem.clearRam(); mem.setIIEMode(true);
        if (mem.loadAppleIIRom(iie.c_str(), true)) {
            ++tested;
            mem.resetSoftSwitches();
            for (uint16_t a = 0x0400; a < 0x0800; ++a) mem.memWrite(a, 0x6D);
            mem.setCycleCounter(25);
            const uint8_t bus = mem.peekFloatingBus();
            (void)mem.memRead(0xC05F);            // CLRDHIRES → DHGR off
            for (uint16_t a : { 0xC079, 0xC07B, 0xC07D, 0xC07F })
                expect(mem.memRead(a) == bus, "//e: $C0" +
                       std::to_string(a & 0xFF) + " answered RDDHIRES");
            expect(mem.memRead(0xC041) == bus, "//e: $C041 answered RDVBLMSK");
        }
    }
    return tested;
}

}  // namespace

int main()
{
    testRomSwitchClearsIntDrive();
    testRestorePushesToHub();
    const int tested = testStatusReads();
    if (failures) {
        std::printf("iic_mig_hub_sync FAILED (%d)\n", failures);
        return 1;
    }
    if (tested == 0) {
        std::printf("iic_mig_hub_sync: MIG half OK, status half SKIPPED "
                    "(no //c ROM present)\n");
        return 0;   // the MIG half needs no ROM and did run
    }
    std::printf("iic_mig_hub_sync OK (%d ROM%s)\n", tested,
                tested == 1 ? "" : "s");
    return 0;
}
