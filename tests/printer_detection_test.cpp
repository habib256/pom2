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

// The printer-detection toolkit a bench links against (docs/printer-detection.md):
//
//   * PrinterPortControl — the option language, validated whole: one bad key
//     and nothing changes; a //c port refuses DIP settings;
//   * SlotBus's access log — a guest routine that decides "printer or modem"
//     from the SSC's DIP bank and its ROM page, proven to have made ZERO
//     writes to the slot before deciding;
//   * PrinterRender — a captured stream saved and rendered to PNG with only
//     the core library linked.

#include "GrapplerCard.h"
#include "M6502.h"
#include "Memory.h"
#include "PrinterPortControl.h"
#include "PrinterRender.h"
#include "SlotBus.h"
#include "SuperSerialCard.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
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

void testOptionLanguage()
{
    SlotBus bus;
    bus.plug(2, std::make_unique<SuperSerialCard>(2));
    bus.plug(1, std::make_unique<GrapplerCard>(1));
    auto* ssc = dynamic_cast<SuperSerialCard*>(bus.peripheral(2));
    auto* gp = dynamic_cast<GrapplerCard*>(bus.peripheral(1));
    std::string err;

    expect(pom2::applyPrinterPortOptions(bus, 2, "mode=printer,dsw1=0xE2,cable=printer,tap=on", err),
           "ssc options apply: " + err);
    expect(ssc->dipSwitch1() == 0xE2, "dsw1 wins over mode (applied after)");
    expect(ssc->dipSwitch2() == 0x7A, "mode=printer wrote the printer DSW2");
    expect(ssc->cable() == SuperSerialCard::Cable::PrinterReady && ssc->printerTap(),
           "cable and tap");

    expect(!pom2::applyPrinterPortOptions(bus, 2, "cable=none,bogus=1", err) &&
           ssc->cable() == SuperSerialCard::Cable::PrinterReady,
           "one bad key: nothing changes");
    expect(!pom2::applyPrinterPortOptions(bus, 2, "dsw1=0x100", err), "dsw out of range");
    expect(!pom2::applyPrinterPortOptions(bus, 2, "online=off", err),
           "a Grappler option on an SSC is refused");
    expect(!pom2::applyPrinterPortOptions(bus, 3, "online=off", err), "empty slot refused");

    ssc->setBuiltInPort(true);
    expect(!pom2::applyPrinterPortOptions(bus, 2, "mode=comm", err),
           "a //c port has no mode switches");
    expect(pom2::applyPrinterPortOptions(bus, 2, "cable=printer-offline", err),
           "but it has a cable");
    ssc->setBuiltInPort(false);

    expect(pom2::applyPrinterPortOptions(bus, 1, "online=off,paper=out,type=5", err),
           "grappler options apply: " + err);
    expect(!gp->online() && gp->paperOut() &&
           gp->printerType() == GrapplerCard::PrinterType::AppleDotMatrix,
           "grappler state");
    const auto st = pom2::describePrinterPort(bus, 1);
    expect(st.card == "grappler" && st.lineBits == 0x0C, "describe: offline + PE + BUSY");
    const std::string json = pom2::printerPortJson(pom2::describePrinterPort(bus, 2));
    expect(json.find("\"dsw1\":\"$E2\"") != std::string::npos, "json: " + json);

    int slot = 0;
    std::string opts;
    expect(pom2::parsePrinterPortSpec("3:tap=on", slot, opts, err) && slot == 3 &&
           opts == "tap=on", "spec parse");
    expect(!pom2::parsePrinterPortSpec("8:tap=on", slot, opts, err), "slot 8 refused");
    expect(!pom2::parsePrinterPortSpec("3:", slot, opts, err), "empty options refused");
}

// A detection routine the way A2FC wants to write it: read the SSC's DIP
// bank and the ROM's class byte, store both, stop. No store to the card.
//   $0300  AD A1 C0   LDA $C0A1    ; DSW1, slot 2
//   $0303  29 03      AND #$03     ; SW1:5-6
//   $0305  8D 00 04   STA $0400
//   $0308  AD 0C C2   LDA $C20C    ; Pascal 1.1 device class
//   $030B  8D 01 04   STA $0401
//   $030E  4C 0E 03   JMP $030E
void testReadOnlyDetectionIsLogged()
{
    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.clearRam();
    auto ssc = std::make_unique<SuperSerialCard>(2);
    ssc->setMode(SuperSerialCard::Mode::Printer);
    mem.slotBus().plug(2, std::move(ssc));
    const uint8_t prog[] = { 0xAD, 0xA1, 0xC0, 0x29, 0x03, 0x8D, 0x00, 0x04,
                             0xAD, 0x0C, 0xC2, 0x8D, 0x01, 0x04,
                             0x4C, 0x0E, 0x03 };
    for (size_t i = 0; i < sizeof(prog); ++i)
        mem.memWrite(static_cast<uint16_t>(0x0300 + i), prog[i]);
    mem.slotBus().enableAccessLog(2);
    cpu.setProgramCounter(0x0300);
    for (int i = 0; i < 50; ++i) cpu.step();

    expect(mem.memRead(0x0400) == 0x02, "the routine reads printer mode");
    const auto log = mem.slotBus().takeAccessLog(2);
    size_t writes = 0;
    bool sawDip = false, sawRom = false;
    for (const auto& e : log) {
        writes += e.write ? 1 : 0;
        sawDip = sawDip || (e.addr == 0xC0A1 && e.value == 0xFE);
        sawRom = sawRom || e.addr == 0xC20C;
    }
    expect(sawDip && sawRom, "both reads are in the log");
    expect(writes == 0, "zero writes to the slot before the decision");
    expect(log.size() >= 2 && log[0].cycle <= log[1].cycle, "timestamps ascend");
    expect(mem.slotBus().takeAccessLog(2).empty(), "the log drains");

    // A write is logged too, with its value.
    mem.slotBus().deviceSelectWrite(0xC0AA, 0x0B);
    const auto after = mem.slotBus().takeAccessLog(2);
    expect(after.size() == 1 && after[0].write && after[0].value == 0x0B,
           "a write is logged with its value");
    mem.slotBus().disableAccessLog(2);
    (void)mem.slotBus().deviceSelectRead(0xC0A1);
    expect(mem.slotBus().takeAccessLog(2).empty(), "disabled: nothing logged");
}

void testRender()
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "pom2_printer_render_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string text = "HELLO FROM THE BENCH\r\n";
    const std::vector<uint8_t> bytes(text.begin(), text.end());
    std::string err;

    expect(pom2::savePrinterBytes((dir / "stream.bin").string(), bytes, err),
           "save bytes: " + err);
    std::ifstream f(dir / "stream.bin", std::ios::binary);
    const std::vector<uint8_t> back{ std::istreambuf_iterator<char>(f),
                                     std::istreambuf_iterator<char>() };
    expect(back == bytes, "saved bytes round-trip");

    using pom2::ImageWriter;
    using pom2::IwModel;
    for (IwModel m : { IwModel::ImageWriterII, IwModel::EpsonFX80 }) {
        const std::string prefix = (dir / ImageWriter::modelName(m)).string();
        const int pages = pom2::renderPrinterBytesToPng(bytes, m, prefix, err, 72);
        expect(pages == 1, std::string("one page rendered for ") +
               ImageWriter::modelName(m) + " " + err);
        std::ifstream png(prefix + "-1.png", std::ios::binary);
        char sig[8] = {};
        png.read(sig, 8);
        expect(png && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G',
               "a PNG file was written");
    }
    fs::remove_all(dir, ec);
}

}  // namespace

int main()
{
    std::printf("Printer detection toolkit test\n");
    testOptionLanguage();
    testReadOnlyDetectionIsLogged();
    testRender();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("PASS\n");
    return 0;
}
