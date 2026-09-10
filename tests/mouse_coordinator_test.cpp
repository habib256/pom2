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

// MouseCoordinator topology, snapshot and lifetime contract.

#include "EmulationController.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "IIcMouse.h"
#include "MouseCoordinator.h"
#include "PrinterCard.h"
#include "SlotBus.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>

int main()
{
    EmulationController controller;
    pom2::MouseCoordinator mouse(controller);

    {
        auto state = controller.lockState();
        auto& memory = state.memory();
        auto& bus = memory.slotBus();
        bus.plug(2, std::make_unique<MouseCard>(2));
        bus.plug(5, std::make_unique<MouseCardAppleWin>(5));

        memory.memWrite(0x047D, 0x34);
        memory.memWrite(0x057D, 0x12);
        memory.memWrite(0x04FD, 0x78);
        memory.memWrite(0x05FD, 0x56);
        memory.memWrite(0x077D, 0xA5);
        memory.memWrite(0x07FD, 0x5A);
    }

    const auto both = mouse.capture();
    assert(both.mamePlugged);
    assert(both.appleWinPlugged);
    assert(both.plugged());
    assert(both.appleWinActive());
    assert(both.slot == 5);
    assert(both.holes.x() == 0x1234);
    assert(both.holes.y() == 0x5678);
    assert(both.holes.status == 0xA5);
    assert(both.holes.mode == 0x5A);
    assert(mouse.routeHost(23, 197, true) == 2);

    // Destroy the previously selected card and replace it with an unrelated
    // device. The next operation must resolve fresh topology, never dereference
    // a retained alias.
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        (void)bus.unplug(5);
        bus.plug(5, std::make_unique<PrinterCard>(5));
    }
    const auto mameOnly = mouse.capture();
    assert(mameOnly.kind == pom2::MouseCoordinator::Kind::Mame);
    assert(mameOnly.slot == 2);
    assert(mameOnly.mamePlugged && !mameOnly.appleWinPlugged);
    assert(mouse.routeHost(1, 2, false) == 1);

    {
        auto state = controller.lockState();
        (void)state.memory().slotBus().unplug(2);
    }
    const auto none = mouse.capture();
    assert(!none.plugged());
    assert(none.kind == pom2::MouseCoordinator::Kind::None);
    assert(none.slot == -1);
    assert(mouse.routeHost(0, 0, false) == 0);

    {
        auto state = controller.lockState();
        state.memory().slotBus().plug(4, std::make_unique<IIcMouse>());
    }
    const auto native = mouse.capture();
    assert(native.kind == pom2::MouseCoordinator::Kind::IIc);
    assert(native.iicPlugged && native.plugged() && native.slot == 4);
    assert(!native.appleWinActive() && !native.mamePlugged);
    assert(mouse.routeHost(10, 20, true) == 1);
    {
        auto state = controller.lockState();
        auto* device = dynamic_cast<IIcMouse*>(state.memory().slotBus().peripheral(4));
        uint8_t out;
        assert(device->iicMouseAccess(0x63, false, false, 0x55, out));
        assert(out == 0x55);
        (void)state.memory().slotBus().unplug(4);
    }
    assert(!mouse.capture().plugged());

    std::cout << "mouse coordinator: OK\n";
    return 0;
}
