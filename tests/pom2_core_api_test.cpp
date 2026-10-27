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

#include <pom2/core.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

void writeLe32(std::ofstream& output, std::uint32_t value)
{
    const char bytes[4] = {
        static_cast<char>(value),
        static_cast<char>(value >> 8),
        static_cast<char>(value >> 16),
        static_cast<char>(value >> 24),
    };
    output.write(bytes, sizeof(bytes));
}

void writeTestCassette(const std::string& path)
{
    std::ofstream tape(path, std::ios::binary);
    tape.write("POM1ACI1", 8);
    tape.put(1); // format version
    tape.put(0); // initial comparator level
    tape.put(0);
    tape.put(0);
    writeLe32(tape, 1);    // one transition
    writeLe32(tape, 1024); // after 1024 emulated CPU cycles
    assert(tape.good());
}

std::vector<char> readFile(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(input), {});
}

/// A blank 800 KiB ProDOS-order image whose block 0 is a boot program:
/// at $0801 it stores $A5 and the X the boot ROM handed over, then spins.
void writeBoot800K(const std::string& path)
{
    std::vector<char> image(1600u * 512u, 0);
    const unsigned char boot[] = {
        0x01,                   // $0800: ProDOS boot-block count
        0xA9, 0xA5,             // LDA #$A5
        0x8D, 0x00, 0x03,       // STA $0300
        0x8E, 0x01, 0x03,       // STX $0301
        0x4C, 0x09, 0x08,       // JMP $0809
    };
    std::copy(std::begin(boot), std::end(boot), image.begin());
    std::ofstream disk(path, std::ios::binary);
    disk.write(image.data(), static_cast<std::streamsize>(image.size()));
    assert(disk.good());
}

pom2::Core makeSmartPortCore(const std::string& sourceDir)
{
    pom2::Core core({
        pom2::CpuModel::CMOS65C02,
        pom2::VideoTiming::NTSC,
        true,
    });
    assert(core.loadRom(sourceDir + "/roms/apple2e.rom"));
    return core;
}

// The 3.5" SmartPort half of the facade, the contract a headless test host
// (pom2games' a2shot) relies on: boot an 800 KiB ProDOS image from slot 5
// with a Disk II still in slot 6, write a block, flush, and read the image
// file back from the host while the core is alive.
void testSmartPort(const std::string& sourceDir)
{
    const std::string imagePath = "pom2_core_api_800k.po";
    writeBoot800K(imagePath);

    pom2::Core core = makeSmartPortCore(sourceDir);
    assert(!core.bootSlot(5));                       // empty slot
    assert(!core.insertSmartPortDisk(0, imagePath)); // no controller yet
    assert(core.attachSmartPort(5));
    assert(!core.attachSmartPort(5));                // slot taken
    assert(core.attachDiskII(sourceDir + "/roms/disk2.rom",
                             sourceDir + "/roms/diskii_p6.rom"));
    assert(!core.smartPortDiskInserted(0));
    assert(!core.insertSmartPortDisk(2, imagePath));
    assert(!core.insertSmartPortDisk(0, "pom2_core_api_missing.po"));
    assert(!core.lastError().empty());

    // A 140 KiB image is not a 3.5" disk.
    const std::string smallPath = "pom2_core_api_140k.po";
    {
        std::ofstream small(smallPath, std::ios::binary);
        const std::vector<char> blank(280u * 512u, 0);
        small.write(blank.data(), static_cast<std::streamsize>(blank.size()));
    }
    assert(!core.insertSmartPortDisk(0, smallPath));
    assert(std::remove(smallPath.c_str()) == 0);

    assert(core.insertSmartPortDisk(0, imagePath));
    assert(core.smartPortDiskInserted(0));
    assert(!core.smartPortDiskInserted(1));

    assert(core.bootSlot(5));
    assert(core.cpuState().programCounter == 0xC500);
    core.run(200000);
    assert(core.read(0x0300) == 0xA5);
    assert(core.read(0x0301) == 0x50);               // X = slot × 16
    const pom2::CpuState booted = core.cpuState();
    assert(booted.programCounter >= 0x0809 && booted.programCounter <= 0x080B);

    // No mechanical timing: a second core replays the boot to the cycle.
    {
        pom2::Core replay = makeSmartPortCore(sourceDir);
        assert(replay.attachSmartPort(5));
        assert(replay.attachDiskII(sourceDir + "/roms/disk2.rom",
                                   sourceDir + "/roms/diskii_p6.rom"));
        assert(replay.insertSmartPortDisk(0, imagePath));
        assert(replay.bootSlot(5));
        replay.run(200000);
        const pom2::CpuState again = replay.cpuState();
        assert(again.cycles == booted.cycles);
        assert(again.programCounter == booted.programCounter);
    }

    // Stream one block through the card's own soft switches ($C0D0-$C0D4
    // for slot 5): unit, block LO/HI, then 512 data bytes.
    const auto writeBlock = [&core](std::uint8_t block, std::uint8_t seed) {
        core.write(0xC0D0, 0x00);
        core.write(0xC0D1, block);
        core.write(0xC0D2, 0x00);
        for (int i = 0; i < 512; ++i)
            core.write(0xC0D3, static_cast<std::uint8_t>(seed + i));
        assert((core.read(0xC0D4) & 0x01) == 0);     // no I/O error
    };
    const auto blockOnHost = [&imagePath](std::uint8_t block,
                                          std::uint8_t seed) {
        const std::vector<char> bytes = readFile(imagePath);
        assert(bytes.size() == 1600u * 512u);
        for (int i = 0; i < 512; ++i)
            if (static_cast<std::uint8_t>(bytes[block * 512u + i])
                != static_cast<std::uint8_t>(seed + i))
                return false;
        return true;
    };

    writeBlock(7, 0x11);
    assert(core.flushSmartPortDisk(0));
    assert(blockOnHost(7, 0x11));
    assert(core.flushSmartPortDisk(0));              // clean: nothing to do
    assert(core.flushSmartPortDisk(1));              // empty unit
    assert(!core.flushSmartPortDisk(2));

    // Inserting over a dirty disk saves it first — the swap a test host
    // uses to reach the file without a flush.
    writeBlock(8, 0x33);
    assert(core.insertSmartPortDisk(0, imagePath));
    assert(blockOnHost(8, 0x33));

#ifndef _WIN32
    // A failed flush names ITS cause. SmartPort35Unit::saveDirty never copied
    // the image's error, so lastError() replayed the last failed LOAD (here
    // "cannot open …missing.po") or the generic fallback. A read-only
    // directory refuses the atomic replace's sibling temp; root ignores that.
    if (geteuid() != 0) {
        const std::string dir = "pom2_core_api_ro";
        const std::string roImage = dir + "/disk.po";
        (void)mkdir(dir.c_str(), 0755);
        writeBoot800K(roImage);
        pom2::Core ro = makeSmartPortCore(sourceDir);
        assert(ro.attachSmartPort(5));
        assert(ro.insertSmartPortDisk(0, roImage));
        assert(!ro.insertSmartPortDisk(0, "pom2_core_api_missing.po"));
        ro.write(0xC0D0, 0x00);
        ro.write(0xC0D1, 10);
        ro.write(0xC0D2, 0x00);
        for (int i = 0; i < 512; ++i) ro.write(0xC0D3, 0xEE);
        assert(chmod(dir.c_str(), 0555) == 0);
        assert(!ro.flushSmartPortDisk(0));
        const std::string err = ro.lastError();
        assert(chmod(dir.c_str(), 0755) == 0);
        assert(err.find("missing") == std::string::npos);
        assert(err.find("pom2tmp") != std::string::npos);
        assert(ro.ejectSmartPortDisk(0));            // writable again: saves
        assert(std::remove(roImage.c_str()) == 0);
        assert(rmdir(dir.c_str()) == 0);
    }
#endif

    // So does an eject.
    writeBlock(9, 0x55);
    assert(core.ejectSmartPortDisk(0));
    assert(!core.smartPortDiskInserted(0));
    assert(blockOnHost(9, 0x55));
    assert(!core.bootSlot(4));                       // nothing in slot 4

    // The signature is checked on the page the CPU will RUN, not only on the
    // card: a //e leaves reset with SLOTC3ROM off, so a card in slot 3 is
    // hidden behind the internal 80-column firmware ($C300 holds no trio).
    {
        pom2::Core slot3 = makeSmartPortCore(sourceDir);
        assert(slot3.attachSmartPort(3));
        assert(slot3.insertSmartPortDisk(0, imagePath));
        assert(!slot3.bootSlot(3));
        assert(slot3.lastError().find("not visible") != std::string::npos);
    }

    // A //c forces INTCXROM and shows a slot-5 card only while ARMED, the
    // arm every reset clears. Unarmed, the CPU ran the //c's own $C500.
    {
        pom2::Core iic({
            pom2::CpuModel::CMOS65C02,
            pom2::VideoTiming::NTSC,
            true,
        });
        assert(iic.loadRom(sourceDir + "/roms/apple2c-32Kv0.rom"));
        assert(iic.attachSmartPort(5));
        assert(iic.insertSmartPortDisk(0, imagePath));
        assert(iic.bootSlot(5));
        iic.run(400000);
        assert(iic.read(0x0300) == 0xA5);
        assert(iic.read(0x0301) == 0x50);
    }

    assert(std::remove(imagePath.c_str()) == 0);
}

} // namespace

// setCpuState (pom2games' a2shot `call:ADR`): push a return address, place
// S and PC, run to the RTS. Nothing else may move — the cycle counter and the
// //e soft switches a test sets up stay as they were, which a RESET-vector
// trampoline could not promise. NMOS and 65C02 alike.
void testSetCpuState()
{
    for (const auto model : { pom2::CpuModel::NMOS6502, pom2::CpuModel::CMOS65C02 }) {
        pom2::CoreConfig cfg;
        cfg.cpu = model;
        cfg.iieMemory = model == pom2::CpuModel::CMOS65C02;
        pom2::Core c(cfg);
        // $0300: INX / RTS. $0400: JMP $0400, the landing site.
        const std::uint8_t code[] = { 0xE8, 0x60 };
        for (std::uint16_t i = 0; i < sizeof code; ++i) c.write(0x0300 + i, code[i]);
        c.write(0x0400, 0x4C); c.write(0x0401, 0x00); c.write(0x0402, 0x04);
        // JSR convention: the stack holds the return address minus one.
        c.write(0x01FF, 0x03); c.write(0x01FE, 0xFF);           // $03FF → RTS lands at $0400
        if (cfg.iieMemory) c.write(0xC001, 0);                  // 80STORE on
        const std::uint64_t cyclesBefore = c.cpuState().cycles;

        pom2::CpuState st = c.cpuState();
        st.accumulator = 0x5A; st.x = 0x05; st.y = 0xA5;
        st.status = 0x00;                                        // bit 5 must come back set
        st.stackPointer = 0xFD; st.programCounter = 0x0300;
        st.cycles = 12345; st.halted = true;                     // both ignored
        c.setCpuState(st);
        const pom2::CpuState got = c.cpuState();
        assert(got.accumulator == 0x5A && got.x == 0x05 && got.y == 0xA5);
        assert(got.stackPointer == 0xFD && got.programCounter == 0x0300);
        assert((got.status & 0x20) && "bit 5 reads 1 on every 6502");
        assert(got.cycles == cyclesBefore && !got.halted);
        if (cfg.iieMemory)
            assert((c.read(0xC018) & 0x80) && "80STORE survives setCpuState");

        c.step();                                                // INX
        assert(c.cpuState().x == 0x06);
        c.step();                                                // RTS
        assert(c.cpuState().programCounter == 0x0400 && c.cpuState().stackPointer == 0xFF);
    }
    std::printf("  ok: setCpuState runs a routine and returns, nothing else moves\n");
}

// Call a guest routine the way pom2games' a2shot `call:` does: return
// address on the stack, registers set, step until it comes back to $0400.
void callRoutine(pom2::Core& c, std::uint16_t entry, std::uint8_t a,
                 std::uint8_t x, std::uint8_t y)
{
    c.write(0x0400, 0x4C); c.write(0x0401, 0x00); c.write(0x0402, 0x04);
    c.write(0x01FF, 0x03); c.write(0x01FE, 0xFF);               // RTS → $0400
    pom2::CpuState st = c.cpuState();
    st.accumulator = a; st.x = x; st.y = y; st.status = 0x04;  // I set
    st.stackPointer = 0xFD; st.programCounter = entry;
    c.setCpuState(st);
    for (int i = 0; i < 200000 && c.cpuState().programCounter != 0x0400; ++i) c.step();
    assert(c.cpuState().programCounter == 0x0400 && "the firmware routine returned");
}

// attachMouse / moveMouse / setMouse (pom2games' request 2), both models,
// through Apple's own firmware: INITMOUSE, SETMOUSE on, then READMOUSE
// publishes the position in the slot-4 screen holes.
void testMouse(const std::string& sourceDir)
{
    for (const auto model : { pom2::MouseModel::AppleMouse,
                              pom2::MouseModel::AppleMouseAppleWin }) {
        pom2::CoreConfig cfg;
        cfg.cpu = pom2::CpuModel::CMOS65C02;
        cfg.iieMemory = true;
        pom2::Core c(cfg);
        assert(c.loadRom(sourceDir + "/roms/apple2e.rom"));
        assert(!c.mouseAttached());
        assert(c.attachMouse(4, model, sourceDir + "/roms"));
        assert(c.mouseAttached());
        assert(!c.attachMouse(5, model, sourceDir + "/roms") && "one mouse card");
        // The entry table at $C412: SETMOUSE, SERVE, READ, CLEAR, POS, CLAMP, HOME, INIT.
        c.coldBoot();      // soft switches at power-on: $C4xx is the card's ROM
        auto entry = [&](int i) { return static_cast<std::uint16_t>(0xC400 | c.read(0xC412 + i)); };
        const std::uint16_t setMouse = entry(0), readMouse = entry(2), initMouse = entry(7);
        callRoutine(c, initMouse, 0, 0xC4, 0x40);
        callRoutine(c, setMouse, 0x01, 0xC4, 0x40);              // mouse on, no interrupts
        auto holeX = [&] { return c.read(0x047C) | (c.read(0x057C) << 8); };
        auto holeY = [&] { return c.read(0x04FC) | (c.read(0x05FC) << 8); };

        c.moveMouse(10, 5, false);
        c.run(40000);
        callRoutine(c, readMouse, 0, 0xC4, 0x40);
        assert(holeX() == 10 && holeY() == 5 && "relative motion reaches the firmware");

        bool there = false;
        for (int frame = 0; frame < 20 && !there; ++frame) {
            there = c.setMouse(300, 120, false);
            c.run(17030);
            callRoutine(c, readMouse, 0, 0xC4, 0x40);
        }
        assert(holeX() == 300 && holeY() == 120 && "absolute position converges");

        // The card's VBL interrupt reaches the 6502 as in the application.
        // SETMOUSE $09 = on + VBL interrupt. The //e ROM's IRQ handler ends in
        // JMP ($03FE): ours services the card, counts, and returns.
        const std::uint16_t serve = entry(1);
        const std::uint8_t handler[] = {
            0xA2, 0xC4, 0xA0, 0x40,                              // LDX #$C4 / LDY #$40
            0x20, static_cast<std::uint8_t>(serve), 0xC4,        // JSR SERVEMOUSE
            0xEE, 0x10, 0x03,                                    // INC $0310
            0xA5, 0x45, 0x40 };                                  // LDA $45 / RTI
        for (std::uint16_t i = 0; i < sizeof handler; ++i) c.write(0x0320 + i, handler[i]);
        c.write(0x03FE, 0x20); c.write(0x03FF, 0x03);
        c.write(0x0310, 0);
        callRoutine(c, setMouse, 0x09, 0xC4, 0x40);
        c.write(0x0330, 0x58);                                   // CLI
        c.write(0x0331, 0x4C); c.write(0x0332, 0x31); c.write(0x0333, 0x03); // JMP $0331
        pom2::CpuState st = c.cpuState();
        st.programCounter = 0x0330; st.stackPointer = 0xF0;
        c.setCpuState(st);
        c.run(17030 * 6);                                        // six frames
        const int vbls = c.read(0x0310);
        assert(vbls >= 5 && vbls <= 7 && "about one VBL interrupt per frame");
    }
    std::printf("  ok: attachMouse, moveMouse and setMouse through the real firmware\n");
}

// Machine::AppleIIc (pom2games' request 3): the 65C02, //e memory and the
// IOU mouse built in — moveMouse()/setMouse() drive it through the system
// ROM's own firmware, whose IRQ handler integrates the motion; attachMouse()
// is refused. The 32 KiB (rev 0) and the 16 KiB (rev 255) ROMs.
void testIIcMachine(const std::string& sourceDir)
{
    for (const char* rom : { "/roms/apple2c-32Kv0.rom", "/roms/apple2c-16K.rom" }) {
        pom2::CoreConfig cfg;
        cfg.machine = pom2::Machine::AppleIIc;
        pom2::Core c(cfg);
        assert(c.loadRom(sourceDir + rom));
        assert(c.mouseAttached() && "the //c's mouse is built in");
        assert(!c.attachMouse(4, pom2::MouseModel::AppleMouse, sourceDir + "/roms"));
        c.coldBoot();
        auto entry = [&](int i) { return static_cast<std::uint16_t>(0xC400 | c.read(0xC412 + i)); };
        const std::uint16_t setMouse = entry(0), readMouse = entry(2), initMouse = entry(7);
        callRoutine(c, initMouse, 0, 0xC4, 0x40);
        callRoutine(c, setMouse, 0x01, 0xC4, 0x40);
        // Let the ROM's IRQ handler run: CLI / JMP *.
        auto runOpen = [&](int cycles) {
            c.write(0x0330, 0x58); c.write(0x0331, 0x4C); c.write(0x0332, 0x31); c.write(0x0333, 0x03);
            pom2::CpuState st = c.cpuState();
            st.programCounter = 0x0330; st.stackPointer = 0xF0;
            c.setCpuState(st);
            c.run(cycles);
        };
        auto holeX = [&] { return c.read(0x047C) | (c.read(0x057C) << 8); };
        auto holeY = [&] { return c.read(0x04FC) | (c.read(0x05FC) << 8); };
        c.moveMouse(10, 5, false);
        runOpen(17030 * 4);
        callRoutine(c, readMouse, 0, 0xC4, 0x40);
        assert(holeX() == 10 && holeY() == 5 && "the //c's IOU mouse moves");
        bool there = false;
        for (int frame = 0; frame < 40 && !there; ++frame) {
            there = c.setMouse(200, 90, false);
            runOpen(17030);
            callRoutine(c, readMouse, 0, 0xC4, 0x40);
        }
        assert(holeX() == 200 && holeY() == 90 && "absolute position converges on the //c");
    }
    // The Mockingboard 4c on a //c (the letter's question): dormant, the
    // mouse firmware answers $C4xx; a write to $C403 wakes it (as 4sports'
    // and DIGIDREAM's detection do) and it answers that page, mouse entry
    // table included. Ctrl-Reset leaves it awake (MAME reset_w); only a power
    // cycle (machine_reset, apple2e.cpp:1292) hands the page back.
    {
        pom2::CoreConfig cfg;
        cfg.machine = pom2::Machine::AppleIIc;
        pom2::Core c(cfg);
        assert(c.loadRom(sourceDir + "/roms/apple2c-32Kv0.rom"));
        assert(c.attachMockingboard());          // slot ignored: the internal connector
        c.coldBoot();
        const std::uint8_t sig = c.read(0xC4FB);  // the mouse firmware's ID byte
        assert(sig == 0xD6 && "dormant: $C4xx is the mouse firmware");
        c.write(0xC403, 0x00);                   // the wake
        assert(c.read(0xC4FB) != 0xD6 && "awake: the 4c answers $C400-$C4FF");
        c.hardReset();
        assert(c.read(0xC4FB) != 0xD6 && "Ctrl-Reset: still awake");
        c.coldBoot();
        assert(c.read(0xC4FB) == 0xD6 && "power cycle: the firmware is back");
    }
    std::printf("  ok: Machine::AppleIIc — built-in mouse, 32 K and 16 K ROMs, 4c wake\n");
}

/// Copy `code` to $0300, close it with a JMP to itself, and run it there
/// with interrupts masked until it reaches that JMP.
void runProgram(pom2::Core& c, std::initializer_list<std::uint8_t> code,
                int maxSteps = 1000)
{
    std::uint16_t at = 0x0300;
    for (const std::uint8_t b : code) c.write(at++, b);
    c.write(at, 0x4C);
    c.write(static_cast<std::uint16_t>(at + 1), static_cast<std::uint8_t>(at));
    c.write(static_cast<std::uint16_t>(at + 2), static_cast<std::uint8_t>(at >> 8));
    pom2::CpuState st = c.cpuState();
    st.programCounter = 0x0300;
    st.status = 0x24;                                // I set
    c.setCpuState(st);
    for (int i = 0; i < maxSteps && c.cpuState().programCounter != at; ++i) c.step();
    assert(c.cpuState().programCounter == at);
}

/// A blank 140 KiB DOS-order image whose track 0 sector 0 is a boot
/// program: at $0801 it stores $A5 and the X the P5 PROM handed over (the
/// slot × 16 it found with JSR $FF58), then spins.
void writeBoot140K(const std::string& path)
{
    std::vector<char> image(35u * 16u * 256u, 0);
    const unsigned char boot[] = {
        0x01,                   // $0800: sectors for the PROM to load
        0xA9, 0xA5,             // LDA #$A5
        0x8D, 0x00, 0x03,       // STA $0300
        0x8E, 0x01, 0x03,       // STX $0301
        0x4C, 0x09, 0x08,       // JMP $0809
    };
    std::copy(std::begin(boot), std::end(boot), image.begin());
    std::ofstream disk(path, std::ios::binary);
    disk.write(image.data(), static_cast<std::streamsize>(image.size()));
    assert(disk.good());
}

// One Disk II per core, in any slot: the P5 PROM finds its slot at run time,
// so a controller in slot 5 boots at $C500 and hands the boot sector X=$50.
void testDiskIISlot(const std::string& sourceDir)
{
    const std::string bootRom = sourceDir + "/roms/disk2.rom";
    const std::string lssRom = sourceDir + "/roms/diskii_p6.rom";
    const std::string imagePath = "pom2_core_api_boot140k.dsk";
    writeBoot140K(imagePath);

    pom2::Core core;
    assert(core.loadRom(sourceDir + "/roms/apple2.rom"));
    assert(!core.flushDisk(0));                      // no controller yet
    assert(!core.attachDiskII(bootRom, lssRom, 0));
    assert(!core.attachDiskII(bootRom, lssRom, 8));
    assert(core.attachDiskII(bootRom, lssRom, 5));
    assert(!core.bootSlot(6));                       // slot 6 stays empty

    // Drive 1 alone: the PROM would wait on drive 0 forever.
    assert(core.insertDisk(1, imagePath));
    assert(core.diskInserted(1) && !core.diskInserted(0));
    assert(!core.bootDisk());
    assert(!core.lastError().empty());

    assert(core.insertDisk(0, imagePath));
    assert(core.flushDisk(0) && core.flushDisk(1));  // clean: nothing to write
    assert(!core.flushDisk(2));
    assert(core.bootSlot(5));
    assert(core.cpuState().programCounter == 0xC500);
    core.run(3000000);
    assert(core.read(0x0300) == 0xA5);
    assert(core.read(0x0301) == 0x50);               // X = slot × 16
    const pom2::CpuState booted = core.cpuState();
    assert(booted.programCounter >= 0x0809 && booted.programCounter <= 0x080B);

    // Moving the controller retires the old one and frees its slot.
    assert(core.attachDiskII(bootRom, lssRom, 6));
    assert(!core.diskInserted(0) && !core.diskInserted(1));
    assert(core.attachSmartPort(5));
    assert(!core.attachDiskII(bootRom, lssRom, 5));  // taken by the SmartPort

    // The sticker: the guest reads the drive's write-protect line (Q6H, then
    // a Q7L read: bit 7), and lifting it restores the writable default.
    assert(!core.setDiskWriteProtected(0, true));    // no disk
    assert(core.insertDisk(0, imagePath));
    assert(!core.setDiskWriteProtected(2, true));
    const auto senseProtect = [&core] {
        runProgram(core, {
            0xAD, 0xE9, 0xC0,   // LDA $C0E9   motor on
            0xAD, 0xED, 0xC0,   // LDA $C0ED   Q6H
            0xAD, 0xEE, 0xC0,   // LDA $C0EE   Q7L: write-protect in bit 7
            0x8D, 0x20, 0x03,   // STA $0320
            0xAD, 0xE8, 0xC0,   // LDA $C0E8   motor off
        });
        return (core.read(0x0320) & 0x80) != 0;
    };
    assert(!core.diskWriteProtected(0) && !senseProtect());
    assert(core.setDiskWriteProtected(0, true));
    assert(core.diskWriteProtected(0) && senseProtect());
    assert(core.setDiskWriteProtected(0, false));
    assert(!core.diskWriteProtected(0) && !senseProtect());
    assert(core.ejectDisk(0));

    // A //c's disk port is slot 6.
    pom2::CoreConfig cfg;
    cfg.machine = pom2::Machine::AppleIIc;
    pom2::Core iic(cfg);
    assert(iic.loadRom(sourceDir + "/roms/apple2c-32Kv0.rom"));
    assert(!iic.attachDiskII(bootRom, lssRom, 5));
    assert(iic.attachDiskII(bootRom, lssRom));

    assert(std::remove(imagePath.c_str()) == 0);
}

// setBusWatch / watchAddresses (pom2games' CHROMABREAK request 1): each CPU
// access to a watched address, at the cycle it lands on; the host's own
// read()/write() click the speaker but are not reported.
void testBusWatch(const std::string& sourceDir)
{
    pom2::Core core;
    assert(core.loadRom(sourceDir + "/roms/apple2.rom"));
    struct Access {
        std::uint16_t address;
        std::uint8_t  value;
        bool          write;
        std::uint64_t cycle;
    };
    std::vector<Access> seen;
    core.setBusWatch([&seen](std::uint16_t a, std::uint8_t v, bool w,
                             std::uint64_t cycle) {
        seen.push_back({a, v, w, cycle});
    });
    assert(!core.watchAddresses(0x2001, 0x2000, false, true));
    assert(core.watchAddresses(0xC030, 0xC030, true, false));
    assert(core.watchAddresses(0x2000, 0x3FFF, false, true));

    const std::uint64_t clicks = core.speakerToggleCount();
    (void)core.read(0xC030);
    core.write(0x2000, 0x11);
    assert(core.speakerToggleCount() == clicks + 1);
    assert(seen.empty());

    const std::uint64_t start = core.cpuState().cycles;
    runProgram(core, {
        0xAD, 0x30, 0xC0,       // LDA $C030
        0xAD, 0x30, 0xC0,       // LDA $C030
        0xA9, 0x5A,             // LDA #$5A
        0x8D, 0x45, 0x23,       // STA $2345
        0xAD, 0x45, 0x23,       // LDA $2345   (writes only: not reported)
    });
    assert(core.speakerToggleCount() == clicks + 3);
    assert(seen.size() == 3);
    assert(seen[0].address == 0xC030 && !seen[0].write);
    assert(seen[1].address == 0xC030 && !seen[1].write);
    assert(seen[2].address == 0x2345 && seen[2].write && seen[2].value == 0x5A);
    // The data cycle of each instruction: LDA abs reads on its 4th cycle.
    assert(seen[0].cycle > start && seen[0].cycle <= start + 4);
    assert(seen[1].cycle - seen[0].cycle == 4);
    assert(seen[2].cycle - seen[1].cycle == 2 + 4);  // LDA #, then STA abs

    core.clearWatchedAddresses();
    runProgram(core, { 0xAD, 0x30, 0xC0, 0x8D, 0x45, 0x23 });
    assert(seen.size() == 3);
    assert(core.watchAddresses(0xC030, 0xC030, true, false));
    core.setBusWatch({});
    runProgram(core, { 0xAD, 0x30, 0xC0 });
    assert(seen.size() == 3);
}

// displayState (request 2): the switches on a ][+, which has no $C01x
// status reads.
void testDisplayState(const std::string& sourceDir)
{
    pom2::Core core;
    assert(core.loadRom(sourceDir + "/roms/apple2.rom"));
    core.write(0xC050, 0);
    core.write(0xC057, 0);
    core.write(0xC052, 0);
    core.write(0xC055, 0);
    pom2::DisplayState d = core.displayState();
    assert(!d.text && d.hiRes && !d.mixed && d.page2);
    assert(!d.eightyColumn && !d.doubleHiRes && !d.eightyStore);
    (void)core.read(0xC051);
    (void)core.read(0xC054);
    d = core.displayState();
    assert(d.text && d.hiRes && !d.page2);
}

// attachRgbCard / rgbDhgrMode (request 3): the mode latch clocked by AN3
// with 80COL as data, on the //e's Féline and on the //c's adapter.
void testRgbCard(const std::string& sourceDir)
{
    // Two AN3 rising edges, each pushing the 80COL level written before it.
    const auto clockLatch = [](pom2::Core& c, bool first, bool second) {
        runProgram(c, {
            0x8D, static_cast<std::uint8_t>(first ? 0x0D : 0x0C), 0xC0,
            0x8D, 0x5E, 0xC0, 0x8D, 0x5F, 0xC0,
            0x8D, static_cast<std::uint8_t>(second ? 0x0D : 0x0C), 0xC0,
            0x8D, 0x5E, 0xC0, 0x8D, 0x5F, 0xC0,
        });
    };

    pom2::Core plus;
    assert(plus.loadRom(sourceDir + "/roms/apple2.rom"));
    assert(!plus.attachRgbCard());                   // the Féline needs a //e

    pom2::Core core = makeSmartPortCore(sourceDir);
    assert(!core.rgbCardAttached());
    assert(core.rgbDhgrMode() == pom2::RgbDhgrMode::Color140);
    assert(!core.attachRgbCard(7, pom2::RgbCardModel::LeChatMauveIIcAdapter));
    assert(!core.attachRgbCard(0));
    assert(core.attachRgbCard());
    assert(core.rgbCardAttached());
    assert(!core.attachRgbCard(6));                  // one card
    assert(core.rgbDhgrMode() == pom2::RgbDhgrMode::Color140);
    clockLatch(core, false, true);
    assert(core.rgbDhgrMode() == pom2::RgbDhgrMode::Mixed);
    clockLatch(core, false, false);
    assert(core.rgbDhgrMode() == pom2::RgbDhgrMode::Mono560);
    clockLatch(core, true, true);
    assert(core.rgbDhgrMode() == pom2::RgbDhgrMode::Color140);

    pom2::CoreConfig cfg;
    cfg.machine = pom2::Machine::AppleIIc;
    pom2::Core iic(cfg);
    assert(iic.loadRom(sourceDir + "/roms/apple2c-32Kv0.rom"));
    assert(!iic.attachRgbCard());                    // a //c takes the adapter
    assert(iic.attachRgbCard(7, pom2::RgbCardModel::LeChatMauveIIcAdapter));
    clockLatch(iic, false, true);
    assert(iic.rgbDhgrMode() == pom2::RgbDhgrMode::Mixed);
}

// Request 5: the unenhanced //e's ROM on Machine::AppleIIe — the NMOS //e a
// "65C02 required" check has to be run on.
void testUnenhancedIIe(const std::string& sourceDir)
{
    pom2::CoreConfig cfg;
    cfg.machine = pom2::Machine::AppleIIe;
    pom2::Core core(cfg);
    assert(core.loadRom(sourceDir + "/roms/apple2e_unenh.rom"));
    assert(core.loadCharacterRom(sourceDir + "/roms/apple2e_char_us_unenh.rom"));
    core.coldBoot();
    assert(core.read(0xFBB3) == 0x06);               // a //e…
    assert(core.read(0xFBC0) == 0xEA);               // …unenhanced
    core.run(2000000);
    assert(!core.cpuState().halted);
}

/// A 140 KiB ProDOS-order image whose block b is filled with the byte b.
void writeBlocks140K(const std::string& path)
{
    std::vector<char> image(280u * 512u);
    for (std::size_t b = 0; b < 280; ++b)
        std::fill(image.begin() + static_cast<std::ptrdiff_t>(b * 512),
                  image.begin() + static_cast<std::ptrdiff_t>((b + 1) * 512),
                  static_cast<char>(b));
    std::ofstream disk(path, std::ios::binary);
    disk.write(image.data(), static_cast<std::streamsize>(image.size()));
    assert(disk.good());
}

// Machine::AppleIIcPlus: its own ROM drives the MIG, the IWM and the 3.5"
// Sony behind the SmartPort calls, and boots it from slot 5; and its slot-6
// ProDOS block driver at $C64E — the slot-5 SmartPort code called as slot 6
// (M.G., "Apple IIc Plus: SmartPort Secrets", 2018) — reads the 5.25" disk.
void testIIcPlus(const std::string& sourceDir)
{
    pom2::CoreConfig cfg;
    cfg.machine = pom2::Machine::AppleIIcPlus;
    pom2::Core core(cfg);
    assert(core.loadRom(sourceDir + "/roms/apple2cp.rom"));
    assert(core.mouseAttached() && "the //c+ mouse is built in");
    assert(!core.attachSmartPort(5) && "the 3.5\" drives are built in");
    assert(!core.smartPortDiskInserted(0));
    assert(!core.bootSlot(5) && "nothing to boot");
    assert(!core.insertSmartPortDisk(2, "x.po"));

    const std::string disk35 = "pom2_core_api_iicplus_800k.po";
    writeBoot800K(disk35);
    assert(core.insertSmartPortDisk(0, disk35));
    assert(core.smartPortDiskInserted(0) && !core.smartPortDiskInserted(1));
    assert(core.bootSlot(5));
    for (int i = 0; i < 60 && core.read(0x0300) != 0xA5; ++i) core.run(500000);
    assert(core.read(0x0300) == 0xA5 && "the ROM booted the internal 3.5\"");
    assert(core.read(0x0301) == 0x50);               // X = slot 5, drive 1
    assert(core.flushSmartPortDisk(0));

    // $C64E: STATUS then READ block 2 of the 5.25" disk, as the article's
    // monitor session does. A = error, X/Y = block count after STATUS.
    const std::string disk525 = "pom2_core_api_iicplus_140k.po";
    writeBlocks140K(disk525);
    assert(core.insertDisk(0, disk525));
    const auto blockCall = [&core](std::uint8_t command) {
        const std::uint8_t params[6] = { command, 0x60, 0x00, 0x10, 0x02, 0x00 };
        for (int i = 0; i < 6; ++i)
            core.write(static_cast<std::uint16_t>(0x0320 + i), params[i]);
        runProgram(core, {
            0xA2, 0x05,             // LDX #5
            0xBD, 0x20, 0x03,       // LDA $0320,X
            0x95, 0x42,             // STA $42,X
            0xCA,                   // DEX
            0x10, 0xF8,             // BPL
            0x20, 0x4E, 0xC6,       // JSR $C64E
            0x8D, 0x30, 0x03,       // STA $0330
            0x8E, 0x31, 0x03,       // STX $0331
            0x8C, 0x32, 0x03,       // STY $0332
        }, 5000000);
    };
    // STATUS: A = $00 on a writable disk, X/Y = 280 blocks — the article's
    // numbers. The ROM drops the four phases with the 3.5" side selected and
    // reads the WP sense ($F485) once drive 1 is on: the IWM hands the drive
    // it selects the phases as they stand (DiskIICard::iwmSelectsDrive).
    blockCall(0x00);
    assert(core.read(0x0330) == 0x00 && "STATUS on a writable disk");
    assert(core.read(0x0331) == 0x18 && core.read(0x0332) == 0x01);   // 280
    blockCall(0x01);                                 // READ block 2
    assert(core.read(0x0330) == 0x00);
    for (std::uint16_t a = 0x1000; a < 0x1200; ++a) assert(core.read(a) == 0x02);

    assert(core.ejectSmartPortDisk(0) && !core.smartPortDiskInserted(0));
    assert(std::remove(disk35.c_str()) == 0);
    assert(core.ejectDisk(0));
    assert(std::remove(disk525.c_str()) == 0);
}

int main()
{
    testSetCpuState();
    testMouse(POM2_TEST_SOURCE_DIR);
    testIIcMachine(POM2_TEST_SOURCE_DIR);
    // ctest protects media for every test (CLAUDE.md, MediaWritePolicy.h);
    // this one writes only images it created, and pins that writes land.
#ifdef _WIN32
    _putenv_s("POM2_MEDIA_WRITE_DEFAULT", "writable");
#else
    setenv("POM2_MEDIA_WRITE_DEFAULT", "writable", 1);
#endif
    testSmartPort(POM2_TEST_SOURCE_DIR);
    testDiskIISlot(POM2_TEST_SOURCE_DIR);
    testBusWatch(POM2_TEST_SOURCE_DIR);
    testDisplayState(POM2_TEST_SOURCE_DIR);
    testRgbCard(POM2_TEST_SOURCE_DIR);
    testUnenhancedIIe(POM2_TEST_SOURCE_DIR);
    testIIcPlus(POM2_TEST_SOURCE_DIR);

    pom2::Core core;

    const std::string sourceDir = POM2_TEST_SOURCE_DIR;
    assert(core.loadRom(sourceDir + "/roms/apple2.rom"));
    assert(core.loadCharacterRom(sourceDir + "/roms/apple2_char.rom"));
    assert(core.attachDiskII(sourceDir + "/roms/disk2.rom",
                             sourceDir + "/roms/diskii_p6.rom"));
    assert(!core.diskInserted());
    assert(!core.bootDisk());
    assert(!core.lastError().empty());

    const std::string diskPath = "pom2_core_api_blank.dsk";
    {
        std::ofstream disk(diskPath, std::ios::binary);
        const std::vector<char> blank(35u * 16u * 256u, 0);
        disk.write(blank.data(), static_cast<std::streamsize>(blank.size()));
        assert(disk.good());
    }
    assert(core.insertDisk(0, diskPath));
    assert(core.diskInserted());
    assert(core.bootDisk());
    assert(core.cpuState().programCounter == 0xC600);
    assert(core.ejectDisk(0));
    assert(!core.diskInserted());
    assert(std::remove(diskPath.c_str()) == 0);

    core.write(0x0300, 0x42);
    assert(core.read(0x0300) == 0x42);

    assert(core.pasteText("PRINT 1\n") == 8);
    core.queueKey('A');

    pom2::Core inputCore;
    assert(inputCore.setPaddleButton(0, true));
    assert((inputCore.read(0xC061) & 0x80) != 0);
    assert(inputCore.setPaddleButton(0, false));
    assert((inputCore.read(0xC061) & 0x80) == 0);
    assert(!inputCore.setPaddleButton(3, true));
    assert(!inputCore.lastError().empty());

    // Normalized axes map to the RC endpoints without pulling GLFW/SDL into
    // the core. A $C070 strobe starts both timers at the same CPU cycle.
    inputCore.setJoystickAxes(-1.0f, 1.0f);
    (void)inputCore.read(0xC070);
    assert((inputCore.read(0xC064) & 0x80) == 0);
    assert((inputCore.read(0xC065) & 0x80) != 0);

    // Raw paddle 0 value 20 expires after 20*11 = 220 emulated cycles.
    assert(inputCore.setPaddle(0, 20));
    assert(inputCore.lastError().empty());
    (void)inputCore.read(0xC070);
    assert((inputCore.read(0xC064) & 0x80) != 0);
    const int paddleEarlyCycles = inputCore.run(100);
    assert(paddleEarlyCycles >= 100 && paddleEarlyCycles < 220);
    assert((inputCore.read(0xC064) & 0x80) != 0);
    assert(inputCore.run(200) >= 200);
    assert((inputCore.read(0xC064) & 0x80) == 0);
    assert(!inputCore.setPaddle(4, 128));
    assert(!inputCore.lastError().empty());

    const pom2::FramebufferView frame = core.renderFrame();
    assert(frame.pixels != nullptr);
    assert(frame.width == 280);
    assert(frame.height == 192);

    std::vector<float> silence(256, 1.0f);
    assert(core.pullSpeakerAudio(silence.data(), silence.size(), 44100)
           == silence.size());
    assert(std::all_of(silence.begin(), silence.end(),
                       [](float sample) { return sample == 0.0f; }));
    assert(core.pullSpeakerAudio(nullptr, 16, 44100) == 0);
    assert(core.pullSpeakerAudio(silence.data(), silence.size(), 0) == 0);

    core.hardReset();
    const std::uint64_t before = core.cpuState().cycles;
    const int executed = core.run(16);
    const pom2::CpuState after = core.cpuState();
    assert(executed >= 16);
    assert(after.cycles > before);

    core.setSpeakerVolume(2.0f);
    for (int i = 0; i < 64; ++i) {
        core.run(16);
        (void)core.read(0xC030);
    }
    std::vector<float> speakerPcm(1024, 0.0f);
    assert(core.pullSpeakerAudio(speakerPcm.data(), speakerPcm.size(), 48000)
           == speakerPcm.size());
    assert(std::all_of(speakerPcm.begin(), speakerPcm.end(),
                       [](float sample) {
                           return std::isfinite(sample)
                               && sample >= -1.0f && sample <= 1.0f;
                       }));
    assert(std::any_of(speakerPcm.begin(), speakerPcm.end(),
                       [](float sample) { return std::abs(sample) > 1.0e-6f; }));

    const std::string cassettePath = "pom2_core_api_input.aci";
    const std::string recordingPath = "pom2_core_api_output.aci";
    writeTestCassette(cassettePath);

    pom2::Core cassetteCore;
    cassetteCore.setAudioSampleRate(44100);
    assert(cassetteCore.loadCassette(cassettePath));
    assert(cassetteCore.cassetteState().loaded);
    assert(!cassetteCore.cassetteState().playEngaged);

    // Loading makes a short mechanical click. Drain it so the signal checked
    // below can only come from the cycle-driven program tape.
    std::vector<float> cassetteStereo(4096 * 2, 0.0f);
    assert(cassetteCore.pullAudio(cassetteStereo.data(), 4096, 44100) == 4096);

    cassetteCore.playCassette();
    assert(cassetteCore.cassetteState().playEngaged);
    cassetteCore.pauseCassette(true);
    assert(cassetteCore.cassetteState().paused);
    cassetteCore.pauseCassette(false);
    assert(!cassetteCore.cassetteState().paused);

    // PLAY arms a program tape. The first $C060 read starts transport, then
    // Memory::advanceCycles moves the comparator at the recorded cycle stamp.
    // Polarity: a positive input reads bit 7 = 0, a low one 1 (MAME
    // apple2e.cpp:2180, AppleWin Tape.cpp after Sather 7-5/7-6).
    assert((cassetteCore.read(0xC060) & 0x80) == 0x80);
    assert(cassetteCore.run(1200) >= 1200);
    assert((cassetteCore.read(0xC060) & 0x80) == 0x00);

    // The monitor starts once 40 ms of tape is queued (CassetteDevice
    // kMonitorLeadSecs); 1200 cycles is about 52 samples.
    cassetteCore.run(60000);
    cassetteStereo.assign(512 * 2, 0.0f);
    assert(cassetteCore.pullAudio(cassetteStereo.data(), 512, 44100) == 512);
    bool cassetteAudible = false;
    for (std::size_t i = 0; i < 512; ++i) {
        const float left = cassetteStereo[2 * i];
        const float right = cassetteStereo[2 * i + 1];
        assert(std::isfinite(left) && std::isfinite(right));
        assert(std::abs(left - right) < 1.0e-7f);
        cassetteAudible = cassetteAudible || std::abs(left) > 1.0e-6f;
    }
    assert(cassetteAudible);

    cassetteCore.rewindCassette();
    assert(cassetteCore.cassetteState().rewinding);
    cassetteCore.run(128);
    assert(!cassetteCore.cassetteState().rewinding);
    cassetteCore.playCassette();
    cassetteCore.stopCassette();
    assert(!cassetteCore.cassetteState().playEngaged);

    // A computer reset preserves the physical tape, while $C020 output is
    // captured independently and can be saved by the embedding host.
    cassetteCore.hardReset();
    assert(cassetteCore.cassetteState().loaded);
    cassetteCore.armCassetteRecording();
    cassetteCore.run(256);
    cassetteCore.write(0xC020, 0);
    assert(cassetteCore.cassetteState().hasRecording);
    assert(cassetteCore.saveCassette(recordingPath));
    cassetteCore.clearCassetteRecording();
    assert(!cassetteCore.cassetteState().hasRecording);
    cassetteCore.ejectCassette();
    assert(!cassetteCore.cassetteState().loaded);
    assert(std::remove(cassettePath.c_str()) == 0);
    assert(std::remove(recordingPath.c_str()) == 0);

    pom2::Core audioCore;
    audioCore.hardReset();
    audioCore.run(32);
    assert(audioCore.attachMockingboard());
    assert(audioCore.mockingboardAttached());
    assert(!audioCore.attachMockingboard());
    assert(!audioCore.lastError().empty());

    const auto viaWrite = [&](std::uint16_t offset, std::uint8_t value) {
        audioCore.write(static_cast<std::uint16_t>(0xC400 + offset), value);
    };
    const auto ayWriteLeft = [&](std::uint8_t reg, std::uint8_t value) {
        viaWrite(0x03, 0xFF); // DDRA
        viaWrite(0x02, 0xFF); // DDRB
        viaWrite(0x01, reg);  // ORA
        viaWrite(0x00, 0x07); // latch
        viaWrite(0x00, 0x04); // inactive
        viaWrite(0x01, value);
        viaWrite(0x00, 0x06); // write
        viaWrite(0x00, 0x04); // inactive
    };
    ayWriteLeft(0, 0x00); // tone period $0100
    ayWriteLeft(1, 0x01);
    ayWriteLeft(7, 0x3E); // tone A only
    ayWriteLeft(8, 0x0F); // full channel-A amplitude
    audioCore.run(17045);

    constexpr std::size_t kStereoFrames = 4096;
    std::vector<float> stereo(kStereoFrames * 2, 0.0f);
    assert(audioCore.pullAudio(stereo.data(), kStereoFrames, 44100)
           == kStereoFrames);
    assert(audioCore.pullAudio(stereo.data(), kStereoFrames, 44100)
           == kStereoFrames); // second block: DC filter settled
    double leftPower = 0.0;
    double rightPower = 0.0;
    for (std::size_t i = 0; i < kStereoFrames; ++i) {
        assert(std::isfinite(stereo[2 * i]));
        assert(std::isfinite(stereo[2 * i + 1]));
        leftPower += static_cast<double>(stereo[2 * i]) * stereo[2 * i];
        rightPower += static_cast<double>(stereo[2 * i + 1])
                    * stereo[2 * i + 1];
    }
    const double leftRms = std::sqrt(leftPower / kStereoFrames);
    const double rightRms = std::sqrt(rightPower / kStereoFrames);
    assert(leftRms > 1.0e-3);
    assert(rightRms < leftRms * 0.05);
    assert(audioCore.pullAudio(nullptr, 16, 44100) == 0);

    pom2::Core moved(std::move(core));
    moved.write(0x0301, 0xA5);
    assert(moved.read(0x0301) == 0xA5);

    pom2::Core cmos({
        pom2::CpuModel::CMOS65C02,
        pom2::VideoTiming::PAL,
        true,
    });
    cmos.coldBoot();
    cmos.step();
    assert(cmos.cpuState().cycles > 0);

    assert(!cmos.loadRom("this-rom-does-not-exist.bin"));
    assert(!cmos.lastError().empty());
    return 0;
}
