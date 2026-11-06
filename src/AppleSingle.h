// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

// AppleSingle — the container CiderPress, cc65 (`-t apple2`) and Cadius use
// to carry one ProDOS file, with its file type and aux type, across a host
// file system that has no place for them. Decode and encode, nothing else:
// no disk, no memory, no UI.
//
// The format: [B] Apple Computer, "AppleSingle/AppleDouble Formats for
// Foreign Files Developer's Note" (1990; copy fetched 2026-11-05 from
// nulib.com/library/AppleSingle_AppleDouble.pdf), Chapter 2:
//   - § AppleSingle file header, Table 2-1: magic (4), version (4), filler
//     (16), number of entries (2), then one 12-byte descriptor per entry —
//     entry ID, offset, length — all "most significant byte first".
//   - § Magic number $00051600; § Version number $00020000; § Filler "all
//     zeros"; § Number of entries, unsigned 16-bit.
//   - § Entry descriptors: ID 0 is invalid; offset from the start of the file;
//     "The length can be 0". Predefined IDs: 1 data fork, 2 resource fork,
//     3 real name, 11 ProDOS File Info.
//   - p. 8-9, Figure 2-3: the ProDOS File Info entry is access (16 bits),
//     file type (16), auxiliary type (32); "only the low byte [low word for
//     the aux type] is significant to ProDOS 8".
//   - Chapter 2 p. 4: "a header followed by ONE OR MORE data entries", and
//     § Number of entries "how many DIFFERENT entries" — so no entries, and
//     the same ID twice, are refused.
//   - § File layout: "The entry data follows all of the entry descriptors",
//     and "Put the data fork entry at the end of the file" — what encode does.
//   - Appendix: readers "should understand version 1 ($00010000)", whose
//     File Info is entry 7 instead of 8 + 11.
// Version 1, which GS/ShrinkIt still writes, is not described by the note
// beyond its Appendix; its layout is taken from [D] CiderPress II
// DiskArc/Arc/AppleSingle_FileEntry.cs @ 49b67b40 (Apache-2.0), itself from
// Apple II File Type Note $E0/0001, which POM2 could not fetch: the 16-byte
// filler names the home file system ("ProDOS" padded with blanks), and for
// ProDOS entry 7 is create date/time (4), modify date/time (4), access (2),
// file type (2), aux type (4). [F] Unverified against the File Type Note;
// settled by reading $E0/0001 or a GS/ShrinkIt-made file.
//
// The input is UNTRUSTED (a file the user dropped on the window): every
// offset and length is checked against the buffer in 64-bit arithmetic
// before anything is read or allocated, entries may not overlap each other
// or the descriptor table, and a failed decode leaves its output untouched.
// The structure follows [D] Casso CassoEmuCore/Core/AppleSingleCodec.cpp @
// 2d899adf (MIT); the hardening is POM2's (Casso accepts overlaps and
// duplicates, and decodes into its output as it goes).

#ifndef POM2_APPLE_SINGLE_H
#define POM2_APPLE_SINGLE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2::applesingle {

constexpr uint32_t kMagic    = 0x00051600;
constexpr uint32_t kVersion1 = 0x00010000;
constexpr uint32_t kVersion2 = 0x00020000;

/// Entries a decode accepts. [F] The note bounds the count only by its 16
/// bits; 15 IDs are predefined and CiderPress II stops at 16 ([D] @ 49b67b40,
/// MAX_ENTRIES). 64 leaves room for application-defined IDs.
constexpr std::size_t kMaxEntries = 64;

struct File {
    std::vector<uint8_t> data;        // entry 1
    std::vector<uint8_t> resource;    // entry 2 (kept, POM2 does not use it)
    std::string          realName;    // entry 3, may be empty
    bool     hasProdosInfo = false;   // entry 11 present (or a v1 ProDOS entry 7)
    uint16_t access   = 0xC3;
    uint16_t fileType = 0;            // only the low byte is a ProDOS type
    uint32_t auxType  = 0;            // only the low 16 bits are a ProDOS aux type
};

/// Magic and a version this reader knows (1 or 2), on a buffer long enough
/// to hold the fixed header.
bool looksLikeAppleSingle(const uint8_t* bytes, std::size_t size);

/// Decode a whole file. On failure returns false, says why in `error` (naming
/// the entry when one is at fault) and leaves `out` as it was.
bool decode(const std::vector<uint8_t>& bytes, File& out, std::string& error);

/// Version 2: real name (when non-empty), ProDOS File Info (when
/// hasProdosInfo), resource fork (when non-empty), then the data fork — last,
/// as the note recommends — always present, even empty.
std::vector<uint8_t> encode(const File& file);

} // namespace pom2::applesingle

#endif // POM2_APPLE_SINGLE_H
