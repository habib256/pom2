// Six — and eight — SmartPort units behind the //c's rear port — 2026-09-08.
//
// On a //c there is no slot: the built-in slot-5 SmartPort card's units are
// served to the machine's own firmware over the disk port, as intelligent
// UniDisks on the SmartPort BUS (IIcExternalSmartPort / SmartPortBusDevice).
// The bus responder took four units; the card answers for eight since this
// morning. This boots ProDOS 8 2.4.3 from the internal 5.25" with the card
// set to six units (two 3.5", four HDV) and reads DEVLST off the global
// page: the //c firmware's INIT scan numbers the chain, ProDOS enumerates it
// through SmartPort STATUS and remaps units 3+ onto empty slots. How many
// the //c ROM's own device table takes is what this measures — the
// assertion is the count the firmware proved it can hold.
//
// Needs roms/apple2c-32Kv0.rom, disks_5.4/dsk/ProDOS_2_4_3.po and
// disks_3.5/A2DeskTop-1.5-en_800k.2mg.

#include "DiskIICard.h"
#include "IIcExternalSmartPort.h"
#include "IWMDevice.h"
#include "M6502.h"
#include "Memory.h"
#include "ProDOSVolume.h"
#include "ResourcePaths.h"
#include "SlotBus.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "TestTempPath.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
void fail(const std::string& what) { std::printf("FAIL: %s\n", what.c_str()); ++g_failures; }

std::string makeVolume(int n, const fs::path& scratchDir)
{
    const fs::path folder = scratchDir / ("vol" + std::to_string(n));
    fs::create_directories(folder);
    std::ofstream(folder / "HELLO.TXT") << "unit " << n << "\r";
    std::vector<uint8_t> image;
    const auto r = pom2::buildVolumeFromFolder(folder.string(), "VOL" + std::to_string(n), image);
    if (!r.ok) { fail("buildVolumeFromFolder: " + r.error); return {}; }
    const fs::path po = scratchDir / ("vol" + std::to_string(n) + ".po");
    std::ofstream f(po, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    return po.string();
}

struct Outcome {
    int devcnt = -1;
    std::vector<uint8_t> devlst;
    uint8_t kernelVersion = 0;
    int busTransactions = 0;
};

Outcome boot(const std::string& rom, const std::string& disk525,
             const std::string& disk35, const std::vector<std::string>& hdvs,
             int unitCount, const fs::path& scratch)
{
    Outcome o;
    Memory mem;
    M6502  cpu(&mem);
    pom2::IWMDevice iwm;
    mem.setCpu(&cpu);
    mem.setIWM(&iwm);
    mem.setIWMAuthoritative(true);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    pom2::IIcExternalSmartPort port(&mem.slotBus());
    mem.setExternalSmartPort(&port);
    if (!mem.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/true)) { fail("cannot load the //c ROM"); return o; }

    auto d2 = std::make_unique<DiskIICard>(6);
    const std::string bootRom = pom2::findResource("roms/disk2.rom");
    const std::string lssRom  = pom2::findResource("roms/diskii_p6.rom");
    if (!bootRom.empty()) d2->loadBootRom(bootRom);
    if (!lssRom.empty())  d2->loadLssRom(lssRom);
    const fs::path scratch525 = scratch / "prodos_2_4_3.po";
    std::error_code ec;
    fs::copy_file(disk525, scratch525, fs::copy_options::overwrite_existing, ec);
    if (ec || !d2->insertDisk(0, scratch525.string())) { fail("cannot insert ProDOS 2.4.3"); return o; }
    d2->setIWM(&iwm);
    mem.slotBus().plug(6, std::move(d2));

    auto sp = std::make_unique<pom2::SmartPortCard>(5);
    sp->setUnitCount(unitCount);
    auto u0 = std::make_unique<pom2::SmartPort35Unit>();
    if (!u0->loadImage(disk35)) { fail("cannot load the 800K image"); return o; }
    sp->setUnit(0, std::move(u0));
    sp->setUnit(1, std::make_unique<pom2::SmartPort35Unit>());
    for (size_t i = 0; i < hdvs.size(); ++i) {
        auto u = std::make_unique<pom2::SmartPortHdvUnit>();
        if (!u->loadImage(hdvs[i])) { fail("cannot load " + hdvs[i]); return o; }
        sp->setUnit(2 + i, std::move(u));
    }
    mem.slotBus().plug(5, std::move(sp));

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();

    auto sample = [&] {
        if (mem.peekMainRam(0xBF00) != 0x4C) return;
        const uint8_t devcnt = mem.peekMainRam(0xBF31);
        if (devcnt >= 14) return;
        o.devcnt = devcnt;
        o.kernelVersion = mem.peekMainRam(0xBFFF);
        o.devlst.clear();
        for (int i = 0; i <= devcnt; ++i)
            o.devlst.push_back(mem.peekMainRam(static_cast<uint16_t>(0xBF32 + i)));
    };
    constexpr long kBudget = 60'000'000;
    for (long total = 0; total < kBudget; ) {
        total += cpu.run(4096);
        if ((total % (1 << 18)) < 4096) sample();
    }
    sample();
    o.busTransactions = port.device().progress().transactions;
    return o;
}

std::string describe(const std::vector<uint8_t>& devlst)
{
    std::string s;
    for (uint8_t d : devlst) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "S%dD%d ", (d >> 4) & 7, (d & 0x80) ? 2 : 1);
        s += buf;
    }
    return s;
}

// The card's devices: everything but the internal 5.25" (S6) and /RAM (S3D2).
int cardDevices(const std::vector<uint8_t>& devlst)
{
    int n = 0;
    for (uint8_t d : devlst) {
        const int slot = (d >> 4) & 7;
        const bool dr2 = (d & 0x80) != 0;
        if (slot == 6) continue;
        if (slot == 3 && dr2) continue;
        ++n;
    }
    return n;
}

}  // namespace

int main()
{
    const std::string rom     = pom2::findResource("roms/apple2c-32Kv0.rom");
    const std::string disk525 = pom2::findResource("disks_5.4/dsk/ProDOS_2_4_3.po");
    const std::string disk35  = pom2::findResource("disks_3.5/A2DeskTop-1.5-en_800k.2mg");
    if (rom.empty() || disk525.empty() || disk35.empty()) {
        std::printf("SKIP iic_smartport_six_units: need roms/apple2c-32Kv0.rom, "
                    "disks_5.4/dsk/ProDOS_2_4_3.po and disks_3.5/A2DeskTop-1.5-en_800k.2mg\n");
        return 77;
    }
    const fs::path scratch = fs::path(pom2test::tempPath("pom2_iic6")).parent_path() / "pom2_iic6_volumes";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);
    std::vector<std::string> hdvs;
    for (int n = 3; n <= 8; ++n) {
        const std::string po = makeVolume(n, scratch);
        if (po.empty()) return 1;
        hdvs.push_back(po);
    }

    // Eight — the card's ceiling: two 3.5", six HDV. What the //c ROM and
    // ProDOS 2.4.3 make of a chain that long is the measurement.
    {
        const Outcome o = boot(rom, disk525, disk35, hdvs, 8, scratch);
        std::printf("  //c, eight units: ProDOS $%02X, DEVCNT=%d, DEVLST: %s (bus transactions %d)\n",
                    o.kernelVersion, o.devcnt, describe(o.devlst).c_str(), o.busTransactions);
        const int n = cardDevices(o.devlst);
        if (n != 8) fail("the //c lists " + std::to_string(n) + " SmartPort devices with eight units, expected 8");
        else std::printf("  ok: eight SmartPort units on the //c's rear port, eight ProDOS devices\n");
    }

    const std::vector<std::string> six(hdvs.begin(), hdvs.begin() + 4);
    const Outcome o = boot(rom, disk525, disk35, six, 6, scratch);
    std::printf("  //c, six units: ProDOS $%02X, DEVCNT=%d, DEVLST: %s (bus transactions %d)\n",
                o.kernelVersion, o.devcnt, describe(o.devlst).c_str(), o.busTransactions);
    const int n = cardDevices(o.devlst);
    if (n != 6) fail("the //c lists " + std::to_string(n) + " SmartPort devices with six units, expected 6");
    else std::printf("  ok: six SmartPort units on the //c's rear port, six ProDOS devices\n");

    const Outcome two = boot(rom, disk525, disk35, hdvs, 2, scratch);
    std::printf("  //c, two units: DEVCNT=%d, DEVLST: %s\n", two.devcnt, describe(two.devlst).c_str());
    const int n2 = cardDevices(two.devlst);
    if (n2 != 2) fail("the //c lists " + std::to_string(n2) + " SmartPort devices with two units, expected 2");
    else std::printf("  ok: two by default\n");

    fs::remove_all(scratch, ec);
    if (g_failures) return 1;
    std::puts("iic_smartport_six_units OK");
    return 0;
}
