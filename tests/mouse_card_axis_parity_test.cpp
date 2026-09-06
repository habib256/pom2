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

// MouseCard X/Y axis-parity integration test.
//
// Boots the REAL Apple Mouse Card firmware (MCU 341-0269 + slot ROM
// 341-0270-c) on a full M6502 + Memory + SlotBus, drives the ProDOS
// mouse firmware (InitMouse / SetMouse / ReadMouse) from a tiny 6502
// stub poked into RAM, and feeds an IDENTICAL host-motion ramp into the
// X and Y axes. After the ramp it reads the Apple cursor position back
// from the standard ProDOS screen holes and asserts X tracked the host
// to the SAME extent as Y (parity).
//
// This pins the symptom "[MouseCard] X stuck at ~8 px while Y covers the
// full range" against a regression: a faithful card must move both axes
// equally for equal host motion. The 8-bit host coordinate wraps every
// 256 px, so the ramp deliberately runs past 256 to exercise the
// updateAxis() wrap-correction (MAME mouse.cpp update_axis<> lines
// 366-370) on BOTH axes.
//
// ProDOS mouse firmware calling convention (Apple II Mouse Tech Note):
//   on entry X = $Cn, Y = $n0 (n = slot), interrupts disabled.
//   Entry offsets read from the real slot ROM 341-0270-c:
//     SetMouse=$CnB3  ReadMouse=$Cn9B  InitMouse=$CnBC
//   ProDOS Mouse screen holes (slot s) — note X/Y are INTERLEAVED low/high:
//     Xlo=$0478+s  Ylo=$04F8+s  Xhi=$0578+s  Yhi=$05F8+s
//
// Skips cleanly (exit 0) if the user-provided ROMs are absent.

#include "M6502.h"
#include "Memory.h"
#include "MouseCard.h"
#include "Logger.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr int kSlot = 4;                       // mouse in slot 4 (//c convention)
constexpr uint16_t kCn   = 0xC000 + kSlot * 0x100;   // $C400
constexpr uint8_t  kXreg = static_cast<uint8_t>(kCn >> 8);   // $C4
constexpr uint8_t  kYreg = static_cast<uint8_t>(kSlot * 0x10); // $40

// Real slot-ROM entry low bytes (verified by dumping 341-0270-c bank 0).
constexpr uint8_t kSetMouse  = 0xB3;
constexpr uint8_t kReadMouse = 0x9B;
constexpr uint8_t kInitMouse = 0xBC;

std::string firstExisting(const std::vector<std::string>& cands)
{
    namespace fs = std::filesystem;
    for (const auto& p : cands) {
        if (fs::exists(p)) return p;
        if (fs::exists("../" + p))    return "../" + p;
        if (fs::exists("../../" + p)) return "../../" + p;
    }
    return {};
}

}  // namespace

int main()
{
    const std::string mainRom = firstExisting({"roms/apple2e.rom"});
    const std::string slotRom = firstExisting({"roms/mouse_341-0270-c.bin"});
    const std::string mcuRom  = firstExisting({"roms/mouse_341-0269.bin"});
    if (mainRom.empty() || slotRom.empty() || mcuRom.empty()) {
        std::printf("SKIP mouse_card_axis_parity: missing ROM(s) "
                    "(apple2e.rom / mouse_341-0270-c.bin / mouse_341-0269.bin)\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    Memory mem;
    M6502  cpu(&mem);

    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    if (mem.loadAppleIIRom(mainRom.c_str()) <= 0) {
        std::printf("FAIL: could not load %s\n", mainRom.c_str());
        return 1;
    }

    auto card = std::make_unique<MouseCard>(kSlot);
    if (!card->loadRoms(slotRom, mcuRom)) {
        std::printf("FAIL: MouseCard::loadRoms refused the ROMs\n");
        return 1;
    }
    MouseCard* mouse = card.get();
    mem.slotBus().plug(kSlot, std::move(card));

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    mem.slotBus().reset();

    // ── 6502 driver stubs in RAM ────────────────────────────────────────
    // Init stub @ $0300: InitMouse, then SetMouse(mode=1), then spin.
    // Read stub @ $0320: ReadMouse, then spin.
    // Pure spin   @ $0340: lets the MCU firmware drain quadrature.
    auto poke = [&](uint16_t a, std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) mem.memWrite(a++, b);
    };
    // $0300 init
    poke(0x0300, {
        0x78,                          // SEI
        0xA2, kXreg, 0xA0, kYreg,      // LDX #$C4 ; LDY #$40
        0x20, kInitMouse, kXreg,       // JSR $C4BC  InitMouse
        0xA9, 0x01,                    // LDA #$01   (mode: mouse on, no IRQ)
        0xA2, kXreg, 0xA0, kYreg,      // LDX #$C4 ; LDY #$40
        0x20, kSetMouse, kXreg,        // JSR $C4B3  SetMouse
        0x4C, 0x12, 0x03,              // JMP $0312  (spin landmark)
    });
    // $0320 read
    poke(0x0320, {
        0x78,                          // SEI
        0xA2, kXreg, 0xA0, kYreg,      // LDX #$C4 ; LDY #$40
        0x20, kReadMouse, kXreg,       // JSR $C49B  ReadMouse
        0x58,                          // CLI
        0x4C, 0x29, 0x03,              // JMP $0329  (spin landmark)
    });
    // $0340 pure spin
    poke(0x0340, { 0x4C, 0x40, 0x03 });    // JMP $0340

    auto runUntil = [&](uint16_t pc, uint16_t landmark, long budget) -> bool {
        cpu.setProgramCounter(pc);
        long c = 0;
        while (c < budget) {
            if (cpu.getProgramCounter() == landmark) return true;
            cpu.step();
            c += cpu.getCurrentInstructionCycles();
        }
        return cpu.getProgramCounter() == landmark;
    };
    auto idle = [&](long cycles) {
        cpu.setProgramCounter(0x0340);
        long c = 0;
        while (c < cycles) { cpu.step(); c += cpu.getCurrentInstructionCycles(); }
    };
    auto readMouse = [&]() { runUntil(0x0320, 0x0329, 2'000'000); };
    // ProDOS holes interleave low/high: X = Xlo($0478+s) | Xhi($0578+s)<<8,
    // Y = Ylo($04F8+s) | Yhi($05F8+s)<<8.
    auto holeX = [&]() {
        return int(mem.memRead(uint16_t(0x0478 + kSlot))) |
              (int(mem.memRead(uint16_t(0x0578 + kSlot))) << 8);
    };
    auto holeY = [&]() {
        return int(mem.memRead(uint16_t(0x04F8 + kSlot))) |
              (int(mem.memRead(uint16_t(0x05F8 + kSlot))) << 8);
    };

    // ── Initialise the mouse firmware ───────────────────────────────────
    if (!runUntil(0x0300, 0x0312, 4'000'000)) {
        std::printf("FAIL: mouse init stub did not reach landmark "
                    "(PC=$%04X) — firmware not executing at $C4xx?\n",
                    cpu.getProgramCounter());
        return 1;
    }
    idle(50'000);
    readMouse();
    std::printf("After init:  appleX=%d appleY=%d\n", holeX(), holeY());

    // ── Identical X/Y motion ramp past the 8-bit wrap ───────────────────
    // 40 increments of +20 = +800 host px on each axis (well past 256).
    constexpr int kStep = 20;
    constexpr int kIncrements = 40;
    uint8_t hx = 0, hy = 0;
    for (int i = 0; i < kIncrements; ++i) {
        hx = static_cast<uint8_t>(hx + kStep);
        hy = static_cast<uint8_t>(hy + kStep);
        mouse->setHostMouse(hx, hy, false);
        idle(40'000);                  // let the MCU drain ~20 px of quadrature
        if ((i & 7) == 7) {
            readMouse();
            std::printf("  step %2d: host=+%d  appleX=%d appleY=%d\n",
                        i + 1, (i + 1) * kStep, holeX(), holeY());
        }
    }
    readMouse();
    const int finalX = holeX();
    const int finalY = holeY();
    const int hostTotal = kIncrements * kStep;     // 800
    std::printf("FINAL: host ramp=+%d  appleX=%d appleY=%d\n",
                hostTotal, finalX, finalY);

    // ── Parity assertions ───────────────────────────────────────────────
    // Equal host motion must yield equal Apple motion on both axes. Y is
    // the known-good reference; X must track it. Require both axes to have
    // tracked most of the +800 ramp, and to agree within a tiny slack
    // (quadrature draining can leave at most a couple of px of lag).
    constexpr int kTrackMin = 700;     // of the +800 ramp
    constexpr int kSlack    = 16;      // max tolerated X-vs-Y divergence
    const int diff = finalX > finalY ? finalX - finalY : finalY - finalX;
    if (finalY < kTrackMin) {
        std::printf("FAIL: Y axis did not track host ramp (Y=%d < %d) — "
                    "harness/firmware broken\n", finalY, kTrackMin);
        return 1;
    }
    if (finalX < kTrackMin) {
        std::printf("FAIL: X axis STUCK — tracked only %d of +%d host px while "
                    "Y reached %d. [MouseCard] X-stuck regression.\n",
                    finalX, hostTotal, finalY);
        return 1;
    }
    if (diff > kSlack) {
        std::printf("FAIL: X/Y diverged (X=%d, Y=%d, diff=%d > slack %d) — "
                    "axis-parity regression\n", finalX, finalY, diff, kSlack);
        return 1;
    }
    assert(finalX >= kTrackMin && finalY >= kTrackMin && diff <= kSlack);

    std::printf("OK mouse_card_axis_parity (X=%d Y=%d, parity within %d)\n",
                finalX, finalY, kSlack);
    return 0;
}
