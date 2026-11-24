// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "A2rFlux.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace pom2::a2r {
namespace {

constexpr uint8_t kTail[4] = {0xFF, 0x0A, 0x0D, 0x0A};
constexpr int kQuarterTracks = 160;
constexpr uint64_t kWozTickPs = 125000;             // [B] WOZ 2.1: 125 ns ticks
constexpr uint64_t kA2r2TickPs = 125000;            // [B] A2R 2: "a single tick is 125 nanoseconds"
constexpr uint64_t kNominalRevolutionPs = 200000000000ull;   // 300 RPM

enum CaptureType : uint8_t { kTiming = 1, kBits = 2, kXTiming = 3 };

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// One capture of one quarter track: its transitions as absolute times.
struct Capture {
    int qt = -1;
    uint8_t type = 0;
    bool solved = false;              // A2R 3 SLVD: already one revolution
    uint64_t loopHintPs = 0;          // 0 = none
    std::vector<uint64_t> timesPs;    // each flux transition, from the capture start
    uint64_t endPs = 0;               // the solved loop's length (SLVD)
};

// [B] Both formats: a byte per transition, the ticks since the previous
// one; 255 adds 255 and continues.
void decodeFlux(const uint8_t* data, std::size_t size, uint64_t psPerTick, Capture& cap)
{
    uint64_t ticks = 0, pending = 0;
    for (std::size_t i = 0; i < size; ++i) {
        pending += data[i];
        if (data[i] == 255) continue;
        ticks += pending;
        pending = 0;
        cap.timesPs.push_back(ticks * psPerTick);
    }
    cap.endPs = (ticks + pending) * psPerTick;
}

uint32_t crc32(const uint8_t* data, std::size_t size)
{
    // [B] WOZ 2 reference, Appendix A: the standard CRC-32 (reflected,
    //     polynomial $EDB88320), as zlib computes it.
    uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// The revolution of a capture: transitions [a, b), where b is the same
// point of the disk one turn after a. False when the capture is too short.
bool chooseLoop(const Capture& cap, std::size_t& a, std::size_t& b, bool& refined)
{
    const auto& t = cap.timesPs;
    const std::size_t n = t.size();
    constexpr std::size_t kWindow = 512;
    if (n < kWindow * 4) return false;
    const uint64_t hint = cap.loopHintPs ? cap.loopHintPs : kNominalRevolutionPs;
    // Start 1 ms in: the head may still be settling at the capture start.
    a = std::lower_bound(t.begin(), t.end(), t.front() + 1000000000ull) - t.begin();
    if (a + kWindow >= n) return false;
    const uint64_t lo = t[a] + hint - hint / 25, hi = t[a] + hint + hint / 25;
    auto delta = [&](std::size_t i) { return static_cast<int64_t>(t[i + 1] - t[i]); };
    uint64_t best = std::numeric_limits<uint64_t>::max();
    std::size_t bestAt = 0;
    for (std::size_t j = std::lower_bound(t.begin(), t.end(), lo) - t.begin(); j < n && t[j] <= hi; ++j) {
        if (j + kWindow >= n) break;
        uint64_t score = 0;
        for (std::size_t k = 0; k < kWindow && score < best; ++k)
            score += static_cast<uint64_t>(std::llabs(delta(a + k) - delta(j + k)));
        if (score < best) { best = score; bestAt = j; }
    }
    if (bestAt) {
        b = bestAt;
        refined = true;
        return true;
    }
    // Too short to compare: take the hint as it is.
    b = std::lower_bound(t.begin(), t.end(), t[a] + hint) - t.begin();
    refined = false;
    return b < n && b > a;
}

// Transitions [a, b) as WOZ flux bytes, in 125 ns ticks rounded on the
// running time so no error accumulates; two transitions closer than half a
// tick become one.
std::vector<uint8_t> encodeWoz(const std::vector<uint64_t>& t, std::size_t a, std::size_t b, uint64_t originPs)
{
    std::vector<uint8_t> out;
    int64_t previous = 0;
    for (std::size_t i = a; i < b; ++i) {
        const int64_t tick = static_cast<int64_t>((t[i] - originPs + kWozTickPs / 2) / kWozTickPs);
        int64_t v = tick - previous;
        if (v <= 0) continue;
        previous = tick;
        while (v >= 255) { out.push_back(255); v -= 255; }
        out.push_back(static_cast<uint8_t>(v));
    }
    return out;
}

bool parseA2r2(const std::vector<uint8_t>& f, std::vector<Capture>& caps, uint8_t& diskType,
               bool& protect, std::string& error)
{
    std::size_t p = 8;
    while (p + 8 <= f.size()) {
        const uint8_t* id = f.data() + p;
        const uint32_t size = rd32(f.data() + p + 4);
        const std::size_t data = p + 8;
        if (size > f.size() - data) { error = "A2R chunk runs past the end of the file"; return false; }
        if (std::memcmp(id, "INFO", 4) == 0 && size >= 36) {
            diskType = f[data + 33];
            protect = f[data + 34] == 1;
        } else if (std::memcmp(id, "STRM", 4) == 0) {
            std::size_t q = data;
            const std::size_t end = data + size;
            while (q < end && f[q] != 0xFF) {
                if (q + 10 > end) { error = "A2R STRM capture truncated"; return false; }
                Capture cap;
                cap.qt = f[q];
                cap.type = f[q + 1];
                const uint32_t length = rd32(f.data() + q + 2);
                cap.loopHintPs = uint64_t(rd32(f.data() + q + 6)) * kA2r2TickPs;
                q += 10;
                if (length > end - q) { error = "A2R STRM capture truncated"; return false; }
                if (cap.type == kTiming || cap.type == kXTiming) {
                    decodeFlux(f.data() + q, length, kA2r2TickPs, cap);
                    caps.push_back(std::move(cap));
                }
                q += length;
            }
        }
        p = data + size;
    }
    return true;
}

bool parseA2r3(const std::vector<uint8_t>& f, std::vector<Capture>& caps, uint8_t& driveType,
               bool& protect, uint8_t& hardSectors, std::string& error)
{
    std::size_t p = 8;
    while (p + 8 <= f.size()) {
        const uint8_t* id = f.data() + p;
        const uint32_t size = rd32(f.data() + p + 4);
        const std::size_t data = p + 8;
        if (size > f.size() - data) { error = "A2R chunk runs past the end of the file"; return false; }
        const std::size_t end = data + size;
        if (std::memcmp(id, "INFO", 4) == 0 && size >= 37) {
            driveType = f[data + 33];
            protect = f[data + 34] == 1;
            hardSectors = f[data + 36];
        } else if ((std::memcmp(id, "RWCP", 4) == 0 || std::memcmp(id, "SLVD", 4) == 0) && size >= 16) {
            const bool solved = id[0] == 'S';
            const uint64_t psPerTick = rd32(f.data() + data + 1);
            if (psPerTick == 0) { error = "A2R capture resolution is zero"; return false; }
            std::size_t q = data + 16;
            while (q < end && f[q] != 'X') {
                Capture cap;
                cap.solved = solved;
                std::size_t indexAt = 0;
                unsigned indexCount = 0;
                if (!solved) {
                    // [B] RWCP entry: 'C', type, u16 location, N, N x u32 index, u32 size.
                    if (q + 5 > end || f[q] != 'C') { error = "A2R RWCP capture damaged"; return false; }
                    cap.type = f[q + 1];
                    cap.qt = rd16(f.data() + q + 2);
                    indexCount = f[q + 4];
                    indexAt = q + 5;
                    q = indexAt + 4u * indexCount;
                } else {
                    // [B] SLVD entry: 'T', u16 location, 2 mirror bytes, 6 reserved, N, N x u32.
                    if (q + 12 > end || f[q] != 'T') { error = "A2R SLVD track damaged"; return false; }
                    cap.type = kTiming;
                    cap.qt = rd16(f.data() + q + 1);
                    indexCount = f[q + 11];
                    indexAt = q + 12;
                    q = indexAt + 4u * indexCount;
                }
                if (q + 4 > end) { error = "A2R capture truncated"; return false; }
                const uint32_t length = rd32(f.data() + q);
                q += 4;
                if (length > end - q) { error = "A2R capture truncated"; return false; }
                // [B] "loop_time = capture.indexSignals[info.hardSectorCount]".
                if (indexCount > hardSectors)
                    cap.loopHintPs = uint64_t(rd32(f.data() + indexAt + 4u * hardSectors)) * psPerTick;
                if (cap.type == kTiming || cap.type == kXTiming) {
                    decodeFlux(f.data() + q, length, psPerTick, cap);
                    caps.push_back(std::move(cap));
                }
                q += length;
            }
        }
        p = data + size;
    }
    return true;
}

} // namespace

bool looksLikeA2r(const std::vector<uint8_t>& f)
{
    return f.size() >= 16 && f[0] == 'A' && f[1] == '2' && f[2] == 'R' && (f[3] == '2' || f[3] == '3')
        && std::memcmp(f.data() + 4, kTail, 4) == 0;
}

bool toWoz(const std::vector<uint8_t>& f, std::vector<uint8_t>& woz, std::string& note, std::string& error)
{
    if (!looksLikeA2r(f)) { error = "not an A2R 2 or A2R 3 flux capture"; return false; }
    const int version = f[3] - '0';
    std::vector<Capture> caps;
    uint8_t type = 0, hardSectors = 0;
    bool protect = false;
    if (version == 2) {
        if (!parseA2r2(f, caps, type, protect, error)) return false;
        if (type != 1) { error = "this A2R is a 3.5-inch capture; POM2 converts 5.25-inch ones"; return false; }
    } else {
        if (!parseA2r3(f, caps, type, protect, hardSectors, error)) return false;
        if (type != 1) {
            error = "this A2R is not an Apple 5.25-inch capture (drive type " + std::to_string(type) + ")";
            return false;
        }
    }

    // One capture a quarter track: a solved one, else the longest timing.
    std::array<const Capture*, kQuarterTracks> pick{};
    for (const auto& cap : caps) {
        if (cap.qt < 0 || cap.qt >= kQuarterTracks || cap.timesPs.empty()) continue;
        const Capture*& slot = pick[cap.qt];
        if (!slot || (cap.solved && !slot->solved)
                || (!slot->solved && !cap.solved && cap.timesPs.size() > slot->timesPs.size()))
            slot = &cap;
    }

    std::array<std::vector<uint8_t>, kQuarterTracks> tracks;
    int solved = 0, refined = 0, hinted = 0;
    for (int qt = 0; qt < kQuarterTracks; ++qt) {
        const Capture* cap = pick[qt];
        if (!cap) continue;
        if (cap->solved) {
            tracks[qt] = encodeWoz(cap->timesPs, 0, cap->timesPs.size(), 0);
            ++solved;
            continue;
        }
        std::size_t a = 0, b = 0;
        bool good = false;
        if (!chooseLoop(*cap, a, b, good)) continue;
        // The revolution starts at transition a: the first byte is the
        // interval from a to a+1, the last from b-1 to b (which IS a, a turn later).
        tracks[qt] = encodeWoz(cap->timesPs, a + 1, b + 1, cap->timesPs[a]);
        ++(good ? refined : hinted);
    }

    // [B] WOZ practice: a whole track's capture also answers the head at the
    //     quarter tracks either side of it, when those have none of their own.
    std::array<int, kQuarterTracks> map{};
    map.fill(-1);
    for (int qt = 0; qt < kQuarterTracks; ++qt)
        if (!tracks[qt].empty()) map[qt] = qt;
    for (int qt = 0; qt < kQuarterTracks; ++qt) {
        if (map[qt] >= 0) continue;
        for (int n : {qt - 1, qt + 1})
            if (n >= 0 && n < kQuarterTracks && n % 4 == 0 && !tracks[n].empty()) { map[qt] = n; break; }
    }

    // ── The WOZ 2.1 file ─────────────────────────────────────────────────
    std::vector<int> order;   // TRKS entry -> quarter track whose data it holds
    std::array<uint8_t, kQuarterTracks> fluxMap{};
    fluxMap.fill(0xFF);
    for (int qt = 0; qt < kQuarterTracks; ++qt) {
        if (map[qt] < 0) continue;
        auto it = std::find(order.begin(), order.end(), map[qt]);
        if (it == order.end()) { order.push_back(map[qt]); it = order.end() - 1; }
        fluxMap[qt] = static_cast<uint8_t>(it - order.begin());
    }
    if (order.empty()) { error = "the A2R holds no timing capture POM2 can loop"; return false; }

    woz.assign(1536, 0);
    std::memcpy(woz.data(), "WOZ2", 4);
    std::memcpy(woz.data() + 4, kTail, 4);
    auto put16 = [&](std::size_t at, uint32_t v) { woz[at] = uint8_t(v); woz[at + 1] = uint8_t(v >> 8); };
    auto put32 = [&](std::size_t at, uint32_t v) { put16(at, v & 0xFFFF); put16(at + 2, v >> 16); };
    // INFO at 12, 60 bytes.
    std::memcpy(woz.data() + 12, "INFO", 4);
    put32(16, 60);
    woz[20] = 3;                       // INFO version 3: FLUX
    woz[21] = 1;                       // 5.25"
    woz[22] = protect ? 1 : 0;         // the capture's own notch
    woz[23] = 0;                       // not cross-track synchronized: each loop starts where it was cut
    const char creator[] = "POM2 (from an Applesauce A2R)";
    std::memset(woz.data() + 25, ' ', 32);
    std::memcpy(woz.data() + 25, creator, std::min<std::size_t>(32, sizeof creator - 1));
    woz[57] = 1;                       // one side
    woz[59] = 32;                      // optimal bit timing: 4 µs
    // TMAP at 80: every quarter track goes through FLUX.
    std::memcpy(woz.data() + 80, "TMAP", 4);
    put32(84, 160);
    std::memset(woz.data() + 88, 0xFF, 160);
    // TRKS at 248: 160 entries, then the track data from block 3.
    std::memcpy(woz.data() + 248, "TRKS", 4);
    uint32_t largest = 0;
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto& bytes = tracks[order[i]];
        const uint32_t blocks = static_cast<uint32_t>((bytes.size() + 511) / 512);
        const uint32_t start = static_cast<uint32_t>(woz.size() / 512);
        put16(256 + i * 8, start);
        put16(256 + i * 8 + 2, blocks);
        put32(256 + i * 8 + 4, static_cast<uint32_t>(bytes.size()));   // flux: a byte count
        woz.insert(woz.end(), bytes.begin(), bytes.end());
        woz.resize(std::size_t(start + blocks) * 512, 0);
        largest = std::max(largest, blocks);
    }
    put32(252, static_cast<uint32_t>(woz.size() - 256));
    // FLUX on its own block.
    const std::size_t fluxAt = woz.size();
    woz.resize(fluxAt + 8 + 160, 0);
    std::memcpy(woz.data() + fluxAt, "FLUX", 4);
    put32(fluxAt + 4, 160);
    std::memcpy(woz.data() + fluxAt + 8, fluxMap.data(), 160);
    put16(64, largest);                                      // largest track (blocks)
    put16(66, static_cast<uint32_t>(fluxAt / 512));          // flux_block
    put16(68, largest);                                      // largest_flux_track
    put32(8, crc32(woz.data() + 12, woz.size() - 12));

    note = "A2R " + std::to_string(version) + " flux capture: " + std::to_string(order.size())
         + " track" + (order.size() == 1 ? "" : "s") + " as one-revolution WOZ flux";
    if (solved) note += ", " + std::to_string(solved) + " solved by Applesauce";
    if (refined) note += ", " + std::to_string(refined) + " looped where the flux repeats";
    if (hinted) note += ", " + std::to_string(hinted) + " looped at the nominal turn";
    if (protect) note += "; write-protected, as the original disk";
    return true;
}

} // namespace pom2::a2r
