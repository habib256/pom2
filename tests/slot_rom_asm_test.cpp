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

// SlotRomAsm — the assembler itself.
//
// Six cards publish `romLayoutError()` and each of their tests asserts it is
// CLEAR. That is necessary and nowhere near sufficient: an assembler whose
// `finish()` was `return true` would satisfy every one of them. Something has
// to assert the failures are reachable, and prove the arithmetic on values
// that were previously typed by hand. That is this file.
//
// The three ways a hand-assembled page went wrong, all of which had fired:
// a region outgrew its budget and ate its neighbour; a region shrank and left
// a displacement pointing past the routine it named; a displacement was simply
// mistyped. The first is still detected here. The other two are gone — an
// address is a label now, and the assembler computes the byte.

#include "SlotRomAsm.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void expect(bool cond, const std::string& what)
{
    if (!cond) { std::printf("FAIL: %s\n", what.c_str()); ++failures; }
}

/// The error must NAME the thing that went wrong. A layout error that says
/// only "false" sends the reader back to counting bytes, which is the activity
/// this whole exercise exists to abolish.
void expectErrorMentions(const pom2::SlotRomAsm& a,
                         std::initializer_list<const char*> needles,
                         const std::string& what)
{
    if (!a.failed()) { std::printf("FAIL: %s — no error at all\n", what.c_str()); ++failures; return; }
    for (const char* n : needles) {
        if (a.error().find(n) == std::string::npos) {
            std::printf("FAIL: %s — error does not mention '%s': %s\n",
                        what.c_str(), n, a.error().c_str());
            ++failures;
        }
    }
}

using Page = std::array<uint8_t, pom2::kSlotRomBytes>;

} // namespace

int main()
{
    // ── A clean page assembles, and the bytes land where declared ────────
    {
        Page rom{}; rom.fill(0xEA);
        pom2::SlotRomAsm a(rom, 5);
        a.region("boot", 0x20, 0x50).emit({ 0xA9, 0x01, 0x60 });
        expect(a.finish(), "a well-formed page must assemble");
        expect(!a.failed(), "and must not report an error");
        expect(rom[0x20] == 0xA9 && rom[0x21] == 0x01 && rom[0x22] == 0x60,
               "bytes must land at the region's start");
        expect(rom[0x23] == 0xEA, "nothing may be written past the last byte");
    }

    // ── A forward branch resolves; the label may not exist yet ───────────
    // This is the case hand-assembly could not do at all: `BEQ read` had to be
    // written as a number because the routine it names comes later in the file.
    {
        Page rom{};
        pom2::SlotRomAsm a(rom, 5);
        a.region("dispatch", 0x50, 0x66)
         .emit({ 0xA5, 0x42, 0xC9, 0x01 })
         .branch(0xF0, "read");                       // opcode $Cn54, operand $Cn55
        a.region("read", 0x66, 0x91).emit({ 0x60 });
        expect(a.finish(), "a forward branch must resolve");
        expect(rom[0x54] == 0xF0, "the branch opcode survives");
        // $Cn66 - ($Cn55 + 1) = $10. This is the literal the HDV dispatch used
        // to carry as `0xF0, 0x10  // BEQ read (+16)`.
        expect(rom[0x55] == 0x10,
               "forward displacement must be $10, got $" +
               std::to_string(rom[0x55]));
    }

    // ── A backward branch resolves, and negative displacements are right ─
    {
        Page rom{};
        pom2::SlotRomAsm a(rom, 5);
        a.region("loop", 0x50, 0x60)
         .label("top").emit({ 0xAD, 0x09, 0xC0, 0x29, 0x10 })
         .branch(0xF0, "top");                        // opcode $Cn55, operand $Cn56
        expect(a.finish(), "a backward branch must resolve");
        // $Cn50 - ($Cn56 + 1) = -7 = $F9. The SSC spin loops all use it.
        expect(rom[0x56] == 0xF9,
               "backward displacement must be $F9 (-7)");
    }

    // ── Out of range is an error, not a wrapped byte ─────────────────────
    {
        Page rom{};
        pom2::SlotRomAsm a(rom, 5);
        a.region("far", 0x00, 0x10).branch(0xD0, "target");
        a.region("target", 0xF0, 0x100).emit({ 0x60 });
        expect(!a.finish(), "a branch that cannot reach must fail");
        expectErrorMentions(a, { "far", "target" },
                            "an out-of-range branch");
    }

    // ── A reference to a label nobody defined ────────────────────────────
    {
        Page rom{};
        pom2::SlotRomAsm a(rom, 5);
        a.region("boot", 0x20, 0x50).jsr("driver");
        expect(!a.finish(), "an undefined label must fail");
        expectErrorMentions(a, { "driver", "boot" }, "an undefined label");
    }

    // ── The same name defined twice ──────────────────────────────────────
    {
        Page rom{};
        pom2::SlotRomAsm a(rom, 5);
        a.region("boot", 0x20, 0x50).label("here").emit({ 0xEA }).label("here");
        expect(!a.finish(), "a duplicate label must fail");
        expectErrorMentions(a, { "here" }, "a duplicate label");
    }

    // ── Two regions claiming the same bytes ──────────────────────────────
    // The SmartPort bug in its purest form, caught before a byte is written.
    // The bounded builder this replaced could not see it at all: it checked
    // each region against its own limit, never against the others.
    {
        Page rom{};
        pom2::SlotRomAsm a(rom, 5);
        a.region("write", 0x91, 0xC8);
        a.region("status", 0xC0, 0xE0);
        expect(!a.finish(), "overlapping regions must fail");
        expectErrorMentions(a, { "write", "status" }, "a region overlap");
    }

    // ── A region that runs past its limit ────────────────────────────────
    {
        Page rom{}; rom.fill(0xEA);
        rom[0x04] = 0x4C;                      // the neighbour's first byte
        pom2::SlotRomAsm a(rom, 5);
        a.region("tight", 0x00, 0x04).emit({ 1, 2, 3, 4, 5 });
        expect(!a.finish(), "an over-budget region must fail");
        expectErrorMentions(a, { "tight" }, "a region overflow");
        expect(rom[0x04] == 0x4C,
               "an over-budget region must not touch its neighbour");
    }

    // ── A failed page must not be patched afterwards ─────────────────────
    // `put()` refuses to write past `limit_` but does NOT advance `pc_`, so a
    // branch or a jmp emitted at an overflow recorded its operand at
    // `at == limit_` — the neighbour's first byte. `finish()` applied the
    // fixups of a failed assembly anyway, so the overflow it had just
    // reported went on to corrupt the region next door.
    {
        Page rom{}; rom.fill(0xEA);
        rom[0x10] = 0x4C;                      // the neighbour's first byte
        pom2::SlotRomAsm a(rom, 5);
        a.region("tight", 0x00, 0x10)
         .emit({ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 })
         .branch(0xD0, "target");              // one byte past the budget
        a.region("target", 0x20, 0x30).emit({ 0x60 });
        expect(!a.finish(), "an over-budget branch must fail");
        expectErrorMentions(a, { "tight" }, "an over-budget branch");
        expect(rom[0x10] == 0x4C,
               "a fixup from a failed page must not touch the neighbour");
    }

    // ── …and never one past the end of the page ──────────────────────────
    // The same overflow in a region that ENDS the page recorded `at == 256`,
    // which is out of bounds on the card's `std::array<uint8_t, 256>`. The
    // canary sits immediately after the page (both are byte-aligned, so the
    // struct has no padding) and catches the write the sanitisers would only
    // see on a heap-allocated member.
    {
        struct Guarded { Page page; uint8_t canary; };
        static_assert(sizeof(Guarded) == pom2::kSlotRomBytes + 1,
                      "the canary must sit at page offset 256");
        Guarded g{}; g.page.fill(0xEA); g.canary = 0x5A;
        pom2::SlotRomAsm a(g.page, 5);
        a.region("tail", 0xFC, 0x100).emit({ 1, 2, 3, 4 }).jmp("tail");
        expect(!a.finish(), "a jmp emitted past the end of the page must fail");
        expect(g.canary == 0x5A, "rom_[256] must never be written");
    }

    // ── Exactly filling a region is legal ────────────────────────────────
    // Several real layouts do it — FujiNetCard's boot routine ends on the very
    // byte before its successor. Crying wolf there would be worse than useless.
    {
        Page rom{};
        pom2::SlotRomAsm a(rom, 5);
        a.region("exact", 0x00, 0x04).emit({ 1, 2, 3, 4 });
        expect(a.finish(), "an exactly-full region must assemble");
    }

    // ── jmp / jsr carry the page's own high byte ─────────────────────────
    // Every card used to compute `0xC0 + slot` itself, once per reference.
    {
        Page rom{};
        pom2::SlotRomAsm a(rom, 7);
        a.region("boot", 0x20, 0x50).jmp("driver").jsr("driver");
        a.region("driver", 0x60, 0x70).emit({ 0x60 });
        expect(a.finish(), "jmp/jsr must resolve");
        expect(rom[0x20] == 0x4C && rom[0x21] == 0x60 && rom[0x22] == 0xC7,
               "JMP must be 4C <lo> $C7 in slot 7");
        expect(rom[0x23] == 0x20 && rom[0x24] == 0x60 && rom[0x25] == 0xC7,
               "JSR must be 20 <lo> $C7 in slot 7");
        expect(a.pageHi() == 0xC7, "pageHi() must follow the slot");
    }

    // ── byteOf: the Pascal entry table and $CnFF ─────────────────────────
    {
        Page rom{};
        pom2::SlotRomAsm a(rom, 2);
        a.region("entries", 0x0D, 0x11)
         .byteOf("pinit").byteOf("pread").byteOf("pwrite").byteOf("pstatus");
        a.region("pinit",   0x50, 0x60).emit({ 0x60 });
        a.region("pread",   0x60, 0x70).emit({ 0x60 });
        a.region("pwrite",  0x70, 0x80).emit({ 0x60 });
        a.region("pstatus", 0x80, 0xB0).emit({ 0x60 });
        expect(a.finish(), "byteOf must resolve");
        expect(rom[0x0D] == 0x50 && rom[0x0E] == 0x60 &&
               rom[0x0F] == 0x70 && rom[0x10] == 0x80,
               "the entry table must hold each routine's page offset");
    }

    // ── poke stays inside the open region ────────────────────────────────
    {
        Page rom{}; rom.fill(0xEA);
        pom2::SlotRomAsm a(rom, 5);
        a.region("sig", 0x05, 0x0D).poke(0x05, 0x38).poke(0x07, 0x18);
        expect(a.finish(), "poking inside the region is fine");
        expect(rom[0x05] == 0x38 && rom[0x07] == 0x18, "the poked bytes land");
        expect(rom[0x06] == 0xEA, "the gaps keep their fill");
    }
    {
        Page rom{};
        pom2::SlotRomAsm a(rom, 5);
        a.region("sig", 0x05, 0x0D).poke(0x20, 0x38);
        expect(!a.finish(), "poking outside the open region must fail");
        expectErrorMentions(a, { "sig" }, "an out-of-region poke");
    }

    // ── Emitting with no region open ─────────────────────────────────────
    {
        Page rom{};
        pom2::SlotRomAsm a(rom, 5);
        a.emit({ 0xEA });
        expect(!a.finish(), "emitting with no region open must fail");
        expectErrorMentions(a, { "region" }, "an unscoped emit");
    }

    // ── The listing names regions, labels and their occupancy ────────────
    // It exists to be diffed across a change; a listing that omitted the
    // occupancy would hide exactly the drift that matters.
    {
        Page rom{};
        pom2::SlotRomAsm a(rom, 5);
        a.region("boot", 0x20, 0x50).emit({ 0xA9, 0x01 }).label("bootErr")
         .emit({ 0x60 });
        a.finish();
        const std::string l = a.listing();
        expect(l.find("boot") != std::string::npos, "listing names the region");
        expect(l.find("bootErr") != std::string::npos, "listing names labels");
        expect(l.find("3 of 48") != std::string::npos,
               "listing reports used-of-budget, got:\n" + l);
        expect(l.find("A9 01 60") != std::string::npos, "listing dumps bytes");
    }

    // ── The first error is kept, not the last ────────────────────────────
    // Later errors are usually consequences of the first; reporting the tail
    // of a cascade would point at the wrong region.
    {
        Page rom{};
        pom2::SlotRomAsm a(rom, 5);
        a.region("first", 0x00, 0x02).emit({ 1, 2, 3 });
        a.region("second", 0x10, 0x12).emit({ 1, 2, 3 });
        a.finish();
        expectErrorMentions(a, { "first" }, "the first error is the one kept");
        expect(a.error().find("second") == std::string::npos,
               "a later error must not overwrite the first");
    }

    if (failures == 0) std::printf("slot_rom_asm: OK\n");
    return failures == 0 ? 0 : 1;
}
