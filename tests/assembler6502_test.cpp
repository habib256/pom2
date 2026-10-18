// POM2 — GPL-3.0-or-later
#include "Assembler6502.h"
#include "Disassembler6502.h"
#include <array>
#include <cassert>
#include <iostream>

using pom2::assemble6502;
int main()
{
    std::vector<uint8_t> bytes;
    std::string error;
    auto check = [&](const char* text, uint16_t pc, bool cmos, std::vector<uint8_t> expected) {
        assert(assemble6502(text, pc, cmos, bytes, error));
        assert(error.empty() && bytes == expected);
    };
    check(" lda #$00 ; comment", 0x300, false, {0xA9, 0});
    check("LDA $12", 0, false, {0xA5, 0x12});
    check("LDA $0012", 0, false, {0xAD, 0x12, 0});
    check("STA ($12), Y", 0, false, {0x91, 0x12});
    check("LDA ($12,X)", 0, false, {0xA1, 0x12});
    check("JMP ($0012,X)", 0, true, {0x7C, 0x12, 0});
    check("LDA ($12)", 0, true, {0xB2, 0x12});
    check("BNE $0282", 0x300, false, {0xD0, 0x80});
    check("BNE $0381", 0x300, false, {0xD0, 0x7F});
    check("BRA $0000", 0xFFFE, true, {0x80, 0});
    check("BBS3 $12,$0300", 0x300, true, {0xBF, 0x12, 0xFD});
    check("ASL A", 0, false, {0x0A});
    check("INC A", 0, true, {0x1A});
    check("BRK", 0, false, {0, 0});
    check("NOP", 0, true, {0xEA});
    check("LDA #%10101010", 0, false, {0xA9, 0xAA});
    check("LDA #255", 0, false, {0xA9, 255});
    const auto previous = bytes;
    for (const char* invalid : {"", "???", "LDA #$100", "LDA #-1", "LDA $10000", "LDA $1G", "LDA (1", "LDA (256),Y", "RTS A", "BRK $12", "BNE $0281", "BNE $0382", "LDA #1 garbage"}) {
        assert(!assemble6502(invalid, 0x300, true, bytes, error));
        assert(!error.empty() && bytes == previous);
    }
    for (const char* cmosOnly : {"STZ $12", "BRA $0300", "PHX", "INC A", "BBR0 $12,$0300", "JMP ($1234,X)"})
        assert(!assemble6502(cmosOnly, 0x300, false, bytes, error));
    // Every instruction the disassembler names must assemble to equivalent
    // bytes/length. Operand leading zeros preserve absolute modes.
    std::array<uint8_t, 65536> memory{};
    for (bool cmos : {false, true}) for (unsigned op = 0; op < 256; ++op) {
        memory[0x300] = uint8_t(op); memory[0x301] = 0x12; memory[0x302] = 0x34;
        int length = 0;
        const auto text = pom2::disassemble6502(memory.data(), 0x300, length, cmos);
        if (text.compare(0, 3, "???") == 0) continue;
        assert(assemble6502(text, 0x300, cmos, bytes, error));
        assert(bytes.size() == size_t(length));
        if (text == "BRK") assert(bytes[0] == 0 && bytes[1] == 0);
        else if (text != "NOP") for (int i = 0; i < length; ++i) assert(bytes[i] == memory[0x300 + i] || (i == 0 && text.compare(0, 3, "NOP") == 0));
    }
    std::cout << "assembler6502: OK\n";
}
