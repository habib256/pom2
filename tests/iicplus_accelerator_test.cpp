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

// The //c Plus's built-in accelerator, driven by its own ROM.
//
// The machine's 4 MHz is the CGGA, a licensed Zip that answers the Zip's
// registers (MAME apple2e.cpp header: fast = $08 → $C05B, normal = $08 →
// $C05A, unlock = $5A ×4, lock = $A5). POM2 seats a ZipChip on the profile
// (EmulationController::installSocketAccelerator) on a 1× budget, and the
// real ROM does the rest at every reset: unlock, fast, slot mask, lock —
// then `LDA $C000 / STA $CE04 … CMP #$9B` ($FD82, $FDD3): Esc held through
// the reset puts it back to 1 MHz. Until 2026-10-21 the profile was a fixed
// 4× budget and Esc did nothing.

#include "EmulationController.h"
#include "M6502.h"
#include "Memory.h"
#include "ResourcePaths.h"
#include "ZipChip.h"

#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace {
int failures = 0;
void expect(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
void runFrames(EmulationController& c, int n)
{
    for (int i = 0; i < n; ++i) c.tickFrame();
}
} // namespace

int main()
{
    const std::string rom = pom2::findResource("roms/apple2cp.rom");
    if (rom.empty()) { std::printf("skip: needs roms/apple2cp.rom\n"); return 0; }

    EmulationController c;
    c.setCyclesPerFrame(17045);
    c.setVideoStandard(VideoStandard::NTSC);
    {
        auto st = c.lockState();
        st.memory().setIIEMode(true);
        expect(st.memory().loadAppleIIRom(rom.c_str(), /*pickLower16KFor32K=*/true),
               "the //c Plus ROM loads");
        st.cpu().setCpuMode(M6502::CpuMode::CMOS);
        st.memory().resetSoftSwitches();
    }
    {
        std::lock_guard<std::mutex> lk(c.stateMutex());
        c.installSocketAccelerator();
    }
    pom2::ZipChip& zip = c.iicPlusAccelerator();
    {
        auto st = c.lockState();
        expect(st.memory().isIIcPlus(), "the ROM is a //c Plus");
        expect(st.memory().cpuSocketAccelerator() == &zip,
               "the //c Plus seats its own accelerator, not the user's Zip Chip");
    }
    c.setMode(EmulationController::Mode::Running);

    // Power-on: the ROM unlocks, sets it fast, programs the slot mask and
    // locks it again.
    c.hardReset();
    runFrames(c, 120);
    expect(zip.fast(), "after a reset the ROM leaves the accelerator fast");
    expect(!zip.unlocked(), "…and locked");
    {
        auto st = c.lockState();
        const double m = st.memory().cpuSpeedMultiplier();
        std::printf("  //c Plus after reset: fast=%d multiplier=%.2f slot mask=$%02X\n",
                    zip.fast(), m, zip.slotSpeakerMask());
        expect(zip.delayActive(st.memory().getCycleCounter()) ||
               std::fabs(m - pom2::ZipChip::kFastHz / 1022727.0) < 0.01,
               "the frame loop runs it at 4 MHz");
    }
    expect(c.getCyclesPerFrame() == 17045, "the budget is 1x — the 4x is the chip's");

    // Code that touches no slot or speaker runs at the full 4 MHz once the
    // ROM's own disk polling has stopped opening 52 ms slow windows.
    {
        auto st = c.lockState();
        st.memory().memWrite(0x0300, 0x4C);   // JMP $0300
        st.memory().memWrite(0x0301, 0x00);
        st.memory().memWrite(0x0302, 0x03);
        st.cpu().setProgramCounter(0x0300);
    }
    runFrames(c, 10);
    {
        auto st = c.lockState();
        const double m = st.memory().cpuSpeedMultiplier();
        std::printf("  idle loop: multiplier=%.2f\n", m);
        expect(std::fabs(m - pom2::ZipChip::kFastHz / 1022727.0) < 0.01,
               "an idle loop runs at 4 MHz");
    }

    // Esc held through the reset: the key and its strobe survive the reset
    // line, the ROM reads $9B after its delay and slows the accelerator.
    {
        auto st = c.lockState();
        st.memory().queueKey(0x9B);
    }
    c.hardReset();
    runFrames(c, 120);
    expect(!zip.fast(), "Esc held at reset puts the //c Plus at 1 MHz");
    {
        auto st = c.lockState();
        const double m = st.memory().cpuSpeedMultiplier();
        std::printf("  with Esc: fast=%d multiplier=%.2f\n", zip.fast(), m);
        expect(std::fabs(m - 1.0) < 1e-9, "…and the frame loop runs at 1x");
    }

    // A reset without Esc goes back to 4 MHz.
    {
        auto st = c.lockState();
        (void)st.memory().memRead(0xC010);   // the key is gone
        st.memory().queueKey(0x8D);          // something else in the latch
        (void)st.memory().memRead(0xC010);
    }
    c.hardReset();
    runFrames(c, 120);
    expect(zip.fast(), "a reset without Esc is back at 4 MHz");

    // Esc held through the POWER switch: the latch is cleared at power-on,
    // but the keyboard encoder's first scan finds the key still down and
    // strobes it, so the ROM slows the accelerator as on Ctrl-Reset.
    constexpr uint16_t kEscToken = 256;      // GLFW_KEY_ESCAPE
    {
        auto st = c.lockState();
        st.memory().setKeyHeld(kEscToken, true);
        st.memory().queueKey(0x9B);
    }
    c.coldBoot();
    runFrames(c, 120);
    expect(!zip.fast(), "Esc held at power-on puts the //c Plus at 1 MHz");
    {
        auto st = c.lockState();
        (void)st.memory().memRead(0xC010);
        st.memory().setKeyHeld(kEscToken, false);
        st.memory().queueKey(0x9B);          // typed and released: latched only
    }
    c.coldBoot();
    runFrames(c, 120);
    expect(zip.fast(), "a key released before power-on is not seen: 4 MHz");

    // A //c Plus snapshot from before 2026-10-21 carries the user's //e Zip
    // (boot-fast byte 0 when that option booted slow). It is not this
    // chip's state: the restore takes the reset state, 4 MHz.
    {
        pom2::ZipChip socketZip;
        socketZip.setBootFast(false);
        socketZip.onReset(0);
        std::vector<uint8_t> blob;
        socketZip.appendSnapshotState(blob);
        expect(zip.loadSnapshotState(blob.data(), blob.size()), "the old blob is accepted");
        expect(zip.fast() && !zip.unlocked(), "…and leaves the //c Plus chip fast and locked");
        std::vector<uint8_t> own;
        zip.ioWrite(0x5B, 0x00, 0);          // locked: ignored, still fast
        zip.appendSnapshotState(own);
        pom2::ZipChip copy;
        copy.setSoldered(true);
        expect(copy.loadSnapshotState(own.data(), own.size()) && copy.fast(),
               "the chip's own blob round-trips");
    }

    c.setMode(EmulationController::Mode::Stopped);
    if (failures) return 1;
    std::printf("OK iicplus_accelerator\n");
    return 0;
}
