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

#include "ProfileSwitch.h"

#include "EmulationController.h"
#include "Logger.h"
#include "Memory.h"
#include "ResourcePaths.h"
#include "RewindBuffer.h"
#include "Settings.h"
#include "StorageCoordinator.h"
#include "SystemProfile.h"

#include <mutex>

namespace pom2 {

M6502::CpuMode resolveCpuModeSetting(const Settings& settings,
                                     M6502::CpuMode profileDefault)
{
    const std::string override = settings.getString("cpu_mode_override", "auto");
    // A 65C02 is a strict superset of the NMOS 6502, so forcing CMOS is
    // always physically plausible (it was a real socket-upgrade on II/II+).
    if (override == "65c02") return M6502::CpuMode::CMOS;
    // Forcing NMOS only makes sense on a machine that actually shipped an
    // NMOS 6502. The //c, //c+, enhanced //e and the PAL variants have a
    // 65C02 SOLDERED in, and their ROMs use 65C02-only opcodes (LDA (zp) =
    // $B2) that DECODE AS KIL on an NMOS core: a sticky `nmos` override set
    // once on a II+ froze the //c when the user switched to it.
    if (override == "nmos" && profileDefault == M6502::CpuMode::NMOS)
        return M6502::CpuMode::NMOS;
    return profileDefault;
}

ProfileSwitchResult switchProfile(EmulationController& controller,
                                  StorageCoordinator& storage,
                                  Settings& settings,
                                  SystemProfile p,
                                  bool persist,
                                  const ProfileSwitchHooks& hooks)
{
    ProfileSwitchResult result;
    const auto& cfg = profileConfig(p);
    log().info("Profile", std::string("Switching to ") + std::string(cfg.displayName));

    // Stop the CPU worker FIRST, so the flush below sees a quiescent machine
    // and nothing it writes can be undone by a guest still running.
    const bool wasRunning = controller.getMode() == EmulationController::Mode::Running;
    controller.stop();
    // A card that cannot save its disk refuses the switch: the rebuild below
    // destroys the cards, and with them the only copy of unsaved writes.
    if (hooks.flushMedia && !hooks.flushMedia(result.error)) {
        if (wasRunning) controller.start();
        return result;
    }
    // 0. The window commits the new profile here — step 7 reads it.
    if (hooks.afterFlush) hooks.afterFlush();

    // 1. The rewind ring describes the outgoing machine; restoring one of its
    //    frames onto the new ROM and slot map would be nonsense.
    controller.rewind().clear();

    // 2. What is mounted now comes back after the cards are rebuilt.
    StorageCoordinator::RebuildSnapshot mediaSnapshot;
    {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        mediaSnapshot = storage.captureRebuildSnapshot(controller.memory().slotBus());
    }

    // 3-4. Tear down under the lock, set the paging mode, cold-reset RAM.
    {
        auto st = controller.lockState();
        if (hooks.beginLocked) hooks.beginLocked(st);
        st.memory().setIIEMode(cfg.iieMode);
        // RamWorks III is a //e expansion; the //c family has its own
        // on-board aux RAM and takes exactly one bank.
        if (p == SystemProfile::AppleIIe || p == SystemProfile::AppleIIeUnenhanced ||
            p == SystemProfile::AppleIIePAL || p == SystemProfile::AppleIIeUnenhancedPAL) {
            const int banks = settings.getInt("ramworks_banks", 1);
            st.memory().setRamWorksBanks(static_cast<uint32_t>(banks > 0 ? banks : 1));
        } else if (cfg.iieMode) {
            st.memory().setRamWorksBanks(1);
        }
        st.memory().clearRam();
        st.memory().resetSoftSwitches();
    }

    // 5-7 under ONE lock, so the AI server never sees a half-built machine.
    // This is the documented exception to "no file I/O under stateMutex"
    // (CLAUDE.md): the worker is stopped, and atomicity outranks latency.
    {
        auto st = controller.lockState();

        // 5. Main ROM.
        const bool pickLowerHalf = profileUsesLowerRomHalf(p);
        const std::string newRomPath = findFirstResource(cfg.romProbeOrder);
        if (!newRomPath.empty() &&
            st.memory().loadAppleIIRom(newRomPath.c_str(), pickLowerHalf)) {
            result.romPath   = newRomPath;
            result.romLoaded = true;
            result.romStatus = std::string(cfg.iieMode ? "IIe/IIc: " : "loaded: ") + newRomPath;
            if (newRomPath.find("apple2.rom") != std::string::npos &&
                cfg.romProbeOrder.front() != newRomPath) {
                log().warn("Profile",
                    std::string("Loaded generic fallback ") + newRomPath + " for " +
                    std::string(cfg.displayName) + " — profile-specific ROM (" +
                    cfg.romProbeOrder.front() +
                    ") not found; ROM identity may not match the selected machine");
            }
        } else {
            result.romStatus = std::string("NO ROM (") + cfg.romProbeOrder.front() +
                               " not found) — $D000-$FFFF stub only";
            log().warn("Profile", result.romStatus);
        }

        // 6. Character ROM: the user's pick wins over the profile's order.
        std::string newCharPath = hooks.charRomOverride ? hooks.charRomOverride()
                                                        : std::string();
        if (newCharPath.empty()) newCharPath = findFirstResource(cfg.charRomProbeOrder);
        result.charRomPath = newCharPath;
        if (!newCharPath.empty())
            st.memory().loadCharRom(newCharPath.c_str(), hooks.charRomBank);

        // 6b. CPU mode BEFORE the rebuild: the slot factory picks firmware
        //     builds (the CFFA's) by the CPU it is told about.
        st.cpu().setCpuMode(resolveCpuModeSetting(settings, cfg.defaultCpu));

        // 7. The cards.
        if (hooks.plugSlots) hooks.plugSlots(st, cfg.iieMode);
    }

    // 7a. Host endpoints are opened with the lock released.
    if (hooks.afterPlug) hooks.afterPlug();

    // 8. The live media go back over what the settings restored.
    {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        storage.restoreRebuildSnapshot(controller.memory().slotBus(), mediaSnapshot);
    }

    // 9. For the caller's log.
    {
        auto st = controller.lockState();
        result.cpuIsCmos = st.cpu().getCpuMode() == M6502::CpuMode::CMOS;
    }

    // 10. Pacing and the video standard; the snapshot identity follows the
    //     machine so a snapshot of the old one is refused.
    controller.setCyclesPerFrame(cfg.defaultCyclesPerFrame);
    controller.setVideoStandard(cfg.videoStandard);
    controller.setMachineId(snapshotMachineId(p));

    // 11. The CPU fetches its reset vector from the new ROM.
    controller.hardReset();
    controller.start();

    // 12. Remembered for the next launch.
    if (persist) {
        settings.setString("system_profile", std::string(cfg.key));
        settings.save();
    }

    {
        auto st = controller.lockState();
        if (hooks.publishLocked) hooks.publishLocked(st);
    }
    result.applied = true;
    return result;
}

}  // namespace pom2
