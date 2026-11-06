// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WozRecover — see the header.

#include "WozRecover.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace pom2 {
namespace woz {
namespace {

constexpr std::size_t kHeader      = 12;
constexpr std::size_t kInfoData    = 20;     // fixed positions (WOZ reference)
constexpr std::size_t kTmapData    = 88;
constexpr std::size_t kTrksData    = 256;
constexpr std::size_t kTmapEntries = 160;
constexpr std::size_t kWoz1Entry   = 6656;
constexpr std::size_t kWoz1Bits    = 6646;
/// A file is padded back to its declared length only up to this much: a
/// length field that asks for more is itself the damage.
constexpr std::size_t kMaxPad      = 4u * 1024u * 1024u;

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
void wr32(uint8_t* p, uint32_t v)
{
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// [B] Beneath Apple DOS, ch. 3 — the 64 disk bytes of 6-and-2 and the 32 of
//     5-and-3, in value order. -1 = not a legal disk byte.
struct Tables {
    int8_t six[256];
    int8_t five[256];
    Tables()
    {
        static const uint8_t k62[64] = {
            0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6, 0xA7, 0xAB, 0xAC, 0xAD, 0xAE,
            0xAF, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE,
            0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3, 0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD,
            0xDE, 0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF2,
            0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF };
        static const uint8_t k53[32] = {
            0xAB, 0xAD, 0xAE, 0xAF, 0xB5, 0xB6, 0xB7, 0xBA, 0xBB, 0xBD, 0xBE, 0xBF, 0xD6,
            0xD7, 0xDA, 0xDB, 0xDD, 0xDE, 0xDF, 0xEA, 0xEB, 0xED, 0xEE, 0xEF, 0xF5, 0xF6,
            0xF7, 0xFA, 0xFB, 0xFD, 0xFE, 0xFF };
        std::memset(six, -1, sizeof six);
        std::memset(five, -1, sizeof five);
        for (int i = 0; i < 64; ++i) six[k62[i]] = static_cast<int8_t>(i);
        for (int i = 0; i < 32; ++i) five[k53[i]] = static_cast<int8_t>(i);
    }
};
const Tables& tables()
{
    static const Tables t;
    return t;
}

/// The track as the drive's shift register delivers it: a byte is complete
/// when its high bit arrives. Two revolutions, so a field that straddles the
/// end of the stream is seen whole.
std::vector<uint8_t> nibbles(const uint8_t* bits, uint32_t bitCount)
{
    std::vector<uint8_t> out;
    if (bitCount == 0) return out;
    out.reserve(bitCount / 4 + 16);
    uint8_t latch = 0;
    for (uint32_t i = 0; i < bitCount * 2; ++i) {
        const uint32_t b = i % bitCount;
        latch = static_cast<uint8_t>((latch << 1) | ((bits[b >> 3] >> (7 - (b & 7))) & 1));
        if (latch & 0x80) { out.push_back(latch); latch = 0; }
    }
    return out;
}

int fourAndFour(const uint8_t* p) { return ((p[0] << 1) | 1) & p[1]; }

/// True when the data field at `d` (just past D5 AA AD) holds `count` legal
/// disk bytes plus a checksum, and they XOR to zero.
bool dataVerifies(const std::vector<uint8_t>& n, std::size_t d, int count, const int8_t* table)
{
    if (d + static_cast<std::size_t>(count) + 1 > n.size()) return false;
    int x = 0;
    for (int i = 0; i <= count; ++i) {
        const int v = table[n[d + static_cast<std::size_t>(i)]];
        if (v < 0) return false;
        x ^= v;
    }
    return x == 0;
}

void analyse(const uint8_t* bits, uint32_t bitCount, int wholeTrack, TrackReport& r)
{
    const std::vector<uint8_t> n = nibbles(bits, bitCount);
    const Tables& t = tables();
    for (std::size_t i = 0; i + 11 < n.size(); ++i) {
        if (n[i] != 0xD5 || n[i + 1] != 0xAA) continue;
        const bool sixteen = n[i + 2] == 0x96;
        const bool thirteen = n[i + 2] == 0xB5;
        if (!sixteen && !thirteen) continue;
        const int volume = fourAndFour(&n[i + 3]), track = fourAndFour(&n[i + 5]),
                  sector = fourAndFour(&n[i + 7]), sum = fourAndFour(&n[i + 9]);
        if ((volume ^ track ^ sector) != sum) continue;          // not an address field
        if (sector >= (thirteen ? 13 : 16)) continue;
        ++r.addressFields;
        if (thirteen) r.thirteenSector = true;
        if (track != wholeTrack) r.wrongTrackNumber = true;

        // The data field follows within the gap: DOS writes it 5 to 10 sync
        // bytes after the address epilogue; allow generously for odd gaps.
        bool good = false;
        const std::size_t limit = std::min(n.size(), i + 11 + 64);
        for (std::size_t d = i + 11; d + 3 < limit; ++d) {
            if (n[d] == 0xD5 && n[d + 1] == 0xAA && n[d + 2] == 0xAD) {
                good = thirteen ? dataVerifies(n, d + 3, 410, t.five)
                                : dataVerifies(n, d + 3, 342, t.six);
                break;
            }
        }
        const uint32_t bit = 1u << sector;
        if (good) { r.sectorsGood |= bit; r.sectorsBad &= ~bit; }
        else if (!(r.sectorsGood & bit)) r.sectorsBad |= bit;     // a later copy may still verify
    }
}

struct Layout {
    std::size_t info = 0, infoLen = 0, tmap = 0, trks = 0, trksLen = 0;
    bool found = false;
    bool usedFixedPositions = false;
    bool chunkWalkBroken = false;
};

bool isChunk(const std::vector<uint8_t>& f, std::size_t at, const char* id)
{
    return at + 8 <= f.size() && std::memcmp(f.data() + at, id, 4) == 0;
}

/// The chunk walk the reference prescribes; if it does not find all three
/// primary chunks, the fixed positions the same reference guarantees.
Layout locate(const std::vector<uint8_t>& f)
{
    Layout l;
    std::size_t at = kHeader;
    bool i = false, m = false, k = false;
    while (at + 8 <= f.size()) {
        const std::size_t len = rd32(f.data() + at + 4);
        const std::size_t data = at + 8;
        if (isChunk(f, at, "INFO")) { l.info = data; l.infoLen = len; i = true; }
        else if (isChunk(f, at, "TMAP")) { l.tmap = data; m = true; }
        else if (isChunk(f, at, "TRKS")) { l.trks = data; l.trksLen = len; k = true; }
        if (len > f.size() - data) { l.chunkWalkBroken = !(i && m && k); break; }
        at = data + len;
    }
    if (i && m && k) { l.found = true; return l; }
    // Fixed positions: believable only if the IDs are where they should be.
    if (isChunk(f, kInfoData - 8, "INFO") && isChunk(f, kTmapData - 8, "TMAP") &&
        isChunk(f, kTrksData - 8, "TRKS")) {
        l.info = kInfoData; l.infoLen = 60;
        l.tmap = kTmapData;
        l.trks = kTrksData; l.trksLen = rd32(f.data() + kTrksData - 4);
        l.found = true;
        l.usedFixedPositions = true;
    }
    return l;
}

/// The work, on a copy. `apply` false leaves `f` as it came (inspect).
bool process(std::vector<uint8_t>& f, Report& rep, std::string& error, bool apply)
{
    rep = Report{};
    if (f.size() < kHeader || std::memcmp(f.data(), "WOZ", 3) != 0 ||
        (f[3] != '1' && f[3] != '2')) {
        error = "not a WOZ file (no WOZ1 / WOZ2 signature)";
        return false;
    }
    rep.isWoz   = true;
    rep.version = f[3] - '0';
    rep.crcStated   = rd32(f.data() + 8);
    rep.crcComputed = crc32(f.data() + kHeader, f.size() - kHeader);
    rep.crcWasWrong = rep.crcStated != 0 && rep.crcStated != rep.crcComputed;

    const Layout l = locate(f);
    if (!l.found) {
        error = "no INFO, TMAP and TRKS chunks to work from";
        return false;
    }
    if (l.usedFixedPositions)
        rep.repairs.push_back("the chunk chain is broken; INFO, TMAP and TRKS were taken "
                              "from their fixed positions");
    if (l.tmap + kTmapEntries > f.size() || l.info + 2 > f.size()) {
        error = "the file ends inside its INFO or TMAP chunk";
        return false;
    }
    rep.diskType = f[l.info + 1];
    const bool five25 = rep.diskType == 1;

    // A file cut short: put back the length its TRKS chunk declares, so the
    // container is whole again. The bits that were there are not — every
    // track reaching into the padding is unmapped below.
    const std::size_t originalSize = f.size();
    if (l.trksLen > f.size() - l.trks) {
        const std::size_t missing = l.trksLen - (f.size() - l.trks);
        if (missing <= kMaxPad) {
            rep.repairs.push_back("the file is " + std::to_string(missing) +
                                  " bytes short of its TRKS chunk; padded with zeros");
            f.resize(l.trks + l.trksLen, 0);
        } else {
            rep.repairs.push_back("the TRKS chunk declares more data than a disk holds; "
                                  "its length was cut to the file's");
            wr32(f.data() + l.trks - 4, static_cast<uint32_t>(f.size() - l.trks));
        }
    }

    // Where each record sits on the disk. A track is usually mapped at its
    // own position AND at the quarter-tracks either side (the head reads it
    // from there too), so the lowest entry is not the track's position: the
    // whole-track entry is, when there is one.
    int firstQt[kTmapEntries];
    std::fill(std::begin(firstQt), std::end(firstQt), -1);
    for (std::size_t qt = 0; qt < kTmapEntries; ++qt) {
        const uint8_t idx = f[l.tmap + qt];
        if (idx >= kTmapEntries) continue;
        if (firstQt[idx] < 0 || (qt % 4 == 0 && firstQt[idx] % 4 != 0))
            firstQt[idx] = static_cast<int>(qt);
    }

    for (std::size_t idx = 0; idx < kTmapEntries; ++idx) {
        if (firstQt[idx] < 0) continue;                     // nothing maps it
        TrackReport t;
        t.trksIndex = static_cast<int>(idx);
        t.firstQuarterTrack = firstQt[idx];

        std::size_t dataAt = 0, dataBytes = 0, recordAt = 0;
        if (rep.version == 2) {
            recordAt = l.trks + idx * 8;
            if (recordAt + 8 > f.size()) { t.kept = false; t.note = "its record is past the end of the file"; }
            else {
                const std::size_t block = rd16(f.data() + recordAt);
                const std::size_t count = rd16(f.data() + recordAt + 2);
                t.bitCount = rd32(f.data() + recordAt + 4);
                dataAt     = block * 512;
                dataBytes  = count * 512;
                if (block < 3 || count == 0) { t.kept = false; t.note = "its record is empty or points into the header"; }
                else if (dataAt + dataBytes > originalSize) { t.kept = false; t.note = "its bits lie past the end of the file"; }
                else if (t.bitCount == 0 || (static_cast<std::size_t>(t.bitCount) + 7) / 8 > dataBytes) {
                    t.kept = false;
                    t.note = "its bit count does not fit its blocks";
                }
            }
        } else {
            recordAt = l.trks + idx * kWoz1Entry;
            if (recordAt + kWoz1Entry > originalSize) { t.kept = false; t.note = "its entry lies past the end of the file"; }
            else {
                dataAt     = recordAt;
                dataBytes  = kWoz1Bits;
                t.bitCount = rd16(f.data() + recordAt + kWoz1Bits + 2);
                if (t.bitCount == 0 || t.bitCount > kWoz1Bits * 8) { t.kept = false; t.note = "its bit count is not possible"; }
            }
        }
        // A WOZ 2.1 flux track is named by the FLUX chunk's own map, not by
        // TMAP, so it is never walked here: it stays where it is, untouched
        // and unanalysed. [F] its record is not checked either — settled by
        // a damaged flux image to learn from.

        if (!t.kept) {
            rep.repairs.push_back("track " + std::to_string(t.firstQuarterTrack / 4) +
                                  (t.firstQuarterTrack % 4 ? " (+" + std::to_string(t.firstQuarterTrack % 4) + "/4)" : "") +
                                  " unmapped: " + t.note);
            if (apply) {
                for (std::size_t qt = 0; qt < kTmapEntries; ++qt)
                    if (f[l.tmap + qt] == idx) f[l.tmap + qt] = 0xFF;
                if (rep.version == 2 && recordAt + 8 <= f.size())
                    std::memset(f.data() + recordAt, 0, 8);
            }
        } else if (five25 && t.firstQuarterTrack % 4 == 0) {
            analyse(f.data() + dataAt, t.bitCount, t.firstQuarterTrack / 4, t);
        }
        rep.tracks.push_back(t);
    }

    // The chain itself, when it had to be bypassed: the three primary chunks
    // get the sizes the reference fixes for them, so a reader that walks the
    // chain (POM2's does) finds them again.
    if (l.usedFixedPositions && apply) {
        wr32(f.data() + kInfoData - 4, 60);
        wr32(f.data() + kTmapData - 4, static_cast<uint32_t>(kTmapEntries));
        if (rd32(f.data() + kTrksData - 4) > f.size() - kTrksData)
            wr32(f.data() + kTrksData - 4, static_cast<uint32_t>(f.size() - kTrksData));
    }
    if (rep.crcWasWrong)
        rep.repairs.push_back("the CRC did not match the file; recomputed");
    if (apply) {
        const uint32_t crc = crc32(f.data() + kHeader, f.size() - kHeader);
        if (rep.crcStated != 0 || !rep.repairs.empty()) wr32(f.data() + 8, crc);
    }
    return true;
}

std::string sectorList(uint32_t mask, int count)
{
    std::string out;
    for (int s = 0; s < count; ++s) {
        if (!(mask & (1u << s))) continue;
        if (!out.empty()) out += ' ';
        out += std::to_string(s);
    }
    return out;
}

}  // namespace

bool TrackReport::verifies() const
{
    const uint32_t all = (1u << expectedSectors()) - 1;
    return kept && (sectorsGood & all) == all && sectorsBad == 0;
}

int Report::tracksDropped() const
{
    int n = 0;
    for (const TrackReport& t : tracks) n += t.kept ? 0 : 1;
    return n;
}

int Report::tracksNotVerifying() const
{
    int n = 0;
    for (const TrackReport& t : tracks)
        if (t.kept && diskType == 1 && t.firstQuarterTrack % 4 == 0 && !t.verifies()) ++n;
    return n;
}

uint32_t crc32(const uint8_t* data, std::size_t size)
{
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

Report inspect(const std::vector<uint8_t>& file)
{
    Report r;
    std::string error;
    std::vector<uint8_t> copy = file;
    if (!process(copy, r, error, false)) {
        const bool woz = r.isWoz;
        r = Report{};
        r.isWoz = woz;
        r.repairs.push_back(error);
    }
    return r;
}

bool recover(const std::vector<uint8_t>& damaged, std::vector<uint8_t>& healthy,
             Report& report, std::string& error)
{
    std::vector<uint8_t> work = damaged;
    if (!process(work, report, error, true)) return false;
    healthy = std::move(work);
    return true;
}

std::string formatReport(const Report& r)
{
    char line[256];
    std::string out;
    if (!r.isWoz) return "not a WOZ file\n";
    std::snprintf(line, sizeof line, "WOZ%d, %s, CRC stated %08X, computed %08X%s\n", r.version,
                  r.diskType == 1 ? "5.25\"" : r.diskType == 2 ? "3.5\"" : "unknown disk type",
                  static_cast<unsigned>(r.crcStated), static_cast<unsigned>(r.crcComputed),
                  r.crcStated == 0 ? " (none stated)" : r.crcWasWrong ? " — MISMATCH" : "");
    out += line;
    for (const std::string& s : r.repairs) out += "repair: " + s + "\n";

    int fine = 0;
    for (const TrackReport& t : r.tracks) {
        if (!t.kept) continue;                               // already listed as a repair
        const int qt = t.firstQuarterTrack;
        std::string name = "track " + std::to_string(qt / 4);
        if (qt % 4) name += " +" + std::to_string(qt % 4) + "/4";
        if (r.diskType != 1 || qt % 4 != 0) continue;        // not analysed: say nothing
        if (t.verifies()) { ++fine; continue; }
        const int count = t.expectedSectors();
        const uint32_t all = (1u << count) - 1;
        if (t.addressFields == 0) {
            out += name + ": no standard sector (unformatted, protected, or damaged)\n";
            continue;
        }
        const uint32_t missing = all & ~(t.sectorsGood | t.sectorsBad);
        out += name + ":";
        if (t.sectorsBad) out += " data does not verify in sector(s) " + sectorList(t.sectorsBad, count) + ";";
        if (missing)      out += " no address field for sector(s) " + sectorList(missing, count) + ";";
        if (t.wrongTrackNumber) out += " an address field names another track;";
        out.back() = '\n';
    }
    std::snprintf(line, sizeof line, "%d track(s) verify in full, %d do not, %d unmapped\n", fine,
                  r.tracksNotVerifying(), r.tracksDropped());
    out += line;
    return out;
}

}  // namespace woz
}  // namespace pom2
