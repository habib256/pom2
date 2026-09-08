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

#include "MouseCoordinator.h"

#include "EmulationController.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "SlotBus.h"

namespace pom2 {
namespace {

void copyAppleWin(MouseCoordinator::AppleWinSnapshot& out,
                  const MouseCardAppleWin::DebugSnapshot& in)
{
    out.iX = in.iX;
    out.iY = in.iY;
    out.nX = in.nX;
    out.nY = in.nY;
    out.iMinX = in.iMinX;
    out.iMaxX = in.iMaxX;
    out.iMinY = in.iMinY;
    out.iMaxY = in.iMaxY;
    out.bBtn0 = in.bBtn0;
    out.bBtn1 = in.bBtn1;
    out.bPrevBtn0 = in.bPrevBtn0;
    out.bPrevBtn1 = in.bPrevBtn1;
    out.byMode = in.byMode;
    out.byState = in.byState;
    out.by6821A = in.by6821A;
    out.by6821B = in.by6821B;
    out.hostDrained = in.hostDrained;
    out.buffPos = in.buffPos;
    out.dataLen = in.dataLen;
    out.lastCmd = in.lastCmd;
}

} // namespace

MouseCoordinator::Snapshot MouseCoordinator::capture() const
{
    Snapshot snapshot;
    auto state = controller_.lockState();
    auto& bus = state.memory().slotBus();

    MouseCard* mame = nullptr;
    MouseCardAppleWin* appleWin = nullptr;
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        auto* peripheral = bus.peripheral(slot);
        if (auto* mameCard = dynamic_cast<MouseCard*>(peripheral)) {
            snapshot.mamePlugged = true;
            mame = mameCard;
        } else if (auto* appleWinCard =
                       dynamic_cast<MouseCardAppleWin*>(peripheral)) {
            snapshot.appleWinPlugged = true;
            appleWin = appleWinCard;
        }
    }

    if (appleWin) {
        snapshot.kind = Kind::AppleWin;
        snapshot.slot = appleWin->getSlot();
        copyAppleWin(snapshot.appleWin, appleWin->debugSnapshot());
    } else if (mame) {
        snapshot.kind = Kind::Mame;
        snapshot.slot = mame->getSlot();
    }

    if (snapshot.slot > 0 && snapshot.slot < SlotBus::kSlotCount) {
        // The six mouse screen holes live in text page 1 ($0478/$0578/$04F8/
        // $05F8/$0778/$07F8 + slot), and on a //e that page is exactly what
        // 80STORE + PAGE2 moves to AUX: the firmware writes them through the
        // CPU view, so under a mouse-driven 80-column program `peekMainRam`
        // was reading a stale main-bank copy and the Mouse Inspector showed
        // a cursor frozen at whatever was there before the switch. Read the
        // same view the CPU reads — `peekCpuView` resolves the paging with
        // no side effects (Memory.h: no soft switch, no INTC8ROM latch, no
        // $C800 claim).
        auto& memory = state.memory();
        snapshot.holes.xLo = memory.peekCpuView(
            static_cast<std::uint16_t>(0x0478 + snapshot.slot));
        snapshot.holes.xHi = memory.peekCpuView(
            static_cast<std::uint16_t>(0x0578 + snapshot.slot));
        snapshot.holes.yLo = memory.peekCpuView(
            static_cast<std::uint16_t>(0x04F8 + snapshot.slot));
        snapshot.holes.yHi = memory.peekCpuView(
            static_cast<std::uint16_t>(0x05F8 + snapshot.slot));
        snapshot.holes.status = memory.peekCpuView(
            static_cast<std::uint16_t>(0x0778 + snapshot.slot));
        snapshot.holes.mode = memory.peekCpuView(
            static_cast<std::uint16_t>(0x07F8 + snapshot.slot));
    }
    return snapshot;
}

int MouseCoordinator::routeHost(std::uint8_t rawX, std::uint8_t rawY,
                                bool button)
{
    int routed = 0;
    auto state = controller_.lockState();
    auto& bus = state.memory().slotBus();
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        auto* peripheral = bus.peripheral(slot);
        if (auto* mameCard = dynamic_cast<MouseCard*>(peripheral)) {
            mameCard->setHostMouse(rawX, rawY, button);
            ++routed;
        } else if (auto* appleWinCard =
                       dynamic_cast<MouseCardAppleWin*>(peripheral)) {
            appleWinCard->setHostMouse(rawX, rawY, button);
            ++routed;
        }
    }
    return routed;
}

} // namespace pom2
