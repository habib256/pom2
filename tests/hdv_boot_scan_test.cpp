// POM2 — GPL-3.0-or-later
// Boot order around a synthetic HDV / SmartPort card — 2026-10-16.
//
// Three defects, each pinned by running the real system ROM against boot
// images that each leave a distinct marker at $0300 (the shape
// `boot_configuration` uses):
//
//   A. An EMPTY HDV or SmartPort card in slot 7 of a //e used to park the
//      CPU in its boot-failure halt loop, so the autostart scan ($C7 → $C1)
//      never reached the Disk II in slot 6. The synthetic firmware now does
//      what the real Liron does (roms/liron.rom $E90-$ED3): when the scan
//      brought it there, JMP $FABA and let the scan go on. Entered any other
//      way it prints "ERR" and goes to BASIC.
//   B. The HDV card signed $Cn07 = $01, and the four-byte autostart of an
//      Unenhanced //e (LDY #$07 at $FABA) walked past it. It signs $3C now,
//      like AppleWin's HDD firmware.
//   C. On a //c whose SmartPort unit 1 is an empty 3.5" bay, the library's
//      "Mount only" put an HDV in unit 2 — and the //c firmware boots unit 1
//      only ($C570 param block), so the machine never booted it. The empty
//      non-HDV unit 1 is retyped and takes the disk when nothing else on the
//      chain holds a medium.

#include "DiskIICard.h"
#include "EmulationController.h"
#include "Memory.h"
#include "ProDOSHardDiskCard.h"
#include "ResourcePaths.h"
#include "Settings.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "StorageCoordinator.h"
#include "SystemProfile.h"
#include "TestTempPath.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
void expect(bool ok, const std::string& what)
{
    std::printf("%s: %s\n", ok ? "  ok" : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

std::string image(const char* leaf, std::size_t size, uint8_t marker)
{
    const auto path = pom2test::tempPath(leaf);
    std::vector<uint8_t> bytes(size, 0);
    // Sector/block 0 lands at $0800, entry $0801: store the marker, spin.
    const std::array<uint8_t, 9> code{1, 0xA9, marker, 0x8D, 0x00, 0x03, 0x4C, 0x06, 0x08};
    std::copy(code.begin(), code.end(), bytes.begin());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return path;
}

bool configure(EmulationController& controller, pom2::SystemProfile profile)
{
    const auto& cfg = pom2::profileConfig(profile);
    const auto rom = pom2::findFirstResource(cfg.romProbeOrder);
    if (rom.empty()) return false;
    controller.memory().setIIEMode(cfg.iieMode);
    controller.cpu().setCpuMode(cfg.defaultCpu);
    controller.setVideoStandard(cfg.videoStandard);
    return controller.memory().loadAppleIIRom(rom.c_str(), cfg.noPhysicalSlots);
}

bool plugDiskII(EmulationController& controller, const std::string& floppy)
{
    auto d2 = std::make_unique<DiskIICard>(6);
    if (!d2->loadBootRom(pom2::findResource("roms/disk2.rom"))) return false;
    if (!d2->loadLssRom(pom2::findResource("roms/diskii_p6.rom"))) return false;
    d2->setIWM(&controller.iwm());
    if (!floppy.empty() && !d2->insertDisk(0, floppy)) return false;
    controller.memory().slotBus().plug(6, std::move(d2));
    return true;
}

/// Cold boot, then run until a marker lands at $0300 (0 = none in budget).
int bootMarker(EmulationController& controller)
{
    controller.coldBoot();
    for (long cycles = 0; cycles < 30'000'000; ) {
        cycles += controller.cpu().run(4096);
        const auto m = controller.memory().peekMainRam(0x0300);
        if (m == 0x11 || m == 0x33) return m;
    }
    return 0;
}

std::string textPage(Memory& mem)
{
    static const int rowBase[24] = {
        0x400,0x480,0x500,0x580,0x600,0x680,0x700,0x780,
        0x428,0x4A8,0x528,0x5A8,0x628,0x6A8,0x728,0x7A8,
        0x450,0x4D0,0x550,0x5D0,0x650,0x6D0,0x750,0x7D0 };
    std::string s;
    for (int r = 0; r < 24; ++r) {
        for (int c = 0; c < 40; ++c) {
            const uint8_t b = mem.peekMainRam(static_cast<uint16_t>(rowBase[r] + c)) & 0x7F;
            s += (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : ' ';
        }
        s += '\n';
    }
    return s;
}

enum class Slot7 { EmptyHdv, EmptySmartPort, EmptyLiron };

std::unique_ptr<SlotPeripheral> makeEmpty(Slot7 kind)
{
    if (kind == Slot7::EmptyHdv) return std::make_unique<ProDOSHardDiskCard>(7);
    auto sp = std::make_unique<pom2::SmartPortCard>(7);
    if (kind == Slot7::EmptyLiron) {
        const auto liron = pom2::findResource("roms/liron.rom");
        if (liron.empty() || !sp->loadLironRom(liron)) return nullptr;
    }
    return sp;
}

const char* label(Slot7 kind)
{
    switch (kind) {
    case Slot7::EmptyHdv:       return "empty HDV card";
    case Slot7::EmptySmartPort: return "empty SmartPort card";
    case Slot7::EmptyLiron:     return "empty SmartPort card (Liron identity)";
    }
    return "?";
}

}  // namespace

int main()
{
    using pom2::SystemProfile;
    const auto floppy = image("pom2_boot_scan.dsk", 143360, 0x11);
    const auto hard   = image("pom2_boot_scan.hdv", 819200, 0x33);

    // ── A. an empty slot-7 block card hands the scan on to slot 6 ─────────
    for (auto kind : {Slot7::EmptyHdv, Slot7::EmptySmartPort, Slot7::EmptyLiron}) {
        EmulationController controller;
        if (!configure(controller, SystemProfile::AppleIIe)) return 77;
        auto card = makeEmpty(kind);
        if (!card) return 77;
        controller.memory().slotBus().plug(7, std::move(card));
        if (!plugDiskII(controller, floppy)) return 77;
        expect(bootMarker(controller) == 0x11,
               std::string("//e Enhanced, ") + label(kind) +
               " in slot 7: the autostart goes on to the Disk II in slot 6");
    }

    // …and entered outside the scan (bootFromSlot = coldBoot + JMP $C700,
    // so $01 != $07F8), it says so and lands in BASIC instead of hanging.
    for (auto kind : {Slot7::EmptyHdv, Slot7::EmptySmartPort}) {
        EmulationController controller;
        if (!configure(controller, SystemProfile::AppleIIe)) return 77;
        controller.memory().slotBus().plug(7, makeEmpty(kind));
        if (!controller.bootFromSlot(7)) { expect(false, "bootFromSlot(7)"); continue; }
        for (long cycles = 0; cycles < 3'000'000; )
            cycles += controller.cpu().run(4096);
        const std::string screen = textPage(controller.memory());
        const uint16_t pc = controller.cpu().getProgramCounter();
        const bool inSlot7 = (pc >> 8) == 0xC7;
        expect(screen.find("ERR") != std::string::npos && !inSlot7,
               std::string("explicit boot of an ") + label(kind) +
               ": \"ERR\" and BASIC, not a halt in $C7xx");
        if (screen.find("ERR") == std::string::npos || inSlot7)
            std::printf("PC=$%04X\n%s", pc, screen.c_str());
    }

    // ── B. the Unenhanced //e's four-byte autostart boots the HDV card ────
    for (auto profile : {SystemProfile::AppleIIeUnenhanced, SystemProfile::AppleIIPlus}) {
        EmulationController controller;
        if (!configure(controller, profile)) return 77;
        auto card = std::make_unique<ProDOSHardDiskCard>(7);
        if (!card->loadDrive(0, hard)) { expect(false, "mount HDV"); continue; }
        controller.memory().slotBus().plug(7, std::move(card));
        if (!plugDiskII(controller, floppy)) return 77;
        expect(controller.memory().memRead(0xC707) == 0x3C, "HDV card signs $Cn07 = $3C");
        expect(bootMarker(controller) == 0x33,
               std::string(pom2::profileKey(profile)) +
               ": autostart boots the HDV in slot 7 ahead of slot 6");
    }

    // ── C. //c "Mount only" with unit 1 an empty 3.5" bay ─────────────────
    for (auto profile : {SystemProfile::AppleIIc, SystemProfile::AppleIIcPAL}) {
        EmulationController controller;
        if (!configure(controller, profile)) return 77;
        if (!plugDiskII(controller, "")) return 77;
        pom2::SmartPortCard* sp = nullptr;
        {
            auto card = std::make_unique<pom2::SmartPortCard>(5);
            sp = card.get();
            card->setUnitCount(8);
            card->setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
            for (int u = 1; u < 8; ++u)
                card->setUnit(static_cast<std::size_t>(u),
                              std::make_unique<pom2::SmartPortHdvUnit>());
            controller.memory().slotBus().plug(5, std::move(card));
        }
        pom2::Settings settings;
        settings.setReadOnly(true);
        pom2::StorageCoordinator storage;
        const auto r = storage.mountHdvIntoFreeBay(controller, settings, hard);
        if (!r.ok) std::printf("mount: %s\n", r.error.c_str());
        const auto* u0 = sp->unit(0);
        expect(r.ok && r.bootSlot == 5 && u0 &&
                   u0->kindKey() == pom2::SmartPortHdvUnit::kKindKey &&
                   u0->isLoaded() && u0->path() == hard,
               std::string(pom2::profileKey(profile)) +
               ": the HDV lands in unit 1 (retyped from an empty 3.5\")");
        expect(bootMarker(controller) == 0x33,
               std::string(pom2::profileKey(profile)) +
               ": power-on boots it through the //c firmware");
        // A second HDV goes to the next free unit; unit 1 keeps the first.
        const auto hard2 = image("pom2_boot_scan_2.hdv", 819200, 0x33);
        const auto r2 = storage.mountHdvIntoFreeBay(controller, settings, hard2);
        expect(r2.ok && sp->unit(0)->path() == hard && sp->unit(1)->path() == hard2,
               "a second HDV adds a volume in unit 2");
        std::filesystem::remove(hard2);
    }
    // With a medium already on the chain, unit 1 is left alone.
    {
        EmulationController controller;
        if (!configure(controller, SystemProfile::AppleIIc)) return 77;
        pom2::SmartPortCard* sp = nullptr;
        {
            auto card = std::make_unique<pom2::SmartPortCard>(5);
            sp = card.get();
            card->setUnitCount(4);
            card->setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
            for (int u = 1; u < 4; ++u)
                card->setUnit(static_cast<std::size_t>(u),
                              std::make_unique<pom2::SmartPortHdvUnit>());
            std::string err;
            if (!card->mountBay(1, hard, err)) { expect(false, "seed unit 2"); return 1; }
            controller.memory().slotBus().plug(5, std::move(card));
        }
        pom2::Settings settings;
        settings.setReadOnly(true);
        pom2::StorageCoordinator storage;
        const auto hard2 = image("pom2_boot_scan_3.hdv", 819200, 0x33);
        const auto r = storage.mountHdvIntoFreeBay(controller, settings, hard2);
        expect(r.ok && sp->unit(0)->kindKey() == pom2::SmartPort35Unit::kKindKey &&
                   sp->unit(2)->path() == hard2,
               "chain already holding a disk: unit 1 keeps its 3.5\" type");
        std::filesystem::remove(hard2);
    }

    std::filesystem::remove(floppy);
    std::filesystem::remove(hard);
    if (g_failures) { std::printf("hdv_boot_scan: %d failure(s)\n", g_failures); return 1; }
    std::puts("hdv_boot_scan OK");
    return 0;
}
