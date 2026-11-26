// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// SevenZipArchive — see SevenZipArchive.h.

// The SDK's names, renamed as its objects were compiled (cmake/Pom2Lzma.cmake):
// first, before any SDK header.
#include "third_party/lzma/Pom2LzmaPrefix.h"

#include "SevenZipArchive.h"

#include "third_party/lzma/7z.h"
#include "third_party/lzma/7zCrc.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace pom2 {

namespace {

// [B] 7-Zip 24.09 DOC/7zFormat.txt, "SignatureHeader": '7' 'z' BC AF 27 1C,
// then the format version.
constexpr std::uint8_t kSignature[6] = { '7', 'z', 0xBC, 0xAF, 0x27, 0x1C };

// The most the decoder's own state may take (a PPMd model): above 7-Zip's
// largest preset (-mx9, 192 MB), far below a hostile header's 4 GiB.
constexpr std::size_t kMaxModelBytes = std::size_t{256} << 20;

// The SDK's input: the archive in memory, behind its ISeekInStream.
struct MemoryStream {
    ISeekInStream vt;
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;
};

SRes memoryRead(ISeekInStreamPtr pp, void* buf, size_t* size)
{
    // vt is the first member: the SDK hands back the address it was given.
    auto* p = reinterpret_cast<MemoryStream*>(const_cast<ISeekInStream*>(pp));
    const std::size_t n = std::min(*size, p->size - p->pos);
    if (n) std::memcpy(buf, p->data + p->pos, n);
    p->pos += n;
    *size = n;
    return SZ_OK;
}

SRes memorySeek(ISeekInStreamPtr pp, Int64* pos, ESzSeek origin)
{
    auto* p = reinterpret_cast<MemoryStream*>(const_cast<ISeekInStream*>(pp));
    const Int64 base = origin == SZ_SEEK_SET ? 0
                     : origin == SZ_SEEK_CUR ? static_cast<Int64>(p->pos)
                                             : static_cast<Int64>(p->size);
    // A header offset is a 64-bit field the archive chose: no overflow here.
    if ((*pos > 0 && base > INT64_MAX - *pos) || base + *pos < 0
            || static_cast<std::uint64_t>(base + *pos) > p->size)
        return SZ_ERROR_READ;
    p->pos = static_cast<std::size_t>(base + *pos);
    *pos = static_cast<Int64>(p->pos);
    return SZ_OK;
}

// The SDK's allocator, with a ceiling: an allocation the archive asks for
// (a solid block, a PPMd model, an encoded header) above it fails, and the
// SDK reports SZ_ERROR_MEM instead of the process paging out.
struct CappedAlloc {
    ISzAlloc vt;
    std::size_t cap = 0;
};

void* cappedAlloc(ISzAllocPtr pp, size_t size)
{
    const auto* p = reinterpret_cast<const CappedAlloc*>(pp);
    if (size == 0 || size > p->cap) return nullptr;
    return std::malloc(size);
}

void cappedFree(ISzAllocPtr, void* address)
{
    std::free(address);
}

const char* describe(SRes r)
{
    switch (r) {
    case SZ_ERROR_UNSUPPORTED: return "packed with a method POM2 cannot unpack (encrypted, or Deflate / BZip2)";
    case SZ_ERROR_CRC:         return "a member fails its CRC (the archive is damaged)";
    case SZ_ERROR_MEM:         return "it needs more memory to unpack than POM2 allows";
    case SZ_ERROR_INPUT_EOF:   return "the archive is truncated";
    case SZ_ERROR_NO_ARCHIVE:  return "not a 7z archive";
    case SZ_ERROR_ARCHIVE:     return "the archive's directory is damaged";
    default:                   return "the archive is damaged";
    }
}

void appendUtf8(std::string& out, std::uint32_t c)
{
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// A 7z name is UTF-16 (7zFormat.txt, kName). A lone surrogate becomes
// U+FFFD: a name is never invalid UTF-8 on the way to the file system.
std::string utf16ToUtf8(const std::vector<UInt16>& w)
{
    std::string out;
    for (std::size_t i = 0; i < w.size() && w[i]; ++i) {
        std::uint32_t c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < w.size() && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (w[++i] - 0xDC00u);
        else if (c >= 0xD800 && c < 0xE000)
            c = 0xFFFD;
        appendUtf8(out, c);
    }
    return out;
}

} // namespace

bool isSevenZip(const std::vector<std::uint8_t>& head)
{
    return head.size() >= sizeof kSignature && std::equal(std::begin(kSignature), std::end(kSignature), head.begin());
}

struct SevenZipArchive::Impl {
    MemoryStream stream;
    CLookToRead2 look{};
    std::vector<Byte> lookBuffer = std::vector<Byte>(1u << 16);
    CappedAlloc alloc{};
    CSzArEx db{};
    // The SDK's solid-block cache (7z.h, SzArEx_Extract).
    UInt32 blockIndex = 0xFFFFFFFFu;
    Byte* outBuffer = nullptr;
    size_t outBufferSize = 0;

    Impl() { SzArEx_Init(&db); }
    ~Impl()
    {
        ISzAlloc_Free(&alloc.vt, outBuffer);
        SzArEx_Free(&db, &alloc.vt);   // safe on a database never read: SzArEx_Init'ed
    }
};

SevenZipArchive::SevenZipArchive() = default;
SevenZipArchive::~SevenZipArchive() = default;

bool SevenZipArchive::open(const std::vector<std::uint8_t>& bytes, std::string& err,
                           std::size_t maxFolderBytes, std::size_t maxEntries)
{
    impl_.reset();
    entries_.clear();
    if (!isSevenZip(bytes)) { err = "not a 7z archive"; return false; }

    static std::once_flag crcTable;
    std::call_once(crcTable, [] { CrcGenerateTable(); });

    auto s = std::make_unique<Impl>();
    s->stream.vt.Read = memoryRead;
    s->stream.vt.Seek = memorySeek;
    s->stream.data = bytes.data();
    s->stream.size = bytes.size();
    s->alloc.vt.Alloc = cappedAlloc;
    s->alloc.vt.Free = cappedFree;
    // The block itself (checked against maxFolderBytes below, before any
    // decoding), and the decoder's state beside it. A PPMd model is sized by
    // the archive's header, not by the block: libarchive asks for 16 MiB to
    // pack 5 bytes, 7-Zip's own -mx9 for 192 MB. The ceiling is the larger
    // of the two, so a header naming a 4 GiB model is still refused.
    s->alloc.cap = std::max(maxFolderBytes + (std::size_t{1} << 20), kMaxModelBytes);
    LookToRead2_CreateVTable(&s->look, False);
    s->look.buf = s->lookBuffer.data();
    s->look.bufSize = s->lookBuffer.size();
    s->look.realStream = &s->stream.vt;
    LookToRead2_INIT(&s->look)

    const SRes r = SzArEx_Open(&s->db, &s->look.vt, &s->alloc.vt, &s->alloc.vt);
    if (r != SZ_OK) {
        // Before any member is read, a CRC that fails is the header's.
        err = r == SZ_ERROR_CRC ? "the archive's directory fails its CRC (the archive is damaged)" : describe(r);
        return false;
    }

    if (s->db.NumFiles > maxEntries) { err = "the archive holds too many files"; return false; }
    for (UInt32 f = 0; f < s->db.db.NumFolders; ++f) {
        if (SzAr_GetFolderUnpackSize(&s->db.db, f) > maxFolderBytes) {
            err = "the archive unpacks to more than POM2 allows";
            return false;
        }
    }

    std::vector<UInt16> name;
    entries_.reserve(s->db.NumFiles);
    for (UInt32 i = 0; i < s->db.NumFiles; ++i) {
        const size_t length = SzArEx_GetFileNameUtf16(&s->db, i, nullptr);
        name.assign(length, 0);
        if (length) SzArEx_GetFileNameUtf16(&s->db, i, name.data());
        Entry e;
        e.name = utf16ToUtf8(name);
        std::replace(e.name.begin(), e.name.end(), '\\', '/');
        e.directory = SzArEx_IsDir(&s->db, i);
        e.size = e.directory ? 0 : SzArEx_GetFileSize(&s->db, i);
        entries_.push_back(std::move(e));
    }
    impl_ = std::move(s);
    return true;
}

bool SevenZipArchive::extract(std::size_t index, std::vector<std::uint8_t>& out, std::string& err,
                              std::size_t maxBytes, bool* unsupported)
{
    out.clear();
    if (unsupported) *unsupported = false;
    if (!impl_ || index >= entries_.size()) { err = "no such member in the archive"; return false; }
    const Entry& entry = entries_[index];
    if (entry.directory) return true;
    if (entry.size > maxBytes) { err = entry.name + " is too large"; return false; }
    Impl& s = *impl_;
    size_t offset = 0, size = 0;
    const SRes r = SzArEx_Extract(&s.db, &s.look.vt, static_cast<UInt32>(index), &s.blockIndex,
                                  &s.outBuffer, &s.outBufferSize, &offset, &size, &s.alloc.vt, &s.alloc.vt);
    if (r != SZ_OK) {
        // The SDK records the block as cached before decoding it: kept, the
        // next member of that block would be read from a half-written
        // buffer and fail its CRC — "damaged" where the truth is
        // "unsupported". Flushed (7z.h: free it and set it to 0).
        ISzAlloc_Free(&s.alloc.vt, s.outBuffer);
        s.outBuffer = nullptr;
        s.outBufferSize = 0;
        s.blockIndex = 0xFFFFFFFFu;
        if (unsupported) *unsupported = r == SZ_ERROR_UNSUPPORTED;
        err = describe(r);
        return false;
    }
    if (size > maxBytes || offset > s.outBufferSize || size > s.outBufferSize - offset) {
        err = entry.name + " is too large";
        return false;
    }
    out.assign(s.outBuffer + offset, s.outBuffer + offset + size);
    return true;
}

}  // namespace pom2
