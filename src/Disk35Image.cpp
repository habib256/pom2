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

// Disk35Image — Phase 1 implementation. Block storage + 2IMG envelope
// passthrough. Sony GCR encoding (the Phase 2 work) is not here yet
// — the bytes round-trip the file system but the IWM cannot clock
// them out as flux transitions until that lands.
//
// MAME line refs:
//   * 2IMG header decode mirrors `DiskImage.cpp` (POM2's existing 5.25"
//     2IMG path), itself a faithful port of the 2IMG spec used by MAME
//     `ap_dsk35.cpp` for ProDOS .2mg loads.

#include "Disk35Image.h"

#include "Block512Backing.h"

#include "Sony35Gcr.h"
#include "AtomicFileReplace.h"
#include "Logger.h"
#include "PersistentFs.h"
#include "TwoImg.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace pom2 {

namespace {

constexpr int kSectorsPerTrackByZone[5] = { 12, 11, 10, 9, 8 };

uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
uint32_t rd32(const uint8_t* p) {
    return  static_cast<uint32_t>(p[0])        |
           (static_cast<uint32_t>(p[1]) <<  8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

int Disk35Image::sectorsForTrack(int track)
{
    // MAME `ap_dsk35.cpp` zone schedule: 5 zones of 16 tracks each.
    if (track < 0 || track >= kTracks) return 0;
    return kSectorsPerTrackByZone[track / 16];
}

bool Disk35Image::loadFile(const std::string& imgPath)
{
    // Loading a replacement is an implicit eject. First make the outgoing
    // medium durable, then parse the candidate into a separate object so any
    // malformed/unreadable file leaves the current disk fully mounted.
    if (!saveDirty()) return false;
    Disk35Image replacement;
    replacement.writeBackEnabled_ = writeBackEnabled_;
    if (!replacement.loadFileUnchecked(imgPath)) {
        lastError_ = replacement.lastError_;
        return false;
    }
    *this = std::move(replacement);
    return true;
}

bool Disk35Image::loadFileUnchecked(const std::string& imgPath)
{
    eject();
    path_ = imgPath;

    std::ifstream f(imgPath, std::ios::binary | std::ios::ate);
    if (!f) {
        lastError_ = "Disk35Image: cannot open " + imgPath;
        return false;
    }
    const std::streamsize sz = f.tellg();
    if (sz <= 0) {
        lastError_ = "Disk35Image: empty file " + imgPath;
        return false;
    }
    // Raw media is 800 KiB.  Allow wrappers/trailers ample headroom, while
    // rejecting sparse/hostile files before allocating from their length.
    constexpr std::streamsize kMaxDisk35ImageBytes = 16 * 1024 * 1024;
    if (sz > kMaxDisk35ImageBytes) {
        lastError_ = "Disk35Image: file is too large " + imgPath;
        return false;
    }
    f.seekg(0, std::ios::beg);

    std::vector<uint8_t> buf(static_cast<std::size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(buf.data()), sz)) {
        lastError_ = "Disk35Image: short read on " + imgPath;
        return false;
    }

    // Host-filesystem write protection (same rationale as Block512Backing):
    // with write-back on, a chmod-read-only image accepted a session of
    // writes and lost them at flush. Probe once; OR-ed into both branches'
    // header/extension-derived flag below.
    const bool hostReadOnly = [&imgPath]() {
        std::ofstream probe(imgPath,
            std::ios::in | std::ios::out | std::ios::binary);
        return !probe;
    }();
    const std::size_t n = buf.size();

    // Detect 2IMG-wrapped 800K. Same header layout as 5.25"; we just
    // require the payload size to be 819 200 bytes.
    if (n >= 64 &&
        buf[0] == '2' && buf[1] == 'I' && buf[2] == 'M' && buf[3] == 'G')
    {
        const uint16_t headerLen = rd16(buf.data() + 8);
        const uint32_t format    = rd32(buf.data() + 12);
        const uint32_t flags     = rd32(buf.data() + 16);
        const uint32_t dataOff   = rd32(buf.data() + 24);
        const uint32_t dataLen   = rd32(buf.data() + 28);

        if (headerLen < 52 || dataOff < headerLen || dataOff > n ||
            dataLen == 0 ||
            static_cast<std::size_t>(dataOff) + dataLen > n)
        {
            lastError_ = "Disk35Image: malformed 2IMG header in " + imgPath;
            return false;
        }
        // format 1 = ProDOS block-ordered. 800K disks use this.
        if (format != 1) {
            lastError_ = "Disk35Image: 2IMG format " + std::to_string(format) +
                         " not supported for 3.5\" disks (need ProDOS=1)";
            return false;
        }
        if (dataLen != kBytesPerImage) {
            lastError_ = "Disk35Image: 2IMG ProDOS payload must be " +
                         std::to_string(kBytesPerImage) + " bytes, got " +
                         std::to_string(dataLen);
            return false;
        }

        twoImgHeaderRaw_.assign(buf.begin(),
                                buf.begin() + dataOff);
        twoImgTrailerRaw_.assign(buf.begin() + dataOff + dataLen,
                                 buf.end());
        blocks_.assign(buf.begin() + dataOff,
                       buf.begin() + dataOff + dataLen);
        // Flags-word semantics live in TwoImg.h (shared with DiskImage
        // and Block512Backing).
        fileWriteProtected_ = pom2::twoImgWriteProtected(flags) || hostReadOnly;
        kind_   = ImageKind::TwoImg800k;
        loaded_ = true;
        dirty_  = false;
        return true;
    }

    // WOZ flux dump (Applesauce & co). A `.woz` holds bit CELLS, not
    // blocks, and POM2 stores 3.5" media as a flat block array — it has no
    // GCR encoder, so there is nothing to mount a flux image *as*. What it
    // does have is the decoder: the same `Sony35Gcr` walk `Sony35Drive`
    // uses to fold guest-written tracks back into the image. So a `.woz` is
    // decoded ONCE here, at load, and mounts as an ordinary 800K volume.
    //
    // Read-only by construction: giving the blocks back would mean
    // re-encoding GCR into the original flux, which POM2 cannot do. The
    // user's dump is therefore never at risk from a write-back.
    if (n >= 12 && buf[0] == 'W' && buf[1] == 'O' && buf[2] == 'Z' &&
        (buf[3] == '1' || buf[3] == '2'))
    {
        if (!loadWoz(buf, imgPath)) return false;
        fileWriteProtected_ = true;
        kind_   = ImageKind::Woz35;
        loaded_ = true;
        dirty_  = false;
        return true;
    }

    // Bare 800K payload (.po, .dsk, .image).
    if (n == kBytesPerImage) {
        // Cheap "looks like ProDOS" sniff: block 2 should be the volume
        // directory key block. Not authoritative — non-ProDOS 800K disks
        // (rare; Pascal, ProFile) load anyway, but we warn.
        const uint8_t* keyBlock = buf.data() + 2u * kBlockBytes;
        if (!(keyBlock[0] == 0 && keyBlock[1] == 0 &&
              (keyBlock[4] & 0xF0) == 0xF0))
        {
            pom2::log().warn(
                "Disk35",
                "image " + imgPath +
                " doesn't look ProDOS-formatted at block 2");
        }
        blocks_ = std::move(buf);
        // Writability is decided by the FILE, not by its extension. All
        // three names reaching here (.po, .dsk, .image) hold the identical
        // bare 819 200-byte ProDOS block payload and take the identical
        // save path, so there was never a technical reason to treat them
        // differently — the old rule marked .dsk/.image physically WP on
        // the grounds that such dumps are "sometimes read-only by
        // convention".
        //
        // That is the wrong layer for a convention. `fileWriteProtected_`
        // is the PHYSICAL tab: it overrides the user's own
        // `setWriteBackEnabled` opt-in, so an 800K .dsk stayed read-only
        // even after the user explicitly asked for write-back and got no
        // explanation why. Nothing is loosened by dropping it — writes
        // still require that opt-in, which is off by default, plus a
        // host file that is actually writable.
        fileWriteProtected_ = hostReadOnly;
        kind_   = ImageKind::Raw800k;
        loaded_ = true;
        dirty_  = false;
        return true;
    }

    lastError_ = "Disk35Image: " + imgPath +
                 " is not an 800K 3.5\" image (size " + std::to_string(n) +
                 ", expected " + std::to_string(kBytesPerImage) +
                 " or 2IMG-wrapped)";
    return false;
}

void Disk35Image::eject()
{
    loaded_              = false;
    dirty_               = false;
    fileWriteProtected_  = false;
    kind_                = ImageKind::Unknown;
    blocks_.clear();
    twoImgHeaderRaw_.clear();
    twoImgTrailerRaw_.clear();
    path_.clear();
    lastError_.clear();
}

bool Disk35Image::readBlock(uint32_t idx, uint8_t out[kBlockBytes]) const
{
    if (!loaded_ || idx >= kBlockCount) return false;
    std::memcpy(out, blocks_.data() + idx * kBlockBytes, kBlockBytes);
    return true;
}

bool Disk35Image::writeBlock(uint32_t idx, const uint8_t in[kBlockBytes])
{
    if (!loaded_ || idx >= kBlockCount) return false;
    if (isWriteProtected()) return false;
    std::memcpy(blocks_.data() + idx * kBlockBytes, in, kBlockBytes);
    dirty_ = true;
    // A rewind may not cross a media write: an 800 KB image is not captured
    // per rewind frame, so the controller clears the ring instead. See
    // `pom2::mediaWriteEpoch()` in Block512Backing.h.
    noteMediaWrite();
    return true;
}

bool Disk35Image::exportRawTo(const std::string& outPath,
                             std::string& errOut) const
{
    if (!loaded_ || blocks_.size() != kBytesPerImage) {
        errOut = "no 800K image loaded";
        return false;
    }
    // Refuse to overwrite: the caller picks the name, and silently replacing
    // an existing image with a different disk is the one mistake this feature
    // could make that the user cannot undo.
    std::error_code ec;
    if (std::filesystem::exists(outPath, ec)) {
        errOut = outPath + " already exists";
        return false;
    }
    // Same temp-then-rename discipline as saveDirty: a partial 800 KB write
    // must never be left behind looking like a mountable image.
    const std::string tmp = outPath + ".pom2tmp";
    // …and the same temp-path scrutiny as `saveDirty` below: the target was
    // just checked for existence, but the sibling temp name was not, and a
    // symlink there would send the 800 KB write somewhere else entirely.
    std::error_code prepEc;
    if (!prepareTempPath(tmp, prepEc)) {
        errOut = "temp path unusable " + tmp + ": " + prepEc.message();
        return false;
    }
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!f) { errOut = "cannot open " + tmp + " for write"; return false; }
        f.write(reinterpret_cast<const char*>(blocks_.data()),
                static_cast<std::streamsize>(blocks_.size()));
        f.flush();
        f.close();
        if (!f) {
            errOut = "write failed on " + tmp;
            std::error_code rmEc;
            std::filesystem::remove(tmp, rmEc);
            return false;
        }
    }
    // A converted image is a NEW file, not a write-back, but it earns the
    // same durability: the user is about to mount it and let a program write
    // its configuration into it.
    ec.clear();
    (void)syncFileContents(tmp, ec);
    ec.clear();
    if (!replaceFileAtomic(tmp, outPath, ec)) {
        errOut = "cannot create " + outPath + ": " + ec.message();
        std::error_code rmEc;
        std::filesystem::remove(tmp, rmEc);
        return false;
    }
    return true;
}

bool Disk35Image::saveDirty()
{
    // Composed from the two-phase pair so there is ONE copy of the write
    // logic (same shape as `Block512Backing::saveDirty`). The phases are
    // simply not separated in time here.
    PendingWriteBack pending = takeWriteBack();
    if (!pending.valid) return true;
    std::string error;
    if (!commitWriteBack(std::move(pending), error)) {
        lastError_ = error;
        pom2::log().warn("Disk35", lastError_);
        restoreDirty();
        return false;
    }
    return true;
}

Disk35Image::PendingWriteBack Disk35Image::takeWriteBack()
{
    PendingWriteBack out;
    if (!loaded_ || !dirty_) return out;
    // Write-back off (or read-only host file): a SUCCESSFUL no-op, exactly
    // like DiskImage::saveDirty. isWriteProtected() folds the user's
    // write-back opt-out in, and treating that as a hard error wedged the
    // 3.5" paths for the rest of the session: once dirty, unchecking
    // "Write-back (save on eject)" made eject, disk swap and the //c+
    // mount35 flush gate all fail forever with a misleading
    // "write-protected" error. Opting out means "discard on eject", not
    // "refuse to eject" — dirty_ is left set so re-enabling write-back
    // before the eject still saves the session's writes.
    if (isWriteProtected()) return out;

    out.valid = true;
    out.path  = path_;
    out.bytes.reserve(twoImgHeaderRaw_.size() + blocks_.size() +
                      twoImgTrailerRaw_.size());
    out.bytes.insert(out.bytes.end(),
                     twoImgHeaderRaw_.begin(), twoImgHeaderRaw_.end());
    out.bytes.insert(out.bytes.end(), blocks_.begin(), blocks_.end());
    out.bytes.insert(out.bytes.end(),
                     twoImgTrailerRaw_.begin(), twoImgTrailerRaw_.end());
    // The move half of "move out": retire the flag under the caller's lock,
    // atomically with the capture. A block the guest writes while phase 2
    // runs unlocked re-dirties the medium and is NOT in this payload — it
    // still needs a later flush, and `restoreDirty` on failure merges with it
    // for free (there is only the one flag).
    dirty_ = false;
    return out;
}

bool Disk35Image::commitWriteBack(PendingWriteBack&& pending,
                                  std::string& error)
{
    error.clear();
    if (!pending.valid) return true;

    // Never open the user's image with `trunc` and rewrite it in place:
    // save-on-eject writes 800 KB, and an ENOSPC / removable-media / network
    // -share failure part-way through then leaves the ONLY copy of the disk
    // truncated — the rest lives in `pending.bytes`, which dies with the
    // process. Same discipline (and same reason) as `DiskImage::saveDirty`'s
    // writeFileAtomic: emit a sibling temp file — same directory, therefore
    // same filesystem, therefore `rename` is atomic and cannot fail
    // cross-device — and only swap it in once the write fully succeeded.
    // Unique per process AND per call: a fixed `<path>.pom2tmp` is the name
    // every POM2 instance derives, so two of them saving the same image
    // interleaved their writes into one temp file. See tempSiblingPath().
    const std::string tmp = tempSiblingPath(pending.path).string();
    // A rename replaces the inode, so the temp file's umask-derived mode
    // would become the image's. Carry the original's permissions across.
    std::error_code permEc;
    const std::filesystem::perms origPerms =
        std::filesystem::status(pending.path, permEc).permissions();
    const bool havePerms = !permEc;
    // Same temp-path scrutiny as every AtomicFileReplace caller: a symlink
    // or hard link planted at <path>.pom2tmp would redirect the trunc onto
    // the user's file (or another victim) before anything is committed.
    std::error_code prepEc;
    if (!prepareTempPath(tmp, prepEc)) {
        error = "Disk35Image: temp path unusable " + tmp + ": " +
                prepEc.message();
        return false;
    }
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!f) {
            error = "Disk35Image: cannot open " + tmp + " for write";
            std::error_code rm;
            std::filesystem::remove(tmp, rm);
            return false;
        }
        f.write(reinterpret_cast<const char*>(pending.bytes.data()),
                static_cast<std::streamsize>(pending.bytes.size()));
        f.flush();
        f.close();
        if (!f) {
            // The atomic-temp discipline means the user's file is untouched,
            // but the only copy of the session's writes is about to die with
            // the process on a shutdown flush — leave a trace, like
            // DiskIICard::flushPendingWrites does for the same case.
            error = "Disk35Image: write failed on " + tmp;
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    if (havePerms) {
        std::filesystem::permissions(tmp, origPerms, ec);
        ec.clear();
    }
    if (!replaceFileAtomic(tmp, pending.path, ec)) {
        error = "Disk35Image: cannot replace " + pending.path + ": " +
                ec.message();
        std::error_code ec2;
        std::filesystem::remove(tmp, ec2);
        return false;
    }
    // Browser build: durable only once the IDBFS mount is flushed, and only
    // Settings::save used to ask for that. No-op natively.
    markPersistentStateDirty();
    return true;
}


// ─── WOZ 3.5" (flux) → blocks ─────────────────────────────────────────────
//
// WOZ2 layout: 12-byte header, then `ID(4) size(4) payload` chunks. We need
// TMAP (160 bytes: one track-index per track*2+side on a double-sided 3.5")
// and TRKS (160 x 8-byte entries: startBlock u16, blockCount u16, bitCount
// u32, data at startBlock*512). WOZ1's TRKS is a different, fixed-size shape
// and its 3.5" support is not a thing in the wild, so only WOZ2 is decoded.
bool Disk35Image::loadWoz(const std::vector<uint8_t>& buf,
                          const std::string& imgPath)
{
    if (buf[3] != '2') {
        lastError_ = "Disk35Image: " + imgPath +
                     " is WOZ1; only WOZ2 3.5\" images are supported";
        return false;
    }

    std::size_t infoOff = 0, tmapOff = 0, trksOff = 0;
    std::size_t infoLen = 0, tmapLen = 0, trksLen = 0;
    for (std::size_t i = 12; i + 8 <= buf.size();) {
        const uint32_t len = rd32(buf.data() + i + 4);
        const std::size_t payload = i + 8;
        if (len > buf.size() - payload) break;          // truncated chunk
        if (!std::memcmp(buf.data() + i, "INFO", 4)) { infoOff = payload; infoLen = len; }
        if (!std::memcmp(buf.data() + i, "TMAP", 4)) { tmapOff = payload; tmapLen = len; }
        if (!std::memcmp(buf.data() + i, "TRKS", 4)) { trksOff = payload; trksLen = len; }
        i = payload + len;
    }
    if (!infoOff || infoLen < 2 || !tmapOff || tmapLen < 160 || !trksOff) {
        lastError_ = "Disk35Image: " + imgPath + " has no usable WOZ2 "
                     "INFO/TMAP/TRKS chunks";
        return false;
    }
    const uint8_t diskType = buf[infoOff + 1];          // 1 = 5.25", 2 = 3.5"
    if (diskType != 2) {
        lastError_ = "Disk35Image: " + imgPath + " is a 5.25\" WOZ "
                     "(INFO.disk_type " + std::to_string(diskType) +
                     "); mount it as a Disk II image";
        return false;
    }

    blocks_.assign(kBytesPerImage, 0);
    std::vector<uint8_t> seen(kBlockCount, 0);
    int decoded = 0;

    // Caps on what a FILE may make this loader allocate and chew through.
    // DiskImage::loadWoz has carried these since it was written; this loader
    // did not, and the omission is a hang rather than a crash, which is why
    // the fuzzer never flagged it. A WOZ2 claiming bitCount 0xFFFFFFFF over a
    // 15 MB payload, with all 160 TMAP slots pointing at that one track, took
    // 31 s and 294 MB before failing anyway — and mount35() holds the
    // emulator's state mutex across the call, so the CPU worker and the whole
    // UI are frozen for the duration. A real 3.5" track is ~75 000 bits.
    constexpr uint32_t    kMaxTrackBits     = 1u << 20;    // 12x the real thing
    constexpr std::size_t kMaxExpandedBytes = 64u << 20;
    std::size_t expandedBytes = 0;

    for (int slot = 0; slot < 160; ++slot) {
        const uint8_t ti = buf[tmapOff + slot];
        if (ti == 0xFF) continue;                        // unformatted
        const std::size_t e = trksOff + static_cast<std::size_t>(ti) * 8;
        if (e + 8 > trksOff + trksLen) continue;
        const uint32_t startBlk = rd16(buf.data() + e);
        const uint32_t blkCount = rd16(buf.data() + e + 2);
        const uint32_t bitCount = rd32(buf.data() + e + 4);
        if (!blkCount || !bitCount) continue;
        if (bitCount > kMaxTrackBits) continue;          // not a real track
        const std::size_t off = static_cast<std::size_t>(startBlk) * 512u;
        const std::size_t len = static_cast<std::size_t>(blkCount) * 512u;
        if (off >= buf.size() || len > buf.size() - off) continue;
        // Aggregate budget: 160 slots may all point at the same TRK, so a
        // per-track cap alone still lets the file multiply the work by 160.
        if (bitCount > kMaxExpandedBytes - expandedBytes) break;
        expandedBytes += bitCount;

        // The track is a CIRCLE. Walking it once leaves the sector that
        // straddles the seam undecodable — worth ~1 sector per track-side,
        // which is a lot of missing blocks. Walk it once plus an overlap so
        // a wrap-crossing sector appears whole; duplicates are dropped by
        // `seen` below.
        std::vector<uint8_t> cells =
            sony35::cellsFromPackedBits(buf.data() + off, len, bitCount);
        const std::size_t once = cells.size();
        if (once == 0) continue;
        // Reserve, then append from an INDEX range rather than from
        // iterators into `cells` itself. `insert(end(), begin(), begin()+n)`
        // takes its input range from the very container it is resizing,
        // which [sequence.reqmts] leaves undefined — it happens to work on
        // today's libstdc++/libc++ because the reallocation path copies out
        // of the still-live old buffer, but that is an implementation
        // detail, not a guarantee, and a hardened or checked-iterator build
        // is entitled to trap it.
        const std::size_t overlap = std::min<std::size_t>(once, 40000);
        cells.reserve(once + overlap);
        for (std::size_t k = 0; k < overlap; ++k) cells.push_back(cells[k]);

        const int track = slot / 2;
        sony35::decodeSectors(sony35::nibblesFromCells(cells),
                              /*expectTrack=*/track,
            [&](int tr, int head, int sec, const uint8_t* data) {
                const int bi = sony35::blockIndexFor(tr, head, sec);
                if (bi < 0 || bi >= kBlockCount || seen[bi]) return;
                std::memcpy(blocks_.data() +
                                static_cast<std::size_t>(bi) * kBlockBytes,
                            data, kBlockBytes);
                seen[bi] = 1;
                ++decoded;
            });
    }

    if (decoded < kBlockCount) {
        // Report rather than refuse: a dump with a few unreadable sectors is
        // still worth mounting, and saying WHICH is the difference between
        // "POM2 can't read this" and "this dump has holes".
        pom2::log().warn("Disk35",
            "WOZ " + imgPath + ": decoded " + std::to_string(decoded) +
            " of " + std::to_string(kBlockCount) +
            " blocks; the rest read as zeros");
    }
    if (decoded == 0) {
        lastError_ = "Disk35Image: " + imgPath +
                     " yielded no Sony GCR sectors (not an 800K 3.5\" disk?)";
        return false;
    }
    return true;
}

}  // namespace pom2
