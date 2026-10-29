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

// diskii_own_sequencer — POM2's own read sequencer (DiskIICard::
// ownSequencerStep), which reads WOZ media when there is no P6 table.
//
// Apple's 341-0028-A is the control throughout:
//   1. the read latch over 400 000 CPU cycles of a real WOZ: every nibble at
//      the same cycle with the same value (a hash of the whole stream), and
//      the same hold windows — 8 cycles for a data nibble, 16 for a sync
//      byte — which the guest's `LDA $C08C,X / BPL` loops are timed against;
//   2. a whole boot of that disk on the ][+: the same RAM, cycle for cycle,
//      at every checkpoint;
//   3. without a P6 the WOZ senses write-protected (the sequencer does not
//      write), and loading the P6 lifts it.

#include "DiskIICard.h"
#include "DiskIIRoms.h"
#include "M6502.h"
#include "Memory.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

std::string findFile(const std::string& rel)
{
    for (const char* root : { "", "../", "../../" }) {
        const std::string p = root + rel;
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec)) return p;
    }
    std::fprintf(stderr, "missing %s — run tools/fetch_test_media.py\n", rel.c_str());
    std::exit(1);
}

uint64_t fnv(uint64_t h, uint64_t x) { h ^= x; return h * 1099511628211ULL; }

struct Stream {
    int nibbles = 0;
    uint64_t hash = 14695981039346656037ULL;
    std::map<int, int> holds;   // bit-7 run length (CPU cycles) -> count
};

Stream readStream(const std::string& woz, bool appleP6)
{
    Stream s;
    DiskIICard c(6);
    c.setWriteBackEnabled(false);
    if (appleP6) c.loadLssRom(pom2test::diskIIRomPath("diskii_p6.rom"));
    if (!c.insertDisk(0, woz)) { std::fprintf(stderr, "insert %s failed\n", woz.c_str()); std::exit(1); }
    c.deviceSelectRead(0x9);                     // motor on
    c.deviceSelectRead(0xE);                     // Q7L
    for (int i = 0; i < 200'000; ++i) c.advanceCycles(1);
    uint8_t last = 0;
    int run = 0;
    for (int i = 0; i < 400'000; ++i) {
        c.advanceCycles(1);
        const uint8_t v = c.deviceSelectRead(0xC);
        if (v & 0x80) {
            if (!(last & 0x80)) { ++s.nibbles; s.hash = fnv(fnv(s.hash, uint64_t(i)), v); }
            ++run;
        } else if (run) {
            ++s.holds[run];
            run = 0;
        }
        last = v;
    }
    return s;
}

// RAM hashes at checkpoints of a ][+ boot through Apple's boot PROM (so only
// the sequencer differs between the two runs).
std::vector<uint64_t> bootHashes(const std::string& woz, bool appleP6)
{
    Memory mem;
    if (!mem.loadAppleIIRom(findFile("roms/apple2p.rom").c_str())) std::exit(1);
    auto card = std::make_unique<DiskIICard>(6);
    card->setWriteBackEnabled(false);
    card->loadBootRom(pom2test::diskIIRomPath("disk2.rom"));
    if (appleP6) card->loadLssRom(pom2test::diskIIRomPath("diskii_p6.rom"));
    if (!card->insertDisk(0, woz)) std::exit(1);
    mem.slotBus().plug(6, std::move(card));
    M6502 cpu(&mem);
    cpu.hardReset();
    mem.slotBus().reset();
    cpu.setProgramCounter(0xC600);
    std::vector<uint64_t> out;
    for (int checkpoint = 0; checkpoint < 12; ++checkpoint) {
        for (long c = 0; c < 1'000'000;) c += cpu.run(4096);
        uint64_t h = 14695981039346656037ULL;
        for (int a = 0; a < 0xC000; ++a) h = fnv(h, mem.data()[a]);
        out.push_back(fnv(h, cpu.getProgramCounter()));
    }
    return out;
}

} // namespace

int main()
{
    const std::string woz = findFile("disks_5.4/demo/fastloader/fastloader.woz");

    // 1. The read latch, nibble for nibble.
    const Stream apple = readStream(woz, true);
    const Stream own   = readStream(woz, false);
    std::printf("Apple P6: %d nibbles, hash %016llx\n", apple.nibbles,
                static_cast<unsigned long long>(apple.hash));
    std::printf("POM2    : %d nibbles, hash %016llx\n", own.nibbles,
                static_cast<unsigned long long>(own.hash));
    check(apple.nibbles > 10000, "the control reads a stream");
    check(own.nibbles == apple.nibbles && own.hash == apple.hash,
          "the same nibbles at the same cycles as Apple's P6");
    check(own.holds == apple.holds, "the same hold windows as Apple's P6");
    check(own.holds.count(8) && own.holds.count(16),
          "data nibbles held 8 cycles, sync bytes 16");

    // 2. A whole boot, the same RAM at every checkpoint.
    const std::vector<uint64_t> a = bootHashes(woz, true), b = bootHashes(woz, false);
    int firstDiff = -1;
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) { firstDiff = int(i); break; }
    if (firstDiff >= 0) std::fprintf(stderr, "RAM differs from %d M cycles\n", firstDiff + 1);
    check(firstDiff < 0, "the boot runs the same 12 M cycles as with Apple's P6");

    // 3. It does not write: the WOZ senses protected until a P6 is loaded.
    // A writable copy, so the image's own protection is out of the way.
    {
        const auto tmp = std::filesystem::temp_directory_path() / "pom2_own_sequencer.woz";
        std::filesystem::copy_file(woz, tmp, std::filesystem::copy_options::overwrite_existing);
        DiskIICard c(6);
        c.setWriteBackEnabled(true);
        c.insertDisk(0, tmp.string());
        c.driveImage(0).setWriteBackEnabled(true);
        const bool writable = !c.driveImage(0).isWriteProtected();
        check(writable, "the copy is writable (precondition)");
        auto sense = [&c] {
            c.deviceSelectRead(0xD);             // Q6H
            uint8_t v = 0;
            for (int i = 0; i < 64; ++i) { c.advanceCycles(1); v = c.deviceSelectRead(0xE); }
            return (v & 0x80) != 0;              // shifted-in WPT
        };
        c.deviceSelectRead(0x9);                 // motor on
        check(sense(), "without a P6 a WOZ senses write-protected");
        c.loadLssRom(pom2test::diskIIRomPath("diskii_p6.rom"));
        check(!writable || !sense(), "with the P6 the same WOZ senses writable");
        c.deviceSelectRead(0x8);                 // motor off
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
    }

    if (failures) {
        std::fprintf(stderr, "diskii_own_sequencer: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("diskii_own_sequencer OK\n");
    return 0;
}
