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

// Mass-storage pinning for ProDOSHardDiskCard — the 32 MB ProDOS HDV /
// 2IMG gaps from TODO § SmartPort that no existing test covered:
//
//   (a) 32 MB capacity boundary: exactly 65536 blocks (32 MiB) loads;
//       65537 blocks is rejected; a non-512-multiple file is rejected.
//   (a) 16-bit block addressing — a 32 MB volume needs block numbers up
//       to 65535, so the HIGH byte of the block selector ($C0D1) must
//       work. The existing smoke/write-back tests only ever touch blocks
//       0-1 (low byte only); this reaches block 0x101.
//   (b) .2mg with a data offset != 64 (extra header padding).
//
// NOT here (already pinned by `hdv_writeback_smoke_test.cpp`): 2IMG
// header + trailing comment-chunk preservation across write-back, the
// write-protect flag, and the write-back-default-off contract.
//
// (c) Multiple ProDOS partitions per image is a separate FEATURE (today
//     1 image = 1 unit = 1 volume) — out of scope for this pinning test.
//
// Core test: no ImGui, no OpenGL.

#include "Memory.h"
#include "ProDOSHardDiskCard.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <vector>
#include "TestTempPath.h"

namespace {

constexpr size_t kBlk = ProDOSHardDiskCard::kBlockBytes;

void writeFile(const std::string& path, const std::vector<uint8_t>& b)
{
    std::ofstream f(path, std::ios::binary);
    assert(f.good());
    f.write(reinterpret_cast<const char*>(b.data()),
            static_cast<std::streamsize>(b.size()));
    assert(f.good());
}

// Sparse file of `blocks` ProDOS blocks — only the logical size is set, so
// the 32 MiB cases don't materialise 33 MB of bytes. loadImage reads it
// back as zeros.
void writeSparseHdv(const std::string& path, size_t blocks)
{
    std::ofstream f(path, std::ios::binary);
    assert(f.good());
    f.seekp(static_cast<std::streamoff>(blocks * kBlk) - 1);
    const char z = 0;
    f.write(&z, 1);
    assert(f.good());
}

// 2IMG header of `dataOffset` bytes (>= 64). The data-offset field
// (offset 24) is what the loader honours; the header-length field
// (offset 8) stays the conventional 64.
std::vector<uint8_t> twoImgHeader(uint32_t blocks, uint32_t dataOffset,
                                  uint32_t dataLen)
{
    std::vector<uint8_t> h(dataOffset, 0x00);
    h[0] = '2'; h[1] = 'I'; h[2] = 'M'; h[3] = 'G';
    h[8] = 64;                                  // header length (std)
    h[12] = 1;                                  // format 1 = ProDOS
    h[20] = static_cast<uint8_t>(blocks & 0xFF);
    h[21] = static_cast<uint8_t>((blocks >> 8) & 0xFF);
    auto le32 = [&](size_t o, uint32_t v) {
        h[o] = v & 0xFF; h[o + 1] = (v >> 8) & 0xFF;
        h[o + 2] = (v >> 16) & 0xFF; h[o + 3] = (v >> 24) & 0xFF;
    };
    le32(24, dataOffset);
    le32(28, dataLen);
    return h;
}

} // namespace

int main()
{
    // ── (a) ProDOS 16-bit capacity boundary ──────────────────────────
    // ProDOS block numbers are 16-bit → highest block INDEX is $FFFF, so a
    // volume can hold up to 65536 blocks (indices 0..$FFFF) = exactly 32 MiB.
    // Real 32 MiB raw .hdv dumps (e.g. A2DeskTop-GIST.hdv) are exactly 65536
    // blocks and MUST load; only 65537+ needs the unreachable index $10000.
    {
        const std::string p = pom2test::tempPath("pom2_hdv_max.hdv");
        writeSparseHdv(p, 0xFFFF);           // 65535 blocks
        ProDOSHardDiskCard c;
        assert(c.loadImage(p));
        assert(c.getBlockCount() == 0xFFFF);
    }
    {
        const std::string p = pom2test::tempPath("pom2_hdv_32mb.hdv");
        writeSparseHdv(p, 0x10000);          // 65536 blocks — full 32 MiB, legal max
        ProDOSHardDiskCard c;
        assert(c.loadImage(p));
        assert(c.getBlockCount() == 0x10000);
    }
    {
        const std::string p = pom2test::tempPath("pom2_hdv_over.hdv");
        writeSparseHdv(p, 0x10001);          // 65537 blocks — index $10000 unaddressable
        ProDOSHardDiskCard c;
        assert(!c.loadImage(p));
    }
    {
        const std::string p = pom2test::tempPath("pom2_hdv_ragged.hdv");
        writeFile(p, std::vector<uint8_t>(kBlk + 7, 0));  // not a block multiple
        ProDOSHardDiskCard c;
        assert(!c.loadImage(p));
    }

    // ── (a) 16-bit block addressing (block >= 256 reachable) ─────────
    {
        const size_t kBlocks = 0x102;        // blocks 0..0x101
        std::vector<uint8_t> img(kBlocks * kBlk, 0x00);
        for (size_t i = 0; i < 16; ++i)
            img[0x101 * kBlk + i] = static_cast<uint8_t>(0xA0 + i);
        const std::string p = pom2test::tempPath("pom2_hdv_hiblock.hdv");
        writeFile(p, img);

        Memory mem;
        auto c = std::make_unique<ProDOSHardDiskCard>();
        assert(c->loadImage(p));
        assert(c->getBlockCount() == kBlocks);
        mem.slotBus().plug(ProDOSHardDiskCard::kDefaultSlot, std::move(c));
        mem.memWrite(0xC0D0, 0x01);          // block low  = 0x01
        mem.memWrite(0xC0D1, 0x01);          // block high = 0x01  → block 0x101
        for (size_t i = 0; i < 16; ++i)
            assert(mem.memRead(0xC0D2) == static_cast<uint8_t>(0xA0 + i));
    }

    // ── (b) .2mg with data offset != 64 ──────────────────────────────
    {
        const uint32_t off = 128;
        std::vector<uint8_t> blocks(2 * kBlk, 0x00);
        for (size_t i = 0; i < kBlk; ++i)
            blocks[kBlk + i] = static_cast<uint8_t>(i ^ 0x5A);
        auto two = twoImgHeader(2, off, static_cast<uint32_t>(blocks.size()));
        two.insert(two.end(), blocks.begin(), blocks.end());
        const std::string p = pom2test::tempPath("pom2_hdv_off128.2mg");
        writeFile(p, two);

        Memory mem;
        auto c = std::make_unique<ProDOSHardDiskCard>();
        assert(c->loadImage(p));
        assert(c->getBlockCount() == 2);
        mem.slotBus().plug(ProDOSHardDiskCard::kDefaultSlot, std::move(c));
        mem.memWrite(0xC0D0, 0x01);
        mem.memWrite(0xC0D1, 0x00);
        for (size_t i = 0; i < 16; ++i)
            assert(mem.memRead(0xC0D2) == static_cast<uint8_t>(i ^ 0x5A));
    }

    std::printf("HDV mass-storage smoke: OK\n");
    return 0;
}
