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

// AppleIoNames — what an I/O address DOES, in words, for the debugger's
// listing: `LDA $C055` reads "PAGE2 on (page 2)" with no symbol file loaded.
// A symbol file (Debugger panel ▸ Symbols) still wins: it is the program's
// own vocabulary, this is the machine's.
//
// These are DESCRIPTIONS, not Apple's official equate names (TXTPAGE2,
// HISCR, ...): a 6502 developer names these switches their own way (French
// Touch's sysconst.a says _PAGE2on), and the action is what everyone agrees
// on. [B] Apple IIe Technical Reference Manual, soft-switch tables; the
// language card's sixteen switches, Apple II Reference Manual; the 6522's
// sixteen registers, its data sheet; the Disk II's sixteen switches, DOS
// 3.3's RWTS. Each line matches what Memory.cpp / the card decodes — the
// table is a legend for POM2's own behaviour, and `io_names` in
// `beam_view` pins a sample of it.

#ifndef POM2_APPLE_IO_NAMES_H
#define POM2_APPLE_IO_NAMES_H

#include <cstdint>

namespace pom2 {

/// What a motherboard I/O address ($C000-$C08F) does; nullptr when it has
/// no entry. `iicClass` picks the //c meaning where it differs.
inline const char* motherboardIoName(uint16_t address, bool iicClass)
{
    switch (address) {
        case 0xC000: return "KBD read / 80STORE off";
        case 0xC001: return "80STORE on";
        case 0xC002: return "RAMRD main";
        case 0xC003: return "RAMRD aux";
        case 0xC004: return "RAMWRT main";
        case 0xC005: return "RAMWRT aux";
        case 0xC006: return "slot ROM at $C100-$CFFF";
        case 0xC007: return "internal ROM at $C100-$CFFF";
        case 0xC008: return "ALTZP off (main ZP, stack, LC)";
        case 0xC009: return "ALTZP on (aux ZP, stack, LC)";
        case 0xC00A: return "internal ROM at $C300";
        case 0xC00B: return "slot ROM at $C300";
        case 0xC00C: return "80COL off";
        case 0xC00D: return "80COL on";
        case 0xC00E: return "ALTCHAR off";
        case 0xC00F: return "ALTCHAR on";
        case 0xC010: return "KBDSTRB (clear the key strobe)";
        case 0xC011: return "read: LC bank 2?";
        case 0xC012: return "read: LC RAM?";
        case 0xC013: return "read: RAMRD?";
        case 0xC014: return "read: RAMWRT?";
        case 0xC015: return "read: internal Cx ROM?";
        case 0xC016: return "read: ALTZP?";
        case 0xC017: return "read: slot C3 ROM?";
        case 0xC018: return "read: 80STORE?";
        case 0xC019: return "read: VBL";
        case 0xC01A: return "read: TEXT?";
        case 0xC01B: return "read: MIXED?";
        case 0xC01C: return "read: PAGE2?";
        case 0xC01D: return "read: HIRES?";
        case 0xC01E: return "read: ALTCHAR?";
        case 0xC01F: return "read: 80COL?";
        case 0xC030: return "speaker toggle";
        case 0xC050: return "TEXT off (graphics)";
        case 0xC051: return "TEXT on";
        case 0xC052: return "MIXED off";
        case 0xC053: return "MIXED on";
        case 0xC054: return "PAGE2 off (page 1)";
        case 0xC055: return "PAGE2 on (page 2)";
        case 0xC056: return "HIRES off (lo-res)";
        case 0xC057: return "HIRES on";
        case 0xC058: return "AN0 off";
        case 0xC059: return "AN0 on";
        case 0xC05A: return iicClass ? "VBL interrupt off (//c)" : "AN1 off";
        case 0xC05B: return iicClass ? "VBL interrupt on (//c)" : "AN1 on";
        case 0xC05C: return "AN2 off";
        case 0xC05D: return "AN2 on";
        case 0xC05E: return "AN3 off (double hi-res on)";
        case 0xC05F: return "AN3 on (double hi-res off)";
        case 0xC061: return "button 0 / Open-Apple";
        case 0xC062: return "button 1 / Solid-Apple";
        case 0xC063: return "button 2";
        case 0xC064: return "paddle 0";
        case 0xC065: return "paddle 1";
        case 0xC066: return "paddle 2";
        case 0xC067: return "paddle 3";
        case 0xC070: return "paddle trigger";
        case 0xC07E: return iicClass ? "IOUDIS on (//c)" : nullptr;
        case 0xC07F: return iicClass ? "IOUDIS off (//c)" : nullptr;
        // The language card: an odd address read twice enables writing.
        case 0xC080: return "LC: read RAM bank 2, write-protect";
        case 0xC081: return "LC: read ROM, write RAM bank 2 (2 reads)";
        case 0xC082: return "LC: read ROM, write-protect";
        case 0xC083: return "LC: read/write RAM bank 2 (2 reads)";
        case 0xC088: return "LC: read RAM bank 1, write-protect";
        case 0xC089: return "LC: read ROM, write RAM bank 1 (2 reads)";
        case 0xC08A: return "LC: read ROM, write-protect";
        case 0xC08B: return "LC: read/write RAM bank 1 (2 reads)";
        default:     return nullptr;
    }
}

/// A well-known entry point or vector outside the I/O page: the monitor ROM's
/// routines every Apple II program calls, ProDOS's MLI, the page-3 vectors.
/// [B] Apple II Reference Manual, monitor entry points; ProDOS 8 Technical
/// Reference (MLI at $BF00). Names as Apple's listings spell them;
/// [D] cross-checked against AppleWin @ 7fd7fd7c `bin/APPLE2E.SYM`, which
/// agrees on every address it lists (it calls $FD0C RDKEY2 and $FF59 OLDRST).
inline const char* monitorEntryName(uint16_t address)
{
    switch (address) {
        case 0x03F0: return "BRK vector";
        case 0x03F2: return "SOFTEV (reset vector)";
        case 0x03FB: return "NMI vector (JMP)";
        case 0x03FE: return "IRQ vector";
        case 0xBF00: return "ProDOS MLI";
        case 0xF800: return "PLOT";
        case 0xF819: return "HLINE";
        case 0xF828: return "VLINE";
        case 0xF832: return "CLRSCR";
        case 0xF836: return "CLRTOP";
        case 0xF864: return "SETCOL";
        case 0xF871: return "SCRN";
        case 0xFB1E: return "PREAD (read a paddle)";
        case 0xFB2F: return "INIT";
        case 0xFB39: return "SETTXT";
        case 0xFB40: return "SETGR";
        case 0xFC58: return "HOME";
        case 0xFCA8: return "WAIT";
        case 0xFD0C: return "RDKEY";
        case 0xFD6A: return "GETLN";
        case 0xFD8E: return "CROUT";
        case 0xFDDA: return "PRBYTE";
        case 0xFDED: return "COUT";
        case 0xFDF0: return "COUT1";
        case 0xFE89: return "SETKBD";
        case 0xFE93: return "SETVID";
        case 0xFF3A: return "BELL";
        case 0xFF59: return "MON";
        case 0xFF69: return "MONZ";
        case 0xFFFA: return "NMI vector";
        case 0xFFFC: return "RESET vector";
        case 0xFFFE: return "IRQ/BRK vector";
        default:     return nullptr;
    }
}

/// A 6522 VIA register (0-15), as on a Mockingboard's two VIAs.
inline const char* via6522RegisterName(int reg)
{
    static const char* const names[16] = {
        "ORB", "ORA", "DDRB", "DDRA", "T1C-L", "T1C-H", "T1L-L", "T1L-H",
        "T2C-L", "T2C-H", "SR", "ACR", "PCR", "IFR", "IER", "ORA (no handshake)",
    };
    return names[reg & 15];
}

/// A Disk II soft switch (0-15, the low nibble of $C0nX).
inline const char* diskIISwitchName(int sw)
{
    static const char* const names[16] = {
        "phase 0 off", "phase 0 on", "phase 1 off", "phase 1 on",
        "phase 2 off", "phase 2 on", "phase 3 off", "phase 3 on",
        "motor off", "motor on", "drive 1", "drive 2",
        "Q6 low (read)", "Q6 high (load / sense)", "Q7 low (read mode)", "Q7 high (write mode)",
    };
    return names[sw & 15];
}

} // namespace pom2

#endif // POM2_APPLE_IO_NAMES_H
