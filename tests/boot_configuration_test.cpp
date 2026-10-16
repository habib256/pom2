// POM2 — GPL-3.0-or-later
// Execute real system ROMs with distinguishable boot sectors. Do not infer
// boot order from the displayed drives or from firmware signature reads.
#include "DiskIICard.h"
#include "EmulationController.h"
#include "Memory.h"
#include "MediaBoot.h"
#include "LironCard.h"
#include "ProDOSHardDiskCard.h"
#include "ResourcePaths.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "Settings.h"
#include "StorageCoordinator.h"
#include "SlotCardFactory.h"
#include "SlotConfigurationCoordinator.h"
#include "SlotProvisioningCoordinator.h"
#include "SystemProfile.h"
#include "TestTempPath.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace {
class SignatureCard : public SlotPeripheral {
public:
    explicit SignatureCard(uint8_t type) : type_(type) {}
    std::string_view name() const override { return "Boot signature fixture"; }
    uint8_t slotRomRead(uint8_t offset) override {
        if (offset == 1) return 0x20;
        if (offset == 3) return 0;
        if (offset == 5) return 3;
        if (offset == 7) return type_;
        return 0xEA;
    }
private:
    uint8_t type_;
};

std::string image(const char* leaf, std::size_t size, uint8_t marker) {
    const auto path = pom2test::tempPath(leaf);
    std::vector<uint8_t> bytes(size, 0);
    // Sector/block 0 loaded at $0800, entry $0801. Retire a marker then spin.
    const std::array<uint8_t, 9> code{1, 0xA9, marker, 0x8D, 0x00, 0x03, 0x4C, 0x06, 0x08};
    std::copy(code.begin(), code.end(), bytes.begin());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    assert(out.good());
    return path;
}

bool configure(EmulationController& controller, pom2::SystemProfile profile) {
    const auto& cfg = pom2::profileConfig(profile);
    const auto rom = pom2::findFirstResource(cfg.romProbeOrder);
    if (rom.empty()) return false;
    auto& memory = controller.memory();
    memory.setIIEMode(cfg.iieMode);
    controller.cpu().setCpuMode(cfg.defaultCpu);
    controller.setVideoStandard(cfg.videoStandard);
    return memory.loadAppleIIRom(rom.c_str(), cfg.noPhysicalSlots);
}

int autostartSlot(EmulationController& controller) {
    controller.coldBoot();
    for (int instructions = 0; instructions < 500'000; ++instructions) {
        const auto pc = controller.cpu().getProgramCounter();
        if (pc == 0xC600 || pc == 0xC700)
            return (pc >> 8) & 7;
        controller.cpu().step();
    }
    return 0;
}

int bootMarker(EmulationController& controller, bool explicitSlot5 = false) {
    if (explicitSlot5) {
        // Refusal must preserve both RAM and CPU: no surprise restart of a
        // different disk while reporting that the requested HD was booted.
        controller.memory().memWrite(0x0300, 0x77);
        const auto pc = controller.cpu().getProgramCounter();
        std::string error;
        if (!pom2::bootMountedMedia(controller, 5, 0, error)) {
            assert(!error.empty());
            assert(controller.memory().peekMainRam(0x0300) == 0x77);
            assert(controller.cpu().getProgramCounter() == pc);
            return -1;
        }
    }
    else controller.coldBoot();
    for (long cycles = 0; cycles < 30'000'000; ) {
        cycles += controller.cpu().run(4096);
        const auto marker = controller.memory().peekMainRam(0x0300);
        if (marker == 0x11 || marker == 0x22 || marker == 0x33 || marker == 0x44 || marker == 0x55)
            return marker;
    }
    return 0;
}
}

int main() {
    using pom2::SystemProfile;
    int failures = 0;
    auto expect = [&](int actual, int wanted, const char* label) {
        std::printf("%s: got $%02X expected $%02X\n", label, actual, wanted);
        std::fflush(stdout);
        if (actual != wanted) ++failures;
    };
    for (auto profile : {SystemProfile::AppleII, SystemProfile::AppleIIPlus,
                         SystemProfile::AppleIIeUnenhanced, SystemProfile::AppleIIe,
                         SystemProfile::AppleIIeUnenhancedPAL, SystemProfile::AppleIIePAL}) {
        EmulationController controller;
        if (!configure(controller, profile)) return 77;
        controller.memory().slotBus().plug(7, std::make_unique<SignatureCard>(1));
        controller.memory().slotBus().plug(6, std::make_unique<SignatureCard>(0x3C));
        expect(autostartSlot(controller), profile == SystemProfile::AppleII ? 0 :
               (profile == SystemProfile::AppleIIe || profile == SystemProfile::AppleIIePAL ? 7 : 6),
               std::string(pom2::profileKey(profile)).c_str());
        // Explicit slot selection bypasses a higher-priority card. This is
        // what a Disk II "insert + boot" must use instead of coldBoot().
        assert(controller.bootFromSlot(6));
        expect(controller.cpu().getProgramCounter(), 0xC600, "explicit slot 6");
    }
    const auto floppy1 = image("pom2_boot_order_1.dsk", 143360, 0x11);
    const auto floppy2 = image("pom2_boot_order_2.dsk", 143360, 0x22);
    const auto hardDisk = image("pom2_boot_order.hdv", 819200, 0x33);
    const auto internal35 = image("pom2_boot_order_internal.po", 819200, 0x44);
    const auto external35 = image("pom2_boot_order_external.po", 819200, 0x55);
    const auto smartDisk = image("pom2_boot_order_smart.hdv", 819200, 0x55);
    // The same saved choices must become native storage on each machine:
    // a physical S7 HDV on slotted machines, the forced S5 port on //c.
    pom2::Settings bootSettings;
    bootSettings.setReadOnly(true);
    for (int slot = 1; slot <= 7; ++slot)
        bootSettings.setString("slot_" + std::to_string(slot) + "_card", "");
    bootSettings.setString("slot_7_card", "hdv");
    bootSettings.setString("slot_6_card", "diskii");
    bootSettings.setString("slot_5_card", "smartport35");
    bootSettings.setString("disk_path_slot6", floppy1);
    bootSettings.setString("hdv_path", hardDisk);
    bootSettings.setString("smartport_slot5_unit0_type", std::string(pom2::SmartPortHdvUnit::kKindKey));
    bootSettings.setString("smartport_slot5_unit0_path", smartDisk);
    for (auto profile : pom2::allProfiles()) {
        EmulationController controller;
        assert(configure(controller, profile));
        pom2::SlotConfigurationCoordinator slots;
        pom2::SlotCardFactory factory;
        const auto& plan = slots.resolve(bootSettings, profile);
        const auto& cfg = pom2::profileConfig(profile);
        assert(plan[6] == "diskii" && plan[5] == "smartport35");
        assert(plan[7] == (profile == SystemProfile::AppleIIcPAL ? "chatmauve" :
                          cfg.noPhysicalSlots ? "" : "hdv"));
        // Instantiate the storage subset of the production plan using its
        // real factory, then the production settings restore (no manual mounts).
        for (int slot = 1; slot <= 7; ++slot) {
            if (plan[slot] != "diskii" && plan[slot] != "hdv" && plan[slot] != "smartport35") continue;
            auto made = factory.create({plan[slot], slot,
                cfg.defaultCpu == M6502::CpuMode::CMOS, profile});
            assert(made);
            if (auto* disk = dynamic_cast<DiskIICard*>(made.card.get())) {
                disk->setCpu(&controller.cpu());
                if (slot == 6) disk->setIWM(&controller.iwm());
            }
            controller.memory().slotBus().plug(slot, std::move(made.card));
        }
        pom2::StorageCoordinator storage;
        assert(storage.restoreMediaFromSettings(controller.memory().slotBus(), bootSettings).ok());
        if (profile != SystemProfile::AppleII)
            // The S7 HDV signs $Cn07 = $3C (AppleWin's HDD firmware does too),
            // so the four-byte autostart of the ][+ and the Unenhanced //e
            // boots it as well as the Enhanced //e's three-byte one
            // (2026-10-16; before, only the Enhanced machines reached it).
            expect(bootMarker(controller), profile == SystemProfile::AppleIIcPlus ? 0x55 :
                   (cfg.noPhysicalSlots ? 0x11 : 0x33),
                   ("resolved storage boot " + std::string(pom2::profileKey(profile))).c_str());
        assert(bootSettings.getString("slot_7_card") == "hdv");
    }
    auto compactCase = [&](SystemProfile profile, bool disk1, bool disk2,
                           bool hd, bool sonyInternal, bool sonyExternal,
                           bool explicitSlot5, int wanted, const char* label) {
        EmulationController controller;
        if (!configure(controller, profile)) { ++failures; return; }
        auto floppy = std::make_unique<DiskIICard>(6);
        floppy->loadBootRom(pom2::findResource("roms/disk2.rom"));
        floppy->loadLssRom(pom2::findResource("roms/diskii_p6.rom"));
        floppy->setIWM(&controller.iwm());
        if (disk1) assert(floppy->insertDisk(0, floppy1));
        if (disk2) assert(floppy->insertDisk(1, floppy2));
        controller.memory().slotBus().plug(6, std::move(floppy));
        auto smart = std::make_unique<pom2::SmartPortCard>(5);
        smart->setUnit(0, std::make_unique<pom2::SmartPortHdvUnit>());
        std::string error;
        if (hd) assert(smart->mountBay(0, hardDisk, error));
        controller.memory().slotBus().plug(5, std::move(smart));
        if (sonyInternal) assert(controller.mount35(0, internal35));
        if (sonyExternal) {
            controller.connectExternalSony35(1, true);
            assert(controller.mount35(1, external35));
        }
        expect(bootMarker(controller, explicitSlot5), wanted, label);
    };
    for (auto profile : {SystemProfile::AppleIIc, SystemProfile::AppleIIcPAL}) {
        compactCase(profile, true, true, true, false, false, false, 0x11, "//c internal before HD");
        compactCase(profile, false, true, true, false, false, false, 0x33, "//c HD before external 5.25");
        compactCase(profile, true, true, true, false, false, true, 0x33, "//c explicit SmartPort bypasses internal floppy");
    }
    compactCase(SystemProfile::AppleIIcPlus, true, false, true, true, false, false, 0x44, "//c+ internal Sony vs 5.25 vs HD");
    compactCase(SystemProfile::AppleIIcPlus, true, false, true, false, false, false, 0x33, "//c+ HD before 5.25");
    compactCase(SystemProfile::AppleIIcPlus, true, false, false, false, false, false, 0x11, "//c+ 5.25 fallback without HD");
    compactCase(SystemProfile::AppleIIcPlus, false, false, true, true, false, false, 0x44, "//c+ internal Sony vs HD");
    compactCase(SystemProfile::AppleIIcPlus, false, false, true, false, true, false, 0x55, "//c+ external Sony vs HD");
    compactCase(SystemProfile::AppleIIcPlus, true, false, true, false, true, false, 0x55, "//c+ external Sony vs 5.25 vs HD");
    compactCase(SystemProfile::AppleIIcPlus, false, false, true, false, false, false, 0x33, "//c+ HD alone");
    compactCase(SystemProfile::AppleIIcPlus, true, false, true, true, false, true, -1, "//c+ SmartPort refuses competing Sony");
    compactCase(SystemProfile::AppleIIcPlus, true, false, true, false, false, true, 0x33, "//c+ explicit SmartPort before 5.25");
    compactCase(SystemProfile::AppleIIcPlus, false, false, true, false, false, true, 0x33, "//c+ explicit SmartPort alone");
    {
        EmulationController controller;
        assert(configure(controller, SystemProfile::AppleIIcPlus));
        auto floppy = std::make_unique<DiskIICard>(6);
        assert(floppy->loadBootRom(pom2::findResource("roms/disk2.rom")));
        assert(floppy->loadLssRom(pom2::findResource("roms/diskii_p6.rom")));
        floppy->setIWM(&controller.iwm());
        controller.memory().slotBus().plug(6, std::move(floppy));
        assert(controller.mount35(0, internal35));
        controller.connectExternalSony35(1, true);
        assert(controller.mount35(1, external35));
        controller.memory().memWrite(0x0300, 0x77);
        const auto pc = controller.cpu().getProgramCounter();
        std::string error;
        assert(!pom2::bootMountedMedia(controller, -1, 1, error));
        assert(!error.empty());
        assert(controller.memory().peekMainRam(0x0300) == 0x77);
        assert(controller.cpu().getProgramCounter() == pc);
        assert(controller.eject35(0));
        assert(pom2::bootMountedMedia(controller, -1, 1, error));
        int marker = 0;
        for (long cycles = 0; cycles < 30'000'000 && marker != 0x55; ) {
            cycles += controller.cpu().run(4096);
            marker = controller.memory().peekMainRam(0x0300);
            if (marker == 0x44) break;
        }
        expect(marker, 0x55, "explicit external Sony after ejecting internal disk");
    }
    {
        EmulationController controller;
        assert(configure(controller, SystemProfile::AppleIIe));
        auto card = std::make_unique<ProDOSHardDiskCard>(7);
        assert(card->loadDrive(1, hardDisk));
        controller.memory().slotBus().plug(7, std::move(card));
        controller.memory().memWrite(0x0300, 0x77);
        const auto pc = controller.cpu().getProgramCounter();
        std::string error;
        assert(!pom2::bootMountedMedia(controller, 7, 1, error));
        assert(!error.empty());
        assert(controller.memory().peekMainRam(0x0300) == 0x77);
        assert(controller.cpu().getProgramCounter() == pc);
    }
    for (auto profile : {SystemProfile::AppleIIe, SystemProfile::AppleIIeUnenhanced,
                         SystemProfile::AppleIIePAL, SystemProfile::AppleIIeUnenhancedPAL}) {
        EmulationController controller;
        assert(configure(controller, profile));
        auto card = std::make_unique<ProDOSHardDiskCard>(3);
        assert(card->loadDrive(0, hardDisk));
        controller.memory().slotBus().plug(3, std::move(card));
        controller.memory().memWrite(0x0300, 0x77);
        const auto pc = controller.cpu().getProgramCounter();
        std::string error;
        assert(!pom2::bootMountedMedia(controller, 3, 0, error));
        assert(!error.empty());
        assert(controller.memory().peekMainRam(0x0300) == 0x77);
        assert(controller.cpu().getProgramCounter() == pc);
    }
    {
        // A library mount may choose a Liron in slot 4, even with a Disk II
        // in slot 6. Boot the returned route rather than resetting generally.
        EmulationController controller;
        assert(configure(controller, SystemProfile::AppleIIe));
        auto floppy = std::make_unique<DiskIICard>(6);
        assert(floppy->loadBootRom(pom2::findResource("roms/disk2.rom")));
        assert(floppy->loadLssRom(pom2::findResource("roms/diskii_p6.rom")));
        assert(floppy->insertDisk(0, floppy1));
        controller.memory().slotBus().plug(6, std::move(floppy));
        auto liron = std::make_unique<pom2::LironCard>(4);
        assert(liron->romLoaded());
        controller.memory().slotBus().plug(4, std::move(liron));
        pom2::Settings settings;
        settings.setReadOnly(true);
        pom2::StorageCoordinator storage;
        const auto mounted = storage.mountDisk35(controller, settings, 0, internal35);
        assert(mounted.ok && mounted.bootSlot == 4);
        std::string error;
        assert(pom2::bootMountedMedia(controller, mounted.bootSlot, 0, error));
        int marker = 0;
        for (long cycles = 0; cycles < 30'000'000 && marker != 0x44; ) {
            cycles += controller.cpu().run(4096);
            marker = controller.memory().peekMainRam(0x0300);
            if (marker == 0x11) break;
        }
        expect(marker, 0x44, "explicit mounted Liron route before higher slot Disk II");
        pom2::SlotCardFactory factory;
        pom2::SlotProvisioningCoordinator provision(factory, storage);
        const auto target = provision.ensureHdvBootTarget(controller, settings, SystemProfile::AppleIIe);
        assert(target && !target.added && target.slot == 4);
        const auto hdMounted = storage.mountHdv(controller, settings, hardDisk, false);
        assert(hdMounted.ok && hdMounted.bootSlot == target.slot && hdMounted.usesSmartPort);
        assert(pom2::bootMountedMedia(controller, hdMounted.bootSlot, 0, error));
        marker = 0;
        for (long cycles = 0; cycles < 30'000'000 && marker != 0x33; ) {
            cycles += controller.cpu().run(4096);
            marker = controller.memory().peekMainRam(0x0300);
            if (marker == 0x11 || marker == 0x44) break;
        }
        expect(marker, 0x33, "provisioned Liron HD route before higher slot Disk II");
        controller.memory().slotBus().plug(7, std::make_unique<ProDOSHardDiskCard>(7));
        const auto blockMounted = storage.mountHdv(controller, settings, smartDisk, false);
        assert(blockMounted.ok && blockMounted.bootSlot == 7 && !blockMounted.usesSmartPort);
        assert(settings.getString("media_slot4_bay0_path") == hardDisk);
    }
    for (const auto& path : {floppy1, floppy2, hardDisk, internal35, external35, smartDisk})
        std::filesystem::remove(path);
    return failures ? 1 : 0;
}
