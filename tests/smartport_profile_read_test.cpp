// Boot real ProDOS, then execute its MLI to read a file from the SECOND
// 800K drive. No host readBlock shortcuts: //c firmware uses the rear-port
// protocol, Liron uses its real EPROM, SmartPortCard is checked separately.
#include "Disk35Image.h"
#include "DiskIICard.h"
#include "IIcExternalSmartPort.h"
#include "IWMDevice.h"
#include "LironCard.h"
#include "M6502.h"
#include "Memory.h"
#include "ResourcePaths.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "SmartPortHub.h"
#include "Sony35Drive.h"
#include "TestTempPath.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool ok, const std::string& why)
{ if (!ok) throw std::runtime_error(why); }
unsigned word(const std::vector<uint8_t>& b, size_t off)
{ return b.at(off) | (b.at(off + 1) << 8); }
void putWord(std::vector<uint8_t>& b, size_t off, unsigned v)
{ b.at(off) = v; b.at(off + 1) = v >> 8; }
size_t entry(const std::vector<uint8_t>& b, const std::string& name)
{
    for (unsigned block = 2; block; block = word(b, block * 512 + 2))
        for (unsigned i = 0; i < 13; ++i) {
            const size_t off = block * 512 + 4 + i * 39;
            const unsigned n = b.at(off) & 15;
            if (std::string(b.begin() + off + 1, b.begin() + off + 1 + n) == name)
                return off;
        }
    throw std::runtime_error("fixture lacks " + name);
}
unsigned dataBlock(const std::vector<uint8_t>& b, size_t e, unsigned n)
{
    const size_t index = word(b, e + 17) * 512;
    require((b.at(e) >> 4) == 2, "fixture file must be sapling");
    return b.at(index + n) | (b.at(index + 256 + n) << 8);
}
void write(const std::string& path, const std::vector<uint8_t>& b)
{
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), b.size());
    require(f.good(), "cannot write scratch image");
}

// Only documented NMOS 6502 instructions. OPEN /SECOND/DATA, READ 1200
// bytes to $4000, CLOSE. $0301=$5A means success; $EE means error ($0302).
std::vector<uint8_t> readProgram()
{
    std::vector<uint8_t> b;
    std::vector<size_t> branches;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { b.insert(b.end(), bytes); };
    auto mli = [&](uint8_t cmd, uint8_t params) {
        emit({0x20, 0x00, 0xBF, cmd, params, 0x21, 0xB0, 0});
        branches.push_back(b.size() - 1);
    };
    mli(0xC8, 0x00);
    emit({0xAD, 0x05, 0x21, 0x8D, 0x11, 0x21, 0x8D, 0x19, 0x21});
    mli(0xCA, 0x10);
    mli(0xCC, 0x18);
    emit({0xA9, 0x5A, 0x8D, 0x01, 0x03, 0x4C, 0, 0x20});
    const size_t successJump = b.size() - 2;
    const size_t fail = b.size();
    emit({0x8D, 0x02, 0x03, 0xA9, 0xEE, 0x8D, 0x01, 0x03});
    const size_t stop = b.size();
    emit({0x4C, static_cast<uint8_t>(stop), 0x20});
    b[successJump] = stop;
    for (size_t off : branches) b[off] = fail - (off + 1);
    return b;
}

struct Machine { const char* name; const char* rom; bool iie; bool cmos; bool liron; bool plus; bool cClass; int slot; int externalSony = 0; };
void run(const Machine& machine, const std::string& boot, const std::string& second,
         const std::vector<uint8_t>& expected)
{
    const std::string rom = pom2::findResource(machine.rom);
    require(!rom.empty(), std::string("missing ROM: ") + machine.rom);
    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(machine.iie);
    mem.clearRam();
    mem.resetSoftSwitches();
    pom2::IWMDevice iwm;
    pom2::SmartPortHub hub;
    pom2::Disk35Image internal, external, external2;
    internal.setWriteBackEnabled(false);
    external.setWriteBackEnabled(false);
    external2.setWriteBackEnabled(false);
    pom2::Sony35Drive sonyInt, sonyExt, sonyExt2;
    sonyInt.setImage(&internal); sonyExt.setImage(&external); sonyExt2.setImage(&external2);
    hub.attach(&iwm); hub.setSony35(&sonyInt, machine.externalSony ? &sonyExt : nullptr,
                                  machine.externalSony == 2 ? &sonyExt2 : nullptr);
    mem.setIWM(&iwm); mem.setSmartPortHub(&hub);
    pom2::IIcExternalSmartPort port(&mem.slotBus());
    mem.setExternalSmartPort(&port);
    auto diskII = std::make_unique<DiskIICard>(6);
    diskII->setIWM(&iwm);
    mem.slotBus().plug(6, std::move(diskII));
    require(mem.loadAppleIIRom(rom.c_str(), machine.cClass), "ROM load failed");
    std::string error;
    if (machine.liron) {
        auto card = std::make_unique<pom2::LironCard>(machine.slot);
        require(card->romLoaded(), "missing Liron EPROM");
        card->setUnitCount(2);
        card->setBayWriteBack(0, false); card->setBayWriteBack(1, false);
        require(card->mountBay(0, boot, error), error);
        require(card->mountBay(1, second, error), error);
        mem.slotBus().plug(machine.slot, std::move(card));
    } else if (!machine.externalSony || machine.externalSony == 3) {
        auto card = std::make_unique<pom2::SmartPortCard>(machine.slot);
        require(card->loadLironRom(pom2::findResource("roms/liron.rom")), "missing Liron EPROM");
        card->setUnitCount(2);
        for (int bay = 0; bay < (machine.plus ? 1 : 2); ++bay) {
            auto unit = std::make_unique<pom2::SmartPort35Unit>();
            unit->setWriteBackEnabled(false);
            require(unit->loadImage(machine.plus || bay ? second : boot), "800K mount failed");
            card->setUnit(bay, std::move(unit));
        }
        mem.slotBus().plug(machine.slot, std::move(card));
    }
    if (machine.plus) {
        require(internal.loadFile(boot), internal.lastError());
        sonyInt.notifyMediaChange();
        if (machine.externalSony == 1 || machine.externalSony == 2) {
            auto& image = machine.externalSony == 1 ? external : external2;
            auto& drive = machine.externalSony == 1 ? sonyExt : sonyExt2;
            require(image.loadFile(second), image.lastError());
            drive.notifyMediaChange();
        }
    }
    cpu.setCpuMode(machine.cmos ? M6502::CpuMode::CMOS : M6502::CpuMode::NMOS);
    cpu.hardReset();
    // //c-class uses its real reset firmware and external bus. Slotted
    // machines enter the card ROM as PR#slot would on the real machine.
    if (!machine.cClass) cpu.setProgramCounter(0xC000 + machine.slot * 256);
    for (long cycles = 0; cycles < 90'000'000 &&
         !(mem.peekMainRam(0x0300) == 0x5A && cpu.getProgramCounter() == 0x2005);) {
        cycles += cpu.run(4096); iwm.tick(mem.getCycleCounter());
    }
    require(mem.peekMainRam(0x0300) == 0x5A && cpu.getProgramCounter() == 0x2005 &&
            mem.peekMainRam(0xBF00) == 0x4C,
            "ProDOS did not load and execute the test SYSTEM");
    // Install the guest probe without resetting the running ProDOS kernel.
    const auto code = readProgram();
    for (size_t i = 0; i < code.size(); ++i) mem.memWrite(0x2000 + i, code[i]);
    const uint8_t params[] = {3, 0x20, 0x21, 0x00, 0x30, 0};
    for (size_t i = 0; i < sizeof params; ++i) mem.memWrite(0x2100 + i, params[i]);
    const uint8_t read[] = {4, 0, 0x00, 0x40, 0xB0, 0x04, 0, 0};
    for (size_t i = 0; i < sizeof read; ++i) mem.memWrite(0x2110 + i, read[i]);
    mem.memWrite(0x2118, 1); mem.memWrite(0x2119, 0);
    const std::string path = "/SECOND/DATA";
    mem.memWrite(0x2120, path.size());
    for (size_t i = 0; i < path.size(); ++i) mem.memWrite(0x2121 + i, path[i]);
    mem.memWrite(0x0301, 0); mem.memWrite(0x0302, 0);
    cpu.setProgramCounter(0x2000);
    for (long cycles = 0; cycles < 30'000'000 && !mem.peekMainRam(0x0301);) {
        cycles += cpu.run(4096); iwm.tick(mem.getCycleCounter());
    }
    require(mem.peekMainRam(0x0301) == 0x5A,
            "MLI OPEN/READ/CLOSE failed, error " + std::to_string(mem.peekMainRam(0x0302)));
    require(mem.peekMainRam(0x2116) == 0xB0 && mem.peekMainRam(0x2117) == 4, "short read");
    for (size_t i = 0; i < expected.size(); ++i)
        require(mem.peekMainRam(0x4000 + i) == expected[i], "second-drive file differs at " + std::to_string(i));
    std::printf("PASS %s: real ProDOS boot + 1200-byte MLI read on second 800K disk\n", machine.name);
}
} // namespace

int main()
{
    const std::string fixture = pom2::findResource("disks_3.5/A2DeskTop-1.5-en_800k.2mg");
    if (fixture.empty()) { std::puts("SKIP: need A2DeskTop 800K fixture"); return 77; }
    const std::string bootPath = pom2test::tempPath("pom2_sp_matrix_boot.po");
    const std::string secondPath = pom2test::tempPath("pom2_sp_matrix_second.po");
    int result = 0;
    try {
        std::ifstream f(fixture, std::ios::binary);
        std::vector<uint8_t> wrapped((std::istreambuf_iterator<char>(f)), {});
        const size_t offset = word(wrapped, 24);
        require(wrapped.size() >= offset + 819200, "bad 2mg fixture");
        std::vector<uint8_t> boot(wrapped.begin() + offset, wrapped.begin() + offset + 819200);
        std::vector<uint8_t> second = boot;
        const size_t e = entry(boot, "CLOCK.SYSTEM");
        const unsigned block = dataBlock(boot, e, 0);
        const uint8_t stub[] = {0xA9, 0x5A, 0x8D, 0x00, 0x03, 0x4C, 0x05, 0x20};
        std::copy(std::begin(stub), std::end(stub), boot.begin() + block * 512);
        boot[e] = 0x10 | (boot[e] & 15); putWord(boot, e + 17, block);
        putWord(boot, e + 19, 1); putWord(boot, e + 21, sizeof stub); boot[e + 23] = 0;
        second[1028] = 0xF6;
        std::copy_n("SECOND", 6, second.begin() + 1029);
        const size_t data = entry(second, "CLOCK.SYSTEM");
        std::vector<uint8_t> expected(1200);
        for (size_t i = 0; i < expected.size(); ++i) {
            expected[i] = (i * 73 + (i >> 5)) ^ 0xA5;
            second[dataBlock(second, data, i / 512) * 512 + i % 512] = expected[i];
        }
        second[data] = 0x24;
        std::fill_n(second.begin() + data + 1, 15, 0);
        std::copy_n("DATA", 4, second.begin() + data + 1);
        second[data + 16] = 6;
        putWord(second, data + 21, expected.size()); second[data + 23] = 0;
        write(bootPath, boot); write(secondPath, second);
        const Machine machines[] = {
            {"//c+ internal + second external Sony", "roms/apple2cp.rom", true, true, false, true, true, 5, 2},
            {"//c+ internal + external Sony", "roms/apple2cp.rom", true, true, false, true, true, 5, true},
            {"//c+ internal + empty external Sony + UniDisk", "roms/apple2cp.rom", true, true, false, true, true, 5, 3},
            {"//c+ Sony + external SmartPort", "roms/apple2cp.rom", true, true, false, true, true, 5},
            {"//c rev0 external SmartPort", "roms/apple2c-32Kv0.rom", true, true, false, false, true, 5},
            {"IIe Liron slot 5", "roms/apple2e.rom", true, true, true, false, false, 5},
            {"II+ Liron slot 5", "roms/apple2p.rom", false, false, true, false, false, 5},
            {"IIe SmartPort slot 7", "roms/apple2e.rom", true, true, false, false, false, 7},
            {"II+ SmartPort slot 7", "roms/apple2p.rom", false, false, false, false, false, 7},
        };
        for (const auto& machine : machines) run(machine, bootPath, secondPath, expected);
    } catch (const std::exception& e) { std::printf("FAIL: %s\n", e.what()); result = 1; }
    std::filesystem::remove(bootPath); std::filesystem::remove(secondPath);
    return result;
}
