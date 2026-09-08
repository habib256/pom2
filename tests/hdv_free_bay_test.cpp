// An HDV mounted from the library adds a volume — 2026-09-08.
//
// With a SmartPort card answering for N units, "Mount only" lands in the
// first free bay (an empty bay is given the HDV type first), the N+1th
// mount is refused with nothing mounted, and an ejected bay is reused.

#include "EmulationController.h"
#include "Settings.h"
#include "SlotBus.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "StorageCoordinator.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
std::string scratchHdv(const fs::path& dir, int n)
{
    const fs::path p = dir / ("free_bay_" + std::to_string(n) + ".hdv");
    std::vector<char> zeros(512 * 64, 0);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    return p.string();
}
}  // namespace

int main()
{
    const fs::path dir = fs::temp_directory_path() / "pom2_hdv_free_bay";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    std::vector<std::string> imgs;
    for (int n = 0; n < 5; ++n) imgs.push_back(scratchHdv(dir, n));

    EmulationController controller;
    pom2::StorageCoordinator storage;
    pom2::Settings settings;
    settings.setReadOnly(true);
    pom2::SmartPortCard* card = nullptr;
    {
        auto st = controller.lockState();
        auto c = std::make_unique<pom2::SmartPortCard>(5);
        card = c.get();
        c->setUnitCount(4);
        st.memory().slotBus().plug(5, std::move(c));
    }

    // Four mounts land in four distinct bays, in order.
    for (int n = 0; n < 4; ++n) {
        const auto r = storage.mountHdvIntoFreeBay(controller, settings, imgs[n]);
        if (!r.ok) std::printf("mount %d: %s\n", n, r.error.c_str());
        assert(r.ok && r.bootSlot == 5);
        const auto* u = card->unit(static_cast<std::size_t>(n));
        assert(u && u->isLoaded() && u->path() == imgs[n] && "landed in the wrong bay");
    }
    // The fifth is refused, and nothing changed.
    {
        const auto r = storage.mountHdvIntoFreeBay(controller, settings, imgs[4]);
        assert(!r.ok && "a fifth HDV on four units must be refused");
        std::printf("  refused as expected: %s\n", r.error.c_str());
        for (int n = 0; n < 4; ++n)
            assert(card->unit(static_cast<std::size_t>(n))->path() == imgs[n]);
    }
    // Eject bay 1: the next mount reuses it.
    {
        assert(storage.ejectMediaBay(controller, settings, 5, 1).ok);
        const auto r = storage.mountHdvIntoFreeBay(controller, settings, imgs[4]);
        assert(r.ok);
        assert(card->unit(1)->path() == imgs[4] && "the freed bay must be reused");
    }
    // A card answering for two units refuses the third even with eight bays.
    {
        EmulationController c2;
        pom2::SmartPortCard* card2 = nullptr;
        {
            auto st = c2.lockState();
            auto c = std::make_unique<pom2::SmartPortCard>(5);
            card2 = c.get();
            st.memory().slotBus().plug(5, std::move(c));
        }
        assert(card2->unitCount() == 2);
        assert(storage.mountHdvIntoFreeBay(c2, settings, imgs[0]).ok);
        assert(storage.mountHdvIntoFreeBay(c2, settings, imgs[1]).ok);
        assert(!storage.mountHdvIntoFreeBay(c2, settings, imgs[2]).ok);
    }
    fs::remove_all(dir, ec);
    std::puts("hdv_free_bay OK");
    return 0;
}
