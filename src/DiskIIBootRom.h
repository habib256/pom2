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

#pragma once

// DiskIIBootRom — POM2's own 16-sector Disk II boot PROM.
//
// Apple's 341-0027-A is copyrighted and no longer ships with POM2
// (2026-10-29). This page is written from the BOOT PROTOCOL — what boot
// sectors and the Autostart ROM rely on — not from Apple's bytes, so a card
// with no roms/disk2.rom still boots DOS 3.3 and ProDOS disks. Apple's dump,
// when present, is preferred (DiskIICard::loadBootRom): a copy protection
// that leans on its exact internals only works with the real thing.
//
// The contract, from the boot sectors that call back into the PROM:
//
//   $Cn01=$20 $Cn03=$00 $Cn05=$03 $Cn07=$3C  the Autostart/ProDOS signature
//   $CnFF=$00                                ProDOS: a 16-sector Disk II
//   $Cn5C  read entry. Reads physical sector $3D of the current track into
//          the page at ($26) ($26 = 0), then INC $27, INC $3D, and loops
//          while $3D < ($0800); then JMP $0801 with A = $3D and X = slot*16.
//          DOS 3.3 boot1 re-enters here with JMP ($003E); ProDOS's block 0
//          with an RTS onto $Cn5B, and it reads A on the way back.
//   $2B    slot*16, for boot1.
//
// ProDOS asks for more than the protocol. Its block 0 reads its second
// sector through $Cn5C, then COPIES $Cn63-$CnEA into $09F7 and uses the copy
// as its own sector reader: it enters at $63 with a nibble in A, the mode
// (address or data field) pushed with PHP and X = slot*16; it rewrites the
// branch operands at $66 (not D5), $7C/$82/$9D (start over), $A3 (wrong
// track: re-seek) and $A5 (address found: data mode) to land in its retry
// loop, stores $AC at $D4, and appends LDX $2B / CLC / RTS at $EB. So the
// read routine sits on that grid — a branch before each rewritten operand,
// a harmless immediate under $D4, the sector done at $EB, X = slot*16 on
// every way out — and buildDiskIIBootRom() refuses to produce a page that
// is off it. The grid is ProDOS's interface; the 6-and-2 decode inside it is
// this file's own (below).
//
// How it fits in a page: the page is generated for its slot (as POM2's other
// hand-written slot ROMs are), so the data register is `$C0nC` absolute and
// X is free. That lets the 6-and-2 decode run DURING the read — each nibble
// through the table and the running XOR in 21 cycles, inside the 32 a nibble
// takes — with no second pass over the sector. Memory it uses: $0300-$0355
// (the 86 two-bit groups), $036C-$03D5 (nibble -> 6-bit table, indexed from
// $02D6 by the raw nibble), $0200-$02FF (the 256 six-bit groups) and zero
// page $26/$27/$2B/$3A/$3C/$3D/$40/$41 ($40 = the track the last address
// field showed, which ProDOS reads back to re-seek; $41 = the track wanted).
// The table is built from the rule the valid nibbles follow — at least one
// pair of adjacent 1s in bits 0-6, at most one pair of adjacent 0s — which
// yields exactly DiskImage's 64 (pinned).

#include "SlotRomAsm.h"

#include <array>
#include <cstdint>
#include <string>

namespace pom2 {

struct DiskIIBootRomBuild {
    std::array<uint8_t, kSlotRomBytes> rom{};
    bool        ok = false;
    std::string error;
    std::string listing;
};

inline DiskIIBootRomBuild buildDiskIIBootRom(int slot)
{
    DiskIIBootRomBuild out;
    SlotRomAsm a(out.rom, slot, "Disk II boot PROM (POM2)");
    const auto s16 = static_cast<uint8_t>(slot << 4);
    // Low byte of $C080 + slot*16 + reg: the card's soft switches.
    const auto io = [s16](int reg) { return static_cast<uint8_t>(0x80 + s16 + reg); };

    a.region("init", 0x00, 0x5C)
        // The signature bytes are operands; the opcodes around them are
        // harmless, and LDY #$00 seeds the table index.
        .emit({ 0xC9, 0x20,          // CMP #$20
                0xA0, 0x00,          // LDY #$00
                0xE0, 0x03,          // CPX #$03
                0xA5, 0x3C })        // LDA $3C
        // Nibble -> 6-bit table at $02D6+nibble, for nibbles $96-$FF.
        .emit({ 0xA2, 0x96 })        // LDX #$96
        .label("tblLoop")
        .emit({ 0x86, 0x3C,          // STX $3C
                0x8A,                // TXA
                0x4A,                // LSR A
                0x05, 0x3C,          // ORA $3C        v | v>>1
                0x49, 0xFF,          // EOR #$FF       pairs of adjacent 0s
                0x85, 0x26 })        // STA $26
        .branch(0xF0, "tblOnes")     // BEQ            none
        .emit({ 0x38,                // SEC
                0xE9, 0x01,          // SBC #$01
                0x25, 0x26 })        // AND $26        more than one?
        .branch(0xD0, "tblNext")     // BNE
        .label("tblOnes")
        .emit({ 0x8A,                // TXA
                0x4A,                // LSR A
                0x25, 0x3C,          // AND $3C        adjacent 1s...
                0x29, 0x3F })        // AND #$3F       ...in bits 0-6
        .branch(0xF0, "tblNext")     // BEQ
        .emit({ 0x98,                // TYA
                0x9D, 0xD6, 0x02,    // STA $02D6,X
                0xC8 })              // INY
        .label("tblNext")
        .emit({ 0xE8 })              // INX
        .branch(0xD0, "tblLoop")     // BNE            X ends at 0
        .emit({ 0x86, 0x26,          // STX $26        buffer $0800
                0x86, 0x3D,          // STX $3D        sector 0
                0x86, 0x41,          // STX $41        track 0
                0xA9, 0x08,          // LDA #$08
                0x85, 0x27,          // STA $27
                0xA2, s16,           // LDX #slot*16
                0x86, 0x2B,          // STX $2B
                0xAD, io(0x9), 0xC0, // LDA $C0n9      motor on
                0xAD, io(0xA), 0xC0, // LDA $C0nA      drive 1
                0xAD, io(0xE), 0xC0, // LDA $C0nE      read mode
                0xA0, 0x54 })        // LDY #84        phase steps
        // Recalibrate: walk the phases downward, one step per ~5 ms, so the
        // head reaches track 0 from anywhere up to track 40.
        .label("recal")
        .emit({ 0x98,                // TYA
                0x29, 0x03,          // AND #$03
                0x0A,                // ASL A
                0x09, s16,           // ORA #slot*16
                0xAA,                // TAX
                0xBD, 0x81, 0xC0,    // LDA $C081,X    phase on
                0xA9, 0x2C,          // LDA #$2C
                0x20, 0xA8, 0xFC,    // JSR $FCA8      monitor WAIT
                0xBD, 0x80, 0xC0,    // LDA $C080,X    phase off
                0x88 })              // DEY
        .branch(0x10, "recal")       // BPL
        .emit({ 0xEA });             // NOP            on into $Cn5C

    // $Cn5C-$CnEA is laid out on ProDOS's grid (see the header): ProDOS
    // copies $63-$EA, enters at $63 with a nibble in A and the mode on the
    // stack, rewrites the branch operands at $66 $7C $82 $9D $A3 $A5 to leave
    // into its own retry loop, writes $AC at $D4, and appends its return at
    // $EB. Every exit to ProDOS leaves X = slot*16. The regions pin the grid;
    // checkProdosGrid() below proves it.
    a.region("read", 0x5C, 0x83)     // defines the label "read"
        .emit({ 0x18 })              // CLC            looking for an address
        .label("seek")
        .emit({ 0x08 })              // PHP            the mode rides the stack
        .label("rdD5")
        .emit({ 0xAD, io(0xC), 0xC0 })  // LDA $C0nC
        .branch(0x10, "rdD5")        // BPL
        .label("isD5")               // $63: ProDOS enters here
        .emit({ 0xC9, 0xD5 })        // CMP #$D5
        .branch(0xD0, "rdD5")        // BNE      $66
        .label("rdAA")
        .emit({ 0xAD, io(0xC), 0xC0 })  // LDA $C0nC
        .branch(0x10, "rdAA")        // BPL
        .emit({ 0xC9, 0xAA })        // CMP #$AA
        .branch(0xD0, "isD5")        // BNE            maybe a D5 again
        .label("rdKind")
        .emit({ 0xAD, io(0xC), 0xC0 })  // LDA $C0nC
        .branch(0x10, "rdKind")      // BPL
        .emit({ 0x49, 0x96 })        // EOR #$96       A = 0 on an address
        .branch(0xF0, "addr")        // BEQ            field
        .emit({ 0xEA,                // NOP
                0x28 })              // PLP            the mode
        .branch(0x90, "read")        // BCC      $7C   wanted an address
        .emit({ 0x49, 0x3B })        // EOR #$96^$AD   A = 0 on a data field
        .branch(0xF0, "data")        // BEQ
        .label("badField")
        .branch(0xD0, "read");       // BNE      $82

    a.region("addr", 0x83, 0xA6)
        // Volume, track, sector in 4-and-4; $40 keeps the one before last.
        .emit({ 0xA0, 0x03 })        // LDY #$03
        .label("addr44")
        .emit({ 0x85, 0x40 })        // STA $40
        .label("rdOdd")
        .emit({ 0xAD, io(0xC), 0xC0 })  // LDA $C0nC
        .branch(0x10, "rdOdd")       // BPL
        .emit({ 0x2A,                // ROL A          carry is set
                0x85, 0x3A })        // STA $3A
        .label("rdEven")
        .emit({ 0xAD, io(0xC), 0xC0 })  // LDA $C0nC
        .branch(0x10, "rdEven")      // BPL
        .emit({ 0x25, 0x3A,          // AND $3A
                0x88 })              // DEY
        .branch(0xD0, "addr44")      // BNE            A = sector, $40 = track
        .emit({ 0x28,                // PLP
                0xC5, 0x3D })        // CMP $3D
        .branch(0xD0, "read")        // BNE      $9D   another sector
        .emit({ 0xA5, 0x40,          // LDA $40
                0xC5, 0x41 })        // CMP $41
        .branch(0xD0, "read")        // BNE      $A3   another track
        .branch(0xB0, "seek");       // BCS      $A5   PHP with C = 1: data

    a.region("data", 0xA6, 0xEB)
        // 86 + 256 nibbles, each translated and XOR-chained as it arrives.
        .emit({ 0xA2, 0x55 })        // LDX #$55       A = 0 from the EOR
        .label("aux")
        .emit({ 0xAC, io(0xC), 0xC0 })  // LDY $C0nC
        .branch(0x10, "aux")         // BPL
        .emit({ 0x59, 0xD6, 0x02,    // EOR $02D6,Y
                0x9D, 0x00, 0x03,    // STA $0300,X
                0xCA })              // DEX
        .branch(0x10, "aux")         // BPL
        .emit({ 0xE8 })              // INX            X = 0
        .label("prim")
        .emit({ 0xAC, io(0xC), 0xC0 })  // LDY $C0nC
        .branch(0x10, "prim")        // BPL
        .emit({ 0x59, 0xD6, 0x02,    // EOR $02D6,Y
                0x9D, 0x00, 0x02,    // STA $0200,X
                0xE8 })              // INX
        .branch(0xD0, "prim")        // BNE
        .emit({ 0xA2, s16 })         // LDX #slot*16   for ProDOS's retry
        .label("cksum")
        .emit({ 0xAC, io(0xC), 0xC0 })  // LDY $C0nC
        .branch(0x10, "cksum")       // BPL
        .emit({ 0x59, 0xD6, 0x02 })  // EOR $02D6,Y    must cancel out
        .branch(0xD0, "badField")    // BNE            via $81
        // Join: byte i = six bits << 2 | its two bits (swapped on disk),
        // the 86 groups taken from $0355 down, three times.
        .emit({ 0xA0, 0x00,          // LDY #$00
                0xA9, 0x00 })        // LDA #$00  $D4  ProDOS writes $AC here
        .label("join86")
        .emit({ 0xA2, 0x56 })        // LDX #$56
        .label("join")
        .emit({ 0xCA })              // DEX
        .branch(0x30, "join86")      // BMI
        .emit({ 0xB9, 0x00, 0x02,    // LDA $0200,Y
                0x5E, 0x00, 0x03,    // LSR $0300,X
                0x2A,                // ROL A
                0x5E, 0x00, 0x03,    // LSR $0300,X
                0x2A,                // ROL A
                0x91, 0x26,          // STA ($26),Y
                0xC8 })              // INY
        .branch(0xD0, "join")        // BNE
        .emit({ 0xEA });             // NOP            $EB is ProDOS's return

    a.region("next", 0xEB, 0xFF)
        // Next page, next sector, until $0800 sectors are in.
        .emit({ 0xE6, 0x3D,          // INC $3D
                0xE6, 0x27,          // INC $27
                0xA5, 0x3D,          // LDA $3D
                0xCD, 0x00, 0x08 })  // CMP $0800
        .branch(0xB0, "boot")        // BCS
        .jmp("read")
        .label("boot")
        .emit({ 0xA2, s16,           // LDX #slot*16
                0x4C, 0x01, 0x08 }); // JMP $0801      A = $3D

    a.region("id", 0xFF, 0x100).emit({ 0x00 });   // 16-sector Disk II

    out.ok      = a.finish();
    out.error   = a.error();
    out.listing = a.listing();
    if (out.ok) {
        // ProDOS's grid: branch opcodes before each operand it rewrites, a
        // harmless immediate under its $D4, and the regions filled to the
        // byte (a gap would be BRKs on the path).
        const auto& r = out.rom;
        const bool grid = r[0x65] == 0xD0 && r[0x7B] == 0x90 && r[0x81] == 0xD0 &&
                          r[0x9C] == 0xD0 && r[0xA2] == 0xD0 && r[0xA4] == 0xB0 &&
                          r[0xD3] == 0xA9 && r[0x63] == 0xC9 && r[0x82] != 0 &&
                          r[0xA5] != 0 && r[0xEA] == 0xEA && r[0x5B] == 0xEA;
        if (!grid) {
            out.ok = false;
            out.error = "the read routine is off ProDOS's grid";
        }
    }
    return out;
}

} // namespace pom2
