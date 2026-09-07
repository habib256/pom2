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

// CliDispatcher — parses POM2's command line into a CliPlan. Inspired by
// POM1's 3-phase parser, narrowed to Apple II / II+ verbs.
//
//   Phase A (boot)       : preset, --speed/--cpu-max, initial tape path,
//                          save-tape path.
//   Phase B (first frame): apply the speed override + tape path inside
//                          MainWindow's first render() call.
//   Phase C (deferred)   : --load addr:file, --run addr, --paste, --step,
//                          --trace-brk, --play/--rec/--rewind,
//                          --snapshot-save AND --snapshot-load. Fires after a
//                          short settling period, in COMMAND-LINE ORDER — so
//                          a `--snapshot-load x --run 300` does the restore
//                          first because it was typed first, not because of
//                          any phase split. (This comment used to place
//                          --snapshot-load in Phase A; the parser has always
//                          pushed it as a deferred action like the others.)
//
// Every parser is dependency-free for unit testing — pass argv,
// receive std::optional<CliPlan>.

#ifndef POM2_CLI_DISPATCHER_H
#define POM2_CLI_DISPATCHER_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class EmulationController;

namespace pom2 {

/// Save-tape format hint. NoHint defers to the path's file extension.
enum class CliSaveTapeFormat { NoHint, Aci, Wav };

/// Initial hi-res rendering mode. NoHint leaves the Apple2Display default
/// (Color NTSC) untouched. The other values mirror Apple2Display::HiResMode
/// without making this header depend on the display class.
enum class CliDisplayMode {
    NoHint,
    ColorNTSC,
    ChatMauveRGB,
    MonoWhite,
    MonoGreen,
    MonoAmber,
};

/// Apple II preset. Maps 1:1 to `pom2::SystemProfile` plus a `Default`
/// sentinel meaning "use the persisted/profile-default selection from
/// Settings". When non-Default, the CliDispatcher commands the MainWindow
/// to apply that profile via `MainWindow::applyProfile(...)` during
/// Phase A boot.
enum class CliPreset {
    Default,
    AppleII,            // → pom2::SystemProfile::AppleII
    AppleIIPlus,        // → pom2::SystemProfile::AppleIIPlus
    AppleIIeUnenhanced, // → pom2::SystemProfile::AppleIIeUnenhanced (1983 NMOS)
    AppleIIe,           // → pom2::SystemProfile::AppleIIe
    AppleIIc,       // → pom2::SystemProfile::AppleIIc
    AppleIIcPlus,   // → pom2::SystemProfile::AppleIIcPlus
    AppleIIePAL,    // → pom2::SystemProfile::AppleIIePAL (50 Hz)
    AppleIIcPAL,    // → pom2::SystemProfile::AppleIIcPAL (Le Chat Mauve)
    AppleIIeUnenhancedPAL, // → SystemProfile::AppleIIeUnenhancedPAL
                           //   (NMOS 6502 + 50 Hz — the French Touch machine)
};

/// One deferred action consumed in Phase C, in CLI order.
struct CliAction {
    enum class Kind {
        Load,          // addressI + pathS  : memWrite the file at addressI
        Run,           // addressI          : setProgramCounter + start running
        Paste,         // pathS             : feed file content as keystrokes (≤ 4096)
        Step,          // countI            : single-step countI times
        TraceBrk,      // (no args)         : enable BRK trace dump
        PlayTape,      // (no args)         : cassette PLAY
        RecTape,       // (no args)         : cassette REC arm
        RewindTape,    // (no args)         : cassette REW
        SnapshotSave,  // pathS             : write current state to .snap
        SnapshotLoad,  // pathS             : restore from .snap
    };

    Kind        kind;
    int         addressI = 0;
    int         countI   = 0;
    std::string pathS;
};

struct CliPlan {
    // Phase A — read by main() / MainWindow constructor.
    CliPreset                       preset = CliPreset::Default;
    bool                            cpuMax = false;
    /// `--ii-plus`: ignore `roms/apple2e.rom` even when present and force
    /// II+ mode. Useful for software that boots cleanly under the legacy
    /// 12 KB ROM but trips a still-unresolved IIe-paging bug under the
    /// 16 KB Enhanced ROM.
    bool                            forceIIPlus = false;

    /// Positional disk image (first non-flag argument). When set, main()
    /// mounts it into the matching slot — 5.25" Disk II / 800K 3.5" /
    /// ProDOS HDV, picked by `classifyDiskForSlot` — and boots from it
    /// once the worker thread + first frame have settled. Works in both
    /// GUI and `--kiosk` mode.
    std::string                     bootDiskPath;

    /// `--prodos-folder <path>` synthesises a read-only /HOST/ ProDOS
    /// block volume from a host directory and mounts it before the optional
    /// positional boot disk starts.
    std::string                     prodosFolderPath;

    /// `--kiosk`: launch chrome-free, full-screen, with no menus/panels —
    /// only the Apple II screen. Implies the positional disk auto-boot
    /// above. The window can only be closed via the OS (Alt-F4 / WM).
    bool                            kiosk = false;
    bool                            aiControl = false;
    int                             aiControlPort = 6503;

    /// `--fujinet[=PORT]` / `--fujinet-serial[=DEVICE]` / `--fujinet-slot N`.
    /// Plug a FujiNet relay card and arm its link before the machine boots,
    /// so an autostart scan can find a FujiNet on the first try. `fujiNetSlot`
    /// defaults to 7 — the slot the //e scans before the Disk II in slot 6,
    /// which is what makes a machine with a FujiNet attached boot straight
    /// into its CONFIG.
    ///
    /// The serial form's DEVICE may be omitted, meaning "auto": take the only
    /// candidate if there is exactly one, and otherwise stay idle rather than
    /// guessing — opening the wrong device drives DTR/RTS at whatever else is
    /// plugged in.
    /// `fujiNetSlot` is only a PREFERENCE unless the user typed
    /// `--fujinet-slot`. Slot 7's first-run default card is the Le Chat Mauve,
    /// so a bare `--fujinet` on a stock configuration would otherwise always
    /// be refused for a slot the user never asked for. When the preference is
    /// taken, POM2 falls back to the first free slot (7→1) instead — the
    /// behaviour docs/fujinet_plan.md §"CLI" specifies. An EXPLICIT
    /// `--fujinet-slot N` that is occupied stays a hard error.
    enum class FujiNetTransport { None, Tcp, Serial };
    FujiNetTransport                fujiNet = FujiNetTransport::None;
    int                             fujiNetPort = 1985;
    std::string                     fujiNetSerialPath;
    int                             fujiNetSlot = 7;
    bool                            fujiNetSlotExplicit = false;

    std::optional<int>              executionSpeed;        // cycles/frame
    std::string                     initialTapePath;       // --tape <path>
    bool                            initialTapeAutoPlay = false;
    std::string                     saveTapePath;          // --save-tape <path>
    CliSaveTapeFormat               saveTapeFormat = CliSaveTapeFormat::NoHint;
    CliDisplayMode                  displayMode = CliDisplayMode::NoHint;

    /// `--35-disk1 <path>` / `--35-disk2 <path>` — mount an 800K Sony
    /// 3.5" image into the on-board (drive 1) or external (drive 2)
    /// SmartPort 3.5" slot. Only takes effect on the //c+ profile;
    /// other profiles log a warning and ignore. Empty = no mount.
    std::string                     disk35Internal;
    std::string                     disk35External;

    /// `--rgb-card-invert-bit7` — Le Chat Mauve / Video-7 RGB-card
    /// Dragon-Wars-compat toggle. XORs bit 7 of every Chat Mauve HGR /
    /// DHGR-Mixed source byte at decode time, restoring the intended
    /// rendering for software that encoded the brevet bit-7 selector with
    /// the opposite polarity. Mirrors AppleWin's flag of the same name.
    /// `std::nullopt` = leave the persisted value alone.
    std::optional<bool>             rgbCardInvertBit7;

    // Phase C.
    std::vector<CliAction>          deferredActions;
};

/// Parse argv. Returns:
///   * `std::nullopt` and prints to stderr on parse error (caller exits ≠0).
///   * Populated CliPlan otherwise.
///
/// `helpRequestedOut` is set to true if `--help` / `-h` was seen — the
/// usage was printed and the caller should exit 0 without continuing.
std::optional<CliPlan> parseCli(int argc, char* argv[], bool& helpRequestedOut);

/// Run every Phase-C action in `plan.deferredActions`, in order. Errors
/// are logged; the first fatal error short-circuits the rest.
void runDeferredActions(const std::vector<CliAction>& actions,
                        EmulationController& emu);

/// Build a save-tape path honouring `--save-tape-format` when the path
/// has no recognisable extension. Used by main() before handing the path
/// to the cassette device.
std::string resolveSaveTapePath(const std::string& path, CliSaveTapeFormat hint);

} // namespace pom2

#endif // POM2_CLI_DISPATCHER_H
