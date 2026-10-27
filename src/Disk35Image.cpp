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

// Disk35Image — block projections and immutable capture source loading.
// Native GCR flux splicing/serialization lives in Disk35Image_Bitstream.cpp.

#include "Disk35Image.h"
#include "Disk35Bitstream.h"
#include "TwoImgHeader.h"
#include "Disk35Mfm.h"
#include "MediaNotch.h"

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

int Disk35Image::blockIndexFor(int track, int head, int sector) const
{
    if (track < 0 || track >= kTracks || head < 0 || head >= headCount() ||
        sector < 0 || sector >= (blockCount() == kBlockCount1440 ? 18 : sectorsForTrack(track))) return -1;
    if (blockCount() == kBlockCount1440) return (track * 2 + head) * 18 + sector;
    int index = 0;
    for (int t = 0; t < track; ++t) index += headCount() * sectorsForTrack(t);
    return index + head * sectorsForTrack(track) + sector;
}

bool Disk35Image::loadFile(const std::string& imgPath)
{
    // Loading a replacement is an implicit eject. First make the outgoing
    // medium durable, then parse the candidate into a separate object so any
    // malformed/unreadable file leaves the current disk fully mounted.
    if (!saveDirty()) return false;
    Disk35Image replacement;
    replacement.writeBackEnabled_ = writeBackEnabled_;
    replacement.mfmCapable_ = mfmCapable_;
    if (!replacement.loadFileUnchecked(imgPath)) {
        lastError_ = replacement.lastError_;
        return false;
    }
    *this = std::move(replacement);
    // Named at once, not on the first capture: `mountId()` must tell this
    // mount from the next even when it never wrote.
    lineage_ = nextMediaCaptureSeq();
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
    // Raw media is at most 1.44 MiB. Allow wrappers/trailers ample headroom, while
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
    return parseImageBytes(buf, imgPath, pom2::mediaFileIsReadOnly(imgPath));   // MediaNotch.h
}

bool Disk35Image::loadPrepared(std::vector<uint8_t>&& bytes, const std::string& imgPath,
                               bool hostReadOnly)
{
    // loadFile without the file read: phase 2 of a two-phase mount, under the
    // machine lock, on bytes phase 1 read unlocked (MediaMount.h).
    if (!saveDirty()) return false;
    Disk35Image replacement;
    replacement.writeBackEnabled_ = writeBackEnabled_;
    replacement.mfmCapable_ = mfmCapable_;
    replacement.eject();
    replacement.path_ = imgPath;
    constexpr std::size_t kMaxDisk35ImageBytes = 16 * 1024 * 1024;   // as loadFileUnchecked
    if (bytes.empty() || bytes.size() > kMaxDisk35ImageBytes) {
        lastError_ = "Disk35Image: unusable image size " + imgPath;
        return false;
    }
    if (!replacement.parseImageBytes(bytes, imgPath, hostReadOnly)) {
        lastError_ = replacement.lastError_;
        return false;
    }
    *this = std::move(replacement);
    lineage_ = nextMediaCaptureSeq();
    return true;
}

bool Disk35Image::parseImageBytes(std::vector<uint8_t>& buf, const std::string& imgPath,
                                  bool hostReadOnly)
{
    hostReadOnly_ = hostReadOnly;
    const std::size_t n = buf.size();

    // Detect 2IMG-wrapped 400K, 800K or SuperDrive 1.44M payloads.
    if (n >= 64 &&
        buf[0] == '2' && buf[1] == 'I' && buf[2] == 'M' && buf[3] == 'G')
    {
        const uint16_t headerLen = rd16(buf.data() + 8);
        const uint32_t format    = rd32(buf.data() + 12);
        const uint32_t flags     = rd32(buf.data() + 16);
        const uint32_t dataOff   = rd32(buf.data() + 24);
        const uint32_t dataLen   = twoImgDataLength(buf.data());

        // After the declared header, which holds every defined field (bytes
        // 0-47) once it is 52 long, as in Block512Backing. 64 is only the
        // usual size: a 52-byte header with data at 52 is a legal 2IMG.
        if (headerLen < 52 || dataOff < headerLen || dataOff > n ||
            dataLen == 0 ||
            static_cast<std::size_t>(dataOff) + dataLen > n)
        {
            lastError_ = "Disk35Image: malformed 2IMG header in " + imgPath;
            return false;
        }
        // format 1 = ProDOS block order.
        if (format != 1) {
            lastError_ = "Disk35Image: 2IMG format " + std::to_string(format) +
                         " not supported for 3.5\" disks (need ProDOS=1)";
            return false;
        }
        if (dataLen != kBytesPerImage && dataLen != kBytesPerImage400 &&
            !(mfmCapable_ && dataLen == kBytesPerImage1440)) {
            lastError_ = "Disk35Image: 2IMG ProDOS payload must be 400K or 800K"
                         " (1.44M requires a SuperDrive), got " +
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
        fileWriteProtected_ = pom2::twoImgWriteProtected(flags);
        kind_   = dataLen == kBytesPerImage400 ? ImageKind::TwoImg400k :
                  dataLen == kBytesPerImage1440 ? ImageKind::TwoImg1440k : ImageKind::TwoImg800k;
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
    // Keep capture timing for physical GCR writes and MFM sector splicing.
    // GCR HLE block services require raw export; MFM updates valid data fields.
    const bool moof = n >= 12 &&
        std::memcmp(buf.data(), "MOOF\xFF\x0A\x0D\x0A", 8) == 0;
    if (moof || (n >= 12 && buf[0] == 'W' && buf[1] == 'O' &&
                 buf[2] == 'Z' && (buf[3] == '1' || buf[3] == '2')))
    {
        if (!loadBitstream(buf, imgPath, moof)) return false;

        kind_   = moof ? (blocks_.size() == kBytesPerImage400 ? ImageKind::Moof400k
                           : blocks_.size() == kBytesPerImage1440 ? ImageKind::Moof1440k
                                                                  : ImageKind::Moof35)
                       : ImageKind::Woz35;
        loaded_ = true;
        dirty_  = false;
        return true;
    }

    // Bare ProDOS-order payload (.po, .dsk, .image).
    if (n == kBytesPerImage || n == kBytesPerImage400 ||
        (mfmCapable_ && n == kBytesPerImage1440)) {
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
        // three names reaching here (.po, .dsk, .image) hold raw block
        // payloads and take the same save path for each accepted geometry,
        // so there was never a technical reason to treat them
        // differently — the old rule marked .dsk/.image physically WP on
        // the grounds that such dumps are "sometimes read-only by
        // convention".
        //
        // That is the wrong layer for a convention. `fileWriteProtected_`
        // is the PHYSICAL tab: it overrides the user's own
        // `setWriteBackEnabled` policy, so an 800K .dsk stayed read-only
        // even after the user explicitly asked for write-back and got no
        // explanation why. Writes still require an enabled write-back
        // policy (enabled by default) and a writable host file. GCR
        // capture block restrictions are handled separately.
        fileWriteProtected_ = false;
        kind_   = n == kBytesPerImage400 ? ImageKind::Raw400k :
                  n == kBytesPerImage1440 ? ImageKind::Raw1440k : ImageKind::Raw800k;
        loaded_ = true;
        dirty_  = false;
        return true;
    }

    if (n == kBytesPerImage1440 && !mfmCapable_) {
        lastError_ = "Disk35Image: 1.44M MFM requires an Apple IIe SuperDrive controller";
        return false;
    }
    lastError_ = "Disk35Image: " + imgPath +
                 " is not a supported 400K/800K/1.44M 3.5\" image (size " +
                 std::to_string(n) + ")";
    return false;
}

void Disk35Image::eject()
{
    loaded_              = false;
    dirty_               = false;
    detachedSession_     = false;
    fileWriteProtected_  = false;
    hostReadOnly_        = false;
    kind_                = ImageKind::Unknown;
    blocks_.clear();
    moofCells_.reset();
    bitstream_.reset();
    decodedTrackDirty_.fill(false);
    moofBitTiming_ = 0;
    twoImgHeaderRaw_.clear();
    twoImgTrailerRaw_.clear();
    path_.clear();
    lastError_.clear();
    autosave_.reset();
    lineage_ = 0;
}

bool Disk35Image::readBlock(uint32_t idx, uint8_t out[kBlockBytes]) const
{
    if (!loaded_ || idx >= blockCount()) return false;
    if (bitstream_ && kind_ == ImageKind::Moof1440k) refreshDecodedTrack(int(idx / 18));
    else if (bitstream_) for (int track = 0; track < 80; ++track) {
        const int first = blockIndexFor(track, 0, 0);
        const int count = sectorsForTrack(track);
        if (first >= 0 && idx >= uint32_t(first) && idx < uint32_t(first + count * headCount())) {
            refreshDecodedTrack(track * 2 + int((idx - uint32_t(first)) / uint32_t(count)));
            break;
        }
    }
    std::memcpy(out, blocks_.data() + idx * kBlockBytes, kBlockBytes);
    return true;
}

const std::vector<uint8_t>* Disk35Image::originalBitCells(int track, int head) const
{
    if (!loaded_ || !isBitstream() ||
        !moofCells_ || track < 0 || track >= kTracks ||
        head < 0 || head >= kSides) return nullptr;
    const auto& cells = (*moofCells_)[static_cast<std::size_t>(track * 2 + head)];
    return cells.empty() ? nullptr : &cells;
}

bool Disk35Image::writeBlock(uint32_t idx, const uint8_t in[kBlockBytes])
{
    if (!loaded_ || idx >= blockCount()) return false;
    if (isBlockWriteProtected()) return false;
    if (kind_ == ImageKind::Moof1440k) return writeMfmBlock(idx, in);
    std::memcpy(blocks_.data() + idx * kBlockBytes, in, kBlockBytes);
    dirty_ = true;
    autosave_.noteWrite();
    // A rewind may not cross a media write: an 800 KB image is not captured
    // per rewind frame, so the controller clears the ring instead. See
    // `pom2::mediaWriteEpoch()` in Block512Backing.h.
    noteMediaWrite();
    return true;
}

bool Disk35Image::exportRawTo(const std::string& outPath,
                             std::string& errOut) const
{
    for (int slot = 0; slot < 160; ++slot) refreshDecodedTrack(slot);
    if (!loaded_ || (blocks_.size() != kBytesPerImage && blocks_.size() != kBytesPerImage400 &&
                     blocks_.size() != kBytesPerImage1440)) {
        errOut = "no 400K, 800K or 1.44M image loaded";
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
    const std::string tmp = tempSiblingPath(outPath).string();
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
    // A converted image is a NEW file. The exclusive publisher flushes it
    // before making it visible and refuses a destination created meanwhile.
    if (!publishNewFileAtomic(tmp, outPath, ec)) {
        if (ec == std::errc::file_exists)
            errOut = outPath + " already exists";
        else
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
    // Write-back off: a SUCCESSFUL no-op. Opting out means "discard on
    // eject", not "refuse to eject" — dirty_ is left set so re-enabling
    // write-back before the eject still saves the session's writes.
    // Treating that as a hard error wedged the 3.5" paths for the rest of
    // the session (eject, disk swap, the //c+ mount35 flush gate).
    //
    // The notch (hostReadOnly_) is NOT in this gate. Folding it in was
    // the Block512Backing defect `isMediumLocked()` closed: guest writes
    // block N → dirty → user ticks "Write-protected" → takeWriteBack
    // refused → saveDirty returned true with dirty_ still set → eject()
    // dropped the only copy. A chmod-read-only file still accepts the
    // temp-sibling rename (AtomicFileReplace.h). A medium locked at
    // mount (WOZ / 2IMG header) can hold no dirty block at all.
    if (detachedSession_ || !writeBackEnabled_ || fileWriteProtected_) return out;

    out.valid = true;
    out.seq   = nextMediaCaptureSeq();
    if (lineage_ == 0) lineage_ = nextMediaCaptureSeq();
    out.lineage = lineage_;
    out.path  = path_;
    if (bitstream_) {
        out.bitstream = bitstream_;   // serialised in phase 2 (STORAGE-012)
    } else {
    out.bytes.reserve(twoImgHeaderRaw_.size() + blocks_.size() +
                      twoImgTrailerRaw_.size());
    out.bytes.insert(out.bytes.end(),
                     twoImgHeaderRaw_.begin(), twoImgHeaderRaw_.end());
    out.bytes.insert(out.bytes.end(), blocks_.begin(), blocks_.end());
    out.bytes.insert(out.bytes.end(),
                     twoImgTrailerRaw_.begin(), twoImgTrailerRaw_.end());
    }
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
    if (!pending.error.empty()) { error = pending.error; return false; }
    if (pending.bitstream && !serializeBitstreamOf(*pending.bitstream, pending.bytes, error))
        return false;   // the caller re-marks the medium dirty, as for an I/O failure
    // In capture order: the background autosave and the explicit eject /
    // flush paths commit from different threads (MediaAutosave.h).
    return commitMediaInOrder(pending.path, pending.lineage, pending.seq, error,
        [&pending](std::string& err) { return writeWholeFile(pending, err); });
}

bool Disk35Image::writeWholeFile(const PendingWriteBack& pending,
                                 std::string& error)
{

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
    // A rename replaces the inode, so the temp file's umask-derived mode
    // would become the image's: carry the image's permissions across, read
    // at the rename and not before the write (a notch flipped meanwhile).
    std::error_code ec;
    if (!replaceMediaFileAtomic(tmp, pending.path, ec)) {
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


std::shared_ptr<MediaCommitOperation>
Disk35Image::pollAutosave(MediaCommitExecutor& executor, bool force)
{
    if (autosave_.collect("Disk35")) dirty_ = false;
    const bool pending = !detachedSession_ && loaded_ && dirty_ && writeBackEnabled_ &&
                         !fileWriteProtected_;
    if (!autosave_.due(pending, force)) return autosave_.inFlight();
    PendingWriteBack capture = takeWriteBack();
    if (!capture.valid) return autosave_.inFlight();
    // `takeWriteBack` retires the flag for the eject-style callers; an
    // autosave keeps it until the file has landed (MediaAutosave::collect).
    dirty_ = true;
    auto payload = std::make_shared<PendingWriteBack>(std::move(capture));
    return autosave_.start(executor, [payload] {
        MediaCommitResult r;
        r.ok = commitWriteBack(std::move(*payload), r.error);
        return r;
    });
}

MediumPersistence Disk35Image::persistence() const
{
    MediumPersistence p;
    p.loaded  = loaded_;
    p.pending = loaded_ && dirty_;
    p.path    = path_;
    p.state   = autosave_.state(loaded_, dirty_,
                                writeBackEnabled_ && !fileWriteProtected_ && !detachedSession_);
    if (detachedSession_) p.state = "session";
    p.error   = autosave_.error();
    return p;
}

// ─── WOZ 3.5" (flux) → blocks ─────────────────────────────────────────────
//
// WOZ2 layout: 12-byte header, then `ID(4) size(4) payload` chunks. We need
// TMAP (160 bytes: one track-index per track*2+side on a double-sided 3.5")
// and TRKS (160 x 8-byte entries: startBlock u16, blockCount u16, bitCount
// u32, data at startBlock*512). WOZ1's TRKS is a different, fixed-size shape
// and its 3.5" support is not a thing in the wild, so only WOZ2 is decoded.
bool Disk35Image::loadBitstream(const std::vector<uint8_t>& buf,
                                const std::string& imgPath, bool moof)
{
    if (!moof && buf[3] != '2') {
        lastError_ = "Disk35Image: " + imgPath +
                     " is WOZ1; only WOZ2 3.5\" images are supported";
        return false;
    }

    // The MOOF and WOZ headers store CRC32 of all chunks. Zero is the
    // format's explicit "not calculated" value, also used by synthetic
    // fixtures. A WOZ went unchecked, so a corrupt 3.5" one mounted silently
    // where the 5.25" loader refuses the same file.
    {
        const uint32_t expected = rd32(buf.data() + 8);
        if (expected != 0) {
            uint32_t crc = 0xFFFFFFFFu;
            for (std::size_t i = 12; i < buf.size(); ++i) {
                crc ^= buf[i];
                for (int bit = 0; bit < 8; ++bit)
                    crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
            }
            if (~crc != expected) {
                lastError_ = std::string("Disk35Image: ") + (moof ? "MOOF" : "WOZ") +
                             " CRC32 mismatch in " + imgPath;
                return false;
            }
        }
    }

    std::size_t infoOff = 0, tmapOff = 0, trksOff = 0, fluxOff = 0;
    std::size_t infoLen = 0, tmapLen = 0, trksLen = 0, fluxLen = 0;
    for (std::size_t i = 12; i + 8 <= buf.size();) {
        const uint32_t len = rd32(buf.data() + i + 4);
        const std::size_t payload = i + 8;
        if (len > buf.size() - payload) break;          // truncated chunk
        if (!std::memcmp(buf.data() + i, "INFO", 4)) { infoOff = payload; infoLen = len; }
        if (!std::memcmp(buf.data() + i, "TMAP", 4)) { tmapOff = payload; tmapLen = len; }
        if (!std::memcmp(buf.data() + i, "TRKS", 4)) { trksOff = payload; trksLen = len; }
        if (!std::memcmp(buf.data() + i, "FLUX", 4)) { fluxOff = payload; fluxLen = len; }
        i = payload + len;
    }
    if (!infoOff || infoLen < (moof ? 44u : 3u) ||
        !tmapOff || tmapLen < 160 || !trksOff || trksLen < 160 * 8) {
        lastError_ = "Disk35Image: " + imgPath + " has no usable " +
                     (moof ? "MOOF" : "WOZ2") + " "
                     "INFO/TMAP/TRKS chunks";
        return false;
    }
    const uint8_t diskType = buf[infoOff + 1];
    const bool singleSide = moof && diskType == 1;
    const bool mfm = moof && diskType == 3;
    if (mfm && !mfmCapable_) {
        lastError_ = "Disk35Image: 1.44M MFM requires an Apple IIe SuperDrive controller";
        return false;
    }
    if (diskType != 2 && !singleSide && !mfm) {
        lastError_ = "Disk35Image: " + imgPath + " has unsupported " +
                     (moof ? "MOOF" : "WOZ") + " disk type " +
                     std::to_string(diskType) +
                     (moof ? " (only 400K/800K GCR and 1.44M MFM are supported)"
                           : " (5.25-inch images belong in Disk II)");
        return false;
    }
    const uint8_t bitTiming = moof ? buf[infoOff + 4] :
        infoLen >= 40 && buf[infoOff + 39] ? buf[infoOff + 39] : 16;
    if (moof && (buf[infoOff] < 1 || bitTiming < 8 || bitTiming > 64)) {
        lastError_ = "Disk35Image: invalid MOOF INFO version or bit timing";
        return false;
    }
    const size_t fluxFields = moof ? 40 : 46;
    const bool hasFlux = infoLen >= fluxFields + 4 && (moof || buf[infoOff] >= 3) &&
                         rd16(buf.data() + infoOff + fluxFields) != 0 &&
                         rd16(buf.data() + infoOff + fluxFields + 2) != 0;
    if (hasFlux && (!fluxOff || fluxLen < 160)) {
        lastError_ = "Disk35Image: MOOF declares FLUX but has no FLUX map";
        return false;
    }

    const int expectedBlocks = singleSide ? kBlockCount400 :
                               mfm ? kBlockCount1440 : kBlockCount;
    blocks_.assign(static_cast<std::size_t>(expectedBlocks) * kBlockBytes, 0);
    auto originalTracks = std::make_shared<TrackCells>();
    auto backing = std::make_shared<Disk35Bitstream>();
    backing->moof = moof; backing->raw = buf;
    std::vector<uint8_t> seen(expectedBlocks, 0);
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
    const std::size_t kMaxExpandedBytes = (mfm ? 192u : 64u) << 20;
    std::size_t expandedBytes = 0;

    for (int slot = 0; slot < 160; ++slot) {
        if (singleSide && (slot & 1)) continue;
        const uint8_t fluxIndex = hasFlux ? buf[fluxOff + slot] : 0xFF;
        const bool fluxTrack = fluxIndex != 0xFF;
        const uint8_t ti = fluxTrack ? fluxIndex : buf[tmapOff + slot];
        if (ti >= 160) continue;                        // unformatted
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
        std::vector<uint8_t> cells;
        if (fluxTrack) {
            // MOOF FLUX entries use the TRK bitCount field as a BYTE count.
            // Each byte is a 125 ns delta; 255 continues the current delta.
            if (bitCount > len) continue;
            uint64_t totalTicks = 0;
            for (uint32_t j = 0; j < bitCount; ++j) totalTicks += buf[off + j];
            const uint64_t cellCount =
                (totalTicks + bitTiming / 2) / bitTiming;
            if (!cellCount || cellCount > kMaxTrackBits ||
                cellCount * 5 > kMaxExpandedBytes - expandedBytes) continue;
            expandedBytes += static_cast<std::size_t>(cellCount) * 5;
            cells.assign(static_cast<std::size_t>(cellCount), 0);
            if (totalTicks <= UINT32_MAX) backing->periods[slot] = uint32_t(totalTicks);
            uint64_t tick = 0;
            for (uint32_t j = 0; j < bitCount; ++j) {
                const uint8_t delta = buf[off + j];
                tick += delta;
                // The final delta closes the circular track at its index.
                if (delta == 255 || j + 1 == bitCount) continue;
                if (tick < totalTicks) backing->transitions[slot].push_back(uint32_t(tick));
                const uint64_t cell = (tick + bitTiming / 2) / bitTiming;
                if (cell < cellCount) cells[static_cast<std::size_t>(cell)] = 1;
            }
        } else {
            if (bitCount > len * 8u ||
                uint64_t(bitCount) * 5 > kMaxExpandedBytes - expandedBytes) continue;
            expandedBytes += size_t(bitCount) * 5;
            cells = sony35::cellsFromPackedBits(buf.data() + off, len, bitCount);
        }

        (*originalTracks)[static_cast<std::size_t>(slot)] = cells;
        if (!fluxTrack) {
            backing->periods[slot] = uint32_t(cells.size() * bitTiming);
            for (size_t cell = 0; cell < cells.size(); ++cell)
                if (cells[cell]) backing->transitions[slot].push_back(uint32_t(cell * bitTiming));
        }

        if (mfm) {
            const int track = slot / 2;
            const int head = slot % 2;
            const std::size_t overlap = std::min<std::size_t>(cells.size(), 9000);
            cells.reserve(cells.size() + overlap);
            for (std::size_t k = 0; k < overlap; ++k) cells.push_back(cells[k]);
            mfm35::decodeMfmTrack(cells, track, head, [&](int sector, const uint8_t* data, size_t) {
                const int bi = (track * 2 + head) * 18 + sector - 1;
                if (bi < 0 || bi >= expectedBlocks || seen[bi]) return;
                std::memcpy(blocks_.data() + static_cast<std::size_t>(bi) * kBlockBytes,
                            data, kBlockBytes);
                seen[bi] = 1;
                ++decoded;
            });
            continue;
        }

        // The track is a CIRCLE. Walking it once leaves the sector that
        // straddles the seam undecodable — worth ~1 sector per track-side,
        // which is a lot of missing blocks. Walk it once plus an overlap so
        // a wrap-crossing sector appears whole; duplicates are dropped by
        // `seen` below.
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
                const int bi = blockIndexFor(tr, head, sec);
                if (head != slot % 2 || bi < 0 || bi >= expectedBlocks || seen[bi]) return;
                std::memcpy(blocks_.data() +
                                static_cast<std::size_t>(bi) * kBlockBytes,
                            data, kBlockBytes);
                seen[bi] = 1;
                ++decoded;
            });
    }

    if (decoded < expectedBlocks) {
        // Report rather than refuse: a dump with a few unreadable sectors is
        // still worth mounting, and saying WHICH is the difference between
        // "POM2 can't read this" and "this dump has holes".
        pom2::log().warn("Disk35",
            (moof ? "MOOF " : "WOZ ") + imgPath + ": decoded " + std::to_string(decoded) +
            " of " + std::to_string(expectedBlocks) +
            " blocks; the rest read as zeros");
    }
    if (decoded == 0 && mfm) {
        lastError_ = "Disk35Image: " + imgPath +
                     " yielded no CRC-valid MFM sectors";
        return false;
    }
    if (decoded == 0 && !moof && !hasFlux) {
        lastError_ = "Disk35Image: " + imgPath +
                     " yielded no Sony GCR sectors (not an 800K 3.5\" disk?)";
        return false;
    }
    {
        bool anyTrack = false;
        for (const auto& track : *originalTracks)
            if (!track.empty()) { anyTrack = true; break; }
        if (!anyTrack) {
            lastError_ = "Disk35Image: " + imgPath +
                         " has no usable MOOF tracks";
            return false;
        }
        moofCells_ = std::move(originalTracks);
        moofBitTiming_ = bitTiming;
        fileWriteProtected_ = buf[infoOff + 2] != 0;
        bitstream_ = std::move(backing);
        // A layout the write-back serialiser refuses (trailing garbage, an
        // unaligned TRKS end, FLUX before TRKS) mounts write-protected: its
        // guest writes could never reach the file, so eject would keep it
        // dirty for ever and quit would drop them (STORAGE-012).
        std::string why;
        if (!fileWriteProtected_ && !bitstreamRewritable(*bitstream_, why)) {
            fileWriteProtected_ = true;
            pom2::log().warn("Disk35", imgPath + " mounts write-protected: " + why);
        }
    }
    return true;
}

}  // namespace pom2
