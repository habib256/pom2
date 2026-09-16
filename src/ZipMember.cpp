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

#include "ZipMember.h"

#include <algorithm>
#include <climits>

// Only the zlib half of stb_image is used (raw deflate). STB_IMAGE_STATIC
// keeps every entry point internal to this file — MainWindow_MiscPanels and
// Pom2HgrPaintHost own the other copies — and the unused ones are silenced
// here rather than by editing the third-party header.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

namespace pom2 {

namespace {

std::uint32_t crc32Of(const std::vector<std::uint8_t>& bytes)
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::uint8_t b : bytes) {
        crc ^= b;
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

}  // namespace

bool readZipMember(const std::vector<std::uint8_t>& zip,
                   const std::string& member,
                   std::vector<std::uint8_t>& out,
                   std::string& err,
                   std::size_t maxBytes,
                   bool* unsupported)
{
    out.clear();
    if (unsupported) *unsupported = false;
    const std::size_t n = zip.size();
    auto le16 = [&](std::size_t o) -> std::uint32_t {
        return static_cast<std::uint32_t>(zip[o]) |
               (static_cast<std::uint32_t>(zip[o + 1]) << 8);
    };
    auto le32 = [&](std::size_t o) -> std::uint32_t {
        return le16(o) | (le16(o + 2) << 16);
    };

    // End Of Central Directory: scanned back from the tail, because the
    // comment in front of it has a variable length.
    if (n < 22) { err = "archive is too small to be a zip"; return false; }
    const std::size_t maxBack = std::min<std::size_t>(n, 22u + 65535u);
    std::size_t eocd = 0;
    bool found = false;
    for (std::size_t back = 22; back <= maxBack && !found; ++back) {
        const std::size_t o = n - back;
        if (le32(o) == 0x06054B50u) { eocd = o; found = true; }
    }
    if (!found) { err = "no zip central directory (not an archive?)"; return false; }

    const std::uint32_t count = le16(eocd + 10);
    std::size_t cd = le32(eocd + 16);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (cd + 46 > n || le32(cd) != 0x02014B50u) {
            err = "malformed zip central directory";
            return false;
        }
        const std::uint32_t nameLen = le16(cd + 28);
        const std::size_t   next    = cd + 46 + nameLen + le16(cd + 30) + le16(cd + 32);
        if (cd + 46 + nameLen > n) { err = "malformed zip central directory"; return false; }
        const std::string name(reinterpret_cast<const char*>(zip.data() + cd + 46), nameLen);
        if (name != member) { cd = next; continue; }

        const std::uint32_t flags  = le16(cd + 8);
        const std::uint32_t method = le16(cd + 10);
        const std::uint32_t crc    = le32(cd + 16);
        const std::uint32_t csize  = le32(cd + 20);
        const std::uint32_t usize  = le32(cd + 24);
        const std::size_t   local  = le32(cd + 42);
        if ((flags & 1u) || (method != 0 && method != 8)) {
            if (unsupported) *unsupported = true;
            err = member + ": encrypted or compressed with method " +
                  std::to_string(method) + " — not readable in-process";
            return false;
        }
        if (usize > maxBytes || usize > static_cast<std::uint32_t>(INT_MAX) ||
            csize > static_cast<std::uint32_t>(INT_MAX)) {
            err = member + " expands to " + std::to_string(usize) +
                  " bytes — refusing to unpack it";
            return false;
        }
        if (local + 30 > n || le32(local) != 0x04034B50u) {
            err = member + ": malformed local header";
            return false;
        }
        const std::size_t data = local + 30 + le16(local + 26) + le16(local + 28);
        if (data > n || csize > n - data) {
            err = member + ": truncated archive";
            return false;
        }
        const std::uint8_t* src = zip.data() + data;
        if (method == 0) {
            if (csize != usize) { err = member + ": stored sizes disagree"; return false; }
            out.assign(src, src + csize);
        } else {
            out.resize(usize);
            // One spare byte: a decoder that would overrun reports it as a
            // length mismatch instead of a short read that looks complete.
            out.push_back(0);
            const int got = stbi_zlib_decode_noheader_buffer(
                reinterpret_cast<char*>(out.data()), static_cast<int>(out.size()),
                reinterpret_cast<const char*>(src), static_cast<int>(csize));
            if (got != static_cast<int>(usize)) {
                out.clear();
                err = member + ": inflate failed";
                return false;
            }
            out.resize(usize);
        }
        if (crc32Of(out) != crc) {
            out.clear();
            err = member + ": CRC-32 mismatch — damaged archive";
            return false;
        }
        return true;
    }
    err = member + " is not in the archive";
    return false;
}

}  // namespace pom2
