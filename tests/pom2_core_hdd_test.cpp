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

// The hard disk half of the public facade, through nothing but
// <pom2/core.hpp>. Every capacity and protection claim is asked of the device
// by the GUEST — a JSR into the firmware's ProDOS entry, or ProDOS's own MLI —
// because a test of the host file's size says nothing about what a program
// can reach. `--policy-protected` is the same binary under the ctest default,
// POM2_MEDIA_WRITE_DEFAULT=protected, which is read once per process.

#include <pom2/core.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::size_t kBlock = 512;
// [B] ProDOS 8 Technical Reference, appendix B — total_blocks in the volume
// directory header is 2 bytes, so 65535 is the most a volume can declare and
// its last block is $FFFE.
constexpr std::uint32_t kProdosMaxBlocks = 65535;

std::string gSource;

Bytes readFile(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(input), {});
}

void writeFile(const std::string& path, const Bytes& bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    assert(out.good());
}

/// A volume whose block 0 is a boot program — at $0801 it stores $A5 and the X
/// the firmware handed over, then spins — and whose every other block starts
/// with its own number, so a guest read proves WHICH block came back.
Bytes makeVolume(std::uint32_t blocks)
{
    Bytes image(blocks * kBlock, 0);
    for (std::uint32_t b = 1; b < blocks; ++b) {
        image[b * kBlock]     = static_cast<std::uint8_t>(b);
        image[b * kBlock + 1] = static_cast<std::uint8_t>(b >> 8);
    }
    const std::uint8_t boot[] = {
        0x01,                   // $0800: ProDOS boot-block count
        0xA9, 0xA5,             // LDA #$A5
        0x8D, 0x00, 0x03,       // STA $0300
        0x8E, 0x01, 0x03,       // STX $0301
        0x4C, 0x09, 0x08,       // JMP $0809
    };
    std::copy(std::begin(boot), std::end(boot), image.begin());
    return image;
}

void putLe32(Bytes& b, std::size_t at, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

/// A well-formed ProDOS-order 2IMG; the malformed cases edit its fields.
Bytes wrap2mg(const Bytes& payload, std::uint32_t flags = 0, const Bytes& trailer = {})
{
    Bytes out(64, 0);
    std::memcpy(out.data(), "2IMG", 4);
    std::memcpy(out.data() + 4, "PO2A", 4);
    out[8] = 64;
    out[10] = 1;
    putLe32(out, 12, 1);                                            // ProDOS order
    putLe32(out, 16, flags);
    putLe32(out, 20, static_cast<std::uint32_t>(payload.size() / kBlock));
    putLe32(out, 24, 64);
    putLe32(out, 28, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    out.insert(out.end(), trailer.begin(), trailer.end());
    return out;
}

pom2::Core makeIIe()
{
    pom2::CoreConfig config;
    config.machine = pom2::Machine::AppleIIeEnhanced;
    pom2::Core core(config);
    assert(core.loadRom(gSource + "/roms/apple2e.rom"));
    return core;
}

pom2::Core makeIIeDefault() { return makeIIe(); }

pom2::Core makeIIcPlus()
{
    pom2::CoreConfig config;
    config.machine = pom2::Machine::AppleIIcPlus;
    pom2::Core core(config);
    assert(core.loadRom(gSource + "/roms/apple2cp.rom"));
    return core;
}

pom2::Core makeIIc(const char* rom = "/roms/apple2c-32Kv0.rom")
{
    pom2::CoreConfig config;
    config.machine = pom2::Machine::AppleIIc;
    pom2::Core core(config);
    assert(core.loadRom(gSource + rom));
    return core;
}

void runUntilBooted(pom2::Core& core, std::uint8_t expectX)
{
    for (int i = 0; i < 40 && core.read(0x0300) != 0xA5; ++i) core.run(100000);
    assert(core.read(0x0300) == 0xA5);
    assert(core.read(0x0301) == expectX);            // X = slot × 16, the firmware's
}

struct DriverAnswer {
    std::uint8_t a = 0, x = 0, y = 0;
    bool carry = false;
};

constexpr std::uint16_t kCode   = 0x1000;
constexpr std::uint16_t kBuffer = 0x4000;

/// Place `code` at kCode, closed with a JMP to itself, and run it to that JMP.
void runGuest(pom2::Core& core, std::initializer_list<std::uint8_t> code)
{
    std::uint16_t at = kCode;
    for (const std::uint8_t b : code) core.write(at++, b);
    core.write(at, 0x4C);
    core.write(static_cast<std::uint16_t>(at + 1), static_cast<std::uint8_t>(at));
    core.write(static_cast<std::uint16_t>(at + 2), static_cast<std::uint8_t>(at >> 8));
    pom2::CpuState st = core.cpuState();
    st.programCounter = kCode;
    st.status = 0x24;
    core.setCpuState(st);
    for (long i = 0; i < 4000000 && core.cpuState().programCounter != at; ++i) core.step();
    assert(core.cpuState().programCounter == at);
}

/// One ProDOS driver call, as ProDOS makes it: the parameters in $42-$47, then
/// a JSR to the entry $CnFF names. The firmware's own entry, on any device.
DriverAnswer driverCall(pom2::Core& core, int slot, std::uint8_t command,
                        std::uint8_t drive, std::uint16_t block)
{
    const auto page = static_cast<std::uint16_t>(0xC000 | (slot << 8));
    const std::uint8_t entry = core.read(static_cast<std::uint16_t>(page | 0xFF));
    core.write(0x42, command);
    core.write(0x43, static_cast<std::uint8_t>(slot << 4 | drive << 7));
    core.write(0x44, static_cast<std::uint8_t>(kBuffer));
    core.write(0x45, static_cast<std::uint8_t>(kBuffer >> 8));
    core.write(0x46, static_cast<std::uint8_t>(block));
    core.write(0x47, static_cast<std::uint8_t>(block >> 8));
    runGuest(core, {
        0x20, entry, static_cast<std::uint8_t>(page >> 8),   // JSR $Cnxx
        0x08,                   // PHP
        0x8D, 0xF0, 0x03,       // STA $03F0
        0x8E, 0xF1, 0x03,       // STX $03F1
        0x8C, 0xF2, 0x03,       // STY $03F2
        0x68,                   // PLA
        0x8D, 0xF3, 0x03,       // STA $03F3
    });
    DriverAnswer answer;
    answer.a = core.read(0x03F0);
    answer.x = core.read(0x03F1);
    answer.y = core.read(0x03F2);
    answer.carry = (core.read(0x03F3) & 0x01) != 0;
    return answer;
}

std::uint32_t guestStatusBlocks(pom2::Core& core, int slot, std::uint8_t drive = 0)
{
    const DriverAnswer a = driverCall(core, slot, 0x00, drive, 0);
    assert(!a.carry);
    return static_cast<std::uint32_t>(a.x) | static_cast<std::uint32_t>(a.y) << 8;
}

/// Read `block` into kBuffer through the driver; true when it succeeded and
/// the buffer starts with the block's own number.
bool guestReadsBlock(pom2::Core& core, int slot, std::uint16_t block, std::uint8_t drive = 0)
{
    const DriverAnswer a = driverCall(core, slot, 0x01, drive, block);
    return !a.carry
        && core.read(kBuffer) == static_cast<std::uint8_t>(block)
        && core.read(kBuffer + 1) == static_cast<std::uint8_t>(block >> 8);
}

DriverAnswer guestWritesBlock(pom2::Core& core, int slot, std::uint16_t block,
                              std::uint8_t seed, std::uint8_t drive = 0)
{
    for (std::uint16_t i = 0; i < kBlock; ++i)
        core.write(static_cast<std::uint16_t>(kBuffer + i),
                   static_cast<std::uint8_t>(seed + i));
    return driverCall(core, slot, 0x02, drive, block);
}

bool blockIs(const Bytes& image, std::size_t payloadOffset, std::uint32_t block, std::uint8_t seed)
{
    const std::size_t at = payloadOffset + block * kBlock;
    if (at + kBlock > image.size()) return false;
    for (std::size_t i = 0; i < kBlock; ++i)
        if (image[at + i] != static_cast<std::uint8_t>(seed + i)) return false;
    return true;
}

// ── Attaching ─────────────────────────────────────────────────────────────

void testAttachContract()
{
    const std::string path = "pom2_core_hdd_attach.hdv";
    writeFile(path, makeVolume(2000));

    pom2::Core core = makeIIe();
    assert(core.hardDiskSlot() == 0);
    assert(!core.insertHardDisk(0, path));
    assert(core.lastError().find("not attached") != std::string::npos);
    assert(!core.flushHardDisk(0) && !core.ejectHardDisk(0));
    assert(!core.setHardDiskWriteProtected(0, true));
    assert(!core.hardDiskInserted(0) && core.hardDiskBlockCount(0) == 0);

    assert(!core.attachHardDisk(0) && !core.attachHardDisk(8));
    assert(core.lastError().find("slot") != std::string::npos);
    assert(core.attachSmartPort(5));
    assert(!core.attachHardDisk(5));                 // slot taken
    assert(core.lastError().find("occupied") != std::string::npos);
    assert(core.attachHardDisk());
    assert(core.hardDiskSlot() == 7);                // the default
    assert(!core.attachHardDisk(4));                 // one device per core
    assert(core.hardDiskSlot() == 7);

    assert(!core.insertHardDisk(2, path) && !core.insertHardDisk(-1, path));
    assert(core.lastError().find("unit") != std::string::npos);
    assert(!core.flushHardDisk(2) && !core.ejectHardDisk(2));
    assert(!core.hardDiskInserted(2) && core.hardDiskBlockCount(2) == 0);
    assert(!core.setHardDiskWriteProtected(0, true));  // nothing mounted
    assert(core.lastError().find("No volume") != std::string::npos);
    assert(core.flushHardDisk(0));                   // empty: nothing to do
    assert(core.ejectHardDisk(0));

    assert(std::remove(path.c_str()) == 0);
    std::printf("  ok: attach contract\n");
}

// ── Booting through the firmware, in both containers and any slot ─────────

void testBootAndReplay()
{
    const std::string raw = "pom2_core_hdd_boot.hdv";
    const std::string container = "pom2_core_hdd_boot.2mg";
    // Exactly what a 3.5" reading of a .2mg suffix refuses: over 800 KiB.
    const Bytes volume = makeVolume(14690);
    writeFile(raw, volume);
    writeFile(container, wrap2mg(volume));

    for (const std::string& path : { raw, container }) {
        pom2::Core core = makeIIe();
        assert(core.attachHardDisk(5));
        assert(core.insertHardDisk(0, path));
        assert(core.hardDiskInserted(0) && !core.hardDiskInserted(1));
        assert(core.hardDiskBlockCount(0) == 14690);
        assert(!core.hardDiskWriteProtected(0));
        assert(core.bootSlot(core.hardDiskSlot()));
        assert(core.cpuState().programCounter == 0xC500);
        runUntilBooted(core, 0x50);
        assert(guestStatusBlocks(core, 5) == 14690);
        assert(guestReadsBlock(core, 5, 14689));
        const DriverAnswer past = driverCall(core, 5, 0x01, 0, 14690);
        assert(past.carry && past.a == 0x27);        // I/O error, not a wrap
    }

    // Slot 7 hands the boot sector its own X.
    {
        pom2::Core core = makeIIe();
        assert(core.attachHardDisk());
        assert(core.insertHardDisk(0, container));
        assert(core.bootSlot(7));
        runUntilBooted(core, 0x70);
    }

    // Slot 3 on a //e is the 80-column firmware: bootSlot's bus check applies.
    {
        pom2::Core core = makeIIe();
        assert(core.attachHardDisk(3));
        assert(core.insertHardDisk(0, raw));
        assert(!core.bootSlot(3));
        assert(core.lastError().find("not visible") != std::string::npos);
    }

    // The older machines, NMOS 6502 included: the card's ROM must run on them.
    for (const auto& [machine, rom] : {
             std::make_pair(pom2::Machine::AppleIIPlus, "/roms/apple2p.rom"),
             std::make_pair(pom2::Machine::AppleIIe, "/roms/apple2e_unenh.rom") }) {
        pom2::CoreConfig config;
        config.machine = machine;
        pom2::Core core(config);
        assert(core.loadRom(gSource + rom));
        assert(core.attachHardDisk());
        assert(core.insertHardDisk(0, raw));
        assert(core.bootSlot(7));
        runUntilBooted(core, 0x70);
        assert(guestStatusBlocks(core, 7) == 14690);
        assert(guestReadsBlock(core, 7, 14689));
        assert(!guestWritesBlock(core, 7, 9, 0x5A).carry);
        (void)driverCall(core, 7, 0x01, 0, 9);
        assert(core.read(kBuffer) == 0x5A);
    }
    writeFile(raw, volume);                          // those two saved a write

    // The second unit answers as drive 2 of the slot, independently.
    {
        const std::string second = "pom2_core_hdd_drive2.hdv";
        writeFile(second, makeVolume(300));
        pom2::Core core = makeIIe();
        assert(core.attachHardDisk());
        assert(core.insertHardDisk(0, raw));
        assert(core.insertHardDisk(1, second));
        assert(core.hardDiskBlockCount(1) == 300);
        assert(core.bootSlot(7));
        runUntilBooted(core, 0x70);
        assert(guestStatusBlocks(core, 7, 1) == 300);
        assert(guestReadsBlock(core, 7, 299, 1));
        assert(driverCall(core, 7, 0x01, 1, 300).carry);
        assert(guestReadsBlock(core, 7, 300, 0));    // drive 1's own block 300
        assert(std::remove(second.c_str()) == 0);
    }

    // Same script, same ROM, same volume: same state, to the cycle.
    const auto script = [&container]() {
        pom2::Core core = makeIIe();
        assert(core.attachHardDisk());
        assert(core.insertHardDisk(0, container));
        assert(core.bootSlot(7));
        runUntilBooted(core, 0x70);
        (void)guestReadsBlock(core, 7, 1234);
        const pom2::CpuState state = core.cpuState();
        Bytes ram(0xC000);
        for (std::uint32_t a = 0; a < ram.size(); ++a)
            ram[a] = core.read(static_cast<std::uint16_t>(a));
        return std::make_pair(state.cycles, ram);
    };
    assert(script() == script());

    assert(std::remove(raw.c_str()) == 0);
    assert(std::remove(container.c_str()) == 0);
    std::printf("  ok: firmware boot, raw and 2IMG, slots, drive 2, replay\n");
}

// ── Writes: when they reach the file ──────────────────────────────────────

void testWritePersistence()
{
    const std::string path = "pom2_core_hdd_save.2mg";
    const Bytes trailer = { 'c', 'r', 'e', 'a', 't', 'o', 'r' };
    const Bytes volume = makeVolume(2000);
    const Bytes original = wrap2mg(volume, 0, trailer);
    writeFile(path, original);
    const Bytes header(original.begin(), original.begin() + 64);

    {
        pom2::Core core = makeIIe();
        assert(core.attachHardDisk());
        assert(core.insertHardDisk(0, path));
        assert(core.bootSlot(7));
        runUntilBooted(core, 0x70);

        // Pending: in the guest's volume, not yet in the file.
        assert(core.hardDiskUnsavedBlocks(0) == 0);
        assert(!guestWritesBlock(core, 7, 100, 0x11).carry);
        assert(core.hardDiskUnsavedBlocks(0) == 1);
        assert(readFile(path) == original);
        (void)driverCall(core, 7, 0x01, 0, 100);
        assert(core.read(kBuffer + 3) == 0x14);      // the guest reads its write

        // Flush: on the file, the container untouched around it.
        assert(core.flushHardDisk(0));
        assert(core.hardDiskUnsavedBlocks(0) == 0);
        Bytes onHost = readFile(path);
        assert(onHost.size() == original.size());
        assert(blockIs(onHost, 64, 100, 0x11));
        assert(std::equal(header.begin(), header.end(), onHost.begin()));
        assert(std::equal(trailer.begin(), trailer.end(),
                          onHost.end() - static_cast<std::ptrdiff_t>(trailer.size())));
        assert(core.flushHardDisk(0));               // clean: nothing to do

        // Replacing the volume saves it first — and the same file comes back
        // with the write in it, not the bytes from before the flush.
        assert(!guestWritesBlock(core, 7, 101, 0x22).carry);
        assert(core.insertHardDisk(0, path));
        assert(core.hardDiskUnsavedBlocks(0) == 0);
        assert(blockIs(readFile(path), 64, 101, 0x22));
        (void)driverCall(core, 7, 0x01, 0, 101);
        assert(core.read(kBuffer + 5) == 0x27);

        // So does an eject.
        assert(!guestWritesBlock(core, 7, 102, 0x33).carry);
        assert(core.ejectHardDisk(0));
        assert(!core.hardDiskInserted(0) && core.hardDiskBlockCount(0) == 0);
        assert(blockIs(readFile(path), 64, 102, 0x33));

        // An empty bay answers "no device", and takes nothing.
        const DriverAnswer empty = driverCall(core, 7, 0x01, 0, 0);
        assert(empty.carry && empty.a == 0x28);

        // So does destroying the core, with nothing called before it.
        assert(core.insertHardDisk(0, path));
        assert(!guestWritesBlock(core, 7, 103, 0x44).carry);
        assert(!blockIs(readFile(path), 64, 103, 0x44));
    }
    assert(blockIs(readFile(path), 64, 103, 0x44));

    // A move-assignment retires the old machine the same way.
    {
        pom2::Core core = makeIIe();
        assert(core.attachHardDisk());
        assert(core.insertHardDisk(0, path));
        assert(core.bootSlot(7));
        runUntilBooted(core, 0x70);
        assert(!guestWritesBlock(core, 7, 104, 0x55).carry);
        core = makeIIe();
        assert(blockIs(readFile(path), 64, 104, 0x55));
    }

#ifndef _WIN32
    // A failed save is named, keeps the volume mounted and its write pending,
    // and refuses what would have dropped it. A read-only directory refuses
    // the atomic replace's temp file; root ignores that.
    if (geteuid() != 0) {
        const std::string dir = "pom2_core_hdd_ro";
        const std::string inside = dir + "/disk.hdv";
        const std::string other = "pom2_core_hdd_other.hdv";
        (void)mkdir(dir.c_str(), 0755);
        const Bytes volumeBytes = makeVolume(300);
        writeFile(inside, volumeBytes);
        writeFile(other, volumeBytes);
        {
            pom2::Core core = makeIIe();
            assert(core.attachHardDisk());
            assert(core.insertHardDisk(0, inside));
            assert(core.bootSlot(7));
            runUntilBooted(core, 0x70);
            assert(!guestWritesBlock(core, 7, 50, 0x66).carry);
            assert(chmod(dir.c_str(), 0555) == 0);

            assert(!core.flushHardDisk(0));
            assert(core.lastError().find("pom2tmp") != std::string::npos);
            assert(core.hardDiskUnsavedBlocks(0) == 1);
            assert(!core.setHardDiskWriteProtected(0, true));
            assert(!core.hardDiskWriteProtected(0));
            assert(!core.insertHardDisk(0, other));
            assert(!core.ejectHardDisk(0));
            assert(!core.lastError().empty());
            assert(core.hardDiskInserted(0));
            assert(core.hardDiskUnsavedBlocks(0) == 1);
            assert(readFile(inside) == volumeBytes);

            assert(chmod(dir.c_str(), 0755) == 0);
            assert(core.flushHardDisk(0));           // the retry still has it
            assert(blockIs(readFile(inside), 0, 50, 0x66));
        }
        assert(readFile(other) == volumeBytes);
        assert(std::remove(inside.c_str()) == 0);
        assert(std::remove(other.c_str()) == 0);
        assert(rmdir(dir.c_str()) == 0);
    }
#endif

    assert(std::remove(path.c_str()) == 0);
    std::printf("  ok: pending writes, flush, replace, eject, destruction, failed save\n");
}

// ── The capacity limit, asked of the guest ────────────────────────────────

void testCapacityLimit()
{
    const std::string path = "pom2_core_hdd_limit.2mg";

    // The largest volume ProDOS can declare: its last block is $FFFE.
    {
        const Bytes volume = makeVolume(kProdosMaxBlocks);
        writeFile(path, wrap2mg(volume));
        pom2::Core core = makeIIe();
        assert(core.attachHardDisk());
        assert(core.insertHardDisk(0, path));
        assert(core.hardDiskBlockCount(0) == kProdosMaxBlocks);
        assert(core.bootSlot(7));
        runUntilBooted(core, 0x70);
        assert(guestStatusBlocks(core, 7) == 0xFFFF);
        assert(guestReadsBlock(core, 7, 0xFFFE));
        assert(guestReadsBlock(core, 7, 0x0101));    // the high byte is real

        // One past the end fails; it does not come round to block $0000.
        const DriverAnswer past = driverCall(core, 7, 0x01, 0, 0xFFFF);
        assert(past.carry && past.a == 0x27);
        const DriverAnswer pastWrite = guestWritesBlock(core, 7, 0xFFFF, 0x77);
        assert(pastWrite.carry && pastWrite.a == 0x27);

        assert(!guestWritesBlock(core, 7, 0xFFFE, 0x88).carry);
        assert(core.flushHardDisk(0));
        const Bytes onHost = readFile(path);
        assert(onHost.size() == 64 + volume.size());
        assert(blockIs(onHost, 64, 0xFFFE, 0x88));
        assert(std::equal(volume.begin(), volume.begin() + kBlock, onHost.begin() + 64));
    }

    // A physical 65536-block image: mounted, its size told apart host-side
    // only, the guest's 16-bit STATUS held at $FFFF and not wrapped to 0.
    {
        writeFile(path, makeVolume(65536));
        pom2::Core core = makeIIe();
        assert(core.attachHardDisk());
        assert(core.insertHardDisk(0, path));
        assert(core.hardDiskBlockCount(0) == 65536);
        assert(core.bootSlot(7));
        runUntilBooted(core, 0x70);
        assert(guestStatusBlocks(core, 7) == 0xFFFF);
        assert(guestReadsBlock(core, 7, 0xFFFF));
    }

    // 65537 cannot be addressed: refused, not truncated.
    writeFile(path, makeVolume(65537));
    pom2::Core core = makeIIe();
    assert(core.attachHardDisk());
    assert(!core.insertHardDisk(0, path));
    assert(core.lastError().find("65536") != std::string::npos);
    assert(!core.hardDiskInserted(0));

    assert(std::remove(path.c_str()) == 0);
    std::printf("  ok: 65535 and 65536 blocks through the driver, 65537 refused\n");
}

// ── Protection ────────────────────────────────────────────────────────────

void testProtection()
{
    const std::string path = "pom2_core_hdd_wp.2mg";
    const Bytes volume = makeVolume(300);

    const auto guestRefused = [&](pom2::Core& core) {
        const DriverAnswer a = guestWritesBlock(core, 7, 20, 0x99);
        assert(a.carry && a.a == 0x2B);
        assert(core.hardDiskUnsavedBlocks(0) == 0);
        (void)driverCall(core, 7, 0x01, 0, 20);
        assert(core.read(kBuffer) == 20);            // and its volume kept it out
    };

    // The 2IMG's lock, which nothing lifts.
    {
        const Bytes locked = wrap2mg(volume, 0x80000000u);
        writeFile(path, locked);
        pom2::Core core = makeIIe();
        assert(core.attachHardDisk());
        assert(core.insertHardDisk(0, path));
        assert(core.hardDiskWriteProtected(0));
        assert(core.bootSlot(7));
        runUntilBooted(core, 0x70);
        guestRefused(core);
        assert(core.setHardDiskWriteProtected(0, false));
        assert(core.hardDiskWriteProtected(0));
        guestRefused(core);
        assert(core.flushHardDisk(0));
        assert(core.ejectHardDisk(0));
        assert(readFile(path) == locked);
    }

    // The sticker: what the guest wrote first is kept, what comes after is
    // refused, and lifting it writes again. It leaves with the volume.
    {
        writeFile(path, wrap2mg(volume));
        pom2::Core core = makeIIe();
        assert(core.attachHardDisk());
        assert(core.insertHardDisk(0, path));
        assert(core.bootSlot(7));
        runUntilBooted(core, 0x70);
        assert(!guestWritesBlock(core, 7, 21, 0xAA).carry);
        assert(core.setHardDiskWriteProtected(0, true));
        assert(core.hardDiskWriteProtected(0));
        const Bytes afterFlush = readFile(path);
        assert(blockIs(afterFlush, 64, 21, 0xAA));   // written back first
        guestRefused(core);
        assert(core.flushHardDisk(0));
        assert(readFile(path) == afterFlush);
        assert(core.setHardDiskWriteProtected(0, false));
        assert(!core.hardDiskWriteProtected(0));
        assert(!guestWritesBlock(core, 7, 22, 0xBB).carry);
        assert(core.flushHardDisk(0));
        assert(blockIs(readFile(path), 64, 22, 0xBB));

        assert(core.setHardDiskWriteProtected(0, true));
        assert(core.insertHardDisk(0, path));        // the same file, remounted
        assert(!core.hardDiskWriteProtected(0));
    }

#ifndef _WIN32
    // The file's read-only bit, which the sticker's lifting cannot get past.
    if (geteuid() != 0) {
        const Bytes clean = wrap2mg(volume);
        writeFile(path, clean);
        assert(chmod(path.c_str(), 0444) == 0);
        {
            pom2::Core core = makeIIe();
            assert(core.attachHardDisk());
            assert(core.insertHardDisk(0, path));
            assert(core.hardDiskWriteProtected(0));
            assert(core.bootSlot(7));
            runUntilBooted(core, 0x70);
            guestRefused(core);
            assert(core.setHardDiskWriteProtected(0, false));
            assert(core.hardDiskWriteProtected(0));
            guestRefused(core);
        }
        assert(readFile(path) == clean);
        assert(chmod(path.c_str(), 0644) == 0);
    }
#endif

    assert(std::remove(path.c_str()) == 0);
    std::printf("  ok: 2IMG lock, sticker and read-only file refuse the guest's writes\n");
}

// ── Malformed media ───────────────────────────────────────────────────────

void testRefusals()
{
    const std::string good = "pom2_core_hdd_good.hdv";
    const std::string bad = "pom2_core_hdd_bad.img";
    const Bytes volume = makeVolume(300);
    writeFile(good, volume);
    const Bytes wrapped = wrap2mg(volume);

    pom2::Core core = makeIIe();
    assert(core.attachHardDisk());
    assert(core.insertHardDisk(0, good));

    const auto refused = [&](const Bytes& image, const char* expected) {
        writeFile(bad, image);
        const bool mounted = core.insertHardDisk(0, bad);
        if (mounted || core.lastError().find(expected) == std::string::npos)
            std::fprintf(stderr, "expected a refusal naming \"%s\", got: %s\n",
                         expected, core.lastError().c_str());
        assert(!mounted);
        assert(core.lastError().find(expected) != std::string::npos);
        assert(readFile(bad) == image);              // never touched
        assert(core.hardDiskBlockCount(0) == 300);   // no partial mount
    };

    refused(Bytes(wrapped.begin(), wrapped.begin() + 32), "cut short");
    refused(Bytes(wrapped.begin(), wrapped.begin() + 4), "cut short");

    Bytes offsetOut = wrapped;
    putLe32(offsetOut, 24, static_cast<std::uint32_t>(wrapped.size() + 512));
    refused(offsetOut, "data window");

    Bytes lengthOut = wrapped;
    putLe32(lengthOut, 20, 0);
    putLe32(lengthOut, 28, static_cast<std::uint32_t>(volume.size() + kBlock));
    refused(lengthOut, "data window");
    // A payload cut off in the file says the same thing from the other side.
    refused(Bytes(wrapped.begin(), wrapped.end() - kBlock), "data window");

    Bytes offsetInHeader = wrapped;
    putLe32(offsetInHeader, 24, 16);
    refused(offsetInHeader, "data window");

    Bytes noLength = wrapped;
    putLe32(noLength, 28, 0);
    refused(noLength, "data window");

    Bytes notBlocks = wrapped;
    putLe32(notBlocks, 20, 0);
    putLe32(notBlocks, 28, static_cast<std::uint32_t>(volume.size() - 1));
    refused(notBlocks, "512-byte");

    Bytes disagrees = wrapped;
    putLe32(disagrees, 20, 299);
    refused(disagrees, "declares 299 blocks");

    Bytes dosOrder = wrapped;
    putLe32(dosOrder, 12, 0);
    refused(dosOrder, "ProDOS block order");

    refused(Bytes(volume.begin(), volume.end() - 1), "512-byte");
    refused(Bytes{}, "empty");

    assert(!core.insertHardDisk(0, "pom2_core_hdd_missing.hdv"));
    assert(core.lastError().find("missing") != std::string::npos);
    assert(core.hardDiskBlockCount(0) == 300);

    // Over the size the store will even read: refused before it allocates.
    {
        std::ofstream huge(bad, std::ios::binary | std::ios::trunc);
        huge.seekp(65u * 1024u * 1024u - 1);
        huge.put(0);
    }
    assert(!core.insertHardDisk(0, bad));
    assert(core.lastError().find("too large") != std::string::npos);
    assert(core.hardDiskBlockCount(0) == 300);

    // A block count of 0 says nothing, and is not a disagreement.
    Bytes zeroBlocks = wrapped;
    putLe32(zeroBlocks, 20, 0);
    writeFile(bad, zeroBlocks);
    assert(core.insertHardDisk(0, bad));
    assert(core.hardDiskBlockCount(0) == 300);

    // The same data written under any name: the suffix decides nothing.
    const std::string misnamed = "pom2_core_hdd_named.dsk";
    writeFile(misnamed, wrapped);
    assert(core.insertHardDisk(1, misnamed));
    assert(core.hardDiskBlockCount(1) == 300);

    assert(core.ejectHardDisk(0) && core.ejectHardDisk(1));
    assert(std::remove(good.c_str()) == 0);
    assert(std::remove(bad.c_str()) == 0);
    assert(std::remove(misnamed.c_str()) == 0);
    std::printf("  ok: malformed media refused, nothing mounted, nothing written\n");
}

// ── The existing paths, next to it ────────────────────────────────────────

void testBesideOtherMedia()
{
    const std::string hdd = "pom2_core_hdd_beside.2mg";
    const std::string floppy35 = "pom2_core_hdd_beside800.po";
    const std::string floppy525 = "pom2_core_hdd_beside140.po";
    writeFile(hdd, wrap2mg(makeVolume(14690)));
    writeFile(floppy35, makeVolume(1600));
    writeFile(floppy525, makeVolume(280));

    const auto machine = [&]() {
        pom2::Core core = makeIIe();
        assert(core.attachDiskII(gSource + "/roms/disk2.rom", gSource + "/roms/diskii_p6.rom"));
        assert(core.attachSmartPort(5));
        assert(core.attachHardDisk(7));
        assert(core.insertDisk(0, floppy525));
        assert(core.insertSmartPortDisk(0, floppy35));
        assert(core.insertHardDisk(0, hdd));
        return core;
    };

    { pom2::Core core = machine(); assert(core.bootSlot(7)); runUntilBooted(core, 0x70); }
    { pom2::Core core = machine(); assert(core.bootSlot(5)); runUntilBooted(core, 0x50); }
    {
        pom2::Core core = machine();
        assert(core.bootDisk());
        core.run(3000000);
        assert(core.cpuState().programCounter != 0xC600);    // the PROM ran
    }

    // A 3.5" drive still refuses what is not a 3.5" disk, in its own words.
    {
        pom2::Core core = makeIIe();
        assert(core.attachSmartPort(5));
        assert(!core.insertSmartPortDisk(0, hdd));
        assert(core.lastError().find("400K or 800K") != std::string::npos);
    }

    assert(std::remove(hdd.c_str()) == 0);
    assert(std::remove(floppy35.c_str()) == 0);
    assert(std::remove(floppy525.c_str()) == 0);
    std::printf("  ok: Disk II, 3.5\" SmartPort and hard disk on one machine\n");
}

// ── A real ProDOS: a save, seen by the guest and by the host ──────────────

void runToSystemProgram(pom2::Core& core)
{
    // ProDOS 8 enters a system program at $2000 with its MLI ready at $BF00.
    for (long i = 0; i < 60000000; ++i) {
        if (core.cpuState().programCounter == 0x2000 && core.read(0xBF00) == 0x4C) return;
        core.step();
    }
    assert(false && "ProDOS never entered its system program");
}

void poke(pom2::Core& core, std::uint16_t at, std::initializer_list<std::uint8_t> bytes)
{
    for (const std::uint8_t b : bytes) core.write(at++, b);
}

std::uint8_t mliCall(pom2::Core& core, std::uint8_t call, std::uint16_t params)
{
    runGuest(core, {
        0x20, 0x00, 0xBF,       // JSR $BF00
        call,
        static_cast<std::uint8_t>(params), static_cast<std::uint8_t>(params >> 8),
        0x8D, 0xF0, 0x03,       // STA $03F0
    });
    return core.read(0x03F0);
}

constexpr std::uint16_t kPath = 0x5000, kParams = 0x5100, kFileBuffer = 0x6000, kData = 0x7000;
constexpr std::uint16_t kSaveLength = 700;   // two blocks: a sapling, an index block

void testRealProdosSave(pom2::Core (*make)(), const char* label)
{
    const std::string shipped = gSource + "/hdv/A2FILECMD-PRODOS-XL-65C02-enhanced-1.0.2mg";
    const std::string copy = "pom2_core_hdd_prodos.2mg";
    const Bytes original = readFile(shipped);
    assert(original.size() == 64u + kProdosMaxBlocks * kBlock);
    writeFile(copy, original);

    // The volume's own name, from its directory header on the host.
    const std::size_t directory = 64 + 2 * kBlock;
    const std::size_t nameLength = original[directory + 4] & 0x0F;
    std::string path = "/";
    path.append(reinterpret_cast<const char*>(&original[directory + 5]), nameLength);
    path += "/NEWSAVE";

    const auto boot = [&copy](pom2::Core& core) {
        assert(core.attachHardDisk());
        assert(core.insertHardDisk(0, copy));
        assert(core.hardDiskBlockCount(0) == kProdosMaxBlocks);
        assert(core.bootSlot(core.hardDiskSlot()));
        runToSystemProgram(core);
    };
    const auto setPath = [&path](pom2::Core& core) {
        core.write(kPath, static_cast<std::uint8_t>(path.size()));
        for (std::size_t i = 0; i < path.size(); ++i)
            core.write(static_cast<std::uint16_t>(kPath + 1 + i),
                       static_cast<std::uint8_t>(path[i]));
    };
    const auto payload = [](std::uint16_t i) {
        return static_cast<std::uint8_t>(i * 7 + (i >> 8));
    };

    // Same ROM, same volume: ProDOS reaches its system program on the same
    // cycle, with the same RAM. Asked before the save below changes the volume.
    const auto checkpoint = [&boot, make]() {
        pom2::Core core = make();
        boot(core);
        Bytes ram(0xC000);
        for (std::uint32_t a = 0; a < ram.size(); ++a)
            ram[a] = core.read(static_cast<std::uint16_t>(a));
        return std::make_pair(core.cpuState().cycles, ram);
    };
    assert(checkpoint() == checkpoint());
    assert(readFile(copy) == original);              // booting wrote nothing

    {
        pom2::Core core = make();
        boot(core);
        setPath(core);
        for (std::uint16_t i = 0; i < kSaveLength; ++i)
            core.write(static_cast<std::uint16_t>(kData + i), payload(i));

        poke(core, kParams, { 7, kPath & 0xFF, kPath >> 8, 0xC3, 0x06, 0x00, 0x00, 0x01,
                              0, 0, 0, 0 });
        assert(mliCall(core, 0xC0, kParams) == 0);   // CREATE
        poke(core, kParams, { 3, kPath & 0xFF, kPath >> 8,
                              kFileBuffer & 0xFF, kFileBuffer >> 8, 0 });
        assert(mliCall(core, 0xC8, kParams) == 0);   // OPEN
        const std::uint8_t ref = core.read(kParams + 5);
        poke(core, kParams, { 4, ref, kData & 0xFF, kData >> 8,
                              kSaveLength & 0xFF, kSaveLength >> 8, 0, 0 });
        assert(mliCall(core, 0xCB, kParams) == 0);   // WRITE
        poke(core, kParams, { 1, ref });
        assert(mliCall(core, 0xCC, kParams) == 0);   // CLOSE

        // Nothing has reached the host yet; ProDOS's writes are pending.
        assert(core.hardDiskUnsavedBlocks(0) > 0);
        assert(readFile(copy) == original);
        assert(core.flushHardDisk(0));
    }
    assert(readFile(shipped) == original);           // the shipped disk is intact

    // The host reads the guest's save out of the file: catalog, then data.
    const Bytes saved = readFile(copy);
    assert(saved.size() == original.size());
    assert(saved != original);
    const auto blockAt = [](std::uint32_t block) { return 64 + block * kBlock; };
    std::size_t entry = 0;
    for (std::uint32_t block = 2; block != 0 && entry == 0;
         block = static_cast<std::uint32_t>(saved[blockAt(block) + 2])
               | static_cast<std::uint32_t>(saved[blockAt(block) + 3]) << 8) {
        for (std::size_t i = 0; i < 13; ++i) {
            const std::size_t at = blockAt(block) + 4 + i * 39;
            if ((saved[at] >> 4) != 0 && (saved[at] & 0x0F) == 7
                && std::memcmp(&saved[at + 1], "NEWSAVE", 7) == 0) {
                entry = at;
                break;
            }
        }
    }
    assert(entry != 0 && "CREATE reached the volume directory");
    assert((saved[entry] >> 4) == 2);                // sapling: 700 bytes, 2 blocks
    const std::uint32_t eof = static_cast<std::uint32_t>(saved[entry + 21])
                            | static_cast<std::uint32_t>(saved[entry + 22]) << 8
                            | static_cast<std::uint32_t>(saved[entry + 23]) << 16;
    assert(eof == kSaveLength);
    const std::uint32_t index = static_cast<std::uint32_t>(saved[entry + 17])
                              | static_cast<std::uint32_t>(saved[entry + 18]) << 8;
    for (std::uint16_t i = 0; i < kSaveLength; ++i) {
        const std::uint32_t slot = i / kBlock;
        const std::uint32_t data = static_cast<std::uint32_t>(saved[blockAt(index) + slot])
                                 | static_cast<std::uint32_t>(saved[blockAt(index) + 256 + slot]) << 8;
        assert(saved[blockAt(data) + i % kBlock] == payload(i));
    }

    // A second machine boots the saved volume and reads the same file back,
    // through the same MLI.
    {
        pom2::Core core = make();
        boot(core);
        setPath(core);
        poke(core, kParams, { 3, kPath & 0xFF, kPath >> 8,
                              kFileBuffer & 0xFF, kFileBuffer >> 8, 0 });
        assert(mliCall(core, 0xC8, kParams) == 0);   // OPEN
        const std::uint8_t ref = core.read(kParams + 5);
        poke(core, kParams, { 4, ref, kData & 0xFF, kData >> 8,
                              kSaveLength & 0xFF, kSaveLength >> 8, 0, 0 });
        assert(mliCall(core, 0xCA, kParams) == 0);   // READ
        assert(core.read(kParams + 6) == (kSaveLength & 0xFF));
        assert(core.read(kParams + 7) == (kSaveLength >> 8));
        for (std::uint16_t i = 0; i < kSaveLength; ++i)
            assert(core.read(static_cast<std::uint16_t>(kData + i)) == payload(i));
    }

    assert(std::remove(copy.c_str()) == 0);
    std::printf("  ok: real ProDOS on 65535 blocks, %s: CREATE/WRITE, flush, host read, remount\n", label);
}

// ── The //c: its own firmware, the rear port ──────────────────────────────

void testIIc()
{
    const std::string path = "pom2_core_hdd_iic.2mg";
    const Bytes volume = makeVolume(14690);
    const Bytes original = wrap2mg(volume);
    writeFile(path, original);

    {
        pom2::Core core = makeIIc();
        assert(core.attachHardDisk(2));              // the slot is the machine's
        assert(core.hardDiskSlot() == 5);
        assert(!core.attachSmartPort(5));
        assert(core.insertHardDisk(0, path));
        assert(core.hardDiskBlockCount(0) == 14690);
        assert(core.bootSlot(5));
        runUntilBooted(core, 0x50);

        // The firmware served that: its ROM page, not a card's stand-in.
        assert(core.read(0xC500) != 0xEA);
        assert(guestStatusBlocks(core, 5) == 14690);
        assert(guestReadsBlock(core, 5, 14689));
        const DriverAnswer past = driverCall(core, 5, 0x01, 0, 14690);
        assert(past.carry);

        assert(!guestWritesBlock(core, 5, 200, 0x31).carry);
        assert(core.hardDiskUnsavedBlocks(0) == 1);
        assert(readFile(path) == original);
        assert(core.flushHardDisk(0));
        const Bytes onHost = readFile(path);
        assert(blockIs(onHost, 64, 200, 0x31));
        assert(std::equal(original.begin(), original.begin() + 64, onHost.begin()));

        assert(core.setHardDiskWriteProtected(0, true));
        const DriverAnswer refused = guestWritesBlock(core, 5, 201, 0x32);
        assert(refused.carry && refused.a == 0x2B);
        assert(core.flushHardDisk(0));
        assert(readFile(path) == onHost);
        assert(core.setHardDiskWriteProtected(0, false));

        assert(!guestWritesBlock(core, 5, 202, 0x33).carry);
    }
    assert(blockIs(readFile(path), 64, 202, 0x33));  // saved by the destruction

    // The limit, through the bus: 24 bits on the wire, 16 in a ProDOS STATUS.
    for (const std::uint32_t blocks : { kProdosMaxBlocks, std::uint32_t{65536} }) {
        writeFile(path, wrap2mg(makeVolume(blocks)));
        pom2::Core core = makeIIc();
        assert(core.attachHardDisk());
        assert(core.insertHardDisk(0, path));
        assert(core.hardDiskBlockCount(0) == blocks);
        assert(core.bootSlot(5));
        runUntilBooted(core, 0x50);
        assert(guestStatusBlocks(core, 5) == 0xFFFF);  // 65536 used to read 0
        assert(guestReadsBlock(core, 5, 0xFFFE));
        if (blocks == 65536) {
            assert(guestReadsBlock(core, 5, 0xFFFF));
        } else {
            const DriverAnswer past = driverCall(core, 5, 0x01, 0, 0xFFFF);
            assert(past.carry && past.a == 0x27);
        }
    }

    // The second unit is drive 2 of slot 5, on its own.
    {
        const std::string second = "pom2_core_hdd_iic2.hdv";
        writeFile(second, makeVolume(300));
        writeFile(path, original);
        pom2::Core core = makeIIc();
        assert(core.attachHardDisk());
        assert(core.insertHardDisk(0, path));
        assert(core.insertHardDisk(1, second));
        assert(core.bootSlot(5));
        runUntilBooted(core, 0x50);
        assert(guestStatusBlocks(core, 5, 1) == 300);
        assert(guestReadsBlock(core, 5, 299, 1));
        assert(guestReadsBlock(core, 5, 300, 0));    // drive 1's own block 300
        assert(std::remove(second.c_str()) == 0);
    }

    // A Disk II already there stays: the rear port rides beside it.
    {
        writeFile(path, original);
        pom2::Core core = makeIIc();
        assert(core.attachDiskII({}));
        assert(core.attachHardDisk());
        assert(core.insertHardDisk(0, path));
        assert(core.bootSlot(5));
        runUntilBooted(core, 0x50);
    }

    // The 16 KiB rev-255 //c has no SmartPort firmware: the card's page stands
    // in for it, armed by the boot, as it does for attachSmartPort.
    {
        pom2::Core core = makeIIc("/roms/apple2c-16K.rom");
        assert(core.attachHardDisk());
        assert(core.insertHardDisk(0, path));
        assert(core.bootSlot(5));
        runUntilBooted(core, 0x50);
    }

    assert(std::remove(path.c_str()) == 0);
    std::printf("  ok: //c, 32 KiB through its firmware and rear port; 16 KiB through the stand-in\n");
}

// ── The //c+: the rear port, behind its own 3.5" drives ───────────────────

void testIIcPlus()
{
    const std::string path = "pom2_core_hdd_iicp.2mg";
    const std::string floppy = "pom2_core_hdd_iicp800.po";
    const Bytes original = wrap2mg(makeVolume(14690));
    writeFile(path, original);
    writeFile(floppy, makeVolume(1600));

    // Nothing to boot, and the reason why.
    {
        pom2::Core core = makeIIcPlus();
        assert(core.attachHardDisk(3));              // the slot is the machine's
        assert(core.hardDiskSlot() == 5);
        assert(!core.bootSlot(5));
        assert(core.lastError().find("rear port") != std::string::npos);
    }

    // Internal bay empty: the ROM's scan boots the rear port. Its numbering
    // starts at the internal drive, so that is drive 2 and X = $D0.
    constexpr std::uint8_t kDrive2 = 1;
    {
        pom2::Core core = makeIIcPlus();
        assert(core.attachHardDisk());
        assert(core.insertHardDisk(0, path));
        assert(core.hardDiskBlockCount(0) == 14690);
        assert(core.bootSlot(5));
        runUntilBooted(core, 0xD0);
        assert(guestStatusBlocks(core, 5, kDrive2) == 14690);
        assert(guestReadsBlock(core, 5, 14689, kDrive2));
        assert(driverCall(core, 5, 0x01, kDrive2, 14690).carry);

        assert(!guestWritesBlock(core, 5, 200, 0x41, kDrive2).carry);
        assert(core.hardDiskUnsavedBlocks(0) == 1);
        assert(readFile(path) == original);
        assert(core.flushHardDisk(0));
        const Bytes onHost = readFile(path);
        assert(blockIs(onHost, 64, 200, 0x41));
        assert(std::equal(original.begin(), original.begin() + 64, onHost.begin()));

        assert(core.setHardDiskWriteProtected(0, true));
        const DriverAnswer refused = guestWritesBlock(core, 5, 201, 0x42, kDrive2);
        assert(refused.carry && refused.a == 0x2B);
        assert(core.flushHardDisk(0));
        assert(readFile(path) == onHost);
        assert(core.setHardDiskWriteProtected(0, false));

        assert(!guestWritesBlock(core, 5, 202, 0x43, kDrive2).carry);
    }
    assert(blockIs(readFile(path), 64, 202, 0x43));  // saved by the destruction

    // A disk in the internal bay boots first, and the hard disk stays drive 2.
    {
        writeFile(path, original);
        pom2::Core core = makeIIcPlus();
        assert(core.attachHardDisk());
        assert(core.insertSmartPortDisk(0, floppy));
        assert(core.insertHardDisk(0, path));
        assert(core.bootSlot(5));
        runUntilBooted(core, 0x50);
        assert(guestStatusBlocks(core, 5, 0) == 1600);
        assert(guestStatusBlocks(core, 5, kDrive2) == 14690);
        assert(guestReadsBlock(core, 5, 14689, kDrive2));
    }

    // An external 3.5" joins the chain first and takes drive 2 for itself.
    {
        pom2::Core core = makeIIcPlus();
        assert(core.attachHardDisk());
        assert(core.insertSmartPortDisk(0, floppy));
        assert(core.insertSmartPortDisk(1, floppy));
        assert(core.insertHardDisk(0, path));
        assert(core.bootSlot(5));
        runUntilBooted(core, 0x50);
        assert(guestStatusBlocks(core, 5, kDrive2) == 1600);
    }

    // The limit, as on the //c.
    writeFile(path, makeVolume(65536));
    {
        pom2::Core core = makeIIcPlus();
        assert(core.attachHardDisk());
        assert(core.insertHardDisk(0, path));
        assert(core.hardDiskBlockCount(0) == 65536);
        assert(core.bootSlot(5));
        runUntilBooted(core, 0xD0);
        assert(guestStatusBlocks(core, 5, kDrive2) == 0xFFFF);
        assert(guestReadsBlock(core, 5, 0xFFFF, kDrive2));
    }

    assert(std::remove(path.c_str()) == 0);
    assert(std::remove(floppy.c_str()) == 0);
    std::printf("  ok: //c+, rear port as drive 2, boot through its reset scan\n");
}

// ── The process policy ────────────────────────────────────────────────────

void testPolicyProtected()
{
    const std::string path = "pom2_core_hdd_policy.2mg";
    const Bytes original = wrap2mg(makeVolume(300));
    writeFile(path, original);
    {
        pom2::Core core = makeIIe();
        assert(core.attachHardDisk());
        assert(core.insertHardDisk(0, path));
        assert(core.hardDiskWriteProtected(0));
        assert(core.bootSlot(7));
        runUntilBooted(core, 0x70);
        const DriverAnswer refused = guestWritesBlock(core, 7, 20, 0x10);
        assert(refused.carry && refused.a == 0x2B);  // told, not taken and dropped
        assert(core.setHardDiskWriteProtected(0, false));
        assert(core.hardDiskWriteProtected(0));
        assert(core.flushHardDisk(0));
    }
    {
        pom2::Core core = makeIIc();
        assert(core.attachHardDisk());
        assert(core.insertHardDisk(0, path));
        assert(core.hardDiskWriteProtected(0));
        assert(core.bootSlot(5));
        runUntilBooted(core, 0x50);
        const DriverAnswer refused = guestWritesBlock(core, 5, 20, 0x10);
        assert(refused.carry && refused.a == 0x2B);
    }
    {
        pom2::Core core = makeIIcPlus();
        assert(core.attachHardDisk());
        assert(core.insertHardDisk(0, path));
        assert(core.hardDiskWriteProtected(0));
        assert(core.bootSlot(5));
        runUntilBooted(core, 0xD0);
        const DriverAnswer refused = guestWritesBlock(core, 5, 20, 0x10, 1);
        assert(refused.carry && refused.a == 0x2B);
    }
    assert(readFile(path) == original);
    assert(std::remove(path.c_str()) == 0);
    std::printf("  ok: POM2_MEDIA_WRITE_DEFAULT=protected reaches the guest\n");
}

} // namespace

int main(int argc, char** argv)
{
    gSource = POM2_TEST_SOURCE_DIR;
    if (argc > 1 && std::strcmp(argv[1], "--policy-protected") == 0) {
        testPolicyProtected();
        return 0;
    }
    // ctest protects media for every test (CLAUDE.md, MediaWritePolicy.h);
    // this one writes only images it created, and pins that writes land.
#ifdef _WIN32
    _putenv_s("POM2_MEDIA_WRITE_DEFAULT", "writable");
#else
    setenv("POM2_MEDIA_WRITE_DEFAULT", "writable", 1);
#endif
    testAttachContract();
    testBootAndReplay();
    testWritePersistence();
    testCapacityLimit();
    testProtection();
    testRefusals();
    testBesideOtherMedia();
    testRealProdosSave(makeIIeDefault, "//e");
    testIIc();
    testIIcPlus();
    testRealProdosSave([] { return makeIIc(); }, "//c");
    testRealProdosSave(makeIIcPlus, "//c+");
    return 0;
}
