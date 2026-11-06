// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

// apple_single — AppleSingle decode/encode (AppleSingle.h).
//
// The oracle is [B] Apple's "AppleSingle/AppleDouble Formats for Foreign
// Files Developer's Note" (1990), Chapter 2: the hand-built file below is
// laid out from Table 2-1 and Figure 2-3 byte by byte, not from encode().
// The cases ported from [D] Casso UnitTest/EmuTests/AppleSingleCodecTests.cpp
// @ 2d899adf (MIT) are marked; Casso's file-dates cases are not ported (POM2's
// File carries no dates). The hostile inputs are POM2's: the file is one a
// user drops on the window, and a decode must neither read outside it nor
// allocate on the word of a length field.

#include "AppleSingle.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

using Bytes = std::vector<uint8_t>;
namespace as = pom2::applesingle;

void put32(Bytes& v, uint32_t x)
{
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));  v.push_back(uint8_t(x));
}

/// A header with `count` entries and no descriptors yet.
Bytes header(uint16_t count, uint32_t version = as::kVersion2)
{
    Bytes v;
    put32(v, as::kMagic);
    put32(v, version);
    v.insert(v.end(), 16, 0);
    v.push_back(uint8_t(count >> 8));
    v.push_back(uint8_t(count));
    return v;
}

void entry(Bytes& v, uint32_t id, uint32_t offset, uint32_t length)
{
    put32(v, id); put32(v, offset); put32(v, length);
}

/// A refused decode: false, a message, `out` untouched. Returns the message.
std::string refused(const Bytes& bytes)
{
    as::File out;
    out.data = { 0xDE, 0xAD };
    out.realName = "SENTINEL";
    out.fileType = 0x1234;
    std::string error;
    const bool ok = as::decode(bytes, out, error);
    CHECK(!ok);
    CHECK(!error.empty());
    CHECK(out.data == (Bytes{ 0xDE, 0xAD }));
    CHECK(out.realName == "SENTINEL" && out.fileType == 0x1234 && !out.hasProdosInfo);
    return error;
}

bool contains(const std::string& s, const char* what)
{
    return s.find(what) != std::string::npos;
}

// --- Casso's cases ----------------------------------------------------------

void testCassoRoundTrip()
{
    as::File original;
    original.data = { 0xA9, 0x41, 0x60 };
    original.realName = "PROG";
    original.hasProdosInfo = true;
    original.access = 0x00C3;
    original.fileType = 0x0006;
    original.auxType = 0x00000300;
    const Bytes bytes = as::encode(original);
    CHECK(as::looksLikeAppleSingle(bytes.data(), bytes.size()));
    CHECK(bytes[0] == 0x00 && bytes[1] == 0x05 && bytes[2] == 0x16 && bytes[3] == 0x00);
    as::File decoded;
    std::string error;
    CHECK(as::decode(bytes, decoded, error));
    CHECK(decoded.data == original.data);
    CHECK(decoded.realName == "PROG");
    CHECK(decoded.hasProdosInfo);
    CHECK(decoded.fileType == 0x0006 && decoded.auxType == 0x0300 && decoded.access == 0x00C3);
    CHECK(decoded.resource.empty());
}

void testCassoDataForkOnly()
{
    as::File original;
    original.data = { 1, 2, 3, 4 };
    const Bytes bytes = as::encode(original);
    CHECK(bytes.size() == 26 + 12 + 4);
    as::File decoded;
    std::string error;
    CHECK(as::decode(bytes, decoded, error));
    CHECK(decoded.data.size() == 4);
    CHECK(decoded.realName.empty());
    CHECK(!decoded.hasProdosInfo);
}

void testCassoTruncatedAndVersions()
{
    as::File original;
    original.data = { 1, 2, 3, 4, 5, 6, 7, 8 };
    original.realName = "X";
    Bytes bytes = as::encode(original);
    CHECK(bytes.size() == 26 + 24 + 8 + 1);

    CHECK(contains(refused(Bytes(bytes.begin(), bytes.begin() + 40)), "entry table"));
    CHECK(contains(refused(Bytes(bytes.begin(), bytes.begin() + 55)), "entry 1"));

    bytes[5] = 0x03;
    CHECK(contains(refused(bytes), "version"));
    CHECK(!as::looksLikeAppleSingle(bytes.data(), bytes.size()));

    bytes[5] = 0x01;                              // version 1 reads the same entries
    as::File decoded;
    std::string error;
    CHECK(as::looksLikeAppleSingle(bytes.data(), bytes.size()));
    CHECK(as::decode(bytes, decoded, error));
    CHECK(decoded.data.size() == 8 && decoded.realName == "X");

    const Bytes code = { 0xA9, 0x41, 0x60 };
    CHECK(!as::looksLikeAppleSingle(code.data(), code.size()));
    refused(code);
}

// --- From the document ------------------------------------------------------

void testHandBuilt()
{
    // [B] Table 2-1: magic, version, 16 zero bytes, count = 3; then the
    // descriptors (ID, offset, length); then the entries, data fork last.
    const Bytes expected = {
        0x00, 0x05, 0x16, 0x00,  0x00, 0x02, 0x00, 0x00,
        0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
        0x00, 0x03,
        0x00, 0x00, 0x00, 0x03,  0x00, 0x00, 0x00, 0x3E,  0x00, 0x00, 0x00, 0x05,  // name @62
        0x00, 0x00, 0x00, 0x0B,  0x00, 0x00, 0x00, 0x43,  0x00, 0x00, 0x00, 0x08,  // info @67
        0x00, 0x00, 0x00, 0x01,  0x00, 0x00, 0x00, 0x4B,  0x00, 0x00, 0x00, 0x03,  // data @75
        'H', 'E', 'L', 'L', 'O',
        0x00, 0xC3,  0x00, 0x06,  0x00, 0x00, 0x20, 0x00,  // [B] Figure 2-3
        0xA9, 0x00, 0x60,
    };
    as::File f;
    f.data = { 0xA9, 0x00, 0x60 };
    f.realName = "HELLO";
    f.hasProdosInfo = true;
    f.access = 0xC3;
    f.fileType = 0x06;
    f.auxType = 0x2000;
    CHECK(as::encode(f) == expected);

    as::File d;
    std::string error;
    CHECK(as::decode(expected, d, error));
    CHECK(d.data == f.data && d.realName == "HELLO" && d.hasProdosInfo);
    CHECK(d.access == 0xC3 && d.fileType == 0x06 && d.auxType == 0x2000);

    // The same file with its entries in another order, and a hole: order is
    // free and holes are allowed ([B] § File layout).
    Bytes shuffled = header(3);
    entry(shuffled, 1, 70, 3);
    entry(shuffled, 11, 80, 8);
    entry(shuffled, 3, 62, 5);
    shuffled.resize(88, 0xEE);
    const uint8_t name[] = { 'H', 'E', 'L', 'L', 'O' };
    const uint8_t data[] = { 0xA9, 0x00, 0x60 };
    const uint8_t info[] = { 0x00, 0xC3, 0x00, 0x06, 0x00, 0x00, 0x20, 0x00 };
    std::copy(name, name + 5, shuffled.begin() + 62);
    std::copy(data, data + 3, shuffled.begin() + 70);
    std::copy(info, info + 8, shuffled.begin() + 80);
    as::File s;
    CHECK(as::decode(shuffled, s, error));
    CHECK(s.data == f.data && s.realName == "HELLO" && s.fileType == 0x06 && s.auxType == 0x2000);
}

void testRoundTrips()
{
    as::File f;
    f.data.resize(5000);
    for (std::size_t i = 0; i < f.data.size(); ++i) f.data[i] = uint8_t(i * 7);
    f.resource = { 9, 8, 7 };
    f.realName = "A.LONGER.NAME";
    f.hasProdosInfo = true;
    f.access = 0x21;
    f.fileType = 0xFF;
    f.auxType = 0x00012000;       // high word kept, even though ProDOS 8 ignores it
    as::File d;
    std::string error;
    CHECK(as::decode(as::encode(f), d, error));
    CHECK(d.data == f.data && d.resource == f.resource && d.realName == f.realName);
    CHECK(d.hasProdosInfo && d.access == 0x21 && d.fileType == 0xFF && d.auxType == 0x00012000);
    CHECK(as::encode(d) == as::encode(f));

    // Empty data fork: still written, decodes empty.
    as::File empty;
    const Bytes e = as::encode(empty);
    CHECK(e.size() == 26 + 12);
    as::File de;
    de.data = { 1 };
    CHECK(as::decode(e, de, error));
    CHECK(de.data.empty() && !de.hasProdosInfo);
}

// --- Hostile input ----------------------------------------------------------

void testHostile()
{
    // Truncated header.
    Bytes h = header(1);
    h.pop_back();
    CHECK(!as::looksLikeAppleSingle(h.data(), h.size()));
    CHECK(contains(refused(h), "short"));
    CHECK(!as::looksLikeAppleSingle(nullptr, 100));

    // Merely 26 bytes of header: magic, version, no entries.
    const Bytes bare = header(0);
    CHECK(bare.size() == 26);
    CHECK(as::looksLikeAppleSingle(bare.data(), bare.size()));
    CHECK(contains(refused(bare), "no entries"));
    // 26 bytes that are the magic and nothing else.
    Bytes magicOnly(26, 0);
    magicOnly[1] = 0x05; magicOnly[2] = 0x16;
    CHECK(!as::looksLikeAppleSingle(magicOnly.data(), magicOnly.size()));
    CHECK(contains(refused(magicOnly), "version"));

    // Entry count 65535 in a small file: refused before any table is read.
    Bytes many = header(0xFFFF);
    entry(many, 1, 38, 0);
    CHECK(contains(refused(many), "65535"));
    // A count within the limit whose table is not in the file.
    Bytes table = header(10);
    entry(table, 1, 38, 0);
    CHECK(contains(refused(table), "entry table"));

    // Offset past the end.
    Bytes past = header(1);
    entry(past, 1, 1000, 1);
    past.push_back(0);
    CHECK(contains(refused(past), "entry 1"));
    // ...even for an empty entry.
    Bytes pastEmpty = header(1);
    entry(pastEmpty, 1, 1000, 0);
    CHECK(contains(refused(pastEmpty), "past the end"));

    // offset + length wraps to a small number in 32 bits: $FFFFFFF0 + $20.
    Bytes wrap = header(1);
    entry(wrap, 1, 0xFFFFFFF0u, 0x20);
    wrap.resize(64, 0);
    CHECK(contains(refused(wrap), "entry 1"));
    // A huge length must not be allocated on trust.
    Bytes huge = header(1);
    entry(huge, 1, 38, 0xFFFFFFFFu);
    huge.resize(64, 0);
    CHECK(contains(refused(huge), "entry 1"));

    // Two data forks.
    Bytes two = header(2);
    entry(two, 1, 50, 2);
    entry(two, 1, 52, 2);
    two.resize(54, 0x11);
    CHECK(contains(refused(two), "entry 1 (data fork) appears twice"));

    // Overlapping entries, named both.
    Bytes overlap = header(2);
    entry(overlap, 1, 50, 4);
    entry(overlap, 3, 52, 4);
    overlap.resize(56, 'N');
    const std::string why = refused(overlap);
    CHECK(contains(why, "entry 1") && contains(why, "entry 3") && contains(why, "overlaps"));

    // An entry inside the descriptor table.
    Bytes inside = header(1);
    entry(inside, 3, 4, 4);
    CHECK(contains(refused(inside), "header"));

    // Entry ID 0 is invalid ([B] § Entry descriptors).
    Bytes zero = header(1);
    entry(zero, 0, 38, 0);
    CHECK(contains(refused(zero), "ID 0"));

    // A ProDOS info entry too short for its three fields.
    Bytes shortInfo = header(1);
    entry(shortInfo, 11, 38, 4);
    shortInfo.insert(shortInfo.end(), { 0, 0xC3, 0, 6 });
    CHECK(contains(refused(shortInfo), "entry 11"));

    // Zero-length data fork, at the very end of the file: accepted.
    Bytes zeroLen = header(1);
    entry(zeroLen, 1, 38, 0);
    as::File z;
    std::string error;
    CHECK(as::decode(zeroLen, z, error));
    CHECK(z.data.empty());

    // Unknown and application-defined entries are ignored, not refused.
    Bytes unknown = header(2);
    entry(unknown, 0x80000001u, 50, 2);
    entry(unknown, 1, 52, 1);
    unknown.insert(unknown.end(), { 0xAB, 0xCD, 0x60 });
    as::File u;
    CHECK(as::decode(unknown, u, error));
    CHECK(u.data == (Bytes{ 0x60 }));
}

void testVersion1()
{
    // GS/ShrinkIt's shape: home file system "ProDOS" in the filler, entry 7
    // carrying dates, access, type and aux ([D] CiderPress II @ 49b67b40).
    Bytes v1 = header(3, as::kVersion1);
    const char home[] = "ProDOS          ";
    std::copy(home, home + 16, v1.begin() + 8);
    entry(v1, 3, 62, 4);
    entry(v1, 7, 66, 16);
    entry(v1, 1, 82, 2);
    v1.insert(v1.end(), { 'T', 'E', 'S', 'T' });
    v1.insert(v1.end(), { 0x12, 0x34, 0x56, 0x78,  0x12, 0x34, 0x56, 0x78,   // dates
                          0x00, 0xE3,  0x00, 0x06,  0x00, 0x00, 0x08, 0x00 });
    v1.insert(v1.end(), { 0xEA, 0x60 });
    CHECK(as::looksLikeAppleSingle(v1.data(), v1.size()));
    as::File f;
    std::string error;
    CHECK(as::decode(v1, f, error));
    CHECK(f.realName == "TEST" && f.data == (Bytes{ 0xEA, 0x60 }));
    CHECK(f.hasProdosInfo && f.access == 0xE3 && f.fileType == 0x06 && f.auxType == 0x0800);

    // Another home file system: entry 7 is not a ProDOS one and is ignored.
    Bytes mac = v1;
    const char macHome[] = "Macintosh       ";
    std::copy(macHome, macHome + 16, mac.begin() + 8);
    as::File m;
    CHECK(as::decode(mac, m, error));
    CHECK(!m.hasProdosInfo && m.data == (Bytes{ 0xEA, 0x60 }));

    // A ProDOS entry 7 too short for its fields is refused.
    Bytes shortV1 = v1;
    shortV1[26 + 12 + 11] = 12;                 // entry 7's length
    CHECK(contains(refused(shortV1), "entry 7"));
}

} // namespace

int main()
{
    testCassoRoundTrip();
    testCassoDataForkOnly();
    testCassoTruncatedAndVersions();
    testHandBuilt();
    testRoundTrips();
    testHostile();
    testVersion1();
    if (g_fail) {
        std::fprintf(stderr, "apple_single: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("apple_single: OK\n");
    return 0;
}
