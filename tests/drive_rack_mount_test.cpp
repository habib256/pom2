// POM2 — GPL-3.0-or-later
#include "DriveRackMount.h"
#include "DiskIICard.h"
#include "EmulationController.h"
#include "FloppySoundDevice.h"
#include "LironCard.h"
#include "Memory.h"
#include "ProDOSHardDiskCard.h"
#include "Settings.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "StorageCoordinator.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

int main() {
    namespace fs = std::filesystem;
    const auto scratch = fs::temp_directory_path() / ("pom2_rack_mount_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(scratch);
    auto image = [&](const char* name, std::size_t bytes) {
        const auto path = scratch / name;
        std::ofstream file(path, std::ios::binary);
        std::vector<char> contents(bytes, 0);
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        assert(file.good());
        return path.string();
    };
    const auto floppy = image("one.dsk", 143360), other = image("two.dsk", 143360);
    const auto floppy35 = image("disk35.po", 819200), hd = image("small.hdv", 16384);
    {
        EmulationController controller;
        pom2::Settings settings;
        settings.setReadOnly(true);
        pom2::StorageCoordinator storage;
        {
            auto state = controller.lockState();
            auto& bus = state.memory().slotBus();
            bus.plug(4, std::make_unique<DiskIICard>(4));
            bus.plug(6, std::make_unique<DiskIICard>(6));
            bus.plug(7, std::make_unique<ProDOSHardDiskCard>(7));
            auto smart = std::make_unique<pom2::SmartPortCard>(5);
            smart->setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
            smart->setUnit(1, std::make_unique<pom2::SmartPortHdvUnit>());
            bus.plug(5, std::move(smart));
        }
        const auto mode = controller.getMode();
        pom2::DrivePicture target;
        target.slot = 4; target.bay = 1; target.kind = pom2::DrivePictureKind::DiskII;
        std::string error;
        assert(!pom2::mountEmptyDrive(controller, settings, storage, target, hd, error));
        assert(!error.empty());
        assert(pom2::mountEmptyDrive(controller, settings, storage, target, floppy, error));
        // Target the second drive on a non-primary card; no reboot or auto-route.
        assert(controller.getMode() == mode);
        {
            auto state = controller.lockState();
            auto& bus = state.memory().slotBus();
            auto* disk4 = dynamic_cast<DiskIICard*>(bus.peripheral(4));
            auto* disk6 = dynamic_cast<DiskIICard*>(bus.peripheral(6));
            assert(!disk4->isDiskLoaded(0) && disk4->getDiskPath(1) == floppy);
            assert(!disk6->isDiskLoaded(0) && !disk6->isDiskLoaded(1));
        }
        // A stale "empty" snapshot never permits replacing a now-loaded disk.
        assert(!pom2::mountEmptyDrive(controller, settings, storage, target, other, error));
        assert(settings.getString("disk_path_slot4_drive2", "") == floppy);
        target.slot = 5; target.bay = 0; target.kind = pom2::DrivePictureKind::Floppy35;
        assert(!pom2::mountEmptyDrive(controller, settings, storage, target, floppy, error));
        assert(pom2::mountEmptyDrive(controller, settings, storage, target, floppy35, error));
        target.bay = 1; target.kind = pom2::DrivePictureKind::HardDisk;
        assert(pom2::mountEmptyDrive(controller, settings, storage, target, hd, error));
        // Reconfigured mechanism: refuse to target the replacement as an old drive.
        target.slot = 7; target.bay = 0; target.kind = pom2::DrivePictureKind::DiskII;
        assert(!pom2::mountEmptyDrive(controller, settings, storage, target, other, error));
        target.kind = pom2::DrivePictureKind::HardDisk; target.bay = 1;
        assert(!pom2::mountEmptyDrive(controller, settings, storage, target, hd, error)); // image already mounted
        const auto hd2 = image("second.hdv", 16384);
        assert(pom2::mountEmptyDrive(controller, settings, storage, target, hd2, error));
        {
            auto state = controller.lockState();
            auto* card = dynamic_cast<ProDOSHardDiskCard*>(state.memory().slotBus().peripheral(7));
            assert(!card->bayInfo(0).loaded && card->bayInfo(1).path == hd2);
        }
    }
    // An empty Liron unit takes a 3.5" OR an HDV, chosen by the file. Its
    // label ("…or SmartPort HD") used to make the rack call it a hard disk
    // and refuse every 800K image.
    {
        EmulationController controller;
        pom2::Settings settings;
        settings.setReadOnly(true);
        pom2::StorageCoordinator storage;
        {
            auto state = controller.lockState();
            state.memory().slotBus().plug(2, std::make_unique<pom2::LironCard>(2));
            auto* liron = dynamic_cast<pom2::LironCard*>(state.memory().slotBus().peripheral(2));
            assert(liron->bayInfo(0).acceptsSony35OrHdv);
        }
        // The 3.5" click: a Liron bay had no sound path at all (bug hunt
        // 2026-10-16). Counted on the 3.5" source's queue — no audio thread
        // drains it here. Needs the bundled MAME samples to make a sound.
        std::string samples;
        for (const char* d : {"roms/floppy_samples", "../roms/floppy_samples",
                              "../../roms/floppy_samples"})
            if (fs::is_directory(d)) { samples = d; break; }
        const bool clicks = !samples.empty() &&
            controller.floppySound35().loadSamples(samples,
                FloppySoundDevice::FormFactor::FF35);
        const auto queued = [&] { return controller.floppySound35().queuedCommandCount(); };
        pom2::DrivePicture target;
        target.slot = 2; target.bay = 0; target.kind = pom2::DrivePictureKind::Generic;
        std::string error;
        assert(!pom2::mountEmptyDrive(controller, settings, storage, target, floppy, error));
        const int beforeInsert = queued();
        assert(pom2::mountEmptyDrive(controller, settings, storage, target, floppy35, error));
        if (clicks) assert(queued() == beforeInsert + 1 && "a 3.5\" insert in a Liron bay was silent");
        target.bay = 1;
        const auto hd3 = image("liron.hdv", 16384);
        const int beforeHd = queued();
        const bool hdOk = pom2::mountEmptyDrive(controller, settings, storage, target, hd3, error);
        if (!hdOk) std::fprintf(stderr, "liron hdv: %s\n", error.c_str());
        assert(hdOk);
        if (clicks) assert(queued() == beforeHd && "an HDV in a Liron bay clicked like a floppy");
        {
            auto state = controller.lockState();
            auto* liron = dynamic_cast<pom2::LironCard*>(state.memory().slotBus().peripheral(2));
            assert(liron->bayInfo(0).loaded && liron->bayInfo(0).path == floppy35);
            assert(!liron->bayInfo(0).acceptsSony35OrHdv);
            assert(liron->bayInfo(1).loaded && liron->bayInfo(1).path == hd3);
        }
        const int beforeEject = queued();
        assert(storage.ejectMediaBay(controller, settings, 2, 0).ok);
        if (clicks) assert(queued() == beforeEject + 1 && "a 3.5\" eject from a Liron bay was silent");
        if (!clicks) std::puts("note: no floppy samples, 3.5\" click not checked");
    }
    fs::remove_all(scratch);
}
