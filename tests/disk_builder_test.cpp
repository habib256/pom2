// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// disk_builder_test — pins DiskBuilder (blank DOS 3.3 data disk, direct-boot
// disk, "not a startup disk" sector, sector orders).
//
// Two kinds of proof, because self-consistency can hide a mistake:
//   * the sector orders are checked against PUBLISHED tables (Beneath Apple
//     DOS Fig. 3.24, Beneath Apple ProDOS's block card), and the blank disk
//     against POM2's own Dos33DiskFs, which was written independently;
//   * both boot sectors are WALKED by a 6502 decoder (every branch/jump target
//     and absolute operand inside the sector or at a named ROM entry) and RUN
//     by a small 6502 interpreter with a fake PROM read entry at $Cs5C, a
//     fake head stepper and fake Monitor routines. The fake PROM does only
//     what DiskIIBootRom.h's contract says the PROMs share; the fake Monitor
//     routines clobber the registers they are not documented to keep, so the
//     code cannot lean on them by accident.
//
// No external media, no temp files.

#include "DiskBuilder.h"
#include "Dos33DiskFs.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace db = pom2::diskbuild;
namespace fs = pom2::dos33fs;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

constexpr std::size_t kSec = 256;

std::size_t at(int track, int fileIndex) { return (std::size_t(track) * 16 + std::size_t(fileIndex)) * kSec; }

// ── Published tables ───────────────────────────────────────────────────────

// [B] Beneath Apple DOS (4th printing, 1982) Fig. 3.24 p. 3-23: physical
//     sector 0-F → DOS 3.3 logical sector, and → Pascal logical sector.
constexpr int kBadPhysToDos33[16]  = { 0x0, 0x7, 0xE, 0x6, 0xD, 0x5, 0xC, 0x4,
                                       0xB, 0x3, 0xA, 0x2, 0x9, 0x1, 0x8, 0xF };
constexpr int kBadPhysToPascal[16] = { 0x0, 0x8, 0x1, 0x9, 0x2, 0xA, 0x3, 0xB,
                                       0x4, 0xC, 0x5, 0xD, 0x6, 0xE, 0x7, 0xF };
// [B] Beneath Apple ProDOS (1985) reference card: a track's blocks 0-7 are
//     physical sectors 0&2, 4&6, 8&A, C&E, 1&3, 5&7, 9&B, D&F.
constexpr int kBapBlockPhysical[8][2] = { { 0, 2 }, { 4, 6 }, { 8, 10 }, { 12, 14 },
                                          { 1, 3 }, { 5, 7 }, { 9, 11 }, { 13, 15 } };
// POM2's own tables, restated: DiskImage.cpp kDos33LogicalForPhysical /
// kProDosLogicalForPhysical, and Dos33DiskFs.cpp kDosToProdos.
constexpr int kDiskImageDos[16]    = { 0, 7, 14, 6, 13, 5, 12, 4, 11, 3, 10, 2, 9, 1, 8, 15 };
constexpr int kDiskImageProdos[16] = { 0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15 };
constexpr int kDos33FsDosToProdos[16] = { 0, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 15 };

// ── A 6502 instruction table: only the opcodes the two sectors use ─────────

enum class Mode { Imp, Imm, Zp, Abs, AbsX, AbsY, Ind, Rel };
enum class Flow { Next, Branch, Jsr, Jmp, JmpInd, Rts };
struct OpInfo { uint8_t op; Mode mode; Flow flow; };

constexpr OpInfo kOps[] = {
    { 0xA9, Mode::Imm,  Flow::Next },  { 0xA5, Mode::Zp,   Flow::Next },  // LDA
    { 0xAD, Mode::Abs,  Flow::Next },  { 0xBD, Mode::AbsX, Flow::Next },
    { 0xB9, Mode::AbsY, Flow::Next },
    { 0xA6, Mode::Zp,   Flow::Next },  { 0xAE, Mode::Abs,  Flow::Next },  // LDX
    { 0xA0, Mode::Imm,  Flow::Next },  { 0xAC, Mode::Abs,  Flow::Next },  // LDY
    { 0x85, Mode::Zp,   Flow::Next },  { 0x8D, Mode::Abs,  Flow::Next },  // STA
    { 0xEE, Mode::Abs,  Flow::Next },  { 0xCE, Mode::Abs,  Flow::Next },  // INC DEC
    { 0x29, Mode::Imm,  Flow::Next },  { 0x09, Mode::Imm,  Flow::Next },  // AND ORA
    { 0x05, Mode::Zp,   Flow::Next },  { 0x69, Mode::Imm,  Flow::Next },  // ORA ADC
    { 0x18, Mode::Imp,  Flow::Next },  { 0x4A, Mode::Imp,  Flow::Next },  // CLC LSR
    { 0x0A, Mode::Imp,  Flow::Next },  { 0xAA, Mode::Imp,  Flow::Next },  // ASL TAX
    { 0x88, Mode::Imp,  Flow::Next },                                     // DEY
    { 0xF0, Mode::Rel,  Flow::Branch }, { 0xD0, Mode::Rel, Flow::Branch }, // BEQ BNE
    { 0x20, Mode::Abs,  Flow::Jsr },   { 0x60, Mode::Imp,  Flow::Rts },
    { 0x4C, Mode::Abs,  Flow::Jmp },   { 0x6C, Mode::Ind,  Flow::JmpInd },
};

const OpInfo* lookup(uint8_t op)
{
    for (const auto& o : kOps) if (o.op == op) return &o;
    return nullptr;
}

int lengthOf(Mode m)
{
    switch (m) {
        case Mode::Imp: return 1;
        case Mode::Imm: case Mode::Zp: case Mode::Rel: return 2;
        default: return 3;
    }
}

std::string hex(unsigned v, int digits = 4)
{
    char t[8];
    std::snprintf(t, sizeof(t), "$%0*X", digits, v);
    return t;
}

// ── The walker ─────────────────────────────────────────────────────────────

struct WalkRules {
    uint16_t           codeEnd = 0x0880;   // first byte that is data, not code
    std::set<uint16_t> romCalls;            // JSR targets outside the sector
    std::set<uint16_t> romJumps;            // JMP targets outside the sector
    std::set<uint16_t> indirectVectors;     // JMP (vector) operands allowed
    std::set<uint8_t>  zeroPage;            // zero-page cells it may touch
    std::set<uint16_t> slotIo;              // abs,X bases indexed by slot*16
};

// Decodes the sector (loaded at $0800) from $0801 along every path. Returns
// the problems found; empty = every instruction decodes, every target is
// inside the code or a named ROM entry, every operand is sector data, a
// named zero-page cell or a slot soft switch, and the code bytes are covered
// exactly once with no gap.
std::vector<std::string> walk(const std::vector<uint8_t>& sector, const WalkRules& r)
{
    std::vector<std::string> problems;
    std::vector<int> owner(0x100, -1);      // instruction start owning each byte
    std::vector<uint16_t> work{ 0x0801 };
    std::set<uint16_t> seen;
    const auto inCode = [&](uint32_t a) { return a >= 0x0801 && a < r.codeEnd; };
    const auto isData = [&](uint32_t a) { return a == 0x0800 || (a >= r.codeEnd && a < 0x0900); };
    while (!work.empty()) {
        const uint16_t pc = work.back();
        work.pop_back();
        if (!seen.insert(pc).second) continue;
        if (!inCode(pc)) { problems.push_back("control reaches " + hex(pc) + ", outside the code"); continue; }
        const uint8_t opcode = sector[pc - 0x0800u];
        const OpInfo* op = lookup(opcode);
        if (!op) { problems.push_back("unknown opcode " + hex(opcode, 2) + " at " + hex(pc)); continue; }
        const int len = lengthOf(op->mode);
        if (!inCode(uint32_t(pc) + uint32_t(len) - 1)) {
            problems.push_back("instruction at " + hex(pc) + " runs past the code");
            continue;
        }
        for (int i = 0; i < len; ++i) {
            int& o = owner[std::size_t(pc - 0x0800 + i)];
            if (o != -1 && o != pc) problems.push_back("instructions overlap at " + hex(unsigned(pc + i)));
            o = pc;
        }
        const uint8_t b1 = len > 1 ? sector[pc - 0x0800u + 1] : 0;
        const uint16_t w = len > 2 ? uint16_t(b1 | sector[pc - 0x0800u + 2] << 8) : b1;
        const uint16_t next = uint16_t(pc + len);
        switch (op->flow) {
            case Flow::Branch: {
                const uint16_t target = uint16_t(next + int8_t(b1));
                if (!inCode(target)) problems.push_back("branch at " + hex(pc) + " to " + hex(target));
                else work.push_back(target);
                work.push_back(next);
                break;
            }
            case Flow::Jsr:
                if (inCode(w)) work.push_back(w);
                else if (!r.romCalls.count(w)) problems.push_back("JSR " + hex(w) + " at " + hex(pc) + " is not a named ROM entry");
                work.push_back(next);
                break;
            case Flow::Jmp:
                if (inCode(w)) work.push_back(w);
                else if (!r.romJumps.count(w)) problems.push_back("JMP " + hex(w) + " at " + hex(pc) + " is not an allowed target");
                break;
            case Flow::JmpInd:
                if (!r.indirectVectors.count(w)) problems.push_back("JMP (" + hex(w) + ") at " + hex(pc));
                break;
            case Flow::Rts:
                break;
            case Flow::Next:
                switch (op->mode) {
                    case Mode::Zp:
                        if (!r.zeroPage.count(b1)) problems.push_back("zero page " + hex(b1, 2) + " at " + hex(pc));
                        break;
                    case Mode::Abs: case Mode::AbsY:
                        if (!isData(w)) problems.push_back("operand " + hex(w) + " at " + hex(pc) + " is not sector data");
                        break;
                    case Mode::AbsX:
                        if (!isData(w) && !r.slotIo.count(w))
                            problems.push_back("operand " + hex(w) + ",X at " + hex(pc));
                        break;
                    default: break;
                }
                work.push_back(next);
                break;
        }
    }
    for (uint32_t a = 0x0801; a < r.codeEnd; ++a)
        if (owner[a - 0x0800] == -1) problems.push_back("byte " + hex(a) + " is never executed");
    return problems;
}

// ── A small 6502 + Disk II + Monitor, enough for the two boot sectors ──────

struct Machine {
    std::vector<uint8_t> mem = std::vector<uint8_t>(0x10000, 0xEE);   // power-up garbage
    uint8_t a = 0, x = 0, y = 0, sp = 0xFF;
    bool c = false, z = false, n = false;
    uint16_t pc = 0;

    // The drive.
    std::vector<uint8_t> image;      // DOS-ordered
    int slot = 6;
    int halfTrack = 0;
    std::array<bool, 4> magnet{};
    bool motor = false;

    // What happened.
    std::vector<std::string> problems;
    std::vector<std::pair<int, int>> reads;   // (track, physical sector) per PROM read
    std::vector<std::string> calls;           // Monitor routines, in order
    std::vector<uint16_t> indirect;           // JMP (ind) targets
    std::string screen;
    int waits = 0;
    std::string stop;
    uint16_t entry = 0;                       // stop when PC gets here (0: none)

    uint8_t s16() const { return uint8_t(slot << 4); }
    uint16_t readEntry() const { return uint16_t(0xC05C | (slot << 8)); }

    void softSwitch(int reg)
    {
        if (reg < 8) {
            magnet[std::size_t(reg >> 1)] = (reg & 1) != 0;
            // The head moves one half-track toward an energised neighbour
            // magnet when the other neighbour is off; two phases = one track
            // (the PROMs' own recalibrate walks 4 phases per 2 tracks). A test
            // model, not DiskIICard's: enough to tell a correct step sequence
            // from a wrong one, blind to timing and quarter-tracks.
            const int h = halfTrack;
            const bool up = magnet[std::size_t((h + 1) & 3)], down = magnet[std::size_t((h + 3) & 3)];
            if (up && !down && h < 68) ++halfTrack;
            else if (down && !up && h > 0) --halfTrack;
        } else if (reg == 8) {
            motor = false;
        } else if (reg == 9) {
            motor = true;
        }
    }
    bool isSlotIo(uint16_t addr) const { return (addr & 0xFFF0) == 0xC080 + s16(); }
    uint8_t read(uint16_t addr)
    {
        if (isSlotIo(addr)) { softSwitch(addr & 0xF); return 0x00; }
        if (addr >= 0xC000) problems.push_back("read of " + hex(addr));
        return mem[addr];
    }
    void write(uint16_t addr, uint8_t v)
    {
        if (isSlotIo(addr)) { softSwitch(addr & 0xF); return; }
        if (addr >= 0x0200 && (addr & 0xFF00) != 0x0800) problems.push_back("CPU store to " + hex(addr));
        mem[addr] = v;
    }
    void push(uint8_t v) { mem[0x0100u + sp] = v; --sp; }
    uint8_t pull() { ++sp; return mem[0x0100u + sp]; }
    void rts() { const uint8_t lo = pull(); const uint8_t hi = pull(); pc = uint16_t((lo | hi << 8) + 1); }
    void nz(uint8_t v) { z = v == 0; n = (v & 0x80) != 0; }
    uint16_t fetch16() { const uint16_t w = uint16_t(mem[pc] | mem[uint16_t(pc + 1)] << 8); pc = uint16_t(pc + 2); return w; }

    // The PROM's read entry, as DiskIIBootRom.h's contract states it and as
    // Apple's 341-0027-A, POM2's own and the //c's all implement it: find
    // physical sector $3D on the track whose address fields say $41, read it
    // into ($26) with $26 = 0, INC $27, INC $3D, loop while $3D < ($0800),
    // then JMP $0801 with X = slot*16.
    void promRead()
    {
        for (int guard = 0; guard < 256; ++guard) {
            if (!motor) { problems.push_back("PROM entered with the motor off"); stop = "fault"; return; }
            if (mem[0x26] != 0) problems.push_back("$26 is not 0 at a PROM read");
            const int track = halfTrack / 2;
            if ((halfTrack & 1) || track != mem[0x41]) {
                problems.push_back("head on half-track " + std::to_string(halfTrack) + " while $41 = "
                                   + std::to_string(mem[0x41]) + ": the PROM would never find its sector");
                stop = "fault";
                return;
            }
            const int sector = mem[0x3D];
            if (sector > 15 || track >= 35) { problems.push_back("no such sector"); stop = "fault"; return; }
            const uint16_t dest = uint16_t(mem[0x27] << 8);
            if (dest >= 0xC000 || dest < 0x0800) problems.push_back("PROM read into page " + hex(dest));
            std::copy_n(image.begin() + std::ptrdiff_t(at(track, db::dosFileIndexForPhysicalSector(sector))),
                        kSec, mem.begin() + dest);
            reads.push_back({ track, sector });
            ++mem[0x27];
            ++mem[0x3D];
            if (mem[0x3D] < mem[0x0800]) continue;
            x = s16();
            pc = 0x0801;
            return;
        }
        problems.push_back("PROM read loop did not end");
        stop = "fault";
    }

    // Power-up boot: the PROM has recalibrated (head on track 0; magnet 0 on
    // for Apple's and the //c's recalibrate, all off for POM2's), set $26 = 0,
    // $27 = $08, $3D = 0, $41 = 0, $2B = slot*16, motor on, and enters its
    // read entry.
    void boot(bool magnetZeroOn)
    {
        halfTrack = 0;
        magnet = {};
        magnet[0] = magnetZeroOn;
        motor = true;
        mem[0x26] = 0; mem[0x27] = 0x08; mem[0x3D] = 0; mem[0x41] = 0;
        mem[0x2B] = s16();
        x = s16();
        pc = readEntry();
        run();
    }

    void run()
    {
        for (long steps = 0; steps < 5000000 && stop.empty(); ++steps) {
            if (entry && pc == entry) { stop = "entry"; return; }
            if (pc == readEntry()) {
                if (x != s16()) problems.push_back("X is not slot*16 at the PROM read entry");
                promRead();
                continue;
            }
            // Monitor routines. WAIT keeps X and Y (they survive it in every
            // Monitor dump); the others clobber all three registers so the
            // code under test cannot rely on what they keep.
            switch (pc) {
                case 0xFCA8: ++waits; a = 0; c = true; z = true; n = false; rts(); continue;
                case 0xFDED: screen.push_back(char(a & 0x7F)); calls.push_back("COUT");
                             a = 0x5A; x = 0x5B; y = 0x5C; rts(); continue;
                case 0xFE89: calls.push_back("SETKBD"); a = 0; x = 0x38; y = 0x1B; rts(); continue;
                case 0xFE93: calls.push_back("SETVID"); a = 0; x = 0x36; y = 0xF0; rts(); continue;
                case 0xFB2F: calls.push_back("INIT"); a = 0x17; x = 0x42; y = 0x43; rts(); continue;
                case 0xFC58: calls.push_back("HOME"); a = 0xA0; x = 0x44; y = 0x45; rts(); continue;
                case 0xFF69: stop = "monitor"; return;
                default: break;
            }
            if (pc < 0x0801 || pc > 0x08FF) {
                problems.push_back("PC left the sector at " + hex(pc));
                stop = "fault";
                return;
            }
            step();
        }
        if (stop.empty()) { problems.push_back("did not finish"); stop = "fault"; }
    }

    void step()
    {
        const uint16_t at0 = pc;
        const uint8_t op = mem[pc++];
        switch (op) {
            case 0xA9: a = mem[pc++]; nz(a); break;
            case 0xA5: a = read(mem[pc++]); nz(a); break;
            case 0xAD: a = read(fetch16()); nz(a); break;
            case 0xBD: a = read(uint16_t(fetch16() + x)); nz(a); break;
            case 0xB9: a = read(uint16_t(fetch16() + y)); nz(a); break;
            case 0xA6: x = read(mem[pc++]); nz(x); break;
            case 0xAE: x = read(fetch16()); nz(x); break;
            case 0xA0: y = mem[pc++]; nz(y); break;
            case 0xAC: y = read(fetch16()); nz(y); break;
            case 0x85: write(mem[pc++], a); break;
            case 0x8D: write(fetch16(), a); break;
            case 0xEE: { const uint16_t w = fetch16(); const uint8_t v = uint8_t(read(w) + 1); write(w, v); nz(v); break; }
            case 0xCE: { const uint16_t w = fetch16(); const uint8_t v = uint8_t(read(w) - 1); write(w, v); nz(v); break; }
            case 0x29: a &= mem[pc++]; nz(a); break;
            case 0x09: a |= mem[pc++]; nz(a); break;
            case 0x05: a |= read(mem[pc++]); nz(a); break;
            case 0x69: { const unsigned s = unsigned(a) + mem[pc++] + (c ? 1u : 0u); c = s > 0xFF; a = uint8_t(s); nz(a); break; }
            case 0x18: c = false; break;
            case 0x4A: c = (a & 1) != 0; a = uint8_t(a >> 1); nz(a); break;
            case 0x0A: c = (a & 0x80) != 0; a = uint8_t(a << 1); nz(a); break;
            case 0xAA: x = a; nz(x); break;
            case 0x88: --y; nz(y); break;
            case 0xF0: case 0xD0: {
                const int8_t rel = int8_t(mem[pc++]);
                if ((op == 0xF0) == z) pc = uint16_t(pc + rel);
                break;
            }
            case 0x20: { const uint16_t w = fetch16(); const uint16_t ret = uint16_t(pc - 1);
                         push(uint8_t(ret >> 8)); push(uint8_t(ret)); pc = w; break; }
            case 0x60: rts(); break;
            case 0x4C: pc = fetch16(); break;
            case 0x6C: { const uint16_t v = fetch16();
                         pc = uint16_t(mem[v] | mem[uint16_t(v + 1)] << 8); indirect.push_back(pc); break; }
            default:
                problems.push_back("opcode " + hex(op, 2) + " at " + hex(at0) + " is not modelled");
                stop = "fault";
                break;
        }
    }
};

void report(const std::vector<std::string>& problems, const char* what)
{
    for (const auto& p : problems) std::fprintf(stderr, "  %s: %s\n", what, p.c_str());
}

std::vector<uint8_t> patterned(std::size_t n, uint32_t seed)
{
    std::vector<uint8_t> v(n);
    for (auto& b : v) { seed = seed * 1103515245u + 12345u; b = uint8_t(seed >> 16); }
    return v;
}

// ── 1. blank DOS 3.3 ───────────────────────────────────────────────────────

void testBlankDos33()
{
    const std::vector<uint8_t> img = db::blankDos33();
    CHECK(img.size() == db::kImageBytes);
    CHECK(db::kImageBytes == fs::kImageBytes);
    CHECK(db::blankDos33(0).empty());
    CHECK(db::blankDos33(255).empty());
    CHECK(db::blankDos33(1).size() == db::kImageBytes);

    fs::Dos33Disk d;
    std::string err;
    CHECK(d.open(img, fs::SectorOrder::Dos, err));
    CHECK(d.list().empty());
    CHECK(d.freeSectors() == db::kBlankDos33FreeSectors);
    CHECK(d.freeSectors() == 560 - 16 - 16);   // tracks 0 and 17 in use
    CHECK(d.volume() == 254);
    {
        fs::Dos33Disk v;
        CHECK(v.open(db::blankDos33(0x2A), fs::SectorOrder::Dos, err));
        CHECK(v.volume() == 0x2A);
    }

    // The VTOC against Beneath Apple DOS ch. 4 (format) and ch. 8 AE8E (INIT).
    const uint8_t* v = img.data() + at(17, 0);
    CHECK(v[0x00] == 0);
    CHECK(v[0x01] == 17);    // catalog track
    CHECK(v[0x02] == 15);    // catalog sector
    CHECK(v[0x03] == 3);     // DOS release
    CHECK(v[0x04] == 0 && v[0x05] == 0);
    CHECK(v[0x06] == 254);   // volume
    CHECK(v[0x27] == 122);   // T/S pairs per list
    CHECK(v[0x30] == 17);    // last track allocated
    CHECK(v[0x31] == 1);     // direction +1
    CHECK(v[0x34] == 35);
    CHECK(v[0x35] == 16);
    CHECK(v[0x36] == 0x00 && v[0x37] == 0x01);   // 256 bytes
    for (int i : { 0x07, 0x10, 0x26, 0x28, 0x2F, 0x32, 0x33 }) CHECK(v[i] == 0);
    for (int t = 0; t < 35; ++t) {
        const bool used = t == 0 || t == 17;
        CHECK(v[0x38 + 4 * t] == (used ? 0x00 : 0xFF));
        CHECK(v[0x39 + 4 * t] == (used ? 0x00 : 0xFF));
        CHECK(v[0x3A + 4 * t] == 0 && v[0x3B + 4 * t] == 0);
    }
    for (int i = 0x38 + 4 * 35; i < 0x100; ++i) CHECK(v[i] == 0);

    // The catalog chain 17/15 → 17/1, then a zero link; all entries unused.
    {
        int t = v[0x01], s = v[0x02], expect = 15, hops = 0;
        while (t != 0 && hops < 20) {
            CHECK(t == 17);
            CHECK(s == expect);
            const uint8_t* c = img.data() + at(t, s);
            for (int i = 0x0B; i < 0x100; ++i) CHECK(c[i] == 0);
            CHECK(c[0x00] == 0);
            t = c[0x01];
            s = c[0x02];
            --expect;
            ++hops;
        }
        CHECK(hops == 15);
        CHECK(t == 0 && s == 0);
    }

    // Track 0 sector 0 is the not-bootable sector; everything else outside
    // track 17 is zero.
    const std::vector<uint8_t> boot = db::notBootableSector();
    CHECK(std::equal(boot.begin(), boot.end(), img.begin()));
    for (std::size_t i = kSec; i < at(17, 0); ++i) if (img[i]) { CHECK(img[i] == 0); break; }
    for (std::size_t i = at(18, 0); i < img.size(); ++i) if (img[i]) { CHECK(img[i] == 0); break; }

    // Files through POM2's own DOS 3.3 code.
    const std::vector<uint8_t> text = fs::Dos33Disk::textToDos(
        std::vector<uint8_t>{ 'H', 'E', 'L', 'L', 'O', '\n', 'W', 'O', 'R', 'L', 'D', '\n' });
    const std::vector<uint8_t> bin = patterned(1000, 7);
    CHECK(d.writeFile("README", fs::FileType::Text, text, 0, err));
    CHECK(d.writeFile("DATA", fs::FileType::Binary, bin, 0x6000, err));
    CHECK(d.list().size() == 2);
    CHECK(d.freeSectors() == 528 - 2 - 5);   // 1 data + 1 list; 4 data + 1 list
    {
        std::vector<uint8_t> raw;
        CHECK(d.readFile("README", raw, err));
        CHECK(fs::Dos33Disk::payload(0x00, raw) == text);
        uint16_t addr = 0;
        CHECK(d.readFile("DATA", raw, err));
        CHECK(fs::Dos33Disk::payload(0x04, raw, &addr) == bin);
        CHECK(addr == 0x6000);
    }
    CHECK(d.removeFile("README", err));
    CHECK(d.removeFile("DATA", err));
    CHECK(d.freeSectors() == 528);

    // Fill it: a text file of N bytes takes ceil((N+1)/256) data sectors plus
    // one T/S list per 122 of them; 523 + 5 = 528 is exactly the disk.
    {
        fs::Dos33Disk f;
        CHECK(f.open(img, fs::SectorOrder::Dos, err));
        const std::vector<uint8_t> tooBig(523 * 256, 0xC1);   // 524 data + 5 lists
        CHECK(!f.writeFile("BIG", fs::FileType::Text, tooBig, 0, err));
        CHECK(f.freeSectors() == 528);
        const std::vector<uint8_t> fits(523 * 256 - 1, 0xC1);
        CHECK(f.writeFile("BIG", fs::FileType::Text, fits, 0, err));
        CHECK(f.freeSectors() == 0);
        const auto files = f.list();
        CHECK(files.size() == 1 && files[0].sectors == 528);
        CHECK(!f.writeFile("ONE", fs::FileType::Text, std::vector<uint8_t>{ 0xC1 }, 0, err));
        std::vector<uint8_t> raw;
        CHECK(f.readFile("BIG", raw, err));
        CHECK(fs::Dos33Disk::payload(0x00, raw) == fits);
        // Tracks 1 and 2 held data; track 0 did not.
        const uint8_t* fv = f.image().data() + at(17, 0);
        for (int t : { 1, 2 }) CHECK(fv[0x38 + 4 * t] == 0 && fv[0x39 + 4 * t] == 0);
        CHECK(std::equal(boot.begin(), boot.end(), f.image().begin()));
        CHECK(f.removeFile("BIG", err));
        CHECK(f.freeSectors() == 528);
    }
}

// ── 2. the not-bootable sector ─────────────────────────────────────────────

WalkRules notBootableRules()
{
    WalkRules r;
    r.codeEnd  = 0x0825;
    r.romCalls = { 0xFE89, 0xFE93, 0xFB2F, 0xFC58, 0xFDED };
    r.romJumps = { 0xFF69 };
    r.zeroPage = { 0x2B };
    r.slotIo   = { 0xC088 };
    return r;
}

void testNotBootable()
{
    const std::vector<uint8_t> s = db::notBootableSector();
    CHECK(s.size() == 256);
    CHECK(s[0] == 1);

    // The message, high-bit ASCII, $00-terminated, inside the sector.
    std::string want = db::kNotStartupMessage;
    std::vector<uint8_t> hi;
    for (char ch : want) hi.push_back(uint8_t(uint8_t(ch) | 0x80));
    hi.push_back(0);
    const auto it = std::search(s.begin(), s.end(), hi.begin(), hi.end());
    CHECK(it != s.end());
    CHECK(it - s.begin() == 0x26);

    const auto problems = walk(s, notBootableRules());
    CHECK(problems.empty());
    report(problems, "not-bootable walk");

    // Run it from a blank DOS 3.3 disk, in slots 6 and 5, with either
    // recalibrate end state.
    for (int slot : { 6, 5 }) {
        for (bool magnet0 : { true, false }) {
            Machine m;
            m.image = db::blankDos33();
            m.slot = slot;
            m.boot(magnet0);
            CHECK(m.stop == "monitor");
            CHECK(m.problems.empty());
            report(m.problems, "not-bootable run");
            CHECK(m.screen == want);
            CHECK(!m.motor);
            CHECK(m.reads.size() == 1);
            CHECK(m.calls.size() >= 4 && m.calls[0] == "SETKBD" && m.calls[1] == "SETVID"
                  && m.calls[2] == "INIT" && m.calls[3] == "HOME");
        }
    }

    // In block 0 of a ProDOS-ordered image: the first half of block 0 is
    // physical sector 0, the PROM's boot sector; block 0 = DOS sectors 0 and 14.
    std::vector<uint8_t> dsk(db::kImageBytes, 0);
    std::copy(s.begin(), s.end(), dsk.begin());
    std::fill_n(dsk.begin() + std::ptrdiff_t(at(0, 14)), kSec, uint8_t(0x5A));
    const auto po = db::dosOrderToProdosOrder(dsk);
    CHECK(std::equal(s.begin(), s.end(), po.begin()));
    CHECK(po[256] == 0x5A && po[511] == 0x5A);
}

// ── 3./4. direct boot ──────────────────────────────────────────────────────

WalkRules loaderRules(uint16_t entry)
{
    WalkRules r;
    r.codeEnd         = 0x0880;
    r.romCalls        = { 0xFCA8 };
    r.romJumps        = { entry };
    r.indirectVectors = { 0x003E };
    r.zeroPage        = { 0x26, 0x27, 0x2B, 0x3D, 0x3E, 0x3F, 0x41 };
    r.slotIo          = { 0xC080, 0xC081, 0xC088 };
    return r;
}

const std::array<int, 16> kReadOrder = { 0, 2, 4, 6, 8, 10, 12, 14, 1, 3, 5, 7, 9, 11, 13, 15 };

// The buffer offset where page k of a direct-boot payload must sit.
std::size_t payloadSector(std::size_t k)
{
    const int track = 1 + int(k / 16);
    return at(track, db::dosFileIndexForPhysicalSector(kReadOrder[k % 16]));
}

void testDirectBootStatic()
{
    // Capacity and sectors (Casso DirectBootTests @ 2d899adf, ported).
    CHECK(db::directBootCapacity(0x0900) == 0xC000 - 0x0900);   // 46 848
    CHECK(db::directBootCapacity(0x0900) / 256 == 183);
    CHECK(db::directBootCapacity(0x6000) == 0x6000);
    CHECK(db::directBootCapacity(0xBF00) == 256);
    CHECK(db::directBootCapacity(0xBFFF) == 1);
    for (uint16_t bad : { 0x0000, 0x0300, 0x0800, 0x08FF, 0xC000, 0xFFFF }) CHECK(db::directBootCapacity(bad) == 0);
    CHECK(db::directBootSectors(0x6080, 1) == 1);
    CHECK(db::directBootSectors(0x6080, 128) == 1);
    CHECK(db::directBootSectors(0x6080, 129) == 2);
    CHECK(db::directBootSectors(0x0900, 46848) == 183);
    CHECK(db::directBootSectors(0x6000, 0) == 0);

    // The read order: a permutation of 0..15 built with step 2.
    const auto order = db::directBootReadOrder();
    for (int i = 0; i < 16; ++i) CHECK(order[std::size_t(i)] == kReadOrder[std::size_t(i)]);
    {
        std::set<int> u(order.begin(), order.end());
        CHECK(u.size() == 16 && *u.begin() == 0 && *u.rbegin() == 15);
        CHECK(order[1] - order[0] == db::kDirectBootReadStep);
    }

    std::string refusal;
    const std::vector<uint8_t> sentinel(3, 0x5A);

    // Refusals: one reason each, the right one, the buffer untouched.
    {
        std::vector<uint8_t> out = sentinel;
        CHECK(!db::directBoot({}, db::DirectBootSpec{}, out, refusal));
        CHECK(refusal == "the payload is empty: there is nothing to boot");
        CHECK(out == sentinel);
    }
    {
        std::vector<uint8_t> out = sentinel;
        db::DirectBootSpec spec;
        spec.loadAddress = spec.entryAddress = 0x0300;
        CHECK(!db::directBoot(std::vector<uint8_t>(46849, 0xEA), spec, out, refusal));
        CHECK(refusal == "a direct-boot payload must load between $0900 and $BFFF (page $08 holds the "
                         "loader, $C000 up is not RAM); $0300 was requested");
        CHECK(refusal.find('\n') == std::string::npos);
        CHECK(refusal.find("bytes") == std::string::npos);   // the address, not the size
        CHECK(out == sentinel);
        spec.loadAddress = spec.entryAddress = 0xC000;
        CHECK(!db::directBoot(std::vector<uint8_t>(1, 0xEA), spec, out, refusal));
        CHECK(refusal.find("$C000 was requested") != std::string::npos);
        spec.loadAddress = spec.entryAddress = 0x08FF;
        CHECK(!db::directBoot(std::vector<uint8_t>(1, 0xEA), spec, out, refusal));
        CHECK(refusal.find("$08FF was requested") != std::string::npos);
    }
    {
        std::vector<uint8_t> out = sentinel;
        db::DirectBootSpec spec;
        CHECK(!db::directBoot(std::vector<uint8_t>(46849, 0xEA), spec, out, refusal));
        CHECK(refusal == "the payload is 46849 bytes; a direct-boot disk loading at $0900 can carry "
                         "46848 bytes (183 sectors)");
        CHECK(out == sentinel);
        spec.loadAddress = spec.entryAddress = 0x6000;
        CHECK(!db::directBoot(std::vector<uint8_t>(0x6001, 0xEA), spec, out, refusal));
        CHECK(refusal.find("can carry 24576 bytes (96 sectors)") != std::string::npos);
    }
    {
        std::vector<uint8_t> out = sentinel;
        db::DirectBootSpec spec;
        spec.loadAddress = 0x6000;
        spec.entryAddress = 0x6100;
        CHECK(!db::directBoot(std::vector<uint8_t>(256, 0xEA), spec, out, refusal));
        CHECK(refusal == "the entry address $6100 is outside the payload, which occupies $6000-$60FF");
        spec.entryAddress = 0x5FFF;
        CHECK(!db::directBoot(std::vector<uint8_t>(256, 0xEA), spec, out, refusal));
        CHECK(refusal.find("$5FFF") != std::string::npos);
        CHECK(out == sentinel);
        spec.entryAddress = 0x60FF;   // the last byte is fine
        CHECK(db::directBoot(std::vector<uint8_t>(256, 0xEA), spec, out, refusal));
        CHECK(refusal.empty());
    }

    // The largest payload, and the smallest at the top.
    {
        std::vector<uint8_t> out;
        db::DirectBootSpec spec;
        CHECK(db::directBoot(std::vector<uint8_t>(46848, 0xEA), spec, out, refusal));
        CHECK(out.size() == db::kImageBytes);
        CHECK(out[db::kLoaderSectorsLeft] == 183);
        spec.loadAddress = spec.entryAddress = 0xBFFF;
        CHECK(db::directBoot(std::vector<uint8_t>(1, 0x60), spec, out, refusal));
        CHECK(out[db::kLoaderSectorsLeft] == 1 && out[db::kLoaderLoadPage] == 0xBF);
        CHECK(out[payloadSector(0) + 0xFF] == 0x60);
    }

    // The loader's patched bytes.
    {
        std::vector<uint8_t> out;
        db::DirectBootSpec spec;
        spec.loadAddress = 0x6000;
        spec.entryAddress = 0x6123;
        CHECK(db::directBoot(patterned(3 * 256, 3), spec, out, refusal));
        CHECK(out[0] == 1);
        CHECK(out[db::kLoaderSectorsLeft] == 3);
        CHECK(out[db::kLoaderLoadPage] == 0x60);
        CHECK(out[db::kLoaderTrack] == 0 && out[db::kLoaderPhase] == 0 && out[db::kLoaderIndex] == 0);
        CHECK(out[0x53] == 0x4C);   // JMP
        CHECK(out[db::kLoaderEntryLo] == 0x23 && out[db::kLoaderEntryHi] == 0x61);
        for (int i = 0; i < 16; ++i) CHECK(out[db::kLoaderReadOrder + std::size_t(i)] == kReadOrder[std::size_t(i)]);
        CHECK(out[0x4B] == 0x6C && out[0x4C] == 0x3E && out[0x4D] == 0x00);   // JMP ($003E)
        for (std::size_t i = db::kLoaderCodeBytes; i < db::kLoaderReadOrder; ++i) CHECK(out[i] == 0);
        for (std::size_t i = db::kLoaderIndex + 1; i < 256; ++i) CHECK(out[i] == 0);
        // The loader is the same code whatever the spec: only the patch
        // bytes and the order/variables differ.
        std::vector<uint8_t> other;
        db::DirectBootSpec spec2;
        CHECK(db::directBoot(std::vector<uint8_t>(17 * 256, 1), spec2, other, refusal));
        for (std::size_t i = 0; i < db::kLoaderCodeBytes; ++i)
            if (i != db::kLoaderEntryLo && i != db::kLoaderEntryHi) CHECK(out[i] == other[i]);
    }

    // Placement: page k at track 1 + k/16, physical order[k%16], i.e. DOS index
    // dosFileIndexForPhysicalSector(order[k%16]); and nothing anywhere else.
    {
        const std::size_t pages = 40;
        std::vector<uint8_t> payload(pages * 256);
        for (std::size_t k = 0; k < pages; ++k)
            for (std::size_t i = 0; i < 256; ++i) payload[k * 256 + i] = uint8_t(k * 5 + i + 1) | 1;   // never 0
        std::vector<uint8_t> out;
        db::DirectBootSpec spec;
        spec.loadAddress = spec.entryAddress = 0x6000;
        CHECK(db::directBoot(payload, spec, out, refusal));
        std::set<std::size_t> used{ 0 };
        int displaced = 0;
        for (std::size_t k = 0; k < pages; ++k) {
            const std::size_t o = payloadSector(k);
            CHECK(std::equal(payload.begin() + std::ptrdiff_t(k * 256), payload.begin() + std::ptrdiff_t(k * 256 + 256),
                             out.begin() + std::ptrdiff_t(o)));
            if (o != at(1 + int(k / 16), int(k % 16))) ++displaced;
            used.insert(o);
        }
        CHECK(displaced > 0);
        CHECK(used.size() == pages + 1);
        for (std::size_t o = 0; o < out.size(); o += 256) {
            if (used.count(o)) continue;
            const bool zero = std::all_of(out.begin() + std::ptrdiff_t(o), out.begin() + std::ptrdiff_t(o + 256),
                                          [](uint8_t b) { return b == 0; });
            CHECK(zero);
        }
        // No file system: POM2's DOS 3.3 reader finds no VTOC.
        fs::Dos33Disk d;
        std::string err;
        CHECK(!d.open(out, fs::SectorOrder::Dos, err));
    }

    // A load address that is not page-aligned carries a zero lead-in.
    {
        std::vector<uint8_t> out;
        db::DirectBootSpec spec;
        spec.loadAddress = spec.entryAddress = 0x6080;
        CHECK(db::directBoot(std::vector<uint8_t>(1, 0xAA), spec, out, refusal));
        const std::size_t o = payloadSector(0);
        for (std::size_t i = 0; i < 0x80; ++i) CHECK(out[o + i] == 0);
        CHECK(out[o + 0x80] == 0xAA);
        CHECK(out[db::kLoaderLoadPage] == 0x60);
    }
}

void testDirectBootRuns()
{
    struct Case { uint16_t load; std::size_t bytes; uint16_t entry; };
    const Case cases[] = {
        { 0x0900, 46848, 0x0900 },   // the most there is: 183 sectors, 12 tracks
        { 0x6000, 20 * 256, 0x6123 },
        { 0x6080, 300, 0x6085 },
        { 0xBFFF, 1, 0xBFFF },
        { 0x0A00, 16 * 256, 0x0A00 },   // exactly one track
    };
    for (const Case& cs : cases) {
        const std::vector<uint8_t> payload = patterned(cs.bytes, cs.load);
        db::DirectBootSpec spec;
        spec.loadAddress = cs.load;
        spec.entryAddress = cs.entry;
        std::vector<uint8_t> img;
        std::string refusal;
        CHECK(db::directBoot(payload, spec, img, refusal));
        if (img.empty()) continue;

        const std::vector<uint8_t> loader(img.begin(), img.begin() + 256);
        const auto problems = walk(loader, loaderRules(cs.entry));
        CHECK(problems.empty());
        report(problems, "loader walk");

        const std::size_t count = db::directBootSectors(cs.load, cs.bytes);
        const int lastTrack = 1 + int((count - 1) / 16);
        for (int slot : { 6, 5 }) {
            for (bool magnet0 : { true, false }) {
                Machine m;
                m.image = img;
                m.slot = slot;
                m.entry = cs.entry;
                m.boot(magnet0);
                CHECK(m.stop == "entry");
                CHECK(m.pc == cs.entry);
                CHECK(m.problems.empty());
                report(m.problems, "loader run");
                CHECK(!m.motor);
                CHECK(m.halfTrack == 2 * lastTrack);
                CHECK(m.waits == 2 * lastTrack);
                CHECK(std::equal(payload.begin(), payload.end(), m.mem.begin() + cs.load));
                const uint16_t page = uint16_t(cs.load & 0xFF00);
                for (uint16_t a = page; a < cs.load; ++a) CHECK(m.mem[a] == 0);   // the lead-in
                // Nothing but the payload's pages, page $08, zero page and the
                // stack changed: $0200-$07FF and the rest of $0900-$BFFF still
                // hold the power-up pattern.
                const std::size_t end = std::size_t(page) + count * 256;
                bool untouched = true;
                for (std::size_t a = 0x0200; a < 0xC000; ++a) {
                    if ((a >= 0x0800 && a < 0x0900) || (a >= page && a < end)) continue;
                    if (m.mem[a] != 0xEE) { untouched = false; break; }
                }
                CHECK(untouched);
                // The reads: the boot sector, then page k at (1 + k/16, order[k%16]).
                CHECK(m.reads.size() == count + 1);
                if (m.reads.size() == count + 1) {
                    CHECK(m.reads[0] == std::make_pair(0, 0));
                    for (std::size_t k = 0; k < count; ++k)
                        CHECK(m.reads[k + 1] == std::make_pair(1 + int(k / 16), kReadOrder[k % 16]));
                }
                // The vector JMP ($3E) went through was always $Cs5C.
                CHECK(m.indirect.size() == count);
                for (uint16_t t : m.indirect) CHECK(t == uint16_t(0xC05C | slot << 8));
            }
        }
    }
}

// ── 5. orders ──────────────────────────────────────────────────────────────

void testOrders()
{
    for (int p = 0; p < 16; ++p) {
        CHECK(db::dosFileIndexForPhysicalSector(p) == kBadPhysToDos33[p]);
        CHECK(db::prodosFileIndexForPhysicalSector(p) == kBadPhysToPascal[p]);
        CHECK(db::dosFileIndexForPhysicalSector(p) == kDiskImageDos[p]);
        CHECK(db::prodosFileIndexForPhysicalSector(p) == kDiskImageProdos[p]);
        // Dos33DiskFs: DOS sector d sits at kDosToProdos[d] in a .po track.
        CHECK(kDos33FsDosToProdos[db::dosFileIndexForPhysicalSector(p)] == db::prodosFileIndexForPhysicalSector(p));
    }
    // [B] Beneath Apple DOS p. 3-22: logical 2 is physical 11 ($0B).
    CHECK(db::dosFileIndexForPhysicalSector(11) == 2);
    CHECK(db::dosFileIndexForPhysicalSector(2) == 14);
    CHECK(db::dosFileIndexForPhysicalSector(-1) == -1 && db::dosFileIndexForPhysicalSector(16) == -1);
    CHECK(db::prodosFileIndexForPhysicalSector(16) == -1);
    // [B] Beneath Apple ProDOS: block k of a track = these physical sectors.
    for (int k = 0; k < 8; ++k)
        for (int h = 0; h < 2; ++h)
            CHECK(db::prodosFileIndexForPhysicalSector(kBapBlockPhysical[k][h]) == 2 * k + h);

    // Conversions: tag every DOS sector with (track, index).
    std::vector<uint8_t> dsk(db::kImageBytes);
    for (int t = 0; t < 35; ++t)
        for (int s = 0; s < 16; ++s)
            for (std::size_t i = 0; i < 256; ++i) dsk[at(t, s) + i] = i & 1 ? uint8_t(t) : uint8_t(s);
    const auto po = db::dosOrderToProdosOrder(dsk);
    CHECK(po.size() == db::kImageBytes);
    for (int t = 0; t < 35; ++t)
        for (int p = 0; p < 16; ++p) {
            const std::size_t o = at(t, db::prodosFileIndexForPhysicalSector(p));
            CHECK(po[o] == db::dosFileIndexForPhysicalSector(p) && po[o + 1] == t);
        }
    // ProDOS block 0 = DOS sectors 0 and 14 of track 0.
    CHECK(po[0] == 0 && po[256] == 14);
    CHECK(db::prodosOrderToDosOrder(po) == dsk);
    const auto noise = patterned(db::kImageBytes, 99);
    CHECK(db::dosOrderToProdosOrder(db::prodosOrderToDosOrder(noise)) == noise);
    CHECK(db::prodosOrderToDosOrder(db::dosOrderToProdosOrder(noise)) == noise);
    CHECK(db::dosOrderToProdosOrder(std::vector<uint8_t>(100)).empty());
    CHECK(db::prodosOrderToDosOrder(std::vector<uint8_t>(db::kImageBytes + 1)).empty());

    // The blank disk read through Dos33DiskFs in ProDOS order: same disk.
    fs::Dos33Disk d;
    std::string err;
    CHECK(d.open(db::dosOrderToProdosOrder(db::blankDos33(77)), fs::SectorOrder::Prodos, err));
    CHECK(d.volume() == 77);
    CHECK(d.freeSectors() == 528);
    CHECK(d.list().empty());
    const auto data = patterned(5000, 5);
    CHECK(d.writeFile("X", fs::FileType::Binary, data, 0x2000, err));
    fs::Dos33Disk back;
    CHECK(back.open(db::prodosOrderToDosOrder(d.image()), fs::SectorOrder::Dos, err));
    std::vector<uint8_t> raw;
    CHECK(back.readFile("X", raw, err));
    CHECK(fs::Dos33Disk::payload(0x04, raw) == data);
}

} // namespace

int main()
{
    testBlankDos33();
    testNotBootable();
    testDirectBootStatic();
    testDirectBootRuns();
    testOrders();
    if (g_failures) {
        std::fprintf(stderr, "disk_builder: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("disk_builder: OK\n");
    return 0;
}
