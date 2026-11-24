// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// DiskCopy42 — reading Apple's DiskCopy 4.2 disk image (.dc, .dc42, and
// many .image files): an 84-byte header, the sectors, then the tag bytes.
//
// [B] Apple II File Type Note $E0/$0005, "Macintosh DiskCopy 4.2 disk
//     image" — the header layout, the format byte and the checksum.
//
// The sectors are stored in logical block order, which is ProDOS block order
// for an Apple II 800 KB disk: unwrapping gives a .po. The tags (12 bytes a
// sector on a 400/800 KB GCR disk) are Macintosh file-system bookkeeping no
// Apple II controller reads, so they are checked and dropped.

#ifndef POM2_DISKCOPY42_H
#define POM2_DISKCOPY42_H

#include <cstdint>
#include <string>
#include <vector>

namespace pom2::diskcopy42 {

constexpr std::size_t kHeaderBytes = 84;

/// The header is a DiskCopy 4.2 one: the $0100 signature, a data size one of
/// the four the format defines (400, 800, 720 or 1440 KB), and a file exactly
/// header + data + tags long. Says nothing about the checksums.
bool looksLikeDiskCopy42(const std::vector<uint8_t>& bytes);

/// The disk's name, from the header's Pascal string (empty if none).
std::string diskName(const std::vector<uint8_t>& bytes);

/// [B] The DiskCopy checksum: each big-endian 16-bit word added, then the
/// 32-bit sum rotated right by one.
uint32_t checksum(const uint8_t* data, std::size_t size);

/// The sectors, block-ordered (`blocks` out), after both checksums pass.
/// False with the reason otherwise.
bool unwrap(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& blocks,
            std::string& error);

} // namespace pom2::diskcopy42

#endif // POM2_DISKCOPY42_H
