// POM2 — GPL-3.0-or-later
#pragma once
#include "Opcode6502.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {
// One instruction, numeric operands ($hex, %binary or decimal). Failure leaves
// bytes unchanged. Four hex digits force absolute addressing, e.g. LDA $0010.
// `cmos` true = Superset65C02 (Rockwell + WDC), false = NMOS.
bool assemble6502(const std::string& source, uint16_t pc, bool cmos,
                  std::vector<uint8_t>& bytes, std::string& error);
// The instruction set the core runs (`M6502::instructionSet()`): on an
// NCR65C02 BBR/BBS/RMB/SMB/WAI/STP are refused — the core would run the
// bytes as one-byte NOPs.
bool assemble6502(const std::string& source, uint16_t pc, Cpu6502Set set,
                  std::vector<uint8_t>& bytes, std::string& error);
}
