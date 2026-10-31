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

// AbstractionLevels_ImGui — "what does POM2 emulate as silicon, and what does
// it emulate as a service?", as a window instead of a document.
//
// The prose lives in `docs/lle_vs_hle.md` and stays the reference; this panel
// is that document's master table made *live*. Two things a Markdown file
// cannot do, and they are the whole reason the window exists:
//
//   1. **Say which level is running right now.** Nearly every ROM-driven low
//      level in POM2 degrades SILENTLY to a higher one when its dump is
//      absent — Disk II drops from the real P6 LSS PROM to the legacy
//      32-cycle nibble gate, the Mouse Card falls from an executing M68705
//      mask ROM to a C++ state machine, ClockCard and Grappler+ fall back to
//      synthetic ROMs. The machine still works, which is correct product
//      behaviour and exactly why nobody notices. `docs/lle_vs_hle.md`
//      ("Keeping a level once you have it") names this as a structural hole
//      and proposes reporting *degraded* rather than merely *missing*. This
//      panel is that report.
//   2. **Let the user move the boundary.** Four subsystems ship both levels
//      behind one interface — that is a deliberate policy ("when both levels
//      have real users, ship both"), but it was only reachable by knowing
//      which catalog key meant which. Here the choice is the level.
//
// Host-side only: the catalog is static data, the live state is a snapshot
// the caller fills, and every switch is returned as a request for the caller
// to execute. The panel takes no lock and touches no emulator state.

#ifndef POM2_ABSTRACTION_LEVELS_IMGUI_H
#define POM2_ABSTRACTION_LEVELS_IMGUI_H

#include <cstddef>
#include <string>
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

/// The switchable boundaries. One enumerator per subsystem that genuinely
/// ships both levels — not per level, since each is a two-way choice.
enum class AbsToggle {
    None,
    MouseCard,       ///< `mouse` (L0, M68705 mask ROM executes) / `mouseaw` (H1)
    BlockStorage,    ///< `cffa` (L2, real firmware + ATA) / `hdv` (H1, synthetic)
    PrinterIface,    ///< `grappler` (L2, real EPROM) / `printer` (H1, synthetic)
    SerialFirmware,  ///< `ccs7710`: CCS's PROMs (L2) / POM2's page (H1)
    CompositeVideo   ///< OpenEmulator signal demodulation (L1) / artifact LUT (H1)
};

class AbstractionLevels_ImGui
{
public:
    /// How a catalogued subsystem is doing right now.
    enum class Live {
        NotApplicable,  ///< Always present (CPU, memory) — no plug state to report.
        Active,         ///< Running at the catalogued level.
        Degraded,       ///< Running, but at a HIGHER level than catalogued (missing dump).
        NotPlugged      ///< No such card on the bus this session.
    };

    /// Live state for one catalog id. Only ids the caller can actually
    /// observe need a row; anything absent renders as `NotApplicable`.
    struct Row {
        std::string id;
        Live        live   = Live::NotApplicable;
        AbsLevel    actual = AbsLevel::L0;  ///< Meaningful when live == Degraded.
        std::string detail;                 ///< Why, in one clause. May be empty.
    };

    /// One side of a switchable boundary.
    struct ToggleOption {
        std::string label;      ///< "MAME M68705 (real MCU ROM)"
        AbsLevel    level  = AbsLevel::L0;
        bool        available = true;   ///< False → greyed (a dump is missing, say).
        std::string why;        ///< Tooltip: what this side buys and costs.
        std::string blockedBy;  ///< Shown when !available.
    };

    struct ToggleState {
        AbsToggle    id = AbsToggle::None;
        std::string  title;         ///< "Mouse Card"
        ToggleOption low;           ///< The LOWER-level (more faithful) side.
        ToggleOption high;          ///< The HIGHER-level (more forgiving) side.
        int          selected = 0;  ///< 0 = low, 1 = high, -1 = neither is live.
        bool         needsRestart = false;  ///< Switching restarts the machine.
        std::string  note;          ///< Extra line under the selector. May be empty.
    };

    struct Snapshot {
        std::vector<Row>         rows;
        std::vector<ToggleState> toggles;
    };

    /// What the user asked for this frame.
    struct Request {
        AbsToggle toggle = AbsToggle::None;
        int       option = 0;   ///< 0 = low, 1 = high.
    };

    /// Draw the window. `open` is the caller's show-flag (the title-bar close
    /// button clears it). Returns `AbsToggle::None` unless a switch was hit.
    Request render(bool* open, const Snapshot& snap);

private:
    // ── Filtre (etat local du panneau) ──────────────────────────────────
    char        search_[64] = {};
};

} // namespace pom2

#endif // POM2_ABSTRACTION_LEVELS_IMGUI_H
