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

// SoftCard II mailbox protocol — see SoftCard2Protocol.h.

#include "SoftCard2Protocol.h"

#include <cstdio>

namespace pom2 {

namespace {

std::string hex2(uint8_t v) { char b[8]; std::snprintf(b, sizeof b, "$%02X", v); return b; }
std::string hex4(int v) { char b[8]; std::snprintf(b, sizeof b, "$%04X", v & 0xFFFF); return b; }
std::string chr(uint8_t v)
{
    const uint8_t c = v & 0x7F;
    if (c >= 0x20 && c < 0x7F) return std::string("'") + char(c) + "'";
    return hex2(v);
}
std::string diskStatus(uint8_t s)
{
    switch (s) {
        case 0x00: return "ok";
        case 0x10: return "$10 write-protected";
        case 0x20: return "$20 volume mismatch";
        case 0x40: return "$40 I/O error";
        default:   return hex2(s);
    }
}

// Z80 parameter bytes the command takes before the 6502 replies (WRITESEC:
// its sector too; WRITESLOT: the slot only — the character follows the reply).
int z80Count(int cmd)
{
    static const int k[17] = { 5, 2, 3, 4, 260, 1, 1, 1, 1, 0, 2, 34, 2, 2, 0, 4, 3 };
    return k[cmd];
}

int appleCount(int cmd, const std::vector<uint8_t>& reply)
{
    switch (cmd) {
        case 0: return 4;
        case 1: case 4: case 5: case 6: case 7: case 10: return 1;
        case 3: return reply.empty() || reply[0] != 0 ? 1 : 257;
        case 9: return 2560;
        default: return 0;
    }
}

} // namespace

const char* SoftCard2Protocol::commandName(int cmd)
{
    static const char* const k[17] = {
        "CALLSUB", "READMEM", "WRITEMEM", "READSEC", "WRITESEC", "READSLOT",
        "WRITESLOT", "STATSLOT", "INITSLOT", "WSTART", "FORMAT", "UPDATE",
        "BEEP", "CLEAR", "INVERT", "SETPT1", "SETPT2" };
    return cmd >= 0 && cmd < 17 ? k[cmd] : nullptr;
}

void SoftCard2Protocol::z80Sent(uint8_t b)
{
    if (cmd_ < 0) {
        if (b > 16) {               // not a command: out of step, say so once
            emit("SC2  " + hex2(b) + " (not a 6502 BIOS call)");
            return;
        }
        cmd_ = b;
        z80_.clear();
        apple_.clear();
    } else {
        z80_.push_back(b);
    }
    maybeFinish();
}

void SoftCard2Protocol::appleSent(uint8_t b)
{
    if (cmd_ < 0) return;           // the bootstrap's transfers
    apple_.push_back(b);
    maybeFinish();
}

void SoftCard2Protocol::maybeFinish()
{
    const int c = cmd_;
    const int needZ = z80Count(c) + (c == 6 ? 1 : 0);   // WRITESLOT's character
    if (int(z80_.size()) < needZ || int(apple_.size()) < appleCount(c, apple_)) return;
    const auto& z = z80_;
    const auto& a = apple_;
    auto addr = [&](int o) { return int(z[std::size_t(o)] | z[std::size_t(o) + 1] << 8); };
    std::string s = std::string("SC2  ") + commandName(c);
    switch (c) {
        case 0:
            s += " " + hex4(addr(0)) + " A=" + hex2(z[2]) + " X=" + hex2(z[3]) + " Y=" + hex2(z[4])
               + " -> A=" + hex2(a[0]) + " X=" + hex2(a[1]) + " Y=" + hex2(a[2]) + " P=" + hex2(a[3]);
            break;
        case 1:  s += " " + hex4(addr(0)) + " -> " + hex2(a[0]); break;
        case 2:  s += " " + hex4(addr(0)) + " = " + hex2(z[2]); break;
        case 3: case 4:
            s += " S" + std::to_string(z[2]) + ",D" + std::to_string((z[1] & 1) ? 1 : 2)
               + " T" + std::to_string(z[0]) + " sector " + std::to_string(z[3])
               + " -> " + diskStatus(a[0]);
            break;
        case 5:  s += " S" + std::to_string(z[0]) + " -> " + chr(a[0]); break;
        case 6:  s += " S" + std::to_string(z[0]) + " " + chr(z[1]); break;
        case 7:  s += " S" + std::to_string(z[0]) + " -> " + (a[0] ? "ready" : "not ready"); break;
        case 8:  s += " S" + std::to_string(z[0]); break;
        case 9:  s += " -> 2560 bytes"; break;
        case 10: s += " S" + std::to_string(z[0]) + ",D" + std::to_string((z[1] & 1) ? 1 : 2)
                    + " -> " + diskStatus(a[0]); break;
        case 11: s += " (terminal definition, 34 bytes)"; break;
        case 12: s += " duration " + std::to_string(z[0]) + " period " + std::to_string(z[1]); break;
        case 13: s += " " + hex2(z[0]) + "/" + hex2(z[1]); break;
        case 14: break;
        case 15: s += " " + hex4(addr(0)) + " XOR " + hex2(z[2]) + " AND " + hex2(z[3]); break;
        case 16: s += " " + hex4(addr(0)) + " XOR " + hex2(z[2]); break;
        default: break;
    }
    emit(s);
    cmd_ = -1;
}

} // namespace pom2
