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

// Snapshot full-state round-trip regression test.
//
// Two bugs this pins (round-2 audit):
//   * The snapshot CPU section recorded A/X/Y/P/SP but load() discarded
//     them — M6502 only had setProgramCounter. New register setters let the
//     restore reconstruct the full CPU state.
//   * The snapshot MEM section captured only the visible main 64 KB; aux
//     RAM, Language-Card RAM, RamWorks banks, the IIe paging soft-switches,
//     and DisplayState were never serialized, so any IIe/aux/LC program
//     restored garbage. Memory::appendSnapshotState / loadSnapshotState now
//     round-trip that extended state.
//
// Drives M6502 + Memory directly (no AiControlServer / sockets); the server
// just calls these same setters / (de)serializers.

#include "M6502.h"
#include "Memory.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::printf("FAIL: %s\n", msg); ++failures; } } while (0)

// Page in Language-Card bank-1 RAM, read+write enabled (two consecutive
// odd $C08B accesses arm the sticky write-enable).
void pageLcBank1RW(Memory& m) {
    (void)m.memRead(0xC08B);
    (void)m.memRead(0xC08B);
}

}  // namespace

int main()
{
    // ── Part A: M6502 register setters (CPU section restore) ─────────────
    {
        Memory mem; mem.setTestMode(true);
        M6502 cpu(&mem);
        cpu.setProgramCounter(0x1234);
        cpu.setAccumulator(0xA5);
        cpu.setXRegister(0x11);
        cpu.setYRegister(0x22);
        cpu.setStatusRegister(0xC3);
        cpu.setStackPointer(0xF0);
        cpu.setCpuMode(M6502::CpuMode::CMOS);
        CHECK(cpu.getProgramCounter() == 0x1234, "PC setter");
        CHECK(cpu.getAccumulator()    == 0xA5,   "A setter");
        CHECK(cpu.getXRegister()      == 0x11,   "X setter");
        CHECK(cpu.getYRegister()      == 0x22,   "Y setter");
        CHECK(cpu.getStatusRegister() == 0xC3,   "P setter");
        CHECK(cpu.getStackPointer()   == 0xF0,   "SP setter");
    }

    // ── Part B: Memory extended-state blob round-trip ────────────────────
    {
        Memory src; src.setIIEMode(true);
        // Paging soft-switches (write-only on //e): 80STORE + RAMRD + RAMWRT.
        src.memWrite(0xC001, 0);   // 80STORE on
        src.memWrite(0xC003, 0);   // RAMRD on
        src.memWrite(0xC005, 0);   // RAMWRT on
        // DisplayState via display switches: HIRES on, PAGE2 on.
        src.memWrite(0xC057, 0);   // HIRES on
        src.memWrite(0xC055, 0);   // PAGE2 on
        // Aux RAM sentinels (written straight to the aux array).
        uint8_t* sa = src.auxDataMutable();
        sa[0x2000] = 0xBE; sa[0x5000] = 0xEF; sa[0xBFFF] = 0x42;
        // Cycle counter.
        src.setCycleCounter(0x00DEADBEEFull);
        // Language-Card RAM (bank 1 + shared high) via the paging path.
        pageLcBank1RW(src);
        src.memWrite(0xD000, 0x77);   // → lcBank1[0]
        src.memWrite(0xE000, 0x88);   // → lcHigh[0]

        const uint16_t srcMode = src.iieModeFlags();
        const auto     srcDisp = src.getDisplayState();

        std::vector<uint8_t> blob;
        src.appendSnapshotState(blob);
        CHECK(!blob.empty(), "blob produced");

        // Fresh target in a different state.
        Memory dst; dst.setIIEMode(true);
        const bool ok = dst.loadSnapshotState(blob.data(), blob.size());
        CHECK(ok, "loadSnapshotState ok");

        CHECK(dst.iieModeFlags() == srcMode, "iieMemMode round-trip");
        const auto d = dst.getDisplayState();
        CHECK(d.hiRes == srcDisp.hiRes && d.hiRes,           "DisplayState hiRes");
        CHECK(d.page2 == srcDisp.page2 && d.page2,           "DisplayState page2");
        CHECK(d.eightyStore == srcDisp.eightyStore,          "DisplayState 80store");
        CHECK(dst.getCycleCounter() == 0x00DEADBEEFull,      "cycleCounter round-trip");

        const uint8_t* da = dst.auxData();
        CHECK(da[0x2000] == 0xBE, "aux $2000 round-trip");
        CHECK(da[0x5000] == 0xEF, "aux $5000 round-trip");
        CHECK(da[0xBFFF] == 0x42, "aux $BFFF round-trip");

        // LC RAM: read back through the same paging path.
        pageLcBank1RW(dst);
        CHECK(dst.memRead(0xD000) == 0x77, "LC bank1 $D000 round-trip");
        CHECK(dst.memRead(0xE000) == 0x88, "LC high  $E000 round-trip");
    }

    // ── Part B2: annunciators AN0-AN2 travel, and a reset clears them ────
    // AN2 drives A12 of an 8 KB international character generator, so it is
    // not bookkeeping: it picks which 4 KB font the machine renders
    // (`charRomBankOffset`). It was in neither the snapshot trailer nor
    // `resetSoftSwitches`, so a restore or a rewind of a French Touch "Block
    // ASCII" screen came back in the other font, and a reset left the wrong
    // one selected. The 74LS259 annunciator latch has its /CLR on the reset
    // line — MAME `apple2e.cpp` machine_reset zeroes m_an0..m_an3.
    {
        // A synthetic 8 KB two-set char ROM: bank 0 all $11, bank 1 all $22.
        // Content is irrelevant here — only which HALF is selected is.
        const std::filesystem::path chr =
            std::filesystem::temp_directory_path() / "pom2_an2_dualbank.chr";
        {
            std::vector<uint8_t> rom(8192, 0x11);
            std::fill(rom.begin() + 4096, rom.end(), 0x22);
            std::ofstream f(chr, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(rom.data()),
                    static_cast<std::streamsize>(rom.size()));
        }

        Memory src; src.setIIEMode(true);
        CHECK(src.loadCharRom(chr.string().c_str(), /*bank=*/-1) != 0,
              "8 KB dual-bank char ROM loaded");
        CHECK(src.charRomIsDualBank(),      "dual-bank flag");
        CHECK(src.charRomBankOffset() == 0, "AN2 starts clear");

        (void)src.memRead(0xC05D);          // AN2 on → second 4 KB set
        CHECK(src.charRomBankOffset() == 4096, "AN2 selects the high bank");

        std::vector<uint8_t> blob;
        src.appendSnapshotState(blob);

        // Standalone: the 74LS259's /CLR rides the reset line, so a reset
        // drops AN2 whether or not the snapshot ever carried it.
        src.resetSoftSwitches();
        CHECK(src.charRomBankOffset() == 0,
              "resetSoftSwitches left AN2 set (stale font after Ctrl-Reset)");

        Memory dst; dst.setIIEMode(true);
        CHECK(dst.loadCharRom(chr.string().c_str(), -1) != 0, "dst char ROM");
        CHECK(dst.loadSnapshotState(blob.data(), blob.size()), "AN2 blob loads");
        CHECK(dst.charRomBankOffset() == 4096,
              "AN2 did not survive the snapshot — the restored machine renders "
              "the wrong font");

        // …and a reset drops the latch.
        dst.resetSoftSwitches();
        CHECK(dst.charRomBankOffset() == 0,
              "resetSoftSwitches left AN2 set");

        // An older 4-byte IOU section (no annunciators) must still load, and
        // must leave the live values alone — the documented trailer rule.
        std::vector<uint8_t> shortBlob;
        {
            Memory old; old.setIIEMode(true);
            old.appendSnapshotState(shortBlob);
            // Rebuild the blob the way a pre-2026-09-06 build wrote it: the
            // IOU section carried 4 bytes and was the LAST thing in the
            // trailer. Today it carries 9 (AN0/AN1/AN2 + vblWasActive +
            // iicCardWindow_) and is followed by three more optional
            // length-prefixed sections — the No-Slot Clock and the two
            // on-board Sony 3.5" mechanisms — each an empty (zero-length)
            // section on a bare Memory with none of them wired.
            constexpr size_t kEmptyTail = 3 * 4;   // three zero length prefixes
            constexpr size_t kIouNow    = 9;
            constexpr size_t kIouLegacy = 4;
            CHECK(shortBlob.size() > kEmptyTail + kIouNow + 4,
                  "blob long enough to truncate");
            shortBlob.resize(shortBlob.size() - kEmptyTail
                             - (kIouNow - kIouLegacy));
            // Length prefix is 4 bytes LE and the value fits in the low one.
            shortBlob[shortBlob.size() - kIouLegacy - 4] =
                static_cast<uint8_t>(kIouLegacy);
        }
        Memory legacy; legacy.setIIEMode(true);
        CHECK(legacy.loadCharRom(chr.string().c_str(), -1) != 0, "legacy chr");
        (void)legacy.memRead(0xC05D);
        CHECK(legacy.loadSnapshotState(shortBlob.data(), shortBlob.size()),
              "a 4-byte IOU section still loads");
        CHECK(legacy.charRomBankOffset() == 4096,
              "a blob that predates the annunciators must keep the live value");

        std::filesystem::remove(chr);
    }

    // ── Part C: restoreMainRam restores RAM cells ────────────────────────
    {
        Memory src;
        src.writeRamUnchecked(0x1000, 0xAB);
        src.writeRamUnchecked(0xBFFF, 0xCD);
        std::vector<uint8_t> snap(0x10000);
        for (size_t i = 0; i < 0x10000; ++i) snap[i] = src.data()[i];

        Memory dst;
        dst.restoreMainRam(snap.data(), snap.size());
        CHECK(dst.data()[0x1000] == 0xAB, "restoreMainRam $1000");
        CHECK(dst.data()[0xBFFF] == 0xCD, "restoreMainRam $BFFF");
    }

    if (failures == 0) std::printf("OK snapshot_state_roundtrip\n");
    return failures == 0 ? 0 : 1;
}
