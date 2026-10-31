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
//  5. SmartPort: a slot page with the SmartPort ID and a dispatch at
//     $CnFF + 3 that answers like a Liron (steps over the inline command and
//     list pointer, 3 bytes or 5 in the extended set); a JSR to the same
//     page that is NOT the dispatch is not a call. The page is read through
//     peekCpuView, so a card is traced only if it answers peekSlotRom.
//  6. Pascal: the BIOS table at $EC/$ED — entries with a JMP log, console
//     read/write do not, and a table of non-JMPs logs nothing.
//  7. Disk II: a seek of consecutive steps is one line, closed by the data
//     latch; the spindle starting and really stopping (the 556 delay).
//  8. Monitor: COUT lines, a GETLN with what it returns.
//  9. Applecorn: an OSBYTE through its page-2 vector in aux RAM.
// 10. CP/M-65: a BDOS call recognised by its prologue and paired with its
//     return, a BIOS call by its jmpdispatch.
//  4. The trace is free when off: the Debugger is not armed, and SlotBus
//     hangs the observer on a card Z80 plugged after it was set, and takes
//     it off again.

#include "ApplicardCard.h"
#include "DiskIICard.h"
#include "Debugger.h"
#include "M6502.h"
#include "Memory.h"
#include "OsCallTrace.h"
#include "SlotBus.h"
#include "Z80.h"

#include <array>
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

// A SmartPort page: ID bytes, $C5FF = $0A, dispatch at $C50D. STATUS and
// READBLOCK succeed (X/Y = 8 / 512 bytes), anything else answers $28.
class SmartPortStub : public SlotPeripheral {
public:
    SmartPortStub()
    {
        const uint8_t code[] = {
            0x68, 0x85, 0xFA, 0x68, 0x85, 0xFB,         // PLA STA $FA PLA STA $FB
            0xA0, 0x01, 0xB1, 0xFA, 0xA8,               // LDY #1 LDA ($FA),Y TAY
            0x29, 0x40, 0xF0, 0x02, 0xA9, 0x02,         // AND #$40 BEQ +2 LDA #2
            0x18, 0x69, 0x03, 0x65, 0xFA, 0xAA,         // CLC ADC #3 ADC $FA TAX
            0xA5, 0xFB, 0x69, 0x00, 0x48, 0x8A, 0x48,   // LDA $FB ADC #0 PHA TXA PHA
            0x98, 0x29, 0x3F,                           // TYA AND #$3F
            0xF0, 0x0A, 0xC9, 0x01, 0xD0, 0x0C,         // BEQ status CMP #1 BNE err
            0xA2, 0x00, 0xA0, 0x02, 0xD0, 0x02,         // LDX #0 LDY #2 BNE ok
            0xA2, 0x08,                                 // status: LDX #8 (Y = the command, 0)
            0xA9, 0x00, 0x18, 0x60,                     // ok: LDA #0 CLC RTS
            0xA9, 0x28, 0x38, 0x60,                     // err: LDA #$28 SEC RTS
        };
        page_.fill(0xEA);
        page_[1] = 0x20; page_[3] = 0x00; page_[5] = 0x03; page_[7] = 0x00;
        page_[0xFF] = 0x0A;
        for (std::size_t i = 0; i < sizeof code; ++i) page_[0x0D + i] = code[i];
        page_[0x0A] = 0x60;                             // $C50A: RTS (the ProDOS entry)
    }
    std::string_view name() const override { return "SmartPort stub"; }
    uint8_t slotRomRead(uint8_t offset) override { return page_[offset]; }
    bool peekSlotRom(uint8_t offset, uint8_t& out) const override { out = page_[offset]; return true; }
private:
    std::array<uint8_t, 256> page_{};
};

void testSmartPort()
{
    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.slotBus().plug(5, std::make_unique<SmartPortStub>());
    pom2::Debugger dbg;
    dbg.setMemory(&mem);
    dbg.osCalls().bind(&cpu, &mem);
    dbg.osCalls().setSmartPort(true);
    CHECK(dbg.armed());
    cpu.setDebugHook(&dbg);
    poke(mem, 0x0800, {
        0x20, 0x0D, 0xC5, 0x00, 0x00, 0x09,         // JSR SP / STATUS / $0900
        0x20, 0x0D, 0xC5, 0x01, 0x10, 0x09,         // JSR SP / READBLOCK / $0910
        0x20, 0x0D, 0xC5, 0x41, 0x20, 0x09, 0x00, 0x00,   // JSR SP / READBLOCK.X / $00000920
        0x20, 0x0D, 0xC5, 0x05, 0x30, 0x09,         // JSR SP / INIT / $0930
        0x20, 0x0A, 0xC5,                           // JSR $C50A: the ProDOS entry, not a call
        0x4C, 0x1D, 0x08,                           // JMP *
    });
    poke(mem, 0x0900, { 0x03, 0x00, 0x40, 0x09, 0x00 });            // STATUS unit 0, code 0
    mem.memWrite(0x0940, 2);                                         // two devices
    poke(mem, 0x0910, { 0x03, 0x01, 0x00, 0x20, 0x05, 0x00, 0x00 }); // unit 1, buf $2000, block 5
    poke(mem, 0x0920, { 0x03, 0x02, 0x00, 0x30, 0x00, 0x00, 0x40, 0x06, 0x00, 0x00 });
    poke(mem, 0x0930, { 0x01, 0x00 });
    cpu.setStackPointer(0xFF);
    cpu.setProgramCounter(0x0800);
    for (int i = 0; i < 400; ++i) cpu.run(1);
    const auto l = lines(dbg.osCalls());
    CHECK(l.size() == 4);
    CHECK(l.size() > 0 && l[0] == "SP   $0800 S5 STATUS unit=0 code=$00 list=$0940 -> ok, 2 devices");
    CHECK(l.size() > 1 && l[1] == "SP   $0806 S5 READBLOCK unit=1 block=5 buf=$2000 -> ok, 512 bytes");
    CHECK(l.size() > 2 && l[2] == "SP   $080C S5 READBLOCK.X unit=2 block=1600 buf=$00003000 -> ok, 512 bytes");
    CHECK(l.size() > 3 && l[3] == "SP   $0814 S5 INIT unit=0 -> ERR $28 no device");
    if (l.size() != 4) dump(dbg.osCalls());
    dbg.osCalls().setSmartPort(false);
    CHECK(!dbg.armed());
}

void testPascal()
{
    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    pom2::Debugger dbg;
    dbg.setMemory(&mem);
    dbg.osCalls().bind(&cpu, &mem);
    dbg.osCalls().setPascal(true);
    CHECK(dbg.armed());
    cpu.setDebugHook(&dbg);
    // The table at $4000, every entry JMP $4100: RTS.
    mem.memWrite(0xEC, 0x00);
    mem.memWrite(0xED, 0x40);
    for (int e = 0; e < 21; ++e) poke(mem, uint16_t(0x4000 + 3 * e), { 0x4C, 0x00, 0x41 });
    mem.memWrite(0x4100, 0x60);
    // The caller pushes the block-I/O parameters the way the p-machine
    // leaves them: control, drive, buffer, length, block, then JSR.
    poke(mem, 0x0800, {
        0xA9, 'A', 0x20, 0x03, 0x40,                // CWRITE: not logged
        0xA9, 'X', 0x20, 0x09, 0x40,                // PWRITE 'X'
        0xA9, 0x00, 0x48, 0x48,                     // control = 0
        0xA9, 0x00, 0x48, 0xA9, 0x01, 0x48,         // drive = 1
        0xA9, 0x08, 0x48, 0xA9, 0x00, 0x48,         // buffer = $0800
        0xA9, 0x02, 0x48, 0xA9, 0x00, 0x48,         // length = 512
        0xA9, 0x00, 0x48, 0xA9, 0x02, 0x48,         // block = 2
        0xA9, 0x04, 0x20, 0x12, 0x40,               // DREAD, unit 4
        0x4C, 0x2B, 0x08,                           // JMP *
    });
    cpu.setStackPointer(0xFF);
    cpu.setProgramCounter(0x0800);
    for (int i = 0; i < 80; ++i) cpu.run(1);
    auto l = lines(dbg.osCalls());
    CHECK(l.size() == 2);
    CHECK(l.size() > 0 && l[0] == "PASC $0807 PWRITE 'X'");
    CHECK(l.size() > 1 && l[1] == "PASC $0828 DREAD unit=4 block=2 len=512 buf=$0800 drive=1 control=$0000");
    if (l.size() != 2) dump(dbg.osCalls());
    // No JMP at the entries: $EC/$ED is someone else's.
    dbg.osCalls().clear();
    for (int e = 0; e < 21; ++e) mem.memWrite(uint16_t(0x4000 + 3 * e), 0x60);
    cpu.setStackPointer(0xFF);
    cpu.setProgramCounter(0x0805);
    for (int i = 0; i < 80; ++i) cpu.run(1);
    CHECK(dbg.osCalls().lines().empty());
}

struct LineSink : pom2::CardTraceSink {
    std::vector<std::string> lines;
    void onCardLine(const char* l) override { lines.emplace_back(l); }
};

void testDiskHead()
{
    SlotBus bus;
    bus.plug(6, std::make_unique<DiskIICard>(6));
    LineSink sink;
    bus.setCardTrace(&sink);
    SlotPeripheral* d = bus.peripheral(6);
    CHECK(d->cardTrace() == &sink);
    d->deviceSelectRead(0x9);                    // motor on
    // Out two tracks: each next phase on, the previous off.
    const int seq[][2] = { {3, 0}, {5, 2}, {7, 4}, {1, 6} };
    for (const auto& s : seq) { d->deviceSelectRead(uint8_t(s[0])); d->deviceSelectRead(uint8_t(s[1])); }
    d->deviceSelectRead(0xC);                    // reading: the seek is over
    d->deviceSelectRead(0x8);                    // motor off: the 556 keeps it turning…
    CHECK(sink.lines.size() == 2);
    d->advanceCycles(2'000'000);                 // …until it runs out
    CHECK(sink.lines.size() == 3);
    CHECK(sink.lines.size() > 0 && sink.lines[0] == "DISK S6,D1 motor on, track 0");
    CHECK(sink.lines.size() > 1 && sink.lines[1] == "DISK S6,D1 seek 0 -> 2");
    CHECK(sink.lines.size() > 2 && sink.lines[2] == "DISK S6,D1 motor stopped");
    if (sink.lines.size() != 3) for (const auto& l : sink.lines) std::fprintf(stderr, "  | %s\n", l.c_str());
    bus.setCardTrace(nullptr);
    CHECK(d->cardTrace() == nullptr);
}

// A Monitor ROM page with the two entry points the trace knows: COUT at
// $FDED (JMP (CSW)) and GETLN at $FD6A (LDA $33, …): here GETLN "reads"
// the line already at $0200 and returns its length in X.
void testMonitor()
{
    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    std::vector<uint8_t> rom(0x0800, 0xEA);      // $F800-$FFFF
    auto at = [&](uint16_t a, std::initializer_list<int> b) { for (int v : b) rom[size_t(a++ - 0xF800)] = uint8_t(v); };
    at(0xFDED, { 0x6C, 0x36, 0x00 });            // COUT: JMP (CSW)
    at(0xFD6A, { 0xA5, 0x33, 0x20, 0xED, 0xFD,   // GETLN: LDA $33 / JSR COUT (the prompt)
                 0xA2, 0x02, 0x60 });            //        LDX #2 / RTS
    mem.loadRomBytes(rom.data(), rom.size(), 0xF800);
    poke(mem, 0x0036, { 0x00, 0x03 });           // CSW = $0300: RTS
    mem.memWrite(0x0300, 0x60);
    mem.memWrite(0x0033, 0xDD);                  // prompt ']'
    poke(mem, 0x0200, { 0xC8, 0xC9, 0x8D });     // "HI" CR
    pom2::Debugger dbg;
    dbg.setMemory(&mem);
    dbg.osCalls().bind(&cpu, &mem);
    dbg.osCalls().setMonitor(true);
    cpu.setDebugHook(&dbg);
    poke(mem, 0x0800, {
        0xA9, 0xCF, 0x20, 0xED, 0xFD,            // 'O'
        0xA9, 0xCB, 0x20, 0xED, 0xFD,            // 'K'
        0xA9, 0x8D, 0x20, 0xED, 0xFD,            // CR
        0x20, 0x6A, 0xFD,                        // JSR GETLN
        0x4C, 0x12, 0x08,                        // JMP *
    });
    cpu.setStackPointer(0xFF);
    cpu.setProgramCounter(0x0800);
    for (int i = 0; i < 40; ++i) cpu.run(1);
    const auto l = lines(dbg.osCalls());
    CHECK(l.size() == 2);
    CHECK(l.size() > 0 && l[0] == "MON  \"OK\"");
    CHECK(l.size() > 1 && l[1] == "MON  $080F GETLN ']' -> \"HI\"");
    if (l.size() != 2) dump(dbg.osCalls());
}

// Applecorn: the BBC program runs from aux RAM (RAMRD), the MOS in the aux
// language card; OSBYTE through its page-2 vector.
void testApplecorn()
{
    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(true);
    mem.memWrite(0xC003, 0);                     // RAMRD aux
    mem.memWrite(0xC005, 0);                     // RAMWRT aux
    mem.memWrite(0xC009, 0);                     // ALTZP aux
    (void)mem.memRead(0xC08B); (void)mem.memRead(0xC08B);   // LC bank 1, read + write RAM
    poke(mem, 0x020A, { 0x00, 0xD0 });           // OSBYTE vector -> $D000
    poke(mem, 0xD000, { 0xA2, 0x00, 0xA0, 0x80, 0x60 });   // LDX #0 / LDY #$80 / RTS
    poke(mem, 0x0800, {
        0xA9, 0x84, 0x20, 0x00, 0xD0,            // LDA #$84 / JSR (the vector's target)
        0x4C, 0x05, 0x08,                        // JMP *
    });
    pom2::Debugger dbg;
    dbg.setMemory(&mem);
    dbg.osCalls().bind(&cpu, &mem);
    dbg.osCalls().setApplecorn(true);
    cpu.setDebugHook(&dbg);
    cpu.setStackPointer(0xFF);
    cpu.setProgramCounter(0x0800);
    for (int i = 0; i < 12; ++i) cpu.run(1);
    const auto l = lines(dbg.osCalls());
    CHECK(l.size() == 1);
    CHECK(l.size() > 0 && l[0] == "MOS  OSBYTE $84 (read HIMEM) X=$00 Y=$00 -> A=$84 X=$00 Y=$80");
    if (l.size() != 1) dump(dbg.osCalls());
}

// CP/M-65: the BDOS ENTRY of src/bdos/dispatch.S and the BIOS jmpdispatch of
// src/lib/biosentry.S, at addresses the trace has to find for itself.
void testCpm65()
{
    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    poke(mem, 0x6000, {
        0x85, 0x10, 0x86, 0x11,                  // STA param / STX param+1
        0x20, 0x20, 0x60,                        // JSR BEGINSYS ($6020: RTS)
        0x20, 0x30, 0x60,                        // JSR icall
        0x4C, 0x20, 0x60,                        // JMP ENDSYS ($6020: RTS)
    });
    mem.memWrite(0x6020, 0x60);
    poke(mem, 0x6030, { 0xB9, 0x40, 0x60, 0x48, 0xB9, 0x50, 0x60, 0x48, 0x60 });   // icall
    mem.memWrite(0x6040 + 12, 0x60);             // hi (GET_VERSION - 1)
    mem.memWrite(0x6050 + 12, 0x5F);             // lo: $6060 - 1
    poke(mem, 0x6060, { 0xA9, 0x22, 0xA2, 0x00, 0x18, 0x60 });   // LDA #$22 / LDX #0 / CLC / RTS
    poke(mem, 0x6100, {                          // biosentry: jmpdispatch
        0x85, 0x12, 0xB9, 0x1D, 0x61, 0x48, 0xB9, 0x0D, 0x61, 0x48, 0xA5, 0x12, 0x60 });
    mem.memWrite(0x610D + 3, 0xFF);              // lo (SELDSK - 1) = $61FF
    mem.memWrite(0x611D + 3, 0x61);
    mem.memWrite(0x6200, 0x60);                  // SELDSK: RTS
    poke(mem, 0x0800, {
        0xA0, 0x0C, 0x20, 0x00, 0x60,            // LDY #12 / JSR BDOS
        0xA0, 0x03, 0xA9, 0x01, 0x20, 0x00, 0x61,   // LDY #3 / LDA #1 / JSR BIOS
        0x4C, 0x0C, 0x08,                        // JMP *
    });
    pom2::Debugger dbg;
    dbg.setMemory(&mem);
    dbg.osCalls().bind(&cpu, &mem);
    dbg.osCalls().setCpm65(true);
    cpu.setDebugHook(&dbg);
    cpu.setStackPointer(0xFF);
    cpu.setProgramCounter(0x0800);
    for (int i = 0; i < 40; ++i) cpu.run(1);
    const auto l = lines(dbg.osCalls());
    CHECK(l.size() == 2);
    CHECK(l.size() > 0 && l[0] == "CPM65 $0802 BDOS GET_VERSION -> A=$22 X=$00");
    CHECK(l.size() > 1 && l[1] == "CPM65 BIOS SELDSK B:");
    if (l.size() != 2) dump(dbg.osCalls());
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
    testSmartPort();
    testPascal();
    testDiskHead();
    testMonitor();
    testApplecorn();
    testCpm65();
    testSlotBusObserver();
    if (g_fail) {
        std::fprintf(stderr, "os_call_trace: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("os_call_trace: PASS\n");
    return 0;
}
