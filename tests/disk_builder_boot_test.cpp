// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// disk_builder_boot — the disks DiskBuilder.h makes, booted in the real
// machine: POM2's 6502, its Memory and its Disk II card reading the image
// through the drive model, under the boot PROM a user actually has.
//
// `disk_builder` proves the layout, with a 6502 interpreter and a fake PROM
// of its own. That cannot say whether the real card hands the loader the
// sectors in the order it asks, whether its head steps where the loader's
// phase arithmetic sends it, or whether the timing leaves the next sector
// under the head. This does: nothing is faked below the image file.
//
// Every case runs under BOTH boot PROMs — POM2's own (DiskIIBootRom.h, what a
// fresh install has) and Apple's 341-0027 when roms/disk2.rom is present —
// because they end their recalibration differently and the loader must not
// care. And in three containers, because a .po reorders the sectors and a
// .woz goes through the bit-cell path.

#include "DiskBuilder.h"
#include "DiskIICard.h"
#include "DiskImage.h"
#include "M6502.h"
#include "Memory.h"
#include "ProDOSDiskFs.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;
void check(bool ok, const std::string& what)
{
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

std::string findFile(const std::string& rel, bool required)
{
    for (const char* root : { "", "../", "../../" }) {
        const std::string p = root + rel;
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) return p;
    }
    if (required) {
        std::fprintf(stderr, "missing %s — run tools/fetch_test_media.py\n", rel.c_str());
        std::exit(1);
    }
    return {};
}

fs::path workDir()
{
    static const fs::path dir = fs::temp_directory_path() / "pom2_disk_builder_boot";
    fs::create_directories(dir);
    return dir;
}

std::string writeFile(const std::string& name, const std::vector<uint8_t>& bytes)
{
    const fs::path p = workDir() / name;
    std::ofstream(p, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return p.string();
}

/// The DOS-ordered sectors as a WOZ, the way pom2_disk makes one: POM2's own
/// nibblizer and its own WOZ writer.
std::vector<uint8_t> asWoz(const std::vector<uint8_t>& dosOrdered)
{
    const std::string scratch = writeFile("scratch.dsk", dosOrdered);
    auto image = std::make_unique<DiskImage>();              // 242 KB: not on the stack
    if (!image->loadFile(scratch, DiskImage::SectorOrder::Dos33)) return {};
    std::vector<DiskImage::TrackBuffer> surface(DiskImage::kTracks);
    for (int t = 0; t < DiskImage::kTracks; ++t)
        for (int i = 0; i < DiskImage::kNibblesPerTrack; ++i)
            surface[static_cast<std::size_t>(t)][static_cast<std::size_t>(i)] =
                image->nibbleAt(t, i);
    return DiskImage::buildWozFromSurface(surface, false);
}

std::string screenText(const uint8_t* ram)
{
    std::string out;
    for (int row = 0; row < 24; ++row) {
        const int base = 0x400 + 0x80 * (row % 8) + 0x28 * (row / 8);
        for (int col = 0; col < 40; ++col) {
            const uint8_t c = ram[base + col] & 0x7F;
            out += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : ' ';
        }
        out += '\n';
    }
    return out;
}

struct Machine {
    Memory mem;
    std::unique_ptr<M6502> cpu;
    DiskIICard* card = nullptr;

    /// `appleProm` empty = POM2's own boot PROM.
    Machine(const std::string& image, int slot, const std::string& appleProm)
    {
        if (!mem.loadAppleIIRom(findFile("roms/apple2p.rom", true).c_str())) {
            std::fprintf(stderr, "cannot load apple2p.rom\n");
            std::exit(1);
        }
        auto disk = std::make_unique<DiskIICard>(slot);
        disk->setWriteBackEnabled(false);
        if (!appleProm.empty() && !disk->loadBootRom(appleProm)) {
            std::fprintf(stderr, "cannot load %s\n", appleProm.c_str());
            std::exit(1);
        }
        if (!disk->insertDisk(image)) {
            std::fprintf(stderr, "insertDisk %s: %s\n", image.c_str(),
                         disk->getLastError().c_str());
            std::exit(1);
        }
        card = disk.get();
        mem.slotBus().plug(slot, std::move(disk));
        cpu = std::make_unique<M6502>(&mem);
        // Power on, and let the Autostart ROM do what it does on the real
        // machine: its own reset (normal video, the text window, the I/O
        // hooks) and then the slot scan that finds this card. Jumping
        // straight to $Cs00 skips the first half, and the Monitor's COUT
        // then prints through whatever INVFLG the RAM came up with — the
        // message was there, in inverse.
        cpu->hardReset();
        mem.slotBus().reset();
    }

    /// Run until the CPU is about to execute `pc`, or `budget` cycles pass.
    bool runTo(uint16_t pc, long budget)
    {
        for (long cycles = 0; cycles < budget;) {
            if (cpu->getProgramCounter() == pc) return true;
            cycles += cpu->run(1);
        }
        return cpu->getProgramCounter() == pc;
    }
};

/// A payload that proves it ran and that every page of it is where it
/// should be: three bytes of data first (so the entry is NOT the load
/// address), then code that stores a mark on the text screen and spins.
///
///   +0  DE AD BE          data
///   +3  A9 C1     LDA #$C1
///   +5  8D 00 04  STA $0400     'A', top left
///   +8  4C lo hi  JMP +8        spin: where the test waits
///   +11 …         a pattern, different in every page
std::vector<uint8_t> payload(uint16_t load, std::size_t bytes)
{
    std::vector<uint8_t> p(bytes);
    for (std::size_t i = 0; i < bytes; ++i)
        p[i] = static_cast<uint8_t>((i * 7u) ^ (i >> 8) ^ 0x5Au);
    const uint16_t spin = static_cast<uint16_t>(load + 8);
    const uint8_t head[11] = { 0xDE, 0xAD, 0xBE, 0xA9, 0xC1, 0x8D, 0x00, 0x04, 0x4C,
                               static_cast<uint8_t>(spin), static_cast<uint8_t>(spin >> 8) };
    std::memcpy(p.data(), head, sizeof head);
    return p;
}

void bootDirect(const std::string& label, const std::string& image, int slot,
                const std::string& prom, uint16_t load, const std::vector<uint8_t>& want)
{
    Machine m(image, slot, prom);
    const uint16_t spin = static_cast<uint16_t>(load + 8);
    const bool reached = m.runTo(spin, 40'000'000);
    check(reached, label + ": the payload's code runs (PC reaches its spin loop)");
    if (!reached) {
        std::fprintf(stderr, "  PC=$%04X\n%s", m.cpu->getProgramCounter(),
                     screenText(m.mem.data()).c_str());
        return;
    }
    const uint8_t* ram = m.mem.data();
    check(ram[0x0400] == 0xC1, label + ": the payload wrote its mark");
    check(std::memcmp(ram + load, want.data(), want.size()) == 0,
          label + ": every byte of the payload is in memory, in order");
    // The payload starts on track 1 and takes 16 sectors a track: the head
    // ends on the last track it had to read, no further.
    const int lastTrack =
        static_cast<int>((pom2::diskbuild::directBootSectors(load, want.size()) + 15) / 16);
    check(m.card->getCurrentTrack() == lastTrack,
          label + ": the head stops on track " + std::to_string(lastTrack));
    std::printf("  ok: %s (head on track %d)\n", label.c_str(), m.card->getCurrentTrack());
}

void bootBlank(const std::string& label, const std::string& image, int slot,
               const std::string& prom)
{
    Machine m(image, slot, prom);
    // $FF69 is MONZ: the Monitor's prompt, where the sector ends.
    bool seen = false;
    for (int n = 0; n < 200 && !seen; ++n) {
        for (int i = 0; i < 100'000; i += m.cpu->run(4096)) {}
        seen = screenText(m.mem.data()).find(pom2::diskbuild::kNotStartupMessage) !=
               std::string::npos;
    }
    check(seen, label + ": booting it says it is not a startup disk");
    if (!seen) std::fprintf(stderr, "%s", screenText(m.mem.data()).c_str());
    else std::printf("  ok: %s\n", label.c_str());
}

}  // namespace

int main()
{
    using namespace pom2::diskbuild;
    const std::string apple = findFile("roms/disk2.rom", false);
    std::vector<std::pair<std::string, std::string>> proms = { { "POM2 PROM", "" } };
    if (!apple.empty()) proms.push_back({ "Apple PROM", apple });
    else std::printf("  (roms/disk2.rom absent: Apple's PROM not exercised)\n");

    // Direct boot. 40 pages = three tracks: the loader's head stepping is
    // exercised twice, on a real drive model. The second case does not start
    // on a page boundary and is entered three bytes in.
    struct Case { uint16_t load; std::size_t bytes; };
    for (const Case c : { Case{ 0x4000, 40 * 256 }, Case{ 0x6010, 5 * 256 - 16 + 77 } }) {
        const std::vector<uint8_t> code = payload(c.load, c.bytes);
        DirectBootSpec spec;
        spec.loadAddress  = c.load;
        spec.entryAddress = static_cast<uint16_t>(c.load + 3);
        std::vector<uint8_t> sectors;
        std::string refusal;
        if (!directBoot(code, spec, sectors, refusal)) {
            check(false, "directBoot refused: " + refusal);
            continue;
        }
        const std::string dsk = writeFile("boot.dsk", sectors);
        const std::string po  = writeFile("boot.po", dosOrderToProdosOrder(sectors));
        const std::vector<uint8_t> wozBytes = asWoz(sectors);
        check(!wozBytes.empty(), "the direct-boot image nibblizes");
        const std::string woz = writeFile("boot.woz", wozBytes);
        char at[32];
        std::snprintf(at, sizeof at, "$%04X, %zu bytes", c.load, c.bytes);
        for (const auto& prom : proms) {
            bootDirect(std::string("direct boot ") + at + ", .dsk, slot 6, " + prom.first, dsk, 6, prom.second, c.load, code);
            bootDirect(std::string("direct boot ") + at + ", .po, slot 6, " + prom.first, po, 6, prom.second, c.load, code);
            bootDirect(std::string("direct boot ") + at + ", .woz, slot 6, " + prom.first, woz, 6, prom.second, c.load, code);
            bootDirect(std::string("direct boot ") + at + ", .dsk, slot 5, " + prom.first, dsk, 5, prom.second, c.load, code);
        }
    }

    // The blank disks: a DOS 3.3 data disk, and an empty ProDOS volume whose
    // block 0 carries the same sector.
    {
        const std::vector<uint8_t> dos = blankDos33();
        check(dos.size() == kImageBytes, "blankDos33 builds");
        std::string error;
        std::vector<uint8_t> volume = pom2::prodosfs::ProDOSDisk::format(280, "BLANK", error);
        check(volume.size() == kImageBytes, "an empty 280-block ProDOS volume builds: " + error);
        const std::vector<uint8_t> boot = notBootableSector();
        std::copy(boot.begin(), boot.end(), volume.begin());
        {
            pom2::prodosfs::ProDOSDisk disk;
            check(disk.open(volume, error), "the volume still opens with the boot sector on it: " + error);
        }
        const std::string dosDsk = writeFile("blank.dsk", dos);
        const std::string dosPo  = writeFile("blank_dos.po", dosOrderToProdosOrder(dos));
        const std::string proPo  = writeFile("blank_prodos.po", volume);
        const std::string proDsk = writeFile("blank_prodos.dsk", prodosOrderToDosOrder(volume));
        for (const auto& prom : proms) {
            bootBlank("blank DOS 3.3 .dsk, " + prom.first, dosDsk, 6, prom.second);
            bootBlank("blank DOS 3.3 .po, " + prom.first, dosPo, 6, prom.second);
            bootBlank("blank ProDOS .po, " + prom.first, proPo, 6, prom.second);
            bootBlank("blank ProDOS .dsk, slot 5, " + prom.first, proDsk, 5, prom.second);
        }
    }

    std::error_code ec;
    fs::remove_all(workDir(), ec);
    if (failures) { std::printf("disk_builder_boot: %d check(s) failed\n", failures); return 1; }
    std::printf("disk_builder_boot: OK\n");
    return 0;
}
