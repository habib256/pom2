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

#include "Block512Backing.h"
#include "AtomicFileReplace.h"
#include "Logger.h"
#include "PersistentFs.h"
#include "TwoImg.h"
#include "ProDOSVolume.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace pom2 {

namespace {
constexpr std::uintmax_t kMaxBackingFileBytes = 64u * 1024u * 1024u;
}

// ProDOS block numbers are 16-bit. The highest block INDEX is $FFFF, so a
// volume can hold up to 65536 blocks (indices 0..$FFFF); the synthetic HDV
// card's selectedBlock (uint16_t) reaches every one. The cap is therefore the
// block COUNT 0x10000 — anything that needs index $10000+ is unaddressable.
static_assert(Block512Backing::kMaxBlocks <= 0x10000u,
              "kMaxBlocks must keep the highest block index within 16 bits");

// Phase 1 of the two-phase mount — see the header. Pure file I/O against a
// local buffer: no object state is read or written, so this needs no lock.
bool Block512Backing::readImageFile(const std::string& path, PreparedImage& out,
                                    std::string& error)
{
    out = PreparedImage{};

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "Cannot open HDV image: " + path;
        pom2::log().warn("HDV", error);
        return false;
    }

    f.seekg(0, std::ios::end);
    const std::streampos end = f.tellg();
    // The addressable payload is 32 MiB.  Permit a bounded envelope/trailer,
    // but reject sparse/hostile files before allocating their full size.
    if (end < 0 || static_cast<std::uintmax_t>(end) > kMaxBackingFileBytes) {
        error = "HDV image is too large: " + path;
        pom2::log().warn("HDV", error);
        return false;
    }
    const size_t fileSize = static_cast<size_t>(end);
    f.seekg(0, std::ios::beg);
    if (fileSize == 0) {
        error = "HDV image is empty: " + path;
        pom2::log().warn("HDV", error);
        return false;
    }

    out.bytes.resize(fileSize);
    f.read(reinterpret_cast<char*>(out.bytes.data()),
           static_cast<std::streamsize>(out.bytes.size()));
    if (!f) {
        error = "Short read on HDV image: " + path;
        pom2::log().warn("HDV", error);
        out.bytes.clear();
        return false;
    }

    // Host-filesystem write protection, probed HERE rather than at adopt time:
    // it is a syscall, and phase 2 runs under the lock. A chmod-read-only
    // image previously accepted a whole session of writes into RAM and then
    // failed at flush time ("Cannot open … for write", log-only) — silent data
    // loss. Surfacing it as WP makes the guest see the error at write time,
    // like a locked floppy.
    {
        std::ofstream probe(path, std::ios::in | std::ios::out | std::ios::binary);
        out.hostWritable = static_cast<bool>(probe);
    }

    out.path  = path;
    out.valid = true;
    error.clear();
    return true;
}

bool Block512Backing::loadImage(const std::string& path)
{
    // Inline form: keep the historical ORDER — flush the outgoing medium
    // first, then read the new one. It matters when both are the same file:
    // reading first would capture the pre-flush bytes and then overwrite the
    // guest's writes with them. adoptImage() detects that collision for the
    // two-phase callers.
    if (!saveDirty()) return false;

    PreparedImage prepared;
    std::string   error;
    if (!readImageFile(path, prepared, error)) {
        lastError_ = error;
        return false;
    }
    return adoptPrepared(std::move(prepared));
}

// Phase 2 of the two-phase mount — see the header.
bool Block512Backing::adoptImage(PreparedImage&& prepared)
{
    if (!prepared.valid) {
        lastError_ = "HDV image was not prepared";
        pom2::log().warn("HDV", lastError_);
        return false;
    }

    // Same file, and the outgoing copy has unsaved changes: the prepared bytes
    // were read BEFORE the flush below, so adopting them would silently roll
    // the guest's writes back. Flush, then re-read under the lock. The one
    // path where the two-phase form degrades to the inline cost.
    const bool sameFileStillDirty =
        loaded_ && anyDirty_ && !path_.empty() && path_ == prepared.path;

    // A replacement is an implicit eject. Preserve the current in-memory
    // medium until its opted-in write-back has succeeded; otherwise a failed
    // flush followed by an adopt destroys the only copy of guest writes.
    if (!saveDirty()) return false;

    if (sameFileStillDirty) {
        PreparedImage reread;
        std::string   error;
        if (!readImageFile(prepared.path, reread, error)) {
            lastError_ = error;
            return false;
        }
        prepared = std::move(reread);
    }
    return adoptPrepared(std::move(prepared));
}

// The parse-and-adopt half, shared by both entry points. No file I/O: every
// byte it needs is already in `prepared`.
bool Block512Backing::adoptPrepared(PreparedImage&& prepared)
{
    const std::vector<uint8_t>& bytes = prepared.bytes;
    const std::string&          path  = prepared.path;

    // 2IMG / .2mg container: 64-byte header followed by raw block data.
    // Spec: https://apple2.org.za/gswv/a2zine/Docs/DiskImage_2MG_Info.txt
    //   bytes  0..3  magic "2IMG"
    //   bytes 12..15 image format (LE u32) — 0=DOS 3.3 sector, 1=ProDOS, 2=NIB
    //   bytes 16..19 flags         (LE u32) — bit 31 = locked/write-protect
    //                (CiderPress kFlagLocked = 0x80000000), bit 8 =
    //                volume-number-valid, bits 0-7 = volume number
    //   bytes 24..27 data offset   (LE u32) — typically 64
    //   bytes 28..31 data length   (LE u32) — bytes of block data following
    size_t parsedOffset = 0;
    size_t parsedLength = bytes.size();
    bool   parsedWp     = false;
    if (bytes.size() >= 64 &&
        bytes[0] == '2' && bytes[1] == 'I' && bytes[2] == 'M' && bytes[3] == 'G') {
        auto rd32 = [&](size_t o) {
            return static_cast<uint32_t>(bytes[o]) |
                   (static_cast<uint32_t>(bytes[o + 1]) << 8) |
                   (static_cast<uint32_t>(bytes[o + 2]) << 16) |
                   (static_cast<uint32_t>(bytes[o + 3]) << 24);
        };
        const uint32_t format = rd32(12);
        const uint32_t flags  = rd32(16);
        const uint32_t off    = rd32(24);
        const uint32_t len    = rd32(28);
        if (format != 1) {
            lastError_ = "2IMG image is not in ProDOS block order (format=" +
                         std::to_string(format) + ")";
            pom2::log().warn("HDV", lastError_);
            return false;
        }
        if (off < 64 || off > bytes.size() ||
            len == 0 || static_cast<size_t>(off) + len > bytes.size()) {
            lastError_ = "2IMG header points outside the file (offset=" +
                         std::to_string(off) + ", length=" + std::to_string(len) + ")";
            pom2::log().warn("HDV", lastError_);
            return false;
        }
        parsedOffset = off;
        parsedLength = len;
        // Flags-word semantics live in TwoImg.h (shared with DiskImage
        // and Disk35Image).
        parsedWp     = pom2::twoImgWriteProtected(flags);
    }

    if ((parsedLength % kBlockBytes) != 0) {
        lastError_ = "HDV image data is not a whole number of 512-byte blocks: " +
                     std::to_string(parsedLength);
        pom2::log().warn("HDV", lastError_);
        return false;
    }
    if ((parsedLength / kBlockBytes) > kMaxBlocks) {
        lastError_ = "HDV image has more than 65536 ProDOS blocks: " +
                     std::to_string(parsedLength / kBlockBytes);
        pom2::log().warn("HDV", lastError_);
        return false;
    }

    // The 32 MiB copy this used to make was the whole remaining cost of the
    // locked half. A raw .hdv — no container, payload is the entire file — can
    // simply TAKE the buffer phase 1 already allocated, which turns a memcpy of
    // the medium into a pointer swap. Measured on a 32 MiB image: the locked
    // half went from 10.4 ms to 0.4 ms.
    //
    // A 2IMG still copies: its payload starts 64 bytes in, and moving the
    // buffer then shifting the payload down would be the same memcpy wearing a
    // different hat. Containers are the smaller case and the honest one to pay
    // for.
    if (parsedOffset == 0 && parsedLength == bytes.size()) {
        headerBytes_.clear();
        image_ = std::move(prepared.bytes);
    } else {
        headerBytes_.assign(bytes.begin(),
                            bytes.begin() + static_cast<std::ptrdiff_t>(parsedOffset));
        image_.assign(bytes.begin() + static_cast<std::ptrdiff_t>(parsedOffset),
                      bytes.begin() + static_cast<std::ptrdiff_t>(parsedOffset + parsedLength));
    }
    dataOffset_ = parsedOffset;
    dataLength_ = parsedLength;
    wpHeader_   = parsedWp;
    // Host-filesystem write protection: a chmod-read-only image previously
    // accepted a whole session of writes into RAM, then saveDirty() failed
    // at flush time ("Cannot open … for write", log-only) — silent data
    // loss. Probe writability once at load and surface it as WP so the
    // guest sees the error at write time, like a locked floppy.
    if (!wpHeader_ && !prepared.hostWritable) {
        wpHeader_ = true;
        pom2::log().info("HDV",
            "Image file is not writable on disk — mounting "
            "write-protected: " + path);
    }
    supportsWriteBack_ = true;
    synth_      = false;
    hostFolder_.clear();
    dirtyBlocks_.assign(blockCount(), false);
    anyDirty_ = false;
    path_     = path;
    loaded_   = true;

    pom2::log().info("HDV", "Loaded " + path + " (" +
                            std::to_string(blockCount()) + " blocks)");
    return true;
}

bool Block512Backing::loadFromBytes(std::vector<uint8_t> bytes,
                                    const std::string& label,
                                    const std::string& hostFolder)
{
    if (!saveDirty()) return false;
    if (bytes.empty() || (bytes.size() % kBlockBytes) != 0) {
        lastError_ = "synthesised image is empty or not a multiple of 512";
        pom2::log().warn("HDV", lastError_);
        return false;
    }
    image_ = std::move(bytes);
    headerBytes_.clear();
    dataOffset_ = 0;
    dataLength_ = image_.size();
    synth_      = !hostFolder.empty();
    hostFolder_ = hostFolder;
    // Stamp the snapshot: the write-back decode preserves host files whose
    // mtime is later than this (edited on the host while mounted).
    hasMountTime_ = synth_;
    mountTime_    = std::filesystem::file_time_type::clock::now();
    supportsWriteBack_ = synth_;
    wpHeader_   = false;
    dirtyBlocks_.assign(blockCount(), false);
    anyDirty_ = false;
    path_     = label;
    loaded_   = true;
    pom2::log().info("HDV", "Loaded synthesised volume: " + label +
                            " (" + std::to_string(blockCount()) + " blocks)");
    return true;
}

void Block512Backing::eject()
{
    image_.clear();
    headerBytes_.clear();
    dirtyBlocks_.clear();
    dataOffset_ = 0;
    dataLength_ = 0;
    path_.clear();
    hostFolder_.clear();
    loaded_ = false;
    synth_  = false;
    supportsWriteBack_ = false;
    wpHeader_ = false;
    anyDirty_ = false;
}

bool Block512Backing::saveDirty()
{
    // Composed from the two-phase pair so there is ONE copy of the write
    // logic. The phases are simply not separated in time here: this entry
    // point is for single-threaded callers (CLI, tests, the flush inside
    // adoptImage), where there is no lock to get out from under.
    PendingWriteBack pending = takeWriteBack();
    if (!pending.valid) return true;

    // takeWriteBack() MOVED the dirty set out; keep the indices so a failed
    // commit can put them back (pre-split behaviour: a failed save leaves
    // the blocks dirty so the next attempt still has them).
    const std::vector<uint32_t> captured = pending.dirtyIndices;
    const bool wasSynth = pending.synth;
    std::string error;
    std::filesystem::file_time_type stamp{};
    if (!commitWriteBack(std::move(pending), error, &stamp)) {
        lastError_ = error;
        restoreDirty(captured);
        return false;
    }
    // The medium is STILL MOUNTED here (this is the flush path, not the eject
    // path), and the write-back just rewrote host files. Re-stamp the volume
    // so those files are not "host-newer" on the next flush — otherwise the
    // guest's first save landed and every save after it was silently
    // preserved away as if the user had edited the file behind POM2's back.
    if (wasSynth && synth_ && loaded_) {
        mountTime_    = stamp;
        hasMountTime_ = true;
    }
    return true;
}

Block512Backing::PendingWriteBack Block512Backing::takeWriteBack()
{
    PendingWriteBack out;
    if (!loaded_ || !anyDirty_ || !writeBack_
        || wpHeader_ || !supportsWriteBack_) {
        return out;                      // valid = false → nothing to commit
    }

    out.valid      = true;
    out.synth      = synth_;
    out.path       = path_;
    out.hostFolder = hostFolder_;
    out.dataOffset = dataOffset_;
    out.dataLength = dataLength_;

    // Captured in BOTH cases: the file commit writes these blocks, and a
    // failed commit restores them through `restoreDirty` either way.
    for (size_t b = 0; b < dirtyBlocks_.size(); ++b) {
        if (!dirtyBlocks_[b]) continue;
        out.dirtyIndices.push_back(static_cast<uint32_t>(b));
    }

    if (synth_) {
        // The folder decode needs the whole volume, so it is copied. That is
        // a memcpy of at most the image size — microseconds against the
        // directory walk and the file writes phase 2 does.
        out.synthImage   = image_;
        out.hasMountTime = hasMountTime_;
        out.mountTime    = mountTime_;
    } else {
        // The file case only needs the blocks that actually changed, which
        // is normally a handful even on a 32 MiB image.
        out.dirtyBytes.resize(out.dirtyIndices.size() * kBlockBytes);
        for (size_t i = 0; i < out.dirtyIndices.size(); ++i) {
            std::memcpy(out.dirtyBytes.data() + i * kBlockBytes,
                        image_.data() + out.dirtyIndices[i] * kBlockBytes,
                        kBlockBytes);
        }
    }

    // The move half of "move out": retire what was captured, atomically
    // under the caller's lock. A guest write landing while phase 2 runs
    // unlocked re-marks its block; it is NOT in this payload, so its flag
    // must survive for the eject's own inline flush (or the next save).
    std::fill(dirtyBlocks_.begin(), dirtyBlocks_.end(), false);
    anyDirty_ = false;
    return out;
}

void Block512Backing::restoreDirty(const std::vector<uint32_t>& indices)
{
    for (uint32_t b : indices) {
        if (b < dirtyBlocks_.size()) {
            dirtyBlocks_[b] = true;
            anyDirty_ = true;
        }
    }
}

bool Block512Backing::commitWriteBack(PendingWriteBack&& pending,
                                      std::string& error,
                                      std::filesystem::file_time_type* newMountTime)
{
    if (!pending.valid) return true;

    if (pending.synth) {
        pom2::ProDOSDecodeResult r = pom2::decodeVolumeToFolder(
            pending.synthImage, pending.hostFolder,
            pending.hasMountTime ? &pending.mountTime : nullptr);
        if (!r.ok) {
            error = r.error;
            pom2::log().warn("HDV", "Synth folder write-back failed: " + error);
            return false;
        }
        if (newMountTime) *newMountTime = r.completedAt;
        pom2::log().info("HDV", "Synth folder write-back: " +
                                std::to_string(r.filesWritten) + " file(s) → " +
                                pending.hostFolder);
        // Browser build: the host "folder" lives in the IDBFS mount, and a
        // write there reaches IndexedDB only when something flushes. No-op
        // natively — the decode's rename+fsync is already durable.
        markPersistentStateDirty();
        return true;
    }

    // Rewrite a complete sibling copy, preserving the 2IMG envelope and any
    // trailer.  An in-place series of 512-byte writes could leave the user's
    // only image half-updated when a later write/flush failed.
    std::ifstream source(pending.path, std::ios::binary | std::ios::ate);
    if (!source) {
        error = "Cannot open " + pending.path + " for read";
        pom2::log().warn("HDV", error);
        return false;
    }
    const std::streampos end = source.tellg();
    if (end < 0 ||
        static_cast<size_t>(end) < pending.dataOffset + pending.dataLength ||
        static_cast<std::uintmax_t>(end) > kMaxBackingFileBytes) {
        error = "Source image changed size before save: " + pending.path;
        pom2::log().warn("HDV", error);
        return false;
    }
    source.seekg(0, std::ios::beg);
    std::vector<uint8_t> output(static_cast<size_t>(end));
    if (!source.read(reinterpret_cast<char*>(output.data()),
                     static_cast<std::streamsize>(output.size()))) {
        error = "Short read on " + pending.path;
        pom2::log().warn("HDV", error);
        return false;
    }
    for (size_t i = 0; i < pending.dirtyIndices.size(); ++i) {
        std::memcpy(output.data() + pending.dataOffset +
                        static_cast<size_t>(pending.dirtyIndices[i]) * kBlockBytes,
                    pending.dirtyBytes.data() + i * kBlockBytes,
                    kBlockBytes);
    }
    const size_t written = pending.dirtyIndices.size();

    // Unique per process + per call: a fixed `<image>.pom2tmp` is the name
    // every POM2 instance derives, so two of them flushing the same image
    // truncated each other's in-flight write. See tempSiblingPath().
    const std::filesystem::path tmp = tempSiblingPath(pending.path);
    // Same temp-path scrutiny as `Disk35Image::saveDirty` and every other
    // AtomicFileReplace caller: the TARGET was validated at mount, but this
    // sibling name is derived afterwards and gets none of that. A symlink
    // planted at <image>.pom2tmp redirects the trunc onto whatever it points
    // at and the rename then carries the link away, destroying the user's
    // image with nothing left to show what happened; a hard link to the image
    // itself truncates it before anything is committed. See prepareTempPath.
    std::error_code prepEc;
    if (!prepareTempPath(tmp, prepEc)) {
        error = "Temp path unusable " + tmp.string() + ": " + prepEc.message();
        pom2::log().warn("HDV", error);
        return false;
    }
    std::error_code permEc;
    const auto perms = std::filesystem::status(pending.path, permEc).permissions();
    const bool havePerms = !permEc;
    std::ofstream sink(tmp, std::ios::binary | std::ios::trunc);
    if (!sink ||
        !sink.write(reinterpret_cast<const char*>(output.data()),
                    static_cast<std::streamsize>(output.size()))) {
        error = "Write failed on " + tmp.string();
        sink.close();
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        pom2::log().warn("HDV", error);
        return false;
    }
    sink.flush();
    sink.close();
    if (!sink) {
        error = "Flush failed on " + tmp.string();
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        pom2::log().warn("HDV", error);
        return false;
    }
    std::error_code ec;
    if (havePerms) {
        std::filesystem::permissions(tmp, perms, ec);
        ec.clear();
    }
    if (!replaceFileAtomic(tmp, pending.path, ec)) {
        error = "Cannot replace " + pending.path + ": " + ec.message();
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        pom2::log().warn("HDV", error);
        return false;
    }
    // Browser build: the image file lives in the IDBFS mount and reaches
    // IndexedDB only when the frame loop's pump flushes. Before this, only
    // Settings::save marked the store dirty, so a session that wrote a disk
    // and changed nothing else lost the disk on reload. No-op natively.
    markPersistentStateDirty();
    pom2::log().info("HDV", "Saved " + std::to_string(written) +
                            " modified block(s) to " + pending.path);
    return true;
}

void Block512Backing::markDirty(uint32_t blk)
{
    if (blk < dirtyBlocks_.size() && !dirtyBlocks_[blk]) {
        dirtyBlocks_[blk] = true;
        anyDirty_ = true;
    }
}

bool Block512Backing::readBlock(uint32_t blk, uint8_t* dst512) const
{
    const size_t base = static_cast<size_t>(blk) * kBlockBytes;
    if (base + kBlockBytes > image_.size()) return false;
    bumpActivity();
    std::memcpy(dst512, &image_[base], kBlockBytes);
    return true;
}

bool Block512Backing::writeBlock(uint32_t blk, const uint8_t* src512)
{
    if (wpHeader_) return false;
    const size_t base = static_cast<size_t>(blk) * kBlockBytes;
    if (base + kBlockBytes > image_.size()) return false;
    bumpActivity();
    if (std::memcmp(&image_[base], src512, kBlockBytes) != 0) {
        std::memcpy(&image_[base], src512, kBlockBytes);
        markDirty(blk);
        // A rewind may not cross a media write — see mediaWriteEpoch() in
        // the header. Bumped here rather than in markDirty(): that one is
        // idempotent per block (and is also how a failed commit re-marks
        // its captured set), so a second write to the same block, or a
        // restoreDirtyBlocks, would not move the epoch.
        noteMediaWrite();
    }
    return true;
}

uint8_t Block512Backing::readByte(size_t absolute) const
{
    if (!loaded_) return 0xFF;
    bumpActivity();
    return (absolute < image_.size()) ? image_[absolute] : 0xFF;
}

void Block512Backing::writeByte(size_t absolute, uint8_t v)
{
    if (!loaded_ || wpHeader_) return;
    bumpActivity();
    if (absolute < image_.size() && image_[absolute] != v) {
        image_[absolute] = v;
        markDirty(static_cast<uint32_t>(absolute / kBlockBytes));
        noteMediaWrite();                       // see writeBlock
    }
}

} // namespace pom2
