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

// MacBinary — the 128-byte header legacy Mac downloads wrap a disk image in.
// One predicate for the two loaders that unwrap it: DiskImage (5.25") and
// Disk35Image (3.5", since 2026-11-07 — bug hunt 2026-11-01 round 4: only
// the 5.25" loader stripped it, and a wrapped 800K image was refused).
//
// [D] AppleWin DiskImageHelper.cpp: byte 0 == 0 (the "old version" marker),
// byte 1 in 1..63 (Pascal-string filename length), a 0 after the filename,
// bytes 122-123 == 0 (reserved). All four fit inside an ordinary image's
// first sector, and ordinary images DO hit them by chance: a caller strips
// only when that turns an unrecognised length into a recognised one.

#ifndef POM2_MAC_BINARY_H
#define POM2_MAC_BINARY_H

#include <cstddef>
#include <cstdint>

namespace pom2::macbinary {

constexpr std::size_t kHeaderBytes = 128;

inline bool looksLike(const std::uint8_t* p, std::size_t n)
{
    if (n < kHeaderBytes || p[0] != 0x00) return false;
    const std::uint8_t nameLen = p[1];
    if (nameLen == 0 || nameLen >= 64) return false;
    if (p[1 + nameLen + 1] != 0x00) return false;
    return p[122] == 0x00 && p[123] == 0x00;
}

/// The header's data-fork length (big-endian u32 at bytes 83-86). The spec
/// pads the fork to a multiple of 128 and a resource fork may follow, so
/// "everything after the header" is not the image. 0 = absent or implausible.
inline std::size_t dataForkLength(const std::uint8_t* p, std::size_t n)
{
    if (n < kHeaderBytes) return 0;
    const std::size_t len = (std::size_t(p[83]) << 24) | (std::size_t(p[84]) << 16) |
                            (std::size_t(p[85]) << 8)  |  std::size_t(p[86]);
    return len > 0 && len <= n - kHeaderBytes ? len : 0;
}

}  // namespace pom2::macbinary

#endif  // POM2_MAC_BINARY_H
