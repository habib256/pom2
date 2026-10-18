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


OpcodeInfo describeOpcode6502(uint8_t opcode, bool cmos);
}
