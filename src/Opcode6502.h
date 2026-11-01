// POM2 — GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace pom2 {
// 6502 addressing modes for disassembly formatting
enum AddrMode {
    AM_IMP, AM_IMM, AM_ZP, AM_ZPX, AM_ZPY,
    AM_ABS, AM_ABX, AM_ABY, AM_IND,
    AM_IZX, AM_IZY, AM_REL,
    // 65C02 additions:
    AM_ACC,   // accumulator, rendered "INC A"
    AM_IZP,   // (zp)        — 2 bytes
    AM_IAX,   // (abs,X)     — 3 bytes  (JMP)
    AM_ZPR,   // zp,rel      — 3 bytes  (BBR/BBS): "$zz,$tttt"
    AM_BRK    // BRK         — 2 bytes: the $00 opcode + its signature byte
};

struct OpcodeInfo {
    const char* mnemonic;
    AddrMode mode;
};


/// Which 65xx instruction set to decode — the one the CPU core dispatches
/// (`M6502::instructionSet()`), never an assumption about it.
///
///  - Nmos6502: the NMOS 6502 (][, ][+, unenhanced //e).
///  - Ncr65C02: the 65C02 Apple shipped in the enhanced //e and the //c.
///    [B] Apple IIc Technical Reference Manual (1984), Appendix A, the NCR65C02
///        data sheet (c) 1982 NCR, "Execution of invalid op codes": "All are
///        NOPs (reserved for future use) ... X3, X7, XB, XF  1 [byte]
///        1 [cycle]". So $x7 (RMB/SMB), $xF (BBR/BBS), $CB (WAI) and $DB
///        (STP) are one-byte NOPs on it.
///    [C] 6502.org, B. Clark, "65C02 Opcodes" § 6 notes a-f: those columns
///        are "1-cycle NOP on some older 65C02s", RMB/SMB/BBR/BBS "on
///        Rockwell and on modern WDC 65C02s", WAI/STP on WDC.
///  - Superset65C02: Rockwell bit ops + WDC WAI/STP — what POM2's card
///    coprocessors and a TransWarp's own 65C02 run (`M6502::kCmosTable`).
enum class Cpu6502Set : uint8_t { Nmos6502, Ncr65C02, Superset65C02 };

OpcodeInfo describeOpcode6502(uint8_t opcode, Cpu6502Set set);
/// `cmos` true = Superset65C02, false = Nmos6502 (the historical switch).
OpcodeInfo describeOpcode6502(uint8_t opcode, bool cmos);
}
