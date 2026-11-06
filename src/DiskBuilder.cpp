// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// DiskBuilder — see DiskBuilder.h. This file holds the two boot sectors (each
// hand-assembled and listed beside its bytes), the DOS 3.3 VTOC/catalog
// layout, and the sector-order tables.
//
// Evidence, as CLAUDE.md grades it. The three boot PROMs were checked by
// disassembling them on 2026-11-05:
//   Apple 341-0027-A    roms/disk2.rom (not shipped; read here, not copied)
//   POM2's own          src/DiskIIBootRom.h (the listing in that file)
//   //c built-in        roms/apple2c-32Kv0.rom, bank 0, $C600 page
// and the Monitor entries in roms/apple2o.rom, apple2p.rom,
// apple2e_unenh.rom, apple2e.rom (enhanced) and apple2c-32Kv0.rom.
// No byte of any of them is in this file.

#include "DiskBuilder.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace pom2::diskbuild {

namespace {

// ── Sector orders ──────────────────────────────────────────────────────────
//
// [B] Beneath Apple DOS (Worth & Lechner, 1981), Figure 3.24 "Comparison of
//     sector skewing" (p. 3-23): physical sector 0-F holds DOS 3.3 logical
//     sector 0 7 E 6 D 5 C 4 B 3 A 2 9 1 8 F, and Pascal logical sector
//     0 8 1 9 2 A 3 B 4 C 5 D 6 E 7 F. The prose on p. 3-22 gives the other
//     direction of the first: "logical sector 2 … physical sector 11 ($0B)"
//     (fourth printing, May 1982 — archive.org "Beneath_Apple_DOS_OCR").
//     The online text transcription (github knghtbrd/beneath-apple-dos,
//     ch03.txt) words it backwards — "physical sector 2 … as sector 11" —
//     which Figure 3.24 contradicts: physical 2 is logical 14.
// [B] Beneath Apple ProDOS (1985), reference card "Block / physical sector":
//     a 5.25" track's blocks 0-7 are physical sectors 0&2, 4&6, 8&A, C&E,
//     1&3, 5&7, 9&B, D&F — i.e. the Pascal order above, two sectors a block.
// A .dsk stores a track's sectors by DOS logical number and a .po by ProDOS
// (= Pascal) logical number, so "the index in the file" of a physical sector
// is exactly these two tables. They are the same tables as
// kDos33LogicalForPhysical / kProDosLogicalForPhysical in DiskImage.cpp and
// agree with Dos33DiskFs.cpp's kDosToProdos (pinned by disk_builder_test).
constexpr int kDosIndexForPhysical[16]    = { 0, 7, 14, 6, 13, 5, 12, 4, 11, 3, 10, 2, 9, 1, 8, 15 };
constexpr int kProdosIndexForPhysical[16] = { 0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15 };

constexpr std::size_t offsetOf(int track, int fileIndex)
{
    return (std::size_t(track) * std::size_t(kSectorsPerTrack) + std::size_t(fileIndex)) * kSectorBytes;
}

// ── "Not a startup disk" ───────────────────────────────────────────────────
//
// Hand-assembled for POM2 (no source to credit). It runs at $0800 after the
// PROM has read it (byte 0 = 1: this sector only).
//
//   addr   bytes      instruction       why
//   0800   01         .byte $01         the PROM's sector count: just this one
//   0801   A6 2B      LDX $2B           slot*16, left by every boot PROM
//   0803   BD 88 C0   LDA $C088,X       drive motor off
//   0806   20 89 FE   JSR $FE89         SETKBD: IN#0 — after IN#6 the input
//   0809   20 93 FE   JSR $FE93         SETVID: PR#0 — and after PR#6 the
//                                       output hook point at $C600, so COUT
//                                       would re-boot the disk
//   080C   20 2F FB   JSR $FB2F         INIT: text mode, full window
//   080F   20 58 FC   JSR $FC58         HOME: clear the screen
//   0812   AC 25 08   LDY $0825         index of the next character, kept in
//                                       MEMORY: nothing is assumed about what
//                                       COUT preserves
//   0815   B9 26 08   LDA $0826,Y
//   0818   F0 08      BEQ $0822         $00 ends the message
//   081A   20 ED FD   JSR $FDED         COUT
//   081D   EE 25 08   INC $0825
//   0820   D0 F0      BNE $0812         always taken: the message is < 255 bytes
//   0822   4C 69 FF   JMP $FF69         MONZ: the Monitor's `*` prompt
//   0825   00         .byte $00         the index
//   0826   ...        message, high-bit ASCII, then $00
//
// [C] Monitor entries: $FE89 SETKBD, $FE93 SETVID, $FB2F INIT, $FC58 HOME,
//     $FDED COUT (JMP ($0036)), $FF69 MONZ — each found at that address in
//     the ][ (apple2o), ][+ (apple2p), //e (apple2e_unenh), enhanced //e
//     (apple2e) and //c (apple2c-32Kv0, bank 0) dumps. MONZ prints its own
//     carriage return before the prompt (JSR $FD67, GETLNZ).
// [B] Beneath Apple DOS ch. 8, Boot 1 at $0839: "Perform a PR#0 and IN#0 by
//     calling the monitor. Initialize the monitor (TEXT mode, standard
//     window …)" — the same three calls, for the same reason.
// [C] $2B = slot*16: Apple's PROM $C62C STA $2B; POM2's DiskIIBootRom.h
//     init region (STX $2B, "$2B slot*16, for boot1" in its contract); the
//     //c's $C60B STX $2B. $C088 + slot*16 is MOTOR OFF on the Disk II and
//     on the //c's IWM alike.
constexpr uint8_t kNotBootableCode[] = {
    0x01,
    0xA6, 0x2B,
    0xBD, 0x88, 0xC0,
    0x20, 0x89, 0xFE,
    0x20, 0x93, 0xFE,
    0x20, 0x2F, 0xFB,
    0x20, 0x58, 0xFC,
    0xAC, 0x25, 0x08,
    0xB9, 0x26, 0x08,
    0xF0, 0x08,
    0x20, 0xED, 0xFD,
    0xEE, 0x25, 0x08,
    0xD0, 0xF0,
    0x4C, 0x69, 0xFF,
    0x00,
};
constexpr std::size_t kNotBootableMessageAt = 0x26;
static_assert(sizeof(kNotBootableCode) == kNotBootableMessageAt, "listing and bytes disagree");
static_assert(kNotBootableMessageAt + sizeof(kNotStartupMessage) <= kSectorBytes,
              "the message and its $00 must fit in the sector");
static_assert(sizeof(kNotStartupMessage) - 1 < 255, "the BNE loop needs fewer than 255 characters");

// ── Direct-boot loader ─────────────────────────────────────────────────────
//
// [D] Casso DirectBootBuilder.cpp @ 2d899adf (MIT) — the 128 bytes below are
//     Casso's s_kBootLoader byte for byte, with its layout (order at $08E0,
//     variables at $08F0-$08F4, entry operand at $0854). Re-verified for POM2
//     by disassembly: every branch offset and absolute operand matches the
//     listing, and disk_builder_test walks it and runs it.
//
// The trick: the PROM's read loop stops when $3D reaches the byte at $0800
// and then jumps to $0801. The loader sets $3D = the sector it wants, $0800 =
// that + 1, $26/$27 = the page, $41 = the track, and re-enters the PROM's
// read entry $Cs5C: the PROM reads that ONE sector and comes back to $0801,
// where the loader takes the next. One sector per call is what lets it pick
// the ORDER (directBootReadOrder). The PROM never moves the head after its
// recalibrate, so the loader steps it itself.
//
//   addr   bytes      label    instruction     why
//   0800   01                  .byte $01       PROM: read this sector only
//   0801   AD F0 08   enter    LDA $08F0       sectors left
//   0804   F0 48               BEQ done ($084E)
//   0806   AD F4 08            LDA $08F4       index in the track's order
//   0809   D0 08               BNE onTrack ($0813)
//   080B   20 56 08            JSR step ($0856) index 0: next track first
//   080E   AD F2 08            LDA $08F2       track
//   0811   85 41               STA $41         the PROM compares the address
//                                              field's track with $41
//   0813   AE F4 08   onTrack  LDX $08F4
//   0816   BD E0 08            LDA $08E0,X     physical sector to ask for
//   0819   85 3D               STA $3D
//   081B   18                  CLC
//   081C   69 01               ADC #$01
//   081E   8D 00 08            STA $0800       the PROM stops after it
//   0821   A9 00               LDA #$00
//   0823   85 26               STA $26         whole pages: ($26) = page:00
//   0825   AD F1 08            LDA $08F1
//   0828   85 27               STA $27
//   082A   EE F1 08            INC $08F1       next page
//   082D   CE F0 08            DEC $08F0       one sector fewer
//   0830   EE F4 08            INC $08F4
//   0833   AD F4 08            LDA $08F4
//   0836   29 0F               AND #$0F        sixteen to a track
//   0838   8D F4 08            STA $08F4
//   083B   A5 2B               LDA $2B         slot*16
//   083D   4A 4A 4A 4A         LSR ×4          slot
//   0841   09 C0               ORA #$C0
//   0843   85 3F               STA $3F
//   0845   A9 5C               LDA #$5C
//   0847   85 3E               STA $3E         ($3E) = $Cs5C, the read entry
//   0849   A6 2B               LDX $2B         X = slot*16 for the PROM
//   084B   6C 3E 00            JMP ($003E)     → $Cs5C; comes back at $0801
//   084E   A6 2B      done     LDX $2B
//   0850   BD 88 C0            LDA $C088,X     motor off
//   0853   4C 00 00            JMP entry       operand patched ($0854/$0855)
//   0856   A0 02      step     LDY #$02        two phases = one track
//   0858   AD F3 08   phase    LDA $08F3
//   085B   0A                  ASL A
//   085C   05 2B               ORA $2B
//   085E   AA                  TAX
//   085F   BD 80 C0            LDA $C080,X     this magnet off
//   0862   EE F3 08            INC $08F3
//   0865   AD F3 08            LDA $08F3
//   0868   29 03               AND #$03
//   086A   8D F3 08            STA $08F3
//   086D   0A                  ASL A
//   086E   05 2B               ORA $2B
//   0870   AA                  TAX
//   0871   BD 81 C0            LDA $C081,X     the next one on
//   0874   A9 56               LDA #$56
//   0876   20 A8 FC            JSR $FCA8       Monitor WAIT, ~19 ms settle
//   0879   88                  DEY
//   087A   D0 DC               BNE phase ($0858)
//   087C   EE F2 08            INC $08F2       track + 1
//   087F   60                  RTS
//   0880-08DF  zero
//   08E0-08EF  read order (directBootReadOrder)
//   08F0 sectors left · 08F1 load page · 08F2 track (0) · 08F3 phase (0)
//   08F4 index (0)
//
// What it relies on, and where each was verified:
// [C] $Cs5C is the read entry, X = slot*16 on entry, and the loop is
//     "INC $27, INC $3D, CMP $0800, back to $0801 with X = slot*16":
//       Apple 341-0027-A: $C65C CLC / $C65D PHP; reads via LDA $C08C,X;
//         $C69E-$C6A2 LDA $40 / CMP $41 / BNE (the track check, hence $41);
//         $C6EB-$C6F8 INC $27, INC $3D, LDA $3D, CMP $0800, LDX $2B,
//         BCC (back to the read), JMP $0801.
//       POM2's own (DiskIIBootRom.h): the contract in its header ("$Cn5C read
//         entry …", "$2B slot*16"), region "read" at $5C, the track check
//         LDA $40 / CMP $41 in region "addr", region "next" (INC $3D, INC $27,
//         CMP $0800, LDX #slot*16, JMP $0801). It writes STA ($26),Y with Y
//         from 0, hence $26 = 0.
//       //c built-in: $C65C BRA to a retry-count reset that reloads X from
//         $2B itself; $C69E-$C6A2 the same $40/$41 check; $C6EB-$C6F8 the
//         same loop, LDX $4F (= $60, stored at $C60D) before JMP $0801.
//     DOS 3.3's Boot 1 re-enters the PROM the same way — [B] Beneath Apple
//     DOS ch. 8, Boot 1: "Create the address of the ROM sector read
//     subroutine (C65C in our case) and store it at $3E,$3F … It will return
//     to $801 when the sector has been read."
// [C] The head is at phase 0's position on track 0 when $0801 first runs, so
//     `phase` and `track` start at 0: Apple's recalibrate ($C63B-$C650,
//     LDY #$50 … BPL) ends with magnet 0 ON; POM2's (DiskIIBootRom.h,
//     "recal", LDY #84 … BPL) ends with magnet 0 switched OFF again, the head
//     still there; the //c's ($C61D-$C632) is Apple's. Either way "phase 0
//     off, phase 1 on, phase 1 off, phase 2 on" walks the head to track 1.
// [C] $FCA8 WAIT clobbers A only (SEC / PHA / SBC #1 / BNE / PLA / SBC #1 /
//     BNE / RTS) in all five Monitor dumps above, so Y survives as the step
//     counter. A = $56 is Apple's PROM's own settle (LDA #$56 / JSR $FCA8 at
//     $C64A); POM2's PROM uses $2C.
// [C] $0200-$03FF is PROM workspace (Apple's and the //c's table at
//     $036C-$03D5 + $0300-$0355; POM2's also $0200-$02FF), built once at boot
//     and needed on every re-entry: hence a window from $0900.
constexpr uint8_t kLoaderCode[kLoaderCodeBytes] = {
    0x01,
    0xAD, 0xF0, 0x08,   0xF0, 0x48,         0xAD, 0xF4, 0x08,   0xD0, 0x08,
    0x20, 0x56, 0x08,   0xAD, 0xF2, 0x08,   0x85, 0x41,
    0xAE, 0xF4, 0x08,   0xBD, 0xE0, 0x08,   0x85, 0x3D,         0x18,
    0x69, 0x01,         0x8D, 0x00, 0x08,   0xA9, 0x00,         0x85, 0x26,
    0xAD, 0xF1, 0x08,   0x85, 0x27,         0xEE, 0xF1, 0x08,   0xCE, 0xF0, 0x08,
    0xEE, 0xF4, 0x08,   0xAD, 0xF4, 0x08,   0x29, 0x0F,         0x8D, 0xF4, 0x08,
    0xA5, 0x2B,         0x4A, 0x4A, 0x4A, 0x4A,                 0x09, 0xC0,
    0x85, 0x3F,         0xA9, 0x5C,         0x85, 0x3E,         0xA6, 0x2B,
    0x6C, 0x3E, 0x00,
    0xA6, 0x2B,         0xBD, 0x88, 0xC0,   0x4C, 0x00, 0x00,
    0xA0, 0x02,         0xAD, 0xF3, 0x08,   0x0A,               0x05, 0x2B,
    0xAA,               0xBD, 0x80, 0xC0,   0xEE, 0xF3, 0x08,   0xAD, 0xF3, 0x08,
    0x29, 0x03,         0x8D, 0xF3, 0x08,   0x0A,               0x05, 0x2B,
    0xAA,               0xBD, 0x81, 0xC0,   0xA9, 0x56,         0x20, 0xA8, 0xFC,
    0x88,               0xD0, 0xDC,         0xEE, 0xF2, 0x08,   0x60,
};
static_assert(sizeof(kLoaderCode) == 0x80, "the listing ends at $087F");
static_assert(kLoaderReadOrder + 16 <= kLoaderSectorsLeft, "order and variables overlap");

// The most sectors any payload takes, and the tracks they need: memory runs
// out before the disk does, so the loader never steps past track 34.
constexpr std::size_t kMostSectors = (kDirectBootCeiling - kDirectBootLowest) / kSectorBytes;   // 183
static_assert(kMostSectors <= std::size_t(kTracks - kDirectBootFirstTrack) * std::size_t(kSectorsPerTrack),
              "the payload must fit on tracks 1-34");
static_assert(kMostSectors < 256, "the sector count is one byte at $08F0");

std::string hex4(uint32_t v)
{
    char text[8];
    std::snprintf(text, sizeof(text), "$%04X", unsigned(v & 0xFFFF));
    return text;
}

} // namespace

// ── Orders ─────────────────────────────────────────────────────────────────

int dosFileIndexForPhysicalSector(int physicalSector)
{
    return physicalSector >= 0 && physicalSector < kSectorsPerTrack
               ? kDosIndexForPhysical[physicalSector] : -1;
}

int prodosFileIndexForPhysicalSector(int physicalSector)
{
    return physicalSector >= 0 && physicalSector < kSectorsPerTrack
               ? kProdosIndexForPhysical[physicalSector] : -1;
}

std::vector<uint8_t> dosOrderToProdosOrder(const std::vector<uint8_t>& dosOrdered)
{
    if (dosOrdered.size() != kImageBytes) return {};
    std::vector<uint8_t> out(kImageBytes, 0);
    for (int t = 0; t < kTracks; ++t)
        for (int p = 0; p < kSectorsPerTrack; ++p)
            std::copy_n(dosOrdered.begin() + std::ptrdiff_t(offsetOf(t, kDosIndexForPhysical[p])),
                        kSectorBytes,
                        out.begin() + std::ptrdiff_t(offsetOf(t, kProdosIndexForPhysical[p])));
    return out;
}

std::vector<uint8_t> prodosOrderToDosOrder(const std::vector<uint8_t>& prodosOrdered)
{
    if (prodosOrdered.size() != kImageBytes) return {};
    std::vector<uint8_t> out(kImageBytes, 0);
    for (int t = 0; t < kTracks; ++t)
        for (int p = 0; p < kSectorsPerTrack; ++p)
            std::copy_n(prodosOrdered.begin() + std::ptrdiff_t(offsetOf(t, kProdosIndexForPhysical[p])),
                        kSectorBytes,
                        out.begin() + std::ptrdiff_t(offsetOf(t, kDosIndexForPhysical[p])));
    return out;
}

// ── Not a startup disk ─────────────────────────────────────────────────────

std::vector<uint8_t> notBootableSector()
{
    std::vector<uint8_t> s(kSectorBytes, 0);
    std::copy(std::begin(kNotBootableCode), std::end(kNotBootableCode), s.begin());
    std::size_t at = kNotBootableMessageAt;
    for (const char* c = kNotStartupMessage; *c; ++c) s[at++] = uint8_t(uint8_t(*c) | 0x80);
    s[at] = 0x00;
    return s;
}

// ── Blank DOS 3.3 data disk ────────────────────────────────────────────────
//
// [B] Beneath Apple DOS ch. 4, "The VTOC" (the VTOC format table) for every
//     field set below; ch. 4 "The Catalog" for the chain; ch. 8,
//     "AE8E-AF07 INIT function handler" for the values DOS's own INIT writes.
//
// Free space. DOS's INIT frees every track but 0-2 (its DOS image) and 17
// (VTOC + catalog): 560 - 64 = 496 free sectors (ch. 8, AE8E: "Skipping the
// first three tracks and track $11, copy the 4 byte bit mask … to free the
// sectors"). A data disk carries no DOS, so tracks 1 and 2 are freed too:
// 528 free, 32 more. Track 0 stays in use even without DOS: a catalog entry
// whose T/S-list track is $00 means "never used" (ch. 4, File Descriptive
// Entry, byte $00: "This means track 0 can never be used for data even if the
// DOS image is 'wiped' from the diskette"), and the boot sector lives there.
// Tracks 1-2 really are allocatable by DOS: its allocator walks the tracks
// from the last one allocated, stops only at track 0 and past track 34
// (ch. 8, "B244-B2C2 Allocate a disk sector", B272: "Are we back to track
// 0?"), and takes any track whose bit map shows a free sector.
std::vector<uint8_t> blankDos33(uint8_t volume)
{
    if (volume == 0 || volume == 255) return {};   // VTOC $06: 1-254
    std::vector<uint8_t> img(kImageBytes, 0);

    const std::vector<uint8_t> boot = notBootableSector();
    std::copy(boot.begin(), boot.end(), img.begin() + std::ptrdiff_t(offsetOf(0, 0)));

    constexpr int kVtocTrack = 17;
    uint8_t* v = img.data() + offsetOf(kVtocTrack, 0);   // VTOC: track 17 sector 0
    // $00 "Not used": left 0. [F] DOS 3.3-INITed disks are commonly seen with
    //     $04 here; nothing that reads a VTOC (DOS, Dos33DiskFs) looks at it.
    //     Settled by: a byte dump of a disk INITed by DOS 3.3 on real iron.
    v[0x01] = kVtocTrack;   // track of the first catalog sector
    v[0x02] = 15;           // sector of the first catalog sector
    v[0x03] = 3;            // release of the DOS that INITed it: 3
    v[0x06] = volume;       // volume number (1-254)
    v[0x27] = 122;          // T/S pairs per T/S list sector (256-byte sectors)
    v[0x30] = kVtocTrack;   // last track allocated: INIT writes $11 (ch. 8, AE8E)
    v[0x31] = 0x01;         // direction +1 (ch. 8, AE8E): the first search is 18
    v[0x34] = kTracks;      // 35
    v[0x35] = kSectorsPerTrack;   // 16
    v[0x36] = 0x00;         // bytes per sector, LO
    v[0x37] = 0x01;         //                   HI: 256
    // Bit map from $38: four bytes a track, bit set = free; +0 holds sectors
    // F-8 (bit 7 = F), +1 sectors 7-0, +2/+3 unused ("FFFF0000" = all free).
    for (int t = 0; t < kTracks; ++t) {
        const bool used = t == 0 || t == kVtocTrack;
        v[0x38 + 4 * t + 0] = used ? 0x00 : 0xFF;
        v[0x38 + 4 * t + 1] = used ? 0x00 : 0xFF;
    }

    // Catalog: 17/15 → 17/14 → … → 17/1, whose zero link ends the chain
    // (ch. 4: "The last catalog sector (sector 1) has a zero pointer"). Seven
    // entries each, all zero: "never used" (ch. 4, entry byte $00).
    for (int s = 15; s >= 1; --s) {
        uint8_t* c = img.data() + offsetOf(kVtocTrack, s);
        c[0x01] = s > 1 ? uint8_t(kVtocTrack) : uint8_t(0);
        c[0x02] = s > 1 ? uint8_t(s - 1) : uint8_t(0);
    }
    return img;
}

// ── Direct boot ────────────────────────────────────────────────────────────

std::array<uint8_t, 16> directBootReadOrder()
{
    // Every kDirectBootReadStep-th sector, skipping those already taken —
    // a soft interleave. [D] Casso DirectBootBuilder.cpp @ 2d899adf
    // (BuildReadOrder) measured 2 as the fastest step against its own drive
    // model (ascending order costs a revolution a sector: after a sector the
    // PROM decodes it, then the loader runs, and the next sector has gone by).
    // [F] Not measured in POM2 or on a real drive; the order changes the
    // speed, never the result. Settled by: timing a 183-sector boot in POM2
    // with steps 1-4, and on hardware.
    std::array<uint8_t, 16> order{};
    std::array<bool, 16> taken{};
    int at = 0;
    for (int i = 0; i < kSectorsPerTrack; ++i) {
        while (taken[std::size_t(at)]) at = (at + 1) % kSectorsPerTrack;
        order[std::size_t(i)] = uint8_t(at);
        taken[std::size_t(at)] = true;
        at = (at + kDirectBootReadStep) % kSectorsPerTrack;
    }
    return order;
}

std::size_t directBootCapacity(uint16_t loadAddress)
{
    if (loadAddress < kDirectBootLowest || loadAddress >= kDirectBootCeiling) return 0;
    return std::size_t(kDirectBootCeiling - loadAddress);
}

std::size_t directBootSectors(uint16_t loadAddress, std::size_t payloadBytes)
{
    if (payloadBytes == 0) return 0;
    const std::size_t leadIn = loadAddress % kSectorBytes;
    return (leadIn + payloadBytes + kSectorBytes - 1) / kSectorBytes;
}

bool directBoot(const std::vector<uint8_t>& payload, const DirectBootSpec& spec,
                std::vector<uint8_t>& sectors, std::string& refusal)
{
    // One reason, in this order: an address outside the window has a
    // capacity of 0, so asking about the size first would blame the length.
    if (payload.empty()) {
        refusal = "the payload is empty: there is nothing to boot";
        return false;
    }
    const std::size_t capacity = directBootCapacity(spec.loadAddress);
    if (capacity == 0) {
        refusal = "a direct-boot payload must load between " + hex4(kDirectBootLowest) + " and "
                  + hex4(kDirectBootCeiling - 1) + " (page $08 holds the loader, $C000 up is not RAM); "
                  + hex4(spec.loadAddress) + " was requested";
        return false;
    }
    if (payload.size() > capacity) {
        refusal = "the payload is " + std::to_string(payload.size())
                  + " bytes; a direct-boot disk loading at " + hex4(spec.loadAddress) + " can carry "
                  + std::to_string(capacity) + " bytes (" + std::to_string(capacity / kSectorBytes)
                  + " sectors)";
        return false;
    }
    if (spec.entryAddress < spec.loadAddress
        || std::size_t(spec.entryAddress - spec.loadAddress) >= payload.size()) {
        refusal = "the entry address " + hex4(spec.entryAddress)
                  + " is outside the payload, which occupies " + hex4(spec.loadAddress) + "-"
                  + hex4(uint32_t(spec.loadAddress) + uint32_t(payload.size()) - 1);
        return false;
    }

    const std::size_t count = directBootSectors(spec.loadAddress, payload.size());
    const std::array<uint8_t, 16> order = directBootReadOrder();
    std::vector<uint8_t> img(kImageBytes, 0);

    // The loader: physical sector 0 of track 0, which is DOS index 0.
    uint8_t* l = img.data() + offsetOf(0, dosFileIndexForPhysicalSector(0));
    std::copy(std::begin(kLoaderCode), std::end(kLoaderCode), l);
    std::copy(order.begin(), order.end(), l + kLoaderReadOrder);
    l[kLoaderSectorsLeft] = uint8_t(count);
    l[kLoaderLoadPage]    = uint8_t(spec.loadAddress >> 8);
    l[kLoaderTrack]       = 0;   // the head starts on track 0 …
    l[kLoaderPhase]       = 0;   // … at phase 0's position
    l[kLoaderIndex]       = 0;   // 0 = step to the next track first
    l[kLoaderEntryLo]     = uint8_t(spec.entryAddress & 0xFF);
    l[kLoaderEntryHi]     = uint8_t(spec.entryAddress >> 8);

    // The payload behind its lead-in, page k at track 1 + k/16, PHYSICAL
    // sector order[k % 16] — which in this buffer is that sector's DOS index.
    std::vector<uint8_t> onDisk(spec.loadAddress % kSectorBytes, 0);
    onDisk.insert(onDisk.end(), payload.begin(), payload.end());
    for (std::size_t k = 0; k < count; ++k) {
        const int track    = kDirectBootFirstTrack + int(k / std::size_t(kSectorsPerTrack));
        const int physical = order[k % std::size_t(kSectorsPerTrack)];
        const std::size_t from = k * kSectorBytes;
        const std::size_t span = std::min(kSectorBytes, onDisk.size() - from);
        std::copy_n(onDisk.begin() + std::ptrdiff_t(from), span,
                    img.begin() + std::ptrdiff_t(offsetOf(track, dosFileIndexForPhysicalSector(physical))));
    }

    sectors = std::move(img);
    refusal.clear();
    return true;
}

} // namespace pom2::diskbuild
