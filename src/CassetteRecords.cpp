// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CassetteRecords.h"

#include <cmath>

namespace pom2::cassette {
namespace {

// Half-cycle lengths in µs. [B] the 770 Hz header is a 649 µs half, a 0 bit
// two 250 µs halves (2 kHz), a 1 bit two 500 µs halves (1 kHz); [C] the
// sync cycle's halves are 200 and 250 µs.
constexpr double kZeroHalfUs = 250.0, kOneHalfUs = 500.0, kHeaderHalfUs = 649.0;
constexpr double kSyncFirstUs = 200.0, kSyncSecondUs = 250.0;

// [F] The class boundaries are the midpoints between those lengths, not
// the Monitor's own: RDBIT counts an 11-cycle loop down from a value RDBYTE
// and READ reload, so the ROM's 0/1 boundary falls near 0.65-0.7 ms of a
// full cycle against 0.75 ms here. Both sit far from a clean tape's 0.5 and
// 1.0 ms. Settled by: a tape whose cycles fall between the two.
constexpr double kShortMaxUs = (kZeroHalfUs + kOneHalfUs) / 2;     // 375: a 0 half or a sync half
constexpr double kBitPairMaxUs = kZeroHalfUs + kOneHalfUs;        // 750: a full 0 cycle below, a 1 above
constexpr double kHeaderMaxUs = 1000.0;                            // longer: a gap
constexpr int kMinHeaderRun = 64;     // ~42 ms of tone; the ROM writes seconds of it
// A pair of halves longer than a 1 bit's and shorter than a gap: header tone.
constexpr double kOnePairMaxUs = kOneHalfUs + kHeaderHalfUs;   // 1149
constexpr int kHeaderLookahead = 8;   // pairs looked at to tell the next header from a long 1

} // namespace

std::vector<TapeRecord> decodeRecords(const std::vector<uint32_t>& halfCycles, double hz)
{
    std::vector<TapeRecord> records;
    if (!(hz > 0)) return records;
    const double usPerCycle = 1e6 / hz;
    const std::size_t n = halfCycles.size();
    auto us = [&](std::size_t i) { return halfCycles[i] * usPerCycle; };
    auto pairUs = [&](std::size_t i) { return us(i) + us(i + 1); };
    std::size_t i = 0;
    while (i < n) {
        // A header: a run of halves neither short (a bit or the sync) nor a gap.
        int run = 0;
        while (i < n && run < kMinHeaderRun) {
            run = us(i) >= kShortMaxUs && us(i) < kHeaderMaxUs ? run + 1 : 0;
            ++i;
        }
        if (run < kMinHeaderRun) break;
        // [C] READ's RD2 loop: every half up to the first short one is the
        //     header; that short half and the next are the sync cycle.
        while (i < n && us(i) >= kShortMaxUs && us(i) < kHeaderMaxUs) ++i;
        if (i + 1 >= n || us(i) >= kShortMaxUs) continue;   // a gap: look for another header
        i += 2;
        // Bits, a pair of halves each, until a gap or the next header.
        std::vector<int> bits;
        while (i + 1 < n) {
            if (us(i) >= kHeaderMaxUs || us(i + 1) >= kHeaderMaxUs) break;
            const double pair = pairUs(i);
            if (pair >= kOnePairMaxUs) {
                int tone = 0, looked = 0;
                for (std::size_t j = i; j + 1 < n && looked < kHeaderLookahead; j += 2, ++looked)
                    if (pairUs(j) >= kOnePairMaxUs) ++tone;
                if (tone * 4 >= looked * 3) break;   // mostly tone: the next record's header
            }
            bits.push_back(pair > kBitPairMaxUs ? 1 : 0);
            i += 2;
        }
        const std::size_t count = bits.size() / 8;
        if (count < 2) continue;   // not even one byte and its checksum
        TapeRecord rec;
        uint8_t sum = 0xFF;
        for (std::size_t k = 0; k < count; ++k) {
            uint8_t byte = 0;
            for (int b = 0; b < 8; ++b) byte = static_cast<uint8_t>((byte << 1) | bits[k * 8 + b]);
            if (k + 1 < count) {
                rec.bytes.push_back(byte);
                sum ^= byte;
            } else {
                rec.checksumOk = byte == sum;
            }
        }
        records.push_back(std::move(rec));
    }
    return records;
}

TapeKind classify(const std::vector<TapeRecord>& records)
{
    if (records.empty()) return TapeKind::Empty;
    if (records.size() >= 2 && records[0].checksumOk) {
        if (records[0].bytes.size() == 2) return TapeKind::IntegerBasic;
        if (records[0].bytes.size() == 3) return TapeKind::Applesoft;
    }
    return TapeKind::Binary;
}

std::vector<uint32_t> encodeRecords(const std::vector<std::vector<uint8_t>>& records, double hz, int headerHalfCycles)
{
    std::vector<uint32_t> out;
    auto half = [&](double us) { out.push_back(static_cast<uint32_t>(std::lround(us * hz / 1e6))); };
    for (const auto& rec : records) {
        for (int k = 0; k < headerHalfCycles; ++k) half(kHeaderHalfUs);
        half(kSyncFirstUs);
        half(kSyncSecondUs);
        uint8_t sum = 0xFF;
        auto byte = [&](uint8_t v) {
            for (int b = 7; b >= 0; --b) {
                const double us = (v >> b) & 1 ? kOneHalfUs : kZeroHalfUs;
                half(us);
                half(us);
            }
        };
        for (uint8_t v : rec) { byte(v); sum ^= v; }
        byte(sum);
        half(kOneHalfUs);              // the last edge
        half(400000.0);                // silence before the next record
    }
    return out;
}

} // namespace pom2::cassette
