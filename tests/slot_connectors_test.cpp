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

// The Slot Configuration window is built from the machine's OWN connectors.
//
// It used to render one shape for every profile: seven rows labelled
// "Slot 1".."Slot 7" plus two AUX pseudo-rows. That is only true of a
// II/II+///e. An Apple //c has no expansion bus at all, so five of those
// rows were greyed built-ins, one offered a rear adapter under the label
// "Slot 7", and on a //c PAL the last one (slot 3) was dead in every
// direction — a control that could never do anything, named after a
// connector that does not exist on the machine.
//
// `buildConnectorLayout` is the data behind the new window. This pins what
// each machine really has, including the controls: a //c must expose NO
// expansion slot, and a II+ must expose NO internal header.

#include "SlotConnectors.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
void check(bool ok, const std::string& what)
{
    if (ok) { std::printf("  ok: %s\n", what.c_str()); return; }
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
}

using Layout = std::vector<pom2::ConnectorSection>;

Layout layoutFor(pom2::SystemProfile p)
{
    return pom2::buildConnectorLayout(pom2::profileConfig(p));
}

std::vector<std::string> titles(const Layout& l)
{
    std::vector<std::string> t;
    for (const auto& s : l) t.push_back(s.title);
    return t;
}

bool hasKind(const Layout& l, pom2::ConnectorKind k)
{
    for (const auto& s : l)
        for (const auto& r : s.rows)
            if (r.kind == k) return true;
    return false;
}

const pom2::Connector* rowForSlot(const Layout& l, int slot)
{
    for (const auto& s : l)
        for (const auto& r : s.rows)
            if (r.slot == slot) return &r;
    return nullptr;
}

int countRows(const Layout& l)
{
    int n = 0;
    for (const auto& s : l) n += static_cast<int>(s.rows.size());
    return n;
}

// Every machine: a POM2 slot index may drive at most ONE row, or the panel
// would stage two different cards into the same slot.
void checkNoSlotIsListedTwice(const Layout& l, const char* machine)
{
    std::set<int> seen;
    for (const auto& s : l) {
        for (const auto& r : s.rows) {
            if (r.slot < 0) continue;
            check(r.slot >= 1 && r.slot <= 7,
                  std::string(machine) + ": slot index in range");
            check(seen.insert(r.slot).second,
                  std::string(machine) + ": slot " + std::to_string(r.slot) +
                      " drives exactly one connector");
        }
    }
}

}  // namespace

int main()
{
    // ── Apple II+ : a real bus, nothing built in, cassette jacks ─────────
    {
        const Layout l = layoutFor(pom2::SystemProfile::AppleIIPlus);
        check(titles(l) == std::vector<std::string>{"Expansion slots", "Ports"},
              "II+: expansion slots, then ports");
        int slots = 0;
        for (const auto& r : l[0].rows)
            if (r.kind == pom2::ConnectorKind::ExpansionSlot) ++slots;
        check(slots == 7, "II+: seven free expansion slots");
        check(!hasKind(l, pom2::ConnectorKind::BuiltIn),
              "II+: nothing is soldered in");
        // The control for the //c assertions below: this machine HAS slots,
        // and has no internal header or AUX connector.
        check(!hasKind(l, pom2::ConnectorKind::InternalHeader),
              "II+: no internal expansion header");
        check(!hasKind(l, pom2::ConnectorKind::AuxSlot),
              "II+: no auxiliary connector");
        bool cassette = false;
        for (const auto& r : l[1].rows)
            if (r.label.find("Cassette") != std::string::npos) cassette = true;
        check(cassette, "II+: the cassette jacks are listed");
        checkNoSlotIsListedTwice(l, "II+");
    }

    // ── //e Enhanced PAL : bus + the AUX connector, no cassette ──────────
    {
        const Layout l = layoutFor(pom2::SystemProfile::AppleIIePAL);
        check(titles(l) == std::vector<std::string>{
                  "Expansion slots", "Auxiliary connector", "Ports"},
              "//e: slots, AUX connector, ports");
        check(hasKind(l, pom2::ConnectorKind::AuxSlot) &&
              hasKind(l, pom2::ConnectorKind::AuxMemory),
              "//e: the AUX slot and its memory size are both rows");
        bool cassette = false;
        for (const auto& s : l)
            for (const auto& r : s.rows)
                if (r.label.find("Cassette") != std::string::npos) cassette = true;
        check(!cassette, "//e: no cassette jacks (the //e dropped them)");
        checkNoSlotIsListedTwice(l, "//e");
    }

    // ── //c : NO expansion bus. This is the whole point. ─────────────────
    {
        const Layout l = layoutFor(pom2::SystemProfile::AppleIIc);
        check(titles(l) == std::vector<std::string>{
                  "External ports", "Built-in devices",
                  "Internal expansion connector"},
              "//c: ports, built-ins, internal header");
        check(!hasKind(l, pom2::ConnectorKind::ExpansionSlot),
              "//c: not one expansion slot is offered");
        for (const auto& s : l)
            for (const auto& r : s.rows)
                check(r.label.rfind("Slot ", 0) != 0,
                      "//c: no row is named after a slot that does not exist");

        // The DB-15 video expansion is the one port that takes a card the
        // user picks, and it takes exactly one.
        const auto* db15 = rowForSlot(l, 7);
        check(db15 && db15->label.find("DB-15") != std::string::npos,
              "//c: slot 7 is the DB-15 video expansion");
        check(db15 && db15->accepts ==
                          std::vector<std::string>{"", "chatmauve"},
              "//c: the DB-15 accepts the Chat Mauve adapter and nothing else");

        // The internal header is where a Mockingboard 4c goes.
        const auto* header = rowForSlot(l, 3);
        check(header && header->kind == pom2::ConnectorKind::InternalHeader,
              "//c: slot 3 is the internal expansion header");
        check(header && header->accepts ==
                            std::vector<std::string>{"", "mockingboard",
                                                     "mockingboard_c"},
              "//c: the header accepts the Mockingboard 4c only");

        // The serial ports and the disk port are named, not numbered.
        const auto* serial1 = rowForSlot(l, 1);
        check(serial1 && serial1->label.find("Serial port 1") != std::string::npos,
              "//c: slot 1 is the printer serial port");
        const auto* diskPort = rowForSlot(l, 5);
        check(diskPort && diskPort->label.find("DB-19") != std::string::npos,
              "//c: slot 5 is the rear disk port");
        checkNoSlotIsListedTwice(l, "//c");
    }

    // ── //c PAL : the Chat Mauve is soldered on, so the DB-15 is taken ───
    {
        const Layout l = layoutFor(pom2::SystemProfile::AppleIIcPAL);
        const auto* db15 = rowForSlot(l, 7);
        check(db15 != nullptr, "//c PAL: the DB-15 row exists");
        // The profile ships the adapter on-board; the panel renders the row
        // read-only from `builtInSlots[7]`, and the note says what is on it.
        check(db15 && db15->note.find("Chat Mauve") != std::string::npos,
              "//c PAL: the DB-15 note names the built-in adapter");
        checkNoSlotIsListedTwice(l, "//c PAL");
    }

    // ── //c+ : same shape as the //c ─────────────────────────────────────
    {
        const Layout l = layoutFor(pom2::SystemProfile::AppleIIcPlus);
        check(!hasKind(l, pom2::ConnectorKind::ExpansionSlot),
              "//c+: not one expansion slot is offered");
        check(countRows(l) > 0, "//c+: the inventory is not empty");
        checkNoSlotIsListedTwice(l, "//c+");
    }

    // ── The staged-change count: the window's one real invariant ────────
    {
        const auto& iic = pom2::profileConfig(pom2::SystemProfile::AppleIIc);
        const auto& iie = pom2::profileConfig(pom2::SystemProfile::AppleIIePAL);
        std::array<std::string, 8> live{}, draft{};

        check(pom2::pendingChangeCount(iie, draft, live, "", "", -1, 1) == 0,
              "nothing edited counts as nothing staged");

        draft[2] = "mockingboard";
        check(pom2::pendingChangeCount(iie, draft, live, "", "", -1, 1) == 1,
              "an edited slot counts once");

        // THE control. On a //c the panel force-feeds five built-in cards into
        // the draft; counting them would badge "staged changes" on a machine
        // where nothing is editable, and Apply wipes RAM.
        std::array<std::string, 8> iicDraft{}, iicLive{};
        for (std::size_t s = 1; s <= 7; ++s)
            if (iic.builtInSlots[s].has_value())
                iicDraft[s] = iic.builtInSlots[s]->cardKey;   // live stays empty
        check(pom2::pendingChangeCount(iic, iicDraft, iicLive, "", "", -1, 1) == 0,
              "a built-in row never makes Apply offer a cold boot");

        // Sentinels are not values.
        check(pom2::pendingChangeCount(iie, live, live, "", "feline", -1, 1) == 0,
              "an unstaged Chat Mauve model counts nothing");
        check(pom2::pendingChangeCount(iie, live, live, "eve", "feline", -1, 1) == 1,
              "a staged Chat Mauve model counts once");
        check(pom2::pendingChangeCount(iic, live, live, "eve", "feline", -1, 1) == 0,
              "…but never on a //c, where the DB-15 fixes the model");
        check(pom2::pendingChangeCount(iie, live, live, "", "", -1, 16) == 0,
              "an unstaged aux size counts nothing");
        check(pom2::pendingChangeCount(iie, live, live, "", "", 16, 16) == 0,
              "staging the size it already has counts nothing");
        check(pom2::pendingChangeCount(iie, live, live, "", "", 16, 1) == 1,
              "a staged aux size counts once — it cold-boots like the rest");
    }

    if (g_failures) return 1;
    std::puts("slot_connectors OK");
    return 0;
}
