// POM2 Apple II Emulator — GPL-3.0-or-later
// Run the unmodified //c firmware through the production factory. Raw
// X0/Y0 transitions, IRQ acknowledgement, snapshots and host input are
// checked independently before INITMOUSE/SETMOUSE/READMOUSE integration.
#include "M6502.h"
#include "Memory.h"
#include "IIcMouse.h"
#include "SlotCardFactory.h"

#include <cassert>
#include <algorithm>
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
    auto result = factory.create({"iicmouse", 4, true, profile});
    assert(result.card && !result.fallback);
    auto* mouse = dynamic_cast<IIcMouse*>(result.card.get());
    assert(mouse);
    const int slot = mouse->getSlot();
    const uint16_t cn = 0xC000 + slot*0x100;
    const uint8_t hi = uint8_t(cn >> 8);
    mem.slotBus().plug(4, std::move(result.card));
    cpu.setCpuMode(M6502::CpuMode::CMOS);

    mem.resetSoftSwitches();
    assert(!(mem.memRead(0xC040) & 0x80));
    mem.memWrite(0xC059, 0); // IOUDIS set: annunciator, not mouse enable
    assert(!(mem.memRead(0xC040) & 0x80));
    mem.memWrite(0xC07F, 0);
    mem.memWrite(0xC059, 0);
    assert(mem.memRead(0xC040) & 0x80);
    mouse->setHostMouse(1, 255, false); // X forward, Y backward (8-bit wrap)
    mem.advanceCycles(64);
    assert(!mouse->slotIrqAsserted());
    mem.advanceCycles(1);
    assert(mouse->slotIrqAsserted());
    assert(mem.memRead(0xC015) & 0x80);
    assert(!mouse->slotIrqAsserted()); // acknowledge does not clear axis latches
    assert(mem.memRead(0xC017) & 0x80);
    assert(mem.memRead(0xC066) & 0x80);
    assert(mem.memRead(0xC067) & 0x80);
    (void)mem.memRead(0xC048);
    assert(!(mem.memRead(0xC015) & 0x80));
    assert(!(mem.memRead(0xC017) & 0x80));
    mem.memWrite(0xC05D, 0); mem.memWrite(0xC05F, 0); // select falling edges
    assert(mem.memRead(0xC042) & 0x80);
    assert(mem.memRead(0xC043) & 0x80);
    mouse->setHostMouse(2, 254, true);
    mem.advanceCycles(65);
    cpu.setIrqLine(M6502::IRQ_SRC_VBL, true);
    assert(mouse->slotIrqAsserted());
    (void)mem.memRead(0xC017);
    assert(cpu.getIrqSourceMask() == (1u << M6502::IRQ_SRC_VBL));
    (void)mem.memRead(0xC070);
    assert(cpu.getIrqSourceMask() == 0);
    assert(!(mem.memRead(0xC063) & 0x80)); // button is active low
    assert((mem.memRead(0xC06B) & 0x80) == (mem.memRead(0xC063) & 0x80));

    // Save a pending edge halfway through a sampling period. Memory's
    // snapshot must include this motherboard state, not just rewind SLOT4.
    (void)mem.memRead(0xC048);
    mem.memWrite(0xC05C, 0); mem.memWrite(0xC05E, 0);
    mouse->setHostMouse(3, 253, true);
    mem.advanceCycles(32);
    std::vector<uint8_t> saved;
    mem.appendSnapshotState(saved);
    mem.advanceCycles(33);
    assert(mouse->slotIrqAsserted());
    (void)mem.memRead(0xC048);
    assert(mem.loadSnapshotState(saved.data(), saved.size()));
    assert(!mouse->slotIrqAsserted());
    mem.advanceCycles(32);
    assert(!mouse->slotIrqAsserted());
    mem.advanceCycles(1);
    assert(mouse->slotIrqAsserted());
    std::vector<uint8_t> pending;
    mouse->saveIicMouseState(pending);
    (void)mem.memRead(0xC048);
    assert(mouse->loadIicMouseState(pending.data(), pending.size()));
    assert(mouse->slotIrqAsserted());
    auto bad = pending; bad[14] = 65;
    assert(!mouse->loadIicMouseState(bad.data(), bad.size()));
    assert(mouse->slotIrqAsserted()); // invalid restore is inert

    auto call = [&](uint8_t index, uint8_t a) {
        const uint8_t entry = mem.memRead(cn + index);
        // Standard ProDOS mouse calling convention; the RTS must return
        // to $0309 within a bounded budget. Native IOU motion requires IRQs.
        const uint8_t stub[] = {0x58, 0xA2, hi, 0xA0, uint8_t(slot*16),
                               0x20, entry, hi, 0xEA, 0x4C, 0x09, 0x03};
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
        return mem.memRead(low+slot) | (mem.memRead(low+0x100+slot) << 8);
    };
    for (int reset = 0; reset < 2; ++reset) {
        mouse->setHostMouse(0, 0, false);
        mem.resetSoftSwitches();
        cpu.hardReset();
        for (int i=0; i<256; ++i)
            assert(mem.memRead(cn+i) == mem.internalIORomData()[cn-0xC000+i]);
        mem.slotBus().reset();
        call(0x19, 0); // native INITMOUSE
        call(0x12, 1); // polling API; the IOU still needs native hardware IRQs
        call(0x14, 0); // READMOUSE
        const int x = coord(0x478), y = coord(0x4F8);
        // The ROM counts the selected edge once per complete X0/Y0 period
        // (two transitions). Give its IRQ handler time to service each edge.
        for (int step=1; step<=20; ++step) {
            mouse->setHostMouse(step, std::min(step,12), true);
            cpu.run(2000);
        }
        call(0x14, 0);
        assert(coord(0x478) == x+10 && coord(0x4F8) == y+6);
        assert(mem.memRead(0x778+slot) & 0x80); // current button
        mouse->setHostMouse(20, 12, false);
        call(0x14, 0);
        assert(!(mem.memRead(0x778+slot) & 0x80));
    }
    std::printf("PASS mouse firmware: %s, profile %d\n", rom.c_str(), int(profile));
}

int main()
{
    using P = pom2::SystemProfile;
    const struct { P profile; const char* rom; } cases[] = {
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
