// Eight SmartPort units, enumerated by ProDOS — 2026-09-08.
//
// A modern SmartPort controller (A2retroNET on an A2Pico) presents up to
// eight 32 MB volumes; ProDOS 8 2.4+ enumerates units 3+ through the
// SmartPort STATUS call and remaps them onto empty slots. POM2's SmartPort
// card grew from two bays to eight with a configurable unit count. This
// boots ProDOS from unit 0 (A2DeskTop's 800K image) with seven synthesised
// volumes behind it and reads DEVLST off ProDOS's global page: every unit
// the card answers for must be a device, and none beyond the count.
//
// Needs roms/apple2e.rom, roms/liron.rom (the card presents $Cn07=$00 —
// SmartPort class — only on the real dump; a $01 block device is never
// enumerated past drive 2) and disks_3.5/A2DeskTop-1.5-en_800k.2mg.

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

std::string scrapeTextPage(Memory& mem)
{
    static const int rowBase[24] = {
        0x400,0x480,0x500,0x580,0x600,0x680,0x700,0x780,
        0x428,0x4A8,0x528,0x5A8,0x628,0x6A8,0x728,0x7A8,
        0x450,0x4D0,0x550,0x5D0,0x650,0x6D0,0x750,0x7D0 };
    std::string s;
    for (int r = 0; r < 24; ++r) {
        for (int c = 0; c < 40; ++c) {
            const uint8_t b = mem.memRead(static_cast<uint16_t>(rowBase[r] + c)) & 0x7F;
            s += (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : ' ';
        }
        s += '\n';
    }
    return s;
}

// A small synthesised ProDOS volume named VOLn, written to a scratch .po.
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
    std::string screen;
};

Outcome boot(const std::string& rom, const std::string& liron,
             const std::string& disk35, const std::vector<std::string>& volumes,
             int unitCount)
{
    Outcome o;
    Memory mem;
    M6502  cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(true);
    mem.clearRam();
    mem.resetSoftSwitches();
    if (!mem.loadAppleIIRom(rom.c_str())) { fail("cannot load apple2e.rom"); return o; }

    auto card = std::make_unique<pom2::SmartPortCard>(5);
    if (!card->loadLironRom(liron)) { fail("cannot load roms/liron.rom"); return o; }
    card->setUnitCount(unitCount);
    auto u0 = std::make_unique<pom2::SmartPort35Unit>();
    if (!u0->loadImage(disk35)) { fail("cannot load A2DeskTop 2mg"); return o; }
    card->setUnit(0, std::move(u0));
    for (size_t i = 0; i < volumes.size(); ++i) {
        auto u = std::make_unique<pom2::SmartPortHdvUnit>();
        if (!u->loadImage(volumes[i])) { fail("cannot load " + volumes[i]); return o; }
        card->setUnit(i + 1, std::move(u));
    }
    mem.slotBus().plug(5, std::move(card));

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    cpu.setProgramCounter(0xC500);        // what bootFromSlot(5) does on a //e

    // Sample ProDOS's global page through the MAIN bank as the boot runs:
    // A2DeskTop switches the //e's aux memory in once it is up, so a plain
    // memRead of $BF31 after the fact reads aux RAM (all zeros). The last
    // sample with the MLI entry in place ($BF00 = JMP) is the one kept.
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
    constexpr long kBudget = 40'000'000;
    for (long total = 0; total < kBudget; ) {
        total += cpu.run(4096);
        if ((total % (1 << 18)) < 4096) sample();
    }
    sample();
    o.screen = scrapeTextPage(mem);
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

// Count DEVLST entries that are not the /RAM disk (S3,D2) — the card's.
int cardDevices(const std::vector<uint8_t>& devlst)
{
    int n = 0;
    for (uint8_t d : devlst)
        if (!(((d >> 4) & 7) == 3 && (d & 0x80))) ++n;
    return n;
}

}  // namespace

int main()
{
    const std::string rom   = pom2::findResource("roms/apple2e.rom");
    const std::string liron = pom2::findResource("roms/liron.rom");
    const std::string disk  = pom2::findResource("disks_3.5/A2DeskTop-1.5-en_800k.2mg");
    if (rom.empty() || liron.empty() || disk.empty()) {
        std::printf("SKIP smartport_eight_units: need roms/apple2e.rom, roms/liron.rom "
                    "and disks_3.5/A2DeskTop-1.5-en_800k.2mg\n");
        return 77;
    }
    const fs::path scratch = fs::path(pom2test::tempPath("pom2_sp8")).parent_path() / "pom2_sp8_volumes";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);
    std::vector<std::string> volumes;
    for (int n = 2; n <= 8; ++n) {
        const std::string po = makeVolume(n, scratch);
        if (po.empty()) return 1;
        volumes.push_back(po);
    }

    // Eight units answered for: ProDOS must list eight card devices.
    {
        const Outcome o = boot(rom, liron, disk, volumes, 8);
        std::printf("  ProDOS kernel version byte $%02X; DEVCNT=%d; DEVLST: %s\n",
                    o.kernelVersion, o.devcnt, describe(o.devlst).c_str());
        const int n = cardDevices(o.devlst);
        if (n != 8) {
            fail("with unitCount 8, ProDOS lists " + std::to_string(n) +
                 " card devices, expected 8 (screen follows)");
            std::printf("%s\n", o.screen.c_str());
        } else {
            std::printf("  ok: eight units, eight ProDOS devices\n");
        }
    }
    // Two answered for (the default): drive 1 / 2 only, the other bays
    // loaded but invisible — a saved configuration sees what it always did.
    {
        const Outcome o = boot(rom, liron, disk, volumes, 2);
        std::printf("  DEVCNT=%d; DEVLST: %s\n", o.devcnt, describe(o.devlst).c_str());
        const int n = cardDevices(o.devlst);
        if (n != 2) fail("with unitCount 2, ProDOS lists " + std::to_string(n) + " card devices, expected 2");
        else std::printf("  ok: two units by default, two ProDOS devices\n");
    }
    fs::remove_all(scratch, ec);
    if (g_failures) return 1;
    std::puts("smartport_eight_units OK");
    return 0;
}
