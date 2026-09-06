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

#include "PrinterCoordinator.h"

#include "Block512Backing.h"   // pom2::noteMediaWrite

#include "EmulationController.h"
#include "FujiNetCard.h"
#include "GrapplerCard.h"
#include "PrinterCard.h"
#include "Settings.h"
#include "SlotBus.h"
#include "SuperSerialCard.h"

#include <utility>

namespace pom2 {
namespace {

template <typename Card>
Card* findFirst(SlotBus& bus)
{
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        if (auto* card = dynamic_cast<Card*>(bus.peripheral(slot)))
            return card;
    }
    return nullptr;
}

std::string sourceName(PrinterCoordinator::SourceKind kind, int slot)
{
    switch (kind) {
        case PrinterCoordinator::SourceKind::PrinterCard:
            return "Printer card slot " + std::to_string(slot);
        case PrinterCoordinator::SourceKind::Grappler:
            return "Grappler+ slot " + std::to_string(slot);
        case PrinterCoordinator::SourceKind::FujiNet:
            return "FujiNet printer slot " + std::to_string(slot);
        case PrinterCoordinator::SourceKind::SuperSerial:
            return "Super Serial slot " + std::to_string(slot);
        case PrinterCoordinator::SourceKind::None:
            break;
    }
    return {};
}

} // namespace

PrinterCoordinator::PrinterPanelSnapshot
PrinterCoordinator::capturePrinterPanel(EmulationController& controller) const
{
    PrinterPanelSnapshot snapshot;
    auto state = controller.lockState();
    auto& bus = state.memory().slotBus();
    const auto* card = findFirst<PrinterCard>(bus);
    if (!card) return snapshot;

    snapshot.plugged = true;
    snapshot.slot = card->getSlot();
    snapshot.bytesWritten = card->bytesWritten();
    snapshot.spoolTruncated = card->spoolTruncated();
    snapshot.spoolText = card->spoolText();
    return snapshot;
}

bool PrinterCoordinator::clearPrinterPanelSpool(
    EmulationController& controller, int slot)
{
    auto state = controller.lockState();
    auto& bus = state.memory().slotBus();
    auto* card = dynamic_cast<PrinterCard*>(bus.peripheral(slot));
    if (!card) return false;
    card->clearSpool();
    return true;
}

PrinterCoordinator::HostSnapshot
PrinterCoordinator::captureHost(EmulationController& controller) const
{
    HostSnapshot snapshot;
    auto state = controller.lockState();
    auto& bus = state.memory().slotBus();

    auto* printer = findFirst<PrinterCard>(bus);
    auto* grappler = findFirst<GrapplerCard>(bus);
    FujiNetCard* fujiNet = nullptr;
    std::vector<SuperSerialCard*> serialTaps;
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        auto* peripheral = bus.peripheral(slot);
        if (!fujiNet) {
            if (auto* card = dynamic_cast<FujiNetCard*>(peripheral);
                card && card->hasPrinterUnit()) {
                fujiNet = card;
            }
        }
        if (auto* card = dynamic_cast<SuperSerialCard*>(peripheral);
            card && card->printerTap()) {
            serialTaps.push_back(card);
        }
    }

    if (printer) {
        snapshot.printerCardSlot = printer->getSlot();
        snapshot.source = SourceKind::PrinterCard;
        snapshot.sourceSlot = printer->getSlot();
    } else if (grappler) {
        snapshot.source = SourceKind::Grappler;
        snapshot.sourceSlot = grappler->getSlot();
    } else if (fujiNet) {
        snapshot.source = SourceKind::FujiNet;
        snapshot.sourceSlot = fujiNet->getSlot();
    } else if (!serialTaps.empty()) {
        snapshot.source = SourceKind::SuperSerial;
        snapshot.sourceSlot = serialTaps.front()->getSlot();
    }

    if (grappler) {
        snapshot.grapplerPlugged = true;
        snapshot.grapplerSlot = grappler->getSlot();
        snapshot.grapplerRomLoaded = grappler->isRomLoaded();
        snapshot.grapplerBusy = grappler->printerBusy();
        snapshot.grapplerPrinterType =
            static_cast<int>(grappler->printerType());
        snapshot.grapplerMsbSoftwareControl =
            grappler->msbSoftwareControl();
    }

    auto ignore = [&](SourceKind kind, int slot) {
        if (kind != snapshot.source || slot != snapshot.sourceSlot)
            snapshot.ignoredSources.push_back(sourceName(kind, slot));
    };
    if (printer) ignore(SourceKind::PrinterCard, printer->getSlot());
    if (grappler) ignore(SourceKind::Grappler, grappler->getSlot());
    if (fujiNet) ignore(SourceKind::FujiNet, fujiNet->getSlot());
    for (auto* card : serialTaps)
        ignore(SourceKind::SuperSerial, card->getSlot());

    return snapshot;
}

// Paper is irreversible output. Bump the rewind ring's "effects that cannot
// be undone" epoch whenever bytes actually reach the ImageWriter, so the ring
// clears and a later scrub can never span a print.
//
// Resetting `consumed_` on a restore — the other candidate fix — would be
// exactly backwards here: the card spools are deliberately NOT rewound (they
// are append-only output logs; see GrapplerCard.h), so a cursor reset would
// REPRINT the whole spool rather than avoid a duplicate. What the cursor
// cannot defend against is the guest replaying the print after a rewind and
// appending the same bytes a second time; only refusing to rewind across the
// print does, and that is what this is.
//
// The coordinator is the right site: it is the single funnel where bytes stop
// being a card's buffer and start being paper.
void PrinterCoordinator::notePrinted(const FeedBatch& batch)
{
    if (!batch.bytes.empty()) pom2::noteMediaWrite();
}

void PrinterCoordinator::prepareDrain(SourceIdentity identity,
                                      std::size_t total)
{
    if (source_ != identity) {
        source_ = identity;
        consumed_ = total; // cable handover adopts, never reprints backlog
    } else if (total < consumed_) {
        consumed_ = 0;     // panel cleared the current spool
    }
}

PrinterCoordinator::FeedBatch
PrinterCoordinator::drainImageWriter(EmulationController& controller)
{
    FeedBatch batch;
    auto state = controller.lockState();
    auto& bus = state.memory().slotBus();

    if (auto* card = findFirst<PrinterCard>(bus)) {
        batch.source = SourceKind::PrinterCard;
        batch.sourceSlot = card->getSlot();
        prepareDrain({batch.source, reinterpret_cast<std::uintptr_t>(card)},
                     card->bytesWritten());
        consumed_ = card->drainSpoolFrom(consumed_, batch.bytes);
        notePrinted(batch);
        return batch;
    }
    if (auto* card = findFirst<GrapplerCard>(bus)) {
        batch.source = SourceKind::Grappler;
        batch.sourceSlot = card->getSlot();
        prepareDrain({batch.source, reinterpret_cast<std::uintptr_t>(card)},
                     card->bytesWritten());
        consumed_ = card->drainSpoolFrom(consumed_, batch.bytes);
        notePrinted(batch);
        return batch;
    }
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        auto* card = dynamic_cast<FujiNetCard*>(bus.peripheral(slot));
        if (!card || !card->hasPrinterUnit()) continue;
        batch.source = SourceKind::FujiNet;
        batch.sourceSlot = slot;
        prepareDrain({batch.source, reinterpret_cast<std::uintptr_t>(card)},
                     card->bytesWritten());
        consumed_ = card->drainSpoolFrom(consumed_, batch.bytes);
        notePrinted(batch);
        return batch;
    }
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        auto* card = dynamic_cast<SuperSerialCard*>(bus.peripheral(slot));
        if (!card || !card->printerTap()) continue;
        batch.source = SourceKind::SuperSerial;
        batch.sourceSlot = slot;
        prepareDrain({batch.source, reinterpret_cast<std::uintptr_t>(card)},
                     card->printerSpoolBytes());
        consumed_ = card->drainPrinterSpoolFrom(consumed_, batch.bytes);
        notePrinted(batch);
        return batch;
    }

    source_ = {};
    consumed_ = 0;
    return batch;
}

bool PrinterCoordinator::setGrapplerPrinterType(
    EmulationController& controller, int value)
{
    if (value < 0 || value > 7) return false;
    auto state = controller.lockState();
    auto& bus = state.memory().slotBus();
    auto* card = findFirst<GrapplerCard>(bus);
    if (!card) return false;
    card->setPrinterType(static_cast<GrapplerCard::PrinterType>(value));
    return true;
}

PrinterCoordinator::BusyUpdate PrinterCoordinator::setGrapplerBusy(
    EmulationController& controller, bool busy)
{
    BusyUpdate result;
    auto state = controller.lockState();
    auto& bus = state.memory().slotBus();
    auto* card = findFirst<GrapplerCard>(bus);
    if (!card) return result;
    result.grapplerPlugged = true;
    result.changed = card->printerBusy() != busy;
    if (result.changed) card->setPrinterBusy(busy);
    return result;
}

void PrinterCoordinator::persistGrappler(
    Settings& settings, EmulationController& controller) const
{
    const auto snapshot = captureHost(controller);
    if (!snapshot.grapplerPlugged) return;
    settings.setInt("grappler_printer_type", snapshot.grapplerPrinterType);
    settings.setBool("grappler_msb_software",
                     snapshot.grapplerMsbSoftwareControl);
}

void PrinterCoordinator::resetFeedCursor() noexcept
{
    source_ = {};
    consumed_ = 0;
}

} // namespace pom2
