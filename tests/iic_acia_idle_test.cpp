// POM2 Apple II Emulator — GPL-3.0-or-later
// Regression: on a //c with no SSC model in slots 1/2, the ACIA registers
// ($C098-$C09F, $C0A8-$C0AF) read the floating bus. The //c ROM polls both
// ACIA status registers on every IRQ; a video byte with bit 7 set passed for
// a serial interrupt, the ROM swallowed it, and the VBL IRQ never reached the
// program's $03FE handler (ChromaBreak's DHGR screen is cleared to $80, so
// its timing_ticks stuck at 2). The ACIAs are soldered to the //c board, so
// they must answer as an idle 6551 whether or not a card models them.
#include "M6502.h"
#include "Memory.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>

static std::string locate(std::string_view name)
{
    for (const char* prefix : {"", "../", "../../"}) {
        const auto path = std::string(prefix) + std::string(name);
        if (std::filesystem::exists(path)) return path;
    }
    return {};
}

// Fill the video pages, main and aux, so the floating bus reads $FF.
static void poisonVideo(Memory& mem)
{
    for (int aux = 0; aux < 2; ++aux) {
        mem.memWrite(aux ? 0xC005 : 0xC004, 0);           // RAMWRT
        for (uint16_t a = 0x0400; a < 0x0C00; ++a) mem.memWrite(a, 0xFF);
        for (uint32_t a = 0x2000; a < 0x6000; ++a) mem.memWrite(uint16_t(a), 0xFF);
    }
    mem.memWrite(0xC004, 0);
}

static void exercise(const std::string& rom)
{
    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(true);
    assert(mem.loadAppleIIRom(rom.c_str(), true));
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    poisonVideo(mem);
    assert(mem.peekFloatingBus() == 0xFF);

    // End to end: the ROM's IRQ handler must hand the VBL interrupt on to
    // the user vector at $03FE instead of claiming it as serial.
    const uint8_t handler[] = {
        0xE6, 0x06,             // INC $06
        0xAD, 0x70, 0xC0,       // LDA $C070  (acknowledge VBLINT)
        0x40,                   // RTI
    };
    for (unsigned i = 0; i < sizeof(handler); ++i) mem.memWrite(0x0350 + i, handler[i]);
    const uint8_t loop[] = {0x58, 0x4C, 0x01, 0x03};  // CLI ; JMP $0301
    for (unsigned i = 0; i < sizeof(loop); ++i) mem.memWrite(0x0300 + i, loop[i]);
    mem.memWrite(0x03FE, 0x50);
    mem.memWrite(0x03FF, 0x03);
    mem.memWrite(0x0006, 0);
    mem.memWrite(0xC07F, 0);   // IOUDIS clear
    mem.memWrite(0xC05B, 0);   // enable VBL interrupt
    cpu.setProgramCounter(0x0300);
    cpu.run(17045 * 10);
    const uint8_t ticks = mem.memRead(0x0006);
    if (ticks < 8)
        std::fprintf(stderr, "%s: user IRQ ran %u times in 10 frames\n",
                     rom.c_str(), unsigned(ticks));
    assert(ticks >= 8);

    // Register-level: idle 6551, A0-A1 decode, both ports.
    for (uint16_t base : {uint16_t{0xC098}, uint16_t{0xC0A8}}) {
        for (uint16_t off = 0; off < 8; ++off) {
            const uint8_t v = mem.memRead(uint16_t(base + off));
            const uint8_t want = (off & 3) == 1 ? 0x70 : 0x00;
            if (v != want)
                std::fprintf(stderr, "%s: $%04X = $%02X, want $%02X\n",
                             rom.c_str(), base + off, v, want);
            assert(v == want);
        }
    }
    std::printf("PASS iic acia idle: %s (%u VBL IRQs)\n", rom.c_str(), unsigned(ticks));
}

int main()
{
    int tested = 0;
    for (const char* rom : {"roms/apple2c-32Kv0.rom", "roms/apple2c-16K.rom",
                            "roms/apple2cp.rom"}) {
        const auto path = locate(rom);
        if (path.empty()) continue;
        exercise(path);
        ++tested;
    }
    return tested ? 0 : 77;
}
