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

// SystemProfile — canonical Apple II model selection. Each profile
// resolves to:
//   * a CPU type (NMOS 6502 or CMOS 65C02)
//   * a ROM probe order (first existing file wins)
//   * a charset ROM probe order
//   * a Memory IIe paging flag (off for II/II+, on for IIe/IIc)
//   * a default cycles-per-frame (1.023 MHz for II/II+/IIe/IIc; the
//     IIc Plus defaults to 4× = ~4 MHz to match real silicon)
//
// Profile switching happens via `MainWindow::applyProfile()` which does
// a full cold-reset: stops the CPU worker, wipes RAM + soft switches,
// re-loads the new ROM, re-plugs the default slot cards, and restarts
// from the new reset vector. Disk and HDV image MOUNT PATHS are
// preserved across the switch so the user can test the same software
// stack under different machines without re-mounting.

#ifndef POM2_SYSTEM_PROFILE_H
#define POM2_SYSTEM_PROFILE_H

#include "M6502.h"
#include "CpuClock.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pom2 {

/// A slot occupied by construction on this machine (e.g. on-board Disk II
/// at slot 6 on a //c, or the IWM SmartPort at slot 5 on a //c+). When a
/// profile's `builtInSlots[N]` carries a `BuiltInSlot`, the slot card is
/// force-plugged with `cardKey` regardless of user settings, and the
/// Slot Configuration UI renders the row read-only with `label` as a
/// badge so the user knows what's there but can't edit it.
struct BuiltInSlot {
    std::string cardKey;   // matches a kCardTypes[] key in MainWindow_Slots
    std::string label;     // user-visible suffix in the panel
};

enum class SystemProfile {
    AppleII,             // Apple II original (1977), Integer BASIC, 6502 NMOS
    AppleIIPlus,         // Apple II+ (1979), Applesoft + Autostart, 6502 NMOS
    AppleIIeUnenhanced,  // Apple //e (1983), 6502 NMOS, no-mousetext char ROM
    AppleIIe,            // Apple //e Enhanced (1985), 65C02, mousetext char ROM
    AppleIIc,            // Apple //c (1984), 65C02, IIe-class soft switches
    AppleIIcPlus,        // Apple //c Plus (1988), 65C02 @ 4 MHz, built-in SmartPort
    // European PAL variants (50 Hz, 312 lines): same ROMs/CPU as their NTSC
    // siblings, but PAL machine timing. The //c PAL is the machine that took
    // the Le Chat Mauve RGB Péritel adapter on its DB-15 port; //e PAL is the
    // French Touch / DIX target. See CpuClock.h VideoStandard.
    AppleIIePAL,         // Apple //e Enhanced PAL (65C02, 312 lines, 50 Hz)
    AppleIIcPAL,         // Apple //c PAL (Le Chat Mauve RGB adapter machine)
    // Appended after the first release of the enum (palette command ids use
    // the numeric value within a session, so existing values stay put; the
    // DISPLAY order lives in allProfiles(), which slots this one between
    // the //c+ and the Enhanced PAL).
    AppleIIeUnenhancedPAL, // Apple //e Unenhanced PAL — NMOS 6502 + 50 Hz.
                           // The French Touch machine: the 6502-only demo
                           // corpus (OLDSKOOL FORT ET VERT…) counts cycles
                           // that differ on a 65C02.
};

struct ProfileConfig {
    SystemProfile          profile;
    std::string_view       key;             // canonical persistence key
    std::string_view       displayName;
    std::vector<std::string> romProbeOrder;   // 16/32 KB main ROM
    std::vector<std::string> charRomProbeOrder;
    bool                   iieMode;          // Memory::setIIEMode(...)
    M6502::CpuMode         defaultCpu;
    int                    defaultCyclesPerFrame;
    // Indexed by slot $C1-$C7 (entries 1..7 used; entry 0 reserved for the
    // language card sl0 if ever modelled as a SlotPeripheral). A populated
    // entry means "the on-board hardware lives here and the user cannot
    // swap it". An empty entry (nullopt) means the slot is free.
    std::array<std::optional<BuiltInSlot>, 8> builtInSlots;
    // True for machines with NO physical expansion bus — //c and //c+.
    // On these, even the "empty" `builtInSlots` entries (sl3 / sl7) are
    // not user-pluggable: the slot connector simply doesn't exist on
    // real hardware, so POM2 force-empties any non-builtIn slot at
    // profile-apply time and locks the Slot Config picker greyed out.
    bool                   noPhysicalSlots = false;
    // Machine video standard. NTSC (262 lines, 60 Hz) for US machines; PAL
    // (312 lines, ~50 Hz) for the European //e PAL / //c PAL. Appended last
    // and defaulted so existing positional initializers stay valid.
    VideoStandard          videoStandard = VideoStandard::NTSC;
};

/// Resolve a profile enum to its full configuration. The probe orders
/// are ordered by preference; the caller's job is to pick the first
/// existing file at runtime. Every profile is always defined —
/// missing ROM files don't disable the profile, they just degrade the
/// machine to "no ROM" status (the user sees `NO ROM` in the title
/// bar and the CPU starts running garbage at the reset vector).
const ProfileConfig& profileConfig(SystemProfile p);

/// Inverse — parse a persistence/CLI key to a profile enum. Returns
/// `AppleIIPlus` (the historical POM2 default) when the key is empty
/// or unrecognised. Accepts the canonical keys (`ii`, `ii+`, `iie`,
/// `iic`) and a few common aliases (`apple2`, `apple2plus`, `//e`, `//c`).
SystemProfile profileFromKey(std::string_view key);

/// Forward — get the persistence key for a profile.
std::string_view profileKey(SystemProfile p);

/// Which half of a 32 KB main-ROM dump is the firmware POM2 must map at
/// reset — `Memory::loadAppleIIRom(path, pickLower16KFor32K)`. A //c-class
/// dump is TWO firmware banks with bank 0 (the cold-reset entry) in the
/// LOWER half; a //e "system + video" dump puts its 16 KB firmware in the
/// UPPER half. Same file size, opposite slicing, and nothing in the bytes
/// says which — so the answer is a property of the PROFILE and belongs here
/// rather than being re-spelled at each `loadAppleIIRom` call site (bug hunt
/// #10: Reload ROM used the //e slicing on a //c and rebooted into bank 1).
bool profileUsesLowerRomHalf(SystemProfile p);

/// Should `slot_N_card` be persisted for `slot` holding `cardKey` under
/// this profile? False for profile-forced slots: built-in cards, and —
/// except for the user-pluggable "chatmauve" rear-connector adapter — the
/// force-emptied virtual connectors of a `noPhysicalSlots` machine. On
/// those slots the live mapping carries the PROFILE's value, and writing
/// it back would clobber the user's real saved choice (e.g. quitting on
/// //c used to overwrite slot_4_card=mockingboard with the on-board
/// "mouseaw"). `savedKey` is the slot's currently-persisted value: an
/// empty `cardKey` over a saved "chatmauve" IS a user choice (adapter
/// removal must stick), an empty over anything else is the force-empty.
/// Shared by the ~MainWindow persist loop and the Slot Config panel's
/// Apply button so both sites apply the identical guard.
bool slotKeyIsUserChoice(const ProfileConfig& cfg, int slot,
                         std::string_view cardKey, std::string_view savedKey);

/// All profiles in DISPLAY order (not enum order): the NTSC machines
/// chronologically, then the three European PAL variants — Unenhanced //e,
/// Enhanced //e, //c Le Chat Mauve. Used by the Presets menu / palette /
/// toolbar / ROM Status loops, so this array is the one place that decides
/// how the machine list reads.
const std::array<SystemProfile, 9>& allProfiles();

/// Stable machine identity stamped into a snapshot header's `flags` word,
/// so a snapshot taken on one machine cannot be silently restored onto
/// another. Derived from the profile's canonical PERSISTENCE KEY rather than
/// from the enum's numeric value: the key is already contractually stable
/// (it is written to state.cfg and accepted on the command line), whereas
/// the enum is appended to and its order is deliberately not the display
/// order. Never returns 0 — 0 is reserved on the wire to mean "written
/// before this field existed", which must keep loading.
std::uint32_t snapshotMachineId(SystemProfile p);

/// Reverse lookup for the refusal message. Returns an empty view when the
/// id belongs to no profile this build knows (a snapshot from a newer POM2),
/// which the caller should report as "another machine" rather than guess.
std::string_view profileNameForMachineId(std::uint32_t id);

}  // namespace pom2

#endif // POM2_SYSTEM_PROFILE_H
