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

// SlotConnectors — what THIS machine actually has to plug things into.
//
// Slot Configuration used to render one shape for every machine: seven rows
// labelled "Slot 1".."Slot 7", plus two pseudo-rows for the AUX connector.
// That shape is only true of a II/II+///e. An Apple //c has **no expansion
// bus at all** (`ProfileConfig::noPhysicalSlots`): its peripherals are
// soldered or hang off connectors on the back panel. The panel modelled that
// by greying seven rows that correspond to nothing you can point at on the
// real machine — and on a //c PAL, where the profile also ships the Chat
// Mauve on-board, the single remaining row (slot 3) was dead in every
// direction: a control that could never do anything.
//
// So the window is built from the machine's own connector inventory instead.
// A row exists here only if the connector exists on the hardware, it is
// named the way the manual names it, and its KIND says what the user may do
// with it:
//
//   * `ExpansionSlot`   — a real slot on a real bus: pick any card.
//   * `BuiltIn`         — soldered in. Read-only, shown so the user knows
//                         what is there.
//   * `ExternalPort`    — a connector on the case. Some carry a fixed
//                         on-board controller (the //c's serial ports), some
//                         take an adapter the user chooses (the DB-15).
//   * `InternalHeader`  — the //c's internal expansion connector, where a
//                         Mockingboard 4c goes.
//   * `AuxSlot`         — the //e auxiliary connector (80-column card).
//   * `AuxMemory`       — the RamWorks size that connector carries.
//
// The POM2 slot index each row drives is kept in `slot`, so persistence
// (`slot_N_card`) and the plug back-end are untouched: a connector is a
// LABEL over a slot index, not a new mechanism. On a //c the two
// user-choosable connectors map to the two slots the profile leaves free —
// the DB-15 video expansion to slot 7 (the canonical Chat Mauve slot) and
// the internal header to slot 3 — which is what makes an existing
// `state.cfg` keep working across this change.

#ifndef POM2_SLOT_CONNECTORS_H
#define POM2_SLOT_CONNECTORS_H

#include "SystemProfile.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace pom2 {

enum class ConnectorKind {
    ExpansionSlot,
    BuiltIn,
    ExternalPort,
    InternalHeader,
    AuxSlot,
    AuxMemory,
};

struct Connector {
    ConnectorKind kind = ConnectorKind::ExpansionSlot;
    /// POM2 slot index this row drives, or -1 for a row that carries no card
    /// (the game port, the cassette, the AUX memory size).
    int         slot = -1;
    /// How the machine's own documentation names the connector.
    std::string label;
    /// Secondary line: what is on it, or where to go for its media.
    std::string note;
    /// Card keys this connector accepts. EMPTY means the whole catalog —
    /// which is only ever true of a real `ExpansionSlot`. A port that takes
    /// exactly one adapter lists it, so the picker cannot offer a card that
    /// physically cannot go there.
    std::vector<std::string> accepts;
};

struct ConnectorSection {
    std::string            title;
    std::string            blurb;
    std::vector<Connector> rows;
};

/// Build the connector inventory for `cfg`. Pure data — no ImGui, no
/// machine state — so it can be pinned by a headless test. The caller
/// caches it and rebuilds on a profile switch (it allocates).
inline std::vector<ConnectorSection> buildConnectorLayout(const ProfileConfig& cfg)
{
    std::vector<ConnectorSection> out;

    // ── //c-class: no expansion bus. Name the real connectors. ───────────
    if (cfg.noPhysicalSlots) {
        const auto builtInLabel = [&cfg](int slot) -> std::string {
            const auto& b = cfg.builtInSlots[static_cast<std::size_t>(slot)];
            return b.has_value() ? b->label : std::string{};
        };

        ConnectorSection ports;
        ports.title = "External ports";
        ports.blurb = "The connectors on the back panel. The controller "
                      "behind each one is soldered; what you plug into it is "
                      "yours to choose.";
        // Serial 1 / serial 2 are DIN-5 sockets driven by on-board 6551s —
        // the profile forces an `ssc` into slots 1 and 2 for exactly that.
        ports.rows.push_back({ConnectorKind::ExternalPort, 1,
                              "Serial port 1 (printer, DIN-5)",
                              builtInLabel(1), {}});
        ports.rows.push_back({ConnectorKind::ExternalPort, 2,
                              "Serial port 2 (modem, DIN-5)",
                              builtInLabel(2), {}});
        // The hand-control port is also where the //c's mouse lives: the IOU
        // reads it, which is why POM2 carries `iicmouse` rather than a card.
        ports.rows.push_back({ConnectorKind::ExternalPort, 4,
                              "Hand controls (DB-9)",
                              builtInLabel(4), {}});
        // The disk port chains external drives; the drives themselves are
        // mounted in Internal Disks & Media, not here.
        ports.rows.push_back({ConnectorKind::ExternalPort, 5,
                              "Disk port (DB-19)",
                              builtInLabel(5), {}});
        // The one connector on a //c that takes a card the user picks: the
        // video expansion, i.e. the Chat Mauve "Adaptateur IIc". On //c PAL
        // the profile solders it on, and the row becomes read-only by way of
        // `builtInSlots[7]` — the panel checks that, not this table.
        ports.rows.push_back({ConnectorKind::ExternalPort, 7,
                              "Video expansion (DB-15)",
                              cfg.builtInSlots[7].has_value()
                                  ? builtInLabel(7)
                                  : "Le Chat Mauve RGB adapter",
                              {"", "chatmauve"}});
        out.push_back(std::move(ports));

        ConnectorSection internals;
        internals.title = "Built-in devices";
        internals.blurb = "Soldered to the board — shown so you know what the "
                          "machine already has.";
        internals.rows.push_back({ConnectorKind::BuiltIn, 6,
                                  "Internal drive", builtInLabel(6), {}});
        out.push_back(std::move(internals));

        // The internal expansion connector: a header INSIDE the case. The
        // Mockingboard 4c is the card that goes on it, answering at
        // $C400-$C4FF while the machine's own IOU mouse keeps slot 4.
        ConnectorSection header;
        header.title = "Internal expansion connector";
        header.blurb = "A header inside the case. A Mockingboard 4c mounts "
                       "here and answers at $C400-$C4FF.";
        header.rows.push_back({ConnectorKind::InternalHeader, 3,
                               "Expansion header", "",
                               {"", "mockingboard", "mockingboard_c"}});
        out.push_back(std::move(header));
        return out;
    }

    // ── II / II+ / //e: a real expansion bus. ────────────────────────────
    ConnectorSection slots;
    slots.title = "Expansion slots";
    slots.blurb = cfg.iieMode
        ? "Seven slots on the expansion bus. Slot 3 belongs to the built-in "
          "80-column firmware on a //e."
        : "Seven slots on the expansion bus. Slot 0 (the Language Card) is "
          "not modelled as a card.";
    for (int s = 1; s <= 7; ++s) {
        Connector row;
        row.kind  = cfg.builtInSlots[static_cast<std::size_t>(s)].has_value()
                        ? ConnectorKind::BuiltIn
                        : ConnectorKind::ExpansionSlot;
        row.slot  = s;
        row.label = "Slot " + std::to_string(s);
        if (row.kind == ConnectorKind::BuiltIn)
            row.note = cfg.builtInSlots[static_cast<std::size_t>(s)]->label;
        slots.rows.push_back(std::move(row));
    }
    out.push_back(std::move(slots));

    if (cfg.iieMode) {
        ConnectorSection aux;
        aux.title = "Auxiliary connector";
        aux.blurb = "The //e's AUX slot. Its 80-column card is part of the "
                    "machine; only the memory on it is a choice.";
        aux.rows.push_back({ConnectorKind::AuxSlot, -1, "AUX slot",
                            "Extended 80-Column Card (built-in, $C300 firmware)",
                            {}});
        aux.rows.push_back({ConnectorKind::AuxMemory, -1, "AUX memory", "", {}});
        out.push_back(std::move(aux));
    }

    ConnectorSection ports;
    ports.title = "Ports";
    ports.blurb = "Connectors that take no card. Their devices are configured "
                  "from the Devices menu.";
    ports.rows.push_back({ConnectorKind::ExternalPort, -1,
                          cfg.iieMode ? "Game port (DB-9 + 16-pin)"
                                      : "Game port (16-pin)",
                          "Paddles / joystick — Devices \xe2\x86\x92 Joystick",
                          {}});
    if (!cfg.iieMode) {
        // The cassette jacks are II/II+ only: the //e dropped them, and the
        // //c never had them.
        ports.rows.push_back({ConnectorKind::ExternalPort, -1,
                              "Cassette in / out",
                              "Devices \xe2\x86\x92 Cassette", {}});
    }
    out.push_back(std::move(ports));
    return out;
}

/// How many staged edits the Slot Configuration window is holding — the
/// number on its badge, and the gate on its Apply button.
///
/// Extracted from the panel because it is the window's one real invariant and
/// ImGui offers no seam to test it through: **a row the user cannot edit must
/// never make Apply offer a cold boot.** The panel force-feeds the draft with
/// the profile's built-in cards (a //c has five), so counting all seven slots
/// would light up "5 staged changes" on a machine where nothing is editable,
/// and Apply wipes RAM.
///
/// `chatMauveDraft` empty and `ramWorksDraft` negative are the "nothing
/// staged" sentinels, not values. A //c-class profile never stages the Chat
/// Mauve model: the DB-15 connector fixes it.
inline int pendingChangeCount(const ProfileConfig& cfg,
                              const std::array<std::string, 8>& draft,
                              const std::array<std::string, 8>& live,
                              std::string_view chatMauveDraft,
                              std::string_view chatMauveLive,
                              int ramWorksDraft,
                              int ramWorksLive)
{
    int pending = 0;
    for (std::size_t s = 1; s <= 7; ++s) {
        if (cfg.builtInSlots[s].has_value()) continue;
        if (draft[s] != live[s]) ++pending;
    }
    if (!cfg.noPhysicalSlots && !chatMauveDraft.empty() &&
        chatMauveDraft != chatMauveLive)
        ++pending;
    if (ramWorksDraft >= 0 && ramWorksDraft != ramWorksLive)
        ++pending;
    return pending;
}

}  // namespace pom2

#endif  // POM2_SLOT_CONNECTORS_H
