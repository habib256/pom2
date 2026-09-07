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

// ProDOS HDV card smoke test — pins:
//  - ProDOS block-device signature bytes in $Cn00 ROM
//  - driver entry offset in $CnFF
//  - basic block read protocol via $C0D0-$C0D2 soft-switch window
//
// This is a core test: no ImGui, no OpenGL.

#include "Memory.h"
#include "ProDOSHardDiskCard.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <filesystem>
#include <string>
#include <utility>
#include "TestTempPath.h"

static std::string writeTempHdv(const std::vector<uint8_t>& bytes)
{
    const std::string path = pom2test::tempPath("pom2_hdv_smoke.hdv");
    std::ofstream f(path, std::ios::binary);
    assert(f.good());
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    assert(f.good());
    return path;
}

int main()
{
    Memory mem;
    auto card = std::make_unique<ProDOSHardDiskCard>();

    // Build a tiny 2-block image with a known pattern in block 1.
    std::vector<uint8_t> hdv(2 * ProDOSHardDiskCard::kBlockBytes, 0x00);
    for (size_t i = 0; i < ProDOSHardDiskCard::kBlockBytes; ++i) {
        hdv[1 * ProDOSHardDiskCard::kBlockBytes + i] =
            static_cast<uint8_t>((i * 7u + 3u) & 0xFFu);
    }
    const std::string path = writeTempHdv(hdv);
    assert(card->loadImage(path));

    mem.slotBus().plug(ProDOSHardDiskCard::kDefaultSlot, std::move(card));

    // ProDOS signature bytes for block devices:
    // $Cn01=$20, $Cn03=$00, $Cn05=$03.
    assert(mem.memRead(0xC501) == 0x20);
    assert(mem.memRead(0xC503) == 0x00);
    assert(mem.memRead(0xC505) == 0x03);

    // $CnFF = the ProDOS driver entry OFFSET. Not a fixed constant: the page
    // is assembled from labels (SlotRomAsm.h) and $CnFF is `byteOf("driver")`,
    // so the routine may move — it did, when the shared error tail grew to
    // separate "block past the end" ($27) from "empty bay" ($28). What has to
    // hold is that the byte points at real code inside the page and that a
    // JSR there lands on the dispatch, which hdv_status_driver executes.
    {
        const uint8_t entry = mem.memRead(0xC5FF);
        assert(entry >= 0x08 && entry < 0xF0);
        assert(mem.memRead(static_cast<uint16_t>(0xC500 | entry)) == 0xA5);
    }

    // $CnFE = ProDOS device characteristics (TN.PDOS.021): bit 0 status,
    // bit 1 read, bit 2 WRITE. It read $03 — a read-only device — while the
    // ROM has always carried a working WRITE_BLOCK (bug hunt 4 #13).
    assert((mem.memRead(0xC5FE) & 0x07) == 0x07);

    // Select block 1 then stream bytes via $C0D2.
    mem.memWrite(0xC0D0, 0x01); // block low
    mem.memWrite(0xC0D1, 0x00); // block high
    for (size_t i = 0; i < 16; ++i) {
        const uint8_t b = mem.memRead(0xC0D2);
        const uint8_t want = static_cast<uint8_t>((i * 7u + 3u) & 0xFFu);
        assert(b == want);
    }

    // 2IMG (.2mg) container path: same 2-block ProDOS payload preceded by a
    // 64-byte 2IMG header. ProDOSHardDiskCard must strip the header and stream
    // the same bytes through $C0D2.
    {
        std::vector<uint8_t> two(64 + hdv.size(), 0x00);
        two[0] = '2'; two[1] = 'I'; two[2] = 'M'; two[3] = 'G';
        two[8] = 64; two[9] = 0;          // header length
        two[10] = 1; two[11] = 0;         // version 1
        two[12] = 1;                      // image format 1 = ProDOS
        const uint32_t blocks = static_cast<uint32_t>(hdv.size() / ProDOSHardDiskCard::kBlockBytes);
        two[20] = static_cast<uint8_t>(blocks & 0xFF);
        two[21] = static_cast<uint8_t>((blocks >> 8) & 0xFF);
        two[24] = 64;                     // data offset
        const uint32_t dlen = static_cast<uint32_t>(hdv.size());
        two[28] = static_cast<uint8_t>(dlen & 0xFF);
        two[29] = static_cast<uint8_t>((dlen >> 8) & 0xFF);
        two[30] = static_cast<uint8_t>((dlen >> 16) & 0xFF);
        two[31] = static_cast<uint8_t>((dlen >> 24) & 0xFF);
        std::memcpy(two.data() + 64, hdv.data(), hdv.size());

        const std::string p2 = pom2test::tempPath("pom2_hdv_smoke.2mg");
        {
            std::ofstream f(p2, std::ios::binary);
            f.write(reinterpret_cast<const char*>(two.data()),
                    static_cast<std::streamsize>(two.size()));
        }

        Memory mem2;
        auto c2 = std::make_unique<ProDOSHardDiskCard>();
        assert(c2->loadImage(p2));
        assert(c2->getBlockCount() == 2);
        mem2.slotBus().plug(ProDOSHardDiskCard::kDefaultSlot, std::move(c2));
        mem2.memWrite(0xC0D0, 0x01);
        mem2.memWrite(0xC0D1, 0x00);
        for (size_t i = 0; i < 16; ++i) {
            const uint8_t b = mem2.memRead(0xC0D2);
            const uint8_t want = static_cast<uint8_t>((i * 7u + 3u) & 0xFFu);
            assert(b == want);
        }

        // Wrong format (DOS sector order) must be rejected.
        two[12] = 0;
        {
            std::ofstream f(p2, std::ios::binary);
            f.write(reinterpret_cast<const char*>(two.data()),
                    static_cast<std::streamsize>(two.size()));
        }
        ProDOSHardDiskCard c3;
        assert(!c3.loadImage(p2));
    }

    // ── adoptImage resets the firmware cursor, exactly like loadImage ─────
    // `adoptImage` is phase 2 of the two-phase mount (pom2::mountBlockCard),
    // which is the path a GUI mount actually takes — so it is the COMMON
    // path, not the exotic one. It used to forward to the backing store and
    // nothing else, leaving the outgoing image's $C0n0/$C0n1 block select and
    // its byte offset pointed into the incoming medium: a mount landing
    // mid-transfer handed the guest the rest of a 512-byte stream from the
    // wrong offset of the wrong block (bug hunt 4 #9).
    {
        // Every byte distinct per (block, offset), so "resumed at the old
        // cursor" and "restarted at block 0 byte 0" cannot alias.
        auto makeImage = [](uint8_t tag, size_t blocks) {
            std::vector<uint8_t> v(blocks * 512u);
            for (size_t b = 0; b < blocks; ++b)
                for (size_t i = 0; i < 512u; ++i)
                    v[b * 512u + i] =
                        static_cast<uint8_t>(tag + b * 16u + (i & 0x0Fu));
            return v;
        };
        const std::string first =
            (std::filesystem::temp_directory_path() / "pom2_hdv_adopt_a.hdv")
                .string();
        const std::string second =
            (std::filesystem::temp_directory_path() / "pom2_hdv_adopt_b.hdv")
                .string();
        for (const auto& pr : { std::pair<std::string, uint8_t>{first, 0x40},
                                std::pair<std::string, uint8_t>{second, 0x80} }) {
            const auto bytes = makeImage(pr.second, 4);
            std::ofstream f(pr.first, std::ios::binary);
            f.write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        }

        Memory mem3;
        auto card = std::make_unique<ProDOSHardDiskCard>(5);
        ProDOSHardDiskCard* raw = card.get();
        assert(raw->loadImage(first));
        mem3.slotBus().plug(5, std::move(card));

        // Guest starts reading block 2 and stops three bytes in.
        mem3.memWrite(0xC0D0, 0x02);
        mem3.memWrite(0xC0D1, 0x00);
        (void)mem3.memRead(0xC0D2);
        (void)mem3.memRead(0xC0D2);
        (void)mem3.memRead(0xC0D2);

        // A mount lands. The next byte must come from block 0 offset 0 of the
        // NEW image — the cursor is part of what a mount resets.
        pom2::Block512Backing::PreparedImage prepared;
        std::string prepErr;
        assert(pom2::Block512Backing::readImageFile(second, prepared, prepErr));
        assert(raw->adoptImage(std::move(prepared)));
        const uint8_t next = mem3.memRead(0xC0D2);
        if (next != 0x80) {
            std::printf("FAIL: after adoptImage the stream resumed at $%02X, "
                        "want $80 (block 0 byte 0 of the new image) — the "
                        "outgoing image's block/byte cursor survived the "
                        "mount\n", next);
            return 1;
        }
        std::error_code rmEc;
        std::filesystem::remove(first, rmEc);
        std::filesystem::remove(second, rmEc);
    }

    std::printf("HDV card smoke: OK\n");
    return 0;
}
