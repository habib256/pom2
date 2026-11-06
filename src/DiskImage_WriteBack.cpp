// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// DiskImage — phase 2 of the write-back: the FILE work, run with no lock held
// and without the image (`DiskImage::commitWriteBack` is static; the Disk II
// eject has moved the image out by then, the background autosave keeps it
// mounted). Phase 1, the capture under `stateMutex`, is
// `DiskImage::takeWriteBack` in DiskImage.cpp.
//
// Three rules live here, and each one exists because the opposite lost a
// user's data with a success message.
//
//   1. The image file is replaced only while it is still the file this
//      process knows. Changed from outside → the guest's version goes to a
//      sibling and the file is left alone (MediaAutosave.h, "External
//      change").
//
//   2. A sector format takes a dirty track only when EVERY sector of it
//      decodes. A track of which fifteen sectors decode and one does not used
//      to be "saved": the fifteen were written, the sixteenth kept the bytes
//      the file already had, the dirty flag was cleared and the log said
//      "Saved 1 modified track(s)". The guest went on reading its own sector
//      from memory until the next mount, when it silently became the old one.
//      .dsk/.do/.po hold decoded sectors and nothing else: a track that is
//      not sixteen (or thirteen) good sectors cannot be stored in them, and
//      saying so is the only honest answer.
//      [D] Casso NibblizationLayer.cpp:1296-1314 @ 2d899adf — refuses a
//          partly decoded track; that is where the rule was taken from.
//
//   3. A refusal does not leave the guest's work in RAM only. The complete
//      surface is written beside the image as `<image>.recovered.woz` — a
//      bit-cell format holds any nibble stream, so nothing is lost if the
//      process dies before the guest repairs the track — and the error says
//      where. The commit still FAILS: the dirty flags stay set, the retry
//      runs, and the day the track decodes again the image itself is written.
//      [D] Casso DiskImageStore.cpp:923-938 @ 2d899adf — same fallback.

#include "DiskImage.h"
#include "AtomicFileReplace.h"
#include "Logger.h"
#include "PersistentFs.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Atomic-ish file replace for the write-back paths. Every saveDirty
// branch used to open the USER'S SOURCE FILE with std::ios::trunc and
// write in place: an ENOSPC / removable-media / I/O failure mid-write
// left the original truncated — and since the dirty flags survive a
// failure, the retry then pre-filled its buffer from the now-truncated
// file and wrote zeros over the remaining tracks. Write a sibling temp
// file instead (same directory → same filesystem → POSIX rename is
// atomic) and only replace the original once the write fully succeeded.
template <typename Emit>
bool writeFileAtomic(const std::string& path, std::string& lastError,
                     Emit&& emit)
{
    // Unique per process AND per call. A fixed `<image>.pom2tmp` is the name
    // EVERY POM2 instance derives, so two of them saving the same image (a
    // second window, a headless run) opened one temp file with trunc,
    // interleaved their writes, and whichever renamed last published a disk
    // made of both. See pom2::tempSiblingPath.
    const std::string tmp = pom2::tempSiblingPath(path).string();
    // The temp name is derived from a target the caller validated, but gets
    // none of that scrutiny itself, and `ofstream(trunc)` follows symlinks.
    // Anything already sitting at <image>.pom2tmp is our own crash debris or
    // somebody else's redirection — see `prepareTempPath`, and
    // `Disk35Image::saveDirty`, which has always done this.
    std::error_code prepEc;
    if (!pom2::prepareTempPath(tmp, prepEc)) {
        lastError = "Temp path unusable " + tmp + ": " + prepEc.message();
        return false;
    }
    {
        std::ofstream wf(tmp, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!wf) { lastError = "Cannot open " + tmp + " for write"; return false; }
        emit(wf);
        wf.flush();
        wf.close();
        if (!wf) {
            lastError = "Short write on " + tmp;
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    // The rename replaces the inode, so without the helper the image would
    // inherit the temp file's umask-derived mode. It reads the image's mode
    // AT the rename, under the notch's lock (MediaNotch.h).
    std::error_code ec;
    if (!pom2::replaceMediaFileAtomic(tmp, path, ec)) {
        lastError = "Cannot replace " + path + ": " + ec.message();
        std::error_code ec2;
        std::filesystem::remove(tmp, ec2);
        return false;
    }
    // Browser build: mark IDBFS dirty for images under /persistent; the
    // frame loop flushes that mount to IndexedDB. Uploaded /uploads images
    // are MEMFS only: retain them through a session or Automatic resume.
    // No-op natively — the rename + fsync above is already durable.
    pom2::markPersistentStateDirty();
    return true;
}

std::string trackList(const std::vector<int>& tracks)
{
    std::string list;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (i) list += ", ";
        list += std::to_string(tracks[i]);
    }
    return list;
}

/// A dirty track whose nibble stream does not decode into a full set of
/// sectors cannot be written back to a SECTOR format at all — see rule 2 in
/// the file comment. Failing keeps the dirty flags set, so nothing is thrown
/// away; `recovered` names the lossless copy rule 3 wrote (empty when that
/// write failed too, with `recoveryError` saying why).
bool reportUndecodable(const std::vector<int>& tracks, const std::string& recovered,
                       const std::string& recoveryError, std::string& lastError)
{
    lastError = "track " + trackList(tracks) + " was modified but its nibble "
                "stream no longer decodes into a full set of sectors, so it "
                "cannot be written back to this format. Nothing was saved to "
                "the image and the changes are kept in memory; retry, or turn "
                "write-back off to eject without saving.";
    if (!recovered.empty())
        lastError += " The complete disk as the guest sees it was saved to " +
                     recovered + ", which mounts as it is.";
    else if (!recoveryError.empty())
        lastError += " A recovery copy could not be written either: " +
                     recoveryError;
    pom2::log().warn("Disk II", "Save refused — " + lastError);
    return false;
}

bool isUnformatted(const DiskImage::TrackBuffer& t)
{
    return std::all_of(t.begin(), t.end(), [](uint8_t b) { return b == 0; });
}

}  // namespace

// ── CRC-32 as the WOZ header carries it ─────────────────────────────────────
// [B] WOZ 2.1 reference, "CRC" appendix — the Gary S. Brown table CRC-32:
//     reflected polynomial 0xEDB88320, initial value and final XOR 0xFFFFFFFF.
uint32_t DiskImage::wozCrc32(const uint8_t* data, std::size_t n)
{
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int b = 0; b < 8; ++b)
                c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i)
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

// ── The surface as a WOZ 2 file ─────────────────────────────────────────────
// [B] WOZ 2.1 reference (applesaucefdc.com/woz/reference2/) — the layout
//     written here: 12-byte header, INFO (60 bytes) at 20, TMAP (160) at 88,
//     TRKS at 256 with 160 eight-byte TRK records, bit data from block 3 in
//     512-byte blocks, bits MSB first.
std::vector<uint8_t> DiskImage::buildWozFromSurface(
    const std::vector<TrackBuffer>& surface, bool thirteenSector)
{
    constexpr std::size_t kBlock = 512, kDataStart = 3 * kBlock;
    std::vector<uint8_t> out(kDataStart, 0);
    const auto put16 = [&out](std::size_t o, uint32_t v) {
        out[o] = static_cast<uint8_t>(v); out[o + 1] = static_cast<uint8_t>(v >> 8);
    };
    const auto put32 = [&put16](std::size_t o, uint32_t v) {
        put16(o, v & 0xFFFFu); put16(o + 2, v >> 16);
    };

    std::memcpy(out.data(), "WOZ2\xFF\x0A\x0D\x0A", 8);
    std::memcpy(out.data() + 12, "INFO", 4);  put32(16, 60);
    // Offsets, not pointers: `out` grows with every track below.
    constexpr std::size_t kInfo = 20, kTmap = 88;
    out[kInfo + 0] = 2;                       // INFO version
    out[kInfo + 1] = 1;                       // 5.25"
    std::memset(out.data() + kInfo + 5, ' ', 32);
    std::memcpy(out.data() + kInfo + 5, "POM2", 4);   // creator
    out[kInfo + 37] = 1;                      // sides
    out[kInfo + 38] = thirteenSector ? 2 : 0; // boot sector format: 13 / unknown
    out[kInfo + 39] = 32;                     // optimal bit timing: 4 µs
    std::memcpy(out.data() + 80, "TMAP", 4);  put32(84, 160);
    std::memset(out.data() + kTmap, 0xFF, 160);
    std::memcpy(out.data() + 248, "TRKS", 4);

    uint32_t largestBlocks = 0;
    uint8_t  trk = 0;
    const int count = std::min<int>(static_cast<int>(surface.size()), kTracks);
    for (int t = 0; t < count; ++t) {
        const TrackBuffer& buf = surface[static_cast<std::size_t>(t)];
        if (isUnformatted(buf)) continue;     // no flux: the track is absent
        // The same cell layout the read path derives (expandTrackBits), so a
        // drive reading this file sees the bit stream it saw from the nibbles.
        uint8_t widths[kNibblesPerTrack];
        computeSyncRunWidths(buf.data(), widths);
        std::vector<uint8_t> packed;
        packed.reserve(kNibblesPerTrack * 10 / 8 + 1);
        uint32_t bits = 0;
        uint8_t  acc = 0;
        const auto push = [&](int bit) {
            acc = static_cast<uint8_t>((acc << 1) | bit);
            if ((++bits & 7u) == 0) { packed.push_back(acc); acc = 0; }
        };
        for (int i = 0; i < kNibblesPerTrack; ++i) {
            for (int b = 7; b >= 0; --b) push((buf[static_cast<std::size_t>(i)] >> b) & 1);
            if (widths[i] == 10) { push(0); push(0); }
        }
        if (bits & 7u) packed.push_back(static_cast<uint8_t>(acc << (8 - (bits & 7u))));

        const uint32_t blocks = static_cast<uint32_t>((packed.size() + kBlock - 1) / kBlock);
        const uint32_t start  = static_cast<uint32_t>(out.size() / kBlock);
        const std::size_t rec = 256 + static_cast<std::size_t>(trk) * 8;
        put16(rec, start); put16(rec + 2, blocks); put32(rec + 4, bits);
        out.insert(out.end(), packed.begin(), packed.end());
        out.resize((static_cast<std::size_t>(start) + blocks) * kBlock, 0);
        // The head reads track t from a quarter-track either side of it.
        const std::size_t qt = static_cast<std::size_t>(t) * 4;
        out[kTmap + qt] = trk;
        if (qt > 0)       out[kTmap + qt - 1] = trk;
        if (qt + 1 < 160) out[kTmap + qt + 1] = trk;
        largestBlocks = std::max(largestBlocks, blocks);
        ++trk;
    }
    put16(kInfo + 44, largestBlocks);         // INFO largest_track, in blocks
    put32(252, static_cast<uint32_t>(out.size() - 256));
    put32(8, wozCrc32(out.data() + 12, out.size() - 12));
    return out;
}

// ── Phase 2 ─────────────────────────────────────────────────────────────────
bool DiskImage::commitWriteBack(PendingWriteBack&& pending, std::string& error,
                                std::string* divertedTo)
{
    error.clear();
    if (divertedTo) divertedTo->clear();
    if (!pending.valid) return true;
    // The WOZ header CRC, over the payload that is about to reach the file.
    // Here rather than at capture: the capture runs under `stateMutex`, and a
    // flux image is megabytes.
    if (pending.kind == PendingWriteBack::Kind::Whole && pending.wozCrc &&
        pending.payload.size() > 12) {
        const uint32_t crc = wozCrc32(pending.payload.data() + 12,
                                      pending.payload.size() - 12);
        for (int i = 0; i < 4; ++i)
            pending.payload[8 + static_cast<std::size_t>(i)] =
                static_cast<uint8_t>(crc >> (8 * i));
    }
    // In capture order: the background autosave and the eject / swap / flush
    // paths commit from different threads (MediaAutosave.h). The same lock
    // also serialises the sector formats' read-modify-write of the file.
    return pom2::commitMediaInOrder(pending.path, pending.lineage, pending.seq,
                                    pending.mounted, error, divertedTo,
        [&pending](const std::string& target, bool wholeSurface, std::string& err) {
        const auto emit = [&pending](std::ofstream& wf, const uint8_t* data,
                                     std::size_t n) {
            if (!pending.header.empty())
                wf.write(reinterpret_cast<const char*>(pending.header.data()),
                         static_cast<std::streamsize>(pending.header.size()));
            wf.write(reinterpret_cast<const char*>(data),
                     static_cast<std::streamsize>(n));
            if (!pending.trailer.empty())
                wf.write(reinterpret_cast<const char*>(pending.trailer.data()),
                         static_cast<std::streamsize>(pending.trailer.size()));
        };

        // A WOZ or a nibble image is its complete payload already, so a
        // diverted write is the same bytes under another name.
        if (pending.kind == PendingWriteBack::Kind::Whole) {
            if (!writeFileAtomic(target, err, [&](std::ofstream& wf) {
                    emit(wf, pending.payload.data(), pending.payload.size());
                }))
                return false;
            pom2::log().info("Disk II", wholeSurface
                ? "Saved the guest's copy to " + target
                : pending.summary);
            return true;
        }

        // Sector formats. Normally: read the existing file and decode the
        // dirty tracks on top of it — the pre-fill is what keeps the tracks
        // the guest never touched. A diverted mount may not use the file at
        // its own path as the base (it is somebody else's now), so every
        // track is decoded from the captured surface instead. For an
        // enveloped source the payload starts past the header.
        const bool is13 = pending.kind == PendingWriteBack::Kind::Sectors13;
        const std::size_t imageBytes = is13 ? kBytesPerImage13 : kBytesPerImage;
        const int perTrack = is13 ? kSectorsPerTrack13 : kSectorsPerTrack;
        const uint32_t allSectors = (1u << perTrack) - 1u;
        const bool fromSurface =
            wholeSurface && pending.surface.size() == static_cast<std::size_t>(kTracks);
        std::vector<uint8_t> bytes(imageBytes, 0);
        if (!fromSurface) {
            std::ifstream rf(target, std::ios::binary);
            if (!rf ||
                !rf.seekg(static_cast<std::streamoff>(pending.header.size())) ||
                !rf.read(reinterpret_cast<char*>(bytes.data()),
                         static_cast<std::streamsize>(imageBytes))) {
                err = "Cannot preserve unchanged tracks: source image "
                      "is missing or truncated: " + target;
                return false;
            }
        }
        // 16-sector: the file holds LOGICAL sectors, the track PHYSICAL ones,
        // through the DOS or ProDOS skew. 13-sector: file offset =
        // (track*13 + S)*256 where S is the address-field sector number
        // (decodeNibbles13 indexes by S), so no skew.
        const int* skew = logicalForPhysical(pending.order);
        const auto fileSector = [&](int t, int p) -> uint8_t* {
            const int s = is13 ? p : skew[p];
            return bytes.data() +
                   (static_cast<std::size_t>(t) * perTrack + s) * kSectorBytes;
        };
        int decodedTracks = 0;
        std::vector<int> undecodable;
        const auto decodeTrack = [&](int t, const TrackBuffer& nibbles) {
            uint8_t sectors[kSectorsPerTrack][kSectorBytes];
            uint32_t mask = 0;
            if (is13) decodeNibbles13(nibbles, t, sectors, &mask);
            else      decodeNibbles(nibbles, t, sectors, &mask);
            // All of them or none (rule 2): a sector that did not decode is
            // NOT allowed to keep the file's bytes under a success message.
            if (mask != allSectors) { undecodable.push_back(t); return; }
            for (int p = 0; p < perTrack; ++p)
                std::memcpy(fileSector(t, p), sectors[p], kSectorBytes);
            ++decodedTracks;
        };
        if (fromSurface) {
            for (int t = 0; t < kTracks; ++t) {
                const TrackBuffer& nibbles = pending.surface[static_cast<std::size_t>(t)];
                // The unformatted surface is zeros in the backing file
                // (DiskImage::eraseSurface), not a track that failed to decode.
                if (!isUnformatted(nibbles)) decodeTrack(t, nibbles);
            }
        } else {
            for (std::size_t i = 0; i < pending.trackNumbers.size(); ++i)
                decodeTrack(pending.trackNumbers[i], pending.trackNibbles[i]);
        }
        if (!undecodable.empty()) {
            // Rule 3: the surface goes to a WOZ beside the target before the
            // refusal is reported.
            std::string recovered, recoveryError;
            if (pending.surface.size() == static_cast<std::size_t>(kTracks)) {
                const std::string wozPath = target + ".recovered.woz";
                const std::vector<uint8_t> woz = buildWozFromSurface(pending.surface, is13);
                if (writeFileAtomic(wozPath, recoveryError, [&](std::ofstream& wf) {
                        wf.write(reinterpret_cast<const char*>(woz.data()),
                                 static_cast<std::streamsize>(woz.size()));
                    }))
                    recovered = wozPath;
            }
            return reportUndecodable(undecodable, recovered, recoveryError, err);
        }
        if (!writeFileAtomic(target, err, [&](std::ofstream& wf) {
                emit(wf, bytes.data(), bytes.size());
            }))
            return false;
        pom2::log().info("Disk II", wholeSurface
            ? "Saved the guest's copy to " + target
            : "Saved " + std::to_string(decodedTracks) + pending.summary);
        return true;
    });
}
