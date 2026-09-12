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

// Disk-image parser fuzz smoke — bounded, deterministic, self-contained.
//
// The image loaders are the widest untrusted-input surface POM2 has: a `.woz`
// or a `.2mg` is something a user downloads and drags in, and every byte of
// its header is attacker-chosen. `loadWoz` walks a chunk list driven by
// 32-bit lengths, indexes TRKS entries through a TMAP byte, and hands a
// caller-declared bit count to the cell walker — four separate chances to
// trust a number that came out of the file.
//
// So this feeds MUTATED containers to every loader and then USES what comes
// back. Two deliberate choices:
//
//   • Seeds are SYNTHESISED here, never read from `disks_5.4/` or `hdv/`.
//     Not one disk file is tracked by git, so a corpus-reading fuzzer would
//     silently test nothing on a fresh clone or in CI — passing for the worst
//     possible reason.
//   • Seeds are VALID containers, not random bytes. Random bytes are rejected
//     at the magic and never reach the section walker, which is where the
//     bugs would be. The tracks inside carry pseudo-random bits — the GCR
//     decode legitimately finds no sectors, and that is fine: what is under
//     test is the container arithmetic, not the sector codec.
//
// Mutations are STRUCTURE-AWARE, and that turned out to be the whole game. A
// first version scattered byte-flips over the first 2 KB and could not catch a
// deliberately removed TRKS bounds check even in 600 rounds: the fields that
// matter are four bytes each in a ~250 KB file, so blind flipping essentially
// never lands on one. `mutateWoz` instead parses the chunk list and aims at
// the numbers the loader trusts — chunk lengths, TMAP track indices, and the
// TRKS start/count/bit-count triple. With that, the same sabotage is caught in
// well under a second.
//
// A flipped bit deep inside a track payload is deliberately NOT interesting:
// it just produces undecodable GCR, which is a correct outcome rather than a
// defect.
//
// Under a plain build this catches crashes, hangs and assertion failures.
// Under `-fsanitize=address,undefined` it additionally catches every
// out-of-bounds read the loaders could be talked into. Run it that way when
// touching a parser:
//
//   cmake -B build_asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"
//   cmake --build build_asan --target test_fuzz_disk_image && ctest -R fuzz_

#include "Block512Backing.h"
#include "Disk35Image.h"
#include "DiskImage.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ─── Seed synthesis ──────────────────────────────────────────────────────

void put16(std::vector<uint8_t>& v, uint16_t x)
{ v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
void put32(std::vector<uint8_t>& v, uint32_t x)
{ for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }

/// A structurally valid WOZ2 whose TRKS blocks hold pseudo-random cells.
/// `diskType` 1 = 5.25" (DiskImage), 2 = 3.5" (Disk35Image).
std::vector<uint8_t> makeWoz(uint8_t diskType, int trackCount, std::mt19937& rng)
{
    std::vector<uint8_t> woz = { 'W', 'O', 'Z', '2', 0xFF, 0x0A, 0x0D, 0x0A };
    put32(woz, 0);                                    // CRC — unchecked

    std::vector<uint8_t> info(60, 0);
    info[0]  = 2;                                     // INFO version
    info[1]  = diskType;
    info[37] = (diskType == 2) ? 2 : 1;               // sides
    woz.insert(woz.end(), { 'I','N','F','O' });
    put32(woz, uint32_t(info.size()));
    woz.insert(woz.end(), info.begin(), info.end());

    std::vector<uint8_t> tmap(160, 0xFF);
    for (int i = 0; i < trackCount && i < 160; ++i) tmap[i] = uint8_t(i);
    woz.insert(woz.end(), { 'T','M','A','P' });
    put32(woz, 160);
    woz.insert(woz.end(), tmap.begin(), tmap.end());

    // TRKS entries reference 512-byte blocks counted from the start of file,
    // so the payload layout has to be decided before the entries are written.
    const size_t trksHeader = 8 + 160 * 8;
    const size_t dataStart  = woz.size() + trksHeader;
    size_t block = (dataStart + 511) / 512;

    std::vector<uint8_t> entries, blob;
    for (int i = 0; i < 160; ++i) {
        if (i >= trackCount) { put16(entries, 0); put16(entries, 0); put32(entries, 0); continue; }
        const uint32_t bits   = 8 * 512 * 2;          // two blocks per track
        std::vector<uint8_t> packed(bits / 8);
        for (auto& b : packed) b = uint8_t(rng());
        const uint32_t blocks = uint32_t((packed.size() + 511) / 512);
        put16(entries, uint16_t(block));
        put16(entries, uint16_t(blocks));
        put32(entries, bits);
        blob.insert(blob.end(), packed.begin(), packed.end());
        blob.resize(((blob.size() + 511) / 512) * 512, 0);
        block += blocks;
    }
    woz.insert(woz.end(), { 'T','R','K','S' });
    put32(woz, uint32_t(entries.size() + blob.size()));
    woz.insert(woz.end(), entries.begin(), entries.end());
    woz.resize(((woz.size() + 511) / 512) * 512, 0);  // align to `block`
    woz.insert(woz.end(), blob.begin(), blob.end());
    return woz;
}

/// A 2IMG wrapper around a bare ProDOS-order payload of `payload` bytes.
std::vector<uint8_t> make2img(size_t payload, std::mt19937& rng)
{
    std::vector<uint8_t> v = { '2', 'I', 'M', 'G' };
    v.insert(v.end(), { 'P','O','M','2' });           // creator
    put16(v, 64);                                     // header length
    put16(v, 1);                                      // version
    put32(v, 1);                                      // format: ProDOS order
    put32(v, 0);                                      // flags
    put32(v, uint32_t(payload / 512));                // block count
    put32(v, 64);                                     // data offset
    put32(v, uint32_t(payload));                      // data length
    v.resize(64, 0);
    const size_t base = v.size();
    v.resize(base + payload);
    for (size_t i = base; i < v.size(); ++i) v[i] = uint8_t(rng());
    return v;
}

std::vector<uint8_t> makeRaw(size_t n, std::mt19937& rng)
{
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = uint8_t(rng());
    return v;
}

struct Seed { std::vector<uint8_t> bytes; const char* ext; };

std::vector<Seed> buildSeeds(std::mt19937& rng)
{
    std::vector<Seed> s;
    s.push_back({ makeWoz(1, 35, rng), ".woz" });      // 5.25" flux
    s.push_back({ makeWoz(2, 24, rng), ".woz" });      // 3.5" flux
    s.push_back({ make2img(143360, rng), ".2mg" });    // 5.25" wrapped
    s.push_back({ makeRaw(143360, rng), ".dsk" });     // bare 5.25"
    s.push_back({ makeRaw(143360, rng), ".po" });      // bare, ProDOS order
    s.push_back({ makeRaw(232960, rng), ".nib" });     // nibble dump
    s.push_back({ makeRaw(65536, rng),  ".hdv" });     // block device
    return s;
}

// ─── Mutation ────────────────────────────────────────────────────────────

uint32_t rd32(const uint8_t* p)
{ return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

void wr32(uint8_t* p, uint32_t v)
{ for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }

/// Aim at the numbers `loadWoz` has to trust. Walking the container first is
/// what makes the difference between a fuzzer that exercises the walker and
/// one that mostly produces files rejected at the magic.
/// Returns false when `b` is not (or is no longer) a WOZ.
bool mutateWoz(std::vector<uint8_t>& b, std::mt19937& rng)
{
    if (b.size() < 12 || std::memcmp(b.data(), "WOZ", 3) != 0) return false;

    // Chunk list: name[4] + len[4] + payload.
    struct Chunk { size_t hdr, payload, len; };
    std::vector<Chunk> chunks;
    size_t tmapOff = 0, tmapLen = 0, trksOff = 0, trksLen = 0;
    for (size_t i = 12; i + 8 <= b.size();) {
        const uint32_t len = rd32(b.data() + i + 4);
        const size_t payload = i + 8;
        if (len > b.size() - payload) break;
        chunks.push_back({ i, payload, len });
        if (!std::memcmp(b.data() + i, "TMAP", 4)) { tmapOff = payload; tmapLen = len; }
        if (!std::memcmp(b.data() + i, "TRKS", 4)) { trksOff = payload; trksLen = len; }
        i = payload + len;
    }
    if (chunks.empty()) return false;

    switch (rng() % 4) {
        case 0: {   // a chunk LENGTH — truncates the walker's view of a chunk
            const Chunk& c = chunks[rng() % chunks.size()];
            const uint32_t v = (rng() % 2) ? uint32_t(rng() % 4096) : 0xFFFFFFFFu;
            wr32(b.data() + c.hdr + 4, v);
            break;
        }
        case 1: {   // TMAP entries — this is what indexes the TRKS array
            if (!tmapLen) return false;
            const int n = 1 + int(rng() % 16);
            for (int k = 0; k < n; ++k)
                b[tmapOff + (rng() % tmapLen)] = uint8_t(rng());
            break;
        }
        case 2: {   // a TRKS entry: startBlock / blockCount / bitCount
            if (trksLen < 8) return false;
            const size_t entries = std::min<size_t>(160, trksLen / 8);
            const size_t e = trksOff + (rng() % entries) * 8;
            if (e + 8 > b.size()) return false;
            wr32(b.data() + e,     uint32_t(rng()));   // start | count
            wr32(b.data() + e + 4, (rng() % 2) ? 0xFFFFFFFFu : uint32_t(rng()));
            break;
        }
        case 3: {   // truncate the file mid-payload
            std::uniform_int_distribution<size_t> d(12, b.size());
            b.resize(d(rng));
            break;
        }
    }
    return true;
}

void mutate(std::vector<uint8_t>& b, std::mt19937& rng)
{
    if (b.empty()) return;
    switch (rng() % 6) {
        case 0: {                                     // truncate
            std::uniform_int_distribution<size_t> d(1, b.size());
            b.resize(d(rng));
            break;
        }
        case 1: {                                     // smash a 32-bit field
            if (b.size() < 8) break;
            std::uniform_int_distribution<size_t> d(0, std::min<size_t>(b.size() - 4, 2048));
            const size_t off = d(rng) & ~size_t(3);
            for (int i = 0; i < 4; ++i) b[off + i] = uint8_t(rng());
            break;
        }
        case 2: {                                     // max a length — overflow bait
            if (b.size() < 8) break;
            std::uniform_int_distribution<size_t> d(0, std::min<size_t>(b.size() - 4, 2048));
            const size_t off = d(rng) & ~size_t(3);
            for (int i = 0; i < 4; ++i) b[off + i] = 0xFF;
            break;
        }
        case 3: {                                     // scatter
            std::uniform_int_distribution<size_t> d(0, b.size() - 1);
            const int n = 1 + int(rng() % 24);
            for (int i = 0; i < n; ++i) b[d(rng)] = uint8_t(rng());
            break;
        }
        case 4: {                                     // trailing garbage
            const size_t add = rng() % 1024;
            for (size_t i = 0; i < add; ++i) b.push_back(uint8_t(rng()));
            break;
        }
        case 5: {                                     // zero a span
            std::uniform_int_distribution<size_t> d(0, b.size() - 1);
            const size_t off = d(rng);
            const size_t n = std::min<size_t>(b.size() - off, 1 + (rng() % 256));
            std::memset(b.data() + off, 0, n);
            break;
        }
    }
}

// ─── Exercise ────────────────────────────────────────────────────────────

// Load AND use. A loader that returns true but leaves a half-built object is
// only caught by reading through it afterwards — which is what the LSS does
// the instant the drive spins up.
void exercise(const fs::path& p)
{
    {
        DiskImage img;
        if (img.loadFile(p.string())) {
            // The read API the LSS actually drives, including quarter-tracks
            // past the end — where a head that walks off the last phase goes.
            // Out-of-range must clamp, not index.
            for (int qt = -2; qt < 164; qt += 3) {
                const int bl = img.trackBitLength(qt);
                (void)img.trackPeriod(qt);
                (void)img.fluxEvents(qt);
                (void)img.getNextTransition(qt, 0);
                for (int b = 0; b < 8; ++b)
                    (void)img.bitAt(qt, (b * 997) % (bl > 0 ? bl : 1));
                (void)img.bitAt(qt, bl);              // one past the end
                (void)img.bitAt(qt, -1);
            }
            for (int t = -1; t < 36; t += 5)
                for (int i = 0; i < 16; ++i) (void)img.nibbleAt(t, i * 421);
            (void)img.isWriteProtected();
            (void)img.hasUnsavedChanges();
            (void)img.is13Sector();
            (void)img.sectorsPerTrack();
        }
    }
    {
        pom2::Disk35Image img;
        if (img.loadFile(p.string())) {
            uint8_t blk[512];
            for (uint32_t i = 0; i < 1600; i += 211) (void)img.readBlock(i, blk);
            (void)img.readBlock(1600, blk);           // one past the end
            (void)img.readBlock(0xFFFFFFFFu, blk);
            (void)img.isWriteProtected();
        }
    }
    {
        pom2::Block512Backing b;
        if (b.loadImage(p.string())) {
            uint8_t blk[512];
            const uint32_t n = uint32_t(b.blockCount());
            for (uint32_t i = 0; i < n && i < 256; ++i) (void)b.readBlock(i, blk);
            (void)b.readBlock(n, blk);
            (void)b.readBlock(0xFFFFFFFFu, blk);
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    // Fixed by default so a failure reproduces exactly; overridable for a
    // longer soak (`test_fuzz_disk_image 99 20000`) when touching a loader.
    const unsigned seed  = (argc > 1) ? unsigned(std::stoul(argv[1])) : 20260820u;
    const int      iters = (argc > 2) ? std::stoi(argv[2]) : 600;

    std::mt19937 rng(seed);
    const std::vector<Seed> seeds = buildSeeds(rng);
    assert(!seeds.empty());

    // Unique per process: ctest runs the suite with -j, and a shared temp name
    // would let two tests race on the same file.
    const fs::path dir = fs::temp_directory_path() /
        ("pom2_fuzz_disk_" + std::to_string(seed));
    std::error_code ec;
    fs::create_directories(dir, ec);

    for (int i = 0; i < iters; ++i) {
        const Seed& s = seeds[rng() % seeds.size()];
        std::vector<uint8_t> b = s.bytes;
        const int rounds = 1 + int(rng() % 3);
        for (int r = 0; r < rounds; ++r) {
            // Structure-aware for a WOZ most of the time, generic otherwise —
            // the generic pass still matters, since it is what produces the
            // ragged sizes and stray magics the dispatcher has to reject.
            if (!(rng() % 4) || !mutateWoz(b, rng)) mutate(b, rng);
        }

        // Keep the extension: the loaders dispatch on it.
        const fs::path out = dir / (std::string("m") + s.ext);
        {
            std::ofstream f(out, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(b.data()),
                    std::streamsize(b.size()));
        }
        exercise(out);
        fs::remove(out, ec);
    }

    fs::remove_all(dir, ec);
    std::printf("fuzz_disk_image: %d mutants survived (seed %u)\n", iters, seed);
    return 0;
}
