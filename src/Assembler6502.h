// POM2 — GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {
// One instruction, numeric operands ($hex, %binary or decimal). Failure leaves
// bytes unchanged. Four hex digits force absolute addressing, e.g. LDA $0010.
bool assemble6502(const std::string& source, uint16_t pc, bool cmos,
                  std::vector<uint8_t>& bytes, std::string& error);
}
