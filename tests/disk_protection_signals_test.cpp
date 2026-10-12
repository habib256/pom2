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


// Synthetic protection prerequisites, without copyrighted game images or
// SmartDisk II firmware. WOZ structure/bit order: Applesauce WOZ 2.1,
// https://applesaucefdc.com/woz/reference2/ (TMAP / TRKS / bitstream).
//
// Independent flux oracle: scan the ORIGINAL fixture bits on a circular
// timeline, never DiskImage::bitAt/fluxEvents or its search cursor. Dense
// patterns avoid weak gaps, so every expected transition is exact.
// Controller differential: identical MMIO schedules, advancing one card a
// CPU cycle at a time and the other in instruction-sized / large chunks.
// This catches dropped pulses and phase-dependent batching errors, but is
// not a comparison against real hardware or a second silicon model.

#include "DiskImage.h"
#include "DiskIICard.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

void require(bool ok, const std::string& reason) {
    if (!ok) throw std::runtime_error(reason);
}
void put16(std::vector<uint8_t>& v, size_t at, uint16_t n) {
    v.at(at) = n; v.at(at + 1) = n >> 8;
}
void put32(std::vector<uint8_t>& v, size_t at, uint32_t n) {
    for (int i = 0; i < 4; ++i) v.at(at + i) = n >> (8 * i);
}
uint32_t crc32(const std::vector<uint8_t>& v) {
    uint32_t c = ~uint32_t{0};
    for (size_t i = 12; i < v.size(); ++i) {
        c ^= v[i];
        for (int j = 0; j < 8; ++j) c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0);
    }
    return ~c;
}

struct Fixture {
    fs::path directory;
    std::vector<std::vector<uint8_t>> bits;
    std::array<uint8_t, 160> map;
    int cellTicks;
    std::string path;

    Fixture(int obt, const std::vector<int>& lengths) : cellTicks(obt / 4) {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int i = 0; ; ++i) {
            directory = fs::temp_directory_path() /
                ("pom2-protection-" + std::to_string(nonce) + "-" + std::to_string(i));
            if (fs::create_directory(directory)) break;
        }
        map.fill(0xFF);
        // Fat track at 0/0.25/0.5, then distinct quarter-track streams.
        map[0] = map[1] = map[2] = 0;
        map[3] = 1; map[4] = map[5] = 2;
        map[6] = 1; map[7] = 0;
        for (size_t trk = 0; trk < lengths.size(); ++trk) {
            std::vector<uint8_t> track(lengths[trk]);
            // At most two consecutive zeros, including the revolution seam.
            for (size_t cell = 0; cell < track.size(); ++cell)
                track[cell] = ((cell + trk * 3) % 11 != 2 && (cell + trk) % 7 != 3);
            track.front() = track.back() = 1;
            bits.push_back(std::move(track));
        }
        std::vector<uint8_t> file = {'W','O','Z','2',0xFF,0x0A,0x0D,0x0A,0,0,0,0};
        auto chunk = [&](const char* name, const std::vector<uint8_t>& data) {
            file.insert(file.end(), name, name + 4);
            const auto at = file.size(); file.resize(at + 4); put32(file, at, data.size());
            file.insert(file.end(), data.begin(), data.end());
        };
        std::vector<uint8_t> info(60, 0);
        info[0] = 2; info[1] = 1; info[2] = 1; // read-only media
        info[37] = 1; info[39] = obt;
        unsigned largest = 0;
        for (const auto& track : bits)
            largest = std::max(largest, unsigned((track.size() + 4095) / 4096));
        put16(info, 44, largest);
        chunk("INFO", info);
        chunk("TMAP", std::vector<uint8_t>(map.begin(), map.end()));
        std::vector<uint8_t> tracks(1280, 0);
        uint16_t block = 3;
        for (size_t trk = 0; trk < bits.size(); ++trk) {
            const auto& track = bits[trk];
            const uint16_t blocks = (track.size() + 4095) / 4096;
            put16(tracks, trk * 8, block);
            put16(tracks, trk * 8 + 2, blocks);
            put32(tracks, trk * 8 + 4, track.size());
            const auto at = tracks.size();
            // Padding deliberately all ones. Reading rounded-up byte/block
            // length instead of the declared bit_count exposes extra pulses.
            tracks.resize(at + blocks * 512, 0xFF);
            for (size_t cell = 0; cell < track.size(); ++cell)
                if (!track[cell]) tracks[at + cell / 8] &= ~(0x80 >> (cell % 8));
            block += blocks;
        }
        chunk("TRKS", tracks); put32(file, 8, crc32(file));
        path = (directory / "fixture.woz").string();
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(file.data()), file.size());
        require(bool(out), "cannot write synthetic WOZ");
    }
    ~Fixture() { std::error_code ec; fs::remove_all(directory, ec); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    int64_t next(int qt, int64_t from, int64_t anchor) const {
        if (map.at(qt) == 0xFF) return DiskImage::kFluxNever;
        const auto& track = bits.at(map[qt]);
        const int64_t origin = anchor < 0 ? 0 : anchor;
        const int64_t half = cellTicks / 2;
        // First cell whose centre is at or after 'from' (ceil division).
        const int64_t delta = from - origin - half;
        int64_t cell = delta / cellTicks;
        if (delta > 0 && delta % cellTicks) ++cell;
        for (size_t scanned = 0; scanned <= track.size(); ++scanned, ++cell) {
            const int64_t length = int64_t(track.size());
            const int64_t index = (cell % length + length) % length;
            if (track[index]) return origin + cell * cellTicks + half;
        }
        throw std::runtime_error("fixture has no flux");
    }
};

void testFluxOracle() {
    uint32_t random = 0x2E6502u;
    size_t comparisons = 0;
    for (int obt : {28, 32, 36}) {
        Fixture fixture(obt, {50003, 49999, 50009});
        DiskImage image;
        require(image.loadFile(fixture.path), "load fixture: " + image.getLastError());
        for (int qt = 0; qt < 8; ++qt) {
            const int length = fixture.bits[fixture.map[qt]].size();
            require(image.trackBitLength(qt) == length, "declared non-byte bit_count lost");
            require(image.trackPeriod(qt) == length * fixture.cellTicks, "wrong rotation period");
        }
        for (int64_t anchor : {-1LL, 0LL, 137LL, 9876543LL}) {
            const int64_t origin = anchor < 0 ? 0 : anchor;
            auto compare = [&](int qt, int64_t from) {
                const int64_t expected = fixture.next(qt, from, anchor);
                const int64_t actual = image.getNextTransition(qt, from, anchor);
                if (actual != expected) {
                    char message[256];
                    std::snprintf(message, sizeof(message),
                        "flux mismatch: OBT=%d qt=%d anchor=%lld query=%lld expected=%lld actual=%lld",
                        obt, qt, (long long)anchor, (long long)from,
                        (long long)expected, (long long)actual);
                    throw std::runtime_error(message);
                }
                ++comparisons;
            };
            // Every last-bit/first-bit seam, including non-zero angular
            // anchors, negative deltas and exact transition boundaries.
            for (int qt = 0; qt < 8; ++qt) {
                const int64_t period = fixture.bits[fixture.map[qt]].size() * fixture.cellTicks;
                for (int rev : {-2, -1, 0, 1, 2, 17})
                    for (int offset = -12; offset <= 12; ++offset)
                        compare(qt, origin + rev * period + offset);
                int64_t from = origin;
                for (int i = 0; i < 128; ++i) {
                    const auto event = fixture.next(qt, from, anchor);
                    compare(qt, event - 1); compare(qt, event); compare(qt, event + 1);
                    from = event + 1;
                }
            }
            // Interleaved quarter tracks and non-monotonic queries stress
            // the resumed search cursor independently of its implementation.
            for (int i = 0; i < 12000; ++i) {
                random = random * 1664525u + 1013904223u;
                const int qt = (random >> 24) & 7;
                compare(qt, origin + int64_t(random % 9000000) - 500000);
            }
            compare(8, origin + 123); // unmapped quarter-track
        }
    }
    std::printf("PASS: %zu exact flux comparisons (non-byte tracks, fat/quarter tracks, seams, anchors, seeks)\n", comparisons);
}

void advance(DiskIICard& card, int cycles, int chunk) {
    while (cycles > 0) {
        const int step = std::min(cycles, chunk);
        card.advanceCycles(step); cycles -= step;
    }
}

void testFatTrackContinuity(int obt) {
    Fixture fixture(obt, {50003, 50003, 50003});
    DiskIICard moving, stationary;
    moving.setIwmHost(false); stationary.setIwmHost(false);
    for (auto* card : {&moving, &stationary}) {
        require(card->insertDisk(0, fixture.path), "insert fat-track fixture");
        card->deviceSelectRead(9);
        card->deviceSelectRead(0xE);
        card->deviceSelectRead(0xC);
        card->deviceSelectRead(1);
    }
    // Quarter tracks 0, 1 and 2 share exactly the same physical stream.
    // Moving the head between them must preserve the ongoing read, not
    // reset the sequencer or restart the disk's angular position.
    const int switches[] = {3, 0, 1, 2};
    const int positions[] = {1, 2, 1, 0};
    for (int step = 0; step < 12; ++step) {
        advance(moving, 38003, 7); advance(stationary, 38003, 7);
        const int offset = switches[step % 4];
        moving.deviceSelectRead(offset);
        // Match even-address read latency while keeping the fixed head.
        stationary.deviceSelectRead((offset & 1) ? 1 : 0xC);
        require(moving.getQuarterTrack() == positions[step % 4], "fat-track head did not move");
        require(stationary.getQuarterTrack() == 0, "reference head moved");
        for (int sample = 0; sample < 96; ++sample) {
            advance(moving, 7, 7); advance(stationary, 7, 7);
            require(moving.deviceSelectRead(0xC) == stationary.deviceSelectRead(0xC),
                    "fat-track head motion interrupted the read stream (OBT=" + std::to_string(obt) + ")");
        }
    }
}

void testControllerCadence() {
    size_t samples = 0;
    for (int obt : {28, 32, 36}) {
        testFatTrackContinuity(obt);
        Fixture fixture(obt, {50003, 50003, 50003});
        // The schedule crosses aliased fat-track positions and distinct
        // neighbouring streams without pausing guest reads.
        DiskIICard fine, batched;
        fine.setIwmHost(false); batched.setIwmHost(false);
        require(fine.insertDisk(0, fixture.path), "insert fine drive 1");
        require(batched.insertDisk(0, fixture.path), "insert batched drive 1");
        require(fine.insertDisk(1, fixture.path), "insert fine drive 2");
        require(batched.insertDisk(1, fixture.path), "insert batched drive 2");
        require(fine.usingBitLss() && batched.usingBitLss(), "P6 sequencer must be active");
        int64_t cycle = 0;
        auto access = [&](int offset, const char* stage) {
            const uint8_t expected = fine.deviceSelectRead(offset);
            const uint8_t actual = batched.deviceSelectRead(offset);
            if (actual != expected || fine.getQuarterTrack(0) != batched.getQuarterTrack(0) ||
                fine.getQuarterTrack(1) != batched.getQuarterTrack(1) ||
                fine.getActiveDrive() != batched.getActiveDrive()) {
                char message[256];
                std::snprintf(message, sizeof(message),
                    "controller mismatch: %s OBT=%d cpu_cycle=%lld switch=$C0E%X drive=%d qt=%d expected=$%02X actual=$%02X",
                    stage, obt, (long long)cycle, offset, fine.getActiveDrive()+1,
                    fine.getQuarterTrack(fine.getActiveDrive()), expected, actual);
                throw std::runtime_error(message);
            }
            ++samples;
        };
        auto delay = [&](int cycles, int chunk) {
            advance(fine, cycles, 1); advance(batched, cycles, chunk); cycle += cycles;
        };
        access(9, "motor on"); access(0xE, "read mode"); access(0xC, "read latch");
        access(1, "phase 0 on");
        // First prove a normal stream returns data, not two stuck latches.
        int changing = 0; uint8_t previous = 0;
        for (int i = 0; i < 320; ++i) {
            delay(7, 7); access(0xC, "initial stream");
            const uint8_t data = fine.deviceSelectRead(0xC);
            require(data == batched.deviceSelectRead(0xC), "initial extra read differs");
            changing += data != previous; previous = data;
        }
        require(changing > 20, "read stream is stuck");
        const int phaseSwitches[] = {3, 0, 5, 2, 3, 4, 1, 2};
        const int delays[] = {1, 3, 7, 31, 4095, 8191};
        for (int step = 0; step < 768; ++step) {
            const int elapsed = delays[step % 6];
            delay(elapsed, (step % 3 == 0) ? 7 : elapsed);
            access(0xC, "stream / revolution wrap");
            if (step % 16 == 0) {
                access(phaseSwitches[(step / 16) % 8], "head move while reading");
                const int positions[] = {1, 2, 3, 4, 3, 2, 1, 0};
                require(fine.getQuarterTrack() == positions[(step / 16) % 8], "scheduled head did not move");
            }
            if (step % 97 == 0) {
                access(0xB, "drive 2"); delay(37, 37); access(0xC, "drive 2 latch");
                access(0xA, "drive 1");
            }
            if (step % 131 == 0) {
                access(8, "motor coasting"); delay(4097, 4097);
                access(0xC, "coasting latch"); access(9, "resume motor");
            }
        }
        // Full motor stop/restart as well as the shorter coasting windows.
        access(8, "motor off"); delay(1022730, 4096);
        require(!fine.isMotorOn() && !batched.isMotorOn(), "motor failed to stop");
        access(9, "restart"); delay(8193, 8193); access(0xC, "restart latch");
    }
    std::printf("PASS: %zu controller samples match cycle-by-cycle reference across head moves, drive selects, coasting/restart and batching\n", samples);
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: test_disk_protection_signals flux|cadence");
        const std::string mode = argv[1];
        if (mode == "flux") testFluxOracle();
        else if (mode == "cadence") testControllerCadence();
        else throw std::runtime_error("unknown mode: " + mode);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
