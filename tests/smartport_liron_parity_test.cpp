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

// The HLE SmartPortCard answers SmartPort calls the way the real Liron
// firmware (roms/liron.rom, the //c's Protocol Converter) does in front of
// a UniDisk 3.5. The oracle is that firmware itself, run by LironCard over
// the SmartPort bus to POM2's UniDisk responder (SmartPortBusDevice): the
// same calls go to both cards and A, the carry and the returned bytes must
// agree. Apple's reference is the Apple IIc Technical Reference "Calls to
// the Protocol Converter" (Brutal Deluxe scan, iwm_protocolconverter.pdf).
//
// Needs roms/apple2e.rom and roms/liron.rom (exit 77 without them).

#include "LironCard.h"
#include "M6502.h"
#include "Memory.h"
#include "ResourcePaths.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "TestTempPath.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int      kSlot     = 5;
constexpr uint16_t kCallSite = 0x0300;
constexpr uint16_t kPlist    = 0x0380;
constexpr uint16_t kBuf      = 0x2000;
constexpr uint16_t kCtl      = 0x2100;   // an empty CONTROL list: count 0

int g_failures = 0;

struct Call {
    const char*          what;
    uint8_t              cmd;
    std::vector<uint8_t> plist;
    int                  resultBytes;   // bytes of kBuf to compare
};

// Where the HLE card differs from the firmware on purpose. Each is pinned on
// both sides, so a change to either is seen.
//  * STATUS unit 0, byte 1: the Liron answers $00; the card answers $40,
//    "no interrupt pending" (TN SmartPort #2), so that an interrupt handler
//    polling the chain does not take the card for an interrupting one.
//  * INIT on a unit other than 0 is no call a program makes (the manual
//    gives unit $00 only). The firmware sends it to the drive and reports
//    the drive's answer — $7F from POM2's bus responder, which says "last
//    in chain"; the card answers $28.
bool knownDivergence(const char* what)
{
    const std::string w = what;
    return w == "STATUS unit 0 code 0" || w == "INIT unit 1" || w == "INIT unit 2";
}

struct Result {
    uint8_t              a = 0xEE;
    bool                 carry = false;
    bool                 returned = false;
    std::vector<uint8_t> bytes;
};

struct Machine {
    Memory mem;
    M6502  cpu{&mem};
    Machine() { mem.setCpu(&cpu); }
};

Result spCall(Machine& m, const Call& c)
{
    for (int i = 0; i < 0x40; ++i) m.mem.memWrite(static_cast<uint16_t>(kBuf + i), 0xA5);
    uint16_t p = kPlist;
    for (uint8_t b : c.plist) m.mem.memWrite(p++, b);
    m.mem.memWrite(0x0010, 0x00);
    p = kCallSite;
    auto w = [&](uint8_t v) { m.mem.memWrite(p++, v); };
    w(0x20); w(0x0D); w(0xC0 + kSlot);          // JSR $C50D (SmartPort entry)
    w(c.cmd); w(kPlist & 0xFF); w(kPlist >> 8);
    w(0x08);                                    // PHP
    w(0x85); w(0x11);                           // STA $11
    w(0x68); w(0x85); w(0x12);                  // PLA / STA $12
    w(0xA9); w(0x77); w(0x85); w(0x10);         // LDA #$77 / STA $10
    const uint16_t park = p;
    w(0x4C); w(park & 0xFF); w(park >> 8);
    m.cpu.setAccumulator(0xEE);
    m.cpu.setStatusRegister(0x24);
    m.cpu.setStackPointer(0xF0);
    m.cpu.setProgramCounter(kCallSite);
    for (int i = 0; i < 400 && m.cpu.getProgramCounter() != park; ++i) m.cpu.run(10000);
    Result r;
    r.returned = m.mem.memRead(0x10) == 0x77;
    r.a        = m.mem.memRead(0x11);
    r.carry    = (m.mem.memRead(0x12) & 0x01) != 0;
    for (int i = 0; i < c.resultBytes; ++i)
        r.bytes.push_back(m.mem.memRead(static_cast<uint16_t>(kBuf + i)));
    return r;
}

std::string hex(const Result& r)
{
    std::string s;
    char b[8];
    std::snprintf(b, sizeof b, "A=%02X ", r.a);
    s += b;
    s += r.carry ? "C " : "c ";
    if (!r.returned) s += "(no return) ";
    for (uint8_t v : r.bytes) { std::snprintf(b, sizeof b, "%02X", v); s += b; }
    return s;
}

std::string writeImage()
{
    const fs::path p = pom2test::tempPath("smartport_liron_parity_800k.po");
    std::vector<char> img(1600u * 512u);
    for (size_t b = 0; b < 1600; ++b)
        std::memset(img.data() + b * 512, static_cast<int>(b & 0xFF), 512);
    std::ofstream(p, std::ios::binary).write(img.data(), static_cast<std::streamsize>(img.size()));
    return p.string();
}

bool loadIIe(Machine& m, const std::string& rom)
{
    m.mem.setIIEMode(true);
    m.mem.clearRam();
    m.mem.resetSoftSwitches();
    if (!m.mem.loadAppleIIRom(rom.c_str())) return false;
    m.cpu.setCpuMode(M6502::CpuMode::CMOS);
    m.cpu.hardReset();
    return true;
}

}  // namespace

int main()
{
    const std::string rom   = pom2::findResource("roms/apple2e.rom");
    const std::string liron = pom2::findResource("roms/liron.rom");
    if (rom.empty() || liron.empty()) {
        std::printf("SKIP smartport_liron_parity: needs roms/apple2e.rom and roms/liron.rom\n");
        return 77;
    }
    const std::string image = writeImage();

    // The HLE card, two units, a disk in unit 1.
    Machine hle;
    if (!loadIIe(hle, rom)) { std::printf("FAIL: //e ROM\n"); return 1; }
    {
        auto card = std::make_unique<pom2::SmartPortCard>(kSlot);
        card->setUnitCount(2);
        auto unit = std::make_unique<pom2::SmartPort35Unit>();
        if (!unit->loadImage(image)) { std::printf("FAIL: HLE mount\n"); return 1; }
        card->setUnit(0, std::move(unit));
        // Unit 2 is a drive with no disk, as the Liron's second bay is.
        card->setUnit(1, std::make_unique<pom2::SmartPort35Unit>());
        hle.mem.slotBus().plug(kSlot, std::move(card));
    }

    // The real firmware, two units, a disk in unit 1.
    Machine real;
    if (!loadIIe(real, rom)) { std::printf("FAIL: //e ROM\n"); return 1; }
    {
        auto card = std::make_unique<pom2::LironCard>(kSlot);
        if (!card->romLoaded()) { std::printf("FAIL: Liron ROM: %s\n", card->lastError().c_str()); return 1; }
        card->setUnitCount(2);
        std::string err;
        if (!card->mountBay(0, image, err)) { std::printf("FAIL: Liron mount: %s\n", err.c_str()); return 1; }
        real.mem.slotBus().plug(kSlot, std::move(card));
    }

    const uint8_t lo = kBuf & 0xFF, hi = kBuf >> 8;
    const uint8_t clo = kCtl & 0xFF, chi = kCtl >> 8;
    const std::vector<Call> calls = {
        { "STATUS unit 0 code 0",        0x00, {3, 0, lo, hi, 0x00}, 8 },
        { "STATUS unit 1 code 0",        0x00, {3, 1, lo, hi, 0x00}, 4 },
        { "STATUS unit 1 code 1 (DCB)",  0x00, {3, 1, lo, hi, 0x01}, 0 },
        { "STATUS unit 1 code 2",        0x00, {3, 1, lo, hi, 0x02}, 0 },
        { "STATUS unit 1 code 3 (DIB)",  0x00, {3, 1, lo, hi, 0x03}, 25 },
        { "STATUS unit 1 code 5",        0x00, {3, 1, lo, hi, 0x05}, 8 },
        { "STATUS unit 0 code 3",        0x00, {3, 0, lo, hi, 0x03}, 0 },
        { "STATUS unit 2 (empty)",       0x00, {3, 2, lo, hi, 0x00}, 4 },
        { "STATUS unit 9 (none)",        0x00, {3, 9, lo, hi, 0x00}, 0 },
        { "STATUS bad pcount",           0x00, {2, 1, lo, hi, 0x00}, 0 },
        { "READ unit 1 block 2",         0x01, {3, 1, lo, hi, 2, 0, 0}, 4 },
        { "READ unit 0",                 0x01, {3, 0, lo, hi, 2, 0, 0}, 0 },
        { "READ unit 1 block 1600",      0x01, {3, 1, lo, hi, 0x40, 0x06, 0}, 0 },
        { "READ unit 2 (empty)",         0x01, {3, 2, lo, hi, 2, 0, 0}, 0 },
        { "WRITE unit 0",                0x02, {3, 0, lo, hi, 2, 0, 0}, 0 },
        { "FORMAT unit 0",               0x03, {1, 0}, 0 },
        { "CONTROL unit 0 code 0",       0x04, {3, 0, clo, chi, 0x00}, 0 },
        { "CONTROL unit 0 code 1",       0x04, {3, 0, clo, chi, 0x01}, 0 },
        { "CONTROL unit 1 code 0",       0x04, {3, 1, clo, chi, 0x00}, 0 },
        { "CONTROL unit 1 code 1 (DCB)", 0x04, {3, 1, clo, chi, 0x01}, 0 },
        { "INIT unit 0",                 0x05, {1, 0}, 0 },
        { "INIT unit 1",                 0x05, {1, 1}, 0 },
        { "OPEN unit 1",                 0x06, {1, 1}, 0 },
        { "CLOSE unit 1",                0x07, {1, 1}, 0 },
        { "READ $08 unit 1",             0x08, {4, 1, lo, hi, 0x0C, 0x02, 2, 0, 0}, 4 },
        { "WRITE $09 unit 1",            0x09, {4, 1, lo, hi, 0x0C, 0x02, 2, 0, 0}, 0 },
        { "command $0A",                 0x0A, {3, 1, lo, hi, 0x00}, 0 },
        { "STATUS pcount 0",             0x00, {0, 1, lo, hi, 0x00}, 0 },
        { "STATUS pcount 9",             0x00, {9, 1, lo, hi, 0x00}, 0 },
        { "READ pcount 2",               0x01, {2, 1, lo, hi, 2, 0, 0}, 0 },
        { "STATUS unit 1 code 4",        0x00, {3, 1, lo, hi, 0x04}, 0 },
        { "STATUS unit 1 code 6",        0x00, {3, 1, lo, hi, 0x06}, 0 },
        { "CONTROL unit 0 code 4",       0x04, {3, 0, clo, chi, 0x04}, 0 },
        { "CONTROL unit 1 code 2",       0x04, {3, 1, clo, chi, 0x02}, 0 },
        { "CONTROL unit 1 code 3",       0x04, {3, 1, clo, chi, 0x03}, 0 },
        { "CONTROL unit 1 code 9",       0x04, {3, 1, clo, chi, 0x09}, 0 },
        { "CONTROL unit 9",              0x04, {3, 9, clo, chi, 0x00}, 0 },
        { "INIT unit 2",                 0x05, {1, 2}, 0 },
        { "OPEN unit 0",                 0x06, {1, 0}, 0 },
        { "FORMAT unit 1",               0x03, {1, 1}, 0 },
        { "command $0A unit 0",          0x0A, {3, 0, lo, hi, 0x00}, 0 },
        { "CONTROL unit 1 code 5",       0x04, {3, 1, clo, chi, 0x05}, 0 },
        { "CONTROL unit 1 code 7",       0x04, {3, 1, clo, chi, 0x07}, 0 },
        { "STATUS unit 2 code 3 (DIB)",  0x00, {3, 2, lo, hi, 0x03}, 25 },
    };
    for (Machine* m : { &hle, &real }) {
        m->mem.memWrite(kCtl, 0);
        m->mem.memWrite(kCtl + 1, 0);
    }

    for (const Call& c : calls) {
        const Result h = spCall(hle, c);
        const Result r = spCall(real, c);
        const bool same = h.a == r.a && h.carry == r.carry && h.bytes == r.bytes;
        const bool known = knownDivergence(c.what);
        std::printf("%-28s  real %-60s  hle %s%s\n", c.what, hex(r).c_str(),
                    hex(h).c_str(), same ? "" : known ? "   (known)" : "   <-- DIFF");
        if (!r.returned || !h.returned) {
            std::printf("FAIL: %s did not return\n", c.what);
            ++g_failures;
        } else if (known) {
            const std::string w = c.what;
            const bool pinned = w == "STATUS unit 0 code 0"
                ? (r.bytes == std::vector<uint8_t>{2, 0x00, 0, 0, 0, 0, 0, 0} &&
                   h.bytes == std::vector<uint8_t>{2, 0x40, 0, 0, 0, 0, 0, 0} &&
                   !r.carry && !h.carry)
                : (r.carry && r.a == 0x7F && h.carry && h.a == 0x28);
            if (!pinned) {
                std::printf("FAIL: %s moved from its pinned divergence\n", c.what);
                ++g_failures;
            }
        } else if (!same) {
            ++g_failures;
        }
    }
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
