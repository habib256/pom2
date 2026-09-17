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

// ProfileSwitch — the machine half of a profile switch (TODO G5-6).
//
// `MainWindow::applyProfile` is where nine profiles, a CLI flag, a menu and
// the AI server all land, and it holds `stateMutex` across ROM loads on
// purpose (CLAUDE.md). Its steps used to live inline in the window class, so
// no test could reach the ones that lose data: the media snapshot around the
// slot rebuild, the rewind clear, the video standard, the persisted key.
// This is those steps with no ImGui and no GLFW; what only the window knows
// — its card composer, the display, the title bar, the rebuild coordinator —
// arrives through `ProfileSwitchHooks`, each called at the point the old
// inline code did it. The order is the contract; see switchProfile().

#ifndef POM2_PROFILE_SWITCH_H
#define POM2_PROFILE_SWITCH_H

#include "M6502.h"
#include "SystemProfile.h"

#include <functional>
#include <string>

class EmulationController;

namespace pom2 {

class Settings;
class StateAccess;
class StorageCoordinator;

/// `cpu_mode_override` applied to a profile's soldered CPU: "65c02" always
/// wins (a real socket upgrade), "nmos" only where the machine shipped an
/// NMOS part — the //c family and the enhanced //e run 65C02-only opcodes
/// that decode as KIL on an NMOS core.
M6502::CpuMode resolveCpuModeSetting(const Settings& settings,
                                     M6502::CpuMode profileDefault);

struct ProfileSwitchHooks {
    /// Commit the outgoing media. False (with a reason) refuses the switch:
    /// nothing has changed yet, and a machine that was running runs again.
    std::function<bool(std::string& error)> flushMedia;
    /// After the flush, before anything is torn down — the window stops its
    /// host workers and commits the new profile, which step `plugSlots`
    /// reads.
    std::function<void()> afterFlush;
    /// First thing under the lock that rebuilds the machine.
    std::function<void(StateAccess&)> beginLocked;
    /// A character ROM chosen by the user, or empty for the profile's own.
    std::function<std::string()> charRomOverride;
    int charRomBank = 0;
    /// Under the lock, after the ROMs: the window points its display at the
    /// aux memory (or not) and plugs the slot cards.
    std::function<void(StateAccess&, bool iieMode)> plugSlots;
    /// With the lock released, before the media come back: deferred host
    /// endpoints (FujiNet links, SSC listeners), display follow-ups.
    std::function<void()> afterPlug;
    /// Last, under the lock: the rebuild is published.
    std::function<void(StateAccess&)> publishLocked;
};

struct ProfileSwitchResult {
    bool        applied = false;
    std::string error;          ///< why a refused switch was refused
    std::string romPath;        ///< the main ROM that loaded, empty if none
    bool        romLoaded = false;
    std::string romStatus;      ///< the line the ROM panel shows
    std::string charRomPath;
    bool        cpuIsCmos = false;
};

/// Switch the machine to `profile`. Steps, in this order (the numbers are
/// the ones the window's comments have always used):
///   flush (refusal ends here) · 0 afterFlush · rewind cleared · 2 media
///   snapshot · 3-4 beginLocked, //e paging mode, RamWorks banks, cold RAM ·
///   5 main ROM · 6 character ROM · 6b CPU mode · 7 plugSlots · 7a afterPlug ·
///   8 media restored · 10 CPU pacing, video standard, snapshot machine id ·
///   11 hard reset + start · 12 `system_profile` persisted (unless
///   `persist` is false) · publishLocked.
/// The controller is left running afterwards, whatever it was before.
ProfileSwitchResult switchProfile(EmulationController& controller,
                                  StorageCoordinator& storage,
                                  Settings& settings,
                                  SystemProfile profile,
                                  bool persist,
                                  const ProfileSwitchHooks& hooks);

}  // namespace pom2

#endif  // POM2_PROFILE_SWITCH_H
