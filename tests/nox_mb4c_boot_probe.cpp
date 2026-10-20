// POM2 — GPL-3.0-or-later
// Manual integration check for tools/make_nox_mb4c_fix.py. No game is bundled.
// Run from the source root with a v1.37 original or patched HDV argument.
// The original reproduces the VBL IRQ storm; the patched image must pass.
#include "IIcMouse.h"
#include "M6502.h"
#include "Memory.h"
#include "Mockingboard.h"
#include "ResourcePaths.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "SuperSerialCard.h"
#include "TestTempPath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>

int main(int argc, char** argv)
{
    if (argc != 2) { std::fprintf(stderr, "Usage: %s NOX.hdv\n", argv[0]); return 2; }
    const auto rom = pom2::findResource("roms/apple2c-32Kv0.rom");
    if (rom.empty()) { std::puts("Need the //c ROM"); return 77; }
    const std::filesystem::path scratch = pom2test::tempPath("nox_mb4c.hdv");
    std::filesystem::copy_file(argv[1], scratch, std::filesystem::copy_options::overwrite_existing);
    int result = 1;
    {
        Memory mem;
        M6502 cpu(&mem);
        mem.setCpu(&cpu);
        mem.clearRam();
        mem.resetSoftSwitches();
        mem.setIIEMode(true);
        if (!mem.loadAppleIIRom(rom.c_str(), true)) return 1;
        mem.setVideoStandard(VideoStandard::PAL);
        for (int s = 1; s <= 2; ++s)
            mem.slotBus().plug(s, std::make_unique<SuperSerialCard>(s));
        mem.slotBus().plug(4, std::make_unique<IIcMouse>());
        auto card = std::make_unique<MockingboardCard>(3);
        auto* mb = card.get();
        card->setCpu(&cpu);
        card->setCpuClock(1015625.0);
        card->setStandardClock(1015625.0);
        mem.slotBus().plug(3, std::move(card));
        auto sp = std::make_unique<pom2::SmartPortCard>(5);
        auto unit = std::make_unique<pom2::SmartPortHdvUnit>();
        if (!unit->loadImage(scratch.string())) return 1;
        sp->setUnit(0, std::move(unit));
        mem.slotBus().plug(5, std::move(sp));
        mem.setIicSmartPortArmed(true);
        // The explicit slot boot's Monitor text/keyboard setup.
        for (int a = 0x400; a < 0x800; ++a) mem.memWrite(a, 0xA0);
        const uint8_t zp[][2] = {{0x21,40}, {0x23,24}, {0x29,4}, {0x32,255},
                                {0x36,0xF0}, {0x37,0xFD}, {0x38,0x1B}, {0x39,0xFD}};
        for (const auto& p : zp) mem.memWrite(p[0], p[1]);
        cpu.setCpuMode(M6502::CpuMode::CMOS);
        cpu.hardReset();
        cpu.setProgramCounter(0xC500);
        uint64_t total = 0, nextAudio = 0;
        bool keySent = false;
        float peak = 0;
        while (total < 125'000'000 && !cpu.isHalted()) {
            total += cpu.run(4096);
            if (total >= nextAudio) {
                nextAudio += 23583; // 1024 frames at 44.1 kHz, PAL clock
                float left[1024]{}, right[1024]{};
                mb->audioSource()->fillAudioBufferStereo(left, right, 1024);
                for (int i = 0; i < 1024; ++i)
                    peak = std::max({peak, std::abs(left[i]), std::abs(right[i])});
            }
            if (!keySent && total > 100'000'000) {
                mem.queueKey(13);
                keySent = true;
            }
        }
        const auto writes = mb->getViaWriteCount(0);
        std::printf("PC=$%04X IRQ=$%X VIA=%u AY=%u audio peak=%.5f\n",
                    cpu.getProgramCounter(), cpu.getIrqSourceMask(), writes,
                    mb->getAyWriteCount(0), peak);
        // A stopped/muted player alone cannot satisfy this check. The
        // original gets >1 million VIA writes from the unacknowledged VBL.
        result = cpu.isHalted() || cpu.getIrqSourceMask() != 0 ||
                 mb->getAyWriteCount(0) < 1000 || writes > 100000 || peak < 0.001f;
    }
    std::filesystem::remove(scratch);
    return result;
}
