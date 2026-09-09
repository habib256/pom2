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

// A persisted media path that no longer resolves must SAY SO (hunt #15).
//
// `StorageCoordinator::restoreMediaFromSettings` gates every mount on
// `is_regular_file`. When the file is gone the mount simply does not happen —
// and `persistSessionSettings` then writes "" over that key at quit, because
// it persists what is LIVE. So the path is not just unmounted, it is erased,
// and the user is told nothing.
//
// The commonest way to get there is a RELATIVE path: `POM2 disks_5.4/x.dsk`
// launched from the repo root persists "disks_5.4/x.dsk" verbatim (nothing on
// the mount path calls fs::absolute), and the next launch from a desktop
// launcher — a different working directory — resolves nothing.
//
// The SmartPort-unit and generic-bay loops have always pushed
// "persisted path not found: <path>"; the Disk II, HDV and CFFA branches did
// not. Four unresolvable paths produced ONE warning. This pins four.

#include "StorageCoordinator.h"

#include "CffaCard.h"
#include "DiskIICard.h"
#include "ProDOSHardDiskCard.h"
#include "Settings.h"
#include "SlotBus.h"
#include "SmartPortCard.h"
#include "SmartPortUnit.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace fs = std::filesystem;

int main()
{
    // Sandbox HOME/XDG the way storage_coordinator does — Settings resolves
    // (and CREATES) its store directory from them, and a probe must never
    // reach the developer's own state.cfg.
    const fs::path home = fs::temp_directory_path() / "pom2_restore_missing_home";
    fs::remove_all(home);
    fs::create_directories(home / ".config");
    ::setenv("HOME", home.string().c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (home / ".config").string().c_str(), 1);

    SlotBus bus;
    auto diskII = std::make_unique<DiskIICard>(6);
    auto smart  = std::make_unique<pom2::SmartPortCard>(5);
    auto hdv    = std::make_unique<ProDOSHardDiskCard>(4);
    auto cffa   = std::make_unique<pom2::CffaCard>(3);
    DiskIICard*             pDiskII = diskII.get();
    pom2::SmartPortCard*    pSmart  = smart.get();
    ProDOSHardDiskCard*     pHdv    = hdv.get();
    pom2::CffaCard*         pCffa   = cffa.get();
    bus.plug(6, std::move(diskII));
    bus.plug(5, std::move(smart));
    bus.plug(4, std::move(hdv));
    bus.plug(3, std::move(cffa));

    // Four persisted paths, none of which resolves. Relative on purpose: that
    // is the shape a cwd change leaves behind.
    pom2::Settings settings;
    settings.setString("disk_path_slot6",             "disks_5.4/gone.dsk");
    settings.setString("hdv_path",                    "hdv/gone.hdv");
    settings.setString("cffa_slot3_path",             "hdv/gone2.hdv");
    settings.setString("smartport_slot5_unit0_type",  "hdv");
    settings.setString("smartport_slot5_unit0_path",  "hdv/gone3.hdv");

    pom2::StorageCoordinator coordinator;
    const auto restored = coordinator.restoreMediaFromSettings(bus, settings);

    for (const auto& w : restored.warnings)
        std::printf("  warning: %s\n", w.c_str());

    std::size_t notFound = 0;
    for (const auto& w : restored.warnings)
        if (w.find("persisted path not found:") != std::string::npos) ++notFound;

    if (notFound != 4) {
        std::printf("FAIL: 4 unresolvable persisted paths produced %zu "
                    "\"path not found\" warnings (want 4)\n", notFound);
        fs::remove_all(home);
        return 1;
    }
    // …and none of them mounted anything, which is the other half of the
    // contract: the diagnostic replaces silence, it does not replace the gate.
    if (pDiskII->isDiskLoaded(0) || pHdv->isImageLoaded() ||
        pCffa->isImageLoaded() ||
        (pSmart->unit(0) && pSmart->unit(0)->isLoaded())) {
        std::printf("FAIL: a missing path was mounted anyway\n");
        fs::remove_all(home);
        return 1;
    }

    // A path that DOES resolve must still mount, and must warn about nothing.
    const fs::path good = home / "good.dsk";
    {
        std::FILE* f = std::fopen(good.string().c_str(), "wb");
        if (!f) { std::printf("FAIL: cannot create %s\n", good.string().c_str()); return 1; }
        // 35 tracks x 16 sectors x 256 bytes = a minimal .dsk.
        static unsigned char zero[256] = {0};
        for (int i = 0; i < 35 * 16; ++i) std::fwrite(zero, 1, sizeof zero, f);
        std::fclose(f);
    }
    pom2::Settings ok;
    ok.setString("disk_path_slot6", good.string());
    SlotBus bus2;
    auto diskII2 = std::make_unique<DiskIICard>(6);
    DiskIICard* pDiskII2 = diskII2.get();
    bus2.plug(6, std::move(diskII2));
    const auto restored2 = coordinator.restoreMediaFromSettings(bus2, ok);
    if (!pDiskII2->isDiskLoaded(0)) {
        std::printf("FAIL: an existing image no longer mounts\n");
        fs::remove_all(home);
        return 1;
    }
    for (const auto& w : restored2.warnings) {
        if (w.find("persisted path not found:") != std::string::npos) {
            std::printf("FAIL: spurious not-found warning: %s\n", w.c_str());
            fs::remove_all(home);
            return 1;
        }
    }

    std::printf("storage_restore_missing_path: 4/4 warned, good image mounts\n");
    fs::remove_all(home);
    return 0;
}
