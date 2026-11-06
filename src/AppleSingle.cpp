// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

// AppleSingle decode/encode — see AppleSingle.h for the format, its sources
// and why a decode is as strict as it is.

#include "AppleSingle.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace pom2::applesingle {

namespace {

constexpr std::size_t kHeaderSize      = 26;   // magic 4, version 4, filler 16, count 2
constexpr std::size_t kFillerOffset    = 8;
constexpr std::size_t kFillerSize      = 16;
constexpr std::size_t kEntrySize       = 12;   // ID 4, offset 4, length 4
constexpr uint32_t    kIdDataFork      = 1;
constexpr uint32_t    kIdResourceFork  = 2;
constexpr uint32_t    kIdRealName      = 3;
constexpr uint32_t    kIdFileInfoV1    = 7;
constexpr uint32_t    kIdProdosInfo    = 11;
constexpr std::size_t kProdosInfoSize  = 8;    // [B] Figure 2-3
constexpr std::size_t kFileInfoV1Size  = 16;   // [D] CiderPress II, see the header
// [D] CiderPress II FS_PRODOS: the v1 home file system, blank-padded to 16.
constexpr char kHomeProdos[kFillerSize + 1] = "ProDOS          ";

uint32_t be32(const uint8_t* p)
{
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

uint16_t be16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] << 8 | p[1]);
}

void put32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

void put16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

std::string entryName(uint32_t id)
{
    std::string s = "entry " + std::to_string(id);
    switch (id) {
    case kIdDataFork:     return s + " (data fork)";
    case kIdResourceFork: return s + " (resource fork)";
    case kIdRealName:     return s + " (real name)";
    case kIdFileInfoV1:   return s + " (file info)";
    case kIdProdosInfo:   return s + " (ProDOS file info)";
    default:              return s;
    }
}

struct Entry {
    uint32_t id;
    uint32_t offset;
    uint32_t length;
};

} // namespace

bool looksLikeAppleSingle(const uint8_t* bytes, std::size_t size)
{
    if (!bytes || size < kHeaderSize) return false;
    const uint32_t version = be32(bytes + 4);
    return be32(bytes) == kMagic && (version == kVersion1 || version == kVersion2);
}

bool decode(const std::vector<uint8_t>& bytes, File& out, std::string& error)
{
    const uint8_t* const p = bytes.data();
    const uint64_t size = bytes.size();

    if (size < kHeaderSize) {
        error = "too short for an AppleSingle header (" + std::to_string(size) + " of "
              + std::to_string(kHeaderSize) + " bytes)";
        return false;
    }
    if (be32(p) != kMagic) {
        error = "not an AppleSingle file (no $00051600 magic)";
        return false;
    }
    const uint32_t version = be32(p + 4);
    if (version != kVersion1 && version != kVersion2) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "$%08X", static_cast<unsigned>(version));
        error = std::string("AppleSingle version ") + buf + " is not 1 or 2";
        return false;
    }

    const std::size_t count = be16(p + 24);
    if (count == 0) {
        error = "AppleSingle file with no entries";
        return false;
    }
    if (count > kMaxEntries) {
        error = "AppleSingle file claims " + std::to_string(count) + " entries (at most "
              + std::to_string(kMaxEntries) + " accepted)";
        return false;
    }
    const uint64_t tableEnd = kHeaderSize + uint64_t(count) * kEntrySize;
    if (tableEnd > size) {
        error = "the AppleSingle entry table runs past the end of the file";
        return false;
    }

    // Read and check every descriptor before touching any entry's data.
    std::vector<Entry> entries;
    entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const uint8_t* d = p + kHeaderSize + i * kEntrySize;
        const Entry e{ be32(d), be32(d + 4), be32(d + 8) };
        if (e.id == 0) {
            error = "AppleSingle entry ID 0 is invalid";
            return false;
        }
        for (const Entry& seen : entries) {
            if (seen.id == e.id) {
                error = entryName(e.id) + " appears twice";
                return false;
            }
        }
        if (uint64_t(e.offset) + e.length > size) {
            error = entryName(e.id) + " runs past the end of the file";
            return false;
        }
        if (e.length > 0 && e.offset < tableEnd) {
            error = entryName(e.id) + " lies inside the AppleSingle header";
            return false;
        }
        entries.push_back(e);
    }

    // No two entries may share a byte (an empty entry has no bytes).
    std::vector<Entry> spans;
    for (const Entry& e : entries)
        if (e.length > 0) spans.push_back(e);
    std::sort(spans.begin(), spans.end(),
              [](const Entry& a, const Entry& b) { return a.offset < b.offset; });
    for (std::size_t i = 1; i < spans.size(); ++i) {
        if (uint64_t(spans[i - 1].offset) + spans[i - 1].length > spans[i].offset) {
            error = entryName(spans[i - 1].id) + " overlaps " + entryName(spans[i].id);
            return false;
        }
    }

    // Every range is now known to be inside `bytes`: build the result.
    const bool homeProdos = version == kVersion1
        && std::memcmp(p + kFillerOffset, kHomeProdos, kFillerSize) == 0;
    File f;
    bool haveV2Info = false;
    for (const Entry& e : entries) {
        const uint8_t* at = p + e.offset;
        switch (e.id) {
        case kIdDataFork:     f.data.assign(at, at + e.length); break;
        case kIdResourceFork: f.resource.assign(at, at + e.length); break;
        case kIdRealName:     f.realName.assign(reinterpret_cast<const char*>(at), e.length); break;
        case kIdProdosInfo:
            if (e.length < kProdosInfoSize) {
                error = entryName(e.id) + " is " + std::to_string(e.length)
                      + " bytes; it needs " + std::to_string(kProdosInfoSize);
                return false;
            }
            f.hasProdosInfo = true;
            f.access   = be16(at);
            f.fileType = be16(at + 2);
            f.auxType  = be32(at + 4);
            haveV2Info = true;
            break;
        case kIdFileInfoV1:
            // Its layout depends on the home file system; only ProDOS's is
            // read. Entry 11, if a file has both, is the more specific one.
            if (!homeProdos || haveV2Info) break;
            if (e.length < kFileInfoV1Size) {
                error = entryName(e.id) + " is " + std::to_string(e.length)
                      + " bytes; a ProDOS one needs " + std::to_string(kFileInfoV1Size);
                return false;
            }
            f.hasProdosInfo = true;
            f.access   = be16(at + 8);
            f.fileType = be16(at + 10);
            f.auxType  = be32(at + 12);
            break;
        default:
            break;   // [B] § Usage: unknown entries are ignored
        }
    }
    out = std::move(f);
    return true;
}

std::vector<uint8_t> encode(const File& file)
{
    struct Part {
        uint32_t id;
        const uint8_t* bytes;
        std::size_t size;
    };
    std::vector<uint8_t> info;                 // [B] Figure 2-3
    put16(info, file.access);
    put16(info, file.fileType);
    put32(info, file.auxType);
    std::vector<Part> parts;
    if (!file.realName.empty())
        parts.push_back({ kIdRealName, reinterpret_cast<const uint8_t*>(file.realName.data()),
                          file.realName.size() });
    if (file.hasProdosInfo)
        parts.push_back({ kIdProdosInfo, info.data(), info.size() });
    if (!file.resource.empty())
        parts.push_back({ kIdResourceFork, file.resource.data(), file.resource.size() });
    parts.push_back({ kIdDataFork, file.data.data(), file.data.size() });

    std::vector<uint8_t> out;
    put32(out, kMagic);
    put32(out, kVersion2);
    out.insert(out.end(), kFillerSize, 0);
    put16(out, static_cast<uint16_t>(parts.size()));
    uint32_t offset = static_cast<uint32_t>(kHeaderSize + parts.size() * kEntrySize);
    for (const Part& part : parts) {
        put32(out, part.id);
        put32(out, offset);
        put32(out, static_cast<uint32_t>(part.size));
        offset += static_cast<uint32_t>(part.size);
    }
    for (const Part& part : parts)
        if (part.size) out.insert(out.end(), part.bytes, part.bytes + part.size);
    return out;
}

} // namespace pom2::applesingle
