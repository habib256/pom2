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

// A profile switch, step by step (TODO G5-6).
//
// `system_profile_smoke` replayed five of the fourteen steps by hand because
// it could not reach MainWindow::applyProfile. The machine half now lives in
// pom2::switchProfile, driven here with the window's hooks replaced by
// recorders and a composer that plugs a Disk II. Per switch — //e → //c,
// //c → //c PAL (NTSC → PAL), //c PAL → //e — it checks the steps that can
// lose something: the hook ORDER, the media snapshot around the rebuild, the
// rewind clear, the ROM, paging mode and CPU, the video standard and pacing,
// the snapshot machine id, the persisted key, a running machine afterwards.
// Then the CPU override policy, and a refused switch (a flush that fails)
// that must leave everything as it was.

#include "DiskIICard.h"
#include "EmulationController.h"
#include "Memory.h"
#include "ProfileSwitch.h"
#include "ResourcePaths.h"
#include "RewindBuffer.h"
#include "Settings.h"
#include "SlotBus.h"
#include "StorageCoordinator.h"
#include "SystemProfile.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;
void expect(bool ok, const std::string& what)
{
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); ++failures; }
    else       std::printf("  ok: %s\n", what.c_str());
}

struct Recorder {
    std::vector<std::string> calls;
    bool refuseFlush = false;
    bool plugSawIIe  = false;

    pom2::ProfileSwitchHooks hooks()
    {
        pom2::ProfileSwitchHooks h;
        h.flushMedia = [this](std::string& err) {
            calls.push_back("flush");
            if (refuseFlush) { err = "disk full (test)"; return false; }
            return true;
        };
        h.afterFlush  = [this] { calls.push_back("afterFlush"); };
        h.beginLocked = [this](pom2::StateAccess& st) {
            calls.push_back("beginLocked");
            // What the window's rebuild coordinator does: empty the bus.
            for (int s = 1; s < SlotBus::kSlotCount; ++s)
                (void)st.memory().slotBus().unplug(s);
        };
        h.plugSlots = [this](pom2::StateAccess& st, bool iieMode) {
            calls.push_back("plugSlots");
            plugSawIIe = iieMode;
            st.memory().slotBus().plug(6, std::make_unique<DiskIICard>(6));
        };
        h.afterPlug     = [this] { calls.push_back("afterPlug"); };
        h.publishLocked = [this](pom2::StateAccess&) { calls.push_back("publish"); };
        return h;
    }
};

DiskIICard* diskAt6(EmulationController& c)
{
    auto st = c.lockState();
    return dynamic_cast<DiskIICard*>(st.memory().slotBus().peripheral(6));
}

std::string joined(const std::vector<std::string>& v)
{
    std::string out;
    for (const auto& s : v) out += (out.empty() ? "" : ",") + s;
    return out;
}

void checkSwitch(EmulationController& c, pom2::StorageCoordinator& storage,
                 pom2::Settings& settings, pom2::SystemProfile to,
                 const std::string& mounted)
{
    const auto& cfg = pom2::profileConfig(to);
    std::printf("-> %s\n", std::string(cfg.displayName).c_str());

    // Something in the rewind ring, so the clear is observable.
    c.rewind().setEnabled(true);
    {
        auto st = c.lockState();
        c.rewind().capture(st.cpu(), st.memory());
    }
    expect(!c.rewind().empty(), "the rewind ring holds a frame before the switch");
    // Paused recording keeps its frames but takes no new ones. Left on, the
    // worker the switch restarts could capture a fresh frame before the
    // check below — it did, under a loaded full ctest run.
    c.rewind().setEnabled(false);

    Recorder rec;
    const auto r = pom2::switchProfile(c, storage, settings, to, true, rec.hooks());
    expect(r.applied, "the switch is applied");
    expect(joined(rec.calls) ==
           "flush,afterFlush,beginLocked,plugSlots,afterPlug,publish",
           "hooks run in order (" + joined(rec.calls) + ")");
    expect(c.rewind().empty(), "the rewind ring is cleared");
    expect(r.romLoaded && r.romPath == pom2::findFirstResource(cfg.romProbeOrder),
           "the profile's own main ROM is loaded (" + r.romPath + ")");
    expect(!r.charRomPath.empty(), "a character ROM is loaded");
    {
        auto st = c.lockState();
        expect(st.memory().isIIE() == cfg.iieMode, "the paging mode follows the profile");
        const auto want = pom2::resolveCpuModeSetting(settings, cfg.defaultCpu);
        expect(st.cpu().getCpuMode() == want, "the CPU is the resolved one");
        expect((want == M6502::CpuMode::CMOS) == r.cpuIsCmos, "…and reported so");
    }
    expect(rec.plugSawIIe == cfg.iieMode, "the composer is told the paging mode");
    expect(c.getVideoStandard() == cfg.videoStandard, "the video standard follows");
    expect(c.getBaseCyclesPerFrame() == cfg.defaultCyclesPerFrame, "the pacing follows");
    expect(c.machineId() == pom2::snapshotMachineId(to), "the snapshot identity follows");
    expect(settings.getString("system_profile") == std::string(cfg.key),
           "system_profile is persisted");
    expect(c.getMode() == EmulationController::Mode::Running, "the machine runs afterwards");
    auto* d = diskAt6(c);
    expect(d && d->isDiskLoaded(0) && d->getDiskPath(0) == mounted,
           "the mounted disk survives the slot rebuild");
}

}  // namespace

int main()
{
    namespace fs = std::filesystem;
    {
        const fs::path home = fs::temp_directory_path() / "pom2_profile_switch_home";
        std::error_code rec;
        fs::remove_all(home, rec);
        fs::create_directories(home, rec);
        setenv("HOME", home.string().c_str(), 1);
        setenv("XDG_DATA_HOME", home.string().c_str(), 1);
        setenv("XDG_CONFIG_HOME", home.string().c_str(), 1);
    }
    const std::string master = pom2::findResource("disks_5.4/dsk/dos33_master.dsk");
    const fs::path disk = fs::temp_directory_path() / "pom2_profile_switch.dsk";
    std::error_code ec;
    fs::copy_file(master, disk, fs::copy_options::overwrite_existing, ec);
    if (master.empty() || ec) {
        std::printf("FAIL: cannot stage the DOS 3.3 master (%s)\n", master.c_str());
        return 1;
    }

    EmulationController controller;
    pom2::StorageCoordinator storage;
    pom2::Settings settings;
    settings.setReadOnly(true);        // setters work, save() writes nothing

    // Start on a //e with a disk in slot 6.
    {
        Recorder rec;
        const auto r = pom2::switchProfile(controller, storage, settings,
                                           pom2::SystemProfile::AppleIIe, true, rec.hooks());
        expect(r.applied, "bootstrap: //e");
    }
    {
        auto* d = diskAt6(controller);
        auto st = controller.lockState();
        expect(d && d->insertDisk(0, disk.string()), "bootstrap: a disk in slot 6");
    }

    checkSwitch(controller, storage, settings, pom2::SystemProfile::AppleIIc, disk.string());
    checkSwitch(controller, storage, settings, pom2::SystemProfile::AppleIIcPAL, disk.string());
    expect(controller.getVideoStandard() == VideoStandard::PAL, "NTSC → PAL really is PAL");
    settings.setInt("ramworks_banks", 4);
    checkSwitch(controller, storage, settings, pom2::SystemProfile::AppleIIe, disk.string());
    {
        auto st = controller.lockState();
        expect(st.memory().ramWorksBanks() == 4, "a //e takes the saved RamWorks size");
    }

    // The CPU override: honoured where the machine shipped NMOS, not on a
    // soldered 65C02.
    settings.setString("cpu_mode_override", "nmos");
    expect(pom2::resolveCpuModeSetting(settings, M6502::CpuMode::CMOS) == M6502::CpuMode::CMOS,
           "an nmos override is refused on a 65C02 machine");
    expect(pom2::resolveCpuModeSetting(settings, M6502::CpuMode::NMOS) == M6502::CpuMode::NMOS,
           "…and honoured on an NMOS one");
    checkSwitch(controller, storage, settings, pom2::SystemProfile::AppleIIeUnenhanced, disk.string());
    settings.setString("cpu_mode_override", "65c02");
    expect(pom2::resolveCpuModeSetting(settings, M6502::CpuMode::NMOS) == M6502::CpuMode::CMOS,
           "a 65c02 override is honoured anywhere");
    settings.setString("cpu_mode_override", "auto");

    // A refused switch changes nothing.
    {
        std::printf("-> refused\n");
        const auto before = settings.getString("system_profile");
        const auto standard = controller.getVideoStandard();
        const auto id = controller.machineId();
        Recorder rec;
        rec.refuseFlush = true;
        const auto r = pom2::switchProfile(controller, storage, settings,
                                           pom2::SystemProfile::AppleIIcPAL, true, rec.hooks());
        expect(!r.applied && r.error == "disk full (test)", "a failed flush refuses the switch");
        expect(joined(rec.calls) == "flush", "…before any other step (" + joined(rec.calls) + ")");
        expect(settings.getString("system_profile") == before, "…the persisted profile is unchanged");
        expect(controller.getVideoStandard() == standard && controller.machineId() == id,
               "…the machine is unchanged");
        expect(controller.getMode() == EmulationController::Mode::Running,
               "…and a machine that was running runs again");
        auto* d = diskAt6(controller);
        expect(d && d->isDiskLoaded(0), "…with its disk still in");
    }

    // A switch that must not persist (a kiosk session).
    {
        Recorder rec;
        const auto before = settings.getString("system_profile");
        (void)pom2::switchProfile(controller, storage, settings,
                                  pom2::SystemProfile::AppleIIPlus, false, rec.hooks());
        expect(settings.getString("system_profile") == before,
               "persist=false leaves system_profile alone");
        auto st = controller.lockState();
        // setIIEMode(false) drops the backing; pinned so a //e's saved size
        // never rides along into a ][+ session's rewind frames.
        expect(st.memory().ramWorksBanks() == 1,
               "a ][+ does not keep the //e's RamWorks banks");
    }

    controller.stop();
    {
        auto* d = diskAt6(controller);
        auto st = controller.lockState();
        if (d) d->ejectDisk(0);
    }
    fs::remove(disk, ec);
    if (failures) { std::printf("profile_switch: %d failure(s)\n", failures); return 1; }
    std::printf("profile_switch OK\n");
    return 0;
}
