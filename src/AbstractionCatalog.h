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

// AbstractionCatalog — POM2's LLE/HLE scale and the static catalog of what
// each subsystem is emulated as. Pure data, no ImGui: the Abstraction Levels
// panel (AbstractionLevels_ImGui) draws it and the Slot Configuration picker
// tags every card with it, and `abstraction_catalog` checks that every card
// the picker can offer has a row. `docs/lle_vs_hle.md` is the reference for
// every level here; edit the two together.

#ifndef POM2_ABSTRACTION_CATALOG_H
#define POM2_ABSTRACTION_CATALOG_H

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace pom2 {

/// POM2's five-level scale, from silicon to pure host function.
/// Definitions (and the reasoning behind the cut points) in
/// `docs/lle_vs_hle.md` § The POM2 scale. `Host` is the sixth,
/// off-axis bucket: machinery with no hardware referent at all.
enum class AbsLevel {
    L0,    ///< Silicon: internal state machine + timing; a real ROM runs on top
    L1,    ///< Chip-faithful: full register/protocol model at bus timing
    L2,    ///< Real firmware executing, host device underneath
    H1,    ///< Synthetic firmware: invented register protocol, host does the work
    H2,    ///< Host function: nothing guest-visible at all
    Host   ///< Off-axis: host-side machinery with no hardware referent
};

/// Short badge text ("L0") and the long name ("Silicon").
const char* levelBadge(AbsLevel l);
const char* levelName (AbsLevel l);
/// True for L0-L2 — the LLE half of the scale. `Host` is neither.
bool        levelIsLle(AbsLevel l);

/// One catalogued subsystem. Static data, mirroring the master table of
/// `docs/lle_vs_hle.md` — keep the two in step when either moves.
struct AbsEntry {
    const char* id;        ///< Stable key the caller's live rows are matched on.
    const char* group;     ///< Section heading.
    const char* subsystem; ///< Human name.
    AbsLevel    level;     ///< Level as classified in the doc.
    const char* modelled;  ///< What is actually modelled, one line.
    const char* whyNot;    ///< Why not lower (or why this is the floor).
    const char* files;     ///< Implementation files, for the reader who wants the code.
};

/// The catalog itself.
const std::vector<AbsEntry>& abstractionCatalog();

/// The catalog row a Slot Configuration card key is classified under, or
/// nullptr. Several keys share a row (the SmartPort variants, the
/// Mockingboard family, the Echo cards); the rest are their own id.
const AbsEntry* abstractionEntryForCardKey(std::string_view key);

/// Whether card `key` is really on the bus, for the panel's "Now" column.
/// `plan` is the slot plan (MainWindow::slotCards, what the user asked for),
/// `occupied` what the live SlotBus holds per slot. Both are needed: a
/// ROM-gated card refused for a missing dump (CFFA, Slinky, Videoterm,
/// Workstation, Grappler 1981, PIC) leaves its slot empty while the plan
/// still names it, and the plan alone reported it Active. Slots 1-7.
bool slotPlanKeyLive(const std::array<std::string, 8>& plan,
                     const std::array<bool, 8>& occupied,
                     std::string_view key);

} // namespace pom2

#endif // POM2_ABSTRACTION_CATALOG_H
