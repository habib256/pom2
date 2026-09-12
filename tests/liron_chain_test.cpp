// A //e's Liron card with a full chain, enumerated by ProDOS — 2026-09-11.
//
// The //c reached eight SmartPort units on 2026-09-08 (`iic_smartport_six_
// units`): its bank-1 firmware INITs the chain on the rear port and ProDOS
// 8 2.4+ remaps units 3+ onto slots with no disk device. A Liron runs the
// same firmware, byte for byte, from its own EPROM, over the same responder
// (`SmartPortBusDevice`) — but the card had two fixed 3.5" bays. It takes
// 2 to 14 units now (`LironCard::setUnitCount`), fourteen out of the box,
// and a unit holds a 3.5" 800K image OR a ProDOS hard disk. This boots
// ProDOS through the card's REAL firmware — off a 3.5" and off a hard disk
// in unit 0 — and reads DEVLST off the global page: every unit on the chain
// must be a ProDOS device, and none past the count.
//
// Fourteen is ProDOS 8's own ceiling, measured: the firmware's INIT scan
// numbers sixteen units without complaint, and ProDOS 8 2.4 fills its
// 14-entry device table (one entry, S3,D2, stays /RAM's) and stops.
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

#include <cstddef>
#include <cstdint>
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

// A bootable hard disk: the A2DeskTop volume's 1600 blocks at the head of a
// 4 MiB image — past the 1 MiB line, so the Liron takes it as a hard disk.
// ProDOS boots a volume smaller than its device without complaint.
std::string makeBootHdv(const std::string& boot35, const fs::path& scratchDir)
{
    pom2::Disk35Image img;
    if (!img.loadFile(boot35)) { fail("cannot read the 800K image"); return {}; }
    std::vector<uint8_t> hdv(8192u * 512u, 0);
    for (uint32_t b = 0; b < pom2::Disk35Image::kBlockCount; ++b)
        if (!img.readBlock(b, hdv.data() + b * 512u)) { fail("read block"); return {}; }
    const fs::path out = scratchDir / "boot.hdv";
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(hdv.data()), static_cast<std::streamsize>(hdv.size()));
    return out.string();
}

// A small volume padded to 2 MiB: a hard disk, not a 3.5".
std::string makeVolumeHdv(int n, const fs::path& scratchDir)
{
    const std::string po = makeVolume35(n, scratchDir);
    if (po.empty()) return {};
    std::error_code ec;
    fs::resize_file(po, 2u * 1024u * 1024u, ec);
    if (ec) { fail("resize " + po); return {}; }
    const fs::path hdv = fs::path(po).replace_extension(".hdv");
    fs::rename(po, hdv, ec);
    return ec ? std::string{} : hdv.string();
}

struct Outcome {
    int devcnt = -1;
    std::vector<uint8_t> devlst;
    uint8_t kernelVersion = 0;
    int transactions = 0;
    int blocksRead = 0;
    std::vector<std::string> kinds;   // bayInfo().kindLabel per unit
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
    for (int b = 0; b < unitCount; ++b) o.kinds.push_back(liron->bayInfo(b).kindLabel);
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

Outcome expect(const std::string& rom, const std::string& bootImage,
               const std::vector<std::string>& volumes, int units, int listed)
{
    const Outcome o = boot(rom, bootImage, volumes, units);
    std::printf("  //e Liron, %d units: ProDOS $%02X, DEVCNT=%d, DEVLST: %s "
                "(bus transactions %d, blocks read %d)\n",
                units, o.kernelVersion, o.devcnt, describe(o.devlst).c_str(),
                o.transactions, o.blocksRead);
    const int n = cardDevices(o.devlst);
    if (n != listed)
        fail("with " + std::to_string(units) + " units on the Liron, ProDOS lists " +
             std::to_string(n) + " card devices, want " + std::to_string(listed));
    else
        std::printf("  ok: %d units on the chain, %d ProDOS devices\n", units, n);
    return o;
}

int count(const std::vector<std::string>& kinds, const char* kind)
{
    int n = 0;
    for (const auto& k : kinds) if (k == kind) ++n;
    return n;
}

}  // namespace

int main()
{
    const std::string rom   = pom2::findResource("roms/apple2e.rom");
    const std::string liron = pom2::findResource("roms/liron.rom");
    const std::string disk  = pom2::findResource("disks_3.5/A2DeskTop-1.5-en_800k.2mg");
    if (rom.empty() || liron.empty() || disk.empty()) {
        std::printf("SKIP liron_chain: need roms/apple2e.rom, roms/liron.rom "
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
    // Thirteen volumes for units 1-13: 3.5" and hard disks alternating.
    std::vector<std::string> volumes;
    for (int n = 2; n <= 14; ++n) {
        const std::string v = (n & 1) ? makeVolumeHdv(n, scratch) : makeVolume35(n, scratch);
        if (v.empty()) return 1;
        volumes.push_back(v);
    }
    const std::string bootHdv = makeBootHdv(boot35.string(), scratch);
    if (bootHdv.empty()) return 1;

    // The whole chain, booted off a HARD DISK in unit 0: every unit is a
    // device up to ProDOS's table — thirteen here, /RAM keeps the fourteenth.
    {
        const Outcome o = expect(rom, bootHdv, volumes, 14, 13);
        if (o.kinds.empty() || o.kinds[0] != "ProDOS HDV")
            fail("unit 0 did not mount the 4 MiB image as a hard disk");
        if (count(o.kinds, "ProDOS HDV") != 7 || count(o.kinds, "3.5\" 800K") != 7)
            fail("the chain is not seven hard disks and seven 3.5\" disks");
        if (o.blocksRead == 0) fail("nothing was read over the bus from the hard disk");
    }
    // Twelve fit whole; eight off a 3.5"; two, what an older ProDOS can see.
    expect(rom, bootHdv, volumes, 12, 12);
    expect(rom, boot35.string(), volumes, 8, 8);
    expect(rom, boot35.string(), volumes, 2, 2);

    // A unit is whatever disk is put in it, and a failed mount keeps the old.
    {
        pom2::LironCard card(5);
        std::string err;
        if (!card.mountBay(3, volumes[0], err)) fail("mount a 3.5\" in unit 3: " + err);
        if (card.bayInfo(3).kindLabel != "3.5\" 800K") fail("unit 3 is not a 3.5\"");
        if (!card.mountBay(3, volumes[1], err)) fail("mount a hard disk over it: " + err);
        if (card.bayInfo(3).kindLabel != "ProDOS HDV" || card.bayInfo(3).path != volumes[1])
            fail("the hard disk did not replace the 3.5\" in unit 3");
        if (card.blockBackings().size() != 14 || !card.blockBackings()[3]->isLoaded())
            fail("unit 3's hard disk is not offered to the background autosave");
        if (card.mountBay(3, (scratch / "no-such.hdv").string(), err))
            fail("a missing file mounted");
        if (card.bayInfo(3).path != volumes[1])
            fail("a failed mount dropped the disk that was in the unit");
        if (!card.mountBay(3, volumes[0], err) || card.bayInfo(3).kindLabel != "3.5\" 800K" ||
            card.blockBackings()[3]->isLoaded())
            fail("a 3.5\" did not replace the hard disk in unit 3");
        if (!card.ejectBay(3) || card.bayInfo(3).loaded) fail("eject unit 3");
    }

    // The count's rules, without a boot: pairs, 2..14.
    {
        pom2::LironCard card(5);
        // The whole chain by default.
        if (card.bayCount() != 14)
            fail("a fresh Liron has " + std::to_string(card.bayCount()) + " bays, want 14");
        card.setUnitCount(5);  if (card.bayCount() != 6)  fail("5 units did not round up to 6");
        card.setUnitCount(0);  if (card.bayCount() != 2)  fail("0 units did not clamp to 2");
        card.setUnitCount(99); if (card.bayCount() != 14) fail("99 units did not clamp to 14");
    }

    fs::remove_all(scratch, ec);
    if (g_failures) return 1;
    std::puts("liron_chain OK");
    return 0;
}
