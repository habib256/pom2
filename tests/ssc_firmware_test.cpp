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

// Super Serial Card: Apple's EPROM (341-0065-A) and the SW1:5-6 mode switches.
//
// What this pins:
//   * the EPROM map — $Cn00 is the dump's last page, $C800 the whole 2 KB
//     (MAME `a2ssc.cpp:352-376`); anything but 2048 bytes is refused and the
//     card keeps its hand-assembled page;
//   * the mode switches at $C0n1 (DSW1 bits $03, `a2ssc.cpp:118-122`) and the
//     DSW2 re-purposing printer mode brings with it (`a2ssc.cpp:128-144`);
//   * the factory finds the dump on a slotted machine and never on a //c,
//     whose serial ports run off the system ROM;
//   * end to end — a real //e with the real firmware in slot 2: `PR#2` in
//     printer mode puts `PRINT` output on the printer tap, and in
//     communications mode on the 6551's transmit queue with the tap off —
//     but only with DSR and DCD active: the firmware waits for
//     `status & $70 == $10` ($CAF5), so with nothing on the line it waits,
//     as a real card with no modem does. The hand-assembled page checked
//     TDRE alone and printed into the void.
//     ROM-gated: SKIPs when roms/apple2e.rom or the SSC dump is absent.

#include "M6502.h"
#include "Memory.h"
#include "SlotBus.h"
#include "SlotCardFactory.h"
#include "SuperSerialCard.h"
#include "SystemProfile.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool ok, const std::string& what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

std::string firstExisting(const std::vector<std::string>& candidates)
{
    namespace fs = std::filesystem;
    for (const auto& p : candidates) {
        for (const std::string prefix : { "", "../", "../../" }) {
            if (fs::exists(prefix + p)) return prefix + p;
        }
    }
    return {};
}

std::vector<uint8_t> readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

// A synthetic 2 KB image whose every byte names its own offset's page.
std::vector<uint8_t> patternFirmware()
{
    std::vector<uint8_t> fw(SuperSerialCard::kFirmwareBytes);
    for (std::size_t i = 0; i < fw.size(); ++i)
        fw[i] = static_cast<uint8_t>((i >> 8) * 0x10 + (i & 0x0F));
    return fw;
}

void testFirmwareMap()
{
    SuperSerialCard card(2);
    const uint8_t handPage00 = card.slotRomRead(0x00);
    expect(!card.firmwareLoaded(), "a new card has no EPROM");
    expect(card.expansionRomRead(0x000) == 0xFF,
           "no EPROM: $C800 reads $FF");

    expect(!card.loadFirmware(std::vector<uint8_t>(2047, 0xAA)),
           "a 2047-byte image is refused");
    expect(!card.loadFirmware(std::vector<uint8_t>(4096, 0xAA)),
           "a 4096-byte image is refused");
    expect(!card.firmwareLoaded() && card.slotRomRead(0x00) == handPage00,
           "a refused image leaves the hand-assembled page in place");

    const auto fw = patternFirmware();
    expect(card.loadFirmware(fw), "a 2048-byte image loads");
    expect(card.slotRomRead(0x00) == fw[0x700] &&
           card.slotRomRead(0xFF) == fw[0x7FF],
           "$Cn00-$CnFF is the EPROM's last page (a2ssc.cpp:352-355)");
    expect(card.expansionRomRead(0x000) == fw[0x000] &&
           card.expansionRomRead(0x7FF) == fw[0x7FF],
           "$C800-$CFFF is the whole EPROM (a2ssc.cpp:373-376)");
    expect(card.takesC800(), "the card claims the $C800 window");
}

void testModeSwitches()
{
    using Mode = SuperSerialCard::Mode;
    SuperSerialCard card(2);
    // $C0n1: A1 low selects DSW1 alone; $C0n2: A0 low selects DSW2 alone.
    expect(card.mode() == Mode::Communications &&
           (card.deviceSelectRead(0x1) & 0x03) == 0x00,
           "the card ships in communications mode");
    expect(card.deviceSelectRead(0x2) == 0x52, "communications DSW2 is 8N1");

    card.setMode(Mode::Printer);
    expect((card.deviceSelectRead(0x1) & 0x03) == 0x02,
           "printer mode reads $02 at $C0n1 (a2ssc.cpp:121)");
    expect((card.deviceSelectRead(0x1) & 0xF0) == 0xF0,
           "the mode switches leave the baud-rate switches alone");
    const uint8_t dsw2 = card.deviceSelectRead(0x2);
    expect((dsw2 & 0x0C) == 0x08, "printer mode: 80 columns (SW2:3-4)");
    expect((dsw2 & 0x20) == 0x20, "printer mode: no delay after CR (SW2:2)");

    card.setMode(Mode::Communications);
    expect(card.deviceSelectRead(0x2) == 0x52,
           "back to communications mode restores 8N1");

    for (Mode m : { Mode::Communications, Mode::SicP8, Mode::Printer,
                    Mode::SicP8A }) {
        Mode parsed = Mode::Communications;
        expect(SuperSerialCard::parseModeKey(SuperSerialCard::modeKey(m), parsed) &&
               parsed == m, std::string("mode key round-trips: ") +
               SuperSerialCard::modeKey(m));
    }
    Mode untouched = Mode::Printer;
    expect(!SuperSerialCard::parseModeKey("bogus", untouched) &&
           untouched == Mode::Printer,
           "an unknown mode key is refused and leaves the default");
}

// The three handshake inputs, per cable, as the status register shows them.
// Bit set = line INACTIVE for DCD ($20) and DSR ($40); TDRE ($10) is masked
// while CTS is inactive (MAME `mos6551.cpp:286-289`).
void testCableLines()
{
    using Cable = SuperSerialCard::Cable;
    struct Row { Cable cable; uint8_t lineBits; };
    const Row rows[] = {
        { Cable::Nothing,        0x60 },   // DCD, DSR inactive; TDRE masked
        { Cable::PrinterReady,   0x10 },   // all active
        { Cable::PrinterOffline, 0x70 },   // DCD, DSR inactive; CTS active
        { Cable::Modem,          0x30 },   // no telnet peer: no carrier
        { Cable::NullModem,      0x10 },
    };
    for (const Row& r : rows) {
        SuperSerialCard card(2);
        card.setCable(r.cable);
        const uint8_t st = card.deviceSelectRead(0x9);
        expect((st & 0x70) == r.lineBits,
               std::string("status lines for cable ") +
               SuperSerialCard::cableKey(r.cable));
        SuperSerialCard::Cable parsed = Cable::Auto;
        expect(SuperSerialCard::parseCableKey(SuperSerialCard::cableKey(r.cable),
                                              parsed) && parsed == r.cable,
               std::string("cable key round-trips: ") +
               SuperSerialCard::cableKey(r.cable));
    }
    {   // Auto keeps the historical answer: nothing attached, lines inactive,
        // transmitter free; the printer tap alone brings DCD + DSR up.
        SuperSerialCard card(2);
        expect((card.deviceSelectRead(0x9) & 0x70) == 0x70, "auto, idle");
        card.setPrinterTap(true);
        expect((card.deviceSelectRead(0x9) & 0x70) == 0x10, "auto, tap armed");
    }
    {   // CTS inactive parks the byte in TDR; CTS back sends it.
        SuperSerialCard card(2);
        card.setCable(Cable::Nothing);
        card.deviceSelectWrite(0xA, 0x0B);          // DTR on, no IRQs
        card.deviceSelectWrite(0x8, 'A');
        card.deviceSelectWrite(0x8, 'B');           // replaces A in TDR
        expect(card.recentTxText().empty(), "CTS inactive: nothing is sent");
        expect((card.deviceSelectRead(0x9) & 0x10) == 0,
               "CTS inactive: TDRE reads 0");
        card.setCable(Cable::NullModem);
        expect(card.recentTxText() == "B",
               "CTS back: the byte left in TDR is sent, once");
        expect((card.deviceSelectRead(0x9) & 0x10) != 0, "CTS back: TDRE");
    }
    {   // A DCD/DSR change interrupts with DTR asserted, not without.
        SuperSerialCard card(2);
        card.setCable(Cable::PrinterReady);
        (void)card.deviceSelectRead(0x9);           // clear anything pending
        card.setCable(Cable::PrinterOffline);
        expect(card.irqState() == 0, "no DTR: a line change does not interrupt");
        card.deviceSelectWrite(0xA, 0x0B);          // DTR on
        card.setCable(Cable::PrinterReady);
        expect(card.irqState() != 0, "DTR on: a line change interrupts");
        expect((card.deviceSelectRead(0x9) & 0x80) != 0, "status shows IRQ");
    }
    {   // The raw DIP banks.
        SuperSerialCard card(2);
        card.setDipSwitches(0xE2, 0x7C);
        expect(card.deviceSelectRead(0x1) == 0xE2 &&
               card.deviceSelectRead(0x2) == 0x7C &&
               card.deviceSelectRead(0x0) == (0xE2 & 0x7C) &&
               card.deviceSelectRead(0x3) == 0xFF,
               "setDipSwitches: $C0n1 / $C0n2 / $C0n0 / $C0n3");
        expect(card.mode() == SuperSerialCard::Mode::Printer,
               "mode() decodes DSW1 bits $03");
    }
}

void testFactory(const std::string& dump)
{
    pom2::SlotCardFactory factory(
        [&](std::string_view resource) -> std::string {
            return resource == "roms/ssc_341-0065-a.bin" ? dump : std::string{};
        });
    SuperSerialCard iie(2);
    expect(!factory.loadSuperSerialFirmware(iie, pom2::SystemProfile::AppleIIe).empty() &&
           iie.firmwareLoaded(), "a //e card gets the EPROM");
    SuperSerialCard iic(1);
    expect(factory.loadSuperSerialFirmware(iic, pom2::SystemProfile::AppleIIc).empty() &&
           !iic.firmwareLoaded(), "a //c port never gets a card EPROM");
}

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

// A //e with the real SSC in slot 2, booted to the BASIC prompt.
struct FirmwareRig {
    Memory mem;
    M6502  cpu{&mem};
    SuperSerialCard* card = nullptr;

    bool boot(const std::string& romPath, const std::vector<uint8_t>& fw)
    {
        mem.setCpu(&cpu);
        mem.clearRam();
        mem.resetSoftSwitches();
        mem.setIIEMode(true);
        if (!mem.loadAppleIIRom(romPath.c_str(), /*pickLowerHalf=*/false)) {
            expect(false, "could not load " + romPath);
            return false;
        }
        auto ssc = std::make_unique<SuperSerialCard>(2);
        card = ssc.get();
        // An empty image keeps POM2's hand-assembled page (no EPROM).
        if (!fw.empty()) expect(card->loadFirmware(fw), "the shipped dump loads");
        mem.slotBus().plug(2, std::move(ssc));
        mem.slotBus().reset();
        cpu.setCpuMode(M6502::CpuMode::CMOS);
        cpu.hardReset();
        run(3'000'000);
        return true;
    }
    void run(int steps) { for (int i = 0; i < steps; ++i) cpu.step(); }
    void type(const char* s)
    {
        // The paste FIFO, not queueKey: keys typed while the firmware is
        // blocked on an offline printer must wait, not overwrite each other.
        for (const char* p = s; *p; ++p) {
            mem.pasteKeyStream(p, 1);
            run(100'000);
        }
        run(1'000'000);
    }
    std::string spooled() const
    {
        std::vector<uint8_t> bytes;
        card->drainPrinterSpoolFrom(0, bytes);
        std::string out;
        for (uint8_t b : bytes) out.push_back(static_cast<char>(b & 0x7F));
        return out;
    }
    void dump(const std::string& sent)
    {
        std::printf("  sent: \"%s\"\n  PC=$%04X\n  screen:\n%s", sent.c_str(),
                    cpu.getProgramCounter(), scrapeText(mem.data()).c_str());
    }
};

void testEndToEnd(const std::string& dump)
{
    const std::string rom = firstExisting({ "roms/apple2e.rom" });
    if (rom.empty() || dump.empty()) {
        std::printf("  SKIP: roms/apple2e.rom or the SSC dump is absent\n");
        return;
    }
    const auto fw = readFile(dump);
    using Mode = SuperSerialCard::Mode;
    using Cable = SuperSerialCard::Cable;

    {   // Printer mode, tap armed: PR#2 + PRINT reaches the ImageWriter spool.
        FirmwareRig rig;
        if (!rig.boot(rom, fw)) return;
        rig.card->setMode(Mode::Printer);
        rig.card->setPrinterTap(true);
        rig.type("PR#2\r");
        rig.type("PRINT \"HELLO\"\r");
        const std::string sent = rig.spooled();
        if (sent.find("HELLO") == std::string::npos) rig.dump(sent);
        expect(sent.find("HELLO") != std::string::npos,
               "printer mode: PR#2 + PRINT reaches the printer tap");
    }
    {   // Communications mode, lines strapped: the bytes reach the 6551.
        FirmwareRig rig;
        if (!rig.boot(rom, fw)) return;
        rig.card->setModemLinesTied(true);
        rig.type("PR#2\r");
        rig.type("PRINT \"HELLO\"\r");
        const std::string sent = rig.card->recentTxText();
        if (sent.find("HELLO") == std::string::npos) rig.dump(sent);
        expect(sent.find("HELLO") != std::string::npos,
               "communications mode, lines active: PR#2 + PRINT reaches the 6551");
    }
    {   // Communications mode, nothing on the line: the firmware waits for
        // DSR + DCD and sends nothing.
        FirmwareRig rig;
        if (!rig.boot(rom, fw)) return;
        rig.type("PR#2\r");
        rig.type("PRINT \"HELLO\"\r");
        expect(rig.card->recentTxText().empty(),
               "communications mode, nothing on the line: nothing is sent");
    }
    {   // The printer goes offline in the middle of a print, then comes back:
        // the firmware waits at $CAF5 with DCD and DSR inactive, and resumes
        // where it stopped.
        FirmwareRig rig;
        if (!rig.boot(rom, fw)) return;
        rig.card->setMode(Mode::Printer);
        rig.card->setPrinterTap(true);
        rig.card->setCable(Cable::PrinterReady);
        rig.type("PR#2\r");
        rig.card->setCable(Cable::PrinterOffline);
        rig.type("PRINT \"HELLO\"\r");
        const std::string whileOff = rig.spooled();
        expect(whileOff.find("HELLO") == std::string::npos,
               "printer offline: nothing more reaches the printer");
        const uint16_t pc = rig.cpu.getProgramCounter();
        expect(pc >= 0xC800 && pc < 0xD000,
               "printer offline: the guest waits inside the SSC firmware");
        rig.card->setCable(Cable::PrinterReady);
        rig.run(2'000'000);
        const std::string after = rig.spooled();
        if (after.find("HELLO") == std::string::npos) rig.dump(after);
        expect(after.find("HELLO") != std::string::npos,
               "printer back online: the firmware resumes and HELLO prints");
    }
}

// Without the EPROM the card serves POM2's own page. The Monitor stores $Cn00
// in CSW for PR#n and in KSW for IN#n, so $Cn00 has to tell the two apart:
// it used to bind the OUTPUT hook whatever the call, so IN#2 read nothing,
// looped rewriting CSW and returned $C2 as a keystroke, and the first
// character handed to a fresh PR#2 was dropped.
void testHandRom()
{
    const std::string rom = firstExisting({ "roms/apple2e.rom" });
    if (rom.empty()) {
        std::printf("  SKIP: roms/apple2e.rom is absent\n");
        return;
    }
    {   // PR#2 — every character reaches the wire, the first one included.
        FirmwareRig rig;
        if (!rig.boot(rom, {})) return;
        expect(!rig.card->firmwareLoaded(), "hand page: no EPROM loaded");
        rig.card->setPrinterTap(true);
        rig.card->setModemLinesTied(true);
        rig.type("PR#2\r");
        rig.type("PRINT \"HELLO\"\r");
        const std::string out = rig.spooled();
        expect(out.find("PRINT \"HELLO\"") != std::string::npos,
               "hand page: PR#2 sends the echoed command whole");
        expect(out.find("\rHELLO") != std::string::npos,
               "hand page: PR#2 sends the output");
        if (out.find("HELLO") == std::string::npos) rig.dump("PR#2");
    }
    {   // IN#2 — the peer types a command and BASIC runs it.
        FirmwareRig rig;
        if (!rig.boot(rom, {})) return;
        rig.card->setModemLinesTied(true);
        rig.type("IN#2\r");
        const char* in = "PRINT 12345\r";
        rig.card->deliverRxBytes(reinterpret_cast<const uint8_t*>(in), 12);
        rig.run(3'000'000);
        const uint8_t* ram = rig.mem.data();
        expect(ram[0x39] == 0xC2 && ram[0x37] != 0xC2,
               "hand page: IN#2 binds KSW and leaves CSW alone");
        expect(rig.card->rxQueueDepth() == 0, "hand page: IN#2 reads the port");
        const std::string screen = scrapeText(ram);
        const bool ran = screen.find("\n12345") != std::string::npos ||
                         screen.find(" 12345") != std::string::npos;
        expect(ran, "hand page: IN#2 input reaches BASIC");
        if (!ran) rig.dump(in);
    }
}

}  // namespace

int main()
{
    std::printf("Super Serial Card firmware test\n");
    const std::string dump = firstExisting({ "roms/ssc_341-0065-a.bin" });

    testFirmwareMap();
    testModeSwitches();
    testCableLines();
    testHandRom();
    if (dump.empty()) {
        std::printf("  SKIP: roms/ssc_341-0065-a.bin is absent\n");
    } else {
        testFactory(dump);
        testEndToEnd(dump);
    }

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
