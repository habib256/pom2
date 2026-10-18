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

// Disk35Image — 400K/800K GCR and 1.44M MFM block images plus WOZ/MOOF
// captures. Native GCR writes preserve 125 ns flux timing and container
// metadata; GCR block services require conversion to a raw writable image.
// MFM block writes splice existing CRC-valid sectors. All writes use the ordered two-phase
// persistence path; the medium's header and host notch remain authoritative.

#ifndef POM2_DISK35_IMAGE_H
#define POM2_DISK35_IMAGE_H

#include "MediaAutosave.h"
#include "MediaWritePolicy.h"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pom2 {
struct Disk35Bitstream;

class Disk35Image final : public AutosavedMedium
{
public:
    /// 800K 3.5" geometry: 80 tracks × 2 sides × 10 avg sectors × 512 B
    /// = 819 200 bytes. Per-track sector count is zoned (12/11/10/9/8)
    /// for variable-rate Sony recording. Phase 1 stores the flat
    /// payload only; the zone schedule lives in `kSectorsForTrack`
    /// for the Phase 2 encoder.
    static constexpr int      kTracks       = 80;
    static constexpr int      kSides        = 2;
    static constexpr int      kBlockBytes   = 512;
    static constexpr int      kBlockCount   = 1600;
    static constexpr uint32_t kBytesPerImage = kBlockCount * kBlockBytes; // 819200
    static constexpr int      kBlockCount400 = 800;
    static constexpr uint32_t kBytesPerImage400 = kBlockCount400 * kBlockBytes;
    static constexpr int      kBlockCount1440 = 2880;
    static constexpr uint32_t kBytesPerImage1440 = kBlockCount1440 * kBlockBytes;

    /// MAME `ap_dsk35.cpp:apple35_sectors_per_track[]` — count of
    /// physical sectors on the given 3.5" track number (0..79). The 5
    /// zones run 0..15 / 16..31 / 32..47 / 48..63 / 64..79.
    static int sectorsForTrack(int track);

    enum class ImageKind {
        Unknown,
        Raw800k,       // bare 819 200-byte payload (.po, .dsk-as-prodos)
        TwoImg800k,    // .2mg with 2IMG header wrapping a 819 200 payload
        Woz35,         // WOZ2 flux dump, GCR-decoded to blocks at load
        Moof35,        // MOOF 800K GCR bitstream/flux
        Moof400k,      // MOOF 400K single-sided GCR bitstream/flux
        Raw400k,       // bare 409 600-byte payload
        TwoImg400k,    // 2IMG-wrapped 400K payload
        Moof1440k,     // MOOF 1.44M MFM bitstream/flux with sector splicing
        Raw1440k,      // bare 1 474 560-byte MFM payload
        TwoImg1440k,   // 2IMG-wrapped 1.44M payload
    };

    Disk35Image() = default;

    /// Load a 3.5" image. Accepts 400K and 800K GCR raw/2IMG payloads,
    /// WOZ2 800K GCR, and MOOF 400K/800K GCR. When `mfmCapable_` is set by
    /// a SuperDrive-class unit, also accepts 1.44M raw/2IMG and MOOF MFM.
    /// GCR captures allow native flux writes; block writes require export.
    /// MFM block writes splice existing CRC-valid sectors.
    /// On failure, returns false and populates `lastError`.
    bool loadFile(const std::string& path);

    /// Original MOOF bit cells for the physical track and head. Null when
    /// this is a block image, or when the MOOF track is unformatted. The
    /// returned view is valid until the next write/mount under the machine lock.
    const std::vector<uint8_t>* originalBitCells(int track, int head) const;
    uint8_t bitTiming125ns() const { return moofBitTiming_; }
    const std::vector<uint32_t>* originalFluxTicks(int track, int head) const;
    uint32_t trackPeriod125ns(int track, int head) const;
    /// Splice a circular interval without quantizing untouched capture data.
    /// `start/end/transitions` share a 125 ns clock whose index is zero.
    bool writeFlux125ns(int track, int head, int64_t start, int64_t end,
                        const int64_t* transitions, int count, uint32_t period);
    // Self-contained, physically protected media for archival sessions.
    bool appendReadOnlySession(std::vector<uint8_t>& out, std::string& error) const;
    bool loadReadOnlySession(const uint8_t* data, size_t size, std::string& error);
    bool appendSessionMedia(std::vector<uint8_t>& out, std::string& error) const;
    bool loadSessionMedia(const uint8_t* data, size_t size, std::string& error, bool allowWritable);
    bool sessionCaptureReady() const { return !autosave_.inFlight(); }
    bool sessionReplaceable() const { return (!dirty_ || detachedSession_) && !autosave_.inFlight(); }
    bool isSessionMedia() const { return detachedSession_; }
    size_t sessionStorageBytes() const;
    bool isBlockWriteProtected() const { return isWriteProtected() || (isBitstream() && kind_ != ImageKind::Moof1440k); }

    /// Discard the loaded image.
    void eject();

    bool        isLoaded()         const { return loaded_; }
    const std::string& path()      const { return path_; }
    const std::string& lastError() const { return lastError_; }
    ImageKind   kind()             const { return kind_; }
    bool isBitstream() const {
        return kind_ == ImageKind::Woz35 || kind_ == ImageKind::Moof35 ||
               kind_ == ImageKind::Moof400k || kind_ == ImageKind::Moof1440k;
    }
    void setMfmCapable(bool capable) { mfmCapable_ = capable; }
    bool isMfmCapable() const { return mfmCapable_; }
    uint32_t blockCount() const { return static_cast<uint32_t>(blocks_.size() / kBlockBytes); }
    int headCount() const { return blockCount() == kBlockCount400 ? 1 : 2; }
    int blockIndexFor(int track, int head, int sector) const;

    /// Read 512 bytes from block `idx` (0..blockCount()-1). Returns true on
    /// success. Out-of-range or no-image-loaded → false (out is
    /// untouched).
    bool readBlock (uint32_t idx, uint8_t out[kBlockBytes]) const;

    /// Write 512 bytes to block `idx`. No-op (returns false) if no
    /// image is loaded, the block index is out of range, or write-back
    /// is disabled. Mark the image dirty for the next `saveDirty()`.
    bool writeBlock(uint32_t idx, const uint8_t in[kBlockBytes]);

    /// True if any block has been written since the last load/save.
    bool hasUnsavedChanges() const { return dirty_; }

    /// Flush dirty blocks back to the source file. Re-emits the 2IMG
    /// envelope if one was present at load time. Returns true on
    /// success. After save, `dirty_` is cleared.
    /// The replace is atomic (sibling `.pom2tmp` + rename, as in
    /// `DiskImage::saveDirty`): a failed or interrupted save leaves the
    /// user's image exactly as it was rather than truncated.
    ///
    /// Does its file work INLINE, so a caller holding `stateMutex` freezes the
    /// machine and the window for a 800 KB write and two fsyncs. Composed from
    /// the two-phase pair below, which is the split that solves that; this
    /// entry point stays for single-threaded callers with no lock to get out
    /// from under.
    bool saveDirty();

    /// ── Two-phase write-back ────────────────────────────────────────────
    /// The 3.5" mirror of `Block512Backing::takeWriteBack`. Mount was split in
    /// v0.8.5 (`MediaMount.h`); the eject side kept writing 800 KB with
    /// `stateMutex` held — the lock the CPU worker takes every 4096 cycles and
    /// the UI thread takes to paint every frame — and the firmware-issued
    /// eject (`Sony35Drive` register 7) did it from the IWM path itself.
    ///
    /// `bytes` is the COMPLETE file, 2IMG envelope included, so phase 2 needs
    /// nothing from the object: by then the medium may already be gone.
    struct PendingWriteBack {
        bool                 valid = false;   ///< false → phase 2 no-ops
        std::string          path;
        std::vector<uint8_t> bytes;
        /// Capture order (`nextMediaCaptureSeq`): an older capture never
        /// replaces a file a newer one reached. 0 = "treat as newest".
        uint64_t             seq = 0;
        uint64_t             lineage = 0;   ///< the mount it came from
        std::string          error{};      ///< serialization refused; preserve dirty state
    };

    /// Phase 1, WITH the lock: serialise what `saveDirty()` would write and
    /// retire the dirty flag, atomically with the capture. Memcpy only — no
    /// syscall. A no-op capture (nothing dirty, write-back off, medium WP)
    /// returns `valid == false` and leaves `dirty_` alone, so opting back in
    /// to write-back still saves the session's writes.
    PendingWriteBack takeWriteBack();

    /// Phase 2, with NO lock held: perform the deferred write. Static because
    /// the image it came from may no longer exist.
    static bool commitWriteBack(PendingWriteBack&& pending, std::string& error);

    /// Phase-2 FAILURE undo: re-mark the medium dirty so a retry re-captures
    /// it. Unlike the block backing there is no per-block set to merge — the
    /// payload is the whole image — so this is one flag.
    void restoreDirty() { if (loaded_) { dirty_ = true; autosave_.noteWrite(); } }
    /// The same undo, only when this is still the mount a capture came from
    /// (`PendingWriteBack::lineage`; a load or an eject resets it). False —
    /// and nothing touched — when another disk has been mounted since.
    /// This mount's process-unique identity (0 = nothing loaded). Survives
    /// moves; every load draws a new one, so a phase-3 check can tell "the
    /// disk I captured" from "the same file mounted again since".
    uint64_t mountId() const { return loaded_ ? lineage_ : 0; }
    bool restoreDirtyFor(uint64_t lineage)
    {
        if (!loaded_ || lineage == 0 || lineage != lineage_) return false;
        restoreDirty();
        return true;
    }

    /// Background autosave (MediaAutosave.h), `stateMutex` held: the file
    /// follows the guest's writes about a second after they stop, with the
    /// 800 KB write done by the media commit worker. Dirty state is kept
    /// until the commit lands.
    std::shared_ptr<MediaCommitOperation>
    pollAutosave(MediaCommitExecutor& executor, bool force) override;
    MediumPersistence persistence() const override;

    /// Write the decoded 800K payload out as a bare ProDOS-order image
    /// (`.po`), leaving the source file untouched. Returns false and fills
    /// `errOut` on failure.
    ///
    /// GCR block services cannot splice decoded sector changes into a
    /// preserved capture. MFM writes update existing valid sectors directly. Export creates a writable raw volume; tags and
    /// protection patterns are discarded, while the capture stays untouched.
    bool exportRawTo(const std::string& outPath, std::string& errOut) const;

    bool isWriteProtected() const {
        return fileWriteProtected_ || hostReadOnly_ || !writeBackEnabled_;
    }
    /// The medium's OWN lock: the container header bit.
    /// Distinct from `isWriteProtected()`, which also carries the notch
    /// and the write-back opt-out. `takeWriteBack` gates on this plus
    /// `writeBackEnabled_`, not the notch — see Block512Backing::isMediumLocked.
    bool isMediumLocked() const { return fileWriteProtected_; }
    /// The medium's own protection: the container lock flag or
    /// the notch — the host file's read-only bit (MediaNotch.h).
    bool isFileWriteProtected() const { return fileWriteProtected_ || hostReadOnly_; }
    void setWriteBackEnabled(bool on) { writeBackEnabled_ = on; }
    bool isWriteBackEnabled() const   { return writeBackEnabled_; }
    /// Re-apply the notch on a mounted image (the user flipped it in the
    /// UI; the file's bit has already been changed, unlocked).
    void setHostWriteProtected(bool on) { hostReadOnly_ = on; }
    bool isHostWriteProtected() const   { return hostReadOnly_; }

private:
    bool loadFileUnchecked(const std::string& path);
    /// The atomic temp + rename itself, for `commitWriteBack`.
    static bool writeWholeFile(const PendingWriteBack& pending, std::string& error);
    /// WOZ2/MOOF chunk walk + GCR decode into `blocks_`. See the .cpp.
    bool loadBitstream(const std::vector<uint8_t>& buf, const std::string& path,
                       bool moof);
    bool serializeBitstream(std::vector<uint8_t>& bytes, std::string& error) const;
    void refreshDecodedTrack(int slot) const;
    bool writeMfmBlock(uint32_t idx, const uint8_t* in);

    bool         loaded_              = false;
    bool         dirty_               = false;
    bool         detachedSession_     = false;
    MediaAutosave autosave_;
    uint64_t      lineage_ = 0;   // this mount, for commit ordering; 0 = unnamed
    bool         writeBackEnabled_    = pom2::mediaWritableByDefault();   // MediaWritePolicy.h
    bool         fileWriteProtected_  = false;   // header / format says so
    bool         hostReadOnly_        = false;   // the notch (MediaNotch.h)
    ImageKind    kind_                = ImageKind::Unknown;
    bool         mfmCapable_          = false;
    std::string  path_;
    std::string  lastError_;

    // Flat block-major payload (1600 × 512 = 819 200 bytes). Heap-
    // allocated because file-scope members > 800 KB would push the
    // class out of the typical cache-line-friendly small-object range
    // for the controller wiring.
    mutable std::vector<uint8_t> blocks_;
    mutable std::array<bool, kTracks * kSides> decodedTrackDirty_{};

    using TrackCells = std::array<std::vector<uint8_t>, kTracks * kSides>;
    std::shared_ptr<TrackCells> moofCells_;
    std::shared_ptr<Disk35Bitstream> bitstream_;
    uint8_t moofBitTiming_ = 0;

    // Captured 2IMG header verbatim so saveDirty re-emits a valid
    // wrapper. Empty when the source was a raw `.po` / `.dsk`.
    std::vector<uint8_t> twoImgHeaderRaw_;
    std::vector<uint8_t> twoImgTrailerRaw_;
};

/// Where a `Disk35Image::PendingWriteBack` lifted out under `stateMutex` goes
/// to be written with the lock RELEASED.
///
/// The firmware-issued eject (`Sony35Drive`, register 7) runs on the CPU
/// worker inside that lock, so it cannot write and it cannot wait for a
/// write. It captures the payload — a memcpy — and hands it here; the host
/// (`EmulationController`) owns the thread that commits it. A drive with no
/// sink wired falls back to the inline save, which is correct for the
/// single-threaded hosts (CLI, tests) that have no lock to get out from under.
class Disk35WriteBackSink
{
public:
    /// Outcome of the deferred commit, reported back to the submitter.
    ///
    /// The whole point of it is that the submitter may not drop the medium
    /// before it arrives: `submit()` returning means "queued", not "saved",
    /// and a commit CAN fail (disk full, read-only parent). The eject path
    /// therefore holds the disk in the bay until this says the file landed —
    /// the same "a failed save loses nothing" contract the sinkless branch
    /// and `EmulationController::eject35` already honour.
    ///
    /// Invoked with the MACHINE LOCK HELD (the sink takes it; that is why it
    /// may touch drive/image state) and on whatever thread ran the commit, so
    /// the callback must do no file I/O and must not re-take that lock. A
    /// sink that never runs the payload — the destructor's drain — simply
    /// never calls it.
    using Completion = std::function<void(bool ok, const std::string& error)>;

    virtual ~Disk35WriteBackSink() = default;
    /// Takes ownership. Must not block on I/O: it is called with the machine
    /// lock held.
    virtual void submit(Disk35Image::PendingWriteBack&& pending,
                        Completion onDone = {}) = 0;
};

}  // namespace pom2

#endif // POM2_DISK35_IMAGE_H
