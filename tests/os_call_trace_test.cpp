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

// os_call_trace — the ProDOS MLI / CP/M BDOS call trace (OsCallTrace.h).
//
//  1. MLI: a 6502 program makes two MLI calls through a stub at $BF00 that
//     answers like ProDOS (pops the return address, steps over the inline
//     command and parameter pointer, A = error, C on error). The trace
//     decodes each call and pairs it with its result.
//  2. QUIT never returns and is reported at entry; a call whose stack the
//     program unwinds without returning is reported "(no return)".
//  3. BDOS: a Z80 program makes three CALL 5s into a stub BDOS.
//  4. The trace is free when off: the Debugger is not armed, and SlotBus
//     hangs the observer on a card Z80 plugged after it was set, and takes
//     it off again.

#include "ApplicardCard.h"
#include "Debugger.h"
#include "M6502.h"
#include "Memory.h"
#include "OsCallTrace.h"
#include "SlotBus.h"
#include "Z80.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

void poke(Memory& m, uint16_t at, std::initializer_list<int> bytes)
{
    for (int b : bytes) m.memWrite(at++, uint8_t(b));
}

void pokeStr(Memory& m, uint16_t at, const char* s)
{
    for (; *s; ++s) m.memWrite(at++, uint8_t(*s));
}

std::vector<std::string> lines(const pom2::OsCallTrace& t)
{
    return { t.lines().begin(), t.lines().end() };
}

void dump(const pom2::OsCallTrace& t)
{
    for (const auto& l : t.lines()) std::fprintf(stderr, "  | %s\n", l.c_str());
}

// A ProDOS-shaped MLI at $BF00: OPEN succeeds with ref 1, QUIT is never
// expected back, everything else fails with $46.
void installMliStub(Memory& m)
{
    poke(m, 0xBF00, {
        0x68, 0x85, 0xFA, 0x68, 0x85, 0xFB,         // PLA STA $FA PLA STA $FB
        0xA0, 0x02, 0xB1, 0xFA, 0x85, 0xFC,         // LDY #2 LDA ($FA),Y STA $FC
        0xC8, 0xB1, 0xFA, 0x85, 0xFD,               // INY LDA ($FA),Y STA $FD
        0x18, 0xA5, 0xFA, 0x69, 0x03, 0xAA,         // CLC LDA $FA ADC #3 TAX
        0xA5, 0xFB, 0x69, 0x00, 0x48, 0x8A, 0x48,   // LDA $FB ADC #0 PHA TXA PHA
        0xA0, 0x01, 0xB1, 0xFA, 0xC9, 0xC8,         // LDY #1 LDA ($FA),Y CMP #$C8
        0xD0, 0x0A,                                 // BNE $BF30
        0xA0, 0x05, 0xA9, 0x01, 0x91, 0xFC,         // LDY #5 LDA #1 STA ($FC),Y
        0xA9, 0x00, 0x18, 0x60,                     // LDA #0 CLC RTS
        0xA9, 0x46, 0x38, 0x60,                     // $BF30: LDA #$46 SEC RTS
    });
}

void testMli()
{
    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    pom2::Debugger dbg;
    dbg.setMemory(&mem);
    dbg.osCalls().bind(&cpu, &mem);
    CHECK(!dbg.armed());
    dbg.osCalls().setMli(true);
    CHECK(dbg.armed());
    cpu.setDebugHook(&dbg);

    installMliStub(mem);
    poke(mem, 0x0800, {
        0x20, 0x00, 0xBF, 0xC8, 0x00, 0x09,         // JSR MLI / OPEN / $0900
        0x20, 0x00, 0xBF, 0xC4, 0x20, 0x09,         // JSR MLI / GET_FILE_INFO / $0920
        0x4C, 0x0C, 0x08,                           // JMP *
    });
    poke(mem, 0x0900, { 0x03, 0x10, 0x09, 0x00, 0x1C, 0x00 });
    mem.memWrite(0x0910, 13);
    pokeStr(mem, 0x0911, "/PRODOS/HELLO");
    poke(mem, 0x0920, { 0x0A, 0x10, 0x09 });
    cpu.setStackPointer(0xFF);
    cpu.setProgramCounter(0x0800);
    for (int i = 0; i < 200; ++i) cpu.run(1);

    const auto l = lines(dbg.osCalls());
    CHECK(l.size() == 2);
    CHECK(l.size() > 0 && l[0] == "MLI  $0800 OPEN \"/PRODOS/HELLO\" buf=$1C00 -> ref=1");
    CHECK(l.size() > 1 && l[1] == "MLI  $0806 GET_FILE_INFO \"/PRODOS/HELLO\" -> ERR $46 file not found");
    CHECK(mem.peekCpuView(0x0905) == 1);       // the stub really answered
    if (l.size() != 2) dump(dbg.osCalls());

    // QUIT at entry; then a call the program abandons — its stub never
    // returns, the program resets the stack and calls again.
    dbg.osCalls().clear();
    poke(mem, 0x0A00, {
        0x20, 0x00, 0xBF, 0x65, 0x30, 0x09,         // JSR MLI / QUIT / $0930
    });
    poke(mem, 0x0930, { 0x04 });
    mem.memWrite(0xBF00, 0x4C);                 // a QUIT that never comes back:
    mem.memWrite(0xBF01, 0x00);                 // JMP $0B00, which restarts
    mem.memWrite(0xBF02, 0x0B);                 // the stack and calls once more
    poke(mem, 0x0B00, { 0xA2, 0xFF, 0x9A, 0x4C, 0x03, 0x0B });   // LDX #$FF TXS JMP *
    cpu.setStackPointer(0xFF);
    cpu.setProgramCounter(0x0A00);
    for (int i = 0; i < 50; ++i) cpu.run(1);
    const auto q = lines(dbg.osCalls());
    CHECK(q.size() == 1 && q[0] == "MLI  $0A00 QUIT params=$0930");
    if (q.size() != 1) dump(dbg.osCalls());

    dbg.osCalls().setMli(false);
    CHECK(!dbg.armed());
}

void testMliNoReturn()
{
    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    pom2::Debugger dbg;
    dbg.setMemory(&mem);
    dbg.osCalls().bind(&cpu, &mem);
    dbg.osCalls().setMli(true);
    cpu.setDebugHook(&dbg);
    // An MLI that drops the call: it resets the stack and jumps back to the
    // caller's next call, as an error handler unwinding to its loop would.
    // (Not to the return address: reaching it at the call's stack depth IS
    // the return, whatever got the CPU there.)
    poke(mem, 0xBF00, { 0xA2, 0xFF, 0x9A, 0x4C, 0x10, 0x0C });   // LDX #$FF TXS JMP $0C10
    poke(mem, 0x0C00, { 0x20, 0x00, 0xBF, 0xCC, 0x40, 0x09 });   // JSR MLI / CLOSE / $0940
    poke(mem, 0x0C10, { 0x20, 0x00, 0xBF, 0xCD, 0x40, 0x09 });   // JSR MLI / FLUSH / $0940
    poke(mem, 0x0940, { 0x01, 0x00 });
    cpu.setStackPointer(0xFF);
    cpu.setProgramCounter(0x0C00);
    for (int i = 0; i < 12; ++i) cpu.run(1);
    const auto l = lines(dbg.osCalls());
    CHECK(l.size() >= 1 && l[0] == "MLI  $0C00 CLOSE ref=0 -> (no return)");
    if (l.empty() || l[0] != "MLI  $0C00 CLOSE ref=0 -> (no return)") dump(dbg.osCalls());
}

struct FlatBus : pom2::Z80Bus {
    uint8_t ram[0x10000] = {};
    uint8_t z80MemRead(uint16_t a) override { return ram[a]; }
    void    z80MemWrite(uint16_t a, uint8_t v) override { ram[a] = v; }
    void put(uint16_t at, std::initializer_list<int> b) { for (int v : b) ram[at++] = uint8_t(v); }
};

void testBdos()
{
    auto bus = std::make_unique<FlatBus>();
    pom2::Z80 z(*bus);
    pom2::OsCallTrace t;
    t.setBdos(true);
    z.setObserver(&t);
    bus->put(0x0005, { 0xC3, 0x00, 0xF0 });     // JP BDOS
    bus->put(0xF000, {
        0x79, 0xFE, 0x0F, 0x20, 0x03,           // LD A,C  CP 15  JR NZ,+3
        0x3E, 0xFF, 0xC9,                       // LD A,$FF  RET
        0x21, 0x22, 0x00, 0x7D, 0xC9,           // LD HL,$0022  LD A,L  RET
    });
    bus->put(0x0100, {
        0x0E, 0x09, 0x11, 0x00, 0x02, 0xCD, 0x05, 0x00,   // C_WRITESTR $0200
        0x0E, 0x0F, 0x11, 0x5C, 0x00, 0xCD, 0x05, 0x00,   // F_OPEN $005C
        0x0E, 0x0C, 0xCD, 0x05, 0x00,                     // S_BDOSVER
        0x18, 0xFE,                                       // JR *
    });
    std::memcpy(bus->ram + 0x0200, "Hi$", 3);
    bus->ram[0x5C] = 1;
    std::memcpy(bus->ram + 0x5D, "HELLO   COM", 11);
    z.reset();
    z.setPC(0x0100);
    z.setSP(0xFF00);
    for (int i = 0; i < 60; ++i) z.step();
    const auto l = lines(t);
    CHECK(l.size() == 3);
    CHECK(l.size() > 0 && l[0] == "BDOS $0105 C_WRITESTR \"Hi\"");
    CHECK(l.size() > 1 && l[1] == "BDOS $010D F_OPEN A:HELLO.COM -> A=$FF");
    CHECK(l.size() > 2 && l[2] == "BDOS $0112 S_BDOSVER -> HL=$0022");
    if (l.size() != 3) dump(t);
}

void testSlotBusObserver()
{
    pom2::OsCallTrace t;
    SlotBus bus;
    bus.plug(1, std::make_unique<ApplicardCard>());
    CHECK(bus.peripheral(1)->z80Core()->observer() == nullptr);
    bus.setZ80Observer(&t);
    CHECK(bus.peripheral(1)->z80Core()->observer() == &t);
    bus.plug(2, std::make_unique<ApplicardCard>());     // plugged after: still watched
    CHECK(bus.peripheral(2)->z80Core()->observer() == &t);
    bus.setZ80Observer(nullptr);
    CHECK(bus.peripheral(1)->z80Core()->observer() == nullptr);
    CHECK(bus.peripheral(2)->z80Core()->observer() == nullptr);
}

} // namespace

int main()
{
    testMli();
    testMliNoReturn();
    testBdos();
    testSlotBusObserver();
    if (g_fail) {
        std::fprintf(stderr, "os_call_trace: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("os_call_trace: PASS\n");
    return 0;
}
