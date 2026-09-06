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

// Rewind disk-write test (Phase 6 — writes are undone on a rewind).
//
// DiskIICard's snapshot (v2) carries the writable nibble track buffers for
// loaded, non-write-protected, non-WOZ disks, so a rewind reverts a disk
// write. This pins:
//   1. DiskImage media COW: writeNibbleAt then loadMediaSnapshot reverts the
//      nibble (the underlying mechanism);
//   2. end-to-end through the card + MachineSnapshot: write to the disk via
//      the controller, then restoring an earlier full-machine snapshot undoes
//      the write (re-capture is byte-identical to the pre-write capture);
//   3. an empty / no-disk drive adds no media (just a 1-byte present flag).

#include "DiskIICard.h"
#include "DiskImage.h"
#include "M6502.h"
#include "MachineSnapshot.h"
#include "Memory.h"
#include "SnapshotIO.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>

namespace {

// A raw 143360-byte DOS 3.3 sector image loads (and nibblizes) regardless of
// content — we only need structurally-loadable, writable bytes.
std::string writeSyntheticDsk(const char* name)
{
    std::vector<uint8_t> buf(143360);
    for (size_t i = 0; i < buf.size(); ++i)
        buf[i] = static_cast<uint8_t>((i * 7 + 0x11) & 0xFF);
    const auto path = std::filesystem::temp_directory_path() / name;
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    assert(f);
    std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    return path.string();
}

std::vector<uint8_t> fullCapture(M6502& cpu, Memory& mem)
{
    std::vector<uint8_t> blob;
    pom2::SnapshotWriter w(blob);
    pom2::captureMachineState(w, cpu, mem, /*includeSlots=*/true);
    return blob;
}

// Drive a short write burst through the controller at the current head: motor
// on, drive 0, write mode, then a run of Q6H data stores paced by cycles.
void driveWrite(SlotPeripheral* c)
{
    c->deviceSelectRead(0x9);    // motor on
    c->deviceSelectRead(0xA);    // select drive 0
    c->deviceSelectRead(0xF);    // Q7H — write mode
    for (int i = 0; i < 64; ++i) {
        c->deviceSelectWrite(0x0D, static_cast<uint8_t>(0x80 | (i & 0x7F)));  // Q6H store
        c->advanceCycles(32);
    }
    c->deviceSelectRead(0xE);    // back to read mode
    c->deviceSelectRead(0xC);
}

}  // namespace

int main()
{
    const std::string dsk = writeSyntheticDsk("pom2_rewind_diskwrite.dsk");

    // (1) DiskImage media COW reverts a nibble write.
    {
        DiskImage img;
        assert(img.loadFile(dsk));
        assert(!img.isFileWriteProtected() && !img.isWoz());   // can be written in-memory

        std::vector<uint8_t> mediaA;
        img.appendMediaSnapshot(mediaA);
        assert(mediaA.size() == DiskImage::kMediaSnapshotBytes);

        const uint8_t orig = img.nibbleAt(5, 100);
        img.writeNibbleAt(5, 100, static_cast<uint8_t>(orig ^ 0x55));
        assert(img.nibbleAt(5, 100) == static_cast<uint8_t>(orig ^ 0x55));  // write took

        img.loadMediaSnapshot(mediaA.data(), mediaA.size());
        assert(img.nibbleAt(5, 100) == orig);                               // write undone
    }

    // (2) End-to-end: write via the card, then rewind undoes it.
    {
        Memory mem;
        M6502  cpu(&mem);
        mem.slotBus().plug(6, std::make_unique<DiskIICard>(6));
        auto* card = static_cast<DiskIICard*>(mem.slotBus().peripheral(6));
        assert(card->insertDisk(0, dsk));

        const std::vector<uint8_t> fullPre = fullCapture(cpu, mem);
        // The v2 blob must actually carry the media (≫ the ~80-byte mechanical
        // state) for a writable disk.
        assert(fullPre.size() > DiskImage::kMediaSnapshotBytes);

        driveWrite(card);
        const std::vector<uint8_t> fullPost = fullCapture(cpu, mem);
        assert(fullPost != fullPre && "disk write did not change the captured media");

        // Rewind: restore the pre-write snapshot, re-capture — the write is gone.
        pom2::SnapshotReader r(fullPre.data(), fullPre.size());
        assert(r.good());
        assert(pom2::restoreMachineState(r, cpu, mem,
                                         /*transactional=*/false).ok);
        const std::vector<uint8_t> fullAfter = fullCapture(cpu, mem);
        assert(fullAfter == fullPre && "rewind did not undo the disk write");
    }

    // (3) S2: the IN-FLIGHT write burst travels with the card.
    //
    // `lssCycle` (the timing anchor) was already serialised while the flux
    // edges accumulated in `writeBuffer[32]` were not, so a rewind landing
    // INSIDE a sector write resumed with the anchor restored and the burst
    // gone: the splice flushed short and left a hole in the sector, which
    // reached the .dsk once write-back committed.
    //
    // Driving a real burst needs the bit-level LSS and a WOZ/13-sector
    // medium; the serialisation is pinned directly instead, by writing a
    // burst into the v4 tail and requiring it back out unchanged.
    {
        Memory mem;
        M6502  cpu(&mem);
        (void)cpu;
        mem.slotBus().plug(6, std::make_unique<DiskIICard>(6));
        auto* card = static_cast<DiskIICard*>(mem.slotBus().peripheral(6));
        assert(card->insertDisk(0, dsk));

        std::vector<uint8_t> blob;
        card->appendSnapshotState(blob);
        // A quiescent controller's v4 tail is 13 bytes: count(4) = 0,
        // lineActive(1), startTime(8).
        constexpr size_t kTail = 4 + 1 + 8;
        assert(blob.size() > kTail);
        for (int i = 0; i < 4; ++i) assert(blob[blob.size() - kTail + i] == 0);

        // Rewrite it as a burst of three flux stamps with the write line high.
        auto put64 = [](std::vector<uint8_t>& v, uint64_t x) {
            for (int i = 0; i < 8; ++i)
                v.push_back(static_cast<uint8_t>(x >> (8 * i)));
        };
        std::vector<uint8_t> burst(blob.begin(), blob.end() - kTail);
        burst.push_back(3); burst.push_back(0); burst.push_back(0); burst.push_back(0);
        burst.push_back(1);                       // writeLineActive
        put64(burst, 0x0011223344556677ULL);      // writeStartTime
        put64(burst, 111); put64(burst, 222); put64(burst, 333);

        Memory mem2;
        M6502  cpu2(&mem2);
        (void)cpu2;
        mem2.slotBus().plug(6, std::make_unique<DiskIICard>(6));
        auto* card2 = static_cast<DiskIICard*>(mem2.slotBus().peripheral(6));
        assert(card2->insertDisk(0, dsk));
        card2->loadSnapshotState(burst.data(), burst.size());
        std::vector<uint8_t> back;
        card2->appendSnapshotState(back);
        assert(back == burst && "the in-flight write burst did not round-trip");

        // A count past the 32-entry buffer is refused WHOLE (framing check),
        // leaving the card as it was rather than half-restored.
        std::vector<uint8_t> huge = burst;
        huge[burst.size() - 3 * 8 - 8 - 1 - 4] = 99;   // count = 99
        Memory mem3;
        M6502  cpu3(&mem3);
        (void)cpu3;
        mem3.slotBus().plug(6, std::make_unique<DiskIICard>(6));
        auto* card3 = static_cast<DiskIICard*>(mem3.slotBus().peripheral(6));
        assert(card3->insertDisk(0, dsk));
        std::vector<uint8_t> fresh3;
        card3->appendSnapshotState(fresh3);
        card3->loadSnapshotState(huge.data(), huge.size());
        std::vector<uint8_t> after3;
        card3->appendSnapshotState(after3);
        assert(after3 == fresh3 && "an out-of-range burst count was accepted");

        // A pre-v4 blob (no burst tail at all) must still load, and leave the
        // restored controller on a CLEAN splice.
        std::vector<uint8_t> v3(blob.begin(), blob.end() - kTail);
        v3[4] = 3;                                   // version 4 → 3
        Memory mem4;
        M6502  cpu4(&mem4);
        (void)cpu4;
        mem4.slotBus().plug(6, std::make_unique<DiskIICard>(6));
        auto* card4 = static_cast<DiskIICard*>(mem4.slotBus().peripheral(6));
        assert(card4->insertDisk(0, dsk));
        card4->loadSnapshotState(burst.data(), burst.size());   // arm a burst
        card4->loadSnapshotState(v3.data(), v3.size());         // then a v3 blob
        std::vector<uint8_t> back4;
        card4->appendSnapshotState(back4);
        assert(back4.size() == v3.size() + kTail);
        for (int i = 0; i < 4; ++i) assert(back4[v3.size() + i] == 0);
        assert(back4[v3.size() + 4] == 0);           // writeLineActive cleared
    }

    // (4) No disk → no media (cheap blob).
    {
        Memory mem;
        M6502  cpu(&mem);
        mem.slotBus().plug(6, std::make_unique<DiskIICard>(6));
        std::vector<uint8_t> blob;
        mem.slotBus().peripheral(6)->appendSnapshotState(blob);
        assert(blob.size() < 256 && "empty drive should not append media");
    }

    std::filesystem::remove(dsk);
    std::printf("Rewind disk write: OK (media COW + card write-undo + empty-drive guard)\n");
    return 0;
}
