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

// MouseCard absolute closed-loop sync integration test.
//
// Boots the REAL Apple Mouse Card firmware (MCU 341-0269 + slot ROM
// 341-0270-c) on a full M6502 + Memory + SlotBus and reproduces the
// host-side closed-loop that `MainWindow::onMouseMove` runs: each
// frame it reads the guest cursor position back from the firmware
// screen holes and injects the *clamped* quadrature delta needed to
// drive the guest toward an absolute target. It asserts the guest
// cursor CONVERGES to and HOLDS that absolute target.
//
// This is the property that distinguishes the new absolute loop from
// the old open-loop relative feed: a purely relative device has no
// notion of an absolute target and would drift; the closed loop must
// land on the exact target and stay there even after the host pushes
// the cursor *past* the firmware clamp edge and back (clamp-edge
// losses are exactly what made the open-loop feed drift).
//
// Mirrors the harness of mouse_card_axis_parity_test.cpp. Skips
// cleanly (exit 0) if the user-provided ROMs are absent.
//
// ProDOS Mouse screen holes (slot s) — X/Y interleaved low/high:
//   Xlo=$0478+s  Ylo=$04F8+s  Xhi=$0578+s  Yhi=$05F8+s  mode=$07F8+s
// Entry offsets read from the real slot ROM 341-0270-c:
//   SetMouse=$CnB3  ReadMouse=$Cn9B  InitMouse=$CnBC

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

constexpr int kSlot = 4;
constexpr uint16_t kCn   = 0xC000 + kSlot * 0x100;            // $C400
constexpr uint8_t  kXreg = static_cast<uint8_t>(kCn >> 8);    // $C4
constexpr uint8_t  kYreg = static_cast<uint8_t>(kSlot * 0x10);// $40

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

// Same per-frame delta clamp as MainWindow::onMouseMove / the MCU's
// 8-bit signed diff-wrap window.
int clamp127(int v) { return v > 127 ? 127 : (v < -127 ? -127 : v); }

}  // namespace

int main()
{
    const std::string mainRom = firstExisting({"roms/apple2e.rom"});
    const std::string slotRom = firstExisting({"roms/mouse_341-0270-c.bin"});
    const std::string mcuRom  = firstExisting({"roms/mouse_341-0269.bin"});
    if (mainRom.empty() || slotRom.empty() || mcuRom.empty()) {
        std::printf("SKIP mouse_card_absolute_sync: missing ROM(s) "
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

    auto poke = [&](uint16_t a, std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) mem.memWrite(a++, b);
    };
    poke(0x0300, {
        0x78,
        0xA2, kXreg, 0xA0, kYreg,
        0x20, kInitMouse, kXreg,
        0xA9, 0x01,                    // mode $01: mouse on, polled (no IRQ)
        0xA2, kXreg, 0xA0, kYreg,
        0x20, kSetMouse, kXreg,
        0x4C, 0x12, 0x03,
    });
    poke(0x0320, {
        0x78,
        0xA2, kXreg, 0xA0, kYreg,
        0x20, kReadMouse, kXreg,
        0x58,
        0x4C, 0x29, 0x03,
    });
    poke(0x0340, { 0x4C, 0x40, 0x03 });

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
    auto holeX = [&]() {
        return int(mem.peekMainRam(uint16_t(0x0478 + kSlot))) |
              (int(mem.peekMainRam(uint16_t(0x0578 + kSlot))) << 8);
    };
    auto holeY = [&]() {
        return int(mem.peekMainRam(uint16_t(0x04F8 + kSlot))) |
              (int(mem.peekMainRam(uint16_t(0x05F8 + kSlot))) << 8);
    };
    auto modeByte = [&]() {
        return int(mem.peekMainRam(uint16_t(0x07F8 + kSlot)));
    };

    if (!runUntil(0x0300, 0x0312, 4'000'000)) {
        std::printf("FAIL: mouse init stub did not reach landmark (PC=$%04X)\n",
                    cpu.getProgramCounter());
        return 1;
    }
    idle(50'000);
    readMouse();
    std::printf("After init:  mode=$%02X  appleX=%d appleY=%d\n",
                modeByte(), holeX(), holeY());
    if ((modeByte() & 0x01) == 0) {
        std::printf("FAIL: SetMouse did not turn the mouse on "
                    "(mode bit0 clear) — onMouseMove would never engage the "
                    "absolute loop\n");
        return 1;
    }

    // ── Host-side closed loop, exactly as MainWindow::onMouseMove ────────
    // Drive the guest to an absolute target well inside the default clamp
    // window (0..1023). We run frames: read holes, inject one clamped
    // correction toward the target (edge-triggered on hole movement),
    // let the MCU shift the quadrature out, repeat.
    auto converge = [&](int targetX, int targetY, int maxFrames) -> bool {
        uint8_t accumX = 0, accumY = 0;
        int lastHoleX = -0x10000, lastHoleY = -0x10000;   // force first inject
        for (int f = 0; f < maxFrames; ++f) {
            readMouse();                       // app poll → firmware updates holes
            const int hx = holeX(), hy = holeY();
            if (hx == targetX && hy == targetY) return true;
            if (hx != lastHoleX || hy != lastHoleY) {
                accumX = static_cast<uint8_t>(accumX + clamp127(targetX - hx));
                accumY = static_cast<uint8_t>(accumY + clamp127(targetY - hy));
                mouse->setHostMouse(accumX, accumY, false);
                lastHoleX = hx;
                lastHoleY = hy;
            }
            idle(400'000);                     // let the MCU drain up to 127 edges
        }
        readMouse();
        return holeX() == targetX && holeY() == targetY;
    };

    // 1) Converge from the init position to an absolute target.
    constexpr int kTgtX = 400, kTgtY = 300;
    if (!converge(kTgtX, kTgtY, 24)) {
        std::printf("FAIL: closed loop did not converge to (%d,%d) — "
                    "got (%d,%d)\n", kTgtX, kTgtY, holeX(), holeY());
        return 1;
    }
    std::printf("Converged #1: appleX=%d appleY=%d (target %d,%d)\n",
                holeX(), holeY(), kTgtX, kTgtY);

    // 2) Re-converge to a SECOND target after first pushing the cursor
    //    hard past the clamp edge (target 2000 > clamp max 1023). The
    //    firmware clamps and loses ticks — an open-loop feed would stay
    //    pinned at the clamp edge and never come back. The closed loop
    //    must drive it cleanly to the new in-range target.
    converge(2000, 2000, 24);                  // slam into the clamp ceiling
    std::printf("After clamp slam: appleX=%d appleY=%d (clamped)\n",
                holeX(), holeY());
    constexpr int kTgt2X = 120, kTgt2Y = 90;
    if (!converge(kTgt2X, kTgt2Y, 24)) {
        std::printf("FAIL: closed loop did not RE-converge to (%d,%d) after a "
                    "clamp-edge excursion — got (%d,%d). This is the open-loop "
                    "drift the absolute sync is meant to kill.\n",
                    kTgt2X, kTgt2Y, holeX(), holeY());
        return 1;
    }
    std::printf("Converged #2: appleX=%d appleY=%d (target %d,%d)\n",
                holeX(), holeY(), kTgt2X, kTgt2Y);

    assert(holeX() == kTgt2X && holeY() == kTgt2Y);
    std::printf("OK mouse_card_absolute_sync (pixel-exact convergence + "
                "clamp-edge recovery)\n");
    return 0;
}
