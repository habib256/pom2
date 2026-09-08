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

// HDV RAM-writable smoke test. Pins the Nox-Archaist crash fix: a ProDOS
// hard disk with no medium write-protect flag must present a READ/WRITE
// volume to ProDOS even when host-file write-back is OFF (the default).
//
// The crash mechanism this guards against: ProDOSHardDiskCard used to fold
// the "don't modify my .hdv file" preference into isWriteProtected(), so a
// default-mounted HDV reported write-protected on the $Cn03 status register
// (bit 6). The ROM WRITE_BLOCK trampoline reads that bit and returns ProDOS
// error $2B without writing — so a game that writes state on the fly (Nox
// Archaist entering a city) got an unexpected write-protect error on its own
// boot volume and crashed. The fix: status bit 6 reflects only the real
// medium WP flag (2MG header); RAM writes always succeed; persistence to the
// host file is the separate write-back opt-in.
//
// What is checked here, at the soft-switch register level (no ROM, no CPU):
//   1. Raw .hdv, write-back OFF: status bit7=loaded, bit6=NOT-WP; a block
//      written via the data port reads back from RAM; host file untouched.
//   2. 2MG header WP flag set: status bit6=WP; the write is dropped and the
//      block reads back unchanged (real write-protect still honoured).

#include "MediaWritePolicy.h"
#include "ProDOSHardDiskCard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr size_t kBlock = ProDOSHardDiskCard::kBlockBytes;

void writeFile(const fs::path& p, const std::vector<uint8_t>& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    assert(f.good());
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    assert(f.good());
}

std::vector<uint8_t> readAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const size_t sz = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> out(sz);
    f.read(reinterpret_cast<char*>(out.data()),
           static_cast<std::streamsize>(sz));
    return out;
}

// Select a block then stream 512 bytes into it via $C0D2 (low4=0x2).
void cardWriteBlock(ProDOSHardDiskCard& card, uint16_t block, const uint8_t* p)
{
    card.deviceSelectWrite(0x0, static_cast<uint8_t>(block & 0xFF));
    card.deviceSelectWrite(0x1, static_cast<uint8_t>((block >> 8) & 0xFF));
    for (size_t i = 0; i < kBlock; ++i) card.deviceSelectWrite(0x2, p[i]);
}

// Select a block then stream 512 bytes back out of it via $C0D2 (low4=0x2).
void cardReadBlock(ProDOSHardDiskCard& card, uint16_t block, uint8_t* out)
{
    card.deviceSelectWrite(0x0, static_cast<uint8_t>(block & 0xFF));
    card.deviceSelectWrite(0x1, static_cast<uint8_t>((block >> 8) & 0xFF));
    for (size_t i = 0; i < kBlock; ++i) out[i] = card.deviceSelectRead(0x2);
}

}  // namespace

int main()
{
    // ── Case 1: raw .hdv, write-back OFF → R/W volume, file untouched ────
    {
        const fs::path p = fs::temp_directory_path() / "pom2_hdv_rw_1.hdv";
        std::vector<uint8_t> file(3 * kBlock, 0);
        for (size_t i = 0; i < file.size(); ++i)
            file[i] = static_cast<uint8_t>(i & 0xFF);
        writeFile(p, file);
        const auto original = readAll(p);

        ProDOSHardDiskCard card;
        assert(card.loadImage(p.string()));
        // Default follows MediaWritePolicy (writable in the product,
        // protected under the suite's environment); no medium WP flag.
        assert(card.isWriteBackEnabled() == pom2::mediaWritableByDefault());
        card.setWriteBackEnabled(false);             // the user's write-protect
        assert(!card.isWriteProtected());

        // ProDOS-visible status ($Cn03): loaded (bit7=0) AND not WP (bit6=0).
        const uint8_t status = card.deviceSelectRead(0x3);
        assert((status & 0x80) == 0);   // bit7=0 → image loaded
        assert((status & 0x40) == 0);   // bit6=0 → NOT write-protected

        uint8_t pattern[kBlock];
        for (size_t i = 0; i < kBlock; ++i)
            pattern[i] = static_cast<uint8_t>((i * 7u + 3u) & 0xFFu);
        cardWriteBlock(card, 2, pattern);
        assert(card.hasUnsavedChanges());            // landed in RAM

        uint8_t back[kBlock];
        cardReadBlock(card, 2, back);
        assert(std::memcmp(pattern, back, kBlock) == 0);

        card.ejectImage();                           // write-back off → no flush
        assert(readAll(p) == original);              // host file untouched

        fs::remove(p);
        std::printf("hdv_ram_writable: raw .hdv R/W (write-back off) OK\n");
    }

    // ── Case 2: 2MG header WP flag → real write-protect honoured ─────────
    {
        const fs::path p = fs::temp_directory_path() / "pom2_hdv_rw_2.2mg";
        std::vector<uint8_t> file(64 + 2 * kBlock, 0);
        file[0] = '2'; file[1] = 'I'; file[2] = 'M'; file[3] = 'G';
        file[8] = 64;                       // header_len
        file[12] = 1;                       // format = ProDOS
        file[16] = 1;                       // flags bit0 = write-protected
        file[24] = 64;                      // data_offset
        const uint32_t dlen = 2 * static_cast<uint32_t>(kBlock);
        file[28] = static_cast<uint8_t>(dlen & 0xFF);
        file[29] = static_cast<uint8_t>((dlen >> 8) & 0xFF);
        for (size_t i = 0; i < 2 * kBlock; ++i)
            file[64 + i] = static_cast<uint8_t>((i * 11u + 5u) & 0xFFu);
        writeFile(p, file);

        ProDOSHardDiskCard card;
        assert(card.loadImage(p.string()));
        assert(card.isWriteProtected());             // medium WP flag

        const uint8_t status = card.deviceSelectRead(0x3);
        assert((status & 0x80) == 0);   // loaded
        assert((status & 0x40) != 0);   // bit6=1 → write-protected

        uint8_t before[kBlock];
        cardReadBlock(card, 0, before);

        uint8_t pattern[kBlock];
        std::memset(pattern, 0x5A, sizeof(pattern));
        cardWriteBlock(card, 0, pattern);            // must be dropped
        assert(!card.hasUnsavedChanges());

        uint8_t after[kBlock];
        cardReadBlock(card, 0, after);
        assert(std::memcmp(before, after, kBlock) == 0);   // unchanged

        card.ejectImage();
        fs::remove(p);
        std::printf("hdv_ram_writable: 2MG WP flag honoured OK\n");
    }

    std::printf("hdv_ram_writable_smoke OK\n");
    return 0;
}
