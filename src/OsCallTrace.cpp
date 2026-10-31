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

// OS-call trace — see OsCallTrace.h.

#include "OsCallTrace.h"

#include "M6502.h"
#include "Memory.h"

#include <cstdarg>
#include <cstdio>

namespace pom2 {

namespace {

std::string fmt(const char* f, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// ProDOS unit_num: DSSS0000.
std::string unit(uint8_t u)
{
    return fmt("S%d,D%d", (u >> 4) & 7, (u & 0x80) ? 2 : 1);
}

} // namespace

// ProDOS 8 Technical Reference Manual § 4.4 (commands), § B.2 (errors).
const char* OsCallTrace::mliName(uint8_t cmd)
{
    switch (cmd) {
        case 0x40: return "ALLOC_INTERRUPT";
        case 0x41: return "DEALLOC_INTERRUPT";
        case 0x65: return "QUIT";
        case 0x80: return "READ_BLOCK";
        case 0x81: return "WRITE_BLOCK";
        case 0x82: return "GET_TIME";
        case 0xC0: return "CREATE";
        case 0xC1: return "DESTROY";
        case 0xC2: return "RENAME";
        case 0xC3: return "SET_FILE_INFO";
        case 0xC4: return "GET_FILE_INFO";
        case 0xC5: return "ON_LINE";
        case 0xC6: return "SET_PREFIX";
        case 0xC7: return "GET_PREFIX";
        case 0xC8: return "OPEN";
        case 0xC9: return "NEWLINE";
        case 0xCA: return "READ";
        case 0xCB: return "WRITE";
        case 0xCC: return "CLOSE";
        case 0xCD: return "FLUSH";
        case 0xCE: return "SET_MARK";
        case 0xCF: return "GET_MARK";
        case 0xD0: return "SET_EOF";
        case 0xD1: return "GET_EOF";
        case 0xD2: return "SET_BUF";
        case 0xD3: return "GET_BUF";
        default:   return nullptr;
    }
}

const char* OsCallTrace::prodosError(uint8_t code)
{
    switch (code) {
        case 0x00: return "no error";
        case 0x01: return "bad call number";
        case 0x04: return "bad parameter count";
        case 0x25: return "interrupt table full";
        case 0x27: return "I/O error";
        case 0x28: return "no device connected";
        case 0x2B: return "write protected";
        case 0x2E: return "disk switched";
        case 0x40: return "invalid pathname";
        case 0x42: return "file control block table full";
        case 0x43: return "invalid reference number";
        case 0x44: return "path not found";
        case 0x45: return "volume directory not found";
        case 0x46: return "file not found";
        case 0x47: return "duplicate filename";
        case 0x48: return "volume full";
        case 0x49: return "volume directory full";
        case 0x4A: return "incompatible file format";
        case 0x4B: return "unsupported storage type";
        case 0x4C: return "end of file";
        case 0x4D: return "position out of range";
        case 0x4E: return "access error";
        case 0x50: return "file is open";
        case 0x51: return "directory structure damaged";
        case 0x52: return "not a ProDOS volume";
        case 0x53: return "invalid parameter";
        case 0x55: return "volume control block table full";
        case 0x56: return "bad buffer address";
        case 0x57: return "duplicate volume";
        case 0x5A: return "bit map disk address impossible";
        default:   return nullptr;
    }
}

// CP/M 2.2 Interface Guide § 5 (0-40) and the CP/M 3 Programmer's Guide
// § 3 (41-112), with DRI's mnemonics.
const char* OsCallTrace::bdosName(uint8_t fn)
{
    static const char* const k[] = {
        "P_TERMCPM", "C_READ", "C_WRITE", "A_READ", "A_WRITE", "L_WRITE",
        "C_RAWIO", "A_STATIN", "A_STATOUT", "C_WRITESTR", "C_READSTR",
        "C_STAT", "S_BDOSVER", "DRV_ALLRESET", "DRV_SET", "F_OPEN",
        "F_CLOSE", "F_SFIRST", "F_SNEXT", "F_DELETE", "F_READ", "F_WRITE",
        "F_MAKE", "F_RENAME", "DRV_LOGINVEC", "DRV_GET", "F_DMAOFF",
        "DRV_ALLOCVEC", "DRV_SETRO", "DRV_ROVEC", "F_ATTRIB", "DRV_DPB",
        "F_USERNUM", "F_READRAND", "F_WRITERAND", "F_SIZE", "F_RANDREC",
        "DRV_RESET", "DRV_ACCESS", "DRV_FREE", "F_WRITEZF", "F_TESTWRITE",
        "F_LOCK", "F_UNLOCK", "F_MULTISEC", "F_ERRMODE", "DRV_SPACE",
        "P_CHAIN", "DRV_FLUSH", "S_SCB", "S_BIOS",
    };
    if (fn < sizeof k / sizeof k[0]) return k[fn];
    switch (fn) {
        case 59:  return "P_LOAD";
        case 60:  return "P_RSX";
        case 98:  return "F_CLEANUP";
        case 99:  return "F_TRUNCATE";
        case 100: return "DRV_SETLABEL";
        case 101: return "DRV_GETLABEL";
        case 102: return "F_TIMEDATE";
        case 103: return "F_WRITEXFCB";
        case 104: return "T_SET";
        case 105: return "T_GET";
        case 106: return "F_PASSWD";
        case 107: return "S_SERIAL";
        case 108: return "P_CODE";
        case 109: return "C_MODE";
        case 110: return "C_DELIMIT";
        case 111: return "C_WRITEBLK";
        case 112: return "L_WRITEBLK";
        default:  return nullptr;
    }
}

uint8_t OsCallTrace::peek(uint16_t a) const
{
    return mem_ ? mem_->peekCpuView(a) : 0xFF;
}

// A length-prefixed ProDOS name.
std::string OsCallTrace::pathAt(uint16_t a) const
{
    const int len = peek(a) & 0x7F;
    std::string s;
    for (int i = 1; i <= len && i <= 64; ++i) {
        const char c = char(peek(uint16_t(a + i)) & 0x7F);
        s.push_back(c >= 0x20 && c < 0x7F ? c : '?');
    }
    return "\"" + s + "\"";
}

void OsCallTrace::emit(std::string line)
{
    if (sink_) sink_(line);
    lines_.push_back(std::move(line));
    if (lines_.size() > kCapacity) lines_.pop_front();
    ++emitted_;
}

void OsCallTrace::abandon(std::vector<Pending>& open, uint16_t sp, uint16_t mask,
                          const char* who)
{
    while (!open.empty() && (open.back().sp & mask) <= (sp & mask)) {
        emit(fmt("%s ", who) + open.back().text + " -> (no return)");
        open.pop_back();
    }
}

// ── ProDOS MLI ───────────────────────────────────────────────────────────

std::string OsCallTrace::mliCall(uint8_t cmd, uint16_t p) const
{
    const char* name = mliName(cmd);
    std::string s = name ? name : fmt("$%02X", cmd);
    auto w = [&](int o) { return uint16_t(peek(uint16_t(p + o)) | peek(uint16_t(p + o + 1)) << 8); };
    auto b = [&](int o) { return peek(uint16_t(p + o)); };
    switch (cmd) {
        case 0xC0:
            return s + fmt(" %s type=$%02X aux=$%04X storage=$%02X",
                           pathAt(w(1)).c_str(), b(4), w(5), b(7));
        case 0xC1: case 0xC3: case 0xC4: case 0xC6:
            return s + " " + pathAt(w(1));
        case 0xC2:
            return s + " " + pathAt(w(1)) + " " + pathAt(w(3));
        case 0xC8:
            return s + " " + pathAt(w(1)) + fmt(" buf=$%04X", w(3));
        case 0xC9:
            return s + fmt(" ref=%d mask=$%02X char=$%02X", b(1), b(2), b(3));
        case 0xCA: case 0xCB:
            return s + fmt(" ref=%d buf=$%04X req=%u", b(1), w(2), unsigned(w(4)));
        case 0xCC: case 0xCD:
            return s + fmt(" ref=%d", b(1));
        case 0xCE: case 0xD0:
            return s + fmt(" ref=%d pos=%u", b(1), unsigned(w(2) | b(4) << 16));
        case 0xCF: case 0xD1: case 0xD3:
            return s + fmt(" ref=%d", b(1));
        case 0xD2:
            return s + fmt(" ref=%d buf=$%04X", b(1), w(2));
        case 0xC5:
            return s + (b(1) ? " " + unit(b(1)) : std::string(" all")) + fmt(" buf=$%04X", w(2));
        case 0x80: case 0x81:
            return s + " " + unit(b(1)) + fmt(" block=%u buf=$%04X", unsigned(w(4)), w(2));
        default:
            return s + fmt(" params=$%04X", p);
    }
}

std::string OsCallTrace::mliResult(const Pending& c, uint8_t a, bool carry) const
{
    if (carry) {
        const char* e = prodosError(a);
        return fmt("ERR $%02X", a) + (e ? std::string(" ") + e : std::string());
    }
    const uint16_t p = c.arg;
    auto w = [&](int o) { return uint16_t(peek(uint16_t(p + o)) | peek(uint16_t(p + o + 1)) << 8); };
    auto b = [&](int o) { return peek(uint16_t(p + o)); };
    switch (c.fn) {
        case 0xC8: return fmt("ref=%d", b(5));
        case 0xCA: case 0xCB: return fmt("ok, %u bytes", unsigned(w(6)));
        case 0xCF: case 0xD1: return fmt("%u", unsigned(w(2) | b(4) << 16));
        case 0xC4: return fmt("type=$%02X aux=$%04X blocks=%u", b(4), w(5), unsigned(w(8)));
        case 0xC7: return pathAt(w(1));
        case 0xC5: {
            const uint16_t buf = w(2);
            const uint8_t head = peek(buf);
            std::string name;
            for (int i = 1; i <= (head & 0x0F); ++i) name.push_back(char(peek(uint16_t(buf + i)) & 0x7F));
            return name.empty() ? std::string("ok") : unit(head) + " /" + name;
        }
        default: return "ok";
    }
}

void OsCallTrace::mliStep(uint16_t pc)
{
    if (!cpu_) return;
    const uint8_t s = cpu_->getStackPointer();
    if (pc == 0xBF00) {
        // JSR pushed the address of its own last byte.
        const uint16_t jsrLast = uint16_t(peek(uint16_t(0x0101 + s)) | peek(uint16_t(0x0101 + uint8_t(s + 1))) << 8);
        const uint16_t cmdAt = uint16_t(jsrLast + 1);
        Pending c;
        c.ret = uint16_t(cmdAt + 3);
        c.sp  = uint8_t(s + 2);
        c.fn  = peek(cmdAt);
        c.arg = uint16_t(peek(uint16_t(cmdAt + 1)) | peek(uint16_t(cmdAt + 2)) << 8);
        c.text = fmt("$%04X ", uint16_t(jsrLast - 2)) + mliCall(c.fn, c.arg);
        abandon(mliPending_, c.sp, 0xFF, "MLI ");
        if (c.fn == 0x65) { emit("MLI  " + c.text); return; }   // QUIT never comes back
        if (mliPending_.size() < 16) mliPending_.push_back(std::move(c));
        return;
    }
    const Pending& c = mliPending_.back();
    if (s != c.sp) {
        if (s > c.sp) abandon(mliPending_, s, 0xFF, "MLI ");
        return;
    }
    const uint8_t p = cpu_->getStatusRegister();
    emit("MLI  " + c.text + " -> " + mliResult(c, cpu_->getAccumulator(), (p & M6502::Status::C) != 0));
    mliPending_.pop_back();
}

// ── CP/M BDOS ────────────────────────────────────────────────────────────

std::string OsCallTrace::bdosCall(const Z80& z, uint8_t fn, uint16_t de) const
{
    const char* name = bdosName(fn);
    std::string s = name ? name : fmt("#%d", fn);
    const uint8_t e = uint8_t(de);
    auto fcb = [&](uint16_t a) {
        const uint8_t dr = z.peek(a);
        std::string n = dr ? std::string(1, char('A' + ((dr - 1) & 0x0F))) + ":" : std::string();
        for (int i = 1; i <= 11; ++i) {
            if (i == 9) n.push_back('.');
            const char c = char(z.peek(uint16_t(a + i)) & 0x7F);
            if (c != ' ') n.push_back(c >= 0x20 && c < 0x7F ? c : '?');
        }
        if (!n.empty() && n.back() == '.') n.pop_back();
        return n;
    };
    switch (fn) {
        case 2: case 4: case 5:
            return s + (e >= 0x20 && e < 0x7F ? fmt(" '%c'", e) : fmt(" $%02X", e));
        case 6:
            return s + (e == 0xFF ? std::string(" in") : e == 0xFE ? std::string(" status") : fmt(" $%02X", e));
        case 9: {
            std::string t;
            for (int i = 0; i < 48; ++i) {
                const char c = char(z.peek(uint16_t(de + i)) & 0x7F);
                if (c == '$') break;
                t += (c >= 0x20 && c < 0x7F) ? std::string(1, c) : c == '\r' ? "\\r" : c == '\n' ? "\\n" : "?";
            }
            return s + " \"" + t + "\"";
        }
        case 14:
            return s + fmt(" %c:", 'A' + (e & 0x0F));
        case 15: case 16: case 17: case 19: case 20: case 21: case 22:
        case 30: case 33: case 34: case 35: case 36: case 40:
            return s + " " + fcb(de);
        case 23:
            return s + " " + fcb(de) + " " + fcb(uint16_t(de + 16));
        case 26:
            return s + fmt(" $%04X", de);
        case 32:
            return s + (e == 0xFF ? std::string(" get") : fmt(" %d", e));
        case 0: case 1: case 3: case 7: case 8: case 11: case 12: case 13:
        case 18: case 24: case 25: case 27: case 29: case 31: case 37:
            return s;
        default:
            return s + fmt(" DE=$%04X", de);
    }
}

void OsCallTrace::onZ80Instruction(const Z80& z)
{
    const uint16_t pc = z.getPC();
    const uint16_t sp = z.getSP();
    if (pc == 0x0005) {
        const uint16_t ret = uint16_t(z.peek(sp) | z.peek(uint16_t(sp + 1)) << 8);
        const uint16_t back = uint16_t(sp + 2);
        // A card that undoes and replays a stalled instruction shows us the
        // same entry twice.
        if (!bdosPending_.empty() && bdosPending_.back().sp == back && bdosPending_.back().ret == ret)
            return;
        Pending c;
        c.ret = ret;
        c.sp  = back;
        c.fn  = uint8_t(z.getBC());
        c.arg = z.getDE();
        c.text = fmt("$%04X ", uint16_t(ret - 3)) + bdosCall(z, c.fn, c.arg);
        abandon(bdosPending_, back, 0xFFFF, "BDOS");
        if (c.fn == 0) { emit("BDOS " + c.text); return; }   // warm boot
        if (bdosPending_.size() < 16) bdosPending_.push_back(std::move(c));
        return;
    }
    if (bdosPending_.empty() || pc != bdosPending_.back().ret) return;
    const Pending& c = bdosPending_.back();
    if (sp != c.sp) {
        if (sp > c.sp) abandon(bdosPending_, sp, 0xFFFF, "BDOS");
        return;
    }
    const uint8_t a = z.getA();
    std::string r;
    switch (c.fn) {
        case 1: case 3: case 6:
            r = a >= 0x20 && a < 0x7F ? fmt("'%c'", a) : fmt("$%02X", a);
            break;
        case 12: case 24: case 27: case 29: case 31:
            r = fmt("HL=$%04X", z.getHL());
            break;
        case 2: case 4: case 5: case 9: case 26:
            r = "";
            break;
        default:
            r = fmt("A=$%02X", a);
            break;
    }
    emit("BDOS " + c.text + (r.empty() ? std::string() : " -> " + r));
    bdosPending_.pop_back();
}

} // namespace pom2
