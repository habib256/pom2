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

// With the shipped dumps present, every ROM-driven card takes its REAL ROM
// path (TODO G5-15, docs/lle_vs_hle.md § Keeping a level once you have it).
//
// Each of these cards degrades SILENTLY to a lower level when its dump is
// absent — the Disk II to the legacy nibble gate, the ThunderClock and the
// Grappler+ to synthetic stubs, the SmartPort card to a synthetic identity —
// which is the right product behaviour and exactly how an L path stops being
// exercised with a green suite. The tree tracks every dump named here, so
// this test does not skip: a card that lands on its fallback on a complete
// checkout is a regression (a renamed file, a moved probe, a factory that
// stopped asking). Built the way the machine builds them — SlotCardFactory
// with the production resource locator, or the card's own probing
// constructor — and run from the repo root.

#include "ClockCard.h"
#include "CffaCard.h"
#include "DiskIICard.h"
#include "GrapplerCard.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "ResourcePaths.h"
#include "SlotCardFactory.h"
#include "SmartPortCard.h"
#include "TranswarpCard.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace {

int failures = 0;
void expect(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
    else       std::printf("  ok: %s\n", what);
}

std::unique_ptr<SlotPeripheral> make(const char* key, int slot, bool cmos = true)
{
    const pom2::SlotCardFactory factory;
    pom2::SlotCardFactory::Request req;
    req.key = key;
    req.slot = slot;
    req.cpuIsCmos = cmos;
    req.profile = pom2::SystemProfile::AppleIIe;
    auto made = factory.create(req);
    if (made.card && made.actualKey != key) return nullptr;   // a fallback card
    return std::move(made.card);
}

}  // namespace

int main()
{
    namespace fs = std::filesystem;
    // The per-user data dir is the FIRST resource root. A developer who has
    // dumps there would see this test pass with a file missing from the tree,
    // so point it at an empty sandbox before anything resolves a path.
    {
        const fs::path home = fs::temp_directory_path() / "pom2_rom_path_taken_home";
        std::error_code rec;
        fs::remove_all(home, rec);
        fs::create_directories(home, rec);
        setenv("HOME", home.string().c_str(), 1);
        setenv("XDG_DATA_HOME", home.string().c_str(), 1);
    }

    // Disk II: the P6 state PROM drives the bit-level LSS.
    {
        auto card = make("diskii", 6);
        auto* d = dynamic_cast<DiskIICard*>(card.get());
        const std::string master = pom2::findResource("disks_5.4/dsk/dos33_master.dsk");
        const fs::path copy = fs::temp_directory_path() / "pom2_rom_path_taken.dsk";
        std::error_code ec;
        fs::copy_file(master, copy, fs::copy_options::overwrite_existing, ec);
        expect(d && !ec && d->insertDisk(0, copy.string()) && d->usingBitLss(),
               "Disk II: diskii_p6.rom loads and the bit-level LSS runs");
        if (d) d->ejectDisk(0);
        fs::remove(copy, ec);
    }

    // ThunderClock+: the U9 EPROM, not the synthetic ProDOS-signature stub.
    {
        ClockCard clock(4);
        expect(clock.romFromDump(), "ThunderClock+: the Thunderware U9 dump is the slot ROM");
    }

    // Grappler+: the Orange Micro EPROM, not buildStubRom().
    {
        auto card = make("grappler", 1);
        auto* g = dynamic_cast<GrapplerCard*>(card.get());
        expect(g && g->isRomLoaded(), "Grappler+: grappler_plus.bin is the slot ROM");
    }

    // Mouse Card, MAME level: slot EPROM and the 68705 mask ROM both.
    {
        auto card = make("mouse", 4);
        auto* m = dynamic_cast<MouseCard*>(card.get());
        expect(m && m->isReady(), "Mouse Card (L0): slot EPROM + MC68705 ROM loaded");
    }
    {
        auto card = make("mouseaw", 4);
        auto* m = dynamic_cast<MouseCardAppleWin*>(card.get());
        expect(m && m->isReady(), "Mouse Card (AppleWin HLE): slot EPROM loaded");
    }

    // SmartPort 3.5" on a //e: the real Liron identity.
    {
        auto card = make("smartport35", 5);
        auto* s = dynamic_cast<pom2::SmartPortCard*>(card.get());
        expect(s && s->isLironRomLoaded(), "SmartPort card: liron.rom supplies the identity");
    }

    // The ROM-GATED cards: no dump, no card — so a built card is the proof.
    expect(make("liron", 5) != nullptr, "Liron (real firmware): liron.rom found");
    expect(make("workstation", 7) != nullptr, "Workstation Card: 341-0358-A found");
    for (bool cmos : { false, true }) {
        auto card = make("cffa", 7, cmos);
        auto* c = dynamic_cast<pom2::CffaCard*>(card.get());
        expect(c && c->isRomLoaded(),
               cmos ? "CFFA 2.0: the 65C02 firmware loads"
                    : "CFFA 2.0: the 6502 firmware loads");
    }

    // TransWarp: the speed-corrected Monitor overlay.
    {
        pom2::TranswarpCard tw(3);
        const std::string why = tw.loadRomFromDisk();
        expect(why.empty() && tw.hasRom(), "TransWarp: ae_transwarp_1.4.bin loads");
    }

    if (failures) {
        std::printf("rom_path_taken: %d card(s) on a fallback with the dumps present\n", failures);
        return 1;
    }
    std::printf("rom_path_taken OK\n");
    return 0;
}
