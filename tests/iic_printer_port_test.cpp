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

// //c printer port → ImageWriter — regression test.
//
// The //c has no physical slots, so its built-in SSC printer port (slot 1,
// tap armed by default in plugSlotsFromSettings) is the ONLY route from a
// //c-class guest to the host ImageWriter. Nothing else can be plugged.
//
// The //c's built-in printer firmware is not the SSC card's synthetic
// `PR#n` ROM — on a //c, $C100 is internal ROM — and it gates every
// character on the 6551 status register, spinning until the masked value
// `status & (SR_DCD|SR_TDRE)` reads "carrier present, transmitter empty".
// SuperSerialCard reported DCD/DSR INACTIVE whenever no telnet client was
// attached, which is right for the modem port (MAME `mos6551.cpp:37-39`
// inits `m_dsr(1), m_dcd(1)`; AppleWin `SerialComms.cpp:864` returns
// ST_DSR|ST_DCD "when nothing is attached") but wrong at an armed printer
// tap: an ImageWriter cabled to the port IS a DCE sitting there, and a
// printer has no carrier to acquire. The guest hung inside the firmware on
// `PR#1`, and no byte ever reached the spool — on ALL THREE //c profiles,
// with no workaround, because no alternative card is pluggable.
//
// That is the regression this pins. It shipped between two dated entries:
// CHANGELOG 2026-07-28 ("The //c prints for real") was undone on
// 2026-07-30 by the DCD/DSR polarity correction — itself a legitimate fix,
// so the guard has to be the printer tap, not a revert. `deviceAttached()`
// (= telnet peer OR tapped printer) is what the pins now answer to.
//
// Why the existing ssc_acia_smoke did not catch it: it exercises the tap
// against the CARD's synthetic PR#n ROM, which only checks TDRE and is
// therefore blind to DCD. Nothing booted a //c profile and ran PR#1
// through to the spool.
//
// What this pins:
//   * status contract — an armed printer tap must read "device present"
//     with no telnet client, and an idle port must still read "nothing
//     attached" (MAME/AppleWin parity for the modem side is preserved);
//   * the firmware's poll SHAPE — a `status & $30 == $10` wait loop must
//     converge, and the character written afterwards must reach the spool;
//   * end-to-end — a real //c ROM boots DOS 3.3, `PR#1` returns instead of
//     wedging, and `PRINT` output lands in the printer spool. ROM/disk
//     gated: SKIPs (not fails) when the user-provided media is absent.

#include "DiskIICard.h"
#include "IWMDevice.h"
#include "M6502.h"
#include "Memory.h"
#include "SlotBus.h"
#include "SuperSerialCard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

// Slot $C0nX offsets (slot 1 = $C090-$C09F, the //c printer port).
constexpr uint8_t kRdrAddr     = 0x8;
constexpr uint8_t kStatusAddr  = 0x9;
constexpr uint8_t kCommandAddr = 0xA;

// 6551 SR_* bits (mirror SuperSerialCard.h private constants).
constexpr uint8_t SR_TDRE = 0x10;
constexpr uint8_t SR_DCD  = 0x20;
constexpr uint8_t SR_DSR  = 0x40;

std::string firstExisting(const std::vector<std::string>& candidates)
{
    namespace fs = std::filesystem;
    for (const auto& p : candidates) {
        if (fs::exists(p)) return p;
        const std::string up1 = "../" + p;    if (fs::exists(up1)) return up1;
        const std::string up2 = "../../" + p; if (fs::exists(up2)) return up2;
    }
    return {};
}

// Linearise the interleaved 24x40 text page so we can substring-search it.
std::string scrapeText(const uint8_t* ram)
{
    std::string out;
    for (int row = 0; row < 24; ++row) {
        const int base = 0x0400 + 0x80 * (row % 8) + 0x28 * (row / 8);
        for (int col = 0; col < 40; ++col) {
            const char ch = static_cast<char>(ram[base + col] & 0x7F);
            out.push_back((ch >= 0x20 && ch < 0x7F) ? ch : ' ');
        }
        out.push_back('\n');
    }
    return out;
}

// ── The status contract ──────────────────────────────────────────────────

void testArmedTapReadsAsDevicePresent()
{
    SuperSerialCard ssc(1);

    // Idle port, nothing cabled: DCD+DSR SET = "no carrier / not ready".
    // This is the modem-side behaviour ssc_acia_smoke also pins; the fix
    // must not disturb it.
    const uint8_t idle = ssc.deviceSelectRead(kStatusAddr);
    assert((idle & (SR_DCD | SR_DSR)) == (SR_DCD | SR_DSR));
    assert(idle & SR_TDRE);

    // Printer cabled to the port (tap armed), still no telnet client:
    // both pins must now read ACTIVE (bits clear). A printer is a DCE
    // that is simply there.
    ssc.setPrinterTap(true);
    const uint8_t tapped = ssc.deviceSelectRead(kStatusAddr);
    assert((tapped & (SR_DCD | SR_DSR)) == 0);
    assert(tapped & SR_TDRE);

    // Unplug the printer → back to "nothing attached".
    ssc.setPrinterTap(false);
    assert((ssc.deviceSelectRead(kStatusAddr) & (SR_DCD | SR_DSR))
           == (SR_DCD | SR_DSR));

    std::printf("  ok: armed printer tap reads as device-present\n");
}

// ── The firmware's poll shape ────────────────────────────────────────────

void testFirmwareWaitLoopConverges()
{
    // The //c printer firmware's per-character wait is, in shape:
    //     loop: LDA $C099 / AND #$30 / CMP #$10 / BNE loop
    // i.e. spin until DCD is active (bit clear) AND TDRE is set. Model it
    // literally, with a bound: an unconverged loop is the hang.
    SuperSerialCard ssc(1);
    ssc.setPrinterTap(true);
    ssc.deviceSelectWrite(kCommandAddr, 0x0B);   // DTR on, as firmware init does

    bool ready = false;
    for (int spins = 0; spins < 1000 && !ready; ++spins)
        ready = (ssc.deviceSelectRead(kStatusAddr) & (SR_DCD | SR_TDRE)) == SR_TDRE;
    assert(ready && "printer-port firmware wait loop never converged");

    // Having escaped the loop, the firmware writes the character. It must
    // reach the host spool the ImageWriter drains.
    ssc.deviceSelectWrite(kRdrAddr, 'A');
    assert(ssc.printerSpoolBytes() == 1);

    std::printf("  ok: firmware status wait loop converges and the byte spools\n");
}

// ── End to end on a real //c ─────────────────────────────────────────────

// Boot `romPath` with DOS 3.3 in slot 6 and an armed printer tap in slot 1,
// then type `PR#1` and `PRINT "HI"`. Returns 0 on pass/skip, 1 on failure.
int testIIcPrintsThroughPR1(const char* tag, const std::string& romPath,
                            const std::string& disk)
{
    if (romPath.empty() || disk.empty()) {
        std::printf("  SKIP [%s]: ROM or DOS 3.3 disk not present\n", tag);
        return 0;                       // user-provided media — skip, not fail
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
        std::printf("FAIL [%s]: could not load %s\n", tag, romPath.c_str());
        return 1;
    }

    auto drive = std::make_unique<DiskIICard>(6);
    const std::string diskRom = firstExisting({"roms/disk2.rom"});
    if (!diskRom.empty()) drive->loadBootRom(diskRom);
    const std::string lssRom = firstExisting({"roms/diskii_p6.rom"});
    if (!lssRom.empty()) drive->loadLssRom(lssRom);
    drive->insertDisk(disk);
    drive->setIWM(&iwm);
    mem.slotBus().plug(6, std::move(drive));

    auto ssc = std::make_unique<SuperSerialCard>(1);
    ssc->setPrinterTap(true);           // //c printer port, armed by default
    SuperSerialCard* tap = ssc.get();
    mem.slotBus().plug(1, std::move(ssc));
    mem.slotBus().reset();

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();

    // Boot right through HELLO — the bare "]" appears transiently mid-boot,
    // so wait for the SYSTEM MASTER banner and then let it settle, or the
    // keystrokes below land in a machine that is still reading sectors.
    bool booted = false;
    for (int i = 0; i < 120'000'000 && !booted; ++i) {
        cpu.step();
        if ((i & 0x3FFFF) == 0 &&
            scrapeText(mem.data()).find("DOS VERSION 3.3") != std::string::npos)
            booted = true;
    }
    for (int i = 0; i < 20'000'000; ++i) cpu.step();
    if (!booted) {
        std::printf("FAIL [%s]: DOS 3.3 never reached its banner\nScreen:\n%s",
                    tag, scrapeText(mem.data()).c_str());
        return 1;
    }

    auto typeLine = [&](const char* s) {
        for (const char* p = s; *p; ++p) {
            mem.queueKey(static_cast<uint8_t>(*p));
            for (int k = 0; k < 300'000; ++k) cpu.step();
        }
    };

    // Pre-fix this wedged inside the printer firmware and never returned:
    // the PC stopped advancing ($C2BA on the 32 KB dump) and "PR#1" was
    // not even echoed.
    typeLine("PR#1\r");
    const uint16_t pcAfterPr = cpu.getProgramCounter();
    if (scrapeText(mem.data()).find("PR#1") == std::string::npos) {
        std::printf("FAIL [%s]: PR#1 never echoed — guest wedged in the "
                    "printer firmware at $%04X\n", tag, pcAfterPr);
        return 1;
    }

    typeLine("PRINT \"HI\"\r");
    if (cpu.getProgramCounter() == pcAfterPr) {
        std::printf("FAIL [%s]: PC frozen at $%04X across two commands\n",
                    tag, pcAfterPr);
        return 1;
    }

    std::vector<uint8_t> spooled;
    tap->drainPrinterSpoolFrom(0, spooled);
    if (spooled.empty()) {
        std::printf("FAIL [%s]: PR#1 + PRINT produced no bytes at the "
                    "printer tap (PC=$%04X)\n", tag, cpu.getProgramCounter());
        return 1;
    }

    // The firmware echoes the command line and then the program's output,
    // so "HI" must be in there. Bit 7 is set on everything COUT emits.
    std::string text;
    for (uint8_t b : spooled) text.push_back(static_cast<char>(b & 0x7F));
    if (text.find("HI") == std::string::npos) {
        std::printf("FAIL [%s]: spool has %zu bytes but no \"HI\": \"%s\"\n",
                    tag, spooled.size(), text.c_str());
        return 1;
    }

    std::printf("  ok [%s]: PR#1 + PRINT reached the printer spool "
                "(%zu bytes)\n", tag, spooled.size());
    return 0;
}

int testIIcPrintsThroughPort2(const char* tag, const std::string& romPath,
                              const std::string& disk)
{
    if (romPath.empty() || disk.empty()) {
        std::printf("  SKIP [%s]: ROM or DOS 3.3 disk not present\n", tag);
        return 0;                       // user-provided media — skip, not fail
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
        std::printf("FAIL [%s]: could not load %s\n", tag, romPath.c_str());
        return 1;
    }

    auto drive = std::make_unique<DiskIICard>(6);
    const std::string diskRom = firstExisting({"roms/disk2.rom"});
    if (!diskRom.empty()) drive->loadBootRom(diskRom);
    const std::string lssRom = firstExisting({"roms/diskii_p6.rom"});
    if (!lssRom.empty()) drive->loadLssRom(lssRom);
    drive->insertDisk(disk);
    drive->setIWM(&iwm);
    mem.slotBus().plug(6, std::move(drive));

    // An ImageWriter on the MODEM port: port 2, tap armed, printer cable.
    auto ssc = std::make_unique<SuperSerialCard>(2);
    ssc->setBuiltInPort(true);
    ssc->setPrinterTap(true);
    ssc->setCable(SuperSerialCard::Cable::PrinterReady);
    SuperSerialCard* tap = ssc.get();
    mem.slotBus().plug(2, std::move(ssc));
    mem.slotBus().reset();

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();

    // Boot right through HELLO — the bare "]" appears transiently mid-boot,
    // so wait for the SYSTEM MASTER banner and then let it settle, or the
    // keystrokes below land in a machine that is still reading sectors.
    bool booted = false;
    for (int i = 0; i < 120'000'000 && !booted; ++i) {
        cpu.step();
        if ((i & 0x3FFFF) == 0 &&
            scrapeText(mem.data()).find("DOS VERSION 3.3") != std::string::npos)
            booted = true;
    }
    for (int i = 0; i < 20'000'000; ++i) cpu.step();
    if (!booted) {
        std::printf("FAIL [%s]: DOS 3.3 never reached its banner\nScreen:\n%s",
                    tag, scrapeText(mem.data()).c_str());
        return 1;
    }

    auto typeLine = [&](const char* s) {
        for (const char* p = s; *p; ++p) {
            mem.pasteKeyStream(p, 1);   // FIFO: keys wait while it blocks
            for (int k = 0; k < 300'000; ++k) cpu.step();
        }
    };

    typeLine("PR#2\r");
    typeLine("PRINT \"HI\"\r");
    auto spoolText = [&] {
        std::vector<uint8_t> bytes;
        tap->drainPrinterSpoolFrom(0, bytes);
        std::string text;
        for (uint8_t b : bytes) text.push_back(static_cast<char>(b & 0x7F));
        return text;
    };
    if (spoolText().find("HI") == std::string::npos) {
        std::printf("FAIL [%s]: PR#2 + PRINT never reached a printer on "
                    "port 2 (PC=$%04X)\n", tag, cpu.getProgramCounter());
        return 1;
    }
    // The printer goes offline: DCD (pin 5) drops, and the port-2 firmware
    // waits on it exactly as port 1's does.
    tap->setCable(SuperSerialCard::Cable::PrinterOffline);
    typeLine("PRINT \"HO\"\r");
    if (spoolText().find("HO") != std::string::npos) {
        std::printf("FAIL [%s]: port 2 printed with DCD inactive\n", tag);
        return 1;
    }
    tap->setCable(SuperSerialCard::Cable::PrinterReady);
    for (int k = 0; k < 3'000'000; ++k) cpu.step();
    if (spoolText().find("HO") == std::string::npos) {
        std::printf("FAIL [%s]: port 2 did not resume when the printer "
                    "came back (PC=$%04X)\n", tag, cpu.getProgramCounter());
        return 1;
    }
    std::printf("  ok [%s]: a printer on port 2 prints, waits offline, "
                "resumes\n", tag);
    return 0;
}

}  // namespace

int main()
{
    std::printf("//c printer port test\n");

    testArmedTapReadsAsDevicePresent();
    testFirmwareWaitLoopConverges();

    const std::string disk = firstExisting({"disks_5.4/dsk/dos33_master.dsk"});
    int rc = 0;
    rc |= testIIcPrintsThroughPR1(
        "//c-32k", firstExisting({"roms/apple2c-32Kv0.rom"}), disk);
    rc |= testIIcPrintsThroughPort2(
        "//c-32k port 2", firstExisting({"roms/apple2c-32Kv0.rom"}), disk);
    rc |= testIIcPrintsThroughPR1(
        "//c-16k", firstExisting({"roms/apple2c-16K.rom"}), disk);
    rc |= testIIcPrintsThroughPR1(
        "//c+", firstExisting({"roms/apple2cp.rom", "roms/apple2c-plus.rom"}),
        disk);

    if (rc != 0) return 1;
    std::printf("PASS\n");
    return 0;
}
