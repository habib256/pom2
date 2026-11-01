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

// Synthetic Sony 800K WOZ2 fixture, shared by the tests that need one.
//
// Extracted from `woz35_load_test.cpp` on 2026-09-05, when the commercial
// 3.5" WOZ images left the repository (TODO.md § G1) and
// `storage_coordinator_test` — which had been copying "The Oregon Trail
// 800K.woz" — aborted on the missing file. A core test must not depend on a
// disk POM2 is not allowed to redistribute.
//
// The encoder here is deliberately a SECOND, INDEPENDENT implementation,
// written against MAME `flopimg.cpp:2017 build_mac_track_gcr` rather than
// reusing POM2's `Sony35Gcr`: a fixture that shared the decoder's tables
// could not catch a bad table, and a table is exactly the sort of thing
// transcribed wrong once and believed forever. Keep it that way — do not
// "de-duplicate" it against `src/`.

#ifndef POM2_TESTS_WOZ35FIXTURE_H
#define POM2_TESTS_WOZ35FIXTURE_H

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

namespace pom2 {
namespace test {

// ─── An independent Sony 800K GCR encoder (MAME build_mac_track_gcr) ──────

inline constexpr uint8_t kFw[0x40] = {
    0x96, 0x97, 0x9a, 0x9b, 0x9d, 0x9e, 0x9f, 0xa6,
    0xa7, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb2, 0xb3,
    0xb4, 0xb5, 0xb6, 0xb7, 0xb9, 0xba, 0xbb, 0xbc,
    0xbd, 0xbe, 0xbf, 0xcb, 0xcd, 0xce, 0xcf, 0xd3,
    0xd6, 0xd7, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde,
    0xdf, 0xe5, 0xe6, 0xe7, 0xe9, 0xea, 0xeb, 0xec,
    0xed, 0xee, 0xef, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6,
    0xf7, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

inline int sectorsFor(int track) { return 12 - (track / 16); }

inline int blockIndex(int track, int head, int sec)
{
    int idx = 0;
    for (int t = 0; t < track; ++t) idx += 2 * sectorsFor(t);
    return idx + head * sectorsFor(track) + sec;
}

struct BitWriter {
    std::vector<uint8_t> bits;                       // one entry per cell
    void raw(int n, uint32_t v) {
        for (int i = n - 1; i >= 0; --i) bits.push_back((v >> i) & 1);
    }
};

inline uint32_t enc3(uint8_t va, uint8_t vb, uint8_t vc)
{
    return (uint32_t(kFw[((va >> 2) & 0x30) | ((vb >> 4) & 0x0c) |
                        ((vc >> 6) & 0x03)]) << 24) |
           (uint32_t(kFw[va & 0x3f]) << 16) |
           (uint32_t(kFw[vb & 0x3f]) <<  8) |
           (uint32_t(kFw[vc & 0x3f]));
}

/// Encode one (track, head) as a cell stream. `sectorBytes` is
/// sectorsFor(track) * 512, in LOGICAL sector order.
inline std::vector<uint8_t> encodeTrack(int track, int head, const uint8_t* sectorBytes)
{
    BitWriter w;
    const int sectors = sectorsFor(track);
    for (int s = 0; s < sectors; ++s) {
        for (int i = 0; i < 8; ++i) { w.raw(24, 0xff3fcf); w.raw(24, 0xf3fcff); }

        w.raw(24, 0xd5aa96);
        const uint8_t tr  = uint8_t(track);
        const uint8_t sec = uint8_t(s);
        const uint8_t sid = uint8_t(((tr & 0x40) ? 1 : 0) | (head ? 0x20 : 0));
        const uint8_t fmt = 0x22;
        const uint8_t chk = uint8_t(tr ^ sec ^ sid ^ fmt);
        w.raw(8, kFw[tr  & 0x3f]);
        w.raw(8, kFw[sec & 0x3f]);
        w.raw(8, kFw[sid & 0x3f]);
        w.raw(8, kFw[fmt & 0x3f]);
        w.raw(8, kFw[chk & 0x3f]);
        w.raw(24, 0xdeaaff);

        w.raw(24, 0xff3fcf); w.raw(24, 0xf3fcff);

        w.raw(24, 0xd5aaad);
        w.raw(8, kFw[sec & 0x3f]);

        uint8_t tagged[12 + 512 + 1] = {};
        std::memcpy(tagged + 12, sectorBytes + s * 512, 512);

        uint8_t ca = 0, cb = 0, cc = 0;
        for (int i = 0; i < 175; ++i) {
            const uint8_t va = tagged[3*i], vb = tagged[3*i+1];
            const uint8_t vc = (i != 174) ? tagged[3*i+2] : 0;
            cc = uint8_t((cc << 1) | (cc >> 7));
            const uint16_t suma = uint16_t(ca + va + (cc & 1));
            ca = uint8_t(suma);
            const uint8_t vaX = uint8_t(va ^ cc);
            const uint16_t sumb = uint16_t(cb + vb + (suma >> 8));
            cb = uint8_t(sumb);
            const uint8_t vbX = uint8_t(vb ^ ca);
            if (i != 174) cc = uint8_t(cc + vc + (sumb >> 8));
            const uint8_t vcX = uint8_t(vc ^ cb);
            const uint32_t nb = (i != 174) ? 32u : 24u;
            w.raw(int(nb), enc3(vaX, vbX, vcX) >> (32 - nb));
        }
        w.raw(32, enc3(ca, cb, cc));
        w.raw(32, 0xdeaaffff);
    }
    return w.bits;
}

inline void put16(std::vector<uint8_t>& v, uint16_t x)
{ v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
inline void put32(std::vector<uint8_t>& v, uint32_t x)
{ for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }

/// Wrap per-(track,head) cell streams in a WOZ2 container.
// `sides` writes INFO+37, the field the WOZ 2 spec uses for sidedness ("A 3.5
// disk can be 1 or 2") — Disk Type at +1 says only 5.25 vs 3.5.
inline std::vector<uint8_t> buildWoz(const std::vector<std::vector<uint8_t>>& tracks,
                              uint8_t diskType = 2, char version = '2',
                              uint8_t sides = 2)
{
    std::vector<uint8_t> woz = { 'W', 'O', 'Z', uint8_t(version),
                                 0xFF, 0x0A, 0x0D, 0x0A };
    put32(woz, 0);                                   // CRC (unchecked)

    std::vector<uint8_t> info(60, 0);
    info[0] = 2;                                     // INFO version
    info[1] = diskType;
    info[37] = sides;                                // WOZ 2 INFO+37 Disk Sides
    woz.insert(woz.end(), { 'I','N','F','O' }); put32(woz, uint32_t(info.size()));
    woz.insert(woz.end(), info.begin(), info.end());

    std::vector<uint8_t> tmap(160, 0xFF);
    for (size_t i = 0; i < tracks.size() && i < 160; ++i)
        if (!tracks[i].empty()) tmap[i] = uint8_t(i);
    woz.insert(woz.end(), { 'T','M','A','P' }); put32(woz, 160);
    woz.insert(woz.end(), tmap.begin(), tmap.end());

    // TRKS: 160 entries, then the bit blocks. Entries reference 512-byte
    // blocks counted from the START OF FILE, so lay the data out first.
    std::vector<uint8_t> entries, blob;
    const size_t trksHeaderBytes = 8 + 160 * 8;
    const size_t dataStart = woz.size() + trksHeaderBytes;
    size_t firstBlock = (dataStart + 511) / 512;
    // Pad so the first bit block lands on a 512 boundary.
    for (size_t i = 0; i < 160; ++i) {
        if (i >= tracks.size() || tracks[i].empty()) {
            put16(entries, 0); put16(entries, 0); put32(entries, 0);
            continue;
        }
        const uint32_t nbits = uint32_t(tracks[i].size());
        std::vector<uint8_t> packed((nbits + 7) / 8, 0);
        for (uint32_t b = 0; b < nbits; ++b)
            if (tracks[i][b]) packed[b >> 3] |= uint8_t(0x80 >> (b & 7));
        const uint32_t blocks = uint32_t((packed.size() + 511) / 512);
        put16(entries, uint16_t(firstBlock));
        put16(entries, uint16_t(blocks));
        put32(entries, nbits);
        packed.resize(size_t(blocks) * 512, 0);
        blob.insert(blob.end(), packed.begin(), packed.end());
        firstBlock += blocks;
    }
    woz.insert(woz.end(), { 'T','R','K','S' });
    put32(woz, uint32_t(entries.size() + blob.size()));
    woz.insert(woz.end(), entries.begin(), entries.end());
    woz.resize(((woz.size() + 511) / 512) * 512, 0);   // pad to the block grid
    woz.insert(woz.end(), blob.begin(), blob.end());
    return woz;
}

inline std::filesystem::path writeTemp(const char* name, const std::vector<uint8_t>& bytes)
{
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            std::streamsize(bytes.size()));
    return p;
}

/// A recognisable payload for block `bi`.
inline void fillBlock(uint8_t* out, int bi)
{
    for (int i = 0; i < 512; ++i)
        out[i] = uint8_t((bi * 7 + i * 3 + (i >> 4)) & 0xFF);
}

/// Build a mountable 800K WOZ2 carrying real GCR on tracks 0 and 16 (a zone
/// boundary: 12 sectors per track becomes 11), both heads. Every other track
/// is absent from the TMAP, which `Disk35Image` reads as unformatted — enough
/// for any test that needs *a* 3.5" WOZ rather than a particular disk.
/// Returns the path written under the system temp directory.
inline std::filesystem::path writeSyntheticWoz35(const char* name)
{
    std::vector<std::vector<uint8_t>> tracks(160);
    for (int t : { 0, 16 })
        for (int head = 0; head < 2; ++head) {
            const int n = sectorsFor(t);
            std::vector<uint8_t> payload(std::size_t(n) * 512);
            for (int s = 0; s < n; ++s)
                fillBlock(payload.data() + s * 512, blockIndex(t, head, s));
            tracks[std::size_t(t * 2 + head)] =
                encodeTrack(t, head, payload.data());
        }
    return writeTemp(name, buildWoz(tracks));
}

}  // namespace test
}  // namespace pom2

#endif  // POM2_TESTS_WOZ35FIXTURE_H
