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

// Multi-card smoke for the SlotBus + MountableMediaCard capability layer.
//
// Regression guard for the single-instance GUI bugs fixed in the Slot
// Manager work: several cards of the same media-bearing kind can coexist on
// the bus, and the host enumerates them by walking the bus (exactly how
// MainWindow::blockCards() / smartPortCards() do) rather than via a single
// "last plugged wins" pointer. Pins:
//   * Two ProDOSHardDiskCards in different slots enumerate as TWO distinct
//     ProDOSBlockCards; mounting/ejecting one never touches the other.
//   * Two SmartPortCards likewise; per-card unit type + media are isolated.
//   * The generic MountableMediaCard cross-cast finds every media card and
//     reports the right bay count (block card = 1, SmartPort = 2).

#include "MountableMediaCard.h"
#include "ProDOSHardDiskCard.h"
#include "SlotBus.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr size_t kBlockBytes = 512;

// Write a raw .hdv of `blocks` blocks, each filled with `fill + blockIndex`.
std::string writeSynthHdv(const char* tag, size_t blocks, uint8_t fill)
{
    std::vector<uint8_t> data(blocks * kBlockBytes);
    for (size_t b = 0; b < blocks; ++b)
        std::memset(data.data() + b * kBlockBytes,
                    static_cast<uint8_t>(fill + b), kBlockBytes);
    const std::string p =
        (std::filesystem::temp_directory_path() /
         (std::string("pom2_multicard_") + tag + ".hdv")).string();
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    return p;
}

// Mirror of MainWindow::blockCards() — enumerate the bus, cross-cast each
// peripheral to the ProDOSBlockCard mix-in.
std::vector<pom2::ProDOSBlockCard*> blockCards(SlotBus& bus)
{
    std::vector<pom2::ProDOSBlockCard*> out;
    for (int s = 1; s < SlotBus::kSlotCount; ++s)
        if (auto* b = dynamic_cast<pom2::ProDOSBlockCard*>(bus.peripheral(s)))
            out.push_back(b);
    return out;
}

std::vector<pom2::SmartPortCard*> smartPortCards(SlotBus& bus)
{
    std::vector<pom2::SmartPortCard*> out;
    for (int s = 1; s < SlotBus::kSlotCount; ++s)
        if (auto* sp = dynamic_cast<pom2::SmartPortCard*>(bus.peripheral(s)))
            out.push_back(sp);
    return out;
}

std::vector<pom2::MountableMediaCard*> mediaCards(SlotBus& bus)
{
    std::vector<pom2::MountableMediaCard*> out;
    for (int s = 1; s < SlotBus::kSlotCount; ++s)
        if (auto* m = dynamic_cast<pom2::MountableMediaCard*>(bus.peripheral(s)))
            out.push_back(m);
    return out;
}

bool testTwoBlockCardsIndependent()
{
    SlotBus bus;
    bus.plug(4, std::make_unique<ProDOSHardDiskCard>(4));
    bus.plug(5, std::make_unique<ProDOSHardDiskCard>(5));

    auto blocks = blockCards(bus);
    if (blocks.size() != 2) {
        std::printf("FAIL: blockCards()=%zu (want 2)\n", blocks.size());
        return false;
    }
    // Enumeration is slot-ascending → [slot 4, slot 5], distinct slots.
    if (blocks[0]->getSlot() != 4 || blocks[1]->getSlot() != 5) {
        std::printf("FAIL: block card slots %d,%d (want 4,5)\n",
                    blocks[0]->getSlot(), blocks[1]->getSlot());
        return false;
    }

    const std::string pA = writeSynthHdv("blkA", 8, 0x20);
    const std::string pB = writeSynthHdv("blkB", 8, 0x60);

    // Mount into the slot-5 card only.
    std::string err;
    if (!blocks[1]->mountBay(0, pA, err)) {
        std::printf("FAIL: mount slot5: %s\n", err.c_str()); return false;
    }
    if (blocks[0]->isImageLoaded()) {
        std::printf("FAIL: slot4 card loaded after mounting slot5 only\n");
        return false;
    }
    if (!blocks[1]->isImageLoaded() || blocks[1]->getImagePath() != pA) {
        std::printf("FAIL: slot5 card not holding its mount\n"); return false;
    }

    // Now mount the other image into slot 4 — both independent.
    if (!blocks[0]->mountBay(0, pB, err)) {
        std::printf("FAIL: mount slot4: %s\n", err.c_str()); return false;
    }
    if (blocks[0]->getImagePath() == blocks[1]->getImagePath()) {
        std::printf("FAIL: both block cards share a path\n"); return false;
    }
    // Both report 8-block images via the generic bay view.
    if (blocks[0]->bayInfo(0).blockCount != 8 ||
        blocks[1]->bayInfo(0).blockCount != 8) {
        std::printf("FAIL: bayInfo blockCount mismatch\n"); return false;
    }

    // Eject slot 5 — slot 4 must remain mounted.
    blocks[1]->ejectBay(0);
    if (blocks[1]->isImageLoaded()) {
        std::printf("FAIL: slot5 still loaded after eject\n"); return false;
    }
    if (!blocks[0]->isImageLoaded()) {
        std::printf("FAIL: slot4 dropped when slot5 ejected\n"); return false;
    }
    std::printf("OK : two block cards independent (enumeration + isolation)\n");
    return true;
}

bool testTwoSmartPortCardsIndependent()
{
    SlotBus bus;
    bus.plug(2, std::make_unique<pom2::SmartPortCard>(2));
    bus.plug(5, std::make_unique<pom2::SmartPortCard>(5));

    auto sps = smartPortCards(bus);
    if (sps.size() != 2) {
        std::printf("FAIL: smartPortCards()=%zu (want 2)\n", sps.size());
        return false;
    }
    if (sps[0]->getSlot() != 2 || sps[1]->getSlot() != 5) {
        std::printf("FAIL: SmartPort slots %d,%d (want 2,5)\n",
                    sps[0]->getSlot(), sps[1]->getSlot());
        return false;
    }

    // Drive both via the generic capability interface (the same path the
    // Slot Manager uses).
    auto* m2 = dynamic_cast<pom2::MountableMediaCard*>(sps[0]);
    auto* m5 = dynamic_cast<pom2::MountableMediaCard*>(sps[1]);
    if (!m2 || !m5) { std::printf("FAIL: SmartPort not MountableMediaCard\n"); return false; }
    // Eight: a SmartPort card answers for its whole chain by default.
    if (m2->bayCount() != 8 || m5->bayCount() != 8) {
        std::printf("FAIL: SmartPort bayCount != 8\n"); return false;
    }

    const std::string pHdv = writeSynthHdv("spA", 8, 0x11);
    const std::string p35  =
        (std::filesystem::temp_directory_path() / "pom2_multicard_sp35.po").string();
    {   // 800K raw 3.5" image
        std::vector<uint8_t> data(1600 * kBlockBytes, 0x5A);
        std::ofstream f(p35, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }

    // Card@2 unit0 = HDV; card@5 unit0 = 3.5".
    m2->setBayType(0, "hdv");
    m5->setBayType(0, "35");
    std::string err;
    if (!m2->mountBay(0, pHdv, err)) { std::printf("FAIL: m2 mount: %s\n", err.c_str()); return false; }
    if (!m5->mountBay(0, p35,  err)) { std::printf("FAIL: m5 mount: %s\n", err.c_str()); return false; }

    const auto b2 = m2->bayInfo(0);
    const auto b5 = m5->bayInfo(0);
    if (b2.typeKey != "hdv" || !b2.loaded) { std::printf("FAIL: card@2 bay0 wrong\n"); return false; }
    if (b5.typeKey != "35"  || !b5.loaded) { std::printf("FAIL: card@5 bay0 wrong\n"); return false; }
    if (b2.path == b5.path) { std::printf("FAIL: SmartPort cards share media\n"); return false; }

    // Unit 1 of each is still empty + independent.
    if (m2->bayInfo(1).loaded || m5->bayInfo(1).loaded) {
        std::printf("FAIL: unit1 should be empty\n"); return false;
    }

    // Ejecting card@2 unit0 must not touch card@5.
    m2->ejectBay(0);
    if (m2->bayInfo(0).loaded)  { std::printf("FAIL: card@2 still loaded\n"); return false; }
    if (!m5->bayInfo(0).loaded) { std::printf("FAIL: card@5 dropped\n"); return false; }
    std::printf("OK : two SmartPort cards independent (per-card units)\n");
    return true;
}

bool testMixedMediaEnumeration()
{
    SlotBus bus;
    bus.plug(2, std::make_unique<pom2::SmartPortCard>(2));
    bus.plug(5, std::make_unique<ProDOSHardDiskCard>(5));
    bus.plug(6, std::make_unique<pom2::SmartPortCard>(6));

    auto media = mediaCards(bus);
    if (media.size() != 3) {
        std::printf("FAIL: mediaCards()=%zu (want 3)\n", media.size());
        return false;
    }
    // slot 2 = SmartPort (8 bays by default), slot 5 = ProDOS HD (2 bays —
    // drive 1 and drive 2), slot 6 = SmartPort (8). All since 2026-09-11.
    if (media[0]->bayCount() != 8 || media[1]->bayCount() != 2 ||
        media[2]->bayCount() != 8) {
        std::printf("FAIL: bay counts %d,%d,%d (want 8,2,8)\n",
                    media[0]->bayCount(), media[1]->bayCount(),
                    media[2]->bayCount());
        return false;
    }
    std::printf("OK : generic media enumeration (mixed card kinds)\n");
    return true;
}

} // anon namespace

int main()
{
    bool ok = true;
    ok &= testTwoBlockCardsIndependent();
    ok &= testTwoSmartPortCardsIndependent();
    ok &= testMixedMediaEnumeration();
    return ok ? 0 : 1;
}
