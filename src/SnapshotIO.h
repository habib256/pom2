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

// SnapshotIO — binary read/write primitives for the POM2 snapshot format.
// Ported from POM1, magic + namespace renamed.
//
// Format (versioned, little-endian throughout — POM2 is host-side LE only):
//
//   "POM2SNAP" (8 bytes)               magic
//   uint32_t  version                  format version (current = 2)
//   uint32_t  flags                    machine identity (0 = not recorded)
//
//   Section: 8-byte fixed name (NUL-padded) + uint32_t length + payload
//
// Suggested section roster for Apple II:
//
//   "CPU"      16 bytes: PC(2) A X Y status SP cpuMode (6) + cycle count(8).
//              IRQ/NMI lines are NOT persisted — they are transient bus
//              signals re-asserted by the cards on each MMIO access, so they
//              self-correct within a frame of resuming.
//   "MEM"      main 64 KB RAM (restored through writable[] — ROM preserved)
//   "MEX"      (v2) aux RAM + Language-Card RAM + RamWorks banks + paging
//              soft-switches (iieMemMode) + LC latch flags + DisplayState
//   "SLOT0".."SLOT7"   per-slot peripheral payloads (each card decides)
//
// (A "CASS" cassette section was once listed here but never implemented —
// tape state is deliberately outside the snapshot, like disk media.)
//
// Unknown sections are skipped at load time (forward compat). Cards that
// don't have state to persist write a zero-length section, or simply
// don't appear at all.
//
// No compression, no checksum (yet — could be a v2 sweetener). The file
// is small (~64 KB + slot payloads) and the use case is "snapshot now,
// reload now" rather than archival cold storage.
//
// Both classes have a file backend (the original) AND an in-memory backend:
//   SnapshotWriter(std::vector<uint8_t>&)  — accumulate the blob in RAM
//   SnapshotReader(const uint8_t*, size_t) — parse a blob already in RAM
// The memory backend is what the rewind ring buffer (RewindBuffer) uses to
// serialize/restore machine state at 60 Hz without touching the filesystem.
// The wire format is byte-identical between the two backends.

#ifndef POM2_SNAPSHOT_IO_H
#define POM2_SNAPSHOT_IO_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <memory>
#include <ostream>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>

namespace pom2 {

inline constexpr char     kSnapshotMagic[8] = {'P','O','M','2','S','N','A','P'};
// v2 adds the "MEX" section (aux RAM, Language-Card RAM, RamWorks banks,
// paging soft-switches, DisplayState) so IIe/IIc state restores fully.
// The reader accepts any version <= kSnapshotVersion, so v1 files still load.
inline constexpr uint32_t kSnapshotVersion  = 2;
inline constexpr std::size_t kSectionNameLen = 8;

class SnapshotWriter
{
public:
    /// `machineId` is stamped into the header's identity word — pass
    /// `pom2::snapshotMachineId(profile)` so a later load can refuse a
    /// snapshot taken on a different machine. 0 (the default) records no
    /// identity and is what an in-memory rewind frame wants: the ring is
    /// already cleared on every profile switch, so the check would be dead
    /// weight on the hot capture path.
    ///
    /// File-backed: writes the snapshot straight to `path` (truncating any
    /// existing file). `good()` is false if the file could not be opened.
    explicit SnapshotWriter(const std::string& path,
                            std::uint32_t machineId = 0);
    /// Memory-backed: writes the snapshot straight into `sink` as it is
    /// produced (no intermediate copy). `sink` must outlive the writer, and
    /// its previous contents are REPLACED — the constructor clears it. It
    /// used to say "appends", which it never did: it wrote from offset 0 and
    /// left any longer previous content trailing past the new end, where the
    /// reader sees it as a corrupt blob. Capacity is kept, so reusing one
    /// buffer for the rewind ring still costs no allocation. Always `good()`.
    explicit SnapshotWriter(std::vector<uint8_t>& sink,
                            std::uint32_t machineId = 0);
    ~SnapshotWriter();

    bool good() const { return finished_ ? committed_ : out.good(); }

    /// Flush and, for the file backend, atomically publish the completed
    /// snapshot.  Callers that report success to a user must call this
    /// explicitly so delayed disk/close/rename failures are observable.
    /// The destructor calls it as a best-effort fallback.
    bool finish();

    void writeU8 (uint8_t  v);
    void writeU16(uint16_t v);
    void writeU32(uint32_t v);
    void writeU64(uint64_t v);
    void writeBytes(const void* data, std::size_t length);

    /// Begin a named section. Writes the 8-byte name and a placeholder
    /// length; returns a handle the caller passes back to endSection()
    /// once the payload is written. Sections cannot nest.
    struct SectionHandle {
        std::streampos lengthSlot{};
        std::streampos payloadStart{};
    };
    SectionHandle beginSection(std::string_view name);
    void          endSection(SectionHandle handle);

    /// One-shot helper for sections backed by a contiguous buffer.
    void writeSection(std::string_view name, const void* data, std::size_t length);

private:
    void emitHeader();   // magic + version + machine identity

    std::ofstream                   fileStream_;   // engaged for the file ctor
    std::unique_ptr<std::streambuf> memBuf_;       // engaged for the memory ctor
    std::ostream                    out;           // bound to the live buffer
    std::filesystem::path           targetPath_;
    std::filesystem::path           tempPath_;
    bool                            fileBacked_ = false;
    bool                            finished_ = false;
    bool                            committed_ = false;
    std::uint32_t                   machineId_ = 0;
};

class SnapshotReader
{
public:
    /// File-backed: opens and parses the snapshot at `path`.
    explicit SnapshotReader(const std::string& path);
    /// Memory-backed: parses a snapshot already resident in RAM. The bytes
    /// are referenced in place (zero-copy), so `data` MUST outlive the
    /// reader. `length == 0` (or `data == nullptr`) yields a non-good reader
    /// (no valid header).
    SnapshotReader(const uint8_t* data, std::size_t length);
    ~SnapshotReader() = default;

    /// True iff construction parsed a valid POM2 snapshot header AND no
    /// read since has set failbit/badbit. EOF after consuming all
    /// sections is normal — nextSection returns false at EOF.
    bool     good()    const { return ok && !in.fail(); }
    uint32_t version() const { return ver; }
    /// Machine identity recorded in the header, or 0 when the snapshot was
    /// written before the field existed (v1/v2 files, and rewind frames,
    /// which deliberately record none). Compare against
    /// `pom2::snapshotMachineId(currentProfile)` and refuse a mismatch —
    /// the CPU/MEM/MEX sections restore PC and RAM unconditionally, so a
    /// cross-machine load lands them against a different ROM and memory map.
    uint32_t machineId() const { return machineId_; }
    const std::string& error() const { return errorMsg; }

    uint8_t  readU8();
    uint16_t readU16();
    uint32_t readU32();
    uint64_t readU64();
    void     readBytes(void* data, std::size_t length);

    /// Iterate sections in file order. On success, fills `name` (NUL-
    /// trimmed) and `length`. The caller is then expected to either
    /// readBytes(buf, length) to consume the payload, or call
    /// skipCurrentSection() to move past it.
    bool nextSection(std::string& name, std::uint32_t& length);
    void skipCurrentSection();

    bool atSectionBoundary() const { return cursor == sectionEnd; }

private:
    void parseHeader();   // validate magic/version, prime cursor

    std::ifstream                   fileStream_;   // engaged for the file ctor
    std::unique_ptr<std::streambuf> memBuf_;       // engaged for the memory ctor
    std::istream   in;               // bound to whichever buffer is live
    bool           ok = false;
    uint32_t       ver = 0;
    uint32_t       machineId_ = 0;
    std::string    errorMsg;
    std::streampos cursor{};
    std::streampos sectionEnd{};
    std::streamoff fileSize_ = 0;   // total file size; nextSection bounds against it
};

} // namespace pom2

#endif // POM2_SNAPSHOT_IO_H
