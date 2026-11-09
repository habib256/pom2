// POM2 — GPL-3.0-or-later
#include "Disk35Image.h"
#include "MediaAutosave.h"
#include "EmulationController.h"
#include "LironCard.h"
#include "Memory.h"
#include "SlotBus.h"
#include "Settings.h"
#include "SmartPortCard.h"
#include "SmartPort35Unit.h"
#include "StorageCoordinator.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
void write(const fs::path& p, const Bytes& b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), b.size());
    assert(f.good());
}
Bytes read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(f), {});
}
void replacementDuringCommand(const fs::path& dir, bool samePath, bool mountRequest) {
    const std::string prefix = mountRequest ? "mount-" : "eject-";
    const auto oldPath = dir / (prefix + (samePath ? "remount-old.po" : "old.po"));
    const auto newPath = samePath ? oldPath : dir / (prefix + "new.po");
    const auto requestedPath = dir / (prefix + "requested.po");
    const Bytes bytes(pom2::Disk35Image::kBytesPerImage, 0);
    write(oldPath, bytes); write(newPath, bytes);
    if (mountRequest) write(requestedPath, bytes);
    EmulationController controller(AudioDevice::OutputMode::Offline);
    pom2::StorageCoordinator storage;
    pom2::Settings settings; settings.setReadOnly(true);
    {
        auto state = controller.lockState();
        auto card = std::make_unique<pom2::LironCard>(1);
        auto* live = card.get();
        state.memory().slotBus().plug(1, std::move(card));
        std::string error;
        assert(live->mountBay(0, oldPath.string(), error));
        live->setBayWriteBack(0, true);
        Bytes block(512, 0x5a);
        assert(live->drive(0).image()->writeBlock(3, block.data()));
    }
    // Pause the unlocked commit so replacement happens deterministically
    // after capture, rather than relying on the disk write's duration.
    std::unique_lock<std::mutex> gate(pom2::detail::MediaCommitOrder::instance().m);
    pom2::StorageCoordinator::MediaCommandResult result;
    std::thread eject([&] {
        result = mountRequest ? storage.mountMediaBay(controller, settings, 1, 0, requestedPath.string())
                              : storage.ejectMediaBay(controller, settings, 1, 0);
    });
    bool captured = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            auto state = controller.lockState();
            auto* live = dynamic_cast<pom2::LironCard*>(state.memory().slotBus().peripheral(1));
            captured = !live->bayInfo(0).hasUnsavedChanges;
            if (captured) {
                std::string error;
                assert(live->mountBay(0, newPath.string(), error));
                Bytes block(512, 0x6b);
                assert(live->drive(0).image()->writeBlock(7, block.data()));
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    gate.unlock(); eject.join();
    assert(captured);
    auto state = controller.lockState();
    auto* live = dynamic_cast<pom2::LironCard*>(state.memory().slotBus().peripheral(1));
    assert(live->bayInfo(0).loaded && live->bayInfo(0).path == newPath.string());
    assert(!result.ok); // The request was for the outgoing mount only.
    assert(live->bayInfo(0).hasUnsavedChanges);
    if (!samePath) assert(read(newPath) == bytes);
    auto expected = bytes;
    std::fill(expected.begin() + 3 * 512, expected.begin() + 4 * 512, 0x5a);
    assert(read(oldPath) == expected);
    live->drive(0).image()->eject(); // discard the newcomer's deliberate test write
}
void replacementDuringSmartPortCommand(const fs::path& dir, bool samePath, int action) {
    const std::string prefix = action == 2 ? "hdv-route-" : action == 1 ? "routed-" : "type-";
    const auto oldPath = dir / (prefix + (samePath ? "remount-old.po" : "old.po"));
    const auto newPath = samePath ? oldPath : dir / (prefix + "new.po");
    const auto requestedPath = dir / (prefix + "requested.po");
    const Bytes bytes(pom2::Disk35Image::kBytesPerImage, 0);
    write(oldPath, bytes); write(newPath, bytes);
    if (action) write(requestedPath, action == 2 ? Bytes(2 * 1024 * 1024, 0) : bytes);
    EmulationController controller(AudioDevice::OutputMode::Offline);
    pom2::StorageCoordinator storage;
    pom2::Settings settings; settings.setReadOnly(true);
    {
        auto state = controller.lockState();
        auto card = std::make_unique<pom2::SmartPortCard>(1);
        card->setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
        auto* live = card.get();
        state.memory().slotBus().plug(1, std::move(card));
        std::string error;
        assert(live->mountBay(0, oldPath.string(), error));
        live->setBayWriteBack(0, true);
        Bytes block(512, 0x5a);
        assert(dynamic_cast<pom2::SmartPort35Unit*>(live->unit(0))->image().writeBlock(3, block.data()));
    }
    // Pause the unlocked commit so replacement happens deterministically
    // after capture, rather than relying on the disk write's duration.
    std::unique_lock<std::mutex> gate(pom2::detail::MediaCommitOrder::instance().m);
    pom2::StorageCoordinator::MediaCommandResult result;
    std::thread eject([&] {
        if (action == 2) result = storage.mountHdv(controller, settings, requestedPath.string(), true);
        else if (action == 1) result = storage.mountDisk35(controller, settings, 0, requestedPath.string(), false);
        else result = storage.setMediaBayType(controller, settings, 1, 0, "");
    });
    bool captured = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            auto state = controller.lockState();
            auto* live = dynamic_cast<pom2::SmartPortCard*>(state.memory().slotBus().peripheral(1));
            captured = !live->bayInfo(0).hasUnsavedChanges;
            if (captured) {
                std::string error;
                assert(live->mountBay(0, newPath.string(), error));
                Bytes block(512, 0x6b);
                assert(dynamic_cast<pom2::SmartPort35Unit*>(live->unit(0))->image().writeBlock(7, block.data()));
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    gate.unlock(); eject.join();
    assert(captured);
    auto state = controller.lockState();
    auto* live = dynamic_cast<pom2::SmartPortCard*>(state.memory().slotBus().peripheral(1));
    assert(live->bayInfo(0).loaded && live->bayInfo(0).path == newPath.string());
    assert(!result.ok); // The request was for the outgoing mount only.
    assert(live->bayInfo(0).hasUnsavedChanges);
    if (!samePath) assert(read(newPath) == bytes);
    auto expected = bytes;
    std::fill(expected.begin() + 3 * 512, expected.begin() + 4 * 512, 0x5a);
    assert(read(oldPath) == expected);
    dynamic_cast<pom2::SmartPort35Unit*>(live->unit(0))->image().eject(); // discard the newcomer's deliberate test write
}
int main() {
    const auto dir = fs::temp_directory_path() /
        ("pom2-deferred-diversion-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    for (int mode : {0, 1, 2, 3, 4}) {
        const auto path = dir / (std::to_string(mode) + ".po");
        Bytes original(pom2::Disk35Image::kBytesPerImage, 0);
        write(path, original);
        EmulationController controller(AudioDevice::OutputMode::Offline);
        pom2::StorageCoordinator storage;
        std::vector<pom2::StorageCoordinator::DeferredFlush> pending;
        std::string error;
        {
            auto state = controller.lockState();
            auto card = std::make_unique<pom2::LironCard>(1);
            auto* live = card.get();
            state.memory().slotBus().plug(1, std::move(card));
            assert(live->mountBay(0, path.string(), error));
            live->setBayWriteBack(0, true);
            Bytes block(512, 0x5a);
            assert(live->drive(0).image()->writeBlock(3, block.data()));
            if (mode < 3) {
                assert(storage.flushAll(state.memory().slotBus(), error, &pending));
                assert(pending.size() == 1);
            }
            if (mode == 1) {
                // A same-path remount is still a different mount identity.
                assert(live->mountBay(0, path.string(), error));
            } else if (mode == 2) {
                // Completion must preserve writes made after capture.
                block.assign(512, 0x6b);
                assert(live->drive(0).image()->writeBlock(4, block.data()));
            }
        }
        auto external = original;
        external[0] = 0x77;
        write(path, external);
        fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(5));
        std::string notice;
        if (mode >= 3) {
            pom2::Settings settings;
            settings.setReadOnly(true);
            const auto replacement = dir / (std::to_string(mode) + "-replacement.po");
            if (mode == 4) write(replacement, Bytes(original.size(), 0));
            const auto result = storage.mountMediaBay(controller, settings, 1, 0, replacement.string());
            assert(result.ok == (mode == 4));
            notice = result.notice;
        } else {
            assert(storage.commitDeferredFlushes(controller, pending, error));
            assert(error.empty() && pending.empty());
        }
        assert(read(path) == external);
        fs::path sibling;
        for (const auto& entry : fs::directory_iterator(dir))
            if (entry.path().stem().string().find(std::to_string(mode) + ".guest-") == 0)
                sibling = entry.path();
        assert(!sibling.empty());
        if (mode >= 3) assert(notice.find(sibling.string()) != std::string::npos);
        std::fill(original.begin() + 3 * 512, original.begin() + 4 * 512, 0x5a);
        assert(read(sibling) == original);
        {
            auto state = controller.lockState();
            auto* live = dynamic_cast<pom2::LironCard*>(state.memory().slotBus().peripheral(1));
            const auto info = live->bayInfo(0);
            assert(info.persistenceState == (mode == 1 || mode == 4 ? "saved" : "diverted"));
            if (mode != 1 && mode != 4) assert(info.persistenceError.find(sibling.string()) != std::string::npos);
            assert(info.hasUnsavedChanges == (mode == 2));
            if (mode == 2) {
                assert(live->drive(0).image()->saveDirty());
                std::fill(original.begin() + 4 * 512, original.begin() + 5 * 512, 0x6b);
                assert(read(sibling) == original);
                assert(read(path) == external);
                assert(!live->bayInfo(0).hasUnsavedChanges);
            }
            live->drive(0).image()->eject();
        }
    }
    replacementDuringCommand(dir, false, false);
    replacementDuringCommand(dir, true, false);
    replacementDuringCommand(dir, false, true);
    replacementDuringCommand(dir, true, true);
    replacementDuringSmartPortCommand(dir, false, false);
    replacementDuringSmartPortCommand(dir, true, false);
    replacementDuringSmartPortCommand(dir, false, true);
    replacementDuringSmartPortCommand(dir, true, true);
    replacementDuringSmartPortCommand(dir, false, 2);
    replacementDuringSmartPortCommand(dir, true, 2);
    fs::remove_all(dir);
}
