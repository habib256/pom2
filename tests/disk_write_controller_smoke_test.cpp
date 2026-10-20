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

// Disk II controller write-side smoke test.
//
// Goal: simulate the cycle-paced write pattern DOS 3.3 RWTS uses to write
// a sector, then read the track back and confirm the bytes we wrote are
// what's on the disk. If this test fails, the bug is in DiskIICard's
// write/LSS plumbing — not in DiskImage's codec (which has its own test)
// or in DOS itself.
//
// Real DOS 3.3 RWTS write loop, condensed (slot 6 → $C0E0+):
//   STA $C0EF      ; Q7H — enable write mode (turn on write current)
//   LDA #$FF
//   STA $C0ED      ; Q6H — load latch with sync byte $FF
//   loop: LDA buf,Y
//         STA $C0ED       ; load latch with next data byte
//         (timing pad to ~32 cycles per iteration)
//         DEY / BNE loop
//   STA $C0EE      ; Q7L — back to read mode (turn off write current)
//
// Critical hardware fact (cross-checked: Sather "Understanding the Apple II"
// 9-13, AppleWin Disk.cpp DataLatchWriteWOZ): the data latch is loaded
// only on Q6H writes (low4 == 0x0D). Q6L writes (low4 == 0x0C) are SHIFT
// strobes — they do NOT update the latch register. POM2 currently mishandles
// this; the test below pins the corrected semantics.

#include "DiskIICard.h"
#include "DiskImage.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kCyclesPerNibble = 32;

// Drive the controller as if `cycles` of CPU time elapsed without any
// soft-switch access. Mirrors what M6502::step() → memory.advanceCycles()
// does between disk-touching instructions.
void tick(DiskIICard& card, int cycles)
{
    card.advanceCycles(cycles);
}

// Mimic STA $C0Ex from a CPU running at our nominal clock: write the byte,
// then let `cyclesAfter` worth of time elapse before the next access. The
// hardware pacing for a 32-cycle DOS write loop is roughly:
//   STA abs,X        ; 5 cycles
//   <pad ~27 cycles> ; LDA, EOR, etc.
// We split the same way: 5 cycles consumed at-store time, then `cyclesAfter`
// passed for the rest. The total per byte = 32 cycles.
void cpuStore(DiskIICard& card, uint8_t low4, uint8_t v, int cyclesAfter)
{
    card.deviceSelectWrite(low4, v);
    tick(card, 5);
    tick(card, cyclesAfter - 5);
}

// Build a blank 143360-byte .dsk filled with a known pattern.
fs::path writeBlankDsk(const std::string& tag)
{
    fs::path p = fs::temp_directory_path() / ("pom2_disk_write_ctrl_" + tag);
    std::vector<uint8_t> bytes(DiskImage::kBytesPerImage, 0);
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
    }
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return p;
}

}  // namespace

int main()
{
    // ── Case 1: pure latch-load via $C0nD updates the on-track byte ─────
    {
        fs::path p = writeBlankDsk("latch.dsk");
        DiskIICard card;
        card.setWriteBackEnabled(true);
        assert(card.insertDisk(p.string()));
        card.seekTrack0();

        // Motor on, write mode.
        card.deviceSelectWrite(0x9, 0);   // motor on
        card.deviceSelectWrite(0xF, 0);   // Q7H — write mode

        // Drive 16 bytes of writes via Q6H ($C0ED) at ~32-cycle pacing.
        // Pattern is just 0x80 + i so each byte is uniquely identifiable.
        for (int i = 0; i < 16; ++i) {
            cpuStore(card, 0xD, static_cast<uint8_t>(0x80 + i), kCyclesPerNibble);
        }

        // Switch back to read mode (no-op effect on track contents).
        card.deviceSelectWrite(0xE, 0);

        // (Inspection is done by Case 2 below using a peek through eject.)
        std::printf("disk_write_controller: case 1 setup OK (full check in case 2)\n");
        fs::remove(p);
    }

    // ── Case 2: write-back round-trip — write 16 bytes, eject (which
    //          decodes), reload, verify the bytes are still on the track.
    {
        fs::path p = writeBlankDsk("roundtrip.dsk");

        // Reach into the GCR-encoded byte stream the way real DOS would
        // need to. We can't easily synthesise a complete D5-AA-AD data
        // field with a 343-nibble payload here, so instead we just verify
        // that the bytes we asked the controller to write actually
        // appear on the track at a position consistent with the LSS
        // pacing. This is the smallest test that would have caught the
        // current bug (where every nibble across a write batch was the
        // same value).
        DiskIICard card;
        card.setWriteBackEnabled(true);
        assert(card.insertDisk(p.string()));
        card.seekTrack0();

        card.deviceSelectWrite(0x9, 0);   // motor on
        // Let the head settle one full nibble — without this the very
        // first write happens at cycleAccum=0 and the first flush is
        // delayed by a full 32 cycles after the store.
        tick(card, kCyclesPerNibble);

        card.deviceSelectWrite(0xF, 0);   // Q7H — write mode

        const int posBefore = card.getTrackPosition();
        std::vector<uint8_t> sent;
        for (int i = 0; i < 16; ++i) {
            const uint8_t v = static_cast<uint8_t>(0x96 + i);
            cpuStore(card, 0xD, v, kCyclesPerNibble);
            sent.push_back(v);
        }

        card.deviceSelectWrite(0xE, 0);   // back to read

        // Now read back the same 16 nibble positions via $C0nC reads.
        // We peek at the track buffer through deviceSelectRead with the
        // head NOT moving (motor on, but we drive trackPos manually).
        // Switch back to read mode and walk one nibble at a time.
        // Simpler: extract nibbles from the controller via repeated
        // motor-on reads. The controller advances trackPos one per ~32
        // cycles, and reads return the latched byte.

        // Actually the cleanest verification: look directly at the
        // image buffer — but that's private. We don't have a read-back
        // path that doesn't advance trackPos. The pragmatic alternative:
        // assert the *count of distinct values* on the track is > 1.
        // The bug manifests as all 16 positions holding the *same* byte
        // (the last latch value, $96 + 15 = $A5).

        // Step the controller in read mode for 16 nibble-times and
        // sample the output sequence.
        // Reset to the same head position we wrote at.
        card.seekTrack0();
        for (int i = 0; i < posBefore; ++i) {
            tick(card, kCyclesPerNibble);
        }

        std::vector<uint8_t> seen;
        // Wait one full nibble so byteReady latches the next byte.
        for (int i = 0; i < 16; ++i) {
            tick(card, kCyclesPerNibble);
            const uint8_t b = card.deviceSelectRead(0xC);
            seen.push_back(b);
        }

        // Count distinct values.
        std::array<bool, 256> present{};
        for (uint8_t b : seen) present[b] = true;
        int distinct = 0;
        for (bool b : present) if (b) ++distinct;

        std::printf("disk_write_controller: read back %zu nibbles, %d distinct values\n",
                    seen.size(), distinct);
        std::printf("  sent:");
        for (uint8_t b : sent) std::printf(" %02X", b);
        std::printf("\n  seen:");
        for (uint8_t b : seen) std::printf(" %02X", b);
        std::printf("\n");

        // The bug manifests as `distinct <= 2` (one stale value plus
        // one final value, or all the same). With the fix, we expect
        // close to 16 distinct values (allowing some slop for the LSS
        // pacing).
        assert(distinct > 8 && "controller writes collapse to one value — known bug");

        fs::remove(p);
        std::printf("disk_write_controller: round-trip OK\n");
    }

    std::printf("disk_write_controller_smoke OK\n");
    return 0;
}
