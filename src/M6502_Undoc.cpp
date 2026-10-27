// POM2 — GPL-3.0-or-later
//
// The undocumented NMOS 6502 opcodes, with their results.
//
// They used to be length- and cycle-correct NOPs that never computed
// anything (and whose writes never reached the bus) — but the //e
// Unenhanced PAL profile is the French Touch corpus machine, and that code
// uses LAX / SAX / SLO for real. Semantics and bus cycles follow MAME
// `src/devices/cpu/m6502/om6502.lst` (slo_*, rla_*, sre_*, rra_*, sax_*,
// lax_*, dcp_*, isb_*, anc_imm, asr_imm, arr_imm, ane_imm, lxa_imm,
// sbx_imm, sbc_imm, sha_*, shx_*, shy_*, shs_*, las_*) and are pinned against
// the Tom Harte SingleStepTests `6502/v1` vectors (tomharte_cpu_test).
//
// The two "unstable" families are modelled the way those vectors (and
// Visual6502) describe a typical chip:
//   * ANE ($8B) / LXA ($AB): A is OR'd with a magic constant ($EE) first.
//   * SHA / SHX / SHY / TAS: the stored value is ANDed with (high byte of
//     the base address + 1); on a page cross the address's high byte is
//     replaced by that value.
//
// Cycle totals are the ones the placeholders already carried (Tom Harte);
// `cycles` is seeded with 1 for the opcode fetch. Each handler advances it to
// the data access's bus cycle, exactly as the documented addressing mode
// does (Abs → 3, ZeroX → 3, IndZeroX → 5, …), and adds the rest after, so a
// device reading the in-flight cycle sees `LAX $C404` at the instant
// `LDA $C404` would be (an RMW, like ASL, puts its read, write-back and write
// on three successive cycles). Adding the whole cost first used to put every undocumented access
// one cycle late, and the RMW ones three (bug hunt 2026-10-27).

#include "M6502.h"
#include "Memory.h"

namespace {
enum Kind : int {
    kSLO, kRLA, kSRE, kRRA, kSAX, kLAX, kDCP, kISC,   // the $x3/$x7/$xF rows
    kSHA, kSHX, kSHY, kTAS, kLAS,
    kANC, kALR, kARR, kANE, kLXA, kSBX, kUSBC         // the #imm column
};
constexpr bool isRmw(int k)   { return k == kSLO || k == kRLA || k == kSRE || k == kRRA ||
                                       k == kDCP || k == kISC; }
constexpr bool isStore(int k) { return k == kSAX || k == kSHA || k == kSHX || k == kSHY ||
                                       k == kTAS; }
constexpr uint8_t kAneMagic = 0xEE;
} // namespace

void M6502::undocAt(int kind, uint16_t ea, uint16_t base, bool crossed)
{
    const uint8_t C = statusRegister & Status::C;
    if (isRmw(kind)) {
        const uint8_t v = memory->memRead(ea);
        ++cycles;                             // read k, write-back k+1, write k+2
        rmwSecondBusCycle(ea, v);             // NMOS: the original byte, written back
        ++cycles;
        uint8_t r = v;
        switch (kind) {
        case kSLO: statusRegister = (statusRegister & ~Status::C) | (v >> 7);
                   r = static_cast<uint8_t>(v << 1); break;
        case kRLA: statusRegister = (statusRegister & ~Status::C) | (v >> 7);
                   r = static_cast<uint8_t>((v << 1) | C); break;
        case kSRE: statusRegister = (statusRegister & ~Status::C) | (v & 1);
                   r = static_cast<uint8_t>(v >> 1); break;
        case kRRA: statusRegister = (statusRegister & ~Status::C) | (v & 1);
                   r = static_cast<uint8_t>((v >> 1) | (C << 7)); break;
        case kDCP: r = static_cast<uint8_t>(v - 1); break;
        case kISC: r = static_cast<uint8_t>(v + 1); break;
        }
        memory->memWrite(ea, r);
        switch (kind) {
        case kSLO: accumulator |= r; setStatusRegisterNZ(accumulator); break;
        case kRLA: accumulator &= r; setStatusRegisterNZ(accumulator); break;
        case kSRE: accumulator ^= r; setStatusRegisterNZ(accumulator); break;
        case kRRA: adcValue(r); break;
        case kDCP:
            if (accumulator >= r) statusRegister |= Status::C;
            else                  statusRegister &= ~Status::C;
            setStatusRegisterNZ(static_cast<uint8_t>(accumulator - r));
            break;
        case kISC: sbcValue(r); break;
        }
        return;
    }
    if (isStore(kind)) {
        const uint8_t h1 = static_cast<uint8_t>((base >> 8) + 1);
        uint8_t v = 0;
        switch (kind) {
        case kSAX: memory->memWrite(ea, accumulator & xRegister); return;
        case kSHA: v = accumulator & xRegister & h1; break;
        case kSHX: v = xRegister & h1; break;
        case kSHY: v = yRegister & h1; break;
        case kTAS: stackPointer = accumulator & xRegister; v = stackPointer & h1; break;
        }
        if (crossed) ea = static_cast<uint16_t>((v << 8) | (ea & 0x00FF));
        memory->memWrite(ea, v);
        return;
    }
    const uint8_t v = memory->memRead(ea);
    if (kind == kLAX) {
        accumulator = xRegister = v;
    } else {                                  // kLAS
        accumulator = xRegister = stackPointer = static_cast<uint8_t>(v & stackPointer);
    }
    setStatusRegisterNZ(accumulator);
}

template <int Kind> void M6502::UndocZp()
{
    const uint16_t ea = memory->memRead(programCounter++);
    cycles += 1;                              // access at 2, as LDA/ASL zp
    undocAt(Kind, ea, ea, false);
    cycles += 1;                              // (an RMW spent two inside undocAt)
}

template <int Kind, bool Y> void M6502::UndocZpI()
{
    const uint8_t zp = memory->memRead(programCounter++);
    const uint16_t ea = static_cast<uint8_t>(zp + (Y ? yRegister : xRegister));
    cycles += 2;                              // access at 3, as ZeroX
    undocAt(Kind, ea, ea, false);
    cycles += 1;                              // (an RMW spent two inside undocAt)
}

template <int Kind> void M6502::UndocAbs()
{
    uint16_t ea = memory->memRead(programCounter++);
    ea |= static_cast<uint16_t>(memory->memRead(programCounter++)) << 8;
    cycles += 2;                              // access at 3, as Abs
    undocAt(Kind, ea, ea, false);
    cycles += 1;                              // (an RMW spent two inside undocAt)
}

template <int Kind, bool Y> void M6502::UndocAbsI()
{
    uint16_t base = memory->memRead(programCounter++);
    base |= static_cast<uint16_t>(memory->memRead(programCounter++)) << 8;
    const uint16_t ea = static_cast<uint16_t>(base + (Y ? yRegister : xRegister));
    const bool crossed = (base & 0xFF00) != (ea & 0xFF00);
    const bool reads = !isRmw(Kind) && !isStore(Kind);
    // Access at 3 (+1 crossed) for a read, 4 for a store or an RMW, as
    // AbsX / WAbsX / RmwAbsX.
    cycles += reads ? 2 + (crossed ? 1 : 0) : 3;
    nmosIndexDummyRead(base, ea, /*always=*/!reads);
    undocAt(Kind, ea, base, crossed);
    cycles += 1;                              // (an RMW spent two inside undocAt)
}

template <int Kind> void M6502::UndocIndX()
{
    const uint8_t zp = static_cast<uint8_t>(memory->memRead(programCounter++) + xRegister);
    uint16_t ea = memory->memRead(zp);
    ea |= static_cast<uint16_t>(memory->memRead(static_cast<uint8_t>(zp + 1))) << 8;
    cycles += 4;                              // access at 5, as IndZeroX
    undocAt(Kind, ea, ea, false);
    cycles += 1;                              // (an RMW spent two inside undocAt)
}

template <int Kind> void M6502::UndocIndY()
{
    const uint8_t zp = memory->memRead(programCounter++);
    uint16_t base = memory->memRead(zp);
    base |= static_cast<uint16_t>(memory->memRead(static_cast<uint8_t>(zp + 1))) << 8;
    const uint16_t ea = static_cast<uint16_t>(base + yRegister);
    const bool crossed = (base & 0xFF00) != (ea & 0xFF00);
    const bool reads = !isRmw(Kind) && !isStore(Kind);
    // Access at 4 (+1 crossed) for a read, 5 for a store or an RMW.
    cycles += reads ? 3 + (crossed ? 1 : 0) : 4;
    nmosIndexDummyRead(base, ea, /*always=*/!reads);
    undocAt(Kind, ea, base, crossed);
    cycles += 1;                              // (an RMW spent two inside undocAt)
}

template <int Kind> void M6502::UndocImm()
{
    const uint8_t v = memory->memRead(programCounter++);
    cycles += 1;
    switch (Kind) {
    case kANC:
        accumulator &= v;
        setStatusRegisterNZ(accumulator);
        statusRegister = (statusRegister & ~Status::C) | (accumulator >> 7);
        break;
    case kALR:
        accumulator &= v;
        statusRegister = (statusRegister & ~Status::C) | (accumulator & 1);
        accumulator >>= 1;
        setStatusRegisterNZ(accumulator);
        break;
    case kARR: {
        // MAME `arr_imm` / do_arr: the AND, then ROR through C; in decimal
        // mode the result is BCD-fixed and C/V come from the AND.
        const uint8_t t = accumulator & v;
        const uint8_t c = statusRegister & Status::C;
        accumulator = static_cast<uint8_t>((t >> 1) | (c << 7));
        if (!(statusRegister & Status::D)) {
            setStatusRegisterNZ(accumulator);
            statusRegister = (statusRegister & ~(Status::C | Status::V)) |
                             ((accumulator & 0x40) ? Status::C : 0) |
                             (((accumulator ^ (accumulator << 1)) & 0x40) ? Status::V : 0);
        } else {
            setStatusRegisterNZ(accumulator);
            statusRegister = (statusRegister & ~Status::V) |
                             (((t ^ accumulator) & 0x40) ? Status::V : 0);
            if ((t & 0x0F) + (t & 0x01) > 5)
                accumulator = static_cast<uint8_t>((accumulator & 0xF0) | ((accumulator + 6) & 0x0F));
            if ((t & 0xF0) + (t & 0x10) > 0x50) {
                accumulator = static_cast<uint8_t>(accumulator + 0x60);
                statusRegister |= Status::C;
            } else {
                statusRegister &= ~Status::C;
            }
        }
        break;
    }
    case kANE:
        accumulator = static_cast<uint8_t>((accumulator | kAneMagic) & xRegister & v);
        setStatusRegisterNZ(accumulator);
        break;
    case kLXA:
        accumulator = xRegister = static_cast<uint8_t>((accumulator | kAneMagic) & v);
        setStatusRegisterNZ(accumulator);
        break;
    case kSBX: {
        const uint8_t t = accumulator & xRegister;
        if (t >= v) statusRegister |= Status::C; else statusRegister &= ~Status::C;
        xRegister = static_cast<uint8_t>(t - v);
        setStatusRegisterNZ(xRegister);
        break;
    }
    case kUSBC:
        sbcValue(v);
        break;
    }
}

void M6502::installNmosUndocumented()
{
    auto set = [this](int opc, void (M6502::*fn)()) { opcodeTable[opc] = OpcodeEntry{fn, nullptr}; };
    // The eight-row families, opcode = row << 5 | column. Row 4 is SAX, its
    // indexed-by-Y forms SHA; row 5 LAX / LAS. Each row's column set is the
    // same shape, so it is spelled once per Kind.
#define POM2_UNDOC_ROW(row, K)                                         \
    set((row << 5) | 0x03, &M6502::UndocIndX<K>);                       \
    set((row << 5) | 0x07, &M6502::UndocZp<K>);                         \
    set((row << 5) | 0x0F, &M6502::UndocAbs<K>);
    POM2_UNDOC_ROW(0, kSLO) POM2_UNDOC_ROW(1, kRLA) POM2_UNDOC_ROW(2, kSRE)
    POM2_UNDOC_ROW(3, kRRA) POM2_UNDOC_ROW(4, kSAX) POM2_UNDOC_ROW(5, kLAX)
    POM2_UNDOC_ROW(6, kDCP) POM2_UNDOC_ROW(7, kISC)
#undef POM2_UNDOC_ROW
#define POM2_UNDOC_RMW(row, K)                                         \
    set((row << 5) | 0x13, &M6502::UndocIndY<K>);                       \
    set((row << 5) | 0x17, &M6502::UndocZpI<K, false>);                 \
    set((row << 5) | 0x1B, &M6502::UndocAbsI<K, true>);                 \
    set((row << 5) | 0x1F, &M6502::UndocAbsI<K, false>);
    POM2_UNDOC_RMW(0, kSLO) POM2_UNDOC_RMW(1, kRLA) POM2_UNDOC_RMW(2, kSRE)
    POM2_UNDOC_RMW(3, kRRA) POM2_UNDOC_RMW(6, kDCP) POM2_UNDOC_RMW(7, kISC)
#undef POM2_UNDOC_RMW
    // Row 4 ($8x/$9x): SAX zp,Y, then the unstable stores.
    set(0x93, &M6502::UndocIndY<kSHA>);
    set(0x97, &M6502::UndocZpI<kSAX, true>);
    set(0x9B, &M6502::UndocAbsI<kTAS, true>);
    set(0x9C, &M6502::UndocAbsI<kSHY, false>);
    set(0x9E, &M6502::UndocAbsI<kSHX, true>);
    set(0x9F, &M6502::UndocAbsI<kSHA, true>);
    // Row 5 ($Ax/$Bx): LAX reads by Y where the documented loads read by X.
    set(0xB3, &M6502::UndocIndY<kLAX>);
    set(0xB7, &M6502::UndocZpI<kLAX, true>);
    set(0xBB, &M6502::UndocAbsI<kLAS, true>);
    set(0xBF, &M6502::UndocAbsI<kLAX, true>);
    // The #imm column.
    set(0x0B, &M6502::UndocImm<kANC>);
    set(0x2B, &M6502::UndocImm<kANC>);
    set(0x4B, &M6502::UndocImm<kALR>);
    set(0x6B, &M6502::UndocImm<kARR>);
    set(0x8B, &M6502::UndocImm<kANE>);
    set(0xAB, &M6502::UndocImm<kLXA>);
    set(0xCB, &M6502::UndocImm<kSBX>);
    set(0xEB, &M6502::UndocImm<kUSBC>);
}
