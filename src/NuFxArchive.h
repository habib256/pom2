// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// NuFxArchive — reading a ShrinkIt archive (.shk, .sdk, .bxy, .sea).
//
// NuFX is the Apple II's archive format: a master header, then records, each
// a file or a whole disk, each carrying "threads" (data fork, resource fork,
// disk image, filename, comment), each thread stored as is or compressed.
// POM2 reads it to MOUNT what it holds: a disk record becomes a disk image,
// file records become files on a ProDOS volume (ExchangeMedia.h). It never
// writes one.
//
// [B] Apple II File Type Note $E0/$8002, "NuFX (ShrinkIt) archive" (Andy
//     Nicholas, rev. 3) — master and record headers, thread classes/kinds.
// [D] NufxLib 3.1.0 (Andy McFadden, BSD) — the reference reader; POM2's
//     reader is written from the File Type Note, and takes from NufxLib only
//     what the note does not say, each place marked where it is used:
//     ShrinkIt's LZW/1 and LZW/2 stream layout and their two quirks, the
//     thread-EOF repair of old disk records, the Binary II and GS/OS SEA
//     wrappers, and which record versions carry a thread CRC.
//
// Compression: uncompressed, LZW/1 and LZW/2 — every format in the 82
// archives of the local test corpus (724 LZW/1, 168 LZW/2, 356 stored
// threads; 2026-11-24). Huffman squeeze, LZC, deflate and bzip2 are refused
// by name.

#ifndef POM2_NUFX_ARCHIVE_H
#define POM2_NUFX_ARCHIVE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2::nufx {

/// One record: a file (its data fork) or a disk image.
struct Record {
    std::string path;            ///< components joined with '/', high bits stripped
    uint16_t    fileSysId = 0;   ///< 1 ProDOS, 2 DOS 3.3, ... (File Type Note table)
    uint32_t    access = 0;
    uint32_t    fileType = 0;
    uint32_t    auxType = 0;
    uint16_t    storageType = 0; ///< for a disk: its block size
    bool        isDisk = false;
    bool        isDirectory = false;
    bool        hasResourceFork = false;
    std::vector<uint8_t> data;   ///< the data fork, or the disk image
    std::vector<uint8_t> resourceFork;  ///< GS/OS extended files only
};

/// True when the bytes hold a NuFX archive at offset 0, behind a Binary II
/// header (.bxy) or behind GS/OS's self-extracting stub (.sea).
bool looksLikeNuFx(const std::vector<uint8_t>& bytes);

/// Read every record. All-or-nothing: false with the reason on the first
/// damaged header, unsupported compression or failed CRC. `maxBytes` bounds
/// the uncompressed size of the whole archive.
bool readArchive(const std::vector<uint8_t>& bytes, std::vector<Record>& out,
                 std::string& error, std::size_t maxBytes = 64u << 20);

/// CRC-16/XMODEM (polynomial $1021, MSB first), the NuFX CRC.
uint16_t crc16(uint16_t seed, const uint8_t* data, std::size_t size);

/// Expand one LZW/1 (format 2) or LZW/2 (format 3) thread into exactly
/// `uncompressed` bytes. `checkChunkLength` false ignores the compressed
/// length LZW/2 stores per chunk (the "bad Mac" archives). Exposed for the
/// tests.
bool expandLzw(int format, const uint8_t* data, std::size_t size,
               std::size_t uncompressed, std::vector<uint8_t>& out,
               std::string& error, bool checkChunkLength = true);

} // namespace pom2::nufx

#endif // POM2_NUFX_ARCHIVE_H
