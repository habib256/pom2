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

// OS-call trace, the guest systems that are not Apple's: the Monitor's
// console, Applecorn's BBC MOS and CP/M-65 (OsCallTrace.h has the shapes).

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

// One character as the trace shows it: printable, or ^X.
std::string shown(uint8_t c)
{
    c &= 0x7F;
    if (c >= 0x20 && c < 0x7F) return std::string(1, char(c));
    if (c == 0x7F) return "^?";
    return std::string("^") + char(c + 0x40);
}

// The OSBYTEs Applecorn's software uses (izapple2 traceApplecorn.go;
// beebwiki.mdfs.net/OSBYTEs).
const char* osbyteName(uint8_t a)
{
    switch (a) {
        case 0x00: return "OS version";
        case 0x02: return "select input";
        case 0x03: return "select output";
        case 0x7C: return "clear Escape";
        case 0x7D: return "set Escape";
        case 0x7E: return "acknowledge Escape";
        case 0x7F: return "end of file?";
        case 0x80: return "read ADC / buffer";
        case 0x81: return "read key with time limit";
        case 0x82: return "read high-order address";
        case 0x83: return "read OSHWM";
        case 0x84: return "read HIMEM";
        case 0x85: return "HIMEM for mode";
        case 0x86: return "read text cursor";
        case 0x8B: return "set filing system options";
        case 0xDA: return "clear VDU queue";
        default:   return nullptr;
    }
}

} // namespace

std::string OsCallTrace::terminated(uint16_t a, uint8_t end, std::size_t max) const
{
    std::string s;
    for (std::size_t i = 0; i < max; ++i) {
        const uint8_t c = peek(uint16_t(a + i));
        if (c == end || c == 0) break;
        s += shown(c);
    }
    return s;
}

// ── Monitor ──────────────────────────────────────────────────────────────

void OsCallTrace::monStep(uint16_t pc)
{
    if (!cpu_) return;
    const uint8_t s = cpu_->getStackPointer();
    if (!monPending_.empty()) {
        const Pending& c = monPending_.back();
        if (pc == c.ret && s == c.sp) {
            const int n = cpu_->getXRegister();
            std::string line;
            for (int i = 0; i < n; ++i) line += shown(peek(uint16_t(0x0200 + i)));
            // DOS 3.3 runs a command line at its CR and hands back an empty
            // one, the buffer cleared; the echo still says what was typed.
            emit("MON  " + c.text + " -> \"" + line + "\"" +
                 (n || monEcho_.empty() ? std::string() : " (DOS took \"" + monEcho_ + "\")"));
            monPending_.pop_back();
            return;
        }
        // Unwound without returning: DOS 3.3 takes a command line at its CR
        // (output hook), runs it and restarts Applesoft. The line is still
        // in the input buffer.
        if (s > c.sp) {
            emit("MON  " + c.text + " -> \"" + monEcho_ + "\" (not returned)");
            monPending_.pop_back();
        }
        if (pc < 0xFD00 || pc >= 0xFE00) return;
    }
    if (pc == 0xFDED) {
        // COUT, as long as it is the Monitor's (JMP (CSW)); the echo of a
        // line being typed belongs to the GETLN that reads it.
        if (peek(0xFDED) != 0x6C) return;
        const uint8_t c = cpu_->getAccumulator() & 0x7F;
        if (!monPending_.empty()) {
            // GETLN's own output: its prompt (GETLNZ a CR before it), then
            // the echo of the keys, up to the CR that ends the line —
            // after which DOS may print a whole CATALOG from inside it.
            if (monEchoState_ == 0) {
                if (c != 0x0D) monEchoState_ = 1;      // the prompt itself
            } else if (monEchoState_ == 1) {
                if (c == 0x0D) monEchoState_ = 2;
                else if (c == 0x08) { if (!monEcho_.empty()) monEcho_.pop_back(); }
                else if (monEcho_.size() < 120) monEcho_ += shown(c);
            }
            return;
        }
        if (c == 0x0D) {
            if (!monLine_.empty()) emit("MON  \"" + monLine_ + "\"");
            monLine_.clear();
        } else {
            monLine_ += shown(c);
            if (monLine_.size() >= 120) { emit("MON  \"" + monLine_ + "\""); monLine_.clear(); }
        }
        return;
    }
    if (pc != 0xFD6A && pc != 0xFD67 && pc != 0xFD6F) return;
    // GETLN starts LDA PROMPT ($33) on every Apple II monitor.
    if (peek(0xFD6A) != 0xA5 || peek(0xFD6B) != 0x33) return;
    Pending c;
    c.sp = uint8_t(s + 2);
    // GETLNZ falls into GETLN, which the editing keys jump back to: one call.
    if (!monPending_.empty() && monPending_.back().sp == c.sp) return;
    const uint16_t jsrLast = uint16_t(peek(uint16_t(0x0101 + s)) | peek(uint16_t(0x0101 + uint8_t(s + 1))) << 8);
    c.ret = uint16_t(jsrLast + 1);
    c.fn  = 0;
    c.arg = 0;
    const std::string prompt = monLine_.empty() ? std::string() : " \"" + monLine_ + "\"";
    monLine_.clear();
    monEcho_.clear();
    monEchoState_ = pc == 0xFD6F ? 1 : 0;       // GETLN1 prints no prompt
    c.text = fmt("$%04X GETLN", uint16_t(jsrLast - 2)) + prompt +
             (pc == 0xFD6F ? std::string() : " '" + shown(peek(0x33)) + "'");
    abandon(monPending_, c.sp, 0xFF, "MON ");
    if (monPending_.size() < 16) monPending_.push_back(std::move(c));
}

// ── Applecorn (BBC Micro MOS) ────────────────────────────────────────────

void OsCallTrace::acornStep(uint16_t pc)
{
    if (!cpu_ || !mem_ || !(mem_->iieModeFlags() & Memory::MF_RAMRD)) return;
    const uint8_t a = cpu_->getAccumulator(), x = cpu_->getXRegister(), y = cpu_->getYRegister();
    auto word = [&](uint16_t at) { return uint16_t(peek(at) | peek(uint16_t(at + 1)) << 8); };
    auto chr = [&](uint8_t c) { return fmt("$%02X", c) + (c >= 0x20 && c < 0x7F ? " '" + shown(c) + "'" : std::string()); };

    if (pc == 0x8000 || pc == 0x8003) {
        const std::string rom = "\"" + terminated(0x8009, 0) + "\"";
        if (pc == 0x8000) {
            emit("MOS  LANGUAGE " + rom + fmt(" A=$%02X", a));
        } else {
            std::string what;
            switch (a) {
                case 4: what = "OSCLI \"" + terminated(uint16_t(word(0xF2) + y), 0x0D) + "\""; break;
                case 6: {
                    const uint16_t e = word(0xFD);
                    what = fmt("ERROR %d \"", peek(e)) + terminated(uint16_t(e + 1), 0) + "\"";
                    break;
                }
                case 7: what = fmt("OSBYTE $%02X X=$%02X Y=$%02X", peek(0xEF), peek(0xF0), peek(0xF1)); break;
                case 8: what = fmt("OSWORD $%02X", peek(0xEF)); break;
                case 9: what = "*HELP \"" + terminated(uint16_t(word(0xF2) + y), 0x0D) + "\""; break;
                default: what = fmt("%d", a); break;
            }
            emit("MOS  SERVICE " + rom + " " + what);
        }
    }

    const bool inKernel = pc >= 0xC000;
    if (!acornInKernel_ && inKernel && pc >= 0xD000) {
        // A vectored call lands where its page-2 vector points.
        uint16_t api = pc;
        static const struct { uint16_t vec, api; } kVectors[] = {
            { 0x0208, 0xFFF7 }, { 0x020A, 0xFFF4 }, { 0x020C, 0xFFF1 }, { 0x020E, 0xFFEE },
            { 0x0210, 0xFFE0 }, { 0x0212, 0xFFDD }, { 0x0214, 0xFFDA }, { 0x0216, 0xFFD7 },
            { 0x0218, 0xFFD4 }, { 0x021A, 0xFFD1 }, { 0x021C, 0xFFCE },
        };
        for (const auto& v : kVectors)
            if (pc == word(v.vec)) { api = v.api; break; }
        if (pc == word(0xFFFE)) api = 0xFFFE;
        const uint16_t xy = uint16_t(x | y << 8);
        std::string s;
        bool console = false;
        switch (api) {
            case 0xFFCE:
                s = a == 0 ? fmt("OSFIND close handle=%d", y)
                           : fmt("OSFIND open $%02X \"", a) + terminated(xy, 0x0D) + "\"";
                break;
            case 0xFFD1: s = fmt("OSGBPB A=$%02X block=$%04X", a, xy); break;
            case 0xFFD4: s = fmt("OSBPUT handle=%d ", y) + chr(a); break;
            case 0xFFD7: s = fmt("OSBGET handle=%d", y); break;
            case 0xFFDA: s = fmt("OSARGS A=$%02X handle=%d", a, y); break;
            case 0xFFDD: s = fmt("OSFILE A=$%02X \"", a) + terminated(word(xy), 0x0D) + "\""; break;
            case 0xFFF1:
                s = a == 0 ? fmt("OSWORD 0 (read line) buf=$%04X max=%d", word(xy), peek(uint16_t(xy + 2)))
                           : fmt("OSWORD $%02X block=$%04X", a, xy);
                break;
            case 0xFFF4: {
                const char* n = osbyteName(a);
                s = fmt("OSBYTE $%02X", a) + (n ? std::string(" (") + n + ")" : std::string()) +
                    fmt(" X=$%02X Y=$%02X", x, y);
                break;
            }
            case 0xFFF7: s = "OSCLI \"" + terminated(xy, 0x0D) + "\""; break;
            case 0xFFFE: {
                const uint16_t at = uint16_t(word(uint16_t(0x0100 + uint8_t(cpu_->getStackPointer() + 2))) - 1);
                s = fmt("BRK %d \"", peek(at)) + terminated(uint16_t(at + 1), 0) + "\"";
                break;
            }
            // The console, every character: left out, as izapple2 does.
            case 0xFFC8: case 0xFFCB: case 0xFFE0: case 0xFFE3:
            case 0xFFE7: case 0xFFEC: case 0xFFEE:
                console = true;
                break;
            default:
                console = true;                  // an internal jump, not a call
                break;
        }
        acornCall_ = AcornCall{ s, a, x, y, api, !console };
    }
    if (acornInKernel_ && !inKernel && acornCall_.log) {
        std::string r = fmt(" -> A=$%02X X=$%02X Y=$%02X", a, x, y);
        if (acornCall_.api == 0xFFF1 && acornCall_.a == 0) {
            const uint16_t cb = uint16_t(acornCall_.x | acornCall_.y << 8);
            r += " \"" + terminated(word(cb), 0x0D) + "\"";
        }
        emit("MOS  " + acornCall_.text + r);
        acornCall_.log = false;
    }
    acornInKernel_ = inKernel;
}

// ── CP/M-65 ──────────────────────────────────────────────────────────────

const char* OsCallTrace::cpm65BdosName(uint8_t fn)
{
    static const char* const k[] = {
        "EXIT_PROGRAM", "CONSOLE_INPUT", "CONSOLE_OUTPUT", "AUX_INPUT", "AUX_OUTPUT",
        "PRINTER_OUTPUT", "DIRECT_IO", "GET_IO_BYTE", "SET_IO_BYTE", "WRITE_STRING",
        "READ_LINE", "CONSOLE_STATUS", "GET_VERSION", "RESET_DISKS", "SELECT_DISK",
        "OPEN_FILE", "CLOSE_FILE", "FIND_FIRST", "FIND_NEXT", "DELETE_FILE",
        "READ_SEQUENTIAL", "WRITE_SEQUENTIAL", "CREATE_FILE", "RENAME_FILE",
        "GET_LOGIN_BITMAP", "GET_CURRENT_DRIVE", "SET_DMA_ADDRESS",
        "GET_ALLOCATION_BITMAP", "SET_DRIVE_READONLY", "GET_READONLY_BITMAP",
        "SET_FILE_ATTRIBUTES", "GET_DPB", "GET_SET_USER_NUMBER", "READ_RANDOM",
        "WRITE_RANDOM", "COMPUTE_FILE_SIZE", "COMPUTE_RANDOM_POINTER", "RESET_DISK",
        "GET_BIOS", nullptr, "WRITE_RANDOM_FILLED", "GETZP", "GETTPA", "PARSE_FILENAME",
    };
    return fn < sizeof k / sizeof k[0] ? k[fn] : nullptr;
}

const char* OsCallTrace::cpm65BiosName(uint8_t fn)
{
    static const char* const k[] = {
        "CONST", "CONIN", "CONOUT", "SELDSK", "SETSEC", "SETDMA", "READ", "WRITE",
        "RELOCATE", "GETTPA", "SETTPA", "GETZP", "SETZP", "SETBANK", "ADDDRV", "FINDDRV",
    };
    return fn < sizeof k / sizeof k[0] ? k[fn] : nullptr;
}

// src/bdos/dispatch.S ENTRY: STA param / STX param+1 / JSR BEGINSYS /
// JSR icall / JMP ENDSYS, icall = LDA hi,Y / PHA / LDA lo,Y / PHA / RTS.
bool OsCallTrace::isCpm65Bdos(uint16_t pc) const
{
    if (peek(pc) != 0x85 || peek(uint16_t(pc + 2)) != 0x86 ||
        peek(uint16_t(pc + 3)) != uint8_t(peek(uint16_t(pc + 1)) + 1) ||
        peek(uint16_t(pc + 4)) != 0x20 || peek(uint16_t(pc + 7)) != 0x20 ||
        peek(uint16_t(pc + 10)) != 0x4C)
        return false;
    const uint16_t i = uint16_t(peek(uint16_t(pc + 8)) | peek(uint16_t(pc + 9)) << 8);
    return peek(i) == 0xB9 && peek(uint16_t(i + 3)) == 0x48 && peek(uint16_t(i + 4)) == 0xB9 &&
           peek(uint16_t(i + 7)) == 0x48 && peek(uint16_t(i + 8)) == 0x60;
}

// src/lib/biosentry.S: jmpdispatch (STA ptr / LDA hi,Y / PHA / LDA lo,Y /
// PHA / LDA ptr / RTS) over the BIOS's 16-entry table, which follows it —
// a driver's strategy routine dispatches the same way over its own table.
bool OsCallTrace::isCpm65Bios(uint16_t pc) const
{
    if (peek(pc) != 0x85 || peek(uint16_t(pc + 2)) != 0xB9 || peek(uint16_t(pc + 5)) != 0x48 ||
        peek(uint16_t(pc + 6)) != 0xB9 || peek(uint16_t(pc + 9)) != 0x48 ||
        peek(uint16_t(pc + 10)) != 0xA5 || peek(uint16_t(pc + 11)) != peek(uint16_t(pc + 1)) ||
        peek(uint16_t(pc + 12)) != 0x60)
        return false;
    const uint16_t hi = uint16_t(peek(uint16_t(pc + 3)) | peek(uint16_t(pc + 4)) << 8);
    const uint16_t lo = uint16_t(peek(uint16_t(pc + 7)) | peek(uint16_t(pc + 8)) << 8);
    return lo == uint16_t(pc + 13) && hi == uint16_t(lo + 16);
}

void OsCallTrace::cpm65Step(uint16_t pc)
{
    if (!cpu_) return;
    const uint8_t s = cpu_->getStackPointer();
    const uint8_t a = cpu_->getAccumulator(), x = cpu_->getXRegister(), y = cpu_->getYRegister();
    const uint16_t xa = uint16_t(a | x << 8);

    if (!cpmPending_.empty() && pc == cpmPending_.back().ret) {
        const Pending& c = cpmPending_.back();
        if (s != c.sp) {
            if (s > c.sp) abandon(cpmPending_, s, 0xFF, "CPM65");
            return;
        }
        const bool carry = (cpu_->getStatusRegister() & M6502::Status::C) != 0;
        std::string r = fmt("A=$%02X X=$%02X", a, x) + (carry ? " C" : "");
        if (c.fn == 10) r = "\"" + terminated(uint16_t(c.arg + 2), 0, peek(uint16_t(c.arg + 1))) + "\"";
        emit("CPM65 " + c.text + " -> " + r);
        cpmPending_.pop_back();
        return;
    }

    if (isCpm65Bdos(pc)) {
        if (y == 1 || y == 2 || y == 6 || y == 11) return;   // the console, every character
        const char* name = cpm65BdosName(y);
        std::string t = name ? name : fmt("BDOS_%d", y);
        auto fcb = [&](uint16_t f) {
            const uint8_t dr = peek(f);
            std::string n = dr ? std::string(1, char('A' + ((dr - 1) & 0x0F))) + ":" : std::string();
            for (int i = 1; i <= 11; ++i) {
                if (i == 9) n.push_back('.');
                const char ch = char(peek(uint16_t(f + i)) & 0x7F);
                if (ch != ' ') n.push_back(ch >= 0x20 && ch < 0x7F ? ch : '?');
            }
            if (!n.empty() && n.back() == '.') n.pop_back();
            return n;
        };
        switch (y) {
            case 9:  t += " \"" + terminated(xa, '$') + "\""; break;
            case 14: t += fmt(" %c:", 'A' + (a & 0x0F)); break;
            case 15: case 16: case 17: case 19: case 20: case 21: case 22:
            case 30: case 33: case 34: case 35: case 36: case 40:
                t += " " + fcb(xa);
                break;
            case 23: t += " " + fcb(xa) + " " + fcb(uint16_t(xa + 16)); break;
            case 0: case 12: case 13: case 18: case 24: case 25: case 27:
            case 29: case 31: case 38: case 41: case 42:
                break;
            default: t += fmt(" XA=$%04X", xa); break;
        }
        const uint16_t jsrLast = uint16_t(peek(uint16_t(0x0101 + s)) | peek(uint16_t(0x0101 + uint8_t(s + 1))) << 8);
        Pending c;
        c.ret = uint16_t(jsrLast + 1);
        c.sp  = uint8_t(s + 2);
        c.fn  = y;
        c.arg = xa;
        c.text = fmt("$%04X BDOS ", uint16_t(jsrLast - 2)) + t;
        abandon(cpmPending_, c.sp, 0xFF, "CPM65");
        if (y == 0) { emit("CPM65 " + c.text); return; }   // never comes back
        if (cpmPending_.size() < 16) cpmPending_.push_back(std::move(c));
        return;
    }

    if (isCpm65Bios(pc)) {
        if (y <= 2) return;                              // CONST / CONIN / CONOUT
        // SETSEC and SETDMA only aim the next READ / WRITE: kept, and said
        // on the transfer's line.
        if (y == 4) {
            cpmSector_ = uint32_t(peek(xa) | peek(uint16_t(xa + 1)) << 8 | peek(uint16_t(xa + 2)) << 16);
            return;
        }
        if (y == 5) { cpmDma_ = xa; return; }
        const char* name = cpm65BiosName(y);
        std::string t = std::string("BIOS ") + (name ? name : fmt("BIOS_%d", y));
        switch (y) {
            case 3: t += fmt(" %c:", 'A' + (a & 0x0F)); break;
            case 6: case 7: t += fmt(" sector=%u dma=$%04X", unsigned(cpmSector_), cpmDma_); break;
            case 9: case 11: break;
            default: t += fmt(" XA=$%04X", xa); break;
        }
        emit("CPM65 " + t);
    }
}

} // namespace pom2
