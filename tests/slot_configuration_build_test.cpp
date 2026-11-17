// POM2 — GPL-3.0-or-later
#include "SlotConfigurationCoordinator.h"
#include "EmulationController.h"
#include "DiskIICard.h"
#include "PremiumSoftCard.h"
#include "ProDOSHardDiskCard.h"
#include "Settings.h"
#include "TestTempPath.h"
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace {
using Slots = pom2::SlotConfigurationCoordinator;
class ClockProbe final : public SlotPeripheral {
public:
    std::function<void()> beforeClock;
    double cpu = 0, machine = 0, standard = 0;
    std::string_view name() const override { return "rebuild clock probe"; }
    void setCpuClock(double hz) override { beforeClock(); cpu = hz; }
    void setMachineCpuClock(double hz) override { machine = hz; }
    void setStandardClock(double hz) override { standard = hz; }
};
void emptySettings(pom2::Settings& settings)
{
    settings.setReadOnly(true);
    for (int s = 1; s <= 7; ++s) settings.setString("slot_" + std::to_string(s) + "_card", "");
}
void writeImage(const fs::path& path, size_t size)
{
    const std::string bytes(size, '\0');
    std::ofstream f(path, std::ios::binary);
    f.write(bytes.data(), bytes.size()); assert(f.good());
}
void restoredAfterTopology(bool missing)
{
    const fs::path dir = pom2test::tempPath(missing ? "pom2-build-missing" : "pom2-build-media");
    fs::create_directories(dir);
    const auto hdv = dir / "disk.hdv", d1 = dir / "one.dsk", d2 = dir / "two.dsk";
    if (!missing) writeImage(hdv, 4096);
    writeImage(d1, 143360); writeImage(d2, 143360);
    pom2::Settings settings; emptySettings(settings);
    settings.setString("slot_2_card", "hdv");
    settings.setString("slot_3_card", "videoterm"); // invalid on a //e: not dispatched
    settings.setString("slot_4_card", "diskii");
    settings.setString("slot_6_card", "diskii");
    settings.setString("slot_7_card", "clock");
    settings.setString("hdv_path", hdv.string());
    settings.setString("disk_path", d1.string());
    settings.setString("disk_path_slot4_drive2", d2.string());
    EmulationController controller(AudioDevice::OutputMode::Offline);
    auto state = controller.lockState();
    state.memory().setIIEMode(true);
    auto& bus = state.memory().slotBus();
    Slots slots; slots.resolve(settings, pom2::SystemProfile::AppleIIePAL);
    std::vector<int> dispatched;
    auto checkMedia = [&] {
        auto* primary = dynamic_cast<ProDOSHardDiskCard*>(bus.peripheral(1));
        auto* secondary = dynamic_cast<ProDOSHardDiskCard*>(bus.peripheral(2));
        auto* disk4 = dynamic_cast<DiskIICard*>(bus.peripheral(4));
        auto* disk6 = dynamic_cast<DiskIICard*>(bus.peripheral(6));
        assert(primary && secondary && disk4 && disk6);
        assert(primary->isImageLoaded() == !missing);
        assert(!secondary->isImageLoaded());
        assert(disk4->isDiskLoaded(0) && disk4->isDiskLoaded(1));
        assert(!disk6->isDiskLoaded(0) && !disk6->isDiskLoaded(1));
        if (!missing) assert(primary->getImagePath() == hdv.string());
        assert(disk4->getDiskPath(0) == d1.string());
        assert(disk4->getDiskPath(1) == d2.string());
    };
    auto makeProbe = [&] {
        auto p = std::make_unique<ClockProbe>(); p->beforeClock = checkMedia; return p;
    };
    const auto result = slots.buildResolvedPlan(state, settings, pom2::SystemProfile::AppleIIePAL,
        [&] { checkMedia(); return Slots::BuildClocks{4062500, 2031250, 1015625}; }, [&](int slot, const std::string& key) {
            dispatched.push_back(slot);
            if (key == "hdv") bus.plug(slot, std::make_unique<ProDOSHardDiskCard>(slot));
            else if (key == "diskii") {
                bus.plug(slot, std::make_unique<DiskIICard>(slot));
            } else { assert(key == "clock"); bus.plug(slot, makeProbe()); }
        }, [&] {
            assert((dispatched == std::vector<int>{2, 4, 6, 7}));
            assert(!dynamic_cast<ProDOSHardDiskCard*>(bus.peripheral(2))->isImageLoaded());
            assert(!dynamic_cast<DiskIICard*>(bus.peripheral(4))->isDiskLoaded(0));
            // A runtime overlay changes which HDV is primary. Restoration
            // must wait until this final piece of topology exists.
            bus.plug(1, std::make_unique<ProDOSHardDiskCard>(1));
            bus.plug(0, makeProbe());
        });
    checkMedia();
    for (int slot : {0, 7}) {
        auto* probe = dynamic_cast<ClockProbe*>(bus.peripheral(slot));
        assert(probe && probe->cpu == 4062500 && probe->machine == 2031250 && probe->standard == 1015625);
    }
    assert(result.ok() == !missing);
    assert(result.warnings.size() == (missing ? 1u : 0u));
    assert(result.unresolvedPathKeys == (missing ? std::vector<std::string>{"hdv_path"} : std::vector<std::string>{}));
    // Dropping a rejected plan entry does not erase the saved choice.
    assert(settings.getString("slot_3_card") == "videoterm" && slots.dedupCleared(3) == "videoterm");
    bus.clear();
    fs::remove_all(dir);
}
void auxiliaryProfilePolicy()
{
    for (auto profile : {pom2::SystemProfile::AppleIIPlus, pom2::SystemProfile::AppleIIe,
                          pom2::SystemProfile::AppleIIePAL, pom2::SystemProfile::AppleIIc,
                          pom2::SystemProfile::AppleIIcPlus}) {
        for (bool enabled : {false, true}) {
            pom2::Settings settings; emptySettings(settings);
            settings.setBool("premium_softcard", enabled);
            EmulationController controller(AudioDevice::OutputMode::Offline);
            auto state = controller.lockState();
            const auto& cfg = pom2::profileConfig(profile);
            state.memory().setIIEMode(cfg.iieMode);
            Slots slots; slots.resolve(settings, profile);
            int afterCalls = 0;
            const bool expected = enabled && cfg.iieMode && !cfg.noPhysicalSlots;
            auto checkAux = [&] {
                const auto* card = dynamic_cast<PremiumSoftCard*>(state.memory().slotBus().peripheral(0));
                assert((card != nullptr) == expected);
            };
            auto restored = slots.buildResolvedPlan(state, settings, profile,
                [] { return Slots::BuildClocks{4000000, 2000000, 1000000}; },
                [](int, const std::string&) {}, [&] { ++afterCalls; checkAux(); });
            assert(restored.ok() && afterCalls == 1);
            checkAux();
            if (expected) {
                const auto* card = dynamic_cast<PremiumSoftCard*>(state.memory().slotBus().peripheral(0));
                assert(card->cpuClockHz() == 2000000); // machine clock overrides paced clock
            }
        }
    }
}
}
int main() { restoredAfterTopology(false); restoredAfterTopology(true); auxiliaryProfilePolicy(); }
