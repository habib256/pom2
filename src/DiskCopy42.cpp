// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DiskCopy42.h"

namespace pom2::diskcopy42 {
namespace {

uint32_t be32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// [B] File Type Note $E0/$0005: offsets in the 84-byte header.
constexpr std::size_t kName = 0;          // Pascal string, 64 bytes
constexpr std::size_t kDataSize = 64;
constexpr std::size_t kTagSize = 68;
constexpr std::size_t kDataChecksum = 72;
constexpr std::size_t kTagChecksum = 76;
constexpr std::size_t kPrivate = 82;      // $0100: "this is a DiskCopy 4.2 image"

bool knownSize(uint32_t data)
{
    return data == 409600 || data == 819200 || data == 737280 || data == 1474560;
}

} // namespace

bool looksLikeDiskCopy42(const std::vector<uint8_t>& b)
{
    if (b.size() < kHeaderBytes || b[kPrivate] != 0x01 || b[kPrivate + 1] != 0x00 || b[kName] > 63)
        return false;
    const uint32_t data = be32(b.data() + kDataSize), tags = be32(b.data() + kTagSize);
    return knownSize(data) && tags <= data && b.size() == kHeaderBytes + std::size_t(data) + tags;
}

std::string diskName(const std::vector<uint8_t>& b)
{
    if (b.size() < kHeaderBytes || b[kName] > 63) return {};
    return std::string(reinterpret_cast<const char*>(b.data() + kName + 1), b[kName]);
}

uint32_t checksum(const uint8_t* data, std::size_t size)
{
    uint32_t sum = 0;
    for (std::size_t i = 0; i + 1 < size; i += 2) {
        sum += (uint32_t(data[i]) << 8) | data[i + 1];
        sum = (sum >> 1) | (sum << 31);
    }
    return sum;
}

bool unwrap(const std::vector<uint8_t>& b, std::vector<uint8_t>& blocks, std::string& error)
{
    if (!looksLikeDiskCopy42(b)) {
        error = "not a DiskCopy 4.2 image";
        return false;
    }
    const uint32_t dataSize = be32(b.data() + kDataSize), tagSize = be32(b.data() + kTagSize);
    const uint8_t* data = b.data() + kHeaderBytes;
    if (checksum(data, dataSize) != be32(b.data() + kDataChecksum)) {
        error = "the DiskCopy image's sectors fail their checksum";
        return false;
    }
    // [B] The tag checksum leaves out the first 12 bytes of tag data (the
    //     first sector's tags), which DiskCopy 4.0 did not checksum.
    if (tagSize > 12 && checksum(data + dataSize + 12, tagSize - 12) != be32(b.data() + kTagChecksum)) {
        error = "the DiskCopy image's tags fail their checksum";
        return false;
    }
    blocks.assign(data, data + dataSize);
    return true;
}

} // namespace pom2::diskcopy42
