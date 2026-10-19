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

// CharRomDump — see the header for the three conventions.

#include "CharRomDump.h"

#include <cstring>

namespace pom2 {

CharRomFacts normaliseCharRom(uint8_t* bytes, std::size_t size)
{
    CharRomFacts facts{};
    if (!bytes || (size != 2048 && size != 4096)) return facts;

    if (size == 4096) {
        for (std::size_t i = 0; i < size; ++i) bytes[i] ^= 0xFF;
        // A 4 KB IIe-class generator always carries lowercase.
        facts.hasLowercase = true;
        facts.bit7Marker   = true;
        return facts;
    }

    // Which 2 KB convention is this? One pass over the inverse+flashing
    // half is enough: the marked dump sets bit 7 on every flashing-range
    // byte, including in the tracked Videx dump; unmarked inputs do not.
    facts.bit7Marker = false;
    for (std::size_t i = 0; i < 1024; ++i) {
        if (bytes[i] & 0x80) { facts.bit7Marker = true; break; }
    }

    for (std::size_t i = 0; i < 2048; ++i) {
        uint8_t n = bytes[i];
        const bool invert = facts.bit7Marker ? (i < 1024 && !(n & 0x80))
                                             : (i < 512);
        if (invert) n ^= 0x7F;
        uint8_t d = 0;
        for (int j = 0; j < 7; ++j) {
            d = static_cast<uint8_t>((d << 1) | (n & 1));
            n >>= 1;
        }
        bytes[i] = d;
    }

    // Compared AFTER normalisation, which consumes the bit-7 marker the two
    // conventions disagree about — so this one test works on both.
    facts.hasLowercase = std::memcmp(bytes + 1024, bytes + 1536, 512) != 0;
    return facts;
}

} // namespace pom2
