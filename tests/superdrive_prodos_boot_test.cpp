#include "Disk35Image.h"
#include "M6502.h"
#include "Memory.h"
#include "ResourcePaths.h"
#include "SlotBus.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace fs = std::filesystem;

int main()
{
    const std::string rom = pom2::findResource("roms/apple2e.rom");
    const std::string source = pom2::findResource("disks_3.5/CardCat 1.94.po");
    if (rom.empty() || source.empty()) {
        std::puts("superdrive_prodos_boot: SKIP (Apple IIe ROM or CardCat disk absent)");
        return 77;
    }
    const fs::path disk = fs::temp_directory_path() / "pom2_superdrive_boot_1440.po";
    fs::copy_file(source, disk, fs::copy_options::overwrite_existing);
    fs::resize_file(disk, pom2::Disk35Image::kBytesPerImage1440);

    Memory mem;
    M6502 cpu(&mem);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    assert(mem.loadAppleIIRom(rom.c_str(), true));
    auto card = std::make_unique<pom2::SmartPortCard>(5);
    card->setSuperDriveMode(true);
    card->setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
    std::string error;
    assert(card->mountBay(0, disk.string(), error));
    assert(card->unit(0)->blockCount() == 2880);
    mem.slotBus().plug(5, std::move(card));

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    cpu.setProgramCounter(0xC500);
    for (long i = 0; i < 40'000'000L; ++i) cpu.step();

    constexpr uint16_t rows[24] = {
        0x400, 0x480, 0x500, 0x580, 0x600, 0x680, 0x700, 0x780,
        0x428, 0x4A8, 0x528, 0x5A8, 0x628, 0x6A8, 0x728, 0x7A8,
        0x450, 0x4D0, 0x550, 0x5D0, 0x650, 0x6D0, 0x750, 0x7D0,
    };
    std::string screen;
    for (uint16_t row : rows)
        for (int col = 0; col < 40; ++col) {
            const uint8_t value = mem.peekMainRam(static_cast<uint16_t>(row + col));
            const uint8_t ascii = value >= 0xA0 ? value & 0x7F :
                                  value >= 0x80 ? (value & 0x1F) | 0x40 :
                                  value >= 0x60 ? value : value | 0x40;
            screen += ascii >= 0x20 && ascii < 0x7F ? char(ascii) : ' ';
        }
    std::error_code ignored;
    fs::remove(disk, ignored);
    assert(screen.find("Card Cat") != std::string::npos);
    std::puts("superdrive_prodos_boot: Apple IIe booted ProDOS from 2880-block card");
}
