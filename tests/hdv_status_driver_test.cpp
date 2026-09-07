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

// ProDOSHardDiskCard slot-ROM driver — STATUS X/Y + no-media read test.
//
// Pins three driver-level contracts by executing the REAL slot ROM on a
// 6502 (same harness as smartport_write_dispatch_test):
//
//   1. STATUS (cmd $00) must return the device's total block count in
//      X (low) / Y (high) — ProDOS volume scanners (BITSY, ONLINE) size
//      the device from these registers, and a driver that left X/Y
//      unset returned garbage (the exact crash SmartPortCard::buildRom
//      already fixed; the HDV card now mirrors that ROM arrangement,
//      reading the count from $C0n4/$C0n5).
//
//   2. Counts above $FFFF clamp: an exactly-65536-block 32 MiB image
//      (Block512Backing::kMaxBlocks deliberately admits it) must report
//      $FFFF, not truncate to 0 ("empty volume").
//
//   3. READ (cmd $01) with no image mounted must return an I/O error —
//      carry set, A = $28 (NO DEVICE CONNECTED) — instead of streaming
//      $FF with CLC "success", per real ProDOS driver conventions.
//
// Plus a guard that a normal READ still works (the no-media probe grew
// the read routine by 9 bytes and moved the write-block branch target).

#include "M6502.h"
#include "Memory.h"
#include "ProDOSHardDiskCard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr int    kSlot     = 5;
constexpr size_t kBlk      = ProDOSHardDiskCard::kBlockBytes;
constexpr uint8_t kRomHi   = 0xC0 + kSlot;          // $C5
constexpr uint16_t kDevBase = 0xC080 + kSlot * 16;  // $C0D0

// JSR <the card's own ProDOS driver entry> with the given parameter block,
// run to the park loop, return the CPU for register inspection.
//
// The entry offset is READ from $CnFF rather than typed: the ROM is assembled
// from labels (SlotRomAsm.h) and $CnFF is `byteOf("driver")`, so a routine
// moving inside the page — which is exactly what the out-of-range and STATUS
// pre-flights did — must not need this harness edited to keep testing the
// same thing. ProDOS itself finds the driver the same way.
void callDriver(M6502& cpu, Memory& mem, uint8_t cmd, uint16_t block,
                uint16_t buffer)
{
    mem.memWrite(0x42, cmd);
    mem.memWrite(0x43, static_cast<uint8_t>(kSlot << 4));  // unit
    mem.memWrite(0x44, static_cast<uint8_t>(buffer & 0xFF));
    mem.memWrite(0x45, static_cast<uint8_t>(buffer >> 8));
    mem.memWrite(0x46, static_cast<uint8_t>(block & 0xFF));
    mem.memWrite(0x47, static_cast<uint8_t>(block >> 8));

    const uint8_t entry =
        mem.memRead(static_cast<uint16_t>((kRomHi << 8) | 0xFF));
    mem.memWrite(0x0300, 0x20);              // JSR $Cn<entry>
    mem.memWrite(0x0301, entry);
    mem.memWrite(0x0302, kRomHi);
    mem.memWrite(0x0303, 0x4C);              // JMP $0303 (park)
    mem.memWrite(0x0304, 0x03);
    mem.memWrite(0x0305, 0x03);

    cpu.setProgramCounter(0x0300);
    cpu.run(60000);                          // ample for a 512-byte loop
}

bool carrySet(const M6502& cpu) { return (cpu.getStatusRegister() & 0x01) != 0; }

// A 5-block ProDOS-order .2mg with the "locked" flag set. Write protection
// only reaches Block512Backing through a real file — either a 2IMG header or
// a chmod-read-only image — and the 2IMG route is the portable one.
std::string writeLocked2mg(const char* name)
{
    const std::string path =
        (std::filesystem::temp_directory_path() / name).string();
    std::vector<uint8_t> img(64 + 5 * kBlk, 0x00);
    img[0] = '2'; img[1] = 'I'; img[2] = 'M'; img[3] = 'G';
    auto wr32 = [&](size_t o, uint32_t v) {
        img[o + 0] = static_cast<uint8_t>(v);
        img[o + 1] = static_cast<uint8_t>(v >> 8);
        img[o + 2] = static_cast<uint8_t>(v >> 16);
        img[o + 3] = static_cast<uint8_t>(v >> 24);
    };
    wr32(12, 1);                       // format 1 = ProDOS block order
    wr32(16, 0x80000000u);             // flags bit 31 = locked
    wr32(24, 64);                      // data offset
    wr32(28, static_cast<uint32_t>(5 * kBlk));
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(img.data()),
            static_cast<std::streamsize>(img.size()));
    return path;
}

}  // namespace

int main()
{
    Memory mem;
    M6502 cpu(&mem);
    cpu.hardReset();

    auto card = std::make_unique<ProDOSHardDiskCard>(kSlot);
    ProDOSHardDiskCard* raw = card.get();

    // 5-block image; block 1 carries a known pattern for the read guard.
    std::vector<uint8_t> hdv(5 * kBlk, 0x00);
    for (size_t i = 0; i < kBlk; ++i)
        hdv[kBlk + i] = static_cast<uint8_t>((i * 5u + 11u) & 0xFF);
    assert(raw->loadImageFromBytes(hdv, "status-test-5blk"));

    mem.slotBus().plug(kSlot, std::move(card));

    // ── 0. The hand-assembled page fits its declared layout ─────────────
    // The write routine below has ZERO slack: it ends at $CnBF and STATUS
    // starts at $CnC0. One extra byte and it eats STATUS — which is exactly
    // what happened to SmartPortCard (see SlotRomAsm.h). The flag catches a
    // region growing OR shrinking; the calls below say the routines are also
    // still where the dispatch table's hand-computed BEQ offsets point.
    if (raw->romLayoutError()) {
        std::printf("FAIL: the slot ROM did not fit its declared layout\n");
        return 1;
    }

    // ── 1. STATUS returns block count in X/Y ────────────────────────────
    callDriver(cpu, mem, /*cmd=*/0x00, /*block=*/0, /*buffer=*/0x0800);
    if (carrySet(cpu) || cpu.getAccumulator() != 0x00) {
        std::printf("FAIL: STATUS returned error (A=%02X C=%d)\n",
                    cpu.getAccumulator(), carrySet(cpu) ? 1 : 0);
        return 1;
    }
    if (cpu.getXRegister() != 5 || cpu.getYRegister() != 0) {
        std::printf("FAIL: STATUS X/Y = %02X/%02X, want 05/00 "
                    "(block count not returned)\n",
                    cpu.getXRegister(), cpu.getYRegister());
        return 1;
    }
    // Register pair feeding the ROM routine.
    if (mem.memRead(kDevBase + 4) != 5 || mem.memRead(kDevBase + 5) != 0) {
        std::printf("FAIL: $C0n4/$C0n5 = %02X/%02X, want 05/00\n",
                    mem.memRead(kDevBase + 4), mem.memRead(kDevBase + 5));
        return 1;
    }

    // ── Guard: READ still works after the routine grew ──────────────────
    callDriver(cpu, mem, /*cmd=*/0x01, /*block=*/1, /*buffer=*/0x0800);
    if (carrySet(cpu)) {
        std::printf("FAIL: READ of mounted block 1 returned carry set\n");
        return 1;
    }
    for (size_t i = 0; i < 16; ++i) {
        const uint8_t want = static_cast<uint8_t>((i * 5u + 11u) & 0xFF);
        if (mem.memRead(0x0800 + static_cast<uint16_t>(i)) != want) {
            std::printf("FAIL: READ data mismatch at +%zu: %02X want %02X\n",
                        i, mem.memRead(0x0800 + static_cast<uint16_t>(i)),
                        want);
            return 1;
        }
    }

    // ── Guard: WRITE through the ROM driver ($Cn91) ─────────────────────
    // The write routine is the one with no room left, and until now nothing
    // executed it: the HDV tests all drove the backing store directly. So a
    // write routine that had been overrun — the SmartPort failure exactly —
    // would have gone unnoticed here too. Writing a block and reading it back
    // through the same ROM says the dispatch's `BEQ +55` still lands on the
    // first byte of the routine and that the routine still ends in its RTS
    // rather than falling into STATUS.
    {
        for (uint16_t i = 0; i < 512; ++i)
            mem.memWrite(static_cast<uint16_t>(0x0900 + i),
                         static_cast<uint8_t>((i * 3u + 7u) & 0xFF));
        callDriver(cpu, mem, /*cmd=*/0x02, /*block=*/2, /*buffer=*/0x0900);
        if (carrySet(cpu)) {
            std::printf("FAIL: WRITE of block 2 returned carry set (A=%02X)\n",
                        cpu.getAccumulator());
            return 1;
        }
        // Read it back through the ROM, into a different buffer.
        for (uint16_t i = 0; i < 512; ++i)
            mem.memWrite(static_cast<uint16_t>(0x0800 + i), 0x00);
        callDriver(cpu, mem, /*cmd=*/0x01, /*block=*/2, /*buffer=*/0x0800);
        if (carrySet(cpu)) {
            std::printf("FAIL: READ back of block 2 returned carry set\n");
            return 1;
        }
        for (uint16_t i = 0; i < 512; ++i) {
            const uint8_t want = static_cast<uint8_t>((i * 3u + 7u) & 0xFF);
            const uint8_t got  = mem.memRead(static_cast<uint16_t>(0x0800 + i));
            if (got != want) {
                std::printf("FAIL: WRITE/READ round trip differs at +%u: "
                            "%02X want %02X\n", i, got, want);
                return 1;
            }
        }
        // Block 1 must be untouched — a write routine that ran long would
        // keep storing past its 512 bytes.
        callDriver(cpu, mem, /*cmd=*/0x01, /*block=*/1, /*buffer=*/0x0800);
        for (uint16_t i = 0; i < 16; ++i) {
            const uint8_t want = static_cast<uint8_t>((i * 5u + 11u) & 0xFF);
            if (mem.memRead(static_cast<uint16_t>(0x0800 + i)) != want) {
                std::printf("FAIL: WRITE of block 2 disturbed block 1\n");
                return 1;
            }
        }
    }

    // ── 2. 65536-block (32 MiB) volume clamps to $FFFF ──────────────────
    {
        std::vector<uint8_t> big(65536ull * kBlk, 0x00);
        assert(raw->loadImageFromBytes(std::move(big), "status-test-32MiB"));
        assert(raw->getBlockCount() == 65536u);
        callDriver(cpu, mem, /*cmd=*/0x00, /*block=*/0, /*buffer=*/0x0800);
        if (carrySet(cpu) ||
            cpu.getXRegister() != 0xFF || cpu.getYRegister() != 0xFF) {
            std::printf("FAIL: 65536-block STATUS X/Y = %02X/%02X, want "
                        "FF/FF (truncated to 0?)\n",
                        cpu.getXRegister(), cpu.getYRegister());
            return 1;
        }
    }

    // ── 3. READ with no media → carry set, A = $28 ──────────────────────
    raw->ejectImage();
    assert(!raw->isImageLoaded());
    mem.memWrite(0x0800, 0xA7);              // sentinel: must stay intact
    callDriver(cpu, mem, /*cmd=*/0x01, /*block=*/0, /*buffer=*/0x0800);
    if (!carrySet(cpu) || cpu.getAccumulator() != 0x28) {
        std::printf("FAIL: no-media READ returned A=%02X C=%d, want "
                    "A=28 C=1 (was CLC \"success\" over a $FF stream)\n",
                    cpu.getAccumulator(), carrySet(cpu) ? 1 : 0);
        return 1;
    }
    if (mem.memRead(0x0800) != 0xA7) {
        std::printf("FAIL: no-media READ clobbered the caller buffer "
                    "($0800 = %02X)\n", mem.memRead(0x0800));
        return 1;
    }
    // ── 4. WRITE with no media → carry set, A = $28 (not $2B) ──────────
    // WRITE used to test the write-protect bit and never test media at all,
    // so an empty bay answered $2B "write protected" — which is not true of a
    // bay with nothing in it, and is a code a ProDOS caller may reasonably
    // act on by telling the user to unlock the disk. $28 "no device
    // connected" is the honest answer, and it is the one READ already gave.
    //
    // Fixing it meant the write routine had to get SHORTER: it ran
    // $Cn91-$CnBF against a STATUS routine at $CnC0, so there was no room to
    // add a probe. One `BIT $C0n3` now answers both questions and both
    // routines branch to a shared error tail in the gap after boot.
    mem.memWrite(0x0900, 0x5A);
    callDriver(cpu, mem, /*cmd=*/0x02, /*block=*/0, /*buffer=*/0x0900);
    if (!carrySet(cpu) || cpu.getAccumulator() != 0x28) {
        std::printf("FAIL: no-media WRITE returned A=%02X C=%d, want "
                    "A=28 C=1 (was $2B \"write protected\" — it tested the "
                    "WP bit before asking whether media was there)\n",
                    cpu.getAccumulator(), carrySet(cpu) ? 1 : 0);
        return 1;
    }

    // ── 5. Write-protect still reports $2B, and only when media IS there ──
    // The empty-bay fix works by testing media FIRST. This is the half that
    // says it did not simply stop reporting write-protect at all — the same
    // pair smartport_rom_layout checks, for the same reason.
    {
        const std::string locked = writeLocked2mg("pom2_hdv_locked.2mg");
        assert(raw->loadImage(locked));
        assert(raw->isImageLoaded());
        mem.memWrite(0x0900, 0x5A);
        callDriver(cpu, mem, /*cmd=*/0x02, /*block=*/0, /*buffer=*/0x0900);
        if (!carrySet(cpu) || cpu.getAccumulator() != 0x2B) {
            std::printf("FAIL: write-protected WRITE returned A=%02X C=%d, "
                        "want A=2B C=1 (media-first must not stop the card "
                        "reporting write-protect)\n",
                        cpu.getAccumulator(), carrySet(cpu) ? 1 : 0);
            return 1;
        }
        // READ from a locked-but-present volume is perfectly legal.
        callDriver(cpu, mem, /*cmd=*/0x01, /*block=*/0, /*buffer=*/0x0800);
        if (carrySet(cpu)) {
            std::printf("FAIL: READ from a write-protected volume returned "
                        "carry set (A=%02X)\n", cpu.getAccumulator());
            return 1;
        }
        raw->ejectImage();
        std::error_code ec;
        std::filesystem::remove(locked, ec);
    }

    // ── 6. STATUS on an empty bay → carry set, A = $28 ─────────────────
    // It used to answer CLC with X = Y = 0: "there IS a device here and it
    // has zero blocks". That is a different claim from "no device", and a
    // ProDOS volume scanner records it as a live unit — the one thing an
    // empty bay must not look like. READ and WRITE already said $28; STATUS
    // was the third caller of the same wire and never asked (bug hunt 4 #14).
    // Only bit 7 is consulted, so a stale out-of-range block selection left
    // over from an earlier transfer cannot make STATUS fail.
    assert(!raw->isImageLoaded());
    callDriver(cpu, mem, /*cmd=*/0x00, /*block=*/0, /*buffer=*/0x0800);
    if (!carrySet(cpu) || cpu.getAccumulator() != 0x28) {
        std::printf("FAIL: empty-bay STATUS returned A=%02X C=%d, want "
                    "A=28 C=1 (was CLC with a 0-block device)\n",
                    cpu.getAccumulator(), carrySet(cpu) ? 1 : 0);
        return 1;
    }

    // ── 7. Out-of-range READ/WRITE → carry set, A = $27 ────────────────
    // A truncated .hdv, or a 2MG whose header claims more blocks than the
    // file carries, presents ProDOS with a volume bitmap that names blocks
    // the medium does not have. READ streamed 512 × $FF and WRITE dropped
    // its bytes, and BOTH returned CLC "success" — so the guest read an
    // all-$FF block as real data and believed a save that went nowhere. The
    // ATA/CFFA path has refused since it existed
    // (AtaBlockDevice::startCommand); this is the synthetic card catching up
    // (bug hunt 4 #5). $27 is ProDOS's I/O ERROR, which is what a block that
    // is not on the medium is — $28 stays reserved for an empty bay.
    {
        std::vector<uint8_t> five(5 * kBlk, 0x11);
        assert(raw->loadImageFromBytes(std::move(five), "oor-test-5blk"));
        mem.memWrite(0x0800, 0xC3);          // sentinel: must stay intact
        callDriver(cpu, mem, /*cmd=*/0x01, /*block=*/5, /*buffer=*/0x0800);
        if (!carrySet(cpu) || cpu.getAccumulator() != 0x27) {
            std::printf("FAIL: out-of-range READ returned A=%02X C=%d, want "
                        "A=27 C=1 (was CLC over a $FF stream)\n",
                        cpu.getAccumulator(), carrySet(cpu) ? 1 : 0);
            return 1;
        }
        if (mem.memRead(0x0800) != 0xC3) {
            std::printf("FAIL: out-of-range READ clobbered the caller buffer "
                        "($0800 = %02X)\n", mem.memRead(0x0800));
            return 1;
        }
        mem.memWrite(0x0900, 0x77);
        callDriver(cpu, mem, /*cmd=*/0x02, /*block=*/100, /*buffer=*/0x0900);
        if (!carrySet(cpu) || cpu.getAccumulator() != 0x27) {
            std::printf("FAIL: out-of-range WRITE returned A=%02X C=%d, want "
                        "A=27 C=1 (was CLC and the bytes were dropped)\n",
                        cpu.getAccumulator(), carrySet(cpu) ? 1 : 0);
            return 1;
        }
        if (raw->hasUnsavedChanges()) {
            std::printf("FAIL: an out-of-range WRITE dirtied the image\n");
            return 1;
        }
        // The last block IS in range and must still work — the check is
        // `>=`, not `>`, and getting that backwards would break the last
        // block of every volume.
        callDriver(cpu, mem, /*cmd=*/0x01, /*block=*/4, /*buffer=*/0x0800);
        if (carrySet(cpu) || mem.memRead(0x0800) != 0x11) {
            std::printf("FAIL: the LAST in-range block (4 of 5) was refused "
                        "(A=%02X C=%d)\n",
                        cpu.getAccumulator(), carrySet(cpu) ? 1 : 0);
            return 1;
        }
    }

    // ── 8. $CnFE advertises WRITE ──────────────────────────────────────
    // ProDOS 8 TN.PDOS.021: bit 0 = status, bit 1 = read, bit 2 = write,
    // bit 3 = format. $03 says "status + read" — a READ-ONLY device — which
    // is not what a card with a working WRITE_BLOCK is (bug hunt 4 #13).
    {
        const uint8_t fe =
            mem.memRead(static_cast<uint16_t>((kRomHi << 8) | 0xFE));
        if ((fe & 0x04) == 0) {
            std::printf("FAIL: $CnFE = $%02X — the write bit (2) is clear, "
                        "so ProDOS is told this device is read-only\n", fe);
            return 1;
        }
    }

    std::printf("OK hdv_status_driver (STATUS X/Y + clamp + read/write "
                "round trip + no-media $28 on all three + out-of-range $27 "
                "+ $CnFE write bit)\n");
    return 0;
}
