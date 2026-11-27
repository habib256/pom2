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

    // A failed load leaves Memory's outgoing firmware intact. Validate the
    // candidate before afterFlush commits the frontend profile or tears down
    // its cards; otherwise "NO ROM" still executes the old ROM with a new
    // CPU, slot map and snapshot identity. Use the real loader's size/read
    // checks on an isolated memory so this cannot mutate the running machine.
    const bool pickLowerHalf = profileUsesLowerRomHalf(p);
    // The probe order is a list of fallbacks: take the first candidate that
    // LOADS, not the first that exists. A bad `apple2c-32Kv0.rom` used to
    // refuse the //c while `apple2c-16K.rom` sat valid behind it (bug hunt
    // 2026-11-18 round 2). No lock is held here — this is file I/O.
    Memory::AppleIIRomImage romImage;
    std::string romError;      // why the LAST candidate tried was refused
    bool anyCandidate = false;
    const std::string newRomPath = findFirstResourceThat(cfg.romProbeOrder,
        [&](const std::string& path) {
            anyCandidate = true;
            Memory probe;
            probe.setIIEMode(cfg.iieMode);
            std::string error;
            if (Memory::prepareAppleIIRom(path.c_str(), romImage, error) &&
                probe.loadAppleIIRom(romImage, pickLowerHalf))
                return true;
            romError = path + ": " + (error.empty() ? probe.getLastError() : error);
            log().warn("Profile", "Skipping ROM " + romError);
            return false;
        });
    if (newRomPath.empty()) {
        result.error = !anyCandidate
            ? "No ROM found for " + std::string(cfg.displayName)
            : "Cannot load ROM for " + std::string(cfg.displayName) + ": " + romError;
        log().warn("Profile", result.error);
        return result;
    }

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
    //    Under the lock: the AI server is only detached at step 3, so a
    //    /disk or /snapshot/load can clear the same ring meanwhile.
    {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        controller.rewind().clear();
    }

    // 2. What is mounted now comes back after the cards are rebuilt.
    StorageCoordinator::RebuildSnapshot mediaSnapshot;
    {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        mediaSnapshot = storage.captureRebuildSnapshot(controller.memory().slotBus());
    }

    // 3-4. Tear down under the lock, set the paging mode, cold-reset RAM.
    {
        auto st = controller.lockState();
        controller.endInputJournalLocked("the machine was switched to another model");
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
        if (!newRomPath.empty() &&
            st.memory().loadAppleIIRom(romImage, pickLowerHalf)) {
            result.romPath   = newRomPath;
            result.romLoaded = true;
            result.romStatus = std::string(cfg.iieMode ? "IIe/IIc: " : "loaded: ") + newRomPath;
            // The generic ][ dump, and the ENHANCED //e firmware standing in
            // for the Unenhanced one (the only //e dump a browser bundle has):
            // SystemProfile.cpp promises that one is logged, and it was not.
            const bool genericII  = newRomPath.find("apple2.rom") != std::string::npos;
            const bool enhForUnenh = newRomPath.find("apple2e.rom") != std::string::npos &&
                std::string(cfg.romProbeOrder.front()).find("apple2e.rom") == std::string::npos;
            if ((genericII || enhForUnenh) &&
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
    {
        // The //c Plus's own accelerator, or the user's Zip Chip, or none —
        // decided by the ROM just loaded, before the reset that seats it.
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        controller.installSocketAccelerator();
    }
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
