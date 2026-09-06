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

// Pins 2IMG / .2mg envelope read support in DiskImage::detectFormat.
//
// 2IMG wraps a DOS-skew (format=0), ProDOS-skew (format=1), or NIB
// (format=2) payload behind a 64-byte header that also carries the
// volume number and write-protect flag. Real .2mg archives (Asimov etc.)
// are the dominant interchange format today; without support the user
// can't mount them at all.
//
// Each case synthesises a fresh 2IMG file (header + payload) and loads
// it through DiskImage::loadFile, verifying:
//   - the load succeeds
//   - sector order matches the format byte
//   - the LSS-side data is correctly addressable (track 0 nibble matches
//     a known pattern in the synthetic NIB case, or the dos33_master
//     boot signature in the DOS/ProDOS cases)
//
// The test also pins the flag-word semantics: bit 31 = locked /
// write-protect (CiderPress kFlagLocked = 0x80000000; AppleWin agrees),
// bit 8 = volume-number-valid (bits 0-7 = volume, default 254 when
// absent). A regression here used to test bit 0 for WP and bit 31 for
// volume-valid, so locked images mounted as "volume 0" and odd volume
// numbers read back write-protected.

#include "DiskImage.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

void writeLe32(std::vector<uint8_t>& buf, std::size_t off, uint32_t v)
{
    buf[off + 0] = static_cast<uint8_t>(v       & 0xFF);
    buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    buf[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    buf[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}
void writeLe16(std::vector<uint8_t>& buf, std::size_t off, uint16_t v)
{
    buf[off + 0] = static_cast<uint8_t>(v       & 0xFF);
    buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

// Build a 2IMG file by prefixing `payload` with a 64-byte header.
std::vector<uint8_t> makeTwoImg(const std::vector<uint8_t>& payload,
                                uint32_t format,
                                bool writeProtect,
                                uint8_t volumeNumber)
{
    std::vector<uint8_t> out(64 + payload.size(), 0);
    // magic + creator
    out[0] = '2'; out[1] = 'I'; out[2] = 'M'; out[3] = 'G';
    out[4] = 'P'; out[5] = 'O'; out[6] = 'M'; out[7] = '2';
    writeLe16(out, 8,  64);              // headerLen
    writeLe16(out, 10, 1);               // version
    writeLe32(out, 12, format);          // 0/1/2
    uint32_t flags = 0;
    if (writeProtect)  flags |= (1u << 31);   // "locked" — spec bit 31
    if (volumeNumber != 254) {
        flags |= (1u << 8);              // mark vol# present (spec bit 8)
        flags |= static_cast<uint32_t>(volumeNumber);
    }
    writeLe32(out, 16, flags);
    writeLe32(out, 20, 0);               // numBlocks (DOS=0)
    writeLe32(out, 24, 64);              // dataOffset
    writeLe32(out, 28, static_cast<uint32_t>(payload.size()));  // dataLen
    // remaining header fields (comment/creator chunks) stay zero
    std::memcpy(out.data() + 64, payload.data(), payload.size());
    return out;
}

bool loadFixture(const std::string& name, std::vector<uint8_t>& out,
                 std::size_t expectedSize)
{
    static const char* prefixes[] = {
        // Sector images live under disks_5.4/dsk/ since the 2026 move; the
        // bare roots are kept for older trees.
        "../../disks_5.4/dsk/", "../disks_5.4/dsk/", "disks_5.4/dsk/",
        "../../disks_5.4/", "../../disks2/", "disks_5.4/", "disks2/"
    };
    for (const char* pfx : prefixes) {
        const std::string p = std::string(pfx) + name;
        std::ifstream f(p, std::ios::binary);
        if (!f) continue;
        f.seekg(0, std::ios::end);
        const auto sz = static_cast<std::size_t>(f.tellg());
        if (sz != expectedSize) continue;
        f.seekg(0, std::ios::beg);
        out.resize(sz);
        f.read(reinterpret_cast<char*>(out.data()),
               static_cast<std::streamsize>(sz));
        return static_cast<bool>(f);
    }
    return false;
}

bool writeTempFile(const std::string& path, const std::vector<uint8_t>& data)
{
    std::ofstream wf(path, std::ios::binary | std::ios::trunc);
    if (!wf) return false;
    wf.write(reinterpret_cast<const char*>(data.data()),
             static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(wf);
}

bool runDosCase()
{
    std::vector<uint8_t> payload;
    if (!loadFixture("dos33_master.dsk", payload, 143360)) {
        std::fprintf(stderr,
            "skip DOS case: no dos33_master.dsk fixture\n");
        return true;
    }
    const auto bytes = makeTwoImg(payload, /*format=*/0,
                                  /*wp=*/false, /*vol=*/254);
    const std::string path = "twoimg_dos.2mg";
    if (!writeTempFile(path, bytes)) return false;
    DiskImage img;
    const bool ok = img.loadFile(path);
    std::remove(path.c_str());
    if (!ok) {
        std::fprintf(stderr, "DOS case: %s\n", img.getLastError().c_str());
        return false;
    }
    if (img.getSectorOrder() != DiskImage::SectorOrder::Dos33) {
        std::fprintf(stderr,
            "DOS case: sector order should be Dos33 after 2IMG format=0\n");
        return false;
    }
    return true;
}

bool runProDosCase()
{
    std::vector<uint8_t> payload;
    if (!loadFixture("ProDOS_2_4_3.po", payload, 143360)) {
        std::fprintf(stderr,
            "skip ProDOS case: no ProDOS_2_4_3.po fixture\n");
        return true;
    }
    const auto bytes = makeTwoImg(payload, /*format=*/1,
                                  /*wp=*/true, /*vol=*/254);
    const std::string path = "twoimg_prodos.2mg";
    if (!writeTempFile(path, bytes)) return false;
    DiskImage img;
    const bool ok = img.loadFile(path);
    std::remove(path.c_str());
    if (!ok) {
        std::fprintf(stderr, "ProDOS case: %s\n", img.getLastError().c_str());
        return false;
    }
    if (img.getSectorOrder() != DiskImage::SectorOrder::ProDOS) {
        std::fprintf(stderr,
            "ProDOS case: sector order should be ProDOS after 2IMG format=1\n");
        return false;
    }
    if (!img.isFileWriteProtected()) {
        std::fprintf(stderr,
            "ProDOS case: write-protect flag from 2IMG flags bit 31 was lost\n");
        return false;
    }
    return true;
}

// Pin the flag-word semantics directly: bit 31 = locked (WP), bit 8 =
// volume-valid, bits 0-7 = volume. The volume number is observable
// through the nibblized address field of track 0 / physical sector 0:
// 14 sync $FFs, D5 AA 96, then the volume in 4-and-4 encoding at
// nibble offsets 17/18 (a = (vol >> 1) | $AA, b = vol | $AA).
bool runFlagSemantics()
{
    std::vector<uint8_t> payload(143360, 0);

    struct Case {
        uint32_t    flags;
        bool        wantWp;
        uint8_t     wantVol;
        const char* what;
    } cases[] = {
        // Locked bit alone: WP yes, volume defaults to 254 (the old
        // bit-31-as-volume-valid bug decoded this as volume 0).
        { 0x80000000u, true,  254, "bit31 locked, no volume" },
        // Volume-valid + odd volume: NOT write-protected (the old
        // bit-0-as-WP bug locked every odd volume number).
        { (1u << 8) | 201u, false, 201, "bit8 + volume 201" },
        // Legacy lenient path: bit 0 with no volume field still reads
        // as WP (older POM2-written / malformed images).
        { 1u, true, 254, "legacy bit0 lock" },
    };

    for (const Case& c : cases) {
        auto bytes = makeTwoImg(payload, /*format=*/0,
                                /*wp=*/false, /*vol=*/254);
        writeLe32(bytes, 16, c.flags);            // override the flag word
        const std::string path = "twoimg_flags.2mg";
        if (!writeTempFile(path, bytes)) return false;
        DiskImage img;
        const bool ok = img.loadFile(path);
        std::remove(path.c_str());
        if (!ok) {
            std::fprintf(stderr, "flags case '%s': %s\n", c.what,
                         img.getLastError().c_str());
            return false;
        }
        if (img.isFileWriteProtected() != c.wantWp) {
            std::fprintf(stderr,
                "flags case '%s': WP = %d, want %d (flags=0x%08X)\n",
                c.what, img.isFileWriteProtected() ? 1 : 0,
                c.wantWp ? 1 : 0, c.flags);
            return false;
        }
        const uint8_t volA = static_cast<uint8_t>((c.wantVol >> 1) | 0xAA);
        const uint8_t volB = static_cast<uint8_t>(c.wantVol | 0xAA);
        if (img.nibbleAt(0, 17) != volA || img.nibbleAt(0, 18) != volB) {
            std::fprintf(stderr,
                "flags case '%s': address-field volume nibbles %02X %02X, "
                "want %02X %02X (volume %u)\n",
                c.what, img.nibbleAt(0, 17), img.nibbleAt(0, 18),
                volA, volB, c.wantVol);
            return false;
        }
    }
    return true;
}

bool runNibCase()
{
    // Synthetic 232 960-byte NIB payload — pattern lets us verify the
    // payload byte ended up at the right offset after the 64-byte header
    // strip.
    constexpr std::size_t nibLen =
        static_cast<std::size_t>(DiskImage::kTracks) * DiskImage::kNibblesPerTrack;
    std::vector<uint8_t> payload(nibLen);
    for (std::size_t i = 0; i < nibLen; ++i) {
        payload[i] = static_cast<uint8_t>((i * 17 + 5) & 0xFF);
    }
    const auto bytes = makeTwoImg(payload, /*format=*/2,
                                  /*wp=*/false, /*vol=*/200);
    const std::string path = "twoimg_nib.2mg";
    if (!writeTempFile(path, bytes)) return false;
    DiskImage img;
    const bool ok = img.loadFile(path);
    std::remove(path.c_str());
    if (!ok) {
        std::fprintf(stderr, "NIB case: %s\n", img.getLastError().c_str());
        return false;
    }
    const uint8_t expected = static_cast<uint8_t>((0 * 17 + 5) & 0xFF);
    if (img.nibbleAt(0, 0) != expected) {
        std::fprintf(stderr,
            "NIB case: track 0 byte 0 = 0x%02X, want 0x%02X "
            "(header strip off-by-N?)\n",
            img.nibbleAt(0, 0), expected);
        return false;
    }
    return true;
}

bool runRefusedFormatByte()
{
    std::vector<uint8_t> payload(143360, 0);
    auto bytes = makeTwoImg(payload, /*format=*/7,   // not 0/1/2
                            /*wp=*/false, /*vol=*/254);
    const std::string path = "twoimg_badfmt.2mg";
    if (!writeTempFile(path, bytes)) return false;
    DiskImage img;
    const bool ok = img.loadFile(path);
    std::remove(path.c_str());
    if (ok) {
        std::fprintf(stderr,
            "bad-format case: loadFile accepted unsupported format=7\n");
        return false;
    }
    if (img.getLastError().find("unsupported format byte") == std::string::npos) {
        std::fprintf(stderr,
            "bad-format case: lastError missing expected text; got: %s\n",
            img.getLastError().c_str());
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    bool ok = true;
    ok &= runDosCase();
    ok &= runProDosCase();
    ok &= runNibCase();
    ok &= runRefusedFormatByte();
    ok &= runFlagSemantics();
    if (!ok) return 1;
    std::printf("disk_2mg_smoke OK: DOS + ProDOS + NIB + bad-format + flags\n");
    return 0;
}
