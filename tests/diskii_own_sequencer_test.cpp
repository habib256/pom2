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
//   3. a writable WOZ senses writable without a P6, as with it;
//   4. writing: an RWTS-shaped burst — Q7H with the first byte, a load
//      (STA $C0nD) then a shift (ORA $C0nC) 4 CPU cycles later per byte, 40
//      cycles apart for self-sync $FFs and 32 for data — lays down the same
//      flux transitions as Apple's P6, for every phase of the burst against
//      the bit cell (8) and every Q7H-to-shift delay RWTS can have (4).

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

// An RWTS-shaped write burst on a writable copy of `woz`; the flux the track
// holds after it. `lead` moves the burst against the bit cell, `entry` is
// the Q7H-to-shift delay.
std::vector<int> writeBurst(const std::string& woz, bool appleP6, int lead, int entry)
{
    const auto tmp = std::filesystem::temp_directory_path() / "pom2_own_sequencer_write.woz";
    std::filesystem::copy_file(woz, tmp, std::filesystem::copy_options::overwrite_existing);
    DiskIICard c(6);
    c.setWriteBackEnabled(true);
    if (appleP6) c.loadLssRom(pom2test::diskIIRomPath("diskii_p6.rom"));
    if (!c.insertDisk(0, tmp.string())) std::exit(1);
    c.driveImage(0).setWriteBackEnabled(true);
    long now = 0;
    auto at = [&](long t) { if (t > now) { c.advanceCycles(int(t - now)); now = t; } };
    auto rd = [&](long t, uint8_t sw) { at(t); c.deviceSelectRead(sw); };
    auto wr = [&](long t, uint8_t sw, uint8_t v) { at(t); c.deviceSelectWrite(sw, v); };
    rd(0, 0x9);
    long t = 100000 + lead;
    rd(t, 0xD); rd(t + 4, 0xE);                  // sense
    wr(t + 8, 0xF, 0xFF);                        // Q7H, the first byte
    rd(t + 8 + entry, 0xC);                      // Q6L
    t += 40;
    const uint8_t data[] = { 0xD5, 0xAA, 0xAD, 0x96, 0xFF, 0x9A, 0xB7, 0xEA,
                             0xCB, 0xDF, 0xF2, 0xA6, 0xDE, 0xAA, 0xEB, 0xFF };
    for (int i = 0; i < 6; ++i) { wr(t, 0xD, 0xFF); rd(t + 4, 0xC); t += 40; }
    for (uint8_t b : data)      { wr(t, 0xD, b);    rd(t + 4, 0xC); t += 32; }
    rd(t, 0xE);                                  // Q7L: the splice
    rd(t + 4, 0xC);
    at(t + 1000);
    std::vector<int> ev = c.driveImage(0).fluxEvents(c.getHalfTrack() * 2);
    c.deviceSelectRead(0x8);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    return ev;
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

    // 3. A writable WOZ senses writable, with or without the P6.
    {
        const auto tmp = std::filesystem::temp_directory_path() / "pom2_own_sequencer.woz";
        std::filesystem::copy_file(woz, tmp, std::filesystem::copy_options::overwrite_existing);
        for (bool appleP6 : { false, true }) {
            DiskIICard c(6);
            c.setWriteBackEnabled(true);
            if (appleP6) c.loadLssRom(pom2test::diskIIRomPath("diskii_p6.rom"));
            c.insertDisk(0, tmp.string());
            c.driveImage(0).setWriteBackEnabled(true);
            check(!c.driveImage(0).isWriteProtected(), "the copy is writable (precondition)");
            c.deviceSelectRead(0x9);             // motor on
            c.deviceSelectRead(0xD);             // Q6H
            uint8_t v = 0xFF;
            for (int i = 0; i < 64; ++i) { c.advanceCycles(1); v = c.deviceSelectRead(0xE); }
            check(!(v & 0x80), appleP6 ? "the P6 senses the copy writable"
                                       : "POM2's sequencer senses the copy writable");
            c.deviceSelectRead(0x8);
        }
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
    }

    // 4. Writing, against Apple's P6.
    {
        int same = 0, cases = 0;
        for (int lead = 0; lead < 8; ++lead)
            for (int entry = 4; entry < 8; ++entry) {
                ++cases;
                same += writeBurst(woz, true, lead, entry) == writeBurst(woz, false, lead, entry);
            }
        std::printf("write bursts: %d of %d lay down Apple's flux\n", same, cases);
        check(same == cases, "RWTS-shaped writes lay down the same flux as Apple's P6");
    }

    if (failures) {
        std::fprintf(stderr, "diskii_own_sequencer: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("diskii_own_sequencer OK\n");
    return 0;
}
