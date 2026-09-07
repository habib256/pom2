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

// SnapshotIO — see SnapshotIO.h for format documentation. Ported from
// POM1's identical implementation; only the magic constant and namespace
// changed.

#include "SnapshotIO.h"

#include "AtomicFileReplace.h"

#include <algorithm>
#include <cstring>
#include <ios>

namespace pom2 {
namespace {

// Output streambuf that writes straight into a caller-owned vector — no
// intermediate string/stringstream copy (the memory writer runs 60×/s under
// rewind). Random-access seek supports the section-length back-patch.
class VectorOutBuf final : public std::streambuf
{
public:
    // REPLACES the sink's contents, it does not append: writing starts at
    // pos_ == 0, so a reused buffer (the rewind ring's capture scratch) kept
    // whatever tail the previous, longer snapshot had left past the new
    // length — and the reader rejects the blob as trailing garbage.
    explicit VectorOutBuf(std::vector<uint8_t>& v) : vec_(v) { vec_.clear(); }

protected:
    std::streamsize xsputn(const char* s, std::streamsize n) override
    {
        if (n <= 0) return 0;
        const std::size_t end = pos_ + static_cast<std::size_t>(n);
        if (end > vec_.size()) vec_.resize(end);
        std::memcpy(vec_.data() + pos_, s, static_cast<std::size_t>(n));
        pos_ = end;
        return n;
    }
    int_type overflow(int_type ch) override
    {
        if (traits_type::eq_int_type(ch, traits_type::eof())) return ch;
        if (pos_ + 1 > vec_.size()) vec_.resize(pos_ + 1);
        vec_[pos_++] = static_cast<uint8_t>(traits_type::to_char_type(ch));
        return ch;
    }
    pos_type seekoff(off_type off, std::ios_base::seekdir dir,
                     std::ios_base::openmode /*which*/) override
    {
        std::streamoff base =
            dir == std::ios_base::beg ? 0
          : dir == std::ios_base::cur ? static_cast<std::streamoff>(pos_)
          :                             static_cast<std::streamoff>(vec_.size());
        const std::streamoff np = base + off;
        if (np < 0) return pos_type(off_type(-1));
        pos_ = static_cast<std::size_t>(np);
        return pos_type(np);
    }
    pos_type seekpos(pos_type sp, std::ios_base::openmode which) override
    {
        return seekoff(static_cast<off_type>(sp), std::ios_base::beg, which);
    }

private:
    std::vector<uint8_t>& vec_;
    std::size_t           pos_ = 0;
};

// Input streambuf over a const buffer — zero-copy (the get area IS the
// caller's bytes). Random-access seek supports nextSection's seekg.
class ArrayInBuf final : public std::streambuf
{
public:
    ArrayInBuf(const uint8_t* data, std::size_t len)
    {
        if (data && len) {
            char* p = const_cast<char*>(reinterpret_cast<const char*>(data));
            setg(p, p, p + len);   // we never write through it
        }
    }

protected:
    pos_type seekoff(off_type off, std::ios_base::seekdir dir,
                     std::ios_base::openmode /*which*/) override
    {
        const std::streamoff size = egptr() - eback();
        std::streamoff base =
            dir == std::ios_base::beg ? 0
          : dir == std::ios_base::cur ? (gptr() - eback())
          :                             size;
        const std::streamoff np = base + off;
        if (np < 0 || np > size) return pos_type(off_type(-1));
        setg(eback(), eback() + np, egptr());
        return pos_type(np);
    }
    pos_type seekpos(pos_type sp, std::ios_base::openmode which) override
    {
        return seekoff(static_cast<off_type>(sp), std::ios_base::beg, which);
    }
};

void writeFixedName(std::ostream& out, std::string_view name)
{
    char buf[kSectionNameLen]{};
    const std::size_t copy = std::min(name.size(), kSectionNameLen);
    std::memcpy(buf, name.data(), copy);
    out.write(buf, kSectionNameLen);
}

std::string readFixedName(std::istream& in)
{
    char buf[kSectionNameLen]{};
    in.read(buf, kSectionNameLen);
    std::size_t len = 0;
    while (len < kSectionNameLen && buf[len] != '\0') ++len;
    return std::string(buf, len);
}

} // namespace

// ─── Writer ───────────────────────────────────────────────────────────────
// Both ctors bind `out` to the live backing buffer (file rdbuf or the
// vector streambuf), then emit the shared header. The backend members are
// declared before `out`, so its buffer is fully constructed first.
SnapshotWriter::SnapshotWriter(const std::string& path,
                               std::uint32_t machineId)
    : out(nullptr)
    , targetPath_(path)
    , tempPath_(path + ".tmp")
    , fileBacked_(true)
    , machineId_(machineId)
{
    // Clear the temp path BEFORE opening it. Callers vet the target — the
    // AI control server refuses a path outside the working directory, refuses
    // a symlink and demands a .pom2snap extension — but `path + ".tmp"` is
    // derived here and inherits none of that, and trunc follows symlinks.
    // See prepareTempPath().
    std::error_code ec;
    if (!prepareTempPath(tempPath_, ec)) { out.setstate(std::ios::badbit); return; }

    fileStream_.open(tempPath_, std::ios::binary | std::ios::trunc);
    out.rdbuf(fileStream_.rdbuf());
    // `out` over a failed-open filebuf still starts good(); surface the open
    // failure so good() reports it (callers gate on it).
    if (!fileStream_.good()) { out.setstate(std::ios::badbit); return; }
    emitHeader();
}

SnapshotWriter::SnapshotWriter(std::vector<uint8_t>& sink,
                               std::uint32_t machineId)
    : memBuf_(std::make_unique<VectorOutBuf>(sink))
    , out(memBuf_.get())
    , machineId_(machineId)
{
    emitHeader();
}

SnapshotWriter::~SnapshotWriter()
{
    (void)finish();
}

bool SnapshotWriter::finish()
{
    if (finished_) return committed_;
    finished_ = true;

    out.flush();
    bool ok = out.good();
    if (fileBacked_) {
        fileStream_.close();
        ok = ok && !fileStream_.fail();
        if (ok) {
            std::error_code ec;
            ok = replaceFileAtomic(tempPath_, targetPath_, ec);
        }
        if (!ok) {
            std::error_code ignored;
            std::filesystem::remove(tempPath_, ignored);
        }
    }
    committed_ = ok;
    return committed_;
}

void SnapshotWriter::emitHeader()
{
    out.write(kSnapshotMagic, sizeof(kSnapshotMagic));
    writeU32(kSnapshotVersion);
    // Machine identity, not a reserved zero. The version number alone says
    // nothing about WHICH Apple this state came from, and every section
    // restores unconditionally, so without this a //e snapshot loaded on a
    // //c puts PC and 64 KB of RAM against a different ROM and memory map.
    // 0 stays legal on the wire and means "not recorded".
    writeU32(machineId_);
}

void SnapshotWriter::writeU8(uint8_t v) { out.put(static_cast<char>(v)); }
void SnapshotWriter::writeU16(uint16_t v)
{
    char b[2] = { static_cast<char>(v & 0xFF),
                  static_cast<char>((v >> 8) & 0xFF) };
    out.write(b, 2);
}
void SnapshotWriter::writeU32(uint32_t v)
{
    char b[4] = { static_cast<char>(v & 0xFF),
                  static_cast<char>((v >> 8) & 0xFF),
                  static_cast<char>((v >> 16) & 0xFF),
                  static_cast<char>((v >> 24) & 0xFF) };
    out.write(b, 4);
}
void SnapshotWriter::writeU64(uint64_t v)
{
    writeU32(static_cast<uint32_t>(v & 0xFFFFFFFFu));
    writeU32(static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu));
}
void SnapshotWriter::writeBytes(const void* data, std::size_t length)
{
    if (length > 0)
        out.write(static_cast<const char*>(data),
                  static_cast<std::streamsize>(length));
}

SnapshotWriter::SectionHandle
SnapshotWriter::beginSection(std::string_view name)
{
    SectionHandle h{};
    writeFixedName(out, name);
    h.lengthSlot = out.tellp();
    writeU32(0);
    h.payloadStart = out.tellp();
    return h;
}

void SnapshotWriter::endSection(SectionHandle h)
{
    const std::streampos endPos = out.tellp();
    const auto length = static_cast<uint32_t>(endPos - h.payloadStart);
    out.seekp(h.lengthSlot);
    writeU32(length);
    out.seekp(endPos);
}

void SnapshotWriter::writeSection(std::string_view name,
                                   const void* data,
                                   std::size_t length)
{
    SectionHandle h = beginSection(name);
    writeBytes(data, length);
    endSection(h);
}

// ─── Reader ───────────────────────────────────────────────────────────────
SnapshotReader::SnapshotReader(const std::string& path)
    : fileStream_(path, std::ios::binary)
    , in(fileStream_.rdbuf())
{
    if (!fileStream_.good()) {
        errorMsg = "cannot open snapshot file: " + path;
        return;
    }

    // Record the file size up front so nextSection() can reject a section
    // whose declared length runs past EOF — otherwise a crafted huge length
    // drives an unbounded allocation in the consumer (→ bad_alloc → crash).
    in.seekg(0, std::ios::end);
    fileSize_ = in.tellg();
    in.seekg(0, std::ios::beg);

    parseHeader();
}

SnapshotReader::SnapshotReader(const uint8_t* data, std::size_t length)
    : memBuf_(std::make_unique<ArrayInBuf>(data, length))
    , in(memBuf_.get())
{
    // The whole blob is already resident (referenced in place), so the EOF
    // bound nextSection() enforces is simply its length.
    fileSize_ = static_cast<std::streamoff>(length);
    parseHeader();
}

void SnapshotReader::parseHeader()
{
    char magic[sizeof(kSnapshotMagic)]{};
    in.read(magic, sizeof(kSnapshotMagic));
    if (!in.good() ||
        std::memcmp(magic, kSnapshotMagic, sizeof(kSnapshotMagic)) != 0) {
        errorMsg = "snapshot magic mismatch (not a POM2 snapshot)";
        return;
    }

    ver = readU32();
    machineId_ = readU32();
    if (ver == 0 || ver > kSnapshotVersion) {
        errorMsg = "unsupported snapshot version " + std::to_string(ver);
        return;
    }
    ok = true;
    cursor     = in.tellg();
    sectionEnd = cursor;
}

uint8_t SnapshotReader::readU8()
{
    char c = 0;
    in.read(&c, 1);
    return static_cast<uint8_t>(c);
}
uint16_t SnapshotReader::readU16()
{
    unsigned char b[2]{};
    in.read(reinterpret_cast<char*>(b), 2);
    return static_cast<uint16_t>(b[0] | (uint16_t(b[1]) << 8));
}
uint32_t SnapshotReader::readU32()
{
    unsigned char b[4]{};
    in.read(reinterpret_cast<char*>(b), 4);
    return static_cast<uint32_t>(b[0])
         | (static_cast<uint32_t>(b[1]) << 8)
         | (static_cast<uint32_t>(b[2]) << 16)
         | (static_cast<uint32_t>(b[3]) << 24);
}
uint64_t SnapshotReader::readU64()
{
    uint64_t lo = readU32();
    uint64_t hi = readU32();
    return lo | (hi << 32);
}
void SnapshotReader::readBytes(void* data, std::size_t length)
{
    if (length > 0)
        in.read(static_cast<char*>(data),
                static_cast<std::streamsize>(length));
}

bool SnapshotReader::nextSection(std::string& name, std::uint32_t& length)
{
    if (!ok) return false;
    if (cursor != sectionEnd) {
        in.seekg(sectionEnd);
        cursor = sectionEnd;
    }
    if (in.peek() == EOF) return false;

    name = readFixedName(in);
    if (!in.good()) return false;
    length = readU32();
    if (!in.good()) return false;

    cursor     = in.tellg();
    sectionEnd = cursor + static_cast<std::streamoff>(length);
    // Reject a section whose payload runs past EOF (a crafted/truncated file).
    // This single bound protects every consumer from an over-read and from an
    // unbounded allocation sized by the file's own length field.
    if (sectionEnd > fileSize_ || sectionEnd < cursor) {
        errorMsg = "snapshot section length exceeds file size";
        ok = false;
        return false;
    }
    return true;
}

void SnapshotReader::skipCurrentSection()
{
    in.seekg(sectionEnd);
    cursor = sectionEnd;
}

} // namespace pom2
