// An HDV mounted from the library adds a volume — 2026-09-08.
//
// With a SmartPort card answering for N units, "Mount only" lands in the
// first free bay (an empty bay is given the HDV type first), the N+1th
// mount is refused with nothing mounted, and an ejected bay is reused.

#include "EmulationController.h"
#include "LironCard.h"
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
        assert(card2->unitCount() == 8);   // the ceiling, by default (2026-09-11)
        card2->setUnitCount(2);
        assert(storage.mountHdvIntoFreeBay(c2, settings, imgs[0]).ok);
        assert(storage.mountHdvIntoFreeBay(c2, settings, imgs[1]).ok);
        assert(!storage.mountHdvIntoFreeBay(c2, settings, imgs[2]).ok);
    }
    // A //e whose only disk card is a Liron (2026-09-11): its chain takes
    // the hard disks, first free unit first. The library used to answer
    // "plug an HDV or SmartPort card" with the Liron sitting right there.
    {
        EmulationController c3;
        pom2::LironCard* liron = nullptr;
        {
            auto st = c3.lockState();
            auto c = std::make_unique<pom2::LironCard>(6);
            liron = c.get();
            st.memory().slotBus().plug(6, std::move(c));
        }
        for (int n = 0; n < 2; ++n) {
            const auto r = storage.mountHdvIntoFreeBay(c3, settings, imgs[n]);
            if (!r.ok) std::printf("liron mount %d: %s\n", n, r.error.c_str());
            assert(r.ok && r.bootSlot == 6);
            const auto info = liron->bayInfo(n);
            assert(info.loaded && info.path == imgs[n] &&
                   info.kindLabel == "ProDOS HDV" && "a Liron unit took the hard disk");
        }
        std::printf("  ok: the Liron's chain takes hard disks from the library\n");
        // …and 3.5" disks through the library's 3.5" route, which used to
        // drop them on the //c+'s on-board drive a //e does not have. Into
        // unit 1, over the hard disk the loop above put there.
        const fs::path d35 = dir / "disk35.po";
        {
            std::vector<char> zeros(819200, 0);
            std::ofstream f(d35, std::ios::binary | std::ios::trunc);
            f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
        }
        const auto m = storage.mountDisk35(c3, settings, 1, d35.string());
        if (!m.ok) std::printf("liron 3.5: %s\n", m.error.c_str());
        assert(m.ok && m.bootSlot == 6);
        assert(liron->bayInfo(1).kindLabel == "3.5\" 800K" &&
               liron->bayInfo(1).path == d35.string() &&
               !liron->blockBackings()[1]->isLoaded());
    }
    fs::remove_all(dir, ec);
    std::puts("hdv_free_bay OK");
    return 0;
}
