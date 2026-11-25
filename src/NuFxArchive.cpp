// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "NuFxArchive.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace pom2::nufx {
namespace {

// [B] File Type Note $E0/$8002: "NuFile" and "NuFX" with alternate high bits.
constexpr uint8_t kMasterId[6] = {0x4E, 0xF5, 0x46, 0xE9, 0x6C, 0xE5};
constexpr uint8_t kRecordId[4] = {0x4E, 0xF5, 0x46, 0xD8};
constexpr std::size_t kMasterHeaderBytes = 48;
constexpr std::size_t kRecordFixedBytes = 56;   // ID, CRC and attributes up to the dates
constexpr std::size_t kThreadHeaderBytes = 16;

// [D] NufxLib Archive.c: a Binary II header (.bxy) is one 128-byte block
//     before the archive; GS/ShrinkIt's self-extractor (.sea) puts it at
//     $2EE5. NufxLib's limits on a sane record (NufxLib.h).
constexpr uint8_t kBinary2Id[3] = {0x0A, 0x47, 0x4C};
constexpr std::size_t kBinary2Block = 128;
constexpr uint8_t kSeaId[3] = {0xA2, 0x2E, 0x00};
constexpr std::size_t kSeaOffset = 0x2EE5;
constexpr unsigned kMaxAttribCount = 256;
constexpr unsigned kMaxFilename = 1024;
constexpr unsigned kMaxThreads = 16;
constexpr unsigned kMaxRecordVersion = 3;

constexpr uint16_t kClassMessage = 0, kClassControl = 1, kClassData = 2, kClassFilename = 3;
constexpr uint16_t kKindDataFork = 0, kKindDiskImage = 1, kKindRsrcFork = 2;
constexpr uint16_t kFormatStored = 0, kFormatLzw1 = 2, kFormatLzw2 = 3;
constexpr uint16_t kFileSysDos33 = 2;
constexpr uint16_t kFileSysMacMfs = 6;
constexpr uint16_t kStorageDirectory = 0x0D;

constexpr std::size_t kChunk = 4096;   // ShrinkIt compresses 4 KiB at a time

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool archiveStart(const std::vector<uint8_t>& b, std::size_t& at)
{
    auto masterAt = [&](std::size_t o) {
        return b.size() >= o + kMasterHeaderBytes && std::memcmp(b.data() + o, kMasterId, 6) == 0;
    };
    if (masterAt(0)) { at = 0; return true; }
    // [D] NufxLib checks the Binary II "files to follow" byte (offset 127)
    //     is 0, so a .bny whose FIRST file happens to be an archive is not
    //     taken for a .bxy.
    if (b.size() > kBinary2Block && std::memcmp(b.data(), kBinary2Id, 3) == 0
            && b[kBinary2Block - 1] == 0 && masterAt(kBinary2Block)) {
        at = kBinary2Block;
        return true;
    }
    if (b.size() > kSeaOffset && std::memcmp(b.data(), kSeaId, 3) == 0 && masterAt(kSeaOffset)) {
        at = kSeaOffset;
        return true;
    }
    return false;
}

// ── ShrinkIt LZW ──────────────────────────────────────────────────────────
// [D] NufxLib Lzw.c. ShrinkIt compresses each 4 KiB chunk with RLE, then
// LZW of 9-12 bit codes, least significant bit first, the code width rising
// one code EARLY (the width is that of entry+1). LZW/1 starts a fresh table
// on every chunk and stores a CRC of the padded chunks in the stream; LZW/2
// carries the table from chunk to chunk, clears it on code $100, and stores
// each chunk's compressed length.

constexpr uint32_t kClearCode = 0x100;
constexpr uint32_t kFirstCode = 0x101;
constexpr uint32_t kTableTop = 0x1000;

class BitReader {
public:
    BitReader(const uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    // One code of the width `entry` calls for. False past the end.
    bool code(uint32_t entry, uint32_t& value)
    {
        static constexpr uint8_t kWidth[17] = {8, 9, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 12, 12, 12, 12, 12};
        const unsigned width = kWidth[std::min<uint32_t>((entry + 1) >> 8, 16)];
        if (bit_ + width > size_ * 8) return false;
        value = 0;
        for (unsigned i = 0; i < width; ++i, ++bit_)
            value |= static_cast<uint32_t>((data_[bit_ >> 3] >> (bit_ & 7)) & 1) << i;
        return true;
    }
    // Bytes the chunk used: a code ending mid-byte uses that whole byte.
    std::size_t consumed() const { return (bit_ + 7) / 8; }

private:
    const uint8_t* data_;
    std::size_t size_;
    std::size_t bit_ = 0;
};

struct LzwState {
    std::array<uint8_t, kTableTop> ch{};
    std::array<uint16_t, kTableTop> prefix{};
    uint32_t entry = kFirstCode;
    uint32_t oldcode = 0, finalc = 0;
    bool resetFix = false;   // [D] NufxLib: a clear as the chunk's second-to-last code
};

// Emit the string of `code` (KwKwK handled by the caller), return its first byte.
bool emitString(LzwState& s, uint32_t code, std::vector<uint8_t>& out, std::size_t limit)
{
    uint8_t stack[kTableTop];
    std::size_t depth = 0;
    while (code > 0xFF) {
        if (code >= kTableTop || depth >= sizeof(stack)) return false;
        stack[depth++] = s.ch[code];
        code = s.prefix[code];
    }
    s.finalc = code;
    if (out.size() + depth + 1 > limit) return false;
    out.push_back(static_cast<uint8_t>(code));
    while (depth) out.push_back(stack[--depth]);
    return true;
}

// Decode one chunk's codes into exactly `length` bytes appended to `out`.
bool lzwChunk(LzwState& s, bool type2, BitReader& in, std::size_t length,
              std::vector<uint8_t>& out, std::string& error)
{
    const std::size_t end = out.size() + length;
    auto bad = [&](const char* why) { error = why; return false; };
    uint32_t incode = 0;

    if (!type2) { s.entry = kFirstCode; s.resetFix = false; }
    bool fresh = s.entry == kFirstCode && !s.resetFix;
    s.resetFix = false;
    while (out.size() < end) {
        if (fresh) {
            // An empty table: the first code is a byte.
            s.entry = kFirstCode;
            if (!in.code(s.entry, incode)) return bad("LZW data ends early");
            if (incode > 0xFF) return bad("invalid first LZW code");
            out.push_back(static_cast<uint8_t>(incode));
            s.oldcode = s.finalc = incode;
            fresh = false;
            if (out.size() == end && type2) s.resetFix = true;
            continue;
        }
        if (!in.code(s.entry, incode)) return bad("LZW data ends early");
        if (type2 && incode == kClearCode) {
            fresh = true;
            s.entry = kFirstCode;
            if (out.size() == end) break;   // [D] a chunk that ends on a clear
            continue;
        }
        if (incode >= s.entry) {
            // KwKwK: the code being defined right now is oldcode's string
            // plus its own first byte. emitString leaves that byte in finalc.
            if (incode != s.entry) return bad("invalid LZW code");
            if (!emitString(s, s.oldcode, out, end) || out.size() >= end)
                return bad("LZW string overruns the chunk");
            out.push_back(static_cast<uint8_t>(s.finalc));
        } else if (!emitString(s, incode, out, end)) {
            return bad("LZW string overruns the chunk");
        }
        if (s.entry < kTableTop) {
            s.ch[s.entry] = static_cast<uint8_t>(s.finalc);
            s.prefix[s.entry] = static_cast<uint16_t>(s.oldcode);
            ++s.entry;
        }
        s.oldcode = incode;
    }
    return true;
}

// [D] NufxLib Nu_ExpandRLE: escape byte, value, count; count + 1 copies,
// always exactly one 4 KiB chunk out.
bool expandRle(const uint8_t* in, std::size_t size, uint8_t escape, uint8_t (&out)[kChunk],
               std::string& error)
{
    std::size_t o = 0, i = 0;
    while (o < kChunk) {
        if (i >= size) { error = "RLE data ends early"; return false; }
        const uint8_t c = in[i++];
        if (c != escape) { out[o++] = c; continue; }
        if (i + 2 > size) { error = "RLE data ends early"; return false; }
        const uint8_t value = in[i++];
        const std::size_t count = std::size_t(in[i++]) + 1;
        if (o + count > kChunk) { error = "RLE run overruns the chunk"; return false; }
        std::memset(out + o, value, count);
        o += count;
    }
    if (i != size) { error = "RLE data longer than its chunk"; return false; }
    return true;
}

} // namespace

uint16_t crc16(uint16_t seed, const uint8_t* data, std::size_t size)
{
    uint16_t crc = seed;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint16_t>(data[i] << 8);
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

bool looksLikeNuFx(const std::vector<uint8_t>& bytes)
{
    std::size_t at = 0;
    return archiveStart(bytes, at);
}

bool expandLzw(int format, const uint8_t* data, std::size_t size, std::size_t uncompressed,
               std::vector<uint8_t>& out, std::string& error, bool checkChunkLength)
{
    const bool type2 = format == kFormatLzw2;
    if (!type2 && format != kFormatLzw1) { error = "not an LZW thread"; return false; }
    out.clear();
    std::size_t p = 0;
    auto need = [&](std::size_t n) { return p + n <= size; };
    uint16_t streamCrc = 0;
    if (!type2) {
        if (!need(2)) { error = "LZW/1 thread too short"; return false; }
        streamCrc = rd16(data);
        p += 2;
    }
    if (!need(2)) { error = "LZW thread too short"; return false; }
    const uint8_t escape = data[p + 1];   // data[p] is the disk volume number
    p += 2;

    LzwState state;
    uint16_t chunkCrc = 0;
    std::vector<uint8_t> lzwOut;
    lzwOut.reserve(kChunk);
    uint8_t rleOut[kChunk];
    out.reserve(uncompressed);
    while (out.size() < uncompressed) {
        const std::size_t writeLen = std::min(kChunk, uncompressed - out.size());
        std::size_t rleLen = 0, lzwLen = 0;
        bool lzwUsed = false;
        if (type2) {
            if (!need(2)) { error = "LZW/2 chunk header missing"; return false; }
            const uint16_t h = rd16(data + p);
            p += 2;
            lzwUsed = (h & 0x8000) != 0;
            rleLen = h & 0x1FFF;
            if (lzwUsed) {
                if (!need(2)) { error = "LZW/2 chunk header missing"; return false; }
                lzwLen = rd16(data + p);
                p += 2;
                if (lzwLen < 4) { error = "LZW/2 chunk length invalid"; return false; }
                lzwLen -= 4;   // the length counts the 4 header bytes
            }
        } else {
            if (!need(3)) { error = "LZW/1 chunk header missing"; return false; }
            rleLen = rd16(data + p);
            const uint8_t flag = data[p + 2];
            p += 3;
            if (flag > 1) { error = "garbled LZW/1 chunk header"; return false; }
            lzwUsed = flag == 1;
        }
        if (rleLen == 0 || rleLen > kChunk) { error = "LZW chunk length invalid"; return false; }
        const bool rleUsed = rleLen != kChunk;

        const uint8_t* chunk = nullptr;
        if (lzwUsed) {
            BitReader bits(data + p, size - p);
            lzwOut.clear();
            if (!lzwChunk(state, type2, bits, rleLen, lzwOut, error)) return false;
            if (type2 && checkChunkLength && bits.consumed() != lzwLen) {
                error = "LZW/2 chunk does not use its stated length";
                return false;
            }
            p += bits.consumed();
            if (rleUsed) {
                if (!expandRle(lzwOut.data(), rleLen, escape, rleOut, error)) return false;
                chunk = rleOut;
            } else {
                chunk = lzwOut.data();
            }
        } else {
            // [D] NufxLib: a chunk stored without LZW resets the table, and
            //     holds rleLen bytes when RLE'd, else only the bytes written.
            const std::size_t inCount = rleUsed ? rleLen : writeLen;
            if (!need(inCount)) { error = "LZW thread ends early"; return false; }
            if (rleUsed) {
                if (!expandRle(data + p, rleLen, escape, rleOut, error)) return false;
                chunk = rleOut;
            } else {
                std::memset(rleOut, 0, kChunk);
                std::memcpy(rleOut, data + p, inCount);
                chunk = rleOut;
            }
            p += inCount;
            state.entry = kFirstCode;
            state.resetFix = false;
        }
        // [D] NufxLib: LZW/1's CRC covers each chunk padded to 4 KiB.
        if (!type2) chunkCrc = crc16(chunkCrc, chunk, kChunk);
        out.insert(out.end(), chunk, chunk + writeLen);
    }
    if (!type2 && chunkCrc != streamCrc) {
        error = "LZW/1 data fails its CRC";
        return false;
    }
    // ShrinkIt may leave one stray byte after the last chunk ([D] NufxLib).
    return true;
}

bool readArchive(const std::vector<uint8_t>& bytes, std::vector<Record>& out,
                 std::string& error, std::size_t maxBytes)
{
    out.clear();
    std::size_t start = 0;
    if (!archiveStart(bytes, start)) { error = "not a ShrinkIt (NuFX) archive"; return false; }
    const uint8_t* b = bytes.data();
    const std::size_t size = bytes.size();
    const uint32_t totalRecords = rd32(b + start + 8);
    std::size_t p = start + kMasterHeaderBytes;
    std::size_t total = 0;

    for (uint32_t r = 0; r < totalRecords; ++r) {
        const std::string where = "record " + std::to_string(r + 1);
        const std::size_t recordAt = p;
        if (p + kRecordFixedBytes > size || std::memcmp(b + p, kRecordId, 4) != 0) {
            error = where + ": record header not found";
            return false;
        }
        const uint16_t headerCrc = rd16(b + p + 4);
        const uint16_t attribCount = rd16(b + p + 6);
        const uint16_t version = rd16(b + p + 8);
        const uint32_t threads = rd32(b + p + 10);
        Record rec;
        rec.fileSysId = rd16(b + p + 14);
        const uint16_t fileSysInfo = rd16(b + p + 16);
        rec.access = rd32(b + p + 18);
        rec.fileType = rd32(b + p + 22);
        rec.auxType = rd32(b + p + 26);
        rec.storageType = rd16(b + p + 30);
        if (attribCount > kMaxAttribCount || attribCount < kRecordFixedBytes - 4 || version > kMaxRecordVersion
                || threads > kMaxThreads) {
            error = where + ": damaged record header";
            return false;
        }
        // The attribute area ends at attribCount (counted from the ID) minus
        // the 2-byte filename length that follows it; the option list and
        // any extra bytes live inside it ([D] NufxLib Record.c).
        p = recordAt + attribCount - 2;
        if (p + 2 > size) { error = where + ": record header truncated"; return false; }
        const uint16_t nameLength = rd16(b + p);
        p += 2;
        if (nameLength > kMaxFilename || p + nameLength > size) {
            error = where + ": damaged file name";
            return false;
        }
        std::string name(reinterpret_cast<const char*>(b + p), nameLength);
        p += nameLength;
        if (p + std::size_t(threads) * kThreadHeaderBytes > size) {
            error = where + ": thread headers truncated";
            return false;
        }
        // [B] The header CRC covers everything after itself up to the end
        //     of the thread headers.
        const std::size_t threadsAt = p;
        const std::size_t headerEnd = threadsAt + std::size_t(threads) * kThreadHeaderBytes;
        if (crc16(0, b + recordAt + 6, headerEnd - recordAt - 6) != headerCrc) {
            error = where + ": record header fails its CRC";
            return false;
        }

        std::size_t data = headerEnd;
        for (uint32_t t = 0; t < threads; ++t) {
            const uint8_t* h = b + threadsAt + t * kThreadHeaderBytes;
            const uint16_t cls = rd16(h), format = rd16(h + 2), kind = rd16(h + 4), crc = rd16(h + 6);
            const uint32_t eof = rd32(h + 8), compEof = rd32(h + 12);
            // By subtraction: on a 32-bit size_t the sum wrapped past the check.
            if (data > size || compEof > size - data) { error = where + ": thread data truncated"; return false; }
            const uint8_t* payload = b + data;
            data += compEof;

            if (cls == kClassFilename) {
                name.assign(reinterpret_cast<const char*>(payload), std::min<std::size_t>(eof, compEof));
                continue;
            }
            if (cls == kClassControl && kind == 0) { rec.isDirectory = true; continue; }
            if (cls == kClassMessage || cls != kClassData) continue;
            if (kind != kKindDataFork && kind != kKindDiskImage && kind != kKindRsrcFork) continue;

            // In 64 bits whatever size_t is: on wasm32 and armhf the product
            // and the sum wrapped, passed the limit, and reserve() threw —
            // which ends the page in RetroArch's web build (bug hunt 2026-11-25).
            std::uint64_t wide = eof;
            if (kind == kKindDiskImage) {
                rec.isDisk = true;
                // [D] NufxLib Thread.c: ShrinkIt 3.0.1 stored a wrong EOF for
                //     disks; the size is blocks x block size, with block
                //     sizes under 14 meaning 512, and an old GS/ShrinkIt's
                //     "256-byte blocks" on a 280-block DOS 3.3 disk too.
                if (rec.storageType <= 13
                        || (rec.storageType == 256 && rec.auxType == 280 && rec.fileSysId == kFileSysDos33))
                    wide = std::uint64_t(rec.auxType) * 512;
                else
                    wide = std::uint64_t(rec.auxType) * rec.storageType;
            }
            if (total > maxBytes || wide > std::uint64_t(maxBytes - total)) {
                error = "the archive expands past the size limit"; return false;
            }
            const std::size_t length = static_cast<std::size_t>(wide);

            std::vector<uint8_t> expanded;
            if (format == kFormatStored) {
                if (compEof < length) { error = where + ": stored thread truncated"; return false; }
                expanded.assign(payload, payload + length);
            } else if (format == kFormatLzw1 || format == kFormatLzw2) {
                // [D] NufxLib Record.c: a "bad Mac" archive (Mac MFS, '?' as
                //     separator) may store LZW/2 chunk lengths wrong; its
                //     LZW stream delimits itself, so the length is ignored.
                const bool badMac = rec.fileSysId == kFileSysMacMfs && (fileSysInfo & 0xFF) == '?';
                std::string why;
                if (!expandLzw(format, payload, compEof, length, expanded, why, !badMac)) {
                    error = where + ": " + why;
                    return false;
                }
            } else {
                static const char* const kNames[] = {"stored", "Huffman squeeze", "LZW/1", "LZW/2",
                                                     "LZC 12-bit", "LZC 16-bit", "deflate", "bzip2"};
                error = where + ": " + (format < 8 ? kNames[format] : "unknown")
                      + " compression is not supported";
                return false;
            }
            // [D] NufxLib Thread.c: only record version 3 carries the CRC of
            //     the uncompressed data in the thread header.
            if (version >= 3 && crc16(0xFFFF, expanded.data(), expanded.size()) != crc) {
                error = where + ": data fails its CRC";
                return false;
            }
            total += length;
            if (kind == kKindRsrcFork) {
                rec.hasResourceFork = true;
                rec.resourceFork = std::move(expanded);
            } else {
                rec.data = std::move(expanded);
            }
        }
        p = data;

        // The path: high bits stripped when every byte has one, components
        // split on the file system's separator (low byte of fileSysInfo).
        if (!name.empty() && std::all_of(name.begin(), name.end(), [](char c) { return (c & 0x80) != 0; }))
            for (char& c : name) c = static_cast<char>(c & 0x7F);
        const char sep = static_cast<char>(fileSysInfo & 0x7F);
        if (sep && sep != '/') std::replace(name.begin(), name.end(), sep, '/');
        rec.path = name;
        if (rec.storageType == kStorageDirectory && !rec.isDisk) rec.isDirectory = true;
        out.push_back(std::move(rec));
    }
    return true;
}

} // namespace pom2::nufx
