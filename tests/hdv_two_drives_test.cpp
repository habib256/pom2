// The ProDOS HD card's second drive — 2026-09-11.
//
// A plain ProDOS block device ($Cn07 = $01) names a unit by slot and ONE
// drive bit — bit 7 of $43 — so two drives is its ceiling (the AppleWin HDD
// card's HDD1 / HDD2); eight is a SmartPort card's business. The card had
// one. Its driver entry now latches ProDOS's unit byte into $C0n6 before
// dispatching, and $CnFE advertises two volumes ($17).
//
// Part 1 runs the card's own ROM driver, no ProDOS: STATUS / READ / WRITE
// with $43 = $50 (drive 1) and $D0 (drive 2) must each reach their own image,
// and an empty drive 2 must answer $28. Part 2 boots ProDOS off drive 1 with
// a volume in drive 2 and reads DEVLST: S5,D1 and S5,D2 must both be there.
// Part 2 needs roms/apple2e.rom and disks_3.5/A2DeskTop-1.5-en_800k.2mg and
// SKIPs without them; part 1 needs nothing.

#include "M6502.h"
#include "Memory.h"
#include "ProDOSHardDiskCard.h"
#include "ProDOSVolume.h"
#include "ResourcePaths.h"
#include "SlotBus.h"
#include "TestTempPath.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int      kSlot  = 5;
constexpr size_t   kBlk   = ProDOSHardDiskCard::kBlockBytes;
constexpr uint8_t  kRomHi = 0xC0 + kSlot;
constexpr uint8_t  kUnitD1 = kSlot << 4;            // $50
constexpr uint8_t  kUnitD2 = 0x80 | (kSlot << 4);   // $D0

int g_failures = 0;
void fail(const std::string& what) { std::printf("FAIL: %s\n", what.c_str()); ++g_failures; }

void callDriver(M6502& cpu, Memory& mem, uint8_t cmd, uint8_t unit,
                uint16_t block, uint16_t buffer)
{
    mem.memWrite(0x42, cmd);
    mem.memWrite(0x43, unit);
    mem.memWrite(0x44, static_cast<uint8_t>(buffer & 0xFF));
    mem.memWrite(0x45, static_cast<uint8_t>(buffer >> 8));
    mem.memWrite(0x46, static_cast<uint8_t>(block & 0xFF));
    mem.memWrite(0x47, static_cast<uint8_t>(block >> 8));
    const uint8_t entry = mem.memRead(static_cast<uint16_t>((kRomHi << 8) | 0xFF));
    mem.memWrite(0x0300, 0x20);              // JSR $Cn<entry>
    mem.memWrite(0x0301, entry);
    mem.memWrite(0x0302, kRomHi);
    mem.memWrite(0x0303, 0x4C);              // JMP $0303 (park)
    mem.memWrite(0x0304, 0x03);
    mem.memWrite(0x0305, 0x03);
    cpu.setProgramCounter(0x0300);
    cpu.run(60000);
}

bool carrySet(const M6502& cpu) { return (cpu.getStatusRegister() & 0x01) != 0; }

std::vector<uint8_t> patterned(size_t blocks, uint8_t seed)
{
    std::vector<uint8_t> v(blocks * kBlk);
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = static_cast<uint8_t>((i * 7u + seed) & 0xFF);
    return v;
}

void partDriver()
{
    Memory mem;
    M6502 cpu(&mem);
    cpu.hardReset();
    auto card = std::make_unique<ProDOSHardDiskCard>(kSlot);
    ProDOSHardDiskCard* hdv = card.get();
    if (hdv->romLayoutError()) { fail("the slot ROM did not fit its declared layout"); return; }
    mem.slotBus().plug(kSlot, std::move(card));

    if (mem.memRead(static_cast<uint16_t>((kRomHi << 8) | 0xFE)) != 0x17)
        fail("$CnFE does not advertise two volumes ($17)");
    if (hdv->bayCount() != 2) fail("the card does not have two bays");

    // Drive 1 only. Drive 2 is empty: STATUS there is "no device".
    const auto d1 = patterned(5, 0x11);
    if (!hdv->loadImageFromBytes(d1, "drive-1")) { fail("drive 1 load"); return; }
    callDriver(cpu, mem, 0x00, kUnitD2, 0, 0x0800);
    if (!carrySet(cpu) || cpu.getAccumulator() != 0x28)
        fail("STATUS on an empty drive 2 did not return $28");
    callDriver(cpu, mem, 0x00, kUnitD1, 0, 0x0800);
    if (carrySet(cpu) || cpu.getXRegister() != 5)
        fail("STATUS on drive 1 lost its block count after a drive-2 call");

    // Drive 2 gets its own image, a different size and pattern.
    const fs::path d2path = fs::path(pom2test::tempPath("pom2_hdv_d2")).replace_extension(".hdv");
    {
        const auto d2 = patterned(9, 0x5C);
        std::ofstream f(d2path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(d2.data()), static_cast<std::streamsize>(d2.size()));
    }
    std::string err;
    if (!hdv->mountBay(1, d2path.string(), err)) { fail("mount drive 2: " + err); return; }
    if (hdv->isImageLoaded() == false || hdv->getImagePath() != "drive-1")
        fail("mounting drive 2 disturbed drive 1");

    callDriver(cpu, mem, 0x00, kUnitD2, 0, 0x0800);
    if (carrySet(cpu) || cpu.getXRegister() != 9 || cpu.getYRegister() != 0)
        fail("STATUS on drive 2 did not report its 9 blocks");

    // READ block 3 from each drive: each must come from its own image.
    callDriver(cpu, mem, 0x01, kUnitD2, 3, 0x2000);
    if (carrySet(cpu)) fail("READ on drive 2 returned an error");
    callDriver(cpu, mem, 0x01, kUnitD1, 3, 0x2400);
    if (carrySet(cpu)) fail("READ on drive 1 returned an error");
    const auto d2img = patterned(9, 0x5C);
    for (size_t i = 0; i < kBlk; ++i) {
        if (mem.memRead(static_cast<uint16_t>(0x2000 + i)) != d2img[3 * kBlk + i]) {
            fail("drive 2's block 3 is not drive 2's data"); break;
        }
    }
    for (size_t i = 0; i < kBlk; ++i) {
        if (mem.memRead(static_cast<uint16_t>(0x2400 + i)) != d1[3 * kBlk + i]) {
            fail("drive 1's block 3 is not drive 1's data"); break;
        }
    }

    // Past the end of drive 1 (5 blocks) but inside drive 2 (9): the range
    // check must ask the drive that was named.
    callDriver(cpu, mem, 0x01, kUnitD2, 7, 0x2000);
    if (carrySet(cpu)) fail("READ of drive 2's block 7 refused — range checked against drive 1");
    callDriver(cpu, mem, 0x01, kUnitD1, 7, 0x2000);
    if (!carrySet(cpu) || cpu.getAccumulator() != 0x27)
        fail("READ of drive 1's block 7 (past its end) did not return $27");

    // WRITE to drive 2 lands on drive 2 and nowhere else.
    for (size_t i = 0; i < kBlk; ++i) mem.memWrite(static_cast<uint16_t>(0x3000 + i), 0xA5);
    callDriver(cpu, mem, 0x02, kUnitD2, 1, 0x3000);
    if (carrySet(cpu)) fail("WRITE on drive 2 returned an error");
    if (hdv->backing(1).readByte(1 * kBlk + 17) != 0xA5)
        fail("drive 2's WRITE did not reach drive 2");
    if (hdv->backing(0).readByte(1 * kBlk + 17) != d1[1 * kBlk + 17])
        fail("drive 2's WRITE landed on drive 1");
    if (!hdv->bayInfo(1).hasUnsavedChanges || hdv->bayInfo(0).hasUnsavedChanges)
        fail("the dirty flag is on the wrong drive");

    // Rewind state carries the latched drive.
    std::vector<uint8_t> blob;
    hdv->appendSnapshotState(blob);
    callDriver(cpu, mem, 0x00, kUnitD1, 0, 0x0800);
    if (hdv->selectedDrive() != 0) fail("the driver entry did not latch drive 1");
    hdv->loadSnapshotState(blob.data(), blob.size());
    if (hdv->selectedDrive() != 1) fail("a snapshot taken on drive 2 restored drive 1");
    const uint8_t v1[8] = { 'H', 'D', 'V', '1', 0, 0, 0, 0 };
    hdv->loadSnapshotState(v1, sizeof v1);
    if (hdv->selectedDrive() != 0) fail("an HDV1 blob (no drive) did not restore drive 1");

    // Eject drive 2 leaves drive 1 mounted.
    if (!hdv->ejectBay(1)) fail("eject drive 2");
    if (!hdv->isImageLoaded()) fail("ejecting drive 2 ejected drive 1");
    std::error_code ec;
    fs::remove(d2path, ec);
    if (g_failures == 0) std::printf("  ok: driver reaches drive 1 and drive 2 by $43 bit 7\n");
}

std::string makeVolume(const fs::path& dir)
{
    const fs::path folder = dir / "vol2";
    fs::create_directories(folder);
    std::ofstream(folder / "HELLO.TXT") << "drive 2\r";
    std::vector<uint8_t> image;
    const auto r = pom2::buildVolumeFromFolder(folder.string(), "DRIVE2", image);
    if (!r.ok) { fail("buildVolumeFromFolder: " + r.error); return {}; }
    const fs::path po = dir / "drive2.po";
    std::ofstream f(po, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    return po.string();
}

int partBoot()
{
    const std::string rom  = pom2::findResource("roms/apple2e.rom");
    const std::string disk = pom2::findResource("disks_3.5/A2DeskTop-1.5-en_800k.2mg");
    if (rom.empty() || disk.empty()) {
        std::printf("  SKIP boot part: need roms/apple2e.rom and "
                    "disks_3.5/A2DeskTop-1.5-en_800k.2mg\n");
        return 77;
    }
    const fs::path scratch = fs::path(pom2test::tempPath("pom2_hdv2")).parent_path() / "pom2_hdv2_volumes";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);
    const fs::path boot = scratch / "a2desktop.2mg";     // a copy: user media
    fs::copy_file(disk, boot, fs::copy_options::overwrite_existing, ec);
    if (ec) { fail("cannot copy the boot disk"); return 1; }
    const std::string vol2 = makeVolume(scratch);
    if (vol2.empty()) return 1;

    Memory mem;
    M6502  cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(true);
    mem.clearRam();
    mem.resetSoftSwitches();
    if (!mem.loadAppleIIRom(rom.c_str())) { fail("cannot load apple2e.rom"); return 1; }
    auto card = std::make_unique<ProDOSHardDiskCard>(kSlot);
    ProDOSHardDiskCard* hdv = card.get();
    std::string err;
    if (!hdv->mountBay(0, boot.string(), err)) { fail("mount drive 1: " + err); return 1; }
    if (!hdv->mountBay(1, vol2, err))          { fail("mount drive 2: " + err); return 1; }
    mem.slotBus().plug(kSlot, std::move(card));

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    cpu.setProgramCounter(0xC500);           // what bootFromSlot(5) does on a //e

    int devcnt = -1;
    std::vector<uint8_t> devlst;
    auto sample = [&] {
        if (mem.peekMainRam(0xBF00) != 0x4C) return;
        const uint8_t n = mem.peekMainRam(0xBF31);
        if (n >= 14) return;
        devcnt = n;
        devlst.clear();
        for (int i = 0; i <= n; ++i)
            devlst.push_back(mem.peekMainRam(static_cast<uint16_t>(0xBF32 + i)));
    };
    for (long total = 0; total < 40'000'000; ) {
        total += cpu.run(4096);
        if ((total % (1 << 18)) < 4096) sample();
    }
    sample();
    std::string list;
    bool d1 = false, d2 = false;
    for (uint8_t d : devlst) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "S%dD%d ", (d >> 4) & 7, (d & 0x80) ? 2 : 1);
        list += buf;
        if ((d & 0x70) == (kSlot << 4)) (d & 0x80 ? d2 : d1) = true;
    }
    std::printf("  ProDOS off drive 1: DEVCNT=%d, DEVLST: %s\n", devcnt, list.c_str());
    if (!d1) fail("ProDOS did not list S5,D1");
    if (!d2) fail("ProDOS did not list S5,D2 — the card's second drive");
    if (d1 && d2) std::printf("  ok: S5,D1 and S5,D2 are both ProDOS devices\n");
    fs::remove_all(scratch, ec);
    return 0;
}

}  // namespace

int main()
{
    partDriver();
    const int boot = partBoot();
    if (g_failures) return 1;
    if (boot == 77) return 77;
    std::puts("hdv_two_drives OK");
    return 0;
}
