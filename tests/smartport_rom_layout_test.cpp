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

// The SmartPort slot ROM is 256 hand-assembled bytes, and it ran out of room.
//
// `emit()` did `rom[pc++]` on a uint8_t with no bound, so a routine that grew
// past its budget silently overwrote its neighbour. That is what happened: the
// write-block routine ran from $Cn9C to $CnD5, straight THROUGH the ProDOS
// STATUS pre-flight that was supposed to live at $CnC0. The dispatch's
// `JMP $CnC0` landed mid-instruction inside the write loop —
//
//     C0: D3 C0 C8 D0 F8 C6 45 AD D4 C0 29 01 D0 04 ...
//
// — executed an illegal opcode, fell into the write routine's I/O-error
// branch and returned $27. So every ProDOS STATUS call answered "I/O error"
// on a perfectly healthy bay, and the only surviving fragment of STATUS was
// its tail at $CnD6. The whole routine was dead code, and nothing failed.
//
// Two things are pinned here, because the layout is only half the fix:
//
//   1. The page assembles at all (`romLayoutError`). It is written with
//      SlotRomAsm now, so a region over budget, two regions overlapping, a
//      branch that cannot reach its target and a reference to an undefined
//      label are all errors rather than bytes.
//   2. The three ProDOS entry points BEHAVE, which is what actually says the
//      routines are where the dispatch thinks they are. A relocation that
//      compiles and fits but points somewhere wrong still fails here.
//
// Both passes matter. With roms/liron.rom loaded the slot page is re-based on
// the real dump and only $Cn20-$CnE2 is overlaid with POM2's routines —
// $Cn13-$Cn1F and $CnE3-$CnFF are deliberately left as the real controller's
// identity bytes. A routine parked in one of those gaps works perfectly on the
// synthetic base and executes real Liron firmware on the authentic one, which
// is why the pre-flight and STATUS live in the $C800 bank instead. Running the
// whole test twice is what says so.
//
// Empty-bay error codes are part of (2). READ used to fall through to the
// transfer and come back $27 "I/O error"; WRITE checked write-protect BEFORE
// media and came back $2B "write protected". Neither is true of an empty bay:
// the honest answer in the ProDOS driver error set the dispatch already
// speaks ($27/$28/$2B) is $28, "no device connected". Both now pre-flight the
// bay through the shared subroutine at $Cn13.

#include "M6502.h"
#include "Memory.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "ResourcePaths.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int    kSlot       = 5;
constexpr size_t kBlockBytes = 512;
constexpr size_t kBlocks     = 1600;        // 800 K 3.5"

int failures = 0;

void expect(bool cond, const std::string& what)
{
    if (!cond) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

std::string writeSyntheticPo(const char* name)
{
    const auto p = fs::temp_directory_path() / name;
    std::vector<uint8_t> img(kBlocks * kBlockBytes, 0x00);
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(img.data()),
            static_cast<std::streamsize>(img.size()));
    return p.string();
}

struct Result { uint8_t carry; uint8_t a; uint8_t x; uint8_t y; };

} // namespace

int main()
{
    const uint8_t romHi = static_cast<uint8_t>(0xC0 + kSlot);
    const std::string po = writeSyntheticPo("pom2_sp_romlayout.po");
    const std::string lironRom = pom2::findResource("roms/liron.rom");

    for (int pass = 0; pass < 2; ++pass) {
    const bool useLiron = pass == 1;
    if (useLiron && lironRom.empty()) {
        std::printf("smartport_rom_layout: roms/liron.rom absent — "
                    "real-dump pass skipped\n");
        break;
    }
    const std::string tag = useLiron ? "[real Liron ROM] " : "[synthetic ROM] ";

    Memory mem;
    M6502  cpu(&mem);
    cpu.hardReset();

    // Bay 0 holds a writable disk, bay 1 is EMPTY — the case that was wrong.
    auto loaded = std::make_unique<pom2::SmartPort35Unit>();
    expect(loaded->loadImage(po), tag + "load the synthetic .po");
    loaded->setWriteBackEnabled(true);            // clears the WP gate

    auto card  = std::make_unique<pom2::SmartPortCard>(kSlot);
    auto* craw = card.get();
    if (useLiron) expect(card->loadLironRom(lironRom), tag + "load roms/liron.rom");
    card->setUnit(0, std::move(loaded));
    card->setUnit(1, std::make_unique<pom2::SmartPort35Unit>());
    mem.slotBus().plug(kSlot, std::move(card));

    // The two passes must actually differ, or "it works with the real dump"
    // means nothing. $Cn07 is the controller class byte: $01 (ProDOS block
    // device) on the synthetic base, $00 (SmartPort) from the real dump.
    {
        const uint8_t cn07 = mem.memRead(static_cast<uint16_t>(0xC007 + kSlot * 0x100));
        expect(cn07 == (useLiron ? 0x00 : 0x01),
               tag + "$Cn07 should distinguish the two ROM bases (got $"
               + std::to_string(cn07) + ")");
    }

    // ── 1. Nothing overflowed ────────────────────────────────────────────
    expect(!craw->romLayoutError(),
           tag + "a slot-ROM region overflowed its budget");

    // Caller: JSR $Cn50, capture A, X, Y and the carry flag.
    const uint8_t prog[] = {
        0x20, 0x50, romHi,        // JSR $Cn50
        0x85, 0x11,               // STA $11        ; A = error code
        0x86, 0x12,               // STX $12        ; X = blocks low
        0x84, 0x13,               // STY $13        ; Y = blocks high
        0xA9, 0x00, 0x2A,         // LDA #0 / ROL A ; A = carry
        0x85, 0x10,               // STA $10
        0x4C, 0x0E, 0x03          // park
    };
    for (size_t i = 0; i < sizeof prog; ++i)
        mem.memWrite(static_cast<uint16_t>(0x0300 + i), prog[i]);

    auto call = [&](uint8_t cmd, uint8_t unitByte, uint16_t block) -> Result {
        mem.memWrite(0x42, cmd);
        mem.memWrite(0x43, unitByte);
        mem.memWrite(0x44, 0x00);
        mem.memWrite(0x45, 0x08);              // buffer → $0800
        mem.memWrite(0x46, block & 0xFF);
        mem.memWrite(0x47, (block >> 8) & 0xFF);
        for (int i = 0; i < 4; ++i)
            mem.memWrite(static_cast<uint16_t>(0x10 + i), 0xEE);
        cpu.setProgramCounter(0x0300);
        cpu.run(60000);
        return { mem.memRead(0x10), mem.memRead(0x11),
                 mem.memRead(0x12), mem.memRead(0x13) };
    };

    constexpr uint8_t kDrive1 = 0x00;
    constexpr uint8_t kDrive2 = 0x80;          // unit byte bit 7 = drive 2

    // ── 2. STATUS on a healthy bay: the routine is REACHABLE ─────────────
    // This is the case the old layout could not pass. It answered $27
    // because `JMP $CnC0` ran into the write loop; there was no way to reach
    // the real routine at all, so the block count never came back.
    {
        Result r = call(0x00, kDrive1, 0);
        expect(r.carry == 0, tag + "STATUS on a loaded bay must return CLC");
        expect(r.a == 0x00,  tag + "STATUS on a loaded bay must return A = 0");
        const uint16_t blocks =
            static_cast<uint16_t>(r.x) | static_cast<uint16_t>(r.y) << 8;
        expect(blocks == kBlocks,
               tag + "STATUS must report the block count in X/Y (got "
               + std::to_string(blocks) + ")");
    }

    // ── 3. An empty bay is "no device", not "I/O error" or "protected" ───
    {
        Result r = call(0x00, kDrive2, 0);
        expect(r.carry == 1, tag + "STATUS on an empty bay must return SEC");
        expect(r.a == 0x28, tag + "STATUS on an empty bay must be $28, not $"
                            + std::to_string(r.a));
    }
    {
        Result r = call(0x01, kDrive2, 0);
        expect(r.carry == 1, tag + "READ from an empty bay must return SEC");
        expect(r.a == 0x28, tag + "READ from an empty bay must be $28 (was $27 "
                            "I/O error — it fell through to the transfer)");
    }
    {
        Result r = call(0x02, kDrive2, 0);
        expect(r.carry == 1, tag + "WRITE to an empty bay must return SEC");
        expect(r.a == 0x28, tag + "WRITE to an empty bay must be $28 (was $2B "
                            "write-protected — it checked WP before media)");
    }

    // ── 4. Write-protect reports $2B on WRITE only ───────────────────────
    // The empty-bay fix works by testing media first; this is the half that
    // proves it did not simply stop reporting write-protect at all. STATUS
    // must still succeed: ON_LINE names a locked disk.
    {
        auto wp = std::make_unique<pom2::SmartPort35Unit>();
        expect(wp->loadImage(po), tag + "reload for the WP case");
        wp->setWriteBackEnabled(false);        // WP gate on
        craw->setUnit(1, std::move(wp));

        Result r = call(0x02, kDrive2, 0);
        expect(r.carry == 1, tag + "WRITE to a write-protected bay must return SEC");
        expect(r.a == 0x2B, tag + "a LOADED but write-protected bay must be $2B");

        Result s = call(0x01, kDrive2, 0);
        expect(s.carry == 0, tag + "READ from a write-protected bay must succeed");

        // STATUS used to answer $2B here. ON_LINE treats that as "no
        // volume", so a locked 3.5" (a WOZ, a notched .2mg) vanished from
        // every ProDOS scanner. WP is WRITE's error, not STATUS's.
        Result t = call(0x00, kDrive2, 0);
        expect(t.carry == 0, tag + "STATUS on a write-protected bay must return CLC");
        expect(t.a == 0x00, tag + "STATUS on a write-protected bay must return A = 0, not $2B");
        const uint16_t wpBlocks =
            static_cast<uint16_t>(t.x) | static_cast<uint16_t>(t.y) << 8;
        expect(wpBlocks == kBlocks,
               tag + "STATUS must still report the block count on a locked bay");
    }

    }  // pass

    fs::remove(po);

    if (failures) {
        std::printf("smartport_rom_layout FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("smartport_rom_layout OK\n");
    return 0;
}
