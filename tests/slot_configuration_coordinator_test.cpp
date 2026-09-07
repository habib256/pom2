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

// Slot-configuration policy smoke test.
//
// The frontend builds card objects, but the effective mapping is owned and
// resolved by SlotConfigurationCoordinator. Pin the rules which previously
// lived as mutable policy in MainWindow.cpp.

#include "CffaCard.h"
#include "ClockCard.h"
#include "Settings.h"
#include "DiskIICard.h"
#include "EchoPlusCard.h"
#include "EchoPlusTMS5220Card.h"
#include "FujiNetCard.h"
#include "FourPlayCard.h"
#include "FujiNetCardFactory.h"
#include "GrapplerCard.h"
#include "LeChatMauveCard.h"
#include "LironCard.h"
#include "Mockingboard.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "PhasorCard.h"
#include "PrinterCard.h"
#include "ProDOSHardDiskCard.h"
#include "SlotCardCatalog.h"
#include "SlotConfigurationCoordinator.h"
#include "SlotBus.h"
#include "SmartPortCard.h"
#include "SoftCardZ80.h"
#include "SuperSerialCard.h"
#include "TranswarpCard.h"
#include "UthernetCard.h"
#include "UthernetIICard.h"
#include "WorkstationCard.h"

#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using Coordinator = pom2::SlotConfigurationCoordinator;
static_assert(std::is_same_v<
    decltype(std::declval<Coordinator&>().effectivePlan()),
    const Coordinator::CardMap&>);

int main()
{
    pom2::Settings settings;
    pom2::SlotConfigurationCoordinator slots;

    // Fresh-install //e mapping.
    const auto& defaults = slots.resolve(settings,
        pom2::SystemProfile::AppleIIe);
    assert(defaults[0].empty());
    assert(defaults[1] == "grappler");
    assert(defaults[2] == "mockingboard");   // DIX scans $C7→$C1 for its 6522
    assert(defaults[3].empty());
    assert(defaults[4] == "mouseaw");        // Apple's mouse slot; Extasie hard-codes $C4xx
    assert(defaults[5] == "smartport35");
    assert(defaults[6] == "diskii");
    assert(defaults[7] == "chatmauve");

    // Staged edits have an explicit owner and cannot mutate the plan used to
    // build the current machine until settings are deliberately committed.
    slots.draft()[1] = "clock";
    assert(slots.draft()[1] == "clock");
    assert(slots.effectivePlan()[1] == "grappler");
    slots.resetDraft();
    assert(slots.draft() == slots.effectivePlan());

    // Ordinary cards are unique: the first requested occurrence wins.
    settings.setString("slot_1_card", "mockingboard");
    settings.setString("slot_2_card", "mockingboard");
    settings.setString("slot_3_card", "");
    settings.setString("slot_4_card", "");
    settings.setString("slot_5_card", "");
    settings.setString("slot_6_card", "");
    settings.setString("slot_7_card", "");
    const auto& unique = slots.resolve(settings,
        pom2::SystemProfile::AppleIIe);
    assert(unique[1] == "mockingboard");
    assert(unique[2].empty());

    // Storage devices with per-slot state deliberately support multiples.
    settings.setString("slot_1_card", "diskii");
    settings.setString("slot_2_card", "diskii");
    settings.setString("slot_3_card", "cffa");
    settings.setString("slot_4_card", "cffa");
    settings.setString("slot_5_card", "smartport35");
    settings.setString("slot_6_card", "smartport35");
    const auto& multiple = slots.resolve(settings,
        pom2::SystemProfile::AppleIIe);
    assert(multiple[1] == "diskii" && multiple[2] == "diskii");
    assert(multiple[3] == "cffa" && multiple[4] == "cffa");
    assert(multiple[5] == "smartport35" && multiple[6] == "smartport35");

    // A //c has fixed virtual devices and no physical expansion slots. Its
    // two built-in SSCs are a legitimate duplicate; a rear Chat Mauve video
    // adapter remains allowed because it does not use the expansion bus.
    settings.setString("slot_1_card", "phasor");
    settings.setString("slot_2_card", "uthernet");
    settings.setString("slot_3_card", "cffa");
    settings.setString("slot_4_card", "mockingboard");
    settings.setString("slot_5_card", "hdv");
    settings.setString("slot_6_card", "clock");
    settings.setString("slot_7_card", "chatmauve");
    const auto& iic = slots.resolve(settings,
        pom2::SystemProfile::AppleIIc);
    assert(iic[1] == "ssc" && iic[2] == "ssc");
    assert(iic[3].empty());
    assert(iic[4] == "mouseaw");
    assert(iic[5] == "smartport35");
    assert(iic[6] == "diskii");
    assert(iic[7] == "chatmauve");

    assert(pom2::SlotConfigurationCoordinator::isMultiInstance("diskii"));
    assert(!pom2::SlotConfigurationCoordinator::isMultiInstance("ssc"));

    // Live topology is copied from SlotBus, independently of the effective
    // configuration. Exact implementation variants keep distinct keys.
    SlotBus bus;
    std::vector<std::string> seenLiveKeys;
    auto expectLiveKey = [&](std::unique_ptr<SlotPeripheral> card,
                             const char* expected) {
        bus.plug(3, std::move(card));
        const auto live = slots.captureLive(bus);
        assert(live.keys[3] == expected);
        assert(live.plugged(3));
        assert(!live.names[3].empty());
        seenLiveKeys.emplace_back(expected);
    };
    expectLiveKey(std::make_unique<DiskIICard>(3), "diskii");
    expectLiveKey(std::make_unique<ProDOSHardDiskCard>(3), "hdv");
    expectLiveKey(std::make_unique<pom2::CffaCard>(3), "cffa");
    expectLiveKey(std::make_unique<pom2::SmartPortCard>(3), "smartport35");
    expectLiveKey(std::make_unique<SuperSerialCard>(3), "ssc");
    expectLiveKey(std::make_unique<PrinterCard>(3), "printer");
    expectLiveKey(std::make_unique<GrapplerCard>(3), "grappler");
    expectLiveKey(std::make_unique<ClockCard>(3), "clock");
    expectLiveKey(std::make_unique<SoftCardZ80>(), "softcard");
    expectLiveKey(std::make_unique<LeChatMauveCard>(3), "chatmauve");
    expectLiveKey(std::make_unique<MouseCard>(3), "mouse");
    expectLiveKey(std::make_unique<MouseCardAppleWin>(3), "mouseaw");
    expectLiveKey(std::make_unique<MockingboardCard>(
        3, MockingboardCard::Variant::AC), "mockingboard");
    expectLiveKey(std::make_unique<MockingboardCard>(
        3, MockingboardCard::Variant::SoundII), "mockingboard_c");
    expectLiveKey(std::make_unique<PhasorCard>(3), "phasor");
    expectLiveKey(std::make_unique<EchoPlusCard>(3), "echoplus");
    expectLiveKey(std::make_unique<EchoPlusTMS5220Card>(3), "echoplus_tms");
    expectLiveKey(std::make_unique<pom2::UthernetCard>(3), "uthernet");
    expectLiveKey(std::make_unique<pom2::UthernetIICard>(3), "uthernet2");
    expectLiveKey(pom2::makeFujiNetCard(3), "fujinet");
    // hunt #4 #31g: these three had no liveCardKey branch, so captureLive()
    // reported their slot as empty and every "planned vs plugged" comparison
    // showed a pending change that could not be applied away.
    expectLiveKey(std::make_unique<pom2::WorkstationCard>(3), "workstation");
    expectLiveKey(std::make_unique<pom2::FourPlayCard>(3), "4play");
    expectLiveKey(std::make_unique<pom2::TranswarpCard>(3), "transwarp");
    expectLiveKey(std::make_unique<pom2::LironCard>(3), "liron");

    // Every catalog key a live card can carry must round-trip. Keys with no
    // distinct card object of their own are listed explicitly so ADDING a
    // card type without teaching liveCardKey about it fails here.
    {
        static const char* kNoLiveObject[] = {
            "",       // the empty slot
        };
        for (const auto& ct : pom2::kCardTypes) {
            bool exempt = false;
            for (const char* e : kNoLiveObject)
                if (std::string(ct.key) == e) exempt = true;
            if (exempt) continue;
            bool seen = false;
            for (const std::string& k : seenLiveKeys)
                if (k == ct.key) seen = true;
            if (!seen)
                std::fprintf(stderr,
                    "catalog key '%s' has no liveCardKey branch\n", ct.key);
            assert(seen && "SlotConfigurationCoordinator::liveCardKey is "
                           "missing a catalog card type");
        }
    }

    // ── hunt #4 #7: the de-dup must not eat the user's saved key ────────
    // Two slots asking for the same single-instance card: resolve() empties
    // the SECOND one. The shutdown writer persists the plan, so without a
    // record of WHAT was dropped it wrote "" over slot_5_card and the card
    // was gone on the next launch.
    settings.setString("slot_1_card", "");
    settings.setString("slot_2_card", "hdv");
    settings.setString("slot_3_card", "");
    settings.setString("slot_4_card", "");
    settings.setString("slot_5_card", "hdv");
    settings.setString("slot_6_card", "diskii");
    settings.setString("slot_7_card", "");
    const auto& dedup = slots.resolve(settings, pom2::SystemProfile::AppleIIe);
    assert(dedup[2] == "hdv");
    assert(dedup[5].empty());
    assert(slots.dedupCleared(5) == "hdv");
    assert(slots.dedupCleared(2).empty());
    assert(slots.dedupCleared(6).empty());          // multi-instance, kept
    // ...and the Abstraction Levels swap must refuse before it gets there.
    assert(slots.wouldDuplicate("hdv", 5));         // slot 2 already has it
    assert(!slots.wouldDuplicate("hdv", 2));        // that IS the holder
    assert(!slots.wouldDuplicate("diskii", 3));     // multi-instance
    assert(!slots.wouldDuplicate("", 3));
    assert(!slots.wouldDuplicate("cffa", 3));       // nobody holds it

    // A live failure/absence must never erase the effective request. This is
    // the contract that preserves a CFFA assignment while its ROM is missing.
    settings.setString("slot_1_card", "cffa");
    settings.setString("slot_2_card", "");
    settings.setString("slot_3_card", "");
    settings.setString("slot_4_card", "");
    settings.setString("slot_5_card", "");
    settings.setString("slot_6_card", "");
    settings.setString("slot_7_card", "");
    slots.resolve(settings, pom2::SystemProfile::AppleIIe);
    assert(slots.effectivePlan()[1] == "cffa");
    (void)slots.captureLive(SlotBus{});
    assert(slots.effectivePlan()[1] == "cffa");

    std::printf("slot_configuration_coordinator_test: OK\n");
    return 0;
}
