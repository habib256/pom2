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

// SHA-256 (FIPS 180-4). Small enough to keep here rather than grow a
// dependency, and a header because its callers link different source lists:
// the ROM fetch's install gate, the ROM import, and the SDK's fingerprints.

#ifndef POM2_SHA256_H
#define POM2_SHA256_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace pom2 {

namespace sha256_detail {

struct Sha256Ctx {
    std::uint32_t h[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                           0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    std::uint64_t len = 0;
    std::uint8_t  buf[64]{};
    std::size_t   fill = 0;
};

inline std::uint32_t ror32(std::uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

inline void sha256Block(Sha256Ctx& c, const std::uint8_t* p)
{
    static const std::uint32_t K[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
        0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
        0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
        0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
        0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
        0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
        0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
        0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
        0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u };
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (static_cast<std::uint32_t>(p[i * 4    ]) << 24) |
               (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(p[i * 4 + 2]) <<  8) |
                static_cast<std::uint32_t>(p[i * 4 + 3]);
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = ror32(w[i-15],  7) ^ ror32(w[i-15], 18) ^ (w[i-15] >>  3);
        const std::uint32_t s1 = ror32(w[i- 2], 17) ^ ror32(w[i- 2], 19) ^ (w[i- 2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    std::uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3];
    std::uint32_t e = c.h[4], f = c.h[5], g = c.h[6], hh = c.h[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        const std::uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        const std::uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        const std::uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d;
    c.h[4] += e; c.h[5] += f; c.h[6] += g;  c.h[7] += hh;
}

} // namespace sha256_detail

/// Lowercase hex SHA-256 of `n` bytes at `data`.
inline std::string sha256Hex(const std::uint8_t* data, std::size_t n)
{
    using namespace sha256_detail;
    Sha256Ctx c;
    c.len = static_cast<std::uint64_t>(n) * 8u;
    std::size_t i = 0;
    for (; i + 64 <= n; i += 64) sha256Block(c, data + i);
    std::uint8_t tail[128]{};
    const std::size_t rem = n - i;
    if (rem) std::memcpy(tail, data + i, rem);
    tail[rem] = 0x80;
    const std::size_t blocks = (rem + 1 + 8 > 64) ? 2u : 1u;
    const std::size_t end = blocks * 64;
    for (int b = 0; b < 8; ++b)
        tail[end - 1 - static_cast<std::size_t>(b)] =
            static_cast<std::uint8_t>((c.len >> (8 * b)) & 0xFFu);
    for (std::size_t b = 0; b < blocks; ++b) sha256Block(c, tail + b * 64);

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::uint32_t v : c.h) {
        for (int b = 3; b >= 0; --b) {
            const std::uint8_t byte = static_cast<std::uint8_t>((v >> (8 * b)) & 0xFFu);
            out.push_back(hex[byte >> 4]);
            out.push_back(hex[byte & 0x0Fu]);
        }
    }
    return out;
}

} // namespace pom2

#endif // POM2_SHA256_H
