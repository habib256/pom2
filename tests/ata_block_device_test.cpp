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

// Pins the MAME-faithful ATA taskfile core (AtaBlockDevice) — pure logic,
// no ROM required. Covers IDENTIFY capacity, single + multi-sector READ,
// WRITE round-trip, the sector-count==0 ⇒ 256 rule, DRQ/BSY status flow,
// and out-of-range LBA safety. P1 § Cartes de stockage MAME-fidèles.

#include "AtaBlockDevice.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using pom2::AtaBlockDevice;
using pom2::Block512Backing;

namespace {

constexpr size_t kBlk = Block512Backing::kBlockBytes;

// Deterministic per-block byte pattern.
uint8_t pat(uint32_t blk, size_t i) {
    return static_cast<uint8_t>((blk * 7u + i * 3u + 0x11u) & 0xFF);
}

std::vector<uint8_t> makeImage(uint32_t blocks) {
    std::vector<uint8_t> v(blocks * kBlk);
    for (uint32_t b = 0; b < blocks; ++b)
        for (size_t i = 0; i < kBlk; ++i)
            v[b * kBlk + i] = pat(b, i);
    return v;
}

void setLba(AtaBlockDevice& a, uint32_t lba, uint8_t count) {
    a.cs0_w(2, count);
    a.cs0_w(3, lba & 0xFF);
    a.cs0_w(4, (lba >> 8) & 0xFF);
    a.cs0_w(5, (lba >> 16) & 0xFF);
    a.cs0_w(6, 0xE0 | ((lba >> 24) & 0x0F)); // LBA mode, drive 0
}

// Pull one 512-byte sector out of the data port into dst.
void readSector(AtaBlockDevice& a, uint8_t* dst) {
    for (size_t i = 0; i < 256; ++i) {
        uint16_t w = a.cs0_r(0);
        dst[2 * i]     = static_cast<uint8_t>(w & 0xFF);
        dst[2 * i + 1] = static_cast<uint8_t>(w >> 8);
    }
}

void writeSector(AtaBlockDevice& a, const uint8_t* src) {
    for (size_t i = 0; i < 256; ++i) {
        uint16_t w = static_cast<uint16_t>(src[2 * i]) |
                     (static_cast<uint16_t>(src[2 * i + 1]) << 8);
        a.cs0_w(0, w);
    }
}

// Build a tiny write-protected 2IMG (flags bit0 = 1) on disk; returns its path.
std::string writeWp2img(uint32_t blocks) {
    const auto p = std::filesystem::temp_directory_path() / "pom2_ata_wp.2mg";
    std::vector<uint8_t> f(64 + blocks * kBlk, 0);
    std::memcpy(f.data(), "2IMG", 4);
    auto wr32 = [&](size_t o, uint32_t v) {
        f[o] = v & 0xFF; f[o + 1] = (v >> 8) & 0xFF;
        f[o + 2] = (v >> 16) & 0xFF; f[o + 3] = (v >> 24) & 0xFF;
    };
    wr32(12, 1);                       // format = ProDOS block order
    wr32(16, 1);                       // flags  = write-protected
    wr32(24, 64);                      // data offset
    wr32(28, blocks * kBlk);           // data length
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(f.data()),
            static_cast<std::streamsize>(f.size()));
    return p.string();
}

} // namespace

int main() {
    // ── IDENTIFY reports the LBA28 capacity ───────────────────────────────
    {
        AtaBlockDevice a;
        const uint32_t blocks = 100;
        assert(a.backing().loadFromBytes(makeImage(blocks), "ata-test", ""));
        a.cs0_w(7, AtaBlockDevice::kCmdIdentify);
        assert((a.cs0_r(7) & AtaBlockDevice::kStDRQ) != 0); // data ready
        uint16_t id[256];
        for (size_t i = 0; i < 256; ++i) id[i] = a.cs0_r(0);
        const uint32_t total =
            static_cast<uint32_t>(id[60]) | (static_cast<uint32_t>(id[61]) << 16);
        assert(total == blocks);
        // Words 57-58 (current capacity) must ALSO carry the count — the CFFA
        // firmware sizes its partitions from these, not 60-61. Zero here =>
        // "Could not boot partition 1 / Err $28".
        const uint32_t cur =
            static_cast<uint32_t>(id[57]) | (static_cast<uint32_t>(id[58]) << 16);
        assert(cur == blocks);
        assert(id[53] & 0x0001);                       // words 54-58 valid
        // ...and word 53 bit 0 is a CLAIM about words 54-56 (current logical
        // geometry, ATA-1 §6.2.1.6). They were left at ZERO while the flag
        // said they were valid, so a host that trusts the flag computed a
        // zero-cylinder, zero-head drive (bug hunt 4 #25).
        assert(id[55] != 0 && id[56] != 0);           // heads, sectors/track
        assert(id[54] == blocks / (static_cast<uint32_t>(id[55]) * id[56]));
        assert(id[55] == id[3] && id[56] == id[6]);   // agrees with words 1/3/6
        assert(id[49] & 0x0200);                       // LBA supported
        assert((a.cs0_r(7) & AtaBlockDevice::kStDRQ) == 0); // transfer drained
        assert(a.cs0_r(7) & AtaBlockDevice::kStDRDY);
    }

    // ── Single-sector READ matches the backing pattern ────────────────────
    {
        AtaBlockDevice a;
        assert(a.backing().loadFromBytes(makeImage(16), "r", ""));
        setLba(a, 3, 1);
        a.cs0_w(7, AtaBlockDevice::kCmdRead);
        assert(a.cs0_r(7) & AtaBlockDevice::kStDRQ);
        uint8_t buf[kBlk];
        readSector(a, buf);
        for (size_t i = 0; i < kBlk; ++i) assert(buf[i] == pat(3, i));
        assert((a.cs0_r(7) & AtaBlockDevice::kStDRQ) == 0);
    }

    // ── Multi-sector READ spans consecutive LBAs ──────────────────────────
    {
        AtaBlockDevice a;
        assert(a.backing().loadFromBytes(makeImage(16), "rm", ""));
        setLba(a, 6, 3);
        a.cs0_w(7, AtaBlockDevice::kCmdRead);
        for (uint32_t s = 0; s < 3; ++s) {
            assert(a.cs0_r(7) & AtaBlockDevice::kStDRQ);
            uint8_t buf[kBlk];
            readSector(a, buf);
            for (size_t i = 0; i < kBlk; ++i) assert(buf[i] == pat(6 + s, i));
        }
        assert((a.cs0_r(7) & AtaBlockDevice::kStDRQ) == 0); // done after 3
    }

    // ── WRITE round-trips through the backing ─────────────────────────────
    {
        AtaBlockDevice a;
        assert(a.backing().loadFromBytes(makeImage(16), "w", ""));
        uint8_t src[kBlk];
        for (size_t i = 0; i < kBlk; ++i) src[i] = static_cast<uint8_t>(0xC0 ^ i);
        setLba(a, 5, 1);
        a.cs0_w(7, AtaBlockDevice::kCmdWrite);
        assert(a.cs0_r(7) & AtaBlockDevice::kStDRQ); // host may feed data
        writeSector(a, src);
        assert((a.cs0_r(7) & AtaBlockDevice::kStDRQ) == 0);
        uint8_t back[kBlk];
        assert(a.backing().readBlock(5, back));
        for (size_t i = 0; i < kBlk; ++i) assert(back[i] == src[i]);
        // Re-read block 5 through the ATA path too.
        setLba(a, 5, 1);
        a.cs0_w(7, AtaBlockDevice::kCmdRead);
        uint8_t rd[kBlk];
        readSector(a, rd);
        for (size_t i = 0; i < kBlk; ++i) assert(rd[i] == src[i]);
        assert(a.backing().hasUnsavedChanges()); // block 5 marked dirty
    }

    // ── sectorCount == 0 means 256 sectors (DRQ persists past sector 1) ────
    {
        AtaBlockDevice a;
        assert(a.backing().loadFromBytes(makeImage(257), "c0", ""));
        setLba(a, 0, 0); // count 0 ⇒ 256
        a.cs0_w(7, AtaBlockDevice::kCmdRead);
        uint8_t buf[kBlk];
        readSector(a, buf);                          // drain sector 1
        assert(a.cs0_r(7) & AtaBlockDevice::kStDRQ); // still requesting (≠ 1 sector)
    }

    // ── Out-of-range READ is ID NOT FOUND, not a clean zero-filled sector ──
    // ATA-1 §9.1 / ATA-2 §7.2.6: IDNF ($10) = "the requested sector could not
    // be found". POM2 handed the caller 512 zeros with DRDY|DSC and no ERR —
    // so a driver walking past the end of a truncated image read an empty
    // block as real data. WRITE already refused; READ is the half that was
    // missing (bug hunt 4 #11).
    {
        AtaBlockDevice a;
        assert(a.backing().loadFromBytes(makeImage(4), "oor", ""));
        setLba(a, 1000, 1);
        a.cs0_w(7, AtaBlockDevice::kCmdRead);
        const uint8_t st = static_cast<uint8_t>(a.cs0_r(7));
        assert((st & AtaBlockDevice::kStERR) != 0);   // error flagged
        assert((st & AtaBlockDevice::kStDRQ) == 0);   // no data phase
        assert((static_cast<uint8_t>(a.cs0_r(1)) &
                AtaBlockDevice::kErrIDNF) != 0);

        // A multi-sector READ that starts valid and crosses the end fails as
        // ONE command — the caller must not get two good sectors and then
        // silence.
        setLba(a, 3, 2);
        a.cs0_w(7, AtaBlockDevice::kCmdReadMulti);
        assert((a.cs0_r(7) & AtaBlockDevice::kStERR) != 0);
        assert((a.cs0_r(7) & AtaBlockDevice::kStDRQ) == 0);

        // The LAST in-range sector still reads: the bound is `>=`, and
        // getting it backwards would break the last block of every volume.
        setLba(a, 3, 1);
        a.cs0_w(7, AtaBlockDevice::kCmdRead);
        assert((a.cs0_r(7) & AtaBlockDevice::kStERR) == 0);
        assert((a.cs0_r(7) & AtaBlockDevice::kStDRQ) != 0);
        uint8_t buf[kBlk];
        readSector(a, buf);
        for (size_t i = 0; i < kBlk; ++i) assert(buf[i] == pat(3, i));
    }

    // ── Device-select bit: the SLAVE address answers nothing ──────────────
    // Register 6 bit 4 (IDE_DEVICE_HEAD_DRV) picks master/slave on the shared
    // cable. POM2 read it NOWHERE, so the CFFA firmware's slave scan — which
    // MAME deliberately enables by patching m_rom[0x800]/[0x801] to 0x0D
    // (a2cffa.cpp device_start, copied in CffaCard::loadRom), and which the
    // firmware drives at $CCC with `LDA $05F8,Y / EOR #$10 / STA $C08E,X` —
    // found a SECOND drive that was the same medium, and a write addressed to
    // it landed on the master's image (bug hunt 4 #6). MAME's
    // `ata_hle_device::read_cs0` returns 0 for an unselected device.
    {
        AtaBlockDevice a;
        assert(a.backing().loadFromBytes(makeImage(16), "sel", ""));

        // Master: IDENTIFY works and reports the medium.
        a.cs0_w(6, 0xA0);                       // bit 4 clear = master
        a.cs0_w(7, AtaBlockDevice::kCmdIdentify);
        assert((a.cs0_r(7) & AtaBlockDevice::kStDRQ) != 0);
        for (size_t i = 0; i < 256; ++i) (void)a.cs0_r(0);

        // Slave: every register reads 0 (DRDY clear = "nobody home"), and the
        // command is not executed at all.
        a.cs0_w(6, 0xB0);                       // bit 4 set = slave
        a.cs0_w(7, AtaBlockDevice::kCmdIdentify);
        assert(a.cs0_r(7) == 0x0000);
        assert(a.cs0_r(1) == 0x0000);
        assert(a.cs1_r(6) == 0x0000);           // alternate status too

        // And a WRITE addressed to the slave must not reach the master's
        // medium. Feed a whole sector at the slave address, then check the
        // block through the backing.
        uint8_t before[kBlk];
        assert(a.backing().readBlock(3, before));
        a.cs0_w(2, 1);
        a.cs0_w(3, 3);
        a.cs0_w(4, 0);
        a.cs0_w(5, 0);
        a.cs0_w(6, 0xF0);                       // LBA mode, drive 1 (slave)
        a.cs0_w(7, AtaBlockDevice::kCmdWrite);
        for (size_t i = 0; i < 256; ++i) a.cs0_w(0, 0xDEAD);
        uint8_t after[kBlk];
        assert(a.backing().readBlock(3, after));
        for (size_t i = 0; i < kBlk; ++i) assert(after[i] == before[i]);
        assert(!a.backing().hasUnsavedChanges());

        // Back to the master and the device is there again.
        a.cs0_w(6, 0xA0);
        assert((a.cs0_r(7) & AtaBlockDevice::kStDRDY) != 0);
    }

    // ── WRITE to a write-protected device aborts (ERR), no silent success ──
    {
        AtaBlockDevice a;
        const std::string p = writeWp2img(8);
        assert(a.backing().loadImage(p));
        assert(a.backing().isWriteProtected());
        setLba(a, 2, 1);
        a.cs0_w(7, AtaBlockDevice::kCmdWrite);
        const uint8_t st = static_cast<uint8_t>(a.cs0_r(7));
        assert((st & AtaBlockDevice::kStERR) != 0);   // error flagged in Status
        assert((st & AtaBlockDevice::kStDRQ) == 0);   // data phase NOT granted
        assert((static_cast<uint8_t>(a.cs0_r(1)) & AtaBlockDevice::kErrABRT) != 0);
        assert(!a.backing().hasUnsavedChanges());      // nothing written
        std::filesystem::remove(p);
    }

    // ── Out-of-range WRITE aborts before accepting 512 bytes ──────────────
    {
        AtaBlockDevice a;
        assert(a.backing().loadFromBytes(makeImage(4), "oor-write", ""));
        setLba(a, 4, 1);                 // first block past the image
        a.cs0_w(7, AtaBlockDevice::kCmdWrite);
        const uint8_t st = static_cast<uint8_t>(a.cs0_r(7));
        assert((st & AtaBlockDevice::kStERR) != 0);
        assert((st & AtaBlockDevice::kStDRQ) == 0);
        // IDNF, not ABRT: an address off the end and a write-protected medium
        // are different faults and used to share one Error code, so a driver
        // could not tell "locked disk" from "block past the end" (ATA-1 §9.1).
        assert((static_cast<uint8_t>(a.cs0_r(1)) &
                AtaBlockDevice::kErrIDNF) != 0);
        assert(!a.backing().hasUnsavedChanges());

        // A multi-sector request that starts valid but crosses the end must
        // fail as one command, rather than partially modifying the last block.
        setLba(a, 3, 2);
        a.cs0_w(7, AtaBlockDevice::kCmdWriteMulti);
        assert((a.cs0_r(7) & AtaBlockDevice::kStERR) != 0);
        assert((a.cs0_r(7) & AtaBlockDevice::kStDRQ) == 0);
        assert(!a.backing().hasUnsavedChanges());
    }

    // ── CHS addressing (devHead bit 6 clear) ──────────────────────────────
    // MAME `ata_mass_storage_device_base::lba_address()` (atastorage.cpp:
    // 44-53): with IDE_DEVICE_HEAD_L (0x40) clear, the taskfile decodes as
    // cylinder/head/sector through the latched geometry — POM2 used to
    // decode the same registers as a raw LBA28 and read the wrong block.
    {
        AtaBlockDevice a;
        assert(a.backing().loadFromBytes(makeImage(1200), "chs", ""));

        // Default geometry = IDENTIFY page: 16 heads × 63 sectors.
        // C/H/S 1/2/5 → LBA (1×16 + 2)×63 + 5 − 1 = 1138.
        a.cs0_w(2, 1);            // sector count
        a.cs0_w(3, 5);            // sector number (1-based)
        a.cs0_w(4, 1);            // cylinder low
        a.cs0_w(5, 0);            // cylinder high
        a.cs0_w(6, 0xA0 | 2);     // head 2, bit 6 CLEAR = CHS
        a.cs0_w(7, AtaBlockDevice::kCmdRead);
        assert(a.cs0_r(7) & AtaBlockDevice::kStDRQ);
        uint8_t buf[kBlk];
        readSector(a, buf);
        for (size_t i = 0; i < kBlk; ++i) assert(buf[i] == pat(1138, i));

        // INITIALIZE DEVICE PARAMETERS ($91) re-latches the geometry
        // (MAME set_geometry, atastorage.cpp:267-269): 32 spt, 8 heads.
        // C/H/S 2/3/9 → LBA (2×8 + 3)×32 + 9 − 1 = 616.
        a.cs0_w(2, 32);           // sectors per track
        a.cs0_w(6, 0xA0 | 7);     // heads = (devHead & 0x0F) + 1 = 8
        a.cs0_w(7, AtaBlockDevice::kCmdInitParams);
        assert((a.cs0_r(7) & AtaBlockDevice::kStERR) == 0);

        a.cs0_w(2, 1);
        a.cs0_w(3, 9);
        a.cs0_w(4, 2);
        a.cs0_w(5, 0);
        a.cs0_w(6, 0xA0 | 3);
        a.cs0_w(7, AtaBlockDevice::kCmdRead);
        assert(a.cs0_r(7) & AtaBlockDevice::kStDRQ);
        readSector(a, buf);
        for (size_t i = 0; i < kBlk; ++i) assert(buf[i] == pat(616, i));
    }

    // Bound hostile/sparse HDV files before allocating their apparent size.
    {
        const auto p = std::filesystem::temp_directory_path() /
                       "pom2_oversized_sparse.hdv";
        { std::ofstream f(p, std::ios::binary); f.put('\0'); }
        std::filesystem::resize_file(p, 65u * 1024u * 1024u);
        AtaBlockDevice a;
        assert(!a.backing().loadImage(p.string()));
        std::filesystem::remove(p);
    }

    std::printf("ata_block_device_test: OK\n");
    // ── The taskfile steps across a multi-sector transfer (MAME next_sector) ──
    // A driver that reads the registers back after a READ must see where the
    // head stopped: LBA + count, count 0. They used to stand still.
    {
        AtaBlockDevice a;
        const uint32_t blocks = 100;
        assert(a.backing().loadFromBytes(makeImage(blocks), "ata-adv", ""));
        // LBA28: 3 sectors from 5 → registers say 8, count 0.
        a.cs0_w(2, 3);
        a.cs0_w(3, 5); a.cs0_w(4, 0); a.cs0_w(5, 0);
        a.cs0_w(6, 0xE0);
        a.cs0_w(7, AtaBlockDevice::kCmdRead);
        uint8_t buf[512];
        for (int i = 0; i < 3; ++i) readSector(a, buf);
        assert((a.cs0_r(3) & 0xFF) == 8 && (a.cs0_r(4) & 0xFF) == 0);
        assert((a.cs0_r(2) & 0xFF) == 0 && "sector count must count down to 0");
        assert((a.cs0_r(6) & 0x0F) == 0);
        // CHS through the default 16 × 63 geometry: sector 63 of head 0 is
        // the last of its track, so the second sector is head 1, sector 1,
        // and the registers end on head 1, sector 2.
        a.cs0_w(2, 2);
        a.cs0_w(3, 63); a.cs0_w(4, 0); a.cs0_w(5, 0);
        a.cs0_w(6, 0xA0);
        a.cs0_w(7, AtaBlockDevice::kCmdRead);
        for (int i = 0; i < 2; ++i) readSector(a, buf);
        assert((a.cs0_r(3) & 0xFF) == 2 && (a.cs0_r(6) & 0x0F) == 1);
        assert((a.cs0_r(2) & 0xFF) == 0);
        // IDENTIFY is not a sector operation: it leaves the address alone.
        a.cs0_w(3, 0x11); a.cs0_w(2, 0x22);
        a.cs0_w(7, AtaBlockDevice::kCmdIdentify);
        readSector(a, buf);
        assert((a.cs0_r(3) & 0xFF) == 0x11 && (a.cs0_r(2) & 0xFF) == 0x22);
        std::printf("ata: taskfile advances across a multi-sector transfer OK\n");
    }

    return 0;
}
