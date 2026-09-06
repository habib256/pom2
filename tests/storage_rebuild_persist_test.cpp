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

// StorageCoordinator's settings round-trip across a SLOT REBUILD.
//
// `MainWindow_Slots.cpp` Apply (and every profile switch) does exactly this:
// capture the live media, write it into Settings, tear the SlotBus down,
// rebuild it and restore from those same keys. Settings are the transport, so
// a key the capture forgets to write is a key the restore reads STALE — and
// the user gets back a disk they ejected, or loses a write-back opt-in that
// decides whether their session reaches the file at all.
//
// StorageCoordinator was 1640 lines with no test of any kind when this was
// written; these are its first. The HDV assertions below both failed on the
// code as found.

#include "Block512Backing.h"
#include "EmulationController.h"
#include "Memory.h"
#include "ProDOSHardDiskCard.h"
#include "Settings.h"
#include "SlotBus.h"
#include "StorageCoordinator.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool cond, const char* what)
{
    if (cond) std::printf("[ OK ] %s\n", what);
    else    { std::printf("FAIL: %s\n", what); ++failures; }
}

void checkEq(const std::string& got, const std::string& want, const char* what)
{
    if (got == want) std::printf("[ OK ] %s\n", what);
    else {
        std::printf("FAIL: %s — got \"%s\", want \"%s\"\n",
                    what, got.c_str(), want.c_str());
        ++failures;
    }
}

// A minimal but genuinely mountable 2-block ProDOS image.
std::string writeImage(const char* name)
{
    const auto path = std::filesystem::temp_directory_path() / name;
    std::vector<char> bytes(2 * 512, 0);
    std::ofstream f(path, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path.string();
}

}  // namespace

int main()
{
    const std::string image = writeImage("pom2_storage_rebuild.hdv");

    // ── An ejected HDV must not come back after a rebuild ────────────────
    {
        SlotBus bus;
        auto card = std::make_unique<ProDOSHardDiskCard>();
        ProDOSHardDiskCard* hdv = card.get();
        bus.plug(7, std::move(card));
        check(hdv->loadImage(image), "HDV image mounts");

        pom2::StorageCoordinator coord;
        pom2::Settings settings;

        // 1. Mounted: Apply persists the path.
        coord.persistRebuildSettings(settings,
                                     coord.captureRebuildSnapshot(bus));
        checkEq(settings.getString("hdv_path", ""), image,
                "a mounted HDV persists its path across a rebuild");

        // 2. The user ejects, then hits Apply. The path must go with it —
        //    the restore mounts whatever hdv_path still says, so a stale one
        //    silently remounts the image the user just removed.
        hdv->ejectImage();
        coord.persistRebuildSettings(settings,
                                     coord.captureRebuildSnapshot(bus));
        checkEq(settings.getString("hdv_path", ""), std::string(),
                "an EJECTED HDV clears its path across a rebuild");
    }

    // ── The write-back opt-in must survive a rebuild ─────────────────────
    // Disk II and CFFA both persist theirs here; the HDV did not, so a
    // rebuild handed the restore a stale key. With write-back off a block
    // device still reports itself writable to ProDOS (deliberately — see
    // ProDOSHardDiskCard::writeDataByte), so the guest's save appears to
    // succeed and is dropped at quit. Losing this flag loses data silently.
    {
        SlotBus bus;
        auto card = std::make_unique<ProDOSHardDiskCard>();
        ProDOSHardDiskCard* hdv = card.get();
        bus.plug(7, std::move(card));
        hdv->loadImage(image);
        hdv->setWriteBackEnabled(true);

        pom2::StorageCoordinator coord;
        pom2::Settings settings;
        settings.setBool("hdv_writeback", false);   // the stale value

        coord.persistRebuildSettings(settings,
                                     coord.captureRebuildSnapshot(bus));
        check(settings.getBool("hdv_writeback", false),
              "HDV write-back survives a rebuild");
    }

    // ── A host-folder volume must not overwrite the user's hdv_path ──────
    // The Slot Config media column wrote the bay keys itself, with none of
    // the coordinator's guards: ticking "Write-back" (or picking a type) on
    // a synthesised `[host folder] …` volume replaced `hdv_path` with the
    // sentinel, and the next launch tried to mount a path that is not a file
    // — the user's real HDV silently gone from the config. The guarded
    // setters refuse to touch the key for exactly this case.
    {
        EmulationController controller;
        pom2::StorageCoordinator coord;
        pom2::Settings settings;
        settings.setReadOnly(true);
        settings.setString("hdv_path", image);
        settings.setBool("hdv_writeback", false);
        {
            auto state = controller.lockState();
            state.memory().slotBus().plug(
                7, std::make_unique<ProDOSHardDiskCard>(7));
        }
        std::vector<std::uint8_t> bytes(2 * 512, 0);
        check(coord.mountBlockBytes(controller, settings, 7, std::move(bytes),
                                    "[host folder] /tmp/pom2_host",
                                    "/tmp/pom2_host").ok,
              "a synthesised host-folder volume mounts");
        checkEq(settings.getString("hdv_path", ""), image,
                "mounting a host folder leaves hdv_path alone");
        check(coord.setMediaBayWriteBack(controller, settings, 7, 0, true).ok,
              "write-back toggles on the host-folder volume");
        checkEq(settings.getString("hdv_path", ""), image,
                "toggling write-back on a host folder leaves hdv_path alone");
        check(!settings.getBool("hdv_writeback", false),
              "…and leaves hdv_writeback alone too");
    }

    std::error_code ec;
    std::filesystem::remove(image, ec);

    std::printf(failures ? "\nFAILED (%d)\n" : "\nAll storage rebuild "
                "persistence checks passed\n", failures);
    return failures ? 1 : 0;
}
