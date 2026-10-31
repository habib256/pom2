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

// Z80StateBlob — the Z80 register file in a card snapshot: 22 bytes of
// 8-bit fields then IX, IY, SP, PC, WZ little-endian (32 bytes), the layout
// SoftCardZ80 and AlsCpmCard write by hand. The cards with their own Z80
// and RAM (SoftCardIICard, ApplicardCard, PremiumSoftCardCard) share it.

#ifndef POM2_Z80_STATE_BLOB_H
#define POM2_Z80_STATE_BLOB_H

#include "Z80.h"

#include <cstdint>
#include <vector>

namespace pom2 {

constexpr std::size_t kZ80StateBlobBytes = 22 + 10;

inline void appendZ80State(std::vector<uint8_t>& out, const Z80::State& s)
{
    const uint8_t regs8[] = {
        s.a, s.f, s.b, s.c, s.d, s.e, s.h, s.l,
        s.a2, s.f2, s.b2, s.c2, s.d2, s.e2, s.h2, s.l2,
        s.i, s.r, s.im,
        uint8_t((s.iff1 ? 1 : 0) | (s.iff2 ? 2 : 0) | (s.halted ? 4 : 0)
                | (s.afterEi ? 8 : 0) | (s.nmiPending ? 16 : 0)
                | (s.irqLine ? 32 : 0)),
        s.irqData, s.pendingPrefix,
    };
    out.insert(out.end(), regs8, regs8 + sizeof(regs8));
    for (uint16_t w : { s.ix, s.iy, s.sp, s.pc, s.wz }) {
        out.push_back(uint8_t(w));
        out.push_back(uint8_t(w >> 8));
    }
}

/// Reads kZ80StateBlobBytes from `p`; the caller checks the length.
inline Z80::State readZ80State(const uint8_t* p)
{
    Z80::State s;
    s.a = p[0];  s.f = p[1];  s.b = p[2];  s.c = p[3];
    s.d = p[4];  s.e = p[5];  s.h = p[6];  s.l = p[7];
    s.a2 = p[8]; s.f2 = p[9]; s.b2 = p[10]; s.c2 = p[11];
    s.d2 = p[12]; s.e2 = p[13]; s.h2 = p[14]; s.l2 = p[15];
    s.i = p[16]; s.r = p[17]; s.im = p[18];
    const uint8_t misc = p[19];
    s.iff1       = (misc & 1) != 0;
    s.iff2       = (misc & 2) != 0;
    s.halted     = (misc & 4) != 0;
    s.afterEi    = (misc & 8) != 0;
    s.nmiPending = (misc & 16) != 0;
    s.irqLine    = (misc & 32) != 0;
    s.irqData = p[20];
    s.pendingPrefix = p[21];
    p += 22;
    uint16_t* words[] = { &s.ix, &s.iy, &s.sp, &s.pc, &s.wz };
    for (uint16_t* w : words) {
        *w = uint16_t(p[0] | (p[1] << 8));
        p += 2;
    }
    return s;
}

} // namespace pom2

#endif // POM2_Z80_STATE_BLOB_H
