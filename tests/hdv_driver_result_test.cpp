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

// ProDOSHardDiskCard slot ROM — the full RESULT contract of every driver arm.
//
// ProDOS 8 Technical Reference § 6.3 (Calling a Device Driver): "If the
// operation is successful, the driver returns with the carry flag clear and
// with the accumulator containing 0. If not, it returns with the carry set
// and the accumulator containing the error code."
//
// `hdv_status_driver` pins the STATUS arm's A = $00 and the error codes for a
// missing medium; it checks READ's carry only. READ was the one arm that fell
// out of its 512-byte loop straight into `CLC / RTS`, so A held THE LAST BYTE
// OF THE BLOCK — a byte of the user's data sitting where an error code
// belongs. The probe that found it read $2B off a healthy block, which is
// also the ROM's own "write protected" code. WRITE and STATUS in the same
// page already load #$00, and so does SmartPortCard's read arm.
//
// This test drives the REAL slot ROM on a 6502 (same harness shape as
// hdv_status_driver) and asserts (C, A) for every arm at once, so the three
// error codes cannot drift apart from the two success cases:
//
//   READ  ok           C=0 A=$00      READ  past the end   C=1 A=$27
//   WRITE ok           C=0 A=$00      WRITE past the end   C=1 A=$27
//   STATUS ok          C=0 A=$00      WRITE to a locked medium  C=1 A=$2B
//   unknown command    C=1 A=$01      any call, empty bay       C=1 A=$28

#include "M6502.h"
#include "Memory.h"
#include "ProDOSHardDiskCard.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr int      kSlot   = 5;
constexpr size_t   kBlk    = ProDOSHardDiskCard::kBlockBytes;
constexpr uint8_t  kRomHi  = 0xC0 + kSlot;          // $C5

int failures = 0;

// JSR the card's own ProDOS driver entry, read from $CnFF rather than typed:
// the ROM is assembled from labels (SlotRomAsm.h), so a routine moving inside
// the page must not need this harness edited. ProDOS finds it the same way.
void callDriver(M6502& cpu, Memory& mem, uint8_t cmd, uint16_t block,
                uint16_t buffer)
{
    mem.memWrite(0x42, cmd);
    mem.memWrite(0x43, static_cast<uint8_t>(kSlot << 4));   // unit: slot, drive 1
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
    cpu.run(60000);
}

void expect(M6502& cpu, bool wantCarry, uint8_t wantA, const char* what)
{
    const bool carry = (cpu.getStatusRegister() & 0x01) != 0;
    const uint8_t a  = cpu.getAccumulator();
    if (carry != wantCarry || a != wantA) {
        std::printf("FAIL: %s -> C=%d A=$%02X, want C=%d A=$%02X\n",
                    what, carry ? 1 : 0, a, wantCarry ? 1 : 0, wantA);
        ++failures;
    }
}

std::string writeLocked2mg()
{
    const std::string path =
        (std::filesystem::temp_directory_path() /
         "pom2_hdv_driver_result_locked.2mg").string();
    std::vector<uint8_t> img(64 + 5 * kBlk, 0x00);
    std::memcpy(img.data(), "2IMG", 4);
    auto w32 = [&](size_t o, uint32_t v) {
        img[o + 0] = static_cast<uint8_t>(v);
        img[o + 1] = static_cast<uint8_t>(v >> 8);
        img[o + 2] = static_cast<uint8_t>(v >> 16);
        img[o + 3] = static_cast<uint8_t>(v >> 24);
    };
    w32(12, 1);                 // ProDOS block order
    w32(16, 0x80000000u);       // flags bit 31 = locked
    w32(24, 64);
    w32(28, static_cast<uint32_t>(5 * kBlk));
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(img.data()),
            static_cast<std::streamsize>(img.size()));
    return path;
}

}  // namespace

int main()
{
    Memory mem;
    M6502  cpu(&mem);
    cpu.hardReset();

    auto card = std::make_unique<ProDOSHardDiskCard>(kSlot);
    ProDOSHardDiskCard* raw = card.get();

    std::vector<uint8_t> hdv(5 * kBlk, 0x00);
    for (size_t b = 0; b < 5; ++b)
        for (size_t i = 0; i < kBlk; ++i)
            hdv[b * kBlk + i] = static_cast<uint8_t>((b * 37u + i * 5u + 11u) & 0xFF);
    if (!raw->loadImageFromBytes(hdv, "hdv-driver-result")) {
        std::printf("FAIL: could not mount the test image\n");
        return 1;
    }
    mem.slotBus().plug(kSlot, std::move(card));

    if (raw->romLayoutError()) {
        std::printf("FAIL: the slot ROM did not fit its declared layout\n");
        return 1;
    }

    // ── The success arms: carry clear AND A = $00 ───────────────────────
    callDriver(cpu, mem, /*cmd=*/0x01, /*block=*/1, /*buffer=*/0x0800);
    expect(cpu, false, 0x00, "READ of a mounted block");
    for (size_t i = 0; i < kBlk; ++i) {
        const uint8_t want = hdv[kBlk + i];
        const uint8_t got  = mem.memRead(static_cast<uint16_t>(0x0800 + i));
        if (got != want) {
            std::printf("FAIL: READ data mismatch at +%zu: $%02X want $%02X\n",
                        i, got, want);
            ++failures;
            break;
        }
    }

    for (uint16_t i = 0; i < 512; ++i)
        mem.memWrite(static_cast<uint16_t>(0x0900 + i),
                     static_cast<uint8_t>((i * 3u + 7u) & 0xFF));
    callDriver(cpu, mem, /*cmd=*/0x02, /*block=*/3, /*buffer=*/0x0900);
    expect(cpu, false, 0x00, "WRITE of a mounted block");

    callDriver(cpu, mem, /*cmd=*/0x00, /*block=*/0, /*buffer=*/0x0800);
    expect(cpu, false, 0x00, "STATUS with media present");
    if (cpu.getXRegister() != 5 || cpu.getYRegister() != 0) {
        std::printf("FAIL: STATUS X/Y = %02X/%02X, want 05/00\n",
                    cpu.getXRegister(), cpu.getYRegister());
        ++failures;
    }

    // ── The error arms ($27 / $2B / $28 / $01) ──────────────────────────
    mem.memWrite(0x0800, 0xA7);                 // sentinel
    callDriver(cpu, mem, /*cmd=*/0x01, /*block=*/5, /*buffer=*/0x0800);
    expect(cpu, true, 0x27, "READ one block past the end");
    if (mem.memRead(0x0800) != 0xA7) {
        std::printf("FAIL: a refused READ clobbered the caller buffer\n");
        ++failures;
    }

    callDriver(cpu, mem, /*cmd=*/0x01, /*block=*/0xFFFF, /*buffer=*/0x0800);
    expect(cpu, true, 0x27, "READ at block $FFFF");

    callDriver(cpu, mem, /*cmd=*/0x02, /*block=*/9, /*buffer=*/0x0900);
    expect(cpu, true, 0x27, "WRITE past the end");

    callDriver(cpu, mem, /*cmd=*/0x03, /*block=*/0, /*buffer=*/0x0800);
    expect(cpu, true, 0x01, "an unsupported command");

    {
        const std::string locked = writeLocked2mg();
        if (!raw->loadImage(locked)) {
            std::printf("FAIL: could not mount the locked 2IMG\n");
            return 1;
        }
        callDriver(cpu, mem, /*cmd=*/0x02, /*block=*/1, /*buffer=*/0x0900);
        expect(cpu, true, 0x2B, "WRITE to a locked medium");
        if (raw->hasUnsavedChanges()) {
            std::printf("FAIL: a refused WRITE dirtied a block\n");
            ++failures;
        }
        // Media questions come FIRST: an address off the end of a locked
        // medium is $27, not $2B.
        callDriver(cpu, mem, /*cmd=*/0x02, /*block=*/200, /*buffer=*/0x0900);
        expect(cpu, true, 0x27, "WRITE past the end of a locked medium");
        std::error_code ec;
        std::filesystem::remove(locked, ec);
    }

    raw->ejectImage();
    callDriver(cpu, mem, /*cmd=*/0x00, /*block=*/0, /*buffer=*/0x0800);
    expect(cpu, true, 0x28, "STATUS with an empty bay");
    callDriver(cpu, mem, /*cmd=*/0x01, /*block=*/0, /*buffer=*/0x0800);
    expect(cpu, true, 0x28, "READ with an empty bay");
    callDriver(cpu, mem, /*cmd=*/0x02, /*block=*/0, /*buffer=*/0x0900);
    expect(cpu, true, 0x28, "WRITE with an empty bay");

    if (failures) {
        std::printf("hdv_driver_result: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("hdv_driver_result: OK\n");
    return 0;
}
