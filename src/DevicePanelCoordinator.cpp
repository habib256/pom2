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

#include "DevicePanelCoordinator.h"

#include "EmulationController.h"
#include "ClockCard.h"
#include "FujiNetCard.h"
#include "LeChatMauveCard.h"
#include "NetworkBackend.h"
#include "GrapplerCard.h"
#include "PrinterCard.h"
#include "Settings.h"
#include "SlirpNetworkBackend.h"
#include "SlotBus.h"
#include "SystemProfile.h"
#include "SmartPortCard.h"
#include "SuperSerialCard.h"
#include "UthernetCard.h"
#include "UthernetIICard.h"

namespace pom2 {
namespace {

template <typename Card>
Card* findCard(SlotBus& bus)
{
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        if (auto* card = dynamic_cast<Card*>(bus.peripheral(slot)))
            return card;
    }
    return nullptr;
}

} // namespace

DevicePanelCoordinator::DevicePanelCoordinator(EmulationController& controller,
                                               Settings& settings)
    : controller_(controller), settings_(settings)
{
}

DevicePanelCoordinator::InventorySnapshot
DevicePanelCoordinator::captureInventory() const
{
    InventorySnapshot snapshot;
    auto state = controller_.lockState();
    auto& bus = state.memory().slotBus();
    if (const auto* card = findCard<LeChatMauveCard>(bus))
        snapshot.chatMauveSlot = card->getSlot();
    if (const auto* card = findCard<SmartPortCard>(bus))
        snapshot.smartPortSlot = card->getSlot();
    if (const auto* card = findCard<FujiNetCard>(bus))
        snapshot.fujiNetSlot = card->getSlot();
    if (const auto* card = findCard<UthernetCard>(bus))
        snapshot.uthernetSlot = card->getSlot();
    if (const auto* card = findCard<UthernetIICard>(bus))
        snapshot.uthernetIISlot = card->getSlot();
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        const auto* peripheral = bus.peripheral(slot);
        if (dynamic_cast<const SuperSerialCard*>(peripheral))
            snapshot.serialSlots.push_back(slot);
        if (snapshot.printerSlot < 0) {
            if (dynamic_cast<const PrinterCard*>(peripheral))
                snapshot.printerSlot = slot;
        }
        if (snapshot.grapplerSlot < 0) {
            if (const auto* card = dynamic_cast<const GrapplerCard*>(peripheral)) {
                snapshot.grapplerSlot = slot;
                snapshot.grapplerRomLoaded = card->isRomLoaded();
                snapshot.grapplerBusy = card->printerBusy();
            }
        }
        if (const auto* card = dynamic_cast<const ClockCard*>(peripheral)) {
            // Slots are visited ascending; keep overwriting to preserve the
            // former last-plugged/highest-slot alias semantics.
            snapshot.clockSlot = card->getSlot();
            snapshot.clockRomFromDump = card->romFromDump();
        }
    }
    return snapshot;
}

Uthernet_ImGui::Snapshot DevicePanelCoordinator::captureEthernet() const
{
    Uthernet_ImGui::Snapshot snapshot;
    snapshot.slirpCompiledIn = slirpAvailable();
    snapshot.backendChoice = settings_.getString("ethernet_backend", "slirp");

    auto state = controller_.lockState();
    auto& bus = state.memory().slotBus();

    if (const auto* card = findCard<UthernetCard>(bus)) {
        const Cs8900aDevice& chip = card->chip();
        const NetworkBackend* backend = card->backend();
        snapshot.u1Plugged = true;
        snapshot.u1Slot = card->getSlot();
        snapshot.u1Backend = backend ? std::string(backend->name()) : "none";
        snapshot.u1BackendValid = backend && backend->isValid();
        snapshot.u1Mac = chip.macAddress();
        snapshot.u1RxEnabled = chip.receiverEnabled();
        snapshot.u1TxEnabled = chip.transmitterEnabled();
        snapshot.u1Promiscuous = chip.promiscuous();
        snapshot.u1PacketPagePtr = chip.packetPagePointer();
        snapshot.u1Queued = chip.queuedFrames();
        snapshot.u1FramesSent = chip.framesSent();
        snapshot.u1FramesReceived = chip.framesReceived();
        snapshot.u1FramesFiltered = chip.framesFiltered();
    }

    if (const auto* card = findCard<UthernetIICard>(bus)) {
        const W5100Device& chip = card->chip();
        const NetworkBackend* backend = card->backend();
        snapshot.u2Plugged = true;
        snapshot.u2Slot = card->getSlot();
        snapshot.u2Backend = backend ? std::string(backend->name()) : "none";
        snapshot.u2BackendValid = backend && backend->isValid();
        snapshot.u2Mac = chip.macAddress();
        snapshot.u2Ip = chip.localIp();
        snapshot.u2VirtualDns = chip.virtualDnsEnabled();
        snapshot.u2BytesSent = chip.bytesSent();
        snapshot.u2BytesReceived = chip.bytesReceived();
        for (std::size_t i = 0; i < W5100Device::kSocketCount; ++i)
            snapshot.u2Sockets[i] = chip.socketInfo(i);
    }
    return snapshot;
}

void DevicePanelCoordinator::applyEthernet(
    const Uthernet_ImGui::FrameResult& command)
{
    if (!command.requestResetU1 && !command.requestResetU2 &&
        !command.requestVirtualDns) {
        return;
    }

    bool persistVirtualDns = false;
    {
        auto state = controller_.lockState();
        auto& bus = state.memory().slotBus();
        if (command.requestResetU1) {
            if (auto* card = findCard<UthernetCard>(bus)) card->onReset();
        }
        if (auto* card = findCard<UthernetIICard>(bus)) {
            if (command.requestResetU2) card->onReset();
            if (command.requestVirtualDns) {
                card->chip().setVirtualDnsEnabled(command.virtualDnsTo);
                persistVirtualDns = true;
            }
        }
    }
    // Settings can eventually perform host I/O. Never keep the machine lock
    // while crossing that runtime boundary.
    if (persistVirtualDns)
        settings_.setBool("uthernet2_virtual_dns", command.virtualDnsTo);
}

LeChatMauve_ImGui::Snapshot
DevicePanelCoordinator::captureChatMauve() const
{
    LeChatMauve_ImGui::Snapshot snapshot;
    auto state = controller_.lockState();
    auto& bus = state.memory().slotBus();
    if (const auto* card = findCard<LeChatMauveCard>(bus)) {
        snapshot.plugged = true;
        snapshot.slot = card->getSlot();
        snapshot.mode = card->currentMode();
        snapshot.fifoBits = card->fifoBits();
        snapshot.eightyCol = card->eightyCol();
        snapshot.an3High = card->an3High();
        snapshot.invertBit7 = card->invertBit7();
        snapshot.variant = card->variant();
        snapshot.dhgrMode = card->dhgrMode();
        snapshot.eveSwitches = card->eveSwitches();
        snapshot.cpreg = card->cpreg();
        snapshot.auxShadowText = state.memory().auxShadowText();
        snapshot.auxShadowHgr = state.memory().auxShadowHgr();
    }
    return snapshot;
}

void DevicePanelCoordinator::applyChatMauve(
    const LeChatMauve_ImGui::FrameResult& command)
{
    if (!command.requestOverride && !command.requestReset &&
        !command.requestInvertBit7 && !command.requestVariant &&
        !command.requestEveSwitch) {
        return;
    }

    bool persistInvert = false;
    bool persistVariant = false;
    {
        auto state = controller_.lockState();
        auto& bus = state.memory().slotBus();
        auto* card = findCard<LeChatMauveCard>(bus);
        if (!card) return;

        if (command.requestOverride) card->overrideMode(command.overrideTo);
        if (command.requestReset) card->onReset();
        if (command.requestInvertBit7) {
            card->setInvertBit7(command.invertBit7To);
            persistInvert = true;
        }
        if (command.requestVariant) {
            card->setVariant(command.variantTo);
            persistVariant = true;
        }
        // A guest register poked from the UI: same path as `STA $C0Bx`.
        if (command.requestEveSwitch)
            card->setEveSwitch(command.eveSwitch, command.eveSwitchTo);
    }

    if (persistInvert)
        settings_.setBool("chatmauve_invert_bit7", command.invertBit7To);
    if (persistVariant) {
        // A //c-class DB-15 is the Adaptateur IIc: the model is hardware
        // (`plugChatMauve` forces IIcAdapter and ignores this key). Writing
        // it here made the panel's combo look applied, then a later //e
        // session inherited the //c experiment (hunt #21).
        const auto p = pom2::profileFromKey(
            settings_.getString("system_profile", ""));
        if (!pom2::profileConfig(p).noPhysicalSlots)
            settings_.setString("chatmauve_variant",
                                LeChatMauveCard::variantKey(command.variantTo));
    }
}

std::vector<DevicePanelCoordinator::SerialSnapshot>
DevicePanelCoordinator::captureSerialCards() const
{
    std::vector<SerialSnapshot> snapshots;
    auto state = controller_.lockState();
    auto& bus = state.memory().slotBus();
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        const auto* card = dynamic_cast<const SuperSerialCard*>(bus.peripheral(slot));
        if (!card) continue;
        SerialSnapshot snapshot;
        snapshot.slot = slot;
        snapshot.port = card->getPort();
        snapshot.listening = card->isListening();
        snapshot.connected = card->clientConnected();
        snapshot.rawMode = card->rawMode();
        snapshot.printerTap = card->printerTap();
        snapshot.bytesRx = card->bytesRx();
        snapshot.bytesTx = card->bytesTx();
        snapshot.recentRxText = card->recentRxText();
        snapshot.recentTxText = card->recentTxText();
        snapshots.push_back(std::move(snapshot));
    }
    return snapshots;
}

DevicePanelCoordinator::SerialCommandResult
DevicePanelCoordinator::applySerial(const SerialCommand& command)
{
    SerialCommandResult result;
    if (command.empty() || command.slot < 1 || command.slot >= SlotBus::kSlotCount)
        return result;

    auto state = controller_.lockState();
    auto& bus = state.memory().slotBus();
    auto* card = dynamic_cast<SuperSerialCard*>(bus.peripheral(command.slot));
    if (!card) return result;
    result.cardFound = true;

    if (command.requestStop) card->stopListening();
    if (command.requestRawMode) card->setRawMode(command.rawMode);
    if (command.requestPrinterTap) card->setPrinterTap(command.printerTap);
    if (command.requestStart) {
        result.startAttempted = true;
        result.startSucceeded = card->startListening(command.port);
    }
    return result;
}

void DevicePanelCoordinator::persistSerial()
{
    const auto cards = captureSerialCards();
    for (const auto& card : cards) {
        const std::string suffix = "_slot" + std::to_string(card.slot);
        settings_.setBool("ssc_listening" + suffix, card.listening);
        settings_.setInt("ssc_port" + suffix, card.port);
        settings_.setBool("ssc_raw_mode" + suffix, card.rawMode);
        settings_.setBool("ssc_printer_tap" + suffix, card.printerTap);
    }
    if (!cards.empty()) {
        const auto& primary = cards.front();
        settings_.setBool("ssc_listening", primary.listening);
        settings_.setInt("ssc_port", primary.port);
        settings_.setBool("ssc_raw_mode", primary.rawMode);
    }
}

bool DevicePanelCoordinator::setChatMauveInvertBit7(bool enabled)
{
    bool applied = false;
    {
        auto state = controller_.lockState();
        auto& bus = state.memory().slotBus();
        if (auto* card = findCard<LeChatMauveCard>(bus)) {
            card->setInvertBit7(enabled);
            applied = true;
        }
    }
    if (applied) settings_.setBool("chatmauve_invert_bit7", enabled);
    return applied;
}

} // namespace pom2
