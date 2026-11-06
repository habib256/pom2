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

// Two MAME-parity pins on the Disk II READ path (bug hunt #16).
//
// 1. HEAD REACH. MAME `floppy_image_device::seek_phase_w` clamps the head
//    to `(m_tracks-1)*4`, where `m_tracks` is the DRIVE's mechanical range
//    — 42 for the 5.25" SD drive the Disk II uses
//    (`imagedev/floppy.cpp`, floppy_525_sd::setup_characteristics). POM2's
//    port used `(DiskImage::kTracks-1)*4` = 136, i.e. the IMAGE's 35-track
//    count, so the head could never leave track 34 — while `loadWoz`
//    populates all 160 TMAP quarter-track slots (up to track 39.75). A
//    seek to track 35+ silently re-read TRACK 34's address fields.
//    Pin: a 40-track WOZ whose address field carries its own track number
//    must report track 39 after a seek to track 39.
//
// 2. WEAK ZONE. MAME `floppy_image_device::get_next_transition` walks a
//    weak zone (a gap ≥ `m_amplifier_freakout_time` = 16 µs) in fixed 4 µs
//    intervals and returns the first whose hash bit is set — a ~50 %
//    density pulse train for the whole length of the zone, re-drawn every
//    revolution from `m_revolution_count`. POM2 served ONE blip per zone
//    per revolution (and none at all when the caller asked after it had
//    passed), so an erased stretch read back as a single constant byte,
//    identical on every pass: a weak-bit check either always passed or
//    always failed instead of being a coin toss.
//    Pin: an erased stretch must yield many distinct bytes AND differ
//    between two consecutive revolutions, while a repeated identical
//    `getNextTransition` call stays deterministic (rewind reproducibility).
//
// 3. THE ANGLE ACROSS A STEP (2026-11-05; not MAME parity — the WOZ
//    reference, which outranks it). The tracks of a WOZ do not hold the same
//    number of bit cells, and POM2 gives each cell a fixed duration, so its
//    tracks do not share one revolution time. The angular position was kept
//    as an offset in CYCLES, so a head that stepped at 90 % of a 50,000-cell
//    track arrived at 86.5 % of its 52,000-cell neighbour: the platter had
//    jumped back 3.5 % of a turn, some 220 nibbles. Software that times one
//    track against another (cross-track synchronisation, the first thing
//    the WOZ reference tells an implementer about) saw the wrong sector.
//    Pin: stepping with no time elapsed keeps the FRACTION of the turn, in
//    both directions, across the empty quarter-track in between.

#include "DiskIICard.h"
#include "DiskIIRoms.h"
#include "DiskImage.h"
#include "FloppySoundSink.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

uint32_t crc32r(const uint8_t* d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int b = 0; b < 8; ++b) c = (c & 1) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
    }
    return ~c;
}
void put32(std::vector<uint8_t>& v, size_t o, uint32_t x) {
    v[o] = x & 0xFF; v[o+1] = (x >> 8) & 0xFF; v[o+2] = (x >> 16) & 0xFF; v[o+3] = x >> 24;
}
void put16(std::vector<uint8_t>& v, size_t o, uint16_t x) {
    v[o] = x & 0xFF; v[o+1] = (x >> 8) & 0xFF;
}

// Assemble a WOZ1 from a list of (track number -> nibble buffer). TMAP
// gives each whole track T the classic three quarter-track positions
// 4T-1, 4T, 4T+1.
std::string writeWoz1(const std::string& name,
                      const std::vector<std::pair<int, std::vector<uint8_t>>>& trks)
{
    std::vector<uint8_t> f(12, 0);
    std::memcpy(f.data(), "WOZ1", 4);
    f[4] = 0xFF; f[5] = 0x0A; f[6] = 0x0D; f[7] = 0x0A;

    std::vector<uint8_t> info(60, 0);
    info[0] = 1;        // info_version
    info[1] = 1;        // disk_type 5.25"
    info[39] = 32;      // optimal_bit_timing (ignored for WOZ1, kept sane)
    {
        std::vector<uint8_t> c(8, 0);
        std::memcpy(c.data(), "INFO", 4); put32(c, 4, 60);
        f.insert(f.end(), c.begin(), c.end());
        f.insert(f.end(), info.begin(), info.end());
    }

    std::array<uint8_t, 160> tmap{}; tmap.fill(0xFF);
    for (size_t i = 0; i < trks.size(); ++i) {
        const int t = trks[i].first;
        if (t == 0) { tmap[0] = (uint8_t)i; tmap[1] = (uint8_t)i; }
        else {
            tmap[t*4 - 1] = (uint8_t)i;
            tmap[t*4]     = (uint8_t)i;
            if (t*4 + 1 < 160) tmap[t*4 + 1] = (uint8_t)i;
        }
    }
    {
        std::vector<uint8_t> c(8, 0);
        std::memcpy(c.data(), "TMAP", 4); put32(c, 4, 160);
        f.insert(f.end(), c.begin(), c.end());
        f.insert(f.end(), tmap.begin(), tmap.end());
    }

    std::vector<uint8_t> slots(160 * 6656, 0);
    for (size_t i = 0; i < trks.size(); ++i) {
        const auto& nb = trks[i].second;
        std::memcpy(slots.data() + i * 6656, nb.data(), nb.size());
        put16(slots, i*6656 + 6646, (uint16_t)nb.size());
        put16(slots, i*6656 + 6648, (uint16_t)(nb.size() * 8));
        put16(slots, i*6656 + 6650, 0xFFFF);
    }
    {
        std::vector<uint8_t> c(8, 0);
        std::memcpy(c.data(), "TRKS", 4); put32(c, 4, (uint32_t)slots.size());
        f.insert(f.end(), c.begin(), c.end());
        f.insert(f.end(), slots.begin(), slots.end());
    }
    put32(f, 8, crc32r(f.data() + 12, f.size() - 12));

    const auto p = fs::temp_directory_path() / name;
    std::ofstream o(p, std::ios::binary);
    o.write((const char*)f.data(), (std::streamsize)f.size());
    o.close();
    return p.string();
}

// One nibble track carrying 16 copies of `D5 AA 96 <track in 4-and-4> DE AA EB`.
std::vector<uint8_t> trackNibbles(int t) {
    std::vector<uint8_t> n(6400, 0xFF);
    for (int k = 0; k < 16; ++k) {
        const int o = k * 400 + 40;
        n[o+0] = 0xD5; n[o+1] = 0xAA; n[o+2] = 0x96;
        const uint8_t b = (uint8_t)t;
        n[o+3] = (uint8_t)(((b >> 1) & 0x55) | 0xAA);
        n[o+4] = (uint8_t)((b & 0x55) | 0xAA);
        n[o+5] = 0xDE; n[o+6] = 0xAA; n[o+7] = 0xEB;
    }
    return n;
}

void spinUp(DiskIICard& card) {
    card.deviceSelectRead(0x9);   // motor on
    card.deviceSelectRead(0xE);   // Q7L (read mode)
    card.deviceSelectRead(0xC);   // Q6L
    card.advanceCycles(100000);
}

// Issue `n` half-track step pulses (the Disk II stepper moves one HALF
// track = 2 quarter-tracks per single-phase well), continuing the phase
// sequence from `phaseFrom`.
void stepHalfTracks(DiskIICard& card, int from, int n) {
    for (int i = 0; i < n; ++i) {
        const int ph = (from + i + 1) & 3;
        card.deviceSelectRead((uint8_t)(ph * 2 + 1));
        card.advanceCycles(2000);
        card.deviceSelectRead((uint8_t)(ph * 2));
        card.advanceCycles(2000);
    }
}

// Read until an address field turns up; return the track number it carries.
int readTrackId(DiskIICard& card) {
    std::vector<uint8_t> s;
    uint8_t last = 0;
    for (int i = 0; i < 400000 && s.size() < 3000; ++i) {
        card.advanceCycles(6);
        const uint8_t b = card.deviceSelectRead(0xC);
        if ((b & 0x80) && b != last) { s.push_back(b); last = b; }
        else if (!(b & 0x80)) last = 0;
    }
    for (size_t i = 0; i + 7 < s.size(); ++i)
        if (s[i] == 0xD5 && s[i+1] == 0xAA && s[i+2] == 0x96 && s[i+5] == 0xDE)
            return (uint8_t)(((s[i+3] << 1) & 0xAA) | (s[i+4] & 0x55));
    return -1;
}

// ── 1. the head must reach every whole track the WOZ format can carry ───
void testHeadReachesTrack39() {
    std::printf("\n-- head reach: a 40-track WOZ --\n");
    std::vector<std::pair<int, std::vector<uint8_t>>> trks;
    for (int t = 0; t < 40; ++t) trks.emplace_back(t, trackNibbles(t));
    const std::string p = writeWoz1("pom2_reach40.woz", trks);

    DiskImage probe;
    check(probe.loadFile(p), "40-track WOZ loads");
    check(probe.trackBitLength(39 * 4) > 0,
          "quarter-track 156 (track 39) carries bit cells");

    DiskIICard card;
    pom2test::loadDiskIIDefaults(card);
    check(card.insertDisk(0, p), "insert");
    spinUp(card);

    int done = 0;
    for (int t : {0, 17, 34, 35, 39}) {
        stepHalfTracks(card, done, 2 * t - done);
        done = 2 * t;
        const int id = readTrackId(card);
        char msg[96];
        std::snprintf(msg, sizeof(msg),
            "seek to track %2d -> head reads track %d", t, id);
        check(id == t, msg);
    }
    std::error_code ec; fs::remove(p, ec);
}

// ── 2. an erased stretch must read as WEAK, not as one constant byte ────
void testWeakZoneIsNoisyAndVaries() {
    std::printf("\n-- weak zone: an erased stretch --\n");
    // 1000 readable sync bytes, then 5000 erased (all-zero-cell) bytes.
    std::vector<uint8_t> nb(6000, 0x00);
    for (int i = 0; i < 1000; ++i) nb[i] = 0xFF;
    const std::string p = writeWoz1("pom2_weakzone.woz", {{0, nb}});

    DiskImage img;
    check(img.loadFile(p), "weak-zone WOZ loads");
    const int period = img.trackPeriod(0);
    check(period > 0, "track has a period");
    // Determinism (rewind reproducibility): the same query twice must agree.
    const int64_t a = img.getNextTransition(0, period / 2, 0);
    const int64_t b = img.getNextTransition(0, period / 2, 0);
    check(a == b, "getNextTransition is deterministic in the weak zone");

    // Density. [B] WOZ 2.1 reference, "Freaking Out Like a MC3470": of the
    // random bits served while the head sees nothing, "roughly 30%" are 1.
    // POM2 followed MAME's 50 % until 2026-11-06. Counted over the erased
    // stretch (the sync bytes end at cell 8000 = LSS cycle 64 000), on two
    // revolutions: each inside the band, and not the same count.
    int weakPulses[2] = {0, 0};
    for (int r = 0; r < 2; ++r) {
        const int64_t from = int64_t{r} * period + 70000;
        const int64_t to   = int64_t{r} * period + period - 200;
        int64_t cur = from;
        for (;;) {
            const int64_t next = img.getNextTransition(0, cur, 0);
            if (next >= to) break;
            ++weakPulses[r];
            cur = next + 1;
        }
        const double share = weakPulses[r] / ((to - from) / 8.0);
        char dmsg[128];
        std::snprintf(dmsg, sizeof(dmsg),
            "revolution %d: %.1f %% of the weak zone's 4 us intervals carry "
            "a pulse (want 27-33)", r, share * 100.0);
        check(share > 0.27 && share < 0.33, dmsg);
    }
    check(weakPulses[0] != weakPulses[1],
          "two revolutions do not draw the same number of pulses");

    DiskIICard card;
    pom2test::loadDiskIIDefaults(card);
    card.insertDisk(0, p);
    spinUp(card);

    const int samplesPerRev = (period / 2) / 32;   // one sample per nibble time
    std::vector<std::vector<uint8_t>> rev(3);
    for (int r = 0; r < 3; ++r) {
        rev[r].reserve(samplesPerRev);
        for (int i = 0; i < samplesPerRev; ++i) {
            card.advanceCycles(32);
            rev[r].push_back(card.deviceSelectRead(0xC));
        }
    }
    const int weakFrom = 1100;      // past the 1000 readable sync bytes
    int distinct = 0, differ = 0, n = 0;
    std::vector<int> seen(256, 0);
    for (int i = weakFrom; i < samplesPerRev; ++i) {
        if (!seen[rev[1][i]]++) ++distinct;
        if (rev[0][i] != rev[1][i]) ++differ;
        ++n;
    }
    char msg[128];
    std::snprintf(msg, sizeof(msg),
        "erased stretch yields %d distinct byte values (need > 4; "
        "one-blip-per-revolution gives 1)", distinct);
    check(distinct > 4, msg);
    std::snprintf(msg, sizeof(msg),
        "%d/%d samples differ between two revolutions (need > 25%%)",
        differ, n);
    check(n > 0 && differ * 4 > n, msg);
    std::error_code ec; fs::remove(p, ec);
}

// ── 3. a head step keeps the angle, not the cycle offset ────────────────
void testAngleCarriesAcrossAStep() {
    std::printf("\n-- cross-track angle: tracks of unequal length --\n");
    std::vector<uint8_t> t0 = trackNibbles(0), t1 = trackNibbles(1);
    t0.resize(6250, 0xFF);                    // 50,000 bit cells
    t1.resize(6500, 0xFF);                    // 52,000 bit cells
    const std::string p = writeWoz1("pom2_angle.woz", {{0, t0}, {1, t1}});

    DiskIICard card;
    pom2test::loadDiskIIDefaults(card);
    check(card.insertDisk(0, p), "insert");
    const int p0 = card.driveImage(0).trackPeriod(0);
    const int p1 = card.driveImage(0).trackPeriod(4);
    check(p0 == 50000 * 8 && p1 == 52000 * 8, "the two tracks have unequal periods");
    check(card.driveImage(0).trackPeriod(2) == 0,
          "quarter-track 2 (TMAP $FF) lies between them, empty");

    spinUp(card);
    card.deviceSelectRead(0x1);               // phase 0 on: head held on track 0
    // A few turns first. In the first revolution after power-on the anchor
    // would have to go negative to carry the angle forward, which the card
    // cannot represent (-1 is "never spun") and leaves alone — a window of a
    // fifth of a second in which nothing steps off track 0.
    card.advanceCycles(600000);
    int guard = 0;
    while (guard++ < 100000) {                // bring the platter to ~90 % of a turn
        const int pos = card.getFluxPosition(0);
        if (pos >= p0 / 10 * 9 && pos < p0 / 100 * 92) break;
        card.advanceCycles(100);
    }
    const double before = static_cast<double>(card.getFluxPosition(0)) / p0;
    check(before >= 0.90 && before < 0.92, "platter parked near 90 % of a turn");

    // Track 0 -> 1 with NO time elapsed: quarter-tracks 1, 2 (empty), 3, 4.
    card.deviceSelectRead(0x3);               // phase 1 on  -> qt 1
    card.deviceSelectRead(0x0);               // phase 0 off -> qt 2
    card.deviceSelectRead(0x5);               // phase 2 on  -> qt 3
    card.deviceSelectRead(0x2);               // phase 1 off -> qt 4
    check(card.getQuarterTrack(0) == 4, "head on track 1");
    const double after = static_cast<double>(card.getFluxPosition(0)) / p1;
    char msg[160];
    std::snprintf(msg, sizeof(msg),
        "same fraction of the turn after the step: %.4f -> %.4f "
        "(keeping the cycle offset gives %.4f)", before, after,
        before * p0 / p1);
    check(std::fabs(after - before) < 0.0005, msg);

    // …and back: the round trip returns to where it started.
    card.deviceSelectRead(0x3);               // phase 1 on  -> qt 3
    card.deviceSelectRead(0x4);               // phase 2 off -> qt 2
    card.deviceSelectRead(0x1);               // phase 0 on  -> qt 1
    card.deviceSelectRead(0x2);               // phase 1 off -> qt 0
    check(card.getQuarterTrack(0) == 0, "head back on track 0");
    const double back = static_cast<double>(card.getFluxPosition(0)) / p0;
    std::snprintf(msg, sizeof(msg),
        "round trip returns to the same angle: %.4f -> %.4f", before, back);
    check(std::fabs(back - before) < 0.0005, msg);
    std::error_code ec; fs::remove(p, ec);
}

// ── 4. the track-0 stop is heard ────────────────────────────────────────
// A recalibration steps outward far more times than there are tracks. With
// the head already on track 0, every fourth pulse pulls the carriage against
// its stop — the Disk II's boot rattle. The card reported nothing for it (the
// head "did not move"), so no sound bank could ever play it.
struct CountingSink : FloppySoundSink {
    int bumps = 0, steps = 0;
    void motor(bool, bool) override {}
    void step(int, uint64_t) override { ++steps; }
    void click() override {}
    void bump(uint64_t) override { ++bumps; }
};

void testTrackZeroStopIsReported() {
    std::printf("\n-- the track-0 stop --\n");
    const std::string p = writeWoz1("pom2_stop.woz", {{0, trackNibbles(0)}, {1, trackNibbles(1)}});
    DiskIICard card;
    pom2test::loadDiskIIDefaults(card);
    CountingSink sink;
    card.setFloppySound(&sink);
    check(card.insertDisk(0, p), "insert");
    spinUp(card);
    check(card.getQuarterTrack(0) == 0, "head on track 0");

    // RWTS's recalibration, as the phases go: 3, 2, 1, 0, 3, 2, 1, 0 …
    // each one on, a pause, then off.
    for (int i = 0; i < 40; ++i) {
        const int ph = 3 - (i & 3);
        card.deviceSelectRead(static_cast<uint8_t>(ph * 2 + 1));
        card.advanceCycles(2000);
        card.deviceSelectRead(static_cast<uint8_t>(ph * 2));
        card.advanceCycles(2000);
    }
    char msg[96];
    std::snprintf(msg, sizeof(msg), "ten turns of the phases bang the stop %d times (want 10)",
                  sink.bumps);
    check(sink.bumps == 10, msg);
    check(card.getQuarterTrack(0) == 0, "and the head is still on track 0");

    // A real step inward is not a bump.
    const int before = sink.bumps;
    stepHalfTracks(card, 0, 4);
    card.advanceCycles(5000);
    check(sink.bumps == before && card.getQuarterTrack(0) == 8,
          "stepping to track 2 hits nothing");
    std::error_code ec; fs::remove(p, ec);
}

}  // namespace

int main() {
    std::printf("== Disk II track reach + weak-zone parity ==\n");
    testHeadReachesTrack39();
    testWeakZoneIsNoisyAndVaries();
    testAngleCarriesAcrossAStep();
    testTrackZeroStopIsReported();
    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "OK",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
