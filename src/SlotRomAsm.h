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

#pragma once

// SlotRomAsm — a two-pass assembler for POM2's hand-written slot ROMs.
//
// Six cards have no ROM dump and synthesise their $Cn00 page as 6502 opcode
// bytes: SmartPortCard, ProDOSHardDiskCard, FujiNetCard, PrinterCard,
// SuperSerialCard, and GrapplerCard's fallback stub. Until 2026-08-28 they all
// wrote the page the same way — a byte list, with every ADDRESS in it computed
// by hand:
//
//     0xF0, 0x37,              // BEQ write   (+55 -> $Cn91)
//     0x4C, 0xC0, kSlotRomHi,  // JMP $CnC0
//     rom[0xFF] = 0x50;        // ProDOS driver entry offset
//
// Three separate ways to be wrong, and all three have fired:
//
//   * A region outgrew its budget and overwrote its neighbour. SmartPortCard's
//     write routine ate its own ProDOS STATUS, which then answered $27 on a
//     healthy bay for weeks (2026-08-27).
//   * A region SHRANK and left a hand-computed displacement pointing past the
//     routine it names. Changes no byte a hexdump would flag.
//   * A displacement was simply mistyped. `BEQ +55` carried a comment
//     recording that somebody had already re-counted it once.
//
// SlotRom.h's bounded builder detected the first two. This removes the cause
// of all three: an address is never typed, it is a LABEL, and the assembler
// computes the byte. A branch that cannot reach its target, a reference to a
// label nobody defined, two regions claiming the same bytes, and a region that
// runs past its limit are all errors with a message that names the region.
//
// NOT constexpr, and the reason is worth stating because the plan asked for it
// (TODO P1-1). A slot ROM is parameterised by the slot it is plugged into —
// every absolute reference carries $Cn — and the slot comes from settings at
// runtime. There is no constant to fold. What the plan actually wanted is
// here: symbolic labels, declared and bounded regions, branches resolved
// automatically, and a listing you can diff. Failures surface through
// `failed()`, which every card publishes and every card's test asserts.
//
// Usage:
//
//     pom2::SlotRomAsm a(rom_, slot_);
//     a.region("boot", 0x20, 0x50)
//      .emit({ 0xA9, 0x01, 0x85, 0x42 })
//      .jsr("driver")
//      .branch(0xB0, "bootErr")      // BCS bootErr
//      .emit({ 0x4C, 0x01, 0x08 });
//     a.label("bootErr").emit({ ... });
//     a.region("driver", 0x50, 0x66)...;
//     a.region("tail", 0xFE, 0x100).emit({ 0xF7 }).byteOf("driver");
//     romLayoutError_ = !a.finish();

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace pom2 {

/// A slot ROM page is one 6502 page: $Cn00-$CnFF.
inline constexpr std::size_t kSlotRomBytes = 256;

class SlotRomAsm {
public:
    /// `slot` fixes the high byte of every absolute reference ($C0 + slot), so
    /// no card computes it by hand and no card can get it wrong for one
    /// reference while getting it right for the others.
    /// `name` is the card, and appears in the listing. Pass it: a dump that
    /// does not say which page it is loses most of its value when six cards
    /// print theirs one after another.
    SlotRomAsm(std::array<uint8_t, kSlotRomBytes>& rom, int slot,
               std::string_view name = {})
        : rom_(rom), pageHi_(static_cast<uint8_t>(0xC0 + slot)),
          name_(name) {}

    // ── Layout ───────────────────────────────────────────────────────────

    /// Open the region [start, limit) and define `name` as a label at its
    /// start. `limit` is EXCLUSIVE — it is the offset of whatever comes next,
    /// so a layout is declared by chaining its own constants with no
    /// off-by-one arithmetic. Regions may not overlap.
    SlotRomAsm& region(std::string_view name, unsigned start, unsigned limit)
    {
        if (start >= limit || limit > kSlotRomBytes) {
            fail(std::string("region '") + std::string(name) +
                 "' has an impossible span " + hex(start) + ".." + hex(limit));
            return *this;
        }
        for (const Region& r : regions_) {
            if (start < r.limit && r.start < limit) {
                fail(std::string("region '") + std::string(name) +
                     "' (" + hex(start) + ".." + hex(limit) + ") overlaps '" +
                     r.name + "' (" + hex(r.start) + ".." + hex(r.limit) + ")");
                return *this;
            }
        }
        regions_.push_back({ std::string(name), start, limit, start });
        open_  = regions_.size() - 1;
        pc_    = start;
        limit_ = limit;
        return define(name, start);
    }

    /// Name the current offset. Labels may be referenced before they are
    /// defined; everything resolves in `finish()`.
    SlotRomAsm& label(std::string_view name) { return define(name, pc_); }

    // ── Emission ─────────────────────────────────────────────────────────

    SlotRomAsm& emit(std::initializer_list<uint8_t> bytes)
    {
        for (uint8_t b : bytes) put(b);
        return *this;
    }

    /// A relative branch: `opcode` plus the displacement to `target`. The
    /// displacement is computed, never typed, and a target out of the
    /// -128..+127 window is an error rather than a wrapped byte.
    SlotRomAsm& branch(uint8_t opcode, std::string_view target)
    {
        put(opcode);
        fixups_.push_back({ Fixup::Rel, pc_, std::string(target), openName() });
        put(0x00);
        return *this;
    }

    /// `JMP label` / `JSR label` within this page. The $Cn high byte comes
    /// from the slot.
    SlotRomAsm& jmp(std::string_view target) { return absolute(0x4C, target); }
    SlotRomAsm& jsr(std::string_view target) { return absolute(0x20, target); }

    /// One byte holding a label's page offset. This is what a Pascal entry
    /// table ($Cn0D-$Cn10), a ProDOS driver-entry byte ($CnFF) and any
    /// `LDA #<routine` actually want.
    SlotRomAsm& byteOf(std::string_view target)
    {
        fixups_.push_back({ Fixup::Lo, pc_, std::string(target), openName() });
        put(0x00);
        return *this;
    }

    /// The page's own high byte, for the rare `LDA #>routine`.
    uint8_t pageHi() const { return pageHi_; }

    /// Write one byte at a fixed offset inside the open region — signature
    /// bytes and identification tails, which sit at addresses the platform
    /// mandates rather than wherever the code reached. Going through the
    /// region keeps them inside the overlap check.
    SlotRomAsm& poke(unsigned offset, uint8_t value)
    {
        if (open_ == kNoRegion || offset < regions_[open_].start ||
            offset >= regions_[open_].limit) {
            fail("poke " + hex(offset) + " is outside the open region '" +
                 std::string(openName()) + "'");
            return *this;
        }
        rom_[offset] = value;
        if (offset >= regions_[open_].used) regions_[open_].used = offset + 1;
        return *this;
    }

    // ── Finish ───────────────────────────────────────────────────────────

    /// Resolve every pending reference. Call exactly once, at the end of
    /// buildRom(). Returns true when the page assembled cleanly.
    ///
    /// Resolve first, WRITE second, and only when nothing failed. `put()`
    /// refuses to write past `limit_` but does NOT advance `pc_`, so a
    /// `branch()` / `jmp()` / `byteOf()` emitted at an overflow recorded its
    /// operand at `at == limit_` — the neighbour's first byte, which the
    /// fixup pass then overwrote *after* the overflow had already been
    /// reported, and index 256 of a 256-byte `std::array` when the region
    /// ends the page. A page that failed anywhere is not going to be used
    /// (every card publishes `romLayoutError()`), so patching it buys
    /// nothing and costs the neighbour.
    bool finish()
    {
        std::vector<Write> writes;
        writes.reserve(fixups_.size());
        for (const Fixup& f : fixups_) {
            if (f.at >= kSlotRomBytes) {
                fail("region '" + f.region + "' references '" + f.target +
                     "' from " + hex(f.at) + ", past the end of the page");
                continue;
            }
            const Label* l = find(f.target);
            if (!l) {
                fail("region '" + f.region + "' references undefined label '" +
                     f.target + "'");
                continue;
            }
            if (f.kind == Fixup::Rel) {
                // The operand follows the opcode, so the CPU adds to at + 1.
                const int d = static_cast<int>(l->offset) -
                              static_cast<int>(f.at + 1);
                if (d < -128 || d > 127) {
                    fail("branch in region '" + f.region + "' at " +
                         hex(f.at - 1) + " cannot reach '" + f.target +
                         "' (" + hex(l->offset) + "): " + std::to_string(d) +
                         " bytes away");
                    continue;
                }
                writes.push_back({ f.at, static_cast<uint8_t>(d) });
            } else {
                writes.push_back({ f.at, static_cast<uint8_t>(l->offset) });
            }
        }
        fixups_.clear();
        if (!failed_)
            for (const Write& w : writes) rom_[w.at] = w.value;
        // The listing exists to be diffed across a change, so it has to be
        // obtainable from a running build and not only from a debugger:
        //     POM2_DUMP_SLOT_ROM=1 ./POM2 2>slotroms.txt
        // Every card prints its page as it is built, which is the form you
        // want — one file per build, diffable against the last one.
        if (dumpRequested()) std::fputs(listing().c_str(), stderr);
        return !failed_;
    }

    bool failed() const { return failed_; }
    /// The FIRST error, which is the one worth reading: later ones are usually
    /// its consequences.
    const std::string& error() const { return error_; }

    /// A listing meant to be diffed across a change: what each region claims,
    /// how much of it is used, where its labels are, and the bytes.
    std::string listing() const
    {
        std::string out = "; " + (name_.empty() ? std::string("slot ROM") : name_) +
                          " — page $" + hex2(pageHi_) + "00\n";
        for (const Region& r : regions_) {
            out += "\n" + r.name + "  " + hex(r.start) + ".." + hex(r.limit) +
                   "  " + std::to_string(r.used - r.start) + " of " +
                   std::to_string(r.limit - r.start) + " bytes\n";
            for (const Label& l : labels_)
                if (l.offset >= r.start && l.offset < r.limit && l.name != r.name)
                    out += "    " + hex(l.offset) + "  " + l.name + ":\n";
            for (unsigned a = r.start; a < r.used; a += 8) {
                out += "    " + hex(a) + " ";
                for (unsigned b = a; b < a + 8 && b < r.used; ++b)
                    out += " " + hex2(rom_[b]);
                out += "\n";
            }
        }
        if (failed_) out += "\nERROR: " + error_ + "\n";
        return out;
    }

private:
    struct Region { std::string name; unsigned start, limit, used; };
    struct Label  { std::string name; unsigned offset; };
    struct Fixup  {
        enum Kind { Rel, Lo } kind;
        unsigned    at;
        std::string target;
        std::string region;
    };
    struct Write  { unsigned at; uint8_t value; };
    static constexpr std::size_t kNoRegion = static_cast<std::size_t>(-1);

    void put(uint8_t b)
    {
        if (open_ == kNoRegion) { fail("emitted a byte with no region open"); return; }
        if (pc_ >= limit_) {
            fail("region '" + regions_[open_].name + "' (" +
                 hex(regions_[open_].start) + ".." + hex(limit_) +
                 ") ran out of room");
            return;
        }
        rom_[pc_++] = b;
        if (pc_ > regions_[open_].used) regions_[open_].used = pc_;
    }

    SlotRomAsm& absolute(uint8_t opcode, std::string_view target)
    {
        put(opcode);
        fixups_.push_back({ Fixup::Lo, pc_, std::string(target), openName() });
        put(0x00);
        put(pageHi_);
        return *this;
    }

    SlotRomAsm& define(std::string_view name, unsigned offset)
    {
        if (find(name))
            fail("label '" + std::string(name) + "' defined twice");
        else
            labels_.push_back({ std::string(name), offset });
        return *this;
    }

    const Label* find(std::string_view name) const
    {
        for (const Label& l : labels_) if (l.name == name) return &l;
        return nullptr;
    }

    std::string openName() const
    {
        return open_ == kNoRegion ? std::string("<none>") : regions_[open_].name;
    }

    void fail(const std::string& what)
    {
        if (!failed_) { failed_ = true; error_ = what; }
    }

    static bool dumpRequested()
    {
        static const bool on = std::getenv("POM2_DUMP_SLOT_ROM") != nullptr;
        return on;
    }

    static std::string hex(unsigned v)
    {
        char b[16];
        std::snprintf(b, sizeof b, "$%03X", v);
        return b;
    }
    static std::string hex2(unsigned v)
    {
        char b[8];
        std::snprintf(b, sizeof b, "%02X", v & 0xFF);
        return b;
    }

    std::array<uint8_t, kSlotRomBytes>& rom_;
    uint8_t     pageHi_;
    std::string name_;
    std::vector<Region> regions_;
    std::vector<Label>  labels_;
    std::vector<Fixup>  fixups_;
    std::size_t open_   = kNoRegion;
    unsigned    pc_     = 0;
    unsigned    limit_  = 0;
    bool        failed_ = false;
    std::string error_;
};

} // namespace pom2
