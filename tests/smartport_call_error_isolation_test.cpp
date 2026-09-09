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

// A SmartPort call must report ITS OWN outcome — not the last ProDOS-path
// transfer's.
//
// `SmartPortCard` has two front doors onto the same units. The ProDOS block
// driver ($Cn0A / $Cn50) streams bytes through the legacy $C0n3 register and
// latches failures in `ioError_`, which only a $C0n0/$C0n1/$C0n2 register
// write clears. The SmartPort dispatcher ($Cn0D → the $CE00 handler) never
// touches those registers — and its epilogue reads $C0nF, the post-stream
// error re-poll, on EVERY path, not just after a write stream.
//
// So one legitimately failing ProDOS call (a volume scanner reading past the
// end of the volume, ProDOS ONLINE on an empty bay, a bad-block probe) armed
// $C0nF for the rest of the session: the next SmartPort READ_BLOCK / STATUS
// delivered the payload into the caller's buffer and then returned carry-set
// with A = $27 "I/O error". A caller that believes the carry throws the good
// data away; ProDOS 8 reports I/O ERROR on a disk that is fine.
//
// Both halves are driven through the REAL firmware on a 6502 — the point is
// what the guest sees in (C, A), not what the C++ engine returns.

#include "M6502.h"
#include "Memory.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"

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

constexpr int      kSlot   = 5;
constexpr uint32_t kBlocks = 1600;            // 800 K 3.5"
constexpr uint16_t kBuf    = 0x0800;
constexpr uint16_t kPList  = 0x1000;
constexpr uint16_t kSList  = 0x1100;

/// Block N of the medium is filled with byte (0x10 + N).
std::string writeSyntheticPo()
{
    const fs::path p = fs::temp_directory_path() / "pom2_sp_err_isolation.po";
    std::vector<uint8_t> img(std::size_t(kBlocks) * 512);
    for (std::size_t b = 0; b < kBlocks; ++b)
        std::memset(img.data() + b * 512, static_cast<uint8_t>(0x10 + b), 512);
    img[2 * 512 + 4] = 0xF5;                  // "looks ProDOS" at block 2
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(img.data()),
            static_cast<std::streamsize>(img.size()));
    return p.string();
}

struct CallResult { uint8_t a = 0; bool carry = false; };

/// Emit a caller at $0300 that runs `body`, then captures P and A.
CallResult runCaller(Memory& mem, M6502& cpu,
                     const std::vector<uint8_t>& body)
{
    uint16_t a = 0x0300;
    for (uint8_t b : body) mem.memWrite(a++, b);
    mem.memWrite(a++, 0x08);                                     // PHP
    mem.memWrite(a++, 0x8D); mem.memWrite(a++, 0x00); mem.memWrite(a++, 0x20);
    mem.memWrite(a++, 0x68);                                     // PLA
    mem.memWrite(a++, 0x8D); mem.memWrite(a++, 0x03); mem.memWrite(a++, 0x20);
    mem.memWrite(a, 0x4C);                                       // park
    mem.memWrite(static_cast<uint16_t>(a + 1), static_cast<uint8_t>(a));
    mem.memWrite(static_cast<uint16_t>(a + 2), static_cast<uint8_t>(a >> 8));
    mem.memWrite(0x2000, 0xAA);
    mem.memWrite(0x2003, 0x00);
    cpu.setProgramCounter(0x0300);
    cpu.run(400000);
    return { mem.memRead(0x2000), (mem.memRead(0x2003) & 0x01) != 0 };
}

/// The ProDOS block driver at $Cn0A, params in ZP $42-$47.
CallResult prodosRead(Memory& mem, M6502& cpu, uint16_t block)
{
    mem.memWrite(0x42, 0x01);                       // READ
    mem.memWrite(0x43, 0x00);                       // unit byte, drive 1
    mem.memWrite(0x44, static_cast<uint8_t>(kBuf));
    mem.memWrite(0x45, static_cast<uint8_t>(kBuf >> 8));
    mem.memWrite(0x46, static_cast<uint8_t>(block));
    mem.memWrite(0x47, static_cast<uint8_t>(block >> 8));
    return runCaller(mem, cpu,
        { 0x20, 0x0A, static_cast<uint8_t>(0xC0 + kSlot) });
}

/// The SmartPort entry at $Cn0D: JSR, then cmd byte + param-list pointer.
CallResult smartPortCall(Memory& mem, M6502& cpu, uint8_t cmd)
{
    return runCaller(mem, cpu,
        { 0x20, 0x0D, static_cast<uint8_t>(0xC0 + kSlot), cmd,
          static_cast<uint8_t>(kPList), static_cast<uint8_t>(kPList >> 8) });
}

}  // namespace

int main()
{
    const std::string po = writeSyntheticPo();

    Memory mem;
    M6502  cpu(&mem);
    cpu.hardReset();

    auto unit = std::make_unique<pom2::SmartPort35Unit>();
    if (!unit->loadImage(po)) {
        std::printf("FAIL: could not mount the synthetic 800K image: %s\n",
                    unit->lastError().c_str());
        return 1;
    }
    // ctest runs with POM2_MEDIA_WRITE_DEFAULT=protected; this medium is a
    // temp file this test wrote itself, and case 5 needs it writable.
    unit->setWriteBackEnabled(true);
    auto card = std::make_unique<pom2::SmartPortCard>(kSlot);
    card->setUnit(0, std::move(unit));
    card->setUnit(1, std::make_unique<pom2::SmartPort35Unit>());   // empty bay
    mem.slotBus().plug(kSlot, std::move(card));

    int failures = 0;

    // ── 1. A SmartPort READ_BLOCK of block 7, clean machine ─────────────
    auto armRead = [&](uint32_t block) {
        mem.memWrite(kPList + 0, 3);                       // pcount
        mem.memWrite(kPList + 1, 1);                       // unit 1
        mem.memWrite(kPList + 2, static_cast<uint8_t>(kBuf));
        mem.memWrite(kPList + 3, static_cast<uint8_t>(kBuf >> 8));
        mem.memWrite(kPList + 4, static_cast<uint8_t>(block));
        mem.memWrite(kPList + 5, static_cast<uint8_t>(block >> 8));
        mem.memWrite(kPList + 6, static_cast<uint8_t>(block >> 16));
        for (int i = 0; i < 512; ++i)
            mem.memWrite(static_cast<uint16_t>(kBuf + i), 0xEE);
    };

    armRead(7);
    CallResult r = smartPortCall(mem, cpu, 0x01);
    if (r.carry || r.a != 0x00 || mem.memRead(kBuf) != 0x17) {
        std::printf("FAIL: baseline SmartPort READ block 7: C=%d A=$%02X "
                    "buf=$%02X (want C=0 A=$00 buf=$17)\n",
                    r.carry ? 1 : 0, r.a, mem.memRead(kBuf));
        ++failures;
    }

    // ── 2. A ProDOS-path read past the end must fail with $27 ───────────
    r = prodosRead(mem, cpu, 2000);                      // > 1600 blocks
    if (!r.carry || r.a != 0x27) {
        std::printf("FAIL: ProDOS $Cn0A read of block 2000: C=%d A=$%02X "
                    "(want C=1 A=$27)\n", r.carry ? 1 : 0, r.a);
        ++failures;
    }

    // ── 3. …and must NOT poison the next SmartPort call ─────────────────
    armRead(7);
    r = smartPortCall(mem, cpu, 0x01);
    if (r.carry || r.a != 0x00) {
        std::printf("FAIL: SmartPort READ block 7 after an unrelated ProDOS "
                    "failure: C=%d A=$%02X (want C=0 A=$00). The $CE00 "
                    "handler's $C0nF epilogue is reporting the LEGACY "
                    "ioError_ latch, which no SmartPort call ever clears.\n",
                    r.carry ? 1 : 0, r.a);
        ++failures;
    }
    if (mem.memRead(kBuf) != 0x17) {
        std::printf("FAIL: the payload itself is wrong after the error: "
                    "buf=$%02X (want $17)\n", mem.memRead(kBuf));
        ++failures;
    }

    // ── 4. Same for STATUS, which moves no data at all ──────────────────
    (void)prodosRead(mem, cpu, 2000);
    mem.memWrite(kPList + 0, 3);
    mem.memWrite(kPList + 1, 1);
    mem.memWrite(kPList + 2, static_cast<uint8_t>(kSList));
    mem.memWrite(kPList + 3, static_cast<uint8_t>(kSList >> 8));
    mem.memWrite(kPList + 4, 0x00);                      // status code $00
    mem.memWrite(kSList, 0xEE);
    r = smartPortCall(mem, cpu, 0x00);
    if (r.carry || r.a != 0x00) {
        std::printf("FAIL: SmartPort STATUS after an unrelated ProDOS "
                    "failure: C=%d A=$%02X (want C=0 A=$00)\n",
                    r.carry ? 1 : 0, r.a);
        ++failures;
    }
    if ((mem.memRead(kSList) & 0x80) == 0) {
        std::printf("FAIL: STATUS list byte 0 = $%02X — bit 7 (block device) "
                    "not set\n", mem.memRead(kSList));
        ++failures;
    }

    // ── 5. The write stream itself must still work ──────────────────────
    // $C0nF exists so a WRITE whose 512-byte stream fails to commit reports
    // $27 rather than silent success. Gating it on `spPushPages_` must not
    // disarm that path, so drive a real WRITE_BLOCK through it end to end.
    mem.memWrite(kPList + 0, 3);
    mem.memWrite(kPList + 1, 1);                         // unit 1, writable
    mem.memWrite(kPList + 2, static_cast<uint8_t>(kBuf));
    mem.memWrite(kPList + 3, static_cast<uint8_t>(kBuf >> 8));
    mem.memWrite(kPList + 4, 0x09);                      // block 9
    mem.memWrite(kPList + 5, 0x00);
    mem.memWrite(kPList + 6, 0x00);
    for (int i = 0; i < 512; ++i)
        mem.memWrite(static_cast<uint16_t>(kBuf + i), 0x5A);
    r = smartPortCall(mem, cpu, 0x02);
    if (r.carry || r.a != 0x00) {
        std::printf("FAIL: SmartPort WRITE block 9: C=%d A=$%02X "
                    "(want C=0 A=$00)\n", r.carry ? 1 : 0, r.a);
        ++failures;
    }
    armRead(9);
    r = smartPortCall(mem, cpu, 0x01);
    int bad = 0;
    for (int i = 0; i < 512; ++i)
        if (mem.memRead(static_cast<uint16_t>(kBuf + i)) != 0x5A) ++bad;
    if (r.carry || bad) {
        std::printf("FAIL: write/read-back of block 9: C=%d, %d/512 bytes "
                    "wrong\n", r.carry ? 1 : 0, bad);
        ++failures;
    }

    fs::remove(po);
    if (failures == 0)
        std::printf("smartport_call_error_isolation: OK — a SmartPort call "
                    "reports its own outcome, and a failing write still "
                    "reports $27\n");
    return failures == 0 ? 0 : 1;
}
