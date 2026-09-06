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

// A //e runs the real Liron firmware over a real IWM, and boots a 3.5" disk
// from it — over the SmartPort BUS, which is what the firmware speaks.
//
// `LironCard` plugs the actual 4 KB EPROM, an `IWMDevice`, and Sony 3.5"
// mechanisms; nothing is served from the host through a substitute ROM
// (that is `SmartPortCard`'s job, and it stays). The plan in TODO § Storage
// was that with the //c+ booting, this card was small — "the risk was never
// the card". The card was indeed small. The risk was the DRIVE.
//
// What the firmware's device scan actually does, read off the dump with
// POM2's own disassembler at $C800:
//
//     JSR $CA05      ; LDA $C083,X — PH1 high
//                    ; LDA $C087,X — PH3/LSTRB high
//     JSR $CBA9      ; motor off, Q6, then mode := $07
//     LDA $C08B,X    ; SEL high
//     LDA $C089,X    ; motor on
//     LDY #$32
//   poll:
//     LDA $C08E,X    ; status; bit 7 is the SENSE line
//     BMI found
//     DEY / BNE poll ; fifty tries
//     SEC            ; → "no device connected" ($28)
//
// Holding **LSTRB high** through a status read is the giveaway: on a dumb
// Sony mechanism that line is the write strobe, never asserted while
// reading. This is the SmartPort *bus* handshake, and only an INTELLIGENT
// device — a UniDisk 3.5, which carries its own 65C02 — answers it. There is
// no fallback path in this dump's scan: the timeout decrements the device
// count and the scan ends, so ProDOS is told $28 and the //e drops to the
// monitor. POM2 does not emulate that 65C02; `SmartPortBusDevice` answers
// the protocol at the byte level instead, and this test is what says the
// answer is right: the firmware's INIT scan, then its boot READ, then
// ProDOS 8 on the text page.
//
// Two things about the card itself are still load-bearing and pinned below:
//   * the dump's per-slot page and ProDOS identity bytes reach the bus;
//   * the firmware's probe reaches the drive — motor enabled, SEL asserted.
//     Not a formality: the first version of the card forwarded phases only
//     to a *selected* drive, and the firmware sets the register address up
//     before it enables one, so every sense read answered for register 0
//     and the probe never even strobed.
// The head never moves: an intelligent drive seeks inside itself and the
// host sees blocks. `POM2_TRACE_SMARTPORT_BUS=1` prints every byte.

#include "Disk35Image.h"
#include "LironCard.h"
#include "M6502.h"
#include "Memory.h"
#include "ResourcePaths.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace {

constexpr int kSlot = 5;

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
    const std::string rom  = pom2::findResource("roms/apple2e.rom");
    const std::string disk =
        pom2::findResource("disks_3.5/A2DeskTop-1.5-en_800k.2mg");
    if (rom.empty() || disk.empty()) {
        std::printf("SKIP liron_boot35: need roms/apple2e.rom and "
                    "disks_3.5/A2DeskTop-1.5-en_800k.2mg\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    Memory mem;
    M6502  cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(true);
    mem.clearRam();
    mem.resetSoftSwitches();
    if (!mem.loadAppleIIRom(rom.c_str())) {
        std::printf("SKIP liron_boot35: cannot load %s\n", rom.c_str());
        return 77;   // ctest SKIP_RETURN_CODE
    }

    auto card = std::make_unique<pom2::LironCard>(kSlot);
    pom2::LironCard* liron = card.get();
    if (!liron->romLoaded()) {
        std::printf("SKIP liron_boot35: %s\n", liron->lastError().c_str());
        return 77;   // ctest SKIP_RETURN_CODE
    }
    std::string err;
    if (!liron->mountBay(0, disk, err)) {
        std::printf("SKIP liron_boot35: mount failed: %s\n", err.c_str());
        return 77;   // ctest SKIP_RETURN_CODE
    }
    mem.slotBus().plug(kSlot, std::move(card));

    int failures = 0;
    auto fail = [&](const char* what) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    };

    // ── The card announces itself the way ProDOS scans for it ────────────
    // Apple II Reference Manual appendix C: $Cn01/$Cn03/$Cn05 identify a
    // disk device, and $Cn07 = $00 marks the SmartPort class. These come
    // from the dump, so this is really a check that the per-slot page is
    // sliced at the right offset — an off-by-one page would still boot
    // *something* on a lucky slot number.
    const uint8_t sig1 = mem.memRead(static_cast<uint16_t>(0xC001 + kSlot * 256));
    const uint8_t sig3 = mem.memRead(static_cast<uint16_t>(0xC003 + kSlot * 256));
    const uint8_t sig5 = mem.memRead(static_cast<uint16_t>(0xC005 + kSlot * 256));
    const uint8_t sig7 = mem.memRead(static_cast<uint16_t>(0xC007 + kSlot * 256));
    if (sig1 != 0x20 || sig3 != 0x00 || sig5 != 0x03 || sig7 != 0x00) {
        std::printf("FAIL: slot ROM signature is $%02X/$%02X/$%02X/$%02X, "
                    "want $20/$00/$03/$00 — the dump's per-slot page is not "
                    "where the loader thinks\n", sig1, sig3, sig5, sig7);
        ++failures;
    }
    // And the page really is the one for THIS slot: the dump's pages differ
    // only in the `LDX #$0n` at offset 14.
    if (mem.memRead(static_cast<uint16_t>(0xC00F + kSlot * 256)) != kSlot)
        fail("the slot page carries the wrong slot number — LironCard is "
             "serving another slot's copy");

    const bool strict = std::getenv("POM2_LIRON_BOOT_STRICT") != nullptr;

    // ── Run it ───────────────────────────────────────────────────────────
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    cpu.setProgramCounter(static_cast<uint16_t>(0xC000 + kSlot * 256));

    constexpr long kBudget = 90'000'000;      // ~90 emulated seconds
    long total     = 0;
    long bootCycle = -1;
    bool motorSeen = false;
    bool side1Seen = false;
    int  maxTrack  = 0;
    std::string screen;
    while (total < kBudget) {
        total += cpu.run(4096);
        if (liron->drive(0).isMotorOn()) motorSeen = true;
        if (liron->drive(0).side1())     side1Seen = true;
        if (liron->drive(0).track() > maxTrack) maxTrack = liron->drive(0).track();
        if (bootCycle < 0 && (total % (1 << 20)) < 4096) {
            screen = scrapeTextPage(mem.data());
            if (screen.find("ProDOS 8") != std::string::npos) bootCycle = total;
        }
    }
    screen = scrapeTextPage(mem.data());

    // The probe reached the drive. Both of these come from the firmware's
    // own sequence above, so a regression in the card's wiring shows here
    // rather than as a silent "still does not boot".
    if (!motorSeen)
        fail("the drive's motor never ran — the firmware's $C089 never "
             "reached it (IWM devsel → LironCard::onDevsel)");
    if (!side1Seen)
        fail("SEL was never seen by the drive — $C08B is not reaching "
             "Sony35Drive::ssW, and on a Sony that line is also bit 3 of the "
             "register address, so every probe would answer for the wrong "
             "register");
    // Since 2026-09-01 the card's SmartPort bus responder answers the
    // firmware's packets, so the boot IS the assertion. The head never
    // moves: an intelligent drive seeks inside itself, the host only sees
    // blocks — `maxTrack` stays 0 by design and is reported, not asserted.
    if (bootCycle < 0 && screen.find("ProDOS 8") == std::string::npos)
        fail("ProDOS never reached the text page — the firmware found the "
             "device (motor + SEL above) but the bus exchange did not carry "
             "a boot; POM2_TRACE_SMARTPORT_BUS=1 shows where it stopped");
    (void)strict;

    if (failures) {
        std::printf("--- text page ---\n%s---\n", screen.c_str());
        std::printf("drive: motor=%d maxTrack=%d side1=%d ; IWM mode=$%02X "
                    "control=$%02X status=$%02X\n",
                    motorSeen ? 1 : 0, maxTrack, side1Seen ? 1 : 0,
                    liron->iwm().mode(), liron->iwm().control(),
                    liron->iwm().status());
        return 1;
    }
    std::printf("liron_boot35: OK — ProDOS 8 booted from a Liron card in slot "
                "%d at %ld cycles, over the SmartPort bus (%d blocks read)\n",
                kSlot, bootCycle, liron->busProgress().blocksRead);
    return 0;
}
