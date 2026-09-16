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

// On a //c, a read from the internal 5.25" right after a write to the
// SmartPort must still find the drive.
//
// Reported by a2filecmd (2026-09-16): a BASIC program on the `iic` preset
// copied a text file from the internal drive to an HDV on the SmartPort,
// both files open, READ and WRITE alternating. It stopped after 496 bytes
// with NO DEVICE CONNECTED ($28) — the moment BASIC.SYSTEM needed the
// floppy's second block, i.e. the first floppy read after a SmartPort
// write. The same program on a //e with an HDV card copied all 4 960.
//
// The mechanism is two ordinary facts meeting:
//   * the //c's SmartPort firmware talks to the rear port as DRIVE 2 of the
//     IWM and leaves drive 2 selected when it is done — correct, it is the
//     same chip;
//   * ProDOS's Disk II driver turns the motor on BEFORE it re-selects drive
//     1 ($D05F, then $D065).
// So the motor comes on over drive 2, which on this machine holds nothing.
// `DiskIICard` ran its sequencer reset (`lssStart`) on motor-on only when
// the selected drive had MEDIA; MAME runs it whenever the selected drive
// EXISTS (`wozfdc.cpp` case 0x9, `if(floppy) lss_start();`). Skipped, drive
// 1 then spun on a sequencer state that should have been cleared and
// ProDOS's presence check said the drive was not there. A disk in drive 2
// hid it completely, which is why it needed this exact machine to show.
//
// What this pins, on the real //c firmware and ProDOS 2.4.3 + BASIC.SYSTEM:
//   1. the SmartPort is really in play — ProDOS lists slot 5 (without that
//      the copy would fail with PATH NOT FOUND and prove nothing);
//   2. the copy completes with no error on screen;
//   3. the file on the HDV is the whole file, BYTE FOR BYTE — not a size
//      that happens to match.
// Drive 2 stays EMPTY throughout: that is the trigger.
//
// Then two media changes in the middle of a SmartPort WRITE (bug hunt
// 2026-09-16). The bus used to abort the transaction on ANY change of which
// bays held media, which broke the handshake: the firmware's receive loop
// (`$CA02: LDA $C08C,X / BPL`) waited for a byte that never came and the //c
// hung for good. Now:
//   4. a disk going into the OTHER bay leaves the write alone — the copy
//      completes, byte for byte;
//   5. ejecting the disk BEING written answers the guest with an error it can
//      report (I/O ERROR), and the bus goes idle — no hang.
//
// Skips (77) without the 32 KB //c ROM or the ProDOS 2.4.3 disk.

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

constexpr int kLines     = 40;
constexpr int kXs        = 120;
constexpr int kFileBytes = kLines * (3 + kXs + 1);   // 4 960

std::string screen(Memory& mem)
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

void run(M6502& cpu, long cycles)
{
    for (long n = 0; n < cycles;) n += cpu.run(4096);
}

bool waitFor(Memory& mem, M6502& cpu, const char* text, long budget)
{
    for (long n = 0; n < budget; n += 1'000'000) {
        run(cpu, 1'000'000);
        if (screen(mem).find(text) != std::string::npos) return true;
    }
    return false;
}

// A root-directory file off a ProDOS block device: its bytes, or empty with
// `err` set. Seedling and sapling only — all this test ever writes.
std::vector<uint8_t> readProdosFile(pom2::SmartPortUnit& u, const char* name,
                                    std::string& err)
{
    uint8_t blk[512];
    const std::size_t nameLen = std::strlen(name);
    for (uint32_t bn = 2; bn != 0; ) {
        if (!u.readBlock(bn, blk)) { err = "directory read failed"; return {}; }
        for (int i = 0; i < 13; ++i) {
            const uint8_t* e = blk + 4 + i * 39;
            const int storage = e[0] >> 4;
            if (storage == 0 || storage >= 0xE) continue;
            if (static_cast<std::size_t>(e[0] & 0x0F) != nameLen ||
                std::memcmp(e + 1, name, nameLen) != 0) continue;
            const uint32_t key = static_cast<uint32_t>(e[0x11] | e[0x12] << 8);
            const std::size_t eof = static_cast<std::size_t>(
                e[0x15] | e[0x16] << 8 | e[0x17] << 16);
            std::vector<uint8_t> out;
            if (storage == 1) {
                if (!u.readBlock(key, blk)) { err = "seedling read failed"; return {}; }
                out.assign(blk, blk + 512);
            } else if (storage == 2) {
                uint8_t idx[512];
                if (!u.readBlock(key, idx)) { err = "index read failed"; return {}; }
                for (int k = 0; out.size() < eof && k < 256; ++k) {
                    const uint32_t d = static_cast<uint32_t>(idx[k] | idx[256 + k] << 8);
                    if (d == 0) { out.insert(out.end(), 512, 0); continue; }   // sparse
                    if (!u.readBlock(d, blk)) { err = "data read failed"; return {}; }
                    out.insert(out.end(), blk, blk + 512);
                }
            } else {
                err = "unexpected storage type " + std::to_string(storage);
                return {};
            }
            out.resize(eof);
            return out;
        }
        bn = static_cast<uint32_t>(blk[2] | blk[3] << 8);
    }
    err = std::string(name) + " not found";
    return {};
}

enum class Scenario { Plain, MountOtherBay, EjectTargetBay };

const char* scenarioName(Scenario sc)
{
    switch (sc) {
    case Scenario::Plain:          return "plain copy";
    case Scenario::MountOtherBay:  return "mount in the other bay mid-WRITE";
    case Scenario::EjectTargetBay: return "eject the bay being written mid-WRITE";
    }
    return "?";
}

int runScenario(Scenario sc, const std::string& rom, const std::string& po)
{
    const char* tag = scenarioName(sc);
    std::error_code ec;
    const fs::path scratch = fs::temp_directory_path() / "pom2_iic_diskii_after_sp";
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch / "empty", ec);
    const fs::path floppy = scratch / "prodos.po";
    fs::copy_file(po, floppy, fs::copy_options::overwrite_existing, ec);
    if (ec) { std::fprintf(stderr, "cannot copy the ProDOS disk\n"); return 1; }

    // /SCRATCH: an empty ProDOS volume with room to write in (the builder
    // leaves at least 64 free blocks).
    std::vector<uint8_t> vol;
    const auto built = pom2::buildVolumeFromFolder((scratch / "empty").string(),
                                                   "SCRATCH", vol);
    if (!built.ok) { std::fprintf(stderr, "volume: %s\n", built.error.c_str()); return 1; }
    const fs::path hdv = scratch / "scratch.hdv";
    {
        std::ofstream f(hdv, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(vol.data()),
                static_cast<std::streamsize>(vol.size()));
        if (!f) { std::fprintf(stderr, "cannot write the HDV\n"); return 1; }
    }
    // An 800K blank for the second bay (scenario 4).
    const fs::path d35 = scratch / "blank35.po";
    {
        std::ofstream f(d35, std::ios::binary | std::ios::trunc);
        const std::vector<char> zeros(819200, 0);
        f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }

    // The plain //c, wired as EmulationController wires it.
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
    if (!mem.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/true)) {
        std::fprintf(stderr, "//c ROM load failed\n");
        return 1;
    }

    auto disk = std::make_unique<DiskIICard>(6);
    disk->loadBootRom(pom2::findResource("roms/disk2.rom"));
    disk->loadLssRom(pom2::findResource("roms/diskii_p6.rom"));
    if (!disk->insertDisk(0, floppy.string())) { std::fprintf(stderr, "insert failed\n"); return 1; }
    disk->setWriteBackEnabled(true);
    disk->setIWM(&iwm);
    // Drive 2 deliberately left EMPTY: the //c's rear port is drive 2 of the
    // IWM, and an empty drive 2 under a motor-on is the trigger.
    mem.slotBus().plug(6, std::move(disk));

    auto sp = std::make_unique<pom2::SmartPortCard>(5);
    pom2::SmartPortCard* card = sp.get();
    auto hdvUnit = std::make_unique<pom2::SmartPortHdvUnit>();
    pom2::SmartPortHdvUnit* unit = hdvUnit.get();
    sp->setUnit(0, std::move(hdvUnit));
    sp->setUnit(1, std::make_unique<pom2::SmartPort35Unit>());
    {
        std::string err;
        if (!sp->mountBay(0, hdv.string(), err)) {
            std::fprintf(stderr, "HDV mount: %s\n", err.c_str());
            return 1;
        }
    }
    unit->setWriteBackEnabled(true);
    mem.slotBus().plug(5, std::move(sp));

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();

    if (!waitFor(mem, cpu, "BITSY  BYE", 250'000'000)) {
        std::fprintf(stderr, "FAIL [%s]: BITSY BYE never appeared\n%s", tag, screen(mem).c_str());
        return 1;
    }
    run(cpu, 30'000'000);
    // Three steps down lands on BASIC.SYSTEM (prodos_save_smoke's recipe).
    for (char k : { '\x0A', '\x0A', '\x0A' }) {
        mem.pasteRawKeys(&k, 1);
        run(cpu, 5'000'000);
    }
    { const char k = '\r'; mem.pasteRawKeys(&k, 1); }
    run(cpu, 90'000'000);
    if (screen(mem).find("\n]") == std::string::npos) {
        std::fprintf(stderr, "FAIL [%s]: no BASIC prompt\n%s", tag, screen(mem).c_str());
        return 1;
    }

    // 1. The SmartPort must really be in play.
    {
        bool slot5 = false;
        const uint8_t devcnt = mem.memRead(0xBF31);
        for (int i = 0; i <= devcnt && i < 14; ++i)
            if (((mem.memRead(static_cast<uint16_t>(0xBF32 + i)) >> 4) & 7) == 5)
                slot5 = true;
        if (!slot5) {
            std::fprintf(stderr, "FAIL [%s]: ProDOS did not list slot 5 — the copy "
                                 "below would prove nothing\n", tag);
            return 1;
        }
    }

    // T on the floppy: the ProDOS disk is full, so free a large file first.
    const char* makeT =
        "DELETE /SPACE.TRIP/COPYIIPLUS.8.4\r"
        "NEW\r"
        "10 D$=CHR$(4):X$=\"\"\r"
        "20 FOR J=1 TO 120:X$=X$+\"X\":NEXT\r"
        "30 PRINT D$\"OPEN /SPACE.TRIP/T\"\r"
        "40 PRINT D$\"WRITE /SPACE.TRIP/T\"\r"
        "50 FOR I=0 TO 39:N$=RIGHT$(\"00\"+STR$(I),3):PRINT N$;X$:NEXT\r"
        "60 PRINT D$\"CLOSE\"\r"
        "RUN\r";
    mem.pasteText(makeT, std::strlen(makeT));
    run(cpu, 200'000'000);
    if (screen(mem).find("ERROR") != std::string::npos ||
        screen(mem).find("NOT FOUND") != std::string::npos) {
        std::fprintf(stderr, "FAIL [%s]: could not create T on the floppy\n%s",
                     tag, screen(mem).c_str());
        return 1;
    }

    // 2. The copy: both files open, READ from the Disk II and WRITE to the
    //    SmartPort, alternating — every floppy read after the first follows a
    //    SmartPort write.
    const char* copy =
        "NEW\r"
        "10 D$=CHR$(4)\r"
        "20 PRINT D$\"OPEN /SPACE.TRIP/T\"\r"
        "30 PRINT D$\"OPEN /SCRATCH/U\"\r"
        "40 FOR I=1 TO 40\r"
        "50 PRINT D$\"READ /SPACE.TRIP/T\"\r"
        "60 INPUT A$\r"
        "70 PRINT D$\"WRITE /SCRATCH/U\"\r"
        "80 PRINT A$\r"
        "90 NEXT\r"
        "100 PRINT D$\"CLOSE\"\r"
        "RUN\r";
    mem.pasteText(copy, std::strlen(copy));

    if (sc == Scenario::Plain) {
        run(cpu, 400'000'000);
    } else {
        // Instruction by instruction until the host is part-way through
        // SENDING a packet to the bus (write mode, bus addressed, a frame in
        // flight) after the file's first blocks are down — then change the
        // media, and let the machine run on.
        bool fired = false;
        long seen = 0;
        long n = 0;
        while (!fired && n < 400'000'000L) {
            n += cpu.run(1);
            const auto& r = port.registers();
            const bool writing   = (r.control() & 0xC0) == 0xC0;
            const bool addressed = (r.phases() & 0x0A) == 0x0A && (r.control() & 0x10);
            if (port.device().progress().blocksWritten >= 2 && writing && addressed &&
                port.device().active() && ++seen == 10) {
                std::string err;
                const bool ok = (sc == Scenario::MountOtherBay)
                    ? card->mountBay(1, d35.string(), err)
                    : card->ejectBay(0);
                if (!ok) {
                    std::fprintf(stderr, "FAIL [%s]: the media change itself failed: %s\n",
                                 tag, err.c_str());
                    return 1;
                }
                fired = true;
            }
        }
        if (!fired) {
            std::fprintf(stderr, "FAIL [%s]: never caught a SmartPort send — this "
                                 "run proves nothing\n", tag);
            return 1;
        }
        run(cpu, 400'000'000L - n);
        if (port.device().active()) {
            std::fprintf(stderr, "FAIL [%s]: the bus is still mid-transaction long "
                                 "after the change — the firmware is stuck\n%s",
                         tag, screen(mem).c_str());
            return 1;
        }
    }

    const std::string s = screen(mem);
    if (sc == Scenario::EjectTargetBay) {
        // The disk left mid-write: the guest must be TOLD, and must get its
        // prompt back.
        const auto runAt = s.rfind("]RUN");
        if (runAt == std::string::npos ||
            s.find("I/O ERROR", runAt) == std::string::npos ||
            s.find(']', s.find("I/O ERROR", runAt)) == std::string::npos) {
            std::fprintf(stderr, "FAIL [%s]: the guest was not told the disk "
                                 "left, or never got its prompt back\n%s", tag, s.c_str());
            return 1;
        }
        std::printf("  %s: I/O ERROR reported, prompt back, bus idle\n", tag);
        return 0;
    }

    if (s.find("NO DEVICE CONNECTED") != std::string::npos ||
        s.find("BREAK") != std::string::npos ||
        s.find("I/O ERROR") != std::string::npos) {
        std::fprintf(stderr, "FAIL [%s]: the copy stopped\n%s", tag, s.c_str());
        return 1;
    }

    // 3. Byte for byte.
    std::string err;
    const std::vector<uint8_t> got = readProdosFile(*unit, "U", err);
    if (!err.empty()) {
        std::fprintf(stderr, "FAIL [%s]: /SCRATCH/U: %s\n", tag, err.c_str());
        return 1;
    }
    std::vector<uint8_t> want;
    for (int i = 0; i < kLines; ++i) {
        char head[8];
        std::snprintf(head, sizeof(head), "%03d", i);
        want.insert(want.end(), head, head + 3);
        want.insert(want.end(), static_cast<std::size_t>(kXs), static_cast<uint8_t>('X'));
        want.push_back('\r');
    }
    if (got.size() != static_cast<std::size_t>(kFileBytes)) {
        std::fprintf(stderr, "FAIL [%s]: /SCRATCH/U is %zu bytes, expected %d "
                             "(496 = the reported stop after one floppy block)\n",
                     tag, got.size(), kFileBytes);
        return 1;
    }
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (got[i] != want[i]) {
            std::fprintf(stderr, "FAIL [%s]: /SCRATCH/U differs at byte %zu: $%02X, expected $%02X\n",
                         tag, i, got[i], want[i]);
            return 1;
        }
    }
    std::printf("  %s: %d bytes copied Disk II -> SmartPort, byte for byte\n",
                tag, kFileBytes);
    return 0;
}

}  // namespace

// The rear port is DRIVE 2. A 5.25" program on the internal drive that
// energises PH1 + PH3 with the motor on — opposing magnets, which some
// protection code does — was taken for the SmartPort addressing pattern, and
// its reads were answered by the bus responder (bug hunt 2026-09-16).
int portClaimsOnlyDrive2()
{
    namespace fs = std::filesystem;
    const fs::path hdv = fs::temp_directory_path() / "pom2_iic_port_claim.hdv";
    {
        std::vector<char> blocks(512 * 280, 0);
        std::ofstream f(hdv, std::ios::binary | std::ios::trunc);
        f.write(blocks.data(), static_cast<std::streamsize>(blocks.size()));
    }
    SlotBus bus;
    {
        auto sp = std::make_unique<pom2::SmartPortCard>(5);
        sp->setUnit(0, std::make_unique<pom2::SmartPortHdvUnit>());
        bus.plug(5, std::move(sp));
    }
    std::string err;
    auto* card = dynamic_cast<pom2::SmartPortCard*>(bus.peripheral(5));
    if (!card || !card->mountBay(0, hdv.string(), err)) {
        std::printf("FAIL: port-claim rig: cannot mount (%s)\n", err.c_str());
        return 1;
    }
    pom2::IIcExternalSmartPort port(&bus);
    uint64_t t = 0;
    uint8_t out = 0;
    const auto touch = [&](uint8_t off) { (void)port.read(off, t += 8, out); };
    touch(0xA);                                   // drive 1
    touch(0x9);                                   // motor on
    touch(0x3);                                   // PH1 on
    touch(0x7);                                   // PH3 on
    int failures = 0;
    if (port.read(0xC, t += 8, out)) {
        std::printf("FAIL: the rear port claimed a drive-1 read with PH1+PH3 "
                    "(answered $%02X)\n", out);
        ++failures;
    }
    touch(0xB);                                   // drive 2: the rear port
    if (!port.read(0xC, t += 8, out)) {
        std::printf("FAIL: the rear port ignored its own addressing pattern\n");
        ++failures;
    }
    std::error_code ec;
    fs::remove(hdv, ec);
    if (!failures)
        std::printf("[ OK ] the rear port answers drive 2 only\n");
    return failures;
}

int main()
{
    if (portClaimsOnlyDrive2() != 0) return 1;

    const std::string rom = pom2::findResource("roms/apple2c-32Kv0.rom");
    const std::string po  = pom2::findResource("disks_5.4/dsk/ProDOS_2_4_3.po");
    if (rom.empty() || po.empty()) {
        std::printf("iic_diskii_after_smartport SKIP: missing //c ROM or ProDOS disk\n");
        return 77;
    }
    int failures = 0;
    for (Scenario sc : { Scenario::Plain, Scenario::MountOtherBay, Scenario::EjectTargetBay })
        failures += runScenario(sc, rom, po);
    if (failures) {
        std::printf("iic_diskii_after_smartport: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("iic_diskii_after_smartport OK\n");
    return 0;
}
