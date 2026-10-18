// POM2 — GPL-3.0-or-later
#include "Assembler6502.h"
#include "Opcode6502.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace pom2 {
namespace {
bool number(std::string_view text, unsigned& value, bool& wide)
{
    unsigned base = 10;
    wide = false;
    if (text.empty()) return false;
    if (text.front() == '$') { base = 16; text.remove_prefix(1); wide = text.size() > 2; }
    else if (text.front() == '%') { base = 2; text.remove_prefix(1); wide = text.size() > 8; }
    if (text.empty()) return false;
    value = 0;
    for (unsigned char c : text) {
        const unsigned digit = c >= '0' && c <= '9' ? c - '0' :
                               c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
        if (digit >= base || value > (65535 - digit) / base) return false;
        value = value * base + digit;
    }
    wide = wide || value > 255;
    return true;
}
bool opcode(const std::string& mnemonic, AddrMode mode, bool cmos, uint8_t& result)
{
    // Prefer the documented $EA NOP over CMOS reserved NOP encodings.
    if (mnemonic == "NOP" && mode == AM_IMP) { result = 0xEA; return true; }
    for (unsigned op = 0; op < 256; ++op) {
        const auto info = describeOpcode6502(uint8_t(op), cmos);
        if (info.mnemonic == mnemonic && info.mode == mode) { result = uint8_t(op); return true; }
    }
    return false;
}
}

bool assemble6502(const std::string& source, uint16_t pc, bool cmos,
                  std::vector<uint8_t>& bytes, std::string& error)
{
    error.clear();
    auto fail = [&](const char* why) { error = why; return false; };
    const auto comment = source.find(';');
    std::string text = source.substr(0, comment);
    for (char& c : text) c = char(std::toupper(static_cast<unsigned char>(c)));
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return fail("Enter an instruction.");
    text.erase(0, begin);
    const auto split = text.find_first_of(" \t\r\n");
    const std::string mnemonic = text.substr(0, split);
    if (mnemonic == "???") return fail("Unknown mnemonic.");
    std::string operand = split == std::string::npos ? "" : text.substr(split);
    operand.erase(std::remove_if(operand.begin(), operand.end(),
        [](unsigned char c) { return std::isspace(c); }), operand.end());
    AddrMode mode = AM_IMP, fallback = AM_IMP;
    unsigned value = 0, target = 0;
    bool wide = false;
    std::string_view n = operand;
    uint8_t op = 0;
    if (operand.empty()) {
        if (mnemonic == "BRK") mode = AM_BRK;
        else if (!opcode(mnemonic, mode, cmos, op)) mode = AM_ACC;
    } else if (operand == "A") {
        mode = AM_ACC;
        if (!opcode(mnemonic, mode, cmos, op)) mode = AM_IMP; // NMOS ASL A, etc.
        if (mnemonic != "ASL" && mnemonic != "LSR" && mnemonic != "ROL" &&
            mnemonic != "ROR" && mnemonic != "INC" && mnemonic != "DEC")
            return fail("This instruction has no accumulator operand.");
    } else if (operand.front() == '#') {
        mode = AM_IMM; n.remove_prefix(1);
        if (!number(n, value, wide) || value > 255) return fail("Immediate operand must fit in one byte.");
    } else if (opcode(mnemonic, AM_ZPR, cmos, op)) {
        const auto comma = n.find(',');
        if (comma == std::string_view::npos || !number(n.substr(0, comma), value, wide) || value > 255 ||
            !number(n.substr(comma + 1), target, wide)) return fail("Use a zero-page address and a branch target.");
        mode = AM_ZPR;
    } else {
        if (operand.front() == '(') {
            if (operand.size() >= 4 && operand.compare(operand.size() - 3, 3, ",X)") == 0) {
                n.remove_prefix(1); n.remove_suffix(3); mode = AM_IZX; fallback = AM_IAX;
            } else if (operand.size() >= 4 && operand.compare(operand.size() - 3, 3, "),Y") == 0) {
                n.remove_prefix(1); n.remove_suffix(3); mode = AM_IZY;
            } else if (operand.back() == ')') {
                n.remove_prefix(1); n.remove_suffix(1); mode = AM_IZP; fallback = AM_IND;
            } else return fail("Invalid indirect operand.");
        } else if (operand.size() > 2 && operand.compare(operand.size() - 2, 2, ",X") == 0) {
            n.remove_suffix(2); mode = AM_ZPX; fallback = AM_ABX;
        } else if (operand.size() > 2 && operand.compare(operand.size() - 2, 2, ",Y") == 0) {
            n.remove_suffix(2); mode = AM_ZPY; fallback = AM_ABY;
        } else { mode = AM_ZP; fallback = AM_ABS; }
        if (!number(n, value, wide)) return fail("Invalid numeric operand ($hex, %binary or decimal).");
        if (mode == AM_ZP && opcode(mnemonic, AM_REL, cmos, op)) { mode = AM_REL; target = value; }
        else if ((wide || !opcode(mnemonic, mode, cmos, op)) && fallback != AM_IMP) mode = fallback;
    }
    if (!opcode(mnemonic, mode, cmos, op)) return fail("Mnemonic/addressing mode unavailable on this CPU.");
    int length = 1;
    switch (mode) {
    case AM_IMP: case AM_ACC: break;
    case AM_ABS: case AM_ABX: case AM_ABY: case AM_IND: case AM_IAX: case AM_ZPR: length = 3; break;
    default: length = 2; break;
    }
    if (mode == AM_REL || mode == AM_ZPR) {
        const uint16_t delta = uint16_t(target - uint16_t(pc + length));
        if (delta > 127 && delta < 0xFF80) return fail("Branch target is outside -128..127 bytes.");
        if (mode == AM_REL) value = uint8_t(delta);
        else target = uint8_t(delta);
    } else if (length == 2 && value > 255) return fail("Operand must fit in one byte.");
    std::vector<uint8_t> result{op};
    if (length > 1) result.push_back(uint8_t(value));
    if (length > 2) result.push_back(uint8_t(mode == AM_ZPR ? target : value >> 8));
    bytes = std::move(result);
    return true;
}
}
