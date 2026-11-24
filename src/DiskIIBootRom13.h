// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// DiskIIBootRom13 — POM2's own 13-sector Disk II boot PROM.
//
// The 13-sector controller (DOS 3.1 / 3.2, 1978-1980) is the one the
// original Apple ][ and the first ][+ shipped with; its 341-0009 boot PROM is
// Apple's and does not ship with POM2. Without it a 13-sector disk could not
// boot: on the woz-a-day corpus, 19 titles stayed on the `APPLE ][` banner
// in POM2, MAME and AppleWin alike (2026-11-24). This page is written from
// the CONTRACT the 13-sector boot sectors rely on, as DiskIIBootRom.h is for
// the 16-sector one; Apple's dump, when present (roms/disk2_13.rom,
// DiskIICard::loadBootRom13), is preferred.
//
// [C] The contract, read off the boot of DOS 3.2-era disks (VisiCalc 1.37,
// Battalion Commander's RDOS, Apple Bowl) on the 341-0009 dump:
//   $Cn01=$20 $Cn03=$00 $Cn05=$03 $Cn07=$3C  the Autostart signature
//   $0800+nibble  the 5-and-3 translate table: the 32 valid nibbles ($AB-$FF
//          with no two adjacent 0 bits, $D5 excepted) get 0-31 in ascending
//          order. Boot sectors use it in place: Battalion Commander's shifts
//          $0899-$08FF left three times before reading with it.
//   $Cn5D  read entry, which boot1 calls back through JMP ($003E): on the
//          current track, the sector whose address field ($D5 $AA $B5,
//          volume / track / sector in 4-and-4) names $3D, track ignored; its
//          data field ($D5 $AA $AD) translated and XOR-chained as it is read
//          — the 154 secondary values into $0899 down to $0800, the 256
//          primary values into ($26)+0..255 — then the checksum, a bad one
//          reading the sector again; RTS with A = 0 and X = slot*16.
//   boot   sector 0 into $0300, the secondary and primary joined (three
//          passes of 51: byte = primary << 3 | secondary >> 2, the
//          secondary's two low bits rotated into $0399+ and $03CC+), then
//          JMP $0301 with Y = $99 and X = $33 — Battalion Commander's boot1
//          starts with `LDA $0800,Y` and needs that Y — or the Monitor's ERR
//          ($FF2D) when the sector's first byte is not $99.
//   $2B    slot*16; $26/$27 = $0300; $3D = 0.
// The table rule, the read loop, the join and the drive set-up are this
// file's own code; the page is generated for its slot (the data register is
// `$C0nC` absolute), and buildDiskIIBootRom13() refuses a page whose read
// entry is not at $5D.

#include "SlotRomAsm.h"

#include <array>
#include <cstdint>
#include <string>

namespace pom2 {

struct DiskIIBootRom13Build {
    std::array<uint8_t, kSlotRomBytes> rom{};
    bool        ok = false;
    std::string error;
    std::string listing;
};

inline DiskIIBootRom13Build buildDiskIIBootRom13(int slot)
{
    DiskIIBootRom13Build out;
    SlotRomAsm a(out.rom, slot, "Disk II 13-sector boot PROM (POM2)");
    const auto s16 = static_cast<uint8_t>(slot << 4);
    const auto io = [s16](int reg) { return static_cast<uint8_t>(0x80 + s16 + reg); };

    a.region("init", 0x00, 0x5D)
        // The signature bytes are operands of harmless instructions; LDY #$00
        // starts the nibble walk at $FF (the first DEY).
        .emit({ 0xC9, 0x20,          // CMP #$20
                0xA0, 0x00,          // LDY #$00
                0xE0, 0x03,          // CPX #$03
                0xA5, 0x3C,          // LDA $3C
                0xA2, 0x20 })        // LDX #32        values to hand out
        // Valid 5-and-3 nibble: every adjacent pair of bits 0-7 holds a 1,
        // i.e. (n | n >> 1 | $80) = $FF; and not $D5. From $FF down they get
        // 31 down to 0.
        .label("tbl")
        .emit({ 0x88,                // DEY
                0x84, 0x3C,          // STY $3C
                0x98,                // TYA
                0x4A,                // LSR A
                0x05, 0x3C,          // ORA $3C
                0x09, 0x80,          // ORA #$80
                0xC9, 0xFF })        // CMP #$FF
        .branch(0xD0, "tbl")         // BNE            a pair of 0s
        .emit({ 0xC0, 0xD5 })        // CPY #$D5
        .branch(0xF0, "tbl")         // BEQ            reserved
        .emit({ 0xCA,                // DEX
                0x8A,                // TXA
                0x99, 0x00, 0x08 })  // STA $0800,Y
        .branch(0xD0, "tbl")         // BNE            until 32 are placed
        .emit({ 0xA2, s16,           // LDX #slot*16
                0x86, 0x2B,          // STX $2B
                0xAD, io(0x9), 0xC0, // LDA $C0n9      motor on
                0xAD, io(0xA), 0xC0, // LDA $C0nA      drive 1
                0xAD, io(0xE), 0xC0, // LDA $C0nE      read mode
                0xA0, 0x50 })        // LDY #80        half-tracks down
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
        .emit({ 0xA9, 0x03,          // LDA #$03       sector 0 into $0300
                0x85, 0x27,          // STA $27
                0xA9, 0x00,          // LDA #$00
                0x85, 0x26,          // STA $26
                0x85, 0x3D,          // STA $3D
                0xA2, s16 })         // LDX #slot*16
        .jsr("read")
        .jmp("join");

    // $Cn5D: boot1's way back in.
    a.region("read", 0x5D, 0x83)
        .emit({ 0x18 })              // CLC            looking for an address
        .label("seek")
        .emit({ 0x08 })              // PHP            the mode rides the stack
        .label("rdD5")
        .emit({ 0xAD, io(0xC), 0xC0 })  // LDA $C0nC
        .branch(0x10, "rdD5")        // BPL
        .label("isD5")
        .emit({ 0xC9, 0xD5 })        // CMP #$D5
        .branch(0xD0, "rdD5")        // BNE
        .label("rdAA")
        .emit({ 0xAD, io(0xC), 0xC0 })  // LDA $C0nC
        .branch(0x10, "rdAA")        // BPL
        .emit({ 0xC9, 0xAA })        // CMP #$AA
        .branch(0xD0, "isD5")        // BNE            maybe a D5 again
        .label("rdKind")
        .emit({ 0xAD, io(0xC), 0xC0 })  // LDA $C0nC
        .branch(0x10, "rdKind")      // BPL
        .emit({ 0x49, 0xB5 })        // EOR #$B5       A = 0 on an address
        .branch(0xF0, "addr")        // BEQ            field (C = 1)
        .emit({ 0x28 })              // PLP            the mode
        .branch(0x90, "read")        // BCC            wanted an address
        .emit({ 0x49, 0x18 })        // EOR #$B5^$AD   A = 0 on a data field
        .branch(0xF0, "data")        // BEQ
        .branch(0xD0, "read");       // BNE

    a.region("addr", 0x83, 0x9E)
        // Volume, track, sector in 4-and-4: A ends as the sector.
        .emit({ 0xA0, 0x03 })        // LDY #$03
        .label("a44")
        .emit({ 0xAD, io(0xC), 0xC0 })  // LDA $C0nC
        .branch(0x10, "a44")         // BPL
        .emit({ 0x2A,                // ROL A          carry set (EOR's BEQ)
                0x85, 0x3C })        // STA $3C
        .label("a44b")
        .emit({ 0xAD, io(0xC), 0xC0 })  // LDA $C0nC
        .branch(0x10, "a44b")        // BPL
        .emit({ 0x25, 0x3C,          // AND $3C
                0x88 })              // DEY
        .branch(0xD0, "a44")         // BNE
        .emit({ 0x28,                // PLP
                0xC5, 0x3D })        // CMP $3D
        .branch(0xD0, "read")        // BNE            another sector
        .branch(0xB0, "seek");       // BCS            C = 1: its data next

    a.region("data", 0x9E, 0xD0)
        // 154 secondary values, $0899 down to $0800, then 256 primary.
        .emit({ 0xA0, 0x9A,          // LDY #$9A       A = 0 from the EOR
                0x84, 0x3C })        // STY $3C
        .label("sec")
        .emit({ 0xAC, io(0xC), 0xC0 })  // LDY $C0nC
        .branch(0x10, "sec")         // BPL
        .emit({ 0x59, 0x00, 0x08,    // EOR $0800,Y    translate, chain
                0xA4, 0x3C,          // LDY $3C
                0x88,                // DEY
                0x99, 0x00, 0x08,    // STA $0800,Y
                0x84, 0x3C })        // STY $3C
        .branch(0xD0, "sec")         // BNE
        .label("prim")
        .emit({ 0x84, 0x3C })        // STY $3C
        .label("primRd")
        .emit({ 0xAC, io(0xC), 0xC0 })  // LDY $C0nC
        .branch(0x10, "primRd")      // BPL
        .emit({ 0x59, 0x00, 0x08,    // EOR $0800,Y
                0xA4, 0x3C,          // LDY $3C
                0x91, 0x26,          // STA ($26),Y
                0xC8 })              // INY
        .branch(0xD0, "prim")        // BNE
        .label("cksum")
        .emit({ 0xAC, io(0xC), 0xC0 })  // LDY $C0nC
        .branch(0x10, "cksum")       // BPL
        .emit({ 0x59, 0x00, 0x08 })  // EOR $0800,Y    must cancel out
        .branch(0xD0, "read")        // BNE            read it again
        .emit({ 0x60 });             // RTS            A = 0

    a.region("join", 0xD0, 0x100)
        // Three passes of 51: byte = primary << 3 | secondary >> 2; the
        // secondary's bits 0 and 1 rotate into $03CC+X and $0399+X.
        .emit({ 0xA8 })              // TAY            Y = 0
        .label("pass")
        .emit({ 0xA2, 0x00 })        // LDX #$00
        .label("j51")
        .emit({ 0xB9, 0x00, 0x08,    // LDA $0800,Y
                0x4A,                // LSR A
                0x3E, 0xCC, 0x03,    // ROL $03CC,X
                0x4A,                // LSR A
                0x3E, 0x99, 0x03,    // ROL $0399,X
                0x85, 0x3C,          // STA $3C
                0xB1, 0x26,          // LDA ($26),Y
                0x0A, 0x0A, 0x0A,    // ASL A x3
                0x05, 0x3C,          // ORA $3C
                0x91, 0x26,          // STA ($26),Y
                0xC8,                // INY
                0xE8,                // INX
                0xE0, 0x33 })        // CPX #51
        .branch(0xD0, "j51")         // BNE
        .emit({ 0xC0, 0x99 })        // CPY #$99
        .branch(0xD0, "pass")        // BNE            three passes
        .emit({ 0xCC, 0x00, 0x03 })  // CPY $0300      a boot sector says $99
        .branch(0xD0, "err")         // BNE
        .emit({ 0x4C, 0x01, 0x03 })  // JMP $0301      Y = $99, X = $33
        .label("err")
        .emit({ 0x4C, 0x2D, 0xFF }); // JMP $FF2D      the Monitor's ERR

    out.ok      = a.finish();
    out.error   = a.error();
    out.listing = a.listing();
    if (out.ok && (out.rom[0x5D] != 0x18 || out.rom[0x01] != 0x20 || out.rom[0x03] != 0x00 ||
                   out.rom[0x05] != 0x03 || out.rom[0x07] != 0x3C)) {
        out.ok = false;
        out.error = "the read entry is not at $5D, or the signature moved";
    }
    return out;
}

} // namespace pom2
