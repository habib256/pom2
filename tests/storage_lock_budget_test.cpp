// The lock rule, measured — bug hunt #11.
//
// CLAUDE.md: never hold stateMutex across file I/O. The two-phase mount moved
// the INCOMING read off the lock; the write-back of the medium being
// replaced stayed inside phase 2 (adoptImage / loadImage begin with
// saveDirty()), so "Mount" over a dirty 32 MiB image, the SmartPort panel's
// eject and a 3.5" mount over a dirty unit each held the lock for 115-180 ms
// with the CPU worker and the paint thread stopped. A contender thread
// samples lockState() latency while each command runs on an 8 MiB dirty
// image: 34-39 ms before the fix, under a millisecond after; the floor here
// is one PAL frame.

#include "EmulationController.h"
#include "MediaWritePolicy.h"
#include "Settings.h"
#include "SlotBus.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "SmartPort35Unit.h"
#include "SmartPort_ImGui.h"
#include "StorageCoordinator.h"
#include "Disk35Image.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using clk = std::chrono::steady_clock;

namespace {

constexpr long long kFloorUs = 20'000;   // one PAL frame

std::string writeImage(const fs::path& dir, const char* name, std::size_t bytes)
{
    const fs::path p = dir / name;
    std::vector<uint8_t> c(bytes, 0);
    std::ofstream s(p, std::ios::binary | std::ios::trunc);
    s.write(reinterpret_cast<const char*>(c.data()), static_cast<std::streamsize>(c.size()));
    return p.string();
}

struct Contender {
    EmulationController& c;
    std::atomic<bool> stop{false};
    std::atomic<long long> maxWaitUs{0};
    std::thread th;
    explicit Contender(EmulationController& ctrl) : c(ctrl) {
        th = std::thread([this] {
            while (!stop.load()) {
                const auto t0 = clk::now();
                { auto st = c.lockState(); (void)st; }
                const long long us = std::chrono::duration_cast<std::chrono::microseconds>(clk::now() - t0).count();
                long long cur = maxWaitUs.load();
                while (us > cur && !maxWaitUs.compare_exchange_weak(cur, us)) {}
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
    }
    ~Contender() { stop = true; th.join(); }
};

void dirtyUnit(pom2::SmartPortUnit* u, uint32_t blocks)
{
    std::vector<uint8_t> buf(512, 0xA5);
    for (uint32_t i = 0; i < blocks; ++i) u->writeBlock(i, buf.data());
}

pom2::SmartPortCard* plugSmartPort(EmulationController& ctrl)
{
    auto st = ctrl.lockState();
    auto card = std::make_unique<pom2::SmartPortCard>(5);
    auto* raw = card.get();
    st.memory().slotBus().plug(5, std::move(card));
    return raw;
}

void check(const char* what, long long waitUs, long long totalUs)
{
    std::printf("  %-42s total %7lld us, max lock wait %6lld us\n", what, totalUs, waitUs);
    if (waitUs > kFloorUs) {
        std::printf("FAIL: %s held stateMutex for %lld us (floor %lld)\n", what, waitUs, kFloorUs);
        assert(false && "file I/O under stateMutex");
    }
}

}  // namespace

int main()
{
    const fs::path dir = fs::temp_directory_path() / "pom2_storage_lock_budget";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::size_t MiB = 1024u * 1024u;
    const std::string big  = writeImage(dir, "big.hdv", 8 * MiB);
    const std::string big2 = writeImage(dir, "big2.hdv", 8 * MiB);
    const std::string po   = writeImage(dir, "d.po", pom2::Disk35Image::kBytesPerImage);
    const uint32_t kDirty = 12'000;

    // A. mountMediaBay over a dirty unit.
    {
        EmulationController controller; pom2::StorageCoordinator storage; pom2::Settings settings;
        settings.setReadOnly(true);
        auto* card = plugSmartPort(controller);
        assert(storage.setMediaBayType(controller, settings, 5, 0, std::string(pom2::SmartPortHdvUnit::kKindKey)).ok);
        assert(storage.mountMediaBay(controller, settings, 5, 0, big).ok);
        assert(storage.setMediaBayWriteBack(controller, settings, 5, 0, true).ok);
        { auto st = controller.lockState(); dirtyUnit(card->unit(0), kDirty); }
        Contender con(controller);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        con.maxWaitUs = 0;
        const auto t0 = clk::now();
        const auto r = storage.mountMediaBay(controller, settings, 5, 0, big2);
        const auto tot = std::chrono::duration_cast<std::chrono::microseconds>(clk::now() - t0).count();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        assert(r.ok);
        check("mountMediaBay over a dirty 8 MiB unit", con.maxWaitUs.load(), tot);
    }
    // B. The SmartPort panel's eject.
    {
        EmulationController controller; pom2::StorageCoordinator storage; pom2::Settings settings;
        settings.setReadOnly(true);
        auto* card = plugSmartPort(controller);
        assert(storage.setMediaBayType(controller, settings, 5, 0, std::string(pom2::SmartPortHdvUnit::kKindKey)).ok);
        assert(storage.mountMediaBay(controller, settings, 5, 0, big).ok);
        assert(storage.setMediaBayWriteBack(controller, settings, 5, 0, true).ok);
        { auto st = controller.lockState(); dirtyUnit(card->unit(0), kDirty); }
        Contender con(controller);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        con.maxWaitUs = 0;
        pom2::SmartPort_ImGui::Result cmd;
        cmd.units[0].eject = true;
        const auto t0 = clk::now();
        (void)storage.applySmartPortPanel(controller, settings, 5, cmd);
        const auto tot = std::chrono::duration_cast<std::chrono::microseconds>(clk::now() - t0).count();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        check("SmartPort panel eject of a dirty 8 MiB unit", con.maxWaitUs.load(), tot);
    }
    // C. mountDisk35 over a dirty SmartPort unit.
    {
        EmulationController controller; pom2::StorageCoordinator storage; pom2::Settings settings;
        settings.setReadOnly(true);
        auto* card = plugSmartPort(controller);
        assert(storage.setMediaBayType(controller, settings, 5, 0, std::string(pom2::SmartPortHdvUnit::kKindKey)).ok);
        assert(storage.mountMediaBay(controller, settings, 5, 0, big).ok);
        assert(storage.setMediaBayWriteBack(controller, settings, 5, 0, true).ok);
        { auto st = controller.lockState(); dirtyUnit(card->unit(0), kDirty); }
        Contender con(controller);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        con.maxWaitUs = 0;
        const auto t0 = clk::now();
        const auto r = storage.mountDisk35(controller, settings, 0, po);
        const auto tot = std::chrono::duration_cast<std::chrono::microseconds>(clk::now() - t0).count();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        (void)r;
        check("mountDisk35 over a dirty 8 MiB unit", con.maxWaitUs.load(), tot);
    }
    fs::remove_all(dir, ec);
    std::puts("storage_lock_budget OK");
    return 0;
}
