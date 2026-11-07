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

// abstraction_catalog — the LLE/HLE catalog against the cards the Slot
// Configuration picker offers (CARDS-001).
//
// 1. Every card key in `kCardTypes` resolves to a catalog row. Ten did not
//    until 2026-11-07 (workstation, 4play, transwarp, slinky, sam, videoterm,
//    vidhd, grappler1, pic, snesmax): the picker showed them with no
//    [LLE]/[HLE] tag, and the Abstraction Levels panel did not list them.
// 2. Ids are unique — a duplicate would shadow the second row's live state.
// 3. Each group is ONE contiguous run. The panel prints a heading whenever
//    the group changes, and four groups used to appear twice.
// 4. Every row carries a non-empty description, reason and file list.
// 5. The panel's plug rule: a slot counts only when the plan names the card
//    AND the live bus holds one there. A refused ROM-gated card (its dump
//    missing) leaves the slot empty with the plan unchanged, and was shown
//    Active. slot_card_factory pins the refusal itself.

#include "AbstractionCatalog.h"
#include "SlotCardCatalog.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

static int failures = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);       \
            std::fprintf(stderr, __VA_ARGS__);                              \
            std::fprintf(stderr, "\n");                                     \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

int main()
{
    const auto& cat = pom2::abstractionCatalog();
    CHECK(!cat.empty(), "catalog is empty");

    // 1. Every pickable card has a level.
    int cards = 0;
    for (const auto& ct : pom2::kCardTypes) {
        if (ct.key[0] == '\0') continue;           // the empty slot
        ++cards;
        CHECK(pom2::abstractionEntryForCardKey(ct.key) != nullptr,
              "card '%s' (%s) has no abstraction catalog row", ct.key, ct.label);
    }
    CHECK(cards > 30, "only %d card keys seen — kCardTypes not read?", cards);
    CHECK(pom2::abstractionEntryForCardKey("") == nullptr, "empty key resolved");
    CHECK(pom2::abstractionEntryForCardKey("no_such_card") == nullptr,
          "unknown key resolved");

    // The aliases land where the picker expects them.
    struct Alias { const char* key; const char* id; };
    for (const Alias a : {Alias{"smartport35", "smartportcard"},
                          Alias{"superdrive", "smartportcard"},
                          Alias{"printer", "printercard"},
                          Alias{"phasor", "mockingboard"},
                          Alias{"mockingboard_c", "mockingboard"},
                          Alias{"echoplus", "ssi263"},
                          Alias{"echoplus_tms", "tms5220"}}) {
        const auto* e = pom2::abstractionEntryForCardKey(a.key);
        CHECK(e && std::strcmp(e->id, a.id) == 0,
              "'%s' should resolve to '%s', got '%s'", a.key, a.id,
              e ? e->id : "(none)");
    }

    // 2 + 3 + 4.
    std::set<std::string> ids, closedGroups;
    const char* group = nullptr;
    for (const auto& e : cat) {
        CHECK(ids.insert(e.id).second, "duplicate catalog id '%s'", e.id);
        if (!group || std::strcmp(group, e.group) != 0) {
            if (group) closedGroups.insert(group);
            CHECK(closedGroups.count(e.group) == 0,
                  "group '%s' resumes at '%s' — its heading would print twice",
                  e.group, e.id);
            group = e.group;
        }
        CHECK(e.subsystem && *e.subsystem, "'%s' has no subsystem name", e.id);
        CHECK(e.modelled && *e.modelled, "'%s' has no 'modelled' text", e.id);
        CHECK(e.whyNot && *e.whyNot, "'%s' has no 'why not lower' text", e.id);
        CHECK(e.files && *e.files, "'%s' has no file list", e.id);
    }

    // 5.
    std::array<std::string, 8> plan{};
    std::array<bool, 8> occupied{};
    plan[3] = "slinky";                       // asked for, refused: slot empty
    plan[6] = "diskii";  occupied[6] = true;  // asked for and on the bus
    occupied[2] = true;                       // a card the plan does not name
    CHECK(!pom2::slotPlanKeyLive(plan, occupied, "slinky"),
          "a refused card (plan yes, bus empty) reads as plugged");
    CHECK(pom2::slotPlanKeyLive(plan, occupied, "diskii"),
          "a plugged card reads as absent");
    CHECK(!pom2::slotPlanKeyLive(plan, occupied, "mockingboard"),
          "an unplanned key reads as plugged");
    CHECK(!pom2::slotPlanKeyLive(plan, occupied, ""),
          "the empty key matched an empty planned slot");
    plan[0] = "slinky"; occupied[0] = true;   // slot 0 is not a slot here
    CHECK(!pom2::slotPlanKeyLive(plan, occupied, "slinky"),
          "slot 0 was read");

    if (failures) {
        std::fprintf(stderr, "abstraction_catalog: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("abstraction_catalog: %zu rows, %d card keys, all classified\n",
                cat.size(), cards);
    return 0;
}
