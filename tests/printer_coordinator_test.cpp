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

// PrinterCoordinator contract: value snapshots, cable priority and cursor
// handover without retaining SlotBus-owned pointers.

#include "EmulationController.h"
#include "CentronicsPrinter.h"
#include "GrapplerCard.h"
#include "GrapplerClassicCard.h"
#include "PrinterCard.h"
#include "PrinterCoordinator.h"
#include "Settings.h"
#include "SlotBus.h"
#include "SuperSerialCard.h"

#include <cassert>
#include <iostream>
#include <memory>

int main()
{
    EmulationController controller;
    pom2::PrinterCoordinator printers;
    pom2::Settings settings;

    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();

        auto serial = std::make_unique<SuperSerialCard>(1);
        serial->setPrinterTap(true);
        bus.plug(1, std::move(serial));

        auto parallel = std::make_unique<PrinterCard>(2);
        parallel->deviceSelectWrite(0, 'A');
        parallel->deviceSelectWrite(0, 'B');
        bus.plug(2, std::move(parallel));

        auto grappler = std::make_unique<GrapplerCard>(3);
        grappler->deviceSelectWrite(0, 'G');
        bus.plug(3, std::move(grappler));
    }

    auto host = printers.captureHost(controller);
    assert(host.source == pom2::PrinterCoordinator::SourceKind::PrinterCard);
    assert(host.sourceSlot == 2);
    assert(host.printerCardPlugged());
    assert(host.grapplerPlugged && host.grapplerSlot == 3);
    assert(host.ignoredSources.size() == 2);

    // Attaching a cable adopts an existing backlog instead of reprinting it.
    auto batch = printers.drainImageWriter(controller);
    assert(batch.haveSource());
    assert(batch.bytes.empty());

    {
        auto state = controller.lockState();
        auto* card = dynamic_cast<PrinterCard*>(
            state.memory().slotBus().peripheral(2));
        assert(card);
        card->deviceSelectWrite(0, 'C');
    }
    batch = printers.drainImageWriter(controller);
    assert(batch.bytes.size() == 1 && batch.bytes[0] == 'C');

    auto panel = printers.capturePrinterPanel(controller);
    assert(panel.plugged && panel.slot == 2);
    assert(panel.bytesWritten == 3);
    assert(panel.spoolText == "ABC");
    assert(printers.clearPrinterPanelSpool(controller, 2));
    panel = printers.capturePrinterPanel(controller);
    assert(panel.spoolText.empty());

    // Removing the primary source hands the cable to Grappler+ and adopts
    // its old byte. Only output written after the handover is delivered.
    {
        auto state = controller.lockState();
        (void)state.memory().slotBus().unplug(2);
    }
    batch = printers.drainImageWriter(controller);
    assert(batch.source == pom2::PrinterCoordinator::SourceKind::Grappler);
    assert(batch.bytes.empty());
    {
        auto state = controller.lockState();
        auto* card = dynamic_cast<GrapplerCard*>(
            state.memory().slotBus().peripheral(3));
        assert(card);
        card->deviceSelectWrite(0, 'H');
    }
    batch = printers.drainImageWriter(controller);
    assert(batch.bytes.size() == 1 && batch.bytes[0] == 'H');

    assert(printers.setGrapplerPrinterType(controller, 4));
    auto busy = printers.setGrapplerBusy(controller, true);
    assert(busy.grapplerPlugged && busy.changed);
    host = printers.captureHost(controller);
    assert(host.grapplerPrinterType == 4);
    assert(host.grapplerBusy);

    printers.persistGrappler(settings, controller);
    assert(settings.getInt("grappler_printer_type", -1) == 4);

    // A slot rebuild invalidates the source identity before allocator reuse.
    printers.resetFeedCursor();
    {
        auto state = controller.lockState();
        (void)state.memory().slotBus().unplug(3);
    }
    host = printers.captureHost(controller);
    assert(host.source == pom2::PrinterCoordinator::SourceKind::SuperSerial);
    assert(host.sourceSlot == 1);

    // Back-pressure reaches a Centronics card (1981 Grappler) too, and only
    // on edges: a BUSY set by hand survives the per-frame "not busy".
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        (void)bus.unplug(1);
        (void)bus.unplug(2);
        bus.plug(4, std::make_unique<GrapplerClassicCard>(4));
    }
    auto centronics = [&]() {
        auto state = controller.lockState();
        return state.memory().slotBus().peripheral(4)->centronicsPrinter()->busy();
    };
    (void)printers.setGrapplerBusy(controller, true);
    assert(centronics() && "back-pressure did not reach the 1981 Grappler");
    (void)printers.setGrapplerBusy(controller, false);
    assert(!centronics());
    {
        auto state = controller.lockState();
        state.memory().slotBus().peripheral(4)->centronicsPrinter()->setBusy(true);
    }
    (void)printers.setGrapplerBusy(controller, false);
    assert(centronics() && "a hand-set BUSY was cleared by the frame pump");

    std::cout << "printer coordinator: OK\n";
    return 0;
}
