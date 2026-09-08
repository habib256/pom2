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

// DiskImage — loads a 143 360-byte Apple II floppy image (.dsk / .do
// in DOS 3.3 logical sector order, or .po in ProDOS sector order) and
// pre-nibblizes it into per-track buffers ready for the Disk II
// controller to clock out. The on-disk physical sector layout is the
// same in both cases (16 sectors per track, GCR-encoded); the file
// format only changes which file offset maps to which physical sector
// via the skew table.
//
// On-disk layout per sector (~382 nibbles), repeated 16 times per track:
//
//   14 × $FF              sync gap
//   $D5 $AA $96           address-field prologue
//   8 × 4-and-4 nibbles   volume / track / sector / checksum
//   $DE $AA $EB           address-field epilogue
//   5 × $FF               inter-field sync gap
//   $D5 $AA $AD           data-field prologue
//   343 × 6-and-2 nibble  256 data bytes + final XOR checksum
//   $DE $AA $EB           data-field epilogue
//
// 6-and-2 packing (per "Beneath Apple DOS"): each input byte is split into
// the top 6 bits (256 "high" entries) and bottom 2 bits. The 86 "low" entries
// hold three bit-pairs each, packed with the pair-bits swapped for clean
// running-XOR checksum behaviour. On disk the 86 low nibbles are written in
// REVERSE order, then the 256 high nibbles in normal order, then one final
// checksum nibble. All output is run through a 64-entry GCR translate table
// that maps 6-bit values to valid disk bytes (always bit-7 set, no run of
// two zero bits — the constraints the Disk II's data-separator can recover).
//
// Read-only for now: the buffer is regenerated only on insert.

#ifndef POM2_DISK_IMAGE_H
#define POM2_DISK_IMAGE_H

#include "MediaWritePolicy.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

/// NOTE — sizeof(DiskImage) is ~242 KB: the track buffers live IN the object
/// (`tracks`, below), which is what keeps the LSS hot path indirection-free.
/// Never declare one on the stack of a secondary thread — macOS gives a
/// std::thread 512 KB total, and the insert path already stacks one temporary
/// per frame (insertDisk → prepareDisk → loadFile), which is how the AI
/// control server's HTTP thread SIGBUSed on arm64 macOS (2026-08-23).
/// Heap-allocate temporaries (`std::make_unique<DiskImage>()`) instead.
class DiskImage
{
public:
    static constexpr int kTracks            = 35;
    static constexpr int kSectorsPerTrack   = 16;
    static constexpr int kSectorBytes       = 256;
    static constexpr int kBytesPerImage     = kTracks * kSectorsPerTrack * kSectorBytes; // 143360
    static constexpr int kNibblesPerTrack   = 6656;

    // Pre-DOS-3.3 (DOS 3.1/3.2/3.2.1) 13-sector, 5-and-3 GCR. A whole
    // .d13 / .dsk image is 35 × 13 × 256 = 116480 bytes. The 13-sector
    // track nibblizes shorter than 16-sector, so it fits in the same
    // kNibblesPerTrack buffer.
    static constexpr int kSectorsPerTrack13 = 13;
    static constexpr int kBytesPerImage13   = kTracks * kSectorsPerTrack13 * kSectorBytes; // 116480

    /// Quarter-track positions per WOZ TMAP (4 per whole track ×
    /// kTracks). The LSS-side bit-cell / flux storage indexes on this
    /// granularity so .woz disks can expose distinct streams per
    /// quarter-track (Spiradisc / RWTS18 / Locksmith Fast Copy rely on
    /// this). Non-WOZ formats only carry whole-track data; quarter-track
    /// accesses on those alias down to the containing whole track.
    static constexpr int kQuarterTracks     = 160;

    /// File-format sector ordering. Both produce the same on-disk physical
    /// sector layout — only the skew between file offset and physical
    /// sector P differs.
    enum class SectorOrder { Dos33, ProDOS };

    /// What the content-driven format detector decides a buffer is. The
    /// detector inspects the raw bytes (size, magic numbers, embedded
    /// header positions) and produces an `ImageKind` plus the offsets
    /// inside the buffer at which the payload starts and ends. Each kind
    /// has a matching loader path inside loadFile().
    enum class ImageKind {
        Unknown,       // detection failed; loadFile refuses with lastError
        Woz,           // .woz bit-cell native
        Dsk143k,       // 143 360-byte image, DOS 3.3 sector order
        ProDos143k,    // 143 360-byte image, ProDOS sector order
        Nib232k,       // 232 960-byte raw nibble stream (35 × 6656)
        CNib2,         // 223 440-byte raw nibble stream (35 × 6384)
        TwoImgDos,     // .2mg / 2IMG header wrapping DOS-skew payload
        TwoImgProDos,  // .2mg / 2IMG header wrapping ProDOS-skew payload
        TwoImgNib,     // .2mg / 2IMG header wrapping raw nibbles
        Dos32_13,      // 116 480-byte image, DOS 3.x 13-sector (5-and-3 GCR)
    };

    DiskImage();

    /// Load a .dsk / .do image (DOS 3.3 logical sector order), a .po
    /// image (ProDOS sector order), a .nib raw nibble stream (35 × 6656
    /// bytes, no encoding/decoding), or a .woz bit-cell image
    /// (Apple //e copy-protected disks; verbatim port of MAME's
    /// `src/lib/formats/woz_dsk.cpp` chunk parser). When `order` is
    /// omitted, falls back to extension sniffing: `.po` → ProDOS,
    /// `.nib` → raw, `.woz` → WOZ1/WOZ2, anything else → DOS 3.3.
    /// Returns true on success. On failure `getLastError()` has details
    /// and the previously loaded image is left intact (the load goes
    /// into a scratch object and commits only on success).
    bool loadFile(const std::string& path);
    bool loadFile(const std::string& path, SectorOrder order);
    SectorOrder getSectorOrder() const { return sectorOrder; }
    /// 13-sector (pre-DOS-3.3) when true. Lets DiskIICard serve the
    /// 341-0009 boot PROM + 341-0010 LSS instead of the 16-sector pair.
    bool isNib() const { return nibFormat; }
    int  sectorsPerTrack() const { return sectorsPerTrack_; }
    bool is13Sector() const { return sectorsPerTrack_ == kSectorsPerTrack13; }
    /// True iff the image was loaded from a .woz file. WOZ images are
    /// bit-cell-native: the legacy 32-cycle nibble gate cannot decode
    /// them; DiskIICard forces the LSS path on insert.
    ///
    /// Write-back IS implemented (saveDirty splices the dirty quarter-
    /// tracks back into wozRaw and zeroes the header CRC32 per the
    /// Applesauce 2.1 "not computed by the imager" sentinel), and WOZ
    /// sits under the same writeBackEnabled / fileWriteProtected gate as
    /// .dsk/.nib. The one exception is a WOZ carrying **FLUX** tracks:
    /// POM2 cannot serialise their delta streams, so such a medium is
    /// forced physically write-protected at load rather than accepting
    /// writes and dropping them at save time.
    bool isWoz() const { return wozFormat; }

    /// Discard the loaded image. After eject, isLoaded() returns false
    /// and the controller will see the same "no media" behaviour as if
    /// the image had never been loaded.
    void eject();

    bool        isLoaded()       const { return loaded; }
    const std::string& getPath() const { return path; }
    const std::string& getLastError() const { return lastError; }

    /// Read one nibble from `track` at buffer index `index`. `index` is
    /// wrapped modulo kNibblesPerTrack so the controller can advance the
    /// head cursor without bothering to wrap. Returns $FF (= sync nibble)
    /// when no image is loaded or the track is out of range.
    uint8_t nibbleAt(int track, int index) const;

    /// Write one nibble at `track[index]`. Marks the track dirty so a
    /// subsequent saveDirty() will persist it back to the source file.
    /// No-op when no image is loaded or the track is out of range.
    void writeNibbleAt(int track, int index, uint8_t value);

    // ── Media snapshot (rewind) ─────────────────────────────────────────
    // The full nibble track buffers + dirty flags — what writeNibbleAt
    // mutates and reads derive from. Captured (by DiskIICard) only for
    // loaded, NON-write-protected, NON-WOZ images, so disk writes are undone
    // on a rewind. WOZ keeps its authoritative bit-stream in `wozRaw`, a
    // different store, so it is excluded (write-protected originals don't
    // need it anyway). The rewind delta codec keeps the cost near-zero until
    // a track is actually written.
    static constexpr std::size_t kMediaSnapshotBytes =
        static_cast<std::size_t>(kTracks) * kNibblesPerTrack + kTracks;
    void appendMediaSnapshot(std::vector<uint8_t>& out) const;
    void loadMediaSnapshot(const uint8_t* data, std::size_t len);

    // ── LSS bit-cell stream ─────────────────────────────────────────────
    //
    // The Disk II's Logic State Sequencer reads bit cells, not nibbles —
    // each cell is 4 µs on real hardware. Sync $FF bytes carry two
    // trailing zero cells (10 cells total) so the byte boundary slips by
    // 2 bits per sync gap; this is what lets the LSS *lose* alignment
    // during a sync run and *re-sync* on the next data prologue's leading
    // 1-bit. Without this padding, a soft-LSS that always emits 8 cells
    // per byte stays aligned forever — DOS / ProDOS RWTS still works
    // (they probe the latch one byte at a time) but Copy II Plus's RWTS
    // and any copy-protection that bit-bangs $C0EC fails.
    //
    // Standard .dsk/.do/.po expansion (per MAME `ap2_dsk.cpp`):
    //   • each $FF in a run of 2+ contiguous $FFs → 10 cells
    //   • lone $FF                                 → 8 cells
    //   • everything else                          → 8 cells
    // .nib raw nibble images have no sync semantics — every byte = 8 cells.

    /// Total bit-cell count for quarter-track `qt` (0..159). For non-WOZ
    /// formats, all four quarter-tracks of a whole-track alias to the
    /// same stream (matching the "snap to nearest whole track" behaviour
    /// real Disk II hardware approximates when reading a standard .dsk).
    /// For WOZ, each quarter-track has its own bit stream sourced from
    /// the matching TMAP entry. Standard 16-sector .dsk/.do/.po:
    /// ~54944 cells. .nib raw images: exactly 53248 cells (= 6656 × 8).
    int trackBitLength(int qt) const;

    /// Read one bit cell (0 or 1) from quarter-track `qt` (0..159) at
    /// `bitIdx`. `bitIdx` wraps modulo trackBitLength(qt). First call
    /// per (image, qt) lazily expands the bit-cell cache; subsequent
    /// reads are O(1) array index. `writeNibbleAt` invalidates the
    /// containing whole-track's cache.
    uint8_t bitAt(int qt, int bitIdx) const;

    // ── MAME-style flux event view ──────────────────────────────────────
    //
    // Verbatim port of MAME's `floppy_image_device` flux model. A track is
    // a sorted list of flux transition timestamps. Each transition is one
    // physical flux orientation flip on the magnetic surface; the LSS's
    // PULSE input goes high for one LSS cycle when the head crosses one.
    //
    // Time unit: 1 LSS cycle = 0.5 µs (2 per CPU cycle). Per revolution =
    // `trackPeriod(track)` LSS cycles. For a stock 16-sector .dsk that's
    // ~440k LSS cycles (≈ 215 ms ≈ 280 RPM — slightly slow vs MAME's
    // 200 ms / 300 RPM but consistent across reads/writes within POM2).
    //
    // The flux array is derived lazily from the nibble buffer: each "1"
    // bit cell becomes one flux event at the cell center. Per-cell layout
    // matches the bit-cell stream above (8 cells/byte, 10 cells per $FF
    // in a sync run), so reads through the flux model and reads through
    // the bit-cell view produce identical PULSE timing for the same
    // nibble buffer. Cache invalidates on `writeNibbleAt` and `eject`.

    /// Per-quarter-track period in LSS cycles. Equal to
    /// `trackBitLength(qt) * 8`.
    int trackPeriod(int qt) const;

    /// Sorted vector of flux event timestamps (LSS cycles, in [0, period)).
    /// Returns a static empty vector for out-of-range quarter-tracks.
    /// Lazy-built on first call; reused across calls until the underlying
    /// nibble or WOZ bit buffer changes.
    const std::vector<int>& fluxEvents(int qt) const;

    /// MAME `floppy_image_device::get_next_transition(from_when)` —
    /// returns the LSS-cycle timestamp of the next flux transition on
    /// quarter-track `qt` at or after `fromLssCycle`. Wraps across
    /// revolution boundaries: if the current revolution has no further
    /// events past `fromLssCycle`, the returned time is in the next
    /// revolution (so result ≥ fromLssCycle always). Returns
    /// `kFluxNever` only if the track has zero events (= unformatted
    /// quarter-track, which MAME reports as `attotime::never`).
    ///
    /// `revolutionStart` anchors the disk's angular position. The cell
    /// looked up is `(fromLssCycle - revolutionStart) mod track_period`
    /// — matching MAME `floppy_image_device::find_position`, which
    /// computes `(when - m_revolution_start_time)` modulo `m_rev_time`.
    /// Pass `< 0` to fall back to the pre-MAME-anchor behaviour
    /// (`fromLssCycle mod track_period`); this is used during boot
    /// before any motor-on transition has happened, and by the
    /// stand-alone bit-stream decoder smoke tests.
    static constexpr int64_t kFluxNever = INT64_MAX;
    int64_t getNextTransition(int qt, int64_t fromLssCycle,
                              int64_t revolutionStart = -1) const;

    /// MAME `floppy_image_device::write_flux(start, end, count, transitions)`
    /// — splice a window of flux events into quarter-track `qt`. For
    /// non-WOZ formats this re-derives the nibble buffer at the whole
    /// track containing `qt` (cell-windowed flux→bit conversion); for
    /// WOZ formats it splices straight into `bitStream[qt]`.
    ///
    /// `revolutionStart` is the SAME angular anchor `getNextTransition`
    /// takes: MAME's write_flux maps start / end / every transition
    /// through `find_position(base, when)`, which is anchored on
    /// `m_revolution_start_time` (MAME `imagedev/floppy.cpp`
    /// write_flux ~:1050-1095 + find_position ~:1100-1125). Pass the
    /// drive's revolution anchor here so the spliced window lands at
    /// the angular position the read path would report for the same
    /// LSS cycle; `< 0` keeps the legacy unanchored reduction
    /// (`startLssCycle mod track_period`), matching getNextTransition's
    /// `revolutionStart < 0` fallback.
    void writeFlux(int qt, int64_t startLssCycle, int64_t endLssCycle,
                   int count, const int64_t* transitions,
                   int64_t revolutionStart = -1);

    /// MAME `floppy_image_device::set_write_splice(when)` — informational
    /// hook for the upcoming IWM port; currently a no-op for the Disk II
    /// path because the splice point is implicit in `writeFlux`.
    void setWriteSplice(int /*qt*/, int64_t /*lssCycle*/) {}

    /// True if any track has been written since load. Cleared by
    /// saveDirty() and load.
    bool hasUnsavedChanges() const { return anyDirty; }

    /// Decode each dirty track back to the source-file format (.dsk/
    /// .do/.po: 16 logical sectors × 256 bytes per track; .nib: raw
    /// nibble buffer) and overwrite the source file. Returns true on
    /// success. On failure `getLastError()` has details. After save
    /// the dirty bits are cleared.
    bool saveDirty();

    /// User opt-in for write-back. Default: false (read-only) to avoid
    /// silently mutating the source file. Mainwindow flips this before
    /// eject if the user has opted in. WOZ images now go through the
    /// same gate — once `writeBackEnabled` is on and the WOZ INFO byte
    /// doesn't mark the source as physically write-protected, writes
    /// splice into `bitStream[qt]` and serialise back through
    /// saveDirty() (zeroing CRC32 per Applesauce WOZ spec).
    bool isWriteProtected() const {
        return fileWriteProtected || !writeBackEnabled;
    }
    /// PHYSICAL write-protect of the medium (WOZ INFO+2 / 2IMG WP flag),
    /// independent of the write-back toggle. On real hardware this signal
    /// inhibits the write current, so a WP disk can never be mutated — the
    /// write functions and saveDirty() honour it regardless of writeBackEnabled.
    bool isFileWriteProtected() const { return fileWriteProtected; }
    void setWriteBackEnabled(bool on) { writeBackEnabled = on; }

private:
    bool loadFileUnchecked(const std::string& path);
    bool loadFileUnchecked(const std::string& path, SectorOrder order);

    /// Sync-gap rule shared by expandTrackBits (cell-timeline expansion)
    /// and writeFlux (re-pack): a $FF inside a run of ≥ kSyncMinRun is a
    /// 10-cell self-sync nibble, everything else is 8 cells. ONE
    /// definition — the splice drifts silently if expander and writer
    /// ever disagree on the padded timeline.
    static constexpr int kSyncMinRun = 5;

    /// Fill `widths[kNibblesPerTrack]` with the per-nibble cell widths of
    /// `track`'s nibble buffer (8 or 10 per the kSyncMinRun rule above).
    /// Linear circular run-length scan — writeFlux runs this on every
    /// ~30-transition LSS flush under stateMutex, so it must stay O(N)
    /// and allocation-free (the old per-nibble ±4 neighbour probe was
    /// O(9N) plus a heap vector per flush).
    void computeCellWidths(int track, uint8_t* widths) const;

    bool loaded = false;
    SectorOrder sectorOrder = SectorOrder::Dos33;
    int         sectorsPerTrack_ = kSectorsPerTrack;   // 16, or 13 for DOS 3.x
    bool nibFormat = false;
    /// True iff the source was the rarer 6384-byte/track NIB variant
    /// (`CNib2` in AppleWin's terminology). The on-disk format omits the
    /// last 272 sync nibbles per track; the runtime pads each track up
    /// to the standard 6656 with $FF so the rest of the controller and
    /// LSS pipeline stays oblivious. `saveDirty()` truncates back to
    /// 6384/track on write, so a round-trip preserves the source layout.
    bool cnib2Format = false;
    /// True iff the source carried a 2IMG / .2mg wrapper. The 64-byte
    /// (or longer, if comment/creator chunks follow) header is captured
    /// verbatim into `twoImgHeaderRaw` at load time; anything past the
    /// payload goes into `twoImgTrailerRaw`. Write-back re-emits both
    /// envelopes around the freshly composed payload so the file remains
    /// a valid 2IMG after the round-trip.
    bool twoImgFormat = false;
    std::vector<uint8_t> twoImgHeaderRaw;
    std::vector<uint8_t> twoImgTrailerRaw;
    /// Set when loadFile parses a .woz successfully. WOZ stores bit cells
    /// directly so loadWoz populates `bitStream[track]` instead of the
    /// nibble buffer; the existing `expandTrackFlux` derives flux events
    /// from the bit stream as usual. Persisted writes (writeFlux) are
    /// suppressed for WOZ — the splice would clobber the canonical bit
    /// data. Cleared on eject() / non-WOZ load.
    bool wozFormat = false;
    /// Mirror of WOZ INFO byte +2 (`write_protected`). Folded into
    /// `isWriteProtected()` so the per-file flag survives once the WOZ
    /// blanket gate is lifted. Stays false for non-WOZ formats.
    bool fileWriteProtected = false;
    /// WOZ INFO write-protect, also forced for images containing FLUX tracks
    /// until their delta-stream write encoder exists.
    /// WOZ INFO byte +39 — `optimal_bit_timing`. Measured in 125 ns units.
    /// 32 = 4 µs (the standard 5.25" Apple II cell duration; default for
    /// any WOZ1 image, since INFO version 1 didn't carry the field, and
    /// WOZ2 too unless the imager set otherwise). The flux-event view
    /// scales `i * lssCyclesPerCell + lssCyclesPerCell/2` by this so a
    /// disk mastered at e.g. 28 (3.5 µs) or 40 (5 µs) plays back at the
    /// rate Applesauce captured. LSS clock is 2 MHz → 1 LSS cycle =
    /// 500 ns → `lssCyclesPerCell = optimalBitTiming / 4` (integer when
    /// the imager picked a multiple of 4, which is the universal case
    /// in practice). 0 means "field not present, assume default 32".
    /// MAME `as_dsk.cpp:283` reads the same byte.
    uint8_t optimalBitTiming = 32;
    std::string path;
    std::string lastError;
    bool writeBackEnabled = pom2::mediaWritableByDefault();   // see MediaWritePolicy.h
    bool anyDirty = false;

    // 35 × 6656 = ~228 KB. Heap allocation would also work but a flat
    // member fits the "one concern, plain data" style of POM2.
    using TrackBuffer = std::array<uint8_t, kNibblesPerTrack>;
    std::array<TrackBuffer, kTracks> tracks;
    std::array<bool, kTracks>        dirty{};

    // ── Non-WOZ write framing (see writeFlux) ───────────────────────────
    // A nibble store has no notion of angular length, so the flux the head
    // lays down has to be FRAMED back into nibbles the way the read LSS
    // frames it: skip 0-cells until a 1, then that 1 plus the next seven
    // cells are one nibble. The framing is continuous across the ~30-
    // transition chunks DiskIICard flushes in — a nibble routinely straddles
    // two flushes — so the shift accumulator and the destination slot live
    // here, per track, and are re-anchored only when a window arrives that
    // isn't contiguous with the previous one (a new write burst).
    struct WriteFraming {
        int64_t nextCycle = INT64_MIN;   // end of the last spliced window
        int     nibbleIdx = -1;          // buffer slot the next nibble lands in
        uint8_t acc       = 0;           // shift accumulator
        int     accBits   = 0;           // 0 = waiting for the leading 1 bit
        int64_t origin    = 0;           // LSS cycle the burst's cell grid starts at
        int     nextCell  = -1;          // first cell this burst hasn't framed
        // A window may end mid-cell, so that cell's remaining span (and any
        // transition in it) belongs to the NEXT flush. Hold its bit back
        // rather than framing half a cell twice.
        bool    heldValid = false;
        bool    heldBit   = false;
        int     heldCell  = -1;
    };
    std::array<WriteFraming, kTracks> writeFraming{};

    // Lazy per-quarter-track bit-cell expansion cache. Populated on
    // first `bitAt`/`trackBitLength` call; invalidated by `writeNibbleAt`
    // / `eject` / new `loadFile`. For non-WOZ formats only the slots at
    // `qt % 4 == 0` are populated (mirroring whole-track data); WOZ may
    // populate any slot where TMAP[qt] != 0xFF. Mutable so that const
    // `bitAt` can fill the cache on demand.
    mutable std::array<std::vector<uint8_t>, kQuarterTracks> bitStream;
    mutable std::array<bool, kQuarterTracks>                 bitStreamValid{};

    // ── WOZ write-back state ────────────────────────────────────────────
    // The entire WOZ file is snapshotted into `wozRaw` at load time so
    // saveDirty() can splice modified bit-cell streams back into their
    // original chunk slots. Each populated quarter-track gets a stable
    // (byteOff, byteLen, bitCount) triple pointing at the matching TRKS
    // sub-chunk; writeFlux only updates `bitStream[qt]`, and saveDirty
    // walks the dirty bits to re-pack into `wozRaw` before writing.
    //
    // Per the Applesauce WOZ 2.1 spec, we zero the CRC32 header bytes on
    // save (the spec explicitly allows CRC=0 as "not computed by the
    // imager"); recomputing would also be valid but adds little safety
    // because we always re-validate on the next load anyway.
    std::vector<uint8_t> wozRaw;
    std::array<size_t, kQuarterTracks> wozQtByteOff  {};
    std::array<size_t, kQuarterTracks> wozQtByteLen  {};
    std::array<size_t, kQuarterTracks> wozQtBitCount {};
    std::array<bool,   kQuarterTracks> wozQtDirty    {};
    void expandTrackBits(int qt) const;

    // Lazy per-quarter-track flux event cache (sorted ascending, in
    // [0, period)). Mirrors MAME's m_image flux-event vector but stored
    // in LSS cycles rather than 200M-position units.
    mutable std::array<std::vector<int>, kQuarterTracks> fluxStream;
    mutable std::array<bool, kQuarterTracks>             fluxStreamValid{};
    /// True revolution period (LSS cycles) for quarter-tracks populated
    /// from a WOZ FLUX delta stream; 0 everywhere else. A flux capture's
    /// period is the SUM of its tick deltas (MAME `as_dsk.cpp:61-81`
    /// accumulates the same stream into the track's total time) — it is
    /// NOT `bit_count × optimal_bit_timing`, so `trackPeriod` must
    /// return this instead of the synthetic-bitstream-derived product
    /// or every revolution wraps short/long and the flux timeline
    /// desynchronises a little more per revolution.
    mutable std::array<int, kQuarterTracks>              fluxQtPeriodLss{};
    void expandTrackFlux(int qt) const;

    // ── Search cursor for getNextTransition (performance only) ───────────
    // The binary search inside getNextTransition was ~20 % of a disk-active
    // profile (callgrind on `pom2_bench --disk … --frames 1200`): a track
    // holds tens of thousands of flux events, so every call paid ~16 probes
    // — while the LSS actually walks the revolution STRICTLY SEQUENTIALLY
    // and lands on the same index, or the very next one, nearly every time.
    //
    // These two members remember the previous answer. They are a HINT, never
    // an authority: the fast path re-verifies that the remembered index still
    // *is* the lower bound (two comparisons) before using it, so a stale hint
    // — a write splice that rebuilt the flux array, a track step, an eject,
    // a snapshot restore — simply fails verification and falls back to the
    // full search. Nothing has to invalidate them, and that is the point: an
    // invalidation you can forget to place at one of its sites is a
    // correctness bug waiting to happen, a self-verifying hint cannot be.
    //
    // `mutable` because getNextTransition is const and this changes nothing
    // observable. Not thread-safe and doesn't need to be: the flux read path
    // runs on the CPU worker thread only (DiskIICard::lssSync,
    // IWMDevice::sync), both under `stateMutex`.
    mutable int    lbHintQt_  = -1;
    mutable size_t lbHintIdx_ = 0;
    /// LSS cycles per bit cell, derived from `optimalBitTiming`. Default
    /// 8 (= 4 µs / 0.5 µs per LSS cycle). WOZ2 honours INFO+39; WOZ1 and
    /// non-WOZ formats keep the default. Used by `expandTrackFlux` and
    /// `trackPeriod`.
    int lssCyclesPerCell() const;

    /// Aliasing function: maps an externally-supplied quarter-track index
    /// to the actual storage slot. Non-WOZ formats only carry whole-track
    /// data, so qt 0..3 alias to slot 0, qt 4..7 alias to slot 4, etc.
    /// WOZ formats keep distinct streams per quarter-track and pass the
    /// index through unchanged.
    int qtSlot(int qt) const {
        return wozFormat ? qt : (qt & ~3);
    }

    void invalidateBitStream(int slot) {
        bitStreamValid[slot]  = false;
        fluxStreamValid[slot] = false;
        bitStream[slot].clear();
        fluxStream[slot].clear();
    }
    void invalidateWholeTrack(int wholeTrack) {
        // For non-WOZ writes (writeNibbleAt / writeFlux), only the slot
        // at qt = wholeTrack*4 has cached data; the others alias to it.
        invalidateBitStream(wholeTrack * 4);
    }
    void invalidateAllBitStreams() {
        bitStreamValid.fill(false);
        fluxStreamValid.fill(false);
        for (auto& bs : bitStream)  bs.clear();
        for (auto& fs : fluxStream) fs.clear();
        fluxQtPeriodLss.fill(0);
    }

    void nibblizeTrack(int track, const uint8_t* sectors, uint8_t volume,
                       const int* logicalForPhysical);
    static void writeAddressField(uint8_t*& dst, uint8_t volume,
                                  uint8_t track, uint8_t sector);
    static void writeDataField   (uint8_t*& dst, const uint8_t* src);

    // 13-sector (5-and-3) counterparts — verbatim port of MAME
    // `formats/ap2_dsk.cpp` a2_13sect_format. Address prologue D5 AA B5
    // (vs D5 AA 96), data field 5-and-3 (411 nibbles vs 6-and-2 343).
    void nibblizeTrack13(int track, const uint8_t* sectors, uint8_t volume);
    static void writeAddressField13(uint8_t*& dst, uint8_t volume,
                                    uint8_t track, uint8_t sector);
    static void writeDataField13   (uint8_t*& dst, const uint8_t* src);

    /// Result of the content-driven format detector. `payloadOff` and
    /// `payloadLen` describe the slice of the input buffer that the
    /// matching per-format loader should consume — e.g. for a 2MG-wrapped
    /// image these skip the 64-byte header. For plain `.dsk`/`.po`/`.nib`
    /// the payload is the whole file. `order` and `volume` carry the
    /// decoded sector skew + volume number when applicable.
    struct DetectResult {
        ImageKind   kind          = ImageKind::Unknown;
        std::size_t payloadOff    = 0;
        std::size_t payloadLen    = 0;
        SectorOrder order         = SectorOrder::Dos33;
        uint8_t     volumeNumber  = 254;
        bool        writeProtect  = false;  // 2IMG flags bit 31 (locked)
        // Set when the source is wrapped in a 2IMG / .2mg envelope.
        // `twoImgHeaderEnd` is the offset of the first payload byte —
        // every byte before it is part of the header (which includes
        // the 64-byte fixed prefix plus any inline comment/creator
        // chunks); `twoImgTrailerStart` is the offset just past the
        // payload, where trailing comment/creator chunks (if any) live.
        // The loader copies [0, twoImgHeaderEnd) and
        // [twoImgTrailerStart, file end) into `twoImgHeaderRaw` and
        // `twoImgTrailerRaw` so a future write-back can re-emit them
        // byte-for-byte around the modified payload.
        bool        twoImgWrap         = false;
        std::size_t twoImgHeaderEnd    = 0;
        std::size_t twoImgTrailerStart = 0;
        std::string diag;   // human-readable trace, logged on success
        std::string error;  // populated when kind == Unknown
    };

    /// Inspect the raw bytes of an image file and decide what it is.
    /// Pure — no I/O, no member mutation. The file path is only used as
    /// a hint (extension fallback when the content is ambiguous, e.g.
    /// distinguishing a 143 360-byte ProDOS-skewed image from a
    /// DOS-skewed one when the volume directory sniff returns nothing).
    static DetectResult detectFormat(const std::string& path,
                                     const std::vector<uint8_t>& bytes);

    /// Per-format buffer loaders. Each populates `tracks[]` (and any
    /// format-specific state) from the supplied byte slice, sets the
    /// appropriate `*Format` flags, and invalidates the bit-cell cache.
    /// Return false (with `lastError` filled) on internal failure.
    ///
    /// `nibblesPerTrack` controls how many source bytes are read per
    /// track. The standard `.nib` format uses 6656 (= the runtime
    /// storage width). The rarer CNib2 variant uses 6384; the loader
    /// pads each track up to 6656 with $FF (sync gap) so the rest of
    /// the controller is format-agnostic.
    bool loadNibFromBuffer(const uint8_t* data, std::size_t len,
                           int nibblesPerTrack,
                           const std::string& imgPath);
    bool loadSectorImageFromBuffer(const uint8_t* data, std::size_t len,
                                   SectorOrder order, uint8_t volume,
                                   const std::string& imgPath);

    /// WOZ1/WOZ2 parser. Follower of MAME's
    /// `src/lib/formats/as_dsk.cpp` `woz_format` chunk walk: looks for
    /// INFO, TMAP, TRKS in any order; accepts both WOZ1 (160 fixed
    /// 6656-byte slots, bit_count at offset +6648 LE u16) and WOZ2
    /// (160 × 8-byte TRK headers; data at file offset
    /// starting_block × 512 with bit_count as u32). Bits are MSB-first
    /// within each byte. Each quarter-track 0..159 sources its bit
    /// stream from its own TMAP entry, so disks that bury protection
    /// data on quarter- or half-tracks (Spiradisc, RWTS18, Locksmith
    /// Fast Copy) decode correctly.
    bool loadWoz(const std::string& imgPath);

    /// Decode one track's nibble buffer back into 16 × 256-byte logical
    /// sectors. Returns true if any sector was successfully decoded.
    /// Sectors that can't be parsed (no prologue, bad checksum) are
    /// left untouched in `outSectors` (which the caller pre-fills with
    /// the existing file content so unmodified sectors persist).
    bool decodeTrack(int track, uint8_t outSectors[kSectorsPerTrack][kSectorBytes]) const;

    /// 13-sector (5-and-3) decoder, inverse of writeDataField13. Used by
    /// saveDirty for .d13/.dsk write-back. outSectors indexed by the
    /// address-field sector number (0..12).
    bool decodeTrack13(int track, uint8_t outSectors[kSectorsPerTrack13][kSectorBytes]) const;
};

/// Slot-routing class for a disk image — which kind of card/slot the image
/// belongs in. Coarser than `DiskImage::ImageKind`: it only answers "5.25"
/// Disk II vs 800K Sony 3.5" vs ProDOS hard-disk (HDV)", which is what the
/// 1-click "insert + boot" path (Disk Library UI and the `--kiosk` / CLI
/// positional disk launcher) needs to pick a target slot.
enum class DiskSlotClass {
    Unknown,    // extension/size not recognised — caller reports an error
    Floppy525,  // 5.25" Disk II: .dsk/.do/.nib/.woz/.d13, or .po @143360
    Sony35,     // 800K 3.5": .po/.2mg sized ~819200 (±2IMG header slack)
    Hdv,        // ProDOS hard disk: .hdv/.2mg > 800K, 512-aligned
};

/// Classify `path` for slot routing by file extension + size. Mirrors the
/// per-tab predicates the Disk Library scanner uses (accept525 / accept35 /
/// acceptHdv in DiskLibrary_ImGui.cpp) so the UI and the CLI launcher route
/// identically. Returns `Unknown` if the file is missing or unrecognised.
DiskSlotClass classifyDiskForSlot(const std::string& path);

#endif // POM2_DISK_IMAGE_H
