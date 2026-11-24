// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

// cassette_records — reading a program tape's records off its pulse train
// (CassetteRecords.h), the way the Monitor's READ does.
//
// The pulse trains are built from [B] the Reference Manual's frequencies and
// [C] the Monitor WRITE routine's layout (encodeRecords), then disturbed: a
// real tape's halves are not the nominal lengths, and a WAV's edges land on
// sample boundaries. The end-to-end case, a tape the //e ROM itself SAVEs
// and LOADs, needs the firmware and lives in libretro_module.

#include "CassetteRecords.h"

#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

namespace tape = pom2::cassette;
constexpr double kHz = 1022727.0;   // the tape file timebase (CassetteDevice.h)

std::vector<uint8_t> program(std::size_t n, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = uint8_t(rng());
    return v;
}

void jitter(std::vector<uint32_t>& halves, double fraction, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> d(1.0 - fraction, 1.0 + fraction);
    for (auto& h : halves) h = uint32_t(h * d(rng));
}

void testIntegerBasic()
{
    const auto body = program(1234, 1);
    const std::vector<uint8_t> header = {uint8_t(body.size()), uint8_t(body.size() >> 8)};
    auto halves = tape::encodeRecords({header, body}, kHz);
    jitter(halves, 0.15, 2);
    const auto recs = tape::decodeRecords(halves, kHz);
    CHECK(recs.size() == 2);
    if (recs.size() != 2) return;
    CHECK(recs[0].bytes == header && recs[0].checksumOk);
    CHECK(recs[1].bytes == body && recs[1].checksumOk);
    CHECK(tape::classify(recs) == tape::TapeKind::IntegerBasic);
}

void testApplesoft()
{
    const auto body = program(5000, 3);
    const std::vector<uint8_t> header = {uint8_t(body.size()), uint8_t(body.size() >> 8), 0x00};
    auto halves = tape::encodeRecords({header, body}, kHz, 4000);
    jitter(halves, 0.10, 4);
    const auto recs = tape::decodeRecords(halves, kHz);
    CHECK(recs.size() == 2);
    CHECK(tape::classify(recs) == tape::TapeKind::Applesoft);
    if (recs.size() == 2) CHECK(recs[1].bytes == body && recs[1].checksumOk);
}

void testMemoryDump()
{
    const auto dump = program(2048, 5);
    const auto recs = tape::decodeRecords(tape::encodeRecords({dump}, kHz), kHz);
    CHECK(recs.size() == 1 && recs[0].bytes == dump && recs[0].checksumOk);
    CHECK(tape::classify(recs) == tape::TapeKind::Binary);
}

void testBadChecksumAndNoise()
{
    const std::vector<uint8_t> header = {0x10, 0x00};
    auto halves = tape::encodeRecords({header, program(16, 6)}, kHz);
    // Flip one bit of the first record's first data byte: its two halves
    // after the 1500 header halves and the 2 sync halves.
    const std::size_t at = 1500 + 2;
    const uint32_t zero = halves[at] < 400 ? halves[at] : 256, one = 511;
    const bool wasOne = halves[at] > 400;
    halves[at] = halves[at + 1] = wasOne ? zero : one;
    const auto recs = tape::decodeRecords(halves, kHz);
    CHECK(recs.size() == 2 && !recs[0].checksumOk);
    // A checksum failure on the header record is not taken for BASIC.
    CHECK(tape::classify(recs) == tape::TapeKind::Binary);

    // Silence, a header without data, a short burst: no record.
    CHECK(tape::decodeRecords({}, kHz).empty());
    CHECK(tape::classify({}) == tape::TapeKind::Empty);
    std::vector<uint32_t> toneOnly(3000, 664);
    CHECK(tape::decodeRecords(toneOnly, kHz).empty());
    CHECK(tape::decodeRecords(std::vector<uint32_t>(50, 256), kHz).empty());
}

} // namespace

int main()
{
    testIntegerBasic();
    testApplesoft();
    testMemoryDump();
    testBadChecksumAndNoise();
    if (g_fail) {
        std::fprintf(stderr, "cassette_records: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::puts("cassette_records: all checks passed");
    return 0;
}
