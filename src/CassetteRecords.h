// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// CassetteRecords — what a program tape holds, read off its pulse train the
// way the Monitor's READ routine reads it, so a host can tell an Integer
// BASIC program from an Applesoft one or a memory dump BEFORE the guest
// loads it (the libretro core types the right command; CassetteDevice plays
// the tape either way).
//
// [B] Apple II Reference Manual (1978), "The Cassette Interface": a header
//     of 770 Hz tone, then data at 2 kHz for a 0 bit and 1 kHz for a 1 bit.
// [C] Monitor ROM WRITE $FECD / READ $FEFD / RDBYTE $FCEC / RDBIT $FCFD:
//     after the header, a sync cycle shorter than any bit (a 200 µs then a
//     250 µs half), the bytes most significant bit first, then a checksum
//     byte that is $FF exclusive-ORed with every data byte.
// [C] Integer BASIC's SAVE writes a 2-byte length record before the
//     program; Applesoft's writes 3 (the length and a lock/run flag). A
//     Monitor `W` dump is one record with no header record.

#ifndef POM2_CASSETTE_RECORDS_H
#define POM2_CASSETTE_RECORDS_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pom2::cassette {

struct TapeRecord {
    std::vector<uint8_t> bytes;   ///< the data, the checksum byte removed
    bool checksumOk = false;
};

/// Decode the records of a pulse train: `halfCycles` are the durations
/// between level changes, in cycles of `hz`.
std::vector<TapeRecord> decodeRecords(const std::vector<uint32_t>& halfCycles, double hz);

enum class TapeKind { Empty, IntegerBasic, Applesoft, Binary };

/// What the records are: a 2-byte header record then a program is Integer
/// BASIC, a 3-byte one Applesoft; anything else is a memory dump.
TapeKind classify(const std::vector<TapeRecord>& records);

/// The pulse train the Monitor's WRITE makes of `records` (header tone,
/// sync, bits, checksum, each record its own header), in cycles of `hz`.
/// For the tests.
std::vector<uint32_t> encodeRecords(const std::vector<std::vector<uint8_t>>& records, double hz,
                                    int headerHalfCycles = 1500);

} // namespace pom2::cassette

#endif // POM2_CASSETTE_RECORDS_H
