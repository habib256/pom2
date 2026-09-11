// Eight 3.5" units on a //e's Liron card, enumerated by ProDOS — 2026-09-11.
//
// The //c reached eight SmartPort units on 2026-09-08 (`iic_smartport_six_
// units`): its bank-1 firmware INITs the chain on the rear port and ProDOS
// 8 2.4+ remaps units 3+ onto slots with no disk device. A Liron runs the
// same firmware, byte for byte, from its own EPROM, over the same responder
// (`SmartPortBusDevice`) — but the card had two fixed bays. It takes 2, 4, 6
// or 8 now (`LironCard::setUnitCount`), eight out of the box. This boots
// ProDOS from bay 0 (a scratch copy of A2DeskTop's 800K image) with seven 800K volumes behind it,
// through the card's REAL firmware, and reads DEVLST off the global page:
// every unit on the chain must be a ProDOS device, and none past the count.
//
// Needs roms/apple2e.rom, roms/liron.rom and
// disks_3.5/A2DeskTop-1.5-en_800k.2mg.

#include "Disk35Image.h"
#include "LironCard.h"
#include "M6502.h"
#include "Memory.h"
#include "ProDOSVolume.h"
#include "ResourcePaths.h"
#include "SlotBus.h"
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

// A small synthesised ProDOS volume named VOLn, padded to the 1600 blocks a
// `Disk35Image` insists on. The volume header keeps its own (smaller) block
// count; ProDOS takes the device's size from the SmartPort STATUS, and DEVLST
// is all this test reads.
std::string makeVolume35(int n, const fs::path& scratchDir)
{
    const fs::path folder = scratchDir / ("vol" + std::to_string(n));
    fs::create_directories(folder);
    std::ofstream(folder / "HELLO.TXT") << "unit " << n << "\r";
    std::vector<uint8_t> image;
    const auto r = pom2::buildVolumeFromFolder(folder.string(), "VOL" + std::to_string(n), image);
    if (!r.ok) { fail("buildVolumeFromFolder: " + r.error); return {}; }
    if (image.size() > pom2::Disk35Image::kBytesPerImage) { fail("volume too big for 800K"); return {}; }
    image.resize(pom2::Disk35Image::kBytesPerImage, 0);
    const fs::path po = scratchDir / ("vol" + std::to_string(n) + ".po");
    std::ofstream f(po, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    return po.string();
}

struct Outcome {
    int devcnt = -1;
    std::vector<uint8_t> devlst;
    uint8_t kernelVersion = 0;
    int transactions = 0;
    int blocksRead = 0;
};

Outcome boot(const std::string& rom, const std::string& boot35,
             const std::vector<std::string>& volumes, int unitCount)
{
    Outcome o;
    Memory mem;
    M6502  cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(true);
    mem.clearRam();
    mem.resetSoftSwitches();
    if (!mem.loadAppleIIRom(rom.c_str())) { fail("cannot load apple2e.rom"); return o; }

    auto card = std::make_unique<pom2::LironCard>(5);
    pom2::LironCard* liron = card.get();
    if (!liron->romLoaded()) { fail("Liron: " + liron->lastError()); return o; }
    liron->setUnitCount(unitCount);
    if (liron->bayCount() != unitCount) {
        fail("setUnitCount(" + std::to_string(unitCount) + ") left " +
             std::to_string(liron->bayCount()) + " bays");
        return o;
    }
    std::string err;
    if (!liron->mountBay(0, boot35, err)) { fail("mount bay 0: " + err); return o; }
    for (size_t i = 0; i < volumes.size() && static_cast<int>(i) + 1 < unitCount; ++i)
        if (!liron->mountBay(static_cast<int>(i) + 1, volumes[i], err)) {
            fail("mount bay " + std::to_string(i + 1) + ": " + err);
            return o;
        }
    // A bay past the count is not a bay: the host panels must not be able to
    // put a medium where the guest cannot see it.
    if (unitCount < pom2::LironCard::kMaxUnits &&
        liron->mountBay(unitCount, volumes.back(), err))
        fail("mountBay accepted bay " + std::to_string(unitCount) +
             " on a " + std::to_string(unitCount) + "-unit chain");
    mem.slotBus().plug(5, std::move(card));

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    cpu.setProgramCounter(0xC500);        // what bootFromSlot(5) does on a //e

    // Sampled through the MAIN bank while the boot runs: A2DeskTop switches
    // aux memory in once it is up (see smartport_eight_units).
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
    o.transactions = liron->busProgress().transactions;
    o.blocksRead   = liron->busProgress().blocksRead;
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

// DEVLST entries that are not the /RAM disk (S3,D2) — the Liron's.
int cardDevices(const std::vector<uint8_t>& devlst)
{
    int n = 0;
    for (uint8_t d : devlst)
        if (!(((d >> 4) & 7) == 3 && (d & 0x80))) ++n;
    return n;
}

void expect(const std::string& rom, const std::string& boot35,
            const std::vector<std::string>& volumes, int units)
{
    const Outcome o = boot(rom, boot35, volumes, units);
    std::printf("  //e Liron, %d units: ProDOS $%02X, DEVCNT=%d, DEVLST: %s "
                "(bus transactions %d, blocks read %d)\n",
                units, o.kernelVersion, o.devcnt, describe(o.devlst).c_str(),
                o.transactions, o.blocksRead);
    const int n = cardDevices(o.devlst);
    if (n != units)
        fail("with " + std::to_string(units) + " units on the Liron, ProDOS lists " +
             std::to_string(n) + " card devices");
    else
        std::printf("  ok: %d units on the chain, %d ProDOS devices\n", units, n);
}

}  // namespace

int main()
{
    const std::string rom   = pom2::findResource("roms/apple2e.rom");
    const std::string liron = pom2::findResource("roms/liron.rom");
    const std::string disk  = pom2::findResource("disks_3.5/A2DeskTop-1.5-en_800k.2mg");
    if (rom.empty() || liron.empty() || disk.empty()) {
        std::printf("SKIP liron_eight_units: need roms/apple2e.rom, roms/liron.rom "
                    "and disks_3.5/A2DeskTop-1.5-en_800k.2mg\n");
        return 77;
    }
    const fs::path scratch = fs::path(pom2test::tempPath("pom2_liron8")).parent_path() /
                             "pom2_liron8_volumes";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);
    // The boot disk is a COPY: the tracked image is user media (CLAUDE.md),
    // and a binary run outside ctest mounts writable.
    const fs::path boot35 = scratch / "a2desktop.2mg";
    fs::copy_file(disk, boot35, fs::copy_options::overwrite_existing, ec);
    if (ec) { fail("cannot copy the boot disk: " + ec.message()); return 1; }
    std::vector<std::string> volumes;
    for (int n = 2; n <= 8; ++n) {
        const std::string po = makeVolume35(n, scratch);
        if (po.empty()) return 1;
        volumes.push_back(po);
    }

    expect(rom, boot35.string(), volumes, 8);   // the ceiling
    expect(rom, boot35.string(), volumes, 6);
    expect(rom, boot35.string(), volumes, 2);   // what an older ProDOS can see

    // The count's rules, without a boot: pairs, 2..8.
    {
        pom2::LironCard card(5);
        // The whole chain by default, as the //c's rear port can carry it.
        if (card.bayCount() != 8)
            fail("a fresh Liron has " + std::to_string(card.bayCount()) + " bays, want 8");
        card.setUnitCount(5);  if (card.bayCount() != 6) fail("5 units did not round up to 6");
        card.setUnitCount(0);  if (card.bayCount() != 2) fail("0 units did not clamp to 2");
        card.setUnitCount(99); if (card.bayCount() != 8) fail("99 units did not clamp to 8");
    }

    fs::remove_all(scratch, ec);
    if (g_failures) return 1;
    std::puts("liron_eight_units OK");
    return 0;
}
