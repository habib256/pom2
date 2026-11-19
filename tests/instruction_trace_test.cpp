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

// UI-017 — the instruction trace and the callgrind export.
//
// The acceptance: a trace of a known 6502 loop matches its expected lines
// byte for byte. The cycle counts in the expected lines are the 6502's own
// ([B] MOS MCS6500 programming manual, appendix: LDX # 2, DEX 2, BNE 2 / 3
// taken, JMP abs 3); the registers are the ones going IN to each line.

#include "Debugger.h"
#include "DebuggerSymbols.h"
#include "InstructionTrace.h"
#include "M6502.h"
#include "Memory.h"
#include "ProfileExport.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using pom2::InstructionTrace;

namespace {

std::vector<std::string> readLines(const fs::path& p)
{
    std::ifstream f(p);
    std::vector<std::string> out;
    for (std::string l; std::getline(f, l);) out.push_back(l);
    return out;
}

void testFormatTokens()
{
    InstructionTrace::Fields f;
    f.pc = 0xC600; f.bank = pom2::DebugBank{ pom2::DebugBank::Slot, 6, 0 };
    f.bytes[0] = 0xA2; f.bytes[1] = 0x20; f.length = 2;
    f.disassembly = "LDX #$20"; f.label = "BOOT";
    f.a = 0x01; f.x = 0x02; f.y = 0x03; f.p = 0xA5; f.sp = 0xFD;
    f.cycles = 2; f.total = 123456;
    assert(InstructionTrace::formatLine("%PC %BANK|%BYTES|%DIS|", f)
           == "C600 slot6|A2 20   |LDX #$20        |");
    assert(InstructionTrace::formatLine("A=%A X=%X Y=%Y P=%P SP=%SP %FLAGS", f)
           == "A=01 X=02 Y=03 P=A5 SP=FD Nv-bdIzC");
    assert(InstructionTrace::formatLine("%CYC @%TOTAL %LABEL 100%% %Q", f) == "2 @123456 BOOT 100% %Q");
    std::printf("  ok: every token, %% and an unknown token kept as written\n");
}

void testKnownLoop()
{
    Memory mem;
    M6502 cpu(&mem);
    cpu.setCpuMode(M6502::CpuMode::NMOS);
    pom2::Debugger dbg;
    dbg.setMemory(&mem);
    // $0800 LDX #$02 / $0802 DEX / $0803 BNE $0802 / $0805 JMP $0805
    const uint8_t code[] = { 0xA2, 0x02, 0xCA, 0xD0, 0xFD, 0x4C, 0x05, 0x08 };
    for (std::size_t i = 0; i < sizeof code; ++i) mem.memWrite(static_cast<uint16_t>(0x0800 + i), code[i]);
    cpu.setProgramCounter(0x0800);
    const uint8_t p0 = cpu.getStatusRegister(), sp0 = cpu.getStackPointer(), a0 = cpu.getAccumulator();

    const fs::path path = fs::temp_directory_path() / "pom2_trace_test.txt";
    InstructionTrace trace;
    std::string err;
    assert(trace.open(path, "%PC %BANK %BYTES %DIS A=%A X=%X P=%P SP=%SP %CYC", cpu, mem, err));
    dbg.setInstructionTrace(&trace);
    assert(dbg.armed());
    cpu.setDebugHook(&dbg);
    cpu.run(14);          // LDX, DEX, BNE (taken), DEX, BNE, JMP: 2+2+3+2+2+3 = 14
    const uint64_t written = trace.lines();
    trace.close();
    dbg.setInstructionTrace(nullptr);

    // P going in: LDX #2 clears Z and N; DEX to 1 keeps them clear; DEX to 0
    // sets Z. (P's other bits are whatever reset left: p0's.)
    const uint8_t pNZclear = static_cast<uint8_t>(p0 & ~0x82);
    const uint8_t pZ = static_cast<uint8_t>(pNZclear | 0x02);
    char want[6][96];
    std::snprintf(want[0], 96, "0800 main A2 02    LDX #$02         A=%02X X=00 P=%02X SP=%02X 2", a0, p0, sp0);
    std::snprintf(want[1], 96, "0802 main CA       DEX              A=%02X X=02 P=%02X SP=%02X 2", a0, pNZclear, sp0);
    std::snprintf(want[2], 96, "0803 main D0 FD    BNE $0802        A=%02X X=01 P=%02X SP=%02X 3", a0, pNZclear, sp0);
    std::snprintf(want[3], 96, "0802 main CA       DEX              A=%02X X=01 P=%02X SP=%02X 2", a0, pNZclear, sp0);
    std::snprintf(want[4], 96, "0803 main D0 FD    BNE $0802        A=%02X X=00 P=%02X SP=%02X 2", a0, pZ, sp0);
    std::snprintf(want[5], 96, "0805 main 4C 05 08 JMP $0805        A=%02X X=00 P=%02X SP=%02X 3", a0, pZ, sp0);
    const auto got = readLines(path);
    assert(got.size() == 6 && written == 6);
    for (int i = 0; i < 6; ++i) {
        if (got[i] != want[i]) std::fprintf(stderr, "line %d\n got: [%s]\nwant: [%s]\n", i, got[i].c_str(), want[i]);
        assert(got[i] == want[i]);
    }
    fs::remove(path);
    std::printf("  ok: a known loop traces byte for byte (registers in, cycles out)\n");
}

void testCallgrind()
{
    using E = pom2::Debugger::ProfileEntry;
    const pom2::DebugBank main{ pom2::DebugBank::Main, 0, 0 };
    const pom2::DebugBank aux{ pom2::DebugBank::Aux, 0, 0 };
    const std::vector<E> entries = {
        { 0x0803, 10, 30, main }, { 0x0800, 1, 2, main }, { 0x0900, 5, 20, main },
        { 0x0800, 4, 8, aux }, { 0x2000, 7, 14, main }, { 0x3000, 0, 0, main },
    };
    pom2::DebuggerSymbols syms;
    std::string err;
    assert(syms.load("start = $0800\nsub = $0900\nauxfn = aux0:$0800\n", err));
    const std::string text = pom2::callgrindProfile(entries, &syms, "test");

    // An independent reader: header keys, then cost lines under ob/fl/fn.
    std::istringstream in(text);
    std::string line, ob, fl, fn;
    std::map<std::string, std::string> header;
    std::map<std::string, std::pair<uint64_t, uint64_t>> perFn;
    uint64_t c = 0, v = 0;
    bool body = false;
    std::getline(in, line);
    assert(line == "# callgrind format");
    while (std::getline(in, line)) {
        if (line.empty()) { body = true; continue; }
        if (!body) { const auto colon = line.find(": "); header[line.substr(0, colon)] = line.substr(colon + 2); continue; }
        if (line.rfind("ob=", 0) == 0) { ob = line.substr(3); continue; }
        if (line.rfind("fl=", 0) == 0) { fl = line.substr(3); continue; }
        if (line.rfind("fn=", 0) == 0) { fn = line.substr(3); continue; }
        if (line.rfind("totals: ", 0) == 0) { header["totals"] = line.substr(8); continue; }
        assert(line.rfind("0x", 0) == 0 && !fl.empty() && !fn.empty());
        unsigned addr; unsigned long long cy, vi;
        assert(std::sscanf(line.c_str(), "0x%x %llu %llu", &addr, &cy, &vi) == 3);
        perFn[ob + ":" + fn].first += cy; perFn[ob + ":" + fn].second += vi;
        c += cy; v += vi;
    }
    assert(header["version"] == "1" && header["positions"] == "instr");
    assert(header["events"] == "Cycles Visits" && header["cmd"] == "test");
    assert(header["summary"] == "74 27" && header["totals"] == "74 27" && c == 74 && v == 27);
    assert((perFn["main:start"] == std::pair<uint64_t, uint64_t>{ 32, 11 }));   // $0800 + $0803
    assert((perFn["main:sub"] == std::pair<uint64_t, uint64_t>{ 34, 12 }));     // $0900, and $2000 under it
    assert((perFn["aux0:auxfn"] == std::pair<uint64_t, uint64_t>{ 8, 4 }));     // the bank keeps them apart
    assert(perFn.size() == 3);

    // No symbols: the 256-byte page names the routine.
    const std::string bare = pom2::callgrindProfile({ { 0xC65C, 3, 9, main } }, nullptr, "");
    assert(bare.find("fn=page $C6\n0xC65C 9 3\n") != std::string::npos);
    std::printf("  ok: callgrind header, totals, routines by nearest label and bank\n");
}

} // namespace

int main()
{
    std::printf("instruction_trace:\n");
    testFormatTokens();
    testKnownLoop();
    testCallgrind();
    std::printf("instruction_trace: all passed\n");
    return 0;
}
