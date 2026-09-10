// POM2 Apple II Emulator — GPL-3.0-or-later
// Regression: 816Paint/DeskTop INITMOUSE spun at $C42B forever on //c.
// Execute the real EPROM through the production card factory, then use
// READMOUSE to verify motion/buttons and the independent //c VBLINT latch.
#include "M6502.h"
#include "Memory.h"
#include "MouseCardAppleWin.h"
#include "SlotCardFactory.h"

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

static void exercise(pom2::SystemProfile profile, const std::string& rom)
{
    const auto& cfg = pom2::profileConfig(profile);
    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(true);
    mem.setVideoStandard(cfg.videoStandard);
    assert(mem.loadAppleIIRom(rom.c_str(), true));
    pom2::SlotCardFactory factory(locate);
    auto result = factory.create({"mouseaw", 4, true, profile});
    assert(result.card && !result.fallback);
    auto* mouse = dynamic_cast<MouseCardAppleWin*>(result.card.get());
    assert(mouse);
    mem.slotBus().plug(4, std::move(result.card));
    cpu.setCpuMode(M6502::CpuMode::CMOS);

    auto call = [&](uint8_t entry, uint8_t a) {
        // Standard ProDOS mouse calling convention; the RTS must return
        // to $0309 within a bounded budget, even with VBLINT disabled.
        const uint8_t stub[] = {0x78, 0xA2, 0xC4, 0xA0, 0x40,
                               0x20, entry, 0xC4, 0xEA, 0x4C, 0x09, 0x03};
        for (unsigned i = 0; i < sizeof(stub); ++i) mem.memWrite(0x300+i, stub[i]);
        cpu.setAccumulator(a);
        cpu.setProgramCounter(0x300);
        int budget = 200000;
        while (cpu.getProgramCounter() != 0x309 && budget > 0) budget -= cpu.run(1);
        if (budget <= 0) std::fprintf(stderr, "%s: mouse entry %02x stuck at %04x\n",
                                     rom.c_str(), entry, cpu.getProgramCounter());
        assert(budget > 0);
    };
    auto coord = [&](uint16_t low) {
        return mem.memRead(low+4) | (mem.memRead(low+0x100+4) << 8);
    };
    for (int reset = 0; reset < 2; ++reset) {
        mouse->setHostMouse(0, 0, false);
        cpu.hardReset();
        mem.slotBus().reset();
        call(0xBC, 0); // INITMOUSE (includes the offending calibration loop)
        call(0xB3, 1); // SETMOUSE: polling, no mouse IRQ
        call(0x9B, 0); // READMOUSE
        const int x = coord(0x478), y = coord(0x4F8);
        mouse->setHostMouse(20, 12, true);
        call(0x9B, 0);
        assert(coord(0x478) == x+20 && coord(0x4F8) == y+12);
        assert(mem.memRead(0x778+4) & 0x80); // current button
        mouse->setHostMouse(20, 12, false);
        call(0x9B, 0);
        assert(!(mem.memRead(0x778+4) & 0x80));

        if (cfg.noPhysicalSlots) {
            assert(!(mem.memRead(0xC019) & 0x80));
            mem.memWrite(0xC07F, 0); // IOU enabled
            mem.memWrite(0xC05B, 0); // enable VBL interrupt
            cpu.setStatusRegister(cpu.getStatusRegister() | 4);
            cpu.run(45000);
            assert(mem.memRead(0xC019) & 0x80);
            call(0xBC, 0); // also succeeds with VBLINT already latched
            assert(mem.memRead(0xC019) & 0x80); // read/init never acknowledge it
            mem.memWrite(0xC05A, 0);
            assert(!(mem.memRead(0xC019) & 0x80));
        }
    }
    std::printf("PASS mouse firmware: %s, profile %d\n", rom.c_str(), int(profile));
}

int main()
{
    if (locate("roms/mouse_341-0270-c.bin").empty()) return 77;
    using P = pom2::SystemProfile;
    const struct { P profile; const char* rom; } cases[] = {
        {P::AppleIIe, "roms/apple2e.rom"},
        {P::AppleIIc, "roms/apple2c-32Kv0.rom"},
        {P::AppleIIc, "roms/apple2c-16K.rom"},
        {P::AppleIIcPlus, "roms/apple2cp.rom"},
        {P::AppleIIcPAL, "roms/apple2c-32Kv0.rom"},
    };
    int tested = 0;
    for (const auto& c : cases) {
        const auto rom = locate(c.rom);
        if (rom.empty()) continue;
        exercise(c.profile, rom);
        ++tested;
    }
    return tested ? 0 : 77;
}
