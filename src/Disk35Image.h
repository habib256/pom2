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

// Disk35Image — Sony 800K 3.5" disk image holder for the //c+ internal
// drive and the SmartPort daisy-chain port. Loads `.po`, `.2mg` and
// raw 819 200-byte images and exposes a flat block-array view
// (`getBlock(idx) -> 512 bytes`, 1600 blocks total) to the
// `Sony35Drive` consumer.
//
// Phase 1 scope (this file): block storage + 2IMG header support. The
// Sony zoned GCR encoding (5 zones × 16 tracks, 12/11/10/9/8 sectors
// per track, 4:4 GCR) that the IWM bit-cell walker needs to clock out
// is Phase 2 — when that lands, `expandTrackBits(qt)` will be added
// alongside the existing 5.25" path in `DiskImage`. We keep this file
// separate from `DiskImage` because the on-disk physics is different
// enough (variable-rate flux, 80 tracks × 2 sides, no quarter-track
// stepping) that fusing them would tangle two state machines.
//
// MAME source-of-truth references (for the Phase 2 encoder):
//   * `src/lib/formats/ap_dsk35.cpp`     — block ↔ GCR sector encoder
//   * `src/devices/imagedev/floppy.cpp`  — 3.5" zone constants
//   * `src/devices/machine/applefdintf.cpp::add_35`
//
// The image is read/write under user opt-in via `setWriteBackEnabled`.
// Write-back rewrites the .po payload (preserving the 2IMG envelope).

#ifndef POM2_DISK35_IMAGE_H
#define POM2_DISK35_IMAGE_H

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pom2 {

class Disk35Image
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

    /// MAME `ap_dsk35.cpp:apple35_sectors_per_track[]` — count of
    /// physical sectors on the given 3.5" track number (0..79). The 5
    /// zones run 0..15 / 16..31 / 32..47 / 48..63 / 64..79.
    static int sectorsForTrack(int track);

    enum class ImageKind {
        Unknown,
        Raw800k,       // bare 819 200-byte payload (.po, .dsk-as-prodos)
        TwoImg800k,    // .2mg with 2IMG header wrapping a 819 200 payload
        Woz35,         // WOZ2 flux dump, GCR-decoded to blocks at load
    };

    Disk35Image() = default;

    /// Load a 3.5" image. Accepts:
    ///  * 819 200-byte raw images (assumed ProDOS block order)
    ///  * 2IMG-wrapped 819 200-byte ProDOS images
    ///  * WOZ2 flux dumps of 800K disks (`INFO.disk_type == 2`), which are
    ///    GCR-decoded to blocks once, here, through the same `Sony35Gcr`
    ///    walk the drive uses — POM2 has no GCR *encoder*, so a flux image
    ///    has nothing to be mounted as otherwise. Such an image is always
    ///    write-protected: handing blocks back would mean re-encoding the
    ///    original flux.
    /// On failure, returns false and populates `lastError`.
    bool loadFile(const std::string& path);

    /// Discard the loaded image.
    void eject();

    bool        isLoaded()         const { return loaded_; }
    const std::string& path()      const { return path_; }
    const std::string& lastError() const { return lastError_; }
    ImageKind   kind()             const { return kind_; }

    /// Read 512 bytes from block `idx` (0..1599). Returns true on
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
    void restoreDirty() { if (loaded_) dirty_ = true; }

    /// Write the decoded 800K payload out as a bare ProDOS-order image
    /// (`.po`), leaving the source file untouched. Returns false and fills
    /// `errOut` on failure.
    ///
    /// This is the way OUT of a read-only 3.5" WOZ. A WOZ mounts write-
    /// protected because POM2 has the Sony GCR *decoder* but no encoder, so
    /// there is nothing to fold guest writes back into (see `loadFile`) —
    /// but the decode already produced exactly the 1600 blocks a `.po`
    /// holds, so converting costs nothing and the result is fully writable.
    /// The user keeps the capture as an archival master and works on a copy,
    /// which is the right split anyway: a WOZ describes flux, and a program
    /// saving its configuration does not want to be re-mastering flux.
    bool exportRawTo(const std::string& outPath, std::string& errOut) const;

    bool isWriteProtected() const {
        return fileWriteProtected_ || !writeBackEnabled_;
    }
    void setWriteBackEnabled(bool on) { writeBackEnabled_ = on; }
    bool isWriteBackEnabled() const   { return writeBackEnabled_; }

private:
    bool loadFileUnchecked(const std::string& path);
    /// WOZ2 chunk walk + GCR decode into `blocks_`. See the .cpp.
    bool loadWoz(const std::vector<uint8_t>& buf, const std::string& path);

    bool         loaded_              = false;
    bool         dirty_               = false;
    bool         writeBackEnabled_    = false;
    bool         fileWriteProtected_  = false;
    ImageKind    kind_                = ImageKind::Unknown;
    std::string  path_;
    std::string  lastError_;

    // Flat block-major payload (1600 × 512 = 819 200 bytes). Heap-
    // allocated because file-scope members > 800 KB would push the
    // class out of the typical cache-line-friendly small-object range
    // for the controller wiring.
    std::vector<uint8_t> blocks_;

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
