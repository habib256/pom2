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

#include "DiskImage.h"
#include "AtomicFileReplace.h"
#include "Logger.h"
#include "TwoImg.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <vector>

namespace {

// 6-bit → on-disk nibble translation table. The 64 valid disk byte values:
// bit-7 always set, no two consecutive zero bits, and no zero run that
// the Disk II's analog data separator can't recover.
constexpr uint8_t kGcrTable[64] = {
    0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6,
    0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC,
    0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
    0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE,
    0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
    0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6,
    0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
};

// DOS 3.3 sector skewing: physical sector P on disk holds the data for
// logical sector kDos33LogicalForPhysical[P]. The .dsk file stores
// sectors in *logical* order (sector 0 first); on disk we write them in
// *physical* order, pulling each slot's data via this mapping.
constexpr int kDos33LogicalForPhysical[16] = {
    0, 7, 14, 6, 13, 5, 12, 4, 11, 3, 10, 2, 9, 1, 8, 15
};

// ProDOS sector skewing for 5.25" disks. Mirror skew of the DOS 3.3 table
// — both are constant-skew variants of the standard +7/-8 Apple
// interleave, with sectors 0 and 15 fixed. Used for .po images, which
// store data in ProDOS-logical-sector order.
constexpr int kProDosLogicalForPhysical[16] = {
    0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15
};

bool endsWithCi(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        const char a = s[s.size() - n + i];
        const char b = suffix[i];
        const auto lc = [](char c) -> char {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        };
        if (lc(a) != lc(b)) return false;
    }
    return true;
}

// 4-and-4 encoding: split a byte into "odd" (high) and "even" (low) bit
// halves, OR each with $AA so the result is always a valid disk nibble
// (bit-7 set, alternating bits guarantee no zero runs).
inline void write4and4(uint8_t*& dst, uint8_t b)
{
    *dst++ = static_cast<uint8_t>(((b >> 1) & 0x55) | 0xAA);
    *dst++ = static_cast<uint8_t>((b & 0x55) | 0xAA);
}

// Bit-reverse a 2-bit pair: bit 0 ↔ bit 1. Used when packing the low-2-bit
// triples for the 86 secondary nibbles — the swap makes the running-XOR
// checksum recover the original byte cleanly on read-back.
inline uint8_t rev2(uint8_t b) { return ((b & 1) << 1) | ((b >> 1) & 1); }

// Inverse of kGcrTable: maps a disk byte ($96-$FF) back to its 6-bit
// value (0..63), or 0xFF if the byte isn't a valid GCR nibble.
constexpr std::array<uint8_t, 256> makeGcrInverse()
{
    std::array<uint8_t, 256> t{};
    for (int i = 0; i < 256; ++i) t[i] = 0xFF;
    for (uint8_t v = 0; v < 64; ++v) t[kGcrTable[v]] = v;
    return t;
}
constexpr std::array<uint8_t, 256> kGcrInverse = makeGcrInverse();

// 5-bit → on-disk nibble table for 13-sector (5-and-3) GCR, used by
// DOS 3.1/3.2/3.2.1. Verbatim from MAME `formats/ap2_dsk.cpp`
// a2_13sect_format::translate5 (lines 411-415).
constexpr uint8_t kTranslate5[32] = {
    0xAB, 0xAD, 0xAE, 0xAF, 0xB5, 0xB6, 0xB7, 0xBA,
    0xBB, 0xBD, 0xBE, 0xBF, 0xD6, 0xD7, 0xDA, 0xDB,
    0xDD, 0xDE, 0xDF, 0xEA, 0xEB, 0xED, 0xEE, 0xEF,
    0xF5, 0xF6, 0xF7, 0xFA, 0xFB, 0xFD, 0xFE, 0xFF,
};

// Inverse of kTranslate5: disk byte → 5-bit value (0..31), 0xFF if invalid.
constexpr std::array<uint8_t, 256> makeUntranslate5()
{
    std::array<uint8_t, 256> t{};
    for (int i = 0; i < 256; ++i) t[i] = 0xFF;
    for (uint8_t v = 0; v < 32; ++v) t[kTranslate5[v]] = v;
    return t;
}
constexpr std::array<uint8_t, 256> kUntranslate5 = makeUntranslate5();

// Decode one 4-and-4 byte (8 nibbles → 4 bytes). Returns the byte; the
// caller advances the cursor by 2.
inline uint8_t decode4and4(const uint8_t* p)
{
    return static_cast<uint8_t>(((p[0] << 1) & 0xAA) | (p[1] & 0x55));
}

// Can the host filesystem accept a write-back to this image at all?
// Same probe (and same rationale) as `Block512Backing::loadFile` and
// `Disk35Image::loadFile`: a chmod-read-only .dsk used to mount as a
// perfectly writable disk, take a whole session of guest writes, and then
// have `saveDirty()` REPLACE the file anyway — writeFileAtomic renames a
// sibling temp over it, and POSIX rename only needs write permission on
// the DIRECTORY, not on the file. The user's deliberate read-only marking
// was silently defeated (and the file's mode reset to the umask default).
// Where the directory is read-only too, the rename fails instead and the
// whole session's writes are lost with only a log line. Surfacing the
// condition as physical write-protect makes the guest see the error at
// write time, exactly like the notch on a real sleeve.
bool hostFileIsReadOnly(const std::string& path)
{
    std::ofstream probe(path, std::ios::in | std::ios::out | std::ios::binary);
    return !probe;
}

}  // namespace

DiskImage::DiskImage()
{
    for (auto& t : tracks) t.fill(0xFF);
}

uint8_t DiskImage::nibbleAt(int track, int index) const
{
    if (!loaded || track < 0 || track >= kTracks) return 0xFF;
    const int n = ((index % kNibblesPerTrack) + kNibblesPerTrack) % kNibblesPerTrack;
    return tracks[track][n];
}

// ── Content-driven format detection ────────────────────────────────────────
//
// `detectFormat()` is the single decision point for what an image file is.
// loadFile() slurps the file once, hands the bytes to detectFormat, and
// dispatches per ImageKind. Each kind has a matching per-format buffer
// loader. Adding a new format (2MG, MacBinary wrapper, CNib2, …) means
// extending the kind enum + adding a detection branch here, not editing
// the dispatch logic.
DiskImage::DetectResult DiskImage::detectFormat(const std::string& path,
                                                const std::vector<uint8_t>& bytes)
{
    DetectResult r;
    const std::size_t totalLen = bytes.size();
    std::size_t baseOff = 0;
    bool macBinaryStripped = false;

    // ── MacBinary wrapper — 128-byte prefix from legacy Mac downloads ──
    // Predicate per AppleWin's DiskImageHelper.cpp:
    //   byte[0]    == 0     (always-zero "old version" marker)
    //   byte[1]    in [1..63] (Pascal-string filename length)
    //   byte[1+1+len] == 0  (terminator after the filename)
    //   byte[122,123] == 0  (reserved field — zeroes in real MacBinary)
    // All four fit inside the image's own first sector, and ordinary disk
    // images DO hit them by chance — the claim that this is "safe to apply
    // unconditionally" was wrong. See the guard below the lambda.
    auto looksLikeMacBinary = [](const uint8_t* p, std::size_t n) -> bool {
        if (n < 128)             return false;
        if (p[0] != 0x00)        return false;
        const uint8_t nameLen = p[1];
        if (nameLen == 0 || nameLen >= 64) return false;
        if (p[1 + nameLen + 1] != 0x00) return false;
        if (p[122] != 0x00 || p[123] != 0x00) return false;
        return true;
    };
    // The sniff above is only FOUR bytes wide and every one of them lives
    // inside the image's own first sector, so ordinary disk images hit it by
    // chance — and stripping 128 bytes off one leaves a length that matches
    // nothing, so the file becomes unloadable with a diagnostic blaming a
    // MacBinary header it never had. Strip only when doing so actually turns
    // an unrecognised length INTO a recognised one; a buffer that already has
    // a valid size is not a wrapper, whatever its first bytes look like.
    auto knownPayloadLength = [](std::size_t len) {
        return len == static_cast<std::size_t>(kBytesPerImage)    ||
               len == static_cast<std::size_t>(kBytesPerImage13)  ||
               len == static_cast<std::size_t>(kTracks) * kNibblesPerTrack ||
               len == static_cast<std::size_t>(kTracks) * 6384;
    };
    // Magic-based formats have no fixed length — a 2IMG carries a header, a
    // payload and an optional comment — so they are recognised by signature
    // instead. (WOZ has its own MacBinary handling in loadWoz; accepting it
    // here too costs nothing and keeps this one rule.)
    auto magicAfterStrip = [&]() {
        if (totalLen < 128 + 4) return false;
        const uint8_t* q = bytes.data() + 128;
        return std::memcmp(q, "2IMG", 4) == 0 ||
               std::memcmp(q, "WOZ1", 4) == 0 ||
               std::memcmp(q, "WOZ2", 4) == 0;
    };
    if (looksLikeMacBinary(bytes.data(), totalLen) &&
        !knownPayloadLength(totalLen) &&
        (knownPayloadLength(totalLen - 128) || magicAfterStrip())) {
        baseOff = 128;
        macBinaryStripped = true;
    }

    const uint8_t* base = bytes.data() + baseOff;
    const std::size_t n = totalLen - baseOff;

    auto addMacBinaryNote = [&](DetectResult& res) {
        if (macBinaryStripped) {
            if (!res.diag.empty()) res.diag += "; ";
            res.diag += "MacBinary 128-byte header stripped";
            // Record the wrapper through the same envelope plumbing 2IMG
            // uses, so write-back re-emits the 128 header bytes. Without
            // this, saveDirty()'s pre-fill read started at file offset 0
            // (header + shifted tracks) and then truncated the file to a
            // bare image — every NON-dirty track was silently corrupted
            // on the first guest write to a MacBinary-wrapped image.
            // A 2IMG inside MacBinary already set its own (wider) window.
            if (!res.twoImgWrap) {
                res.twoImgWrap         = true;
                res.twoImgHeaderEnd    = baseOff;
                res.twoImgTrailerStart = totalLen;
            }
        }
    };

    // ── 2IMG / .2mg — 64-byte (or longer) envelope around the payload ──
    // Spec: https://apple2.org.za/gswv/a2zine/Docs/DiskImage_2MG_Info.txt
    //   bytes  0..3   magic "2IMG"
    //   bytes  4..7   creator code (4 ASCII, free-form)
    //   bytes  8..9   header length (u16 LE — usually 64)
    //   bytes 10..11  version
    //   bytes 12..15  image format (u32 LE — 0=DOS, 1=ProDOS, 2=NIB)
    //   bytes 16..19  flags (u32 LE — bit 31 = locked/write-protect,
    //                  bit 8 = volume-number-valid, bits 0-7 = vol#)
    //   bytes 20..23  num blocks (ProDOS only — ignored here)
    //   bytes 24..27  data offset (u32 LE — usually 64)
    //   bytes 28..31  data length (u32 LE)
    //   bytes 32..47  comment / creator chunk pointers (preserved verbatim)
    if (n >= 64 &&
        base[0] == '2' && base[1] == 'I' && base[2] == 'M' && base[3] == 'G') {
        auto rd16 = [&](std::size_t o) -> uint16_t {
            return static_cast<uint16_t>(base[o]) |
                   (static_cast<uint16_t>(base[o + 1]) << 8);
        };
        auto rd32 = [&](std::size_t o) -> uint32_t {
            return static_cast<uint32_t>(base[o]) |
                   (static_cast<uint32_t>(base[o + 1]) << 8) |
                   (static_cast<uint32_t>(base[o + 2]) << 16) |
                   (static_cast<uint32_t>(base[o + 3]) << 24);
        };
        const uint16_t headerLen = rd16(8);
        const uint32_t format    = rd32(12);
        const uint32_t flags     = rd32(16);
        const uint32_t dataOff   = rd32(24);
        const uint32_t dataLen   = rd32(28);

        if (headerLen < 52) {
            r.error = "Refused " + path + ": 2IMG header length " +
                      std::to_string(headerLen) + " is too small (need ≥ 52)";
            return r;
        }
        if (dataOff < headerLen || dataOff > n ||
            dataLen == 0 ||
            static_cast<std::size_t>(dataOff) + dataLen > n) {
            r.error = "Refused " + path +
                      ": 2IMG header points outside the file (dataOff=" +
                      std::to_string(dataOff) + ", dataLen=" +
                      std::to_string(dataLen) + ", file=" +
                      std::to_string(n) + ")";
            return r;
        }

        // Flags-word semantics live in TwoImg.h (shared with Disk35Image
        // and Block512Backing — the lock bit was misread identically in
        // all three loaders once; one definition now).
        const uint8_t vol = pom2::twoImgVolume(flags);
        const bool    wp  = pom2::twoImgWriteProtected(flags);

        ImageKind kind = ImageKind::Unknown;
        SectorOrder order = SectorOrder::Dos33;
        switch (format) {
            case 0:
                if (dataLen != static_cast<uint32_t>(kBytesPerImage)) {
                    r.error = "Refused " + path +
                              ": 2IMG DOS payload must be 143360 bytes, got " +
                              std::to_string(dataLen);
                    return r;
                }
                kind = ImageKind::TwoImgDos;
                order = SectorOrder::Dos33;
                break;
            case 1:
                if (dataLen != static_cast<uint32_t>(kBytesPerImage)) {
                    r.error = "Refused " + path +
                              ": 2IMG ProDOS floppy payload must be 143360 "
                              "bytes, got " + std::to_string(dataLen) +
                              " (larger volumes belong on the HDV card)";
                    return r;
                }
                kind = ImageKind::TwoImgProDos;
                order = SectorOrder::ProDOS;
                break;
            case 2:
                if (dataLen != static_cast<uint32_t>(kTracks * kNibblesPerTrack) &&
                    dataLen != static_cast<uint32_t>(kTracks * 6384)) {
                    r.error = "Refused " + path +
                              ": 2IMG NIB payload must be 232960 or 223440 "
                              "bytes, got " + std::to_string(dataLen);
                    return r;
                }
                kind = ImageKind::TwoImgNib;
                break;
            default:
                r.error = "Refused " + path +
                          ": 2IMG header has unsupported format byte " +
                          std::to_string(format);
                return r;
        }

        r.kind               = kind;
        r.order              = order;
        r.payloadOff         = baseOff + dataOff;
        r.payloadLen         = dataLen;
        r.volumeNumber       = vol;
        r.writeProtect       = wp;
        r.twoImgWrap         = true;
        r.twoImgHeaderEnd    = baseOff + dataOff;
        r.twoImgTrailerStart = baseOff + dataOff + dataLen;
        const char* fmtName = (format == 0) ? "DOS 3.3"
                            : (format == 1) ? "ProDOS"
                                            : "NIB";
        r.diag = std::string("2IMG/.2mg wrapper, ") + fmtName +
                 " payload (" + std::to_string(dataLen) + " bytes)" +
                 (wp ? ", write-protected" : "") +
                 ", volume " + std::to_string(static_cast<int>(vol));
        addMacBinaryNote(r);
        return r;
    }

    // ── WOZ — magic bytes (case-sensitive ASCII + sentinel) ────────────
    // Per Applesauce WOZ 2.1 spec: first 8 bytes are 'WOZ1' or 'WOZ2'
    // followed by 0xFF 0x0A 0x0D 0x0A. The extension is a hint but the
    // magic is authoritative.
    if (n >= 8 &&
        base[0] == 'W' && base[1] == 'O' && base[2] == 'Z' &&
        (base[3] == '1' || base[3] == '2') &&
        base[4] == 0xFF && base[5] == 0x0A &&
        base[6] == 0x0D && base[7] == 0x0A) {
        r.kind       = ImageKind::Woz;
        r.payloadOff = baseOff;
        r.payloadLen = n;
        r.diag       = "WOZ" + std::string(1, static_cast<char>(base[3])) +
                       " bit-cell image (" + std::to_string(n) + " bytes)";
        addMacBinaryNote(r);
        return r;
    }

    // ── Raw .nib — exactly 232 960 bytes (35 × 6656) ──────────────────
    if (n == static_cast<std::size_t>(kTracks) * kNibblesPerTrack) {
        r.kind       = ImageKind::Nib232k;
        r.payloadOff = baseOff;
        r.payloadLen = n;
        r.diag       = ".nib raw nibble stream (35 × 6656 bytes)";
        addMacBinaryNote(r);
        return r;
    }

    // ── CNib2 .nib variant — exactly 223 440 bytes (35 × 6384) ────────
    // Rarer 6384/track encoding used by some pre-WOZ tooling. The loader
    // pads each track to the standard 6656 width with $FF sync bytes;
    // saveDirty truncates back when writing.
    if (n == static_cast<std::size_t>(kTracks) * 6384) {
        r.kind       = ImageKind::CNib2;
        r.payloadOff = baseOff;
        r.payloadLen = n;
        r.diag       = ".nib (CNib2) raw nibble stream (35 × 6384 bytes)";
        addMacBinaryNote(r);
        return r;
    }

    // ── 116 480-byte 13-sector image (DOS 3.1/3.2/3.2.1) ──────────────
    // 35 × 13 × 256. Pre-DOS-3.3, 5-and-3 GCR. Always DOS sector order
    // (ProDOS predates this format's relevance and is 16-sector only).
    if (n == static_cast<std::size_t>(kBytesPerImage13)) {
        r.kind         = ImageKind::Dos32_13;
        r.payloadOff   = baseOff;
        r.payloadLen   = n;
        r.order        = SectorOrder::Dos33;
        r.volumeNumber = 254;
        r.diag = "116 480-byte image, DOS 3.x 13-sector (5-and-3 GCR)";
        addMacBinaryNote(r);
        return r;
    }

    // ── 143 360-byte sector image — DOS 3.3 vs ProDOS skew ─────────────
    if (n == static_cast<std::size_t>(kBytesPerImage)) {
        // Default from extension hint. `.po` → ProDOS, else DOS 3.3.
        // Will be overridden below if the vol-dir content sniff disagrees.
        const bool extIsProdos = endsWithCi(path, ".po");
        SectorOrder order = extIsProdos ? SectorOrder::ProDOS
                                        : SectorOrder::Dos33;

        // Content sniff: a ProDOS volume directory key block lives at
        // file offset 0x400 in a ProDOS-skewed image. Its mirror in a
        // DOS-skewed image (which holds the same ProDOS data via DOS
        // sector order) lands at 0xB00. Inspect both; if one matches and
        // the other doesn't, that's the truth — regardless of extension.
        auto looksLikeVolHeader = [base, n](std::size_t off) -> bool {
            if (off + 20 > n) return false;
            const uint8_t* p = base + off;
            if (p[0] != 0x00 || p[1] != 0x00) return false;
            const uint16_t next =
                static_cast<uint16_t>(p[2]) |
                (static_cast<uint16_t>(p[3]) << 8);
            if (next == 0 || next >= 280) return false;  // 280-block vol = blocks 0..279
            const uint8_t st_nl = p[4];
            if ((st_nl & 0xF0) != 0xF0) return false;
            const uint8_t nlen = st_nl & 0x0F;
            if (nlen < 1 || nlen > 15) return false;
            for (uint8_t i = 0; i < nlen; ++i) {
                const uint8_t c = p[5 + i];
                const bool ok = (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') ||
                                c == '.';
                if (!ok) return false;
            }
            return true;
        };
        const bool prodosVolHere = looksLikeVolHeader(0x400);
        const bool dosVolHere    = looksLikeVolHeader(0xB00);
        bool overridden = false;
        if (order == SectorOrder::Dos33 && prodosVolHere && !dosVolHere) {
            order = SectorOrder::ProDOS;
            overridden = true;
        } else if (order == SectorOrder::ProDOS && !prodosVolHere && dosVolHere) {
            order = SectorOrder::Dos33;
            overridden = true;
        }

        r.kind = (order == SectorOrder::ProDOS) ? ImageKind::ProDos143k
                                                : ImageKind::Dsk143k;
        r.payloadOff   = baseOff;
        r.payloadLen   = n;
        r.order        = order;
        r.volumeNumber = 254;  // DOS 3.3 default; 2MG header may override later
        r.diag = std::string("143 360-byte image, ") +
                 (order == SectorOrder::ProDOS ? "ProDOS" : "DOS 3.3") +
                 " sector order" +
                 (overridden ? " (overridden by vol-dir content sniff)" : "");
        addMacBinaryNote(r);
        return r;
    }

    // ── No match → Unknown with a diagnostic error ─────────────────────
    r.error = "Refused " + path + ": " +
              (macBinaryStripped
                ? "after stripping the MacBinary 128-byte header, payload "
                : "file ") +
              "size " + std::to_string(n) +
              " bytes doesn't match any supported format "
              "(143360=.dsk/.po, 232960=.nib, 223440=.nib/CNib2, "
              "WOZ magic missing)";
    return r;
}

bool DiskImage::loadNibFromBuffer(const uint8_t* data, std::size_t len,
                                  int nibblesPerTrack,
                                  const std::string& imgPath)
{
    if (nibblesPerTrack <= 0 || nibblesPerTrack > kNibblesPerTrack) {
        lastError = "loadNibFromBuffer: invalid nibblesPerTrack=" +
                    std::to_string(nibblesPerTrack);
        loaded = false;
        return false;
    }
    const std::size_t expected =
        static_cast<std::size_t>(kTracks) * static_cast<std::size_t>(nibblesPerTrack);
    if (len != expected) {
        lastError = "loadNibFromBuffer: expected " +
                    std::to_string(expected) + " bytes, got " +
                    std::to_string(len);
        loaded = false;
        return false;
    }
    // Non-WOZ images always use the standard 4 µs bit cell — clear any
    // WOZ2 INFO value left by a previous image in this (reused) drive slot.
    optimalBitTiming = 32;
    for (int t = 0; t < kTracks; ++t) {
        // Copy the source nibbles, then pad the remainder of the
        // 6656-wide track buffer with $FF (sync gap). For the standard
        // 6656/track case the pad loop is empty; for CNib2's 6384/track
        // it backfills the 272 trailing nibbles so the LSS sees a normal
        // sync run at the track wrap-around point.
        std::memcpy(tracks[t].data(),
                    data + static_cast<std::size_t>(t) * nibblesPerTrack,
                    static_cast<std::size_t>(nibblesPerTrack));
        if (nibblesPerTrack < kNibblesPerTrack) {
            std::memset(tracks[t].data() + nibblesPerTrack,
                        0xFF,
                        static_cast<std::size_t>(kNibblesPerTrack - nibblesPerTrack));
        }
    }
    // 13-sector detection for raw nibble streams: a 13-sector disk's
    // address fields use the D5 AA B5 prologue (vs D5 AA 96 for 16-sector).
    // Scan track 0 so the card serves the 341-0009 boot PROM for 13s .nib
    // dumps (e.g. dos32std.nib).
    sectorsPerTrack_ = kSectorsPerTrack;
    {
        const auto& b = tracks[0];
        for (int i = 0; i + 2 < kNibblesPerTrack; ++i) {
            if (b[i] != 0xD5 || b[i + 1] != 0xAA) continue;
            if (b[i + 2] == 0xB5) { sectorsPerTrack_ = kSectorsPerTrack13; break; }
            if (b[i + 2] == 0x96) break;   // 16-sector address mark seen first
        }
    }
    path        = imgPath;
    loaded      = true;
    nibFormat   = true;
    cnib2Format = (nibblesPerTrack == 6384);
    wozFormat   = false;
    twoImgFormat = false;
    twoImgHeaderRaw.clear();
    twoImgTrailerRaw.clear();
    fileWriteProtected = false;
    sectorOrder = SectorOrder::Dos33;     // not meaningful for .nib
    dirty.fill(false);
    anyDirty    = false;
    lastError.clear();
    invalidateAllBitStreams();
    return true;
}

bool DiskImage::loadSectorImageFromBuffer(const uint8_t* data, std::size_t len,
                                          SectorOrder order, uint8_t volume,
                                          const std::string& imgPath)
{
    if (len == static_cast<std::size_t>(kBytesPerImage13)) {
        // DOS 3.1/3.2/3.2.1 13-sector, 5-and-3 GCR. Always DOS order.
        sectorsPerTrack_ = kSectorsPerTrack13;
        for (int t = 0; t < kTracks; ++t) {
            nibblizeTrack13(t,
                data + t * kSectorsPerTrack13 * kSectorBytes, volume);
        }
    } else if (len == static_cast<std::size_t>(kBytesPerImage)) {
        sectorsPerTrack_ = kSectorsPerTrack;
        const int* skew = (order == SectorOrder::ProDOS)
                          ? kProDosLogicalForPhysical
                          : kDos33LogicalForPhysical;
        for (int t = 0; t < kTracks; ++t) {
            nibblizeTrack(t,
                data + t * kSectorsPerTrack * kSectorBytes,
                volume, skew);
        }
    } else {
        lastError = "loadSectorImageFromBuffer: expected " +
                    std::to_string(kBytesPerImage) + " or " +
                    std::to_string(kBytesPerImage13) + " bytes, got " +
                    std::to_string(len);
        loaded = false;
        return false;
    }
    path        = imgPath;
    loaded      = true;
    nibFormat   = false;
    cnib2Format = false;
    wozFormat   = false;
    twoImgFormat = false;
    twoImgHeaderRaw.clear();
    twoImgTrailerRaw.clear();
    fileWriteProtected = false;
    // Non-WOZ images always use the standard 4 µs bit cell — clear any
    // WOZ2 INFO value left by a previous image in this (reused) drive slot.
    optimalBitTiming = 32;
    sectorOrder = order;
    dirty.fill(false);
    anyDirty    = false;
    lastError.clear();
    invalidateAllBitStreams();
    return true;
}

bool DiskImage::loadFile(const std::string& imgPath)
{
    if (!saveDirty()) return false;
    // Heap, not stack — see the NOTE on class DiskImage (512 KB macOS threads).
    auto replacement = std::make_unique<DiskImage>();
    replacement->writeBackEnabled = writeBackEnabled;
    if (!replacement->loadFileUnchecked(imgPath)) {
        lastError = replacement->lastError;
        return false;
    }
    *this = std::move(*replacement);
    return true;
}

bool DiskImage::loadFileUnchecked(const std::string& imgPath)
{
    // Slurp the whole file once; detectFormat needs the magic bytes and
    // size, and each per-format loader takes a buffer slice.
    std::ifstream f(imgPath, std::ios::binary);
    if (!f) {
        lastError = "Cannot open " + imgPath;
        loaded = false;
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streampos end = f.tellg();
    // A 5.25-inch image is at most about 1.1 MiB in canonical WOZ1 form.
    // Leave generous room for WOZ2 chunks and wrappers, but reject sparse
    // or hostile multi-gigabyte files before converting their size to a
    // vector allocation.
    constexpr std::uintmax_t kMaxDiskImageBytes = 16u * 1024u * 1024u;
    if (end < 0 || static_cast<std::uintmax_t>(end) > kMaxDiskImageBytes) {
        lastError = "Disk image is too large: " + imgPath;
        loaded = false;
        return false;
    }
    const auto size = static_cast<std::size_t>(end);
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(size);
    if (size > 0) {
        f.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(size));
        if (!f) {
            lastError = "Short read on " + imgPath;
            loaded = false;
            return false;
        }
    }

    // A new medium restarts the write framing (see writeFlux) — a nibble
    // half-assembled from the previous disk must not land on this one.
    writeFraming.fill(WriteFraming{});
    // Every failure below leaves `loaded == false`; the per-format loaders
    // set `path` only on success. Drop the old one here so a FAILED insert
    // can't leave the drive reporting "empty, but the previous disk's path"
    // — the AI control server publishes `path` and `loaded` as independent
    // fields, so the stale pair read as a mounted disk.
    path.clear();

    const DetectResult det = detectFormat(imgPath, bytes);
    if (det.kind == ImageKind::Unknown) {
        lastError = det.error.empty()
                    ? std::string("Unknown image format: ") + imgPath
                    : det.error;
        pom2::log().warn("Disk II", lastError);
        loaded = false;
        return false;
    }

    const uint8_t* payload = bytes.data() + det.payloadOff;
    const std::size_t payloadLen = det.payloadLen;

    bool ok = false;
    sectorsPerTrack_ = kSectorsPerTrack;   // 13s loader overrides; reset on re-mount
    switch (det.kind) {
        case ImageKind::Woz:
            // loadWoz reopens the file itself; refactor to take a buffer
            // is deferred to a follow-up commit (touches the WOZ parser).
            ok = loadWoz(imgPath);
            break;
        case ImageKind::Nib232k:
            ok = loadNibFromBuffer(payload, payloadLen,
                                   kNibblesPerTrack, imgPath);
            if (ok) {
                pom2::log().info("Disk II",
                    "Loaded " + imgPath + " — " + det.diag);
            }
            break;
        case ImageKind::CNib2:
            ok = loadNibFromBuffer(payload, payloadLen,
                                   /*nibblesPerTrack=*/6384, imgPath);
            if (ok) {
                pom2::log().info("Disk II",
                    "Loaded " + imgPath + " — " + det.diag);
            }
            break;
        case ImageKind::Dsk143k:
        case ImageKind::ProDos143k:
        case ImageKind::TwoImgDos:
        case ImageKind::TwoImgProDos:
        case ImageKind::Dos32_13:
            ok = loadSectorImageFromBuffer(payload, payloadLen,
                                           det.order, det.volumeNumber,
                                           imgPath);
            if (ok) {
                pom2::log().info("Disk II",
                    "Loaded " + imgPath + " — " + det.diag);
            }
            break;
        case ImageKind::TwoImgNib: {
            const int npt = (payloadLen ==
                static_cast<std::size_t>(kTracks * kNibblesPerTrack))
                ? kNibblesPerTrack : 6384;
            ok = loadNibFromBuffer(payload, payloadLen, npt, imgPath);
            if (ok) {
                pom2::log().info("Disk II",
                    "Loaded " + imgPath + " — " + det.diag);
            }
            break;
        }
        case ImageKind::Unknown:
            break;  // already handled above; keeps the switch exhaustive
    }

    // Capture the 2IMG envelope bytes for the eventual write-back path.
    // Done after the per-format loader so failed loads don't leave stale
    // wrapper state behind — twoImgFormat stays false when ok == false.
    if (ok && det.twoImgWrap) {
        twoImgFormat = true;
        twoImgHeaderRaw.assign(bytes.begin(),
                               bytes.begin() +
                                   static_cast<std::ptrdiff_t>(det.twoImgHeaderEnd));
        twoImgTrailerRaw.assign(bytes.begin() +
                                    static_cast<std::ptrdiff_t>(det.twoImgTrailerStart),
                                bytes.end());
        // 2IMG flag bit 0 marks the file as write-protected at the
        // container level; honour it on top of any in-payload protection.
        if (det.writeProtect) fileWriteProtected = true;
    } else {
        // Non-2IMG load (or failure): make sure stale wrapper state from a
        // previous 2IMG mount doesn't survive a re-insert.
        twoImgFormat = false;
        twoImgHeaderRaw.clear();
        twoImgTrailerRaw.clear();
    }

    // Host-filesystem write protection, folded in last so no per-format
    // loader can clear it (they all reset `fileWriteProtected` on entry).
    if (ok && !fileWriteProtected && hostFileIsReadOnly(imgPath)) {
        fileWriteProtected = true;
        pom2::log().info("Disk II",
            "Image file is not writable on disk — mounting "
            "write-protected: " + imgPath);
    }

    return ok;
}

bool DiskImage::loadFile(const std::string& imgPath, SectorOrder order)
{
    if (!saveDirty()) return false;
    auto replacement = std::make_unique<DiskImage>();
    replacement->writeBackEnabled = writeBackEnabled;
    if (!replacement->loadFileUnchecked(imgPath, order)) {
        lastError = replacement->lastError;
        return false;
    }
    *this = std::move(*replacement);
    return true;
}

bool DiskImage::loadFileUnchecked(const std::string& imgPath, SectorOrder order)
{
    // Manual sector-order override (bypasses content sniff). Reads the
    // file and pipes it straight to the sector-image loader regardless
    // of what `detectFormat` would have concluded. Useful for a future
    // "Force DOS / Force ProDOS" UI option when the auto-detect mis-fires.
    std::ifstream f(imgPath, std::ios::binary);
    if (!f) {
        lastError = "Cannot open " + imgPath;
        loaded = false;
        return false;
    }
    f.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (size != static_cast<std::size_t>(kBytesPerImage)) {
        lastError = "Expected " + std::to_string(kBytesPerImage) +
                    "-byte 5.25\" image, got " + std::to_string(size);
        loaded = false;
        return false;
    }
    std::vector<uint8_t> buf(size);
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(size));
    if (!f) {
        lastError = "Short read";
        loaded = false;
        return false;
    }
    // A new medium restarts the write framing, and a failed load must not
    // leave the previous disk's path behind — same reasons as the
    // auto-detecting overload above.
    writeFraming.fill(WriteFraming{});
    path.clear();
    const bool ok = loadSectorImageFromBuffer(buf.data(), buf.size(),
                                              order, /*volume=*/254, imgPath);
    if (ok && hostFileIsReadOnly(imgPath)) {
        fileWriteProtected = true;
        pom2::log().info("Disk II",
            "Image file is not writable on disk — mounting "
            "write-protected: " + imgPath);
    }
    if (ok) {
        pom2::log().info("Disk II", "Loaded " + imgPath +
                         " (35 tracks, 16 sectors, GCR-encoded, " +
                         (order == SectorOrder::ProDOS ? "ProDOS" : "DOS 3.3") +
                         " order — manual override)");
    }
    return ok;
}

// ── WOZ1 / WOZ2 loader ─────────────────────────────────────────────────────
//
// Verbatim follower of MAME `src/lib/formats/woz_dsk.cpp`. The WOZ format
// stores raw bit cells (the LSS's natural input) instead of nibbles or
// sectors; that's why .woz disks survive copy protections that tweak
// inter-byte timing or bit alignment, where re-encoded .dsk synthesises
// idealised GCR and loses the protection signature.
//
// File layout (both versions):
//   12-byte header: magic "WOZ1\xFF\n\r\n" or "WOZ2\xFF\n\r\n"
//                  + 4-byte CRC32 of the rest of the file (LE; we skip
//                    validation — corrupt CRC is rare in practice and
//                    MAME's behaviour is to load anyway, log a warning).
//   chunks: 4-byte chunk_id + 4-byte LE length + payload, repeated.
//           Mandatory: INFO, TMAP, TRKS. Optional: META, WRIT, FLUX.
//
// INFO (>= 60 bytes):
//   [0]  info_version  (1 = WOZ1 style INFO, 2+ = WOZ2 fields)
//   [1]  disk_type     (1 = 5.25", 2 = 3.5") — POM2 only handles 5.25"
//   [2]  write_protected
//   [3]  synchronized
//   [4]  cleaned
//   [5..36]  creator (32 chars, space-padded)
//   v2+: disk_sides, boot_sector_format, optimal_bit_timing,
//        compatible_hardware (LE u16), required_ram (LE u16),
//        largest_track (LE u16, in 512-byte blocks)
//
// TMAP (160 bytes):
//   One byte per quarter-track 0..159. Value = TRK index (0..159) or
//   $FF when that quarter-track is unused. Multiple quarter-tracks
//   typically share a TRK (e.g. TMAP[0..2] = 0, TMAP[3] = $FF for the
//   classic "track 0 covers 3 of 4 quarter-track positions").
//
// TRKS:
//   WOZ1: 160 fixed-size 6656-byte slots.
//     bytes 0..6645  bit data (MSB-first within each byte)
//     6646..6647     bytes_used    (LE u16)
//     6648..6649     bit_count     (LE u16)
//     6650..6651     splice_point  (LE u16, $FFFF = none)
//     6652           splice_nibble
//     6653           splice_bit_count
//   WOZ2: 160 × 8-byte TRK headers, then track bit data:
//     hdr  starting_block (LE u16, 0 = unused; offset = block × 512)
//          block_count    (LE u16)
//          bit_count      (LE u32)
//
// All 160 TMAP quarter-track slots are unpacked (FLUX takes precedence
// over TMAP for a populated slot), so per-quarter-track bit data used by
// protections like David-DOS or Locksmith is preserved — see the loader
// body below.
bool DiskImage::loadWoz(const std::string& imgPath)
{
    std::ifstream f(imgPath, std::ios::binary);
    if (!f) {
        lastError = "Cannot open " + imgPath;
        loaded = false; return false;
    }
    f.seekg(0, std::ios::end);
    const auto fileSize = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (fileSize < 12) {
        lastError = "WOZ file truncated (" + std::to_string(fileSize) + " bytes)";
        loaded = false; return false;
    }
    std::vector<uint8_t> buf(fileSize);
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(fileSize));
    if (!f) {
        lastError = "Short read on " + imgPath;
        loaded = false; return false;
    }

    // MacBinary-wrapped WOZ: detectFormat found the magic 128 bytes in,
    // but this loader re-reads the file from scratch and used to demand
    // the magic at offset 0 — wrapped WOZs never loaded at all. Slice
    // the header off; loadFile()'s generic envelope capture re-attaches
    // it for write-back, and every chunk offset below stays relative to
    // the WOZ payload.
    if (fileSize > 140 && std::memcmp(buf.data(), "WOZ", 3) != 0 &&
        (std::memcmp(buf.data() + 128, "WOZ1", 4) == 0 ||
         std::memcmp(buf.data() + 128, "WOZ2", 4) == 0)) {
        buf.erase(buf.begin(), buf.begin() + 128);
    }
    // Size of the WOZ payload actually in `buf` (differs from fileSize
    // when the MacBinary strip above ran) — every bound below uses this.
    const size_t wozSize = buf.size();

    const bool isWoz1 = std::memcmp(buf.data(), "WOZ1", 4) == 0;
    const bool isWoz2 = std::memcmp(buf.data(), "WOZ2", 4) == 0;
    if (!isWoz1 && !isWoz2) {
        lastError = "Not a WOZ file (missing WOZ1/WOZ2 magic)";
        loaded = false; return false;
    }
    if (buf[4] != 0xFF || buf[5] != 0x0A
        || buf[6] != 0x0D || buf[7] != 0x0A) {
        lastError = "WOZ header sentinel bytes wrong";
        loaded = false; return false;
    }
    // Bytes 8..11 = CRC32 of bytes [12..EOF). Verbatim port of MAME
    // `as_dsk.cpp:10-23` `crc32r` (reversed-polynomial 0xedb88320,
    // bit-reflected output). The spec allows CRC=0 as a sentinel
    // meaning "not computed by the imager"; MAME's `as_dsk.cpp:275-277`
    // rejects on mismatch unless the stored CRC is zero — we match
    // that policy.
    {
        const uint32_t expected =
            static_cast<uint32_t>(buf[ 8])        |
            (static_cast<uint32_t>(buf[ 9]) <<  8) |
            (static_cast<uint32_t>(buf[10]) << 16) |
            (static_cast<uint32_t>(buf[11]) << 24);
        if (expected != 0) {
            uint32_t crc = 0xFFFFFFFFu;
            for (size_t i = 12; i < wozSize; ++i) {
                crc ^= buf[i];
                for (int b = 0; b < 8; ++b) {
                    crc = (crc & 1u)
                            ? (crc >> 1) ^ 0xEDB88320u
                            : (crc >> 1);
                }
            }
            crc = ~crc;
            if (crc != expected) {
                char msg[96];
                std::snprintf(msg, sizeof(msg),
                    "WOZ CRC32 mismatch (header=$%08X, computed=$%08X)",
                    expected, crc);
                lastError = msg;
                loaded = false; return false;
            }
        }
    }

    // Walk chunks starting at offset 12.
    int      diskType = 1;
    fileWriteProtected = false;
    // Re-arm the 4 µs default before parsing INFO: this DiskImage object is
    // reused across disk swaps, so a WOZ1 (or truncated-INFO WOZ2) loaded
    // after a non-standard-timing WOZ2 must not inherit the old cell width.
    optimalBitTiming = 32;
    int      infoVersion = isWoz2 ? 2 : 1;
    bool     haveInfo = false;
    bool     haveTmap = false;
    std::array<uint8_t, 160> tmap{};
    tmap.fill(0xFF);
    size_t   trksOff   = 0;
    size_t   trksLen   = 0;
    // FLUX chunk (WOZ2 v2.1+, info_version >= 3). When present, it
    // overrides TRKS bit-cell data for any quarter-track whose
    // `fluxFidx[qt]` is not 0xFF. The flux delta stream preserves
    // sub-cell timing that idealised bit cells lose — required for
    // tightly-mastered protections like Wings of Fury original,
    // Captain Goodnight, Ankh, Sundog: Frozen Legacy.
    uint16_t fluxBlock        = 0;   // INFO+46 (u16 LE, file-block units)
    uint16_t fluxLargestTrack = 0;   // INFO+48 (u16 LE, in blocks)
    bool     boot13           = false;   // INFO+38 boot_sector_format == 2

    auto readU16LE = [&](size_t o) -> uint32_t {
        return static_cast<uint32_t>(buf[o])
             | (static_cast<uint32_t>(buf[o + 1]) << 8);
    };
    auto readU32LE = [&](size_t o) -> uint32_t {
        return static_cast<uint32_t>(buf[o])
             | (static_cast<uint32_t>(buf[o + 1]) << 8)
             | (static_cast<uint32_t>(buf[o + 2]) << 16)
             | (static_cast<uint32_t>(buf[o + 3]) << 24);
    };

    size_t off = 12;
    while (off <= wozSize && wozSize - off >= 8) {
        const uint32_t len = readU32LE(off + 4);
        const size_t   dataOff = off + 8;
        if (static_cast<size_t>(len) > wozSize - dataOff) {
            lastError = "WOZ chunk length runs past EOF";
            loaded = false; return false;
        }
        if (std::memcmp(buf.data() + off, "INFO", 4) == 0) {
            if (len >= 5) {
                infoVersion        = buf[dataOff + 0];
                diskType           = buf[dataOff + 1];
                fileWriteProtected = buf[dataOff + 2] != 0;
            }
            // INFO version 2+ (WOZ2) adds optimal_bit_timing at offset +39
            // (units of 125 ns; 32 = 4 µs default for 5.25"). WOZ1 images
            // and any v2 INFO that's truncated keep the 32 default we
            // initialised the field with. Reject obviously-bogus values
            // (0, or anything > 64 = 8 µs which no real Apple II drive
            // would tolerate) and log so abnormal images are visible.
            if (infoVersion >= 2 && len >= 40) {
                const uint8_t obt = buf[dataOff + 39];
                if (obt >= 8 && obt <= 64) {
                    optimalBitTiming = obt;
                }
            }
            // INFO+38 (v2) boot_sector_format: 2 = 13-sector only. This
            // is what routes a DOS 3.2 .woz to the 13-sector boot PROM —
            // loadWoz never set sectorsPerTrack_, so every 13-sector WOZ
            // was served the 16-sector P5 PROM and could not boot.
            if (infoVersion >= 2 && len >= 39 && buf[dataOff + 38] == 2) {
                boot13 = true;
            }
            // INFO version 3 (WOZ2 v2.1+) adds flux_block / largest_flux_track
            // at offsets +46 / +48. Older versions report 0. MAME
            // `as_dsk.cpp:287-290`.
            if (infoVersion >= 3 && len >= 50) {
                fluxBlock        = static_cast<uint16_t>(readU16LE(dataOff + 46));
                fluxLargestTrack = static_cast<uint16_t>(readU16LE(dataOff + 48));
                if (fluxLargestTrack == 0) fluxBlock = 0;
            }
            haveInfo = true;
        } else if (std::memcmp(buf.data() + off, "TMAP", 4) == 0) {
            if (len >= 160) {
                std::memcpy(tmap.data(), buf.data() + dataOff, 160);
                haveTmap = true;
            }
        } else if (std::memcmp(buf.data() + off, "TRKS", 4) == 0) {
            trksOff = dataOff;
            trksLen = len;
        }
        // META / WRIT / unknown: ignored. FLUX is located indirectly via
        // INFO+46 (fluxBlock) above; we don't need its in-stream offset
        // because the spec guarantees the chunk lives at block-aligned
        // file position `fluxBlock * 512`.
        off = dataOff + len;
    }

    if (!haveInfo || !haveTmap || trksLen == 0) {
        lastError = "WOZ file missing INFO/TMAP/TRKS";
        loaded = false; return false;
    }
    if (infoVersion < 1 || infoVersion > 3) {
        // MAME `as_dsk.cpp:284-286` rejects info_version outside 1..3.
        lastError = "WOZ info_version " + std::to_string(infoVersion)
                  + " outside supported range 1..3";
        loaded = false; return false;
    }
    if (diskType != 1) {
        lastError = "WOZ disk_type " + std::to_string(diskType)
                  + " not supported (only 5.25\" / type 1)";
        loaded = false; return false;
    }

    // Reset state. Tracks[] is filled with sync $FF — the legacy gate
    // returns endless sync (won't boot, but won't crash) if someone
    // accidentally bypasses the LSS path on a WOZ image.
    for (auto& t : tracks) t.fill(0xFF);
    invalidateAllBitStreams();
    dirty.fill(false);
    anyDirty = false;
    wozQtByteOff.fill(0);
    wozQtByteLen.fill(0);
    wozQtBitCount.fill(0);
    wozQtDirty.fill(false);

    // FLUX FIDX lookup: when WOZ2 v2.1+ provides a FLUX chunk, each
    // quarter-track may carry a flux delta stream that overrides the
    // bit-cell stream from TRKS. The FLUX chunk's payload starts with
    // an 8-byte chunk header followed by 160 bytes of FIDX (one per
    // QT) mapping to the same TRK header table used by bit cells
    // (just with a different fidx index — the TRK header's
    // `track_size` field is then a count of delta bytes rather than
    // a bit count).
    std::array<uint8_t, 160> fluxFidx{};
    fluxFidx.fill(0xFF);
    bool haveFlux = false;
    if (fluxBlock != 0) {
        const size_t chunkOff = static_cast<size_t>(fluxBlock) * 512;
        // The 8-byte chunk header (4-byte ID "FLUX" + 4-byte size)
        // precedes the FIDX array. MAME `as_dsk.cpp:320` reads
        // `img[off_flux*512 + 8 + trkid]` directly without verifying
        // the chunk ID; we add a defensive verification to avoid
        // misreading a non-aligned blob as flux.
        if (chunkOff <= wozSize && wozSize - chunkOff >= 8 + 160
            && std::memcmp(buf.data() + chunkOff, "FLUX", 4) == 0) {
            std::memcpy(fluxFidx.data(), buf.data() + chunkOff + 8, 160);
            haveFlux = true;
        }
    }

    // Expanded tracks store one BYTE per bit plus an int per flux event.
    // Bound both each track and their aggregate before allocating: a compact
    // FLUX delta stream full of 0xFF can describe hundreds of millions of
    // synthetic cells even though the source file itself is below 16 MiB.
    constexpr size_t kMaxTrackBits      = 1u << 20;
    constexpr size_t kMaxFluxTrackBytes = 1u << 20;
    constexpr size_t kMaxExpandedBytes  = 64u << 20;
    size_t expandedBytes = 0;

    // Helper: parse one flux track into bitStream[qt] + fluxStream[qt].
    //
    // Flux delta encoding (MAME `as_dsk.cpp:61-81`):
    //   - The TRK header at trks_off + fidx*8 has the same layout as
    //     bit-cell tracks: u16 starting_block, u16 block_count, u32
    //     track_size — but `track_size` here is the byte count of the
    //     delta stream, not a bit count.
    //   - Walk bytes; each byte is a tick count (1 tick = 125 ns).
    //   - A byte == 0xFF means "no flux this step" (continuation).
    //   - Otherwise emit one flux event at cumulative cpos ticks.
    //   - The LAST byte never emits a flux event (it represents the
    //     wrap to the index pulse).
    //   - There's an implicit pulse at position 0 too (the index
    //     pulse), but MAME emits MG_F|0 which is the floppy_image
    //     time-zero marker — for our LSS model we skip the explicit
    //     index pulse (the LSS doesn't read an index line).
    //
    // POM2 storage: LSS-cycle timestamps in fluxStream[qt] (1 LSS
    // cycle = 4 ticks = 500 ns). Synthesise bitStream[qt] sized to
    // `(total_ticks + 31) / 32` cells with a 1 at every cell that
    // contains at least one flux event, so trackBitLength /
    // trackPeriod / bitAt continue to return sensible values.
    // Sub-cell precision is preserved in fluxStream and read by the
    // LSS via `getNextTransition` without going through bitStream.
    auto loadFluxTrack = [&](int qt, uint8_t fidx) -> bool {
        const size_t hdrOff = trksOff + static_cast<size_t>(fidx) * 8;
        const size_t trksEnd = trksOff + trksLen;
        if (hdrOff > trksEnd || trksEnd - hdrOff < 8) return false;
        const uint32_t startBlock = readU16LE(hdrOff + 0);
        const uint32_t trackSize  = readU32LE(hdrOff + 4);
        if (startBlock == 0 || trackSize == 0 ||
            trackSize > kMaxFluxTrackBytes) return false;
        const size_t dataOff = static_cast<size_t>(startBlock) * 512;
        if (dataOff > wozSize ||
            static_cast<size_t>(trackSize) > wozSize - dataOff) return false;

        // First pass: sum total ticks to size the synthetic bitStream.
        uint64_t totalTicks = 0;
        for (uint32_t i = 0; i < trackSize; ++i)
            totalTicks += buf[dataOff + i];
        if (totalTicks == 0) return false;

        // 1 LSS cycle = 4 ticks. Synthetic cells follow the image's
        // nominal cell width (`lssCyclesPerCell()`, honouring INFO+39
        // optimal_bit_timing) so the bit-cell view stays aligned with
        // bit-cell TRKS tracks of the same image.
        const int      cyc       = lssCyclesPerCell();
        const uint64_t periodLss = (totalTicks + 3) / 4;
        const size_t   cellCount =
            static_cast<size_t>((periodLss + cyc - 1) / cyc);
        if (cellCount == 0 || cellCount > kMaxTrackBits) return false;
        const size_t allocation = cellCount +
            static_cast<size_t>(trackSize) * sizeof(int);
        if (allocation > kMaxExpandedBytes - expandedBytes) return false;
        expandedBytes += allocation;

        auto& bits = bitStream[qt];
        auto& flux = fluxStream[qt];
        bits.assign(cellCount, 0);
        flux.clear();
        flux.reserve(trackSize);          // upper bound

        uint64_t cpos = 0;                // cumulative ticks
        for (uint32_t i = 0; i < trackSize; ++i) {
            const uint8_t step = buf[dataOff + i];
            cpos += step;
            if (step != 0xFF && i != trackSize - 1) {
                const int64_t lssCycle = static_cast<int64_t>(cpos / 4);
                flux.push_back(static_cast<int>(lssCycle));
                const size_t cell = static_cast<size_t>(lssCycle / cyc);
                if (cell < bits.size()) bits[cell] = 1;
            }
        }
        bitStreamValid[qt]  = true;
        fluxStreamValid[qt] = true;        // populated directly, skip expand
        // Record the TRUE revolution period — the tick-delta sum, the
        // same total MAME accumulates for a flux track (`as_dsk.cpp:
        // 61-81`). `trackPeriod` must serve this rather than
        // `cellCount × cyc`: the ceil-rounded synthetic cell count
        // multiplied back out overshoots by up to `cyc-1` LSS cycles,
        // and for optimal_bit_timing ≠ 32 the old 8-cycle hard-coding
        // was off by the whole obt/32 ratio — either way the flux
        // timeline slipped against the angular wrap every revolution.
        fluxQtPeriodLss[qt] = static_cast<int>(periodLss);
        return true;
    };

    // Walk all 160 TMAP entries (= every quarter-track). For each
    // non-FF slot, unpack the matching TRK chunk into bitStream[qt].
    // FLUX takes precedence over TMAP when both are present (matches
    // MAME `as_dsk.cpp:316-326`). This is the change from the
    // original "whole-tracks-only" port that walked `tmap[t*4]` for
    // t in 0..34 and lost the inter-track protection data carried
    // at qt%4 != 0 by copy-protected disks.
    int populatedSlots = 0;
    int populatedWholeTracks = 0;
    int populatedFluxSlots = 0;
    for (int qt = 0; qt < kQuarterTracks; ++qt) {
        // FLUX path: highest precedence for v2.1+ images.
        if (haveFlux && fluxFidx[qt] != 0xFF) {
            if (loadFluxTrack(qt, fluxFidx[qt])) {
                ++populatedSlots;
                if ((qt & 3) == 0) ++populatedWholeTracks;
                ++populatedFluxSlots;
                continue;
            }
            // fall through to bit-cell stream if flux parse fails
        }
        const uint8_t trkIdx = tmap[qt];
        if (trkIdx == 0xFF) continue;     // quarter-track absent

        size_t bitDataOff   = 0;
        size_t bitDataBytes = 0;
        size_t bitCount     = 0;
        if (isWoz1) {
            // WOZ1 fixed-slot layout. Each TRK is exactly 6656 bytes.
            const size_t slotOff = trksOff + static_cast<size_t>(trkIdx) * 6656;
            const size_t trksEnd = trksOff + trksLen;
            if (slotOff > trksEnd || trksEnd - slotOff < 6656) continue;
            bitDataOff   = slotOff;
            bitDataBytes = 6646;
            bitCount     = readU16LE(slotOff + 6648);
        } else {
            // WOZ2: 8-byte TRK headers at the start of TRKS, data at
            // file-absolute block offsets.
            const size_t hdrOff = trksOff + static_cast<size_t>(trkIdx) * 8;
            const size_t trksEnd = trksOff + trksLen;
            if (hdrOff > trksEnd || trksEnd - hdrOff < 8) continue;
            const uint32_t startBlock = readU16LE(hdrOff + 0);
            const uint32_t blockCount = readU16LE(hdrOff + 2);
            const uint32_t bc         = readU32LE(hdrOff + 4);
            if (startBlock == 0 || blockCount == 0 || bc == 0) continue;
            bitDataOff   = static_cast<size_t>(startBlock) * 512;
            bitDataBytes = static_cast<size_t>(blockCount) * 512;
            bitCount     = bc;
        }
        // kMaxTrackBits: real 5.25" tracks hold ~50k bits (WOZ1 slots cap
        // at 53,168); 1 Mi-bit is far above any legitimate mastering while
        // stopping a hostile TRKS header from driving a huge expansion
        // (bitStream stores one BYTE per bit). expandedBytes caps the
        // aggregate across bit-cell and FLUX slots — duplicate map entries
        // could otherwise multiply even a legal track.
        if (bitCount == 0
            || bitCount > kMaxTrackBits
            || bitCount > bitDataBytes * 8
            || bitDataOff > wozSize || bitDataBytes > wozSize - bitDataOff) {
            // Defensive: skip malformed track rather than aborting load.
            continue;
        }
        if (bitCount > kMaxExpandedBytes - expandedBytes) {
            lastError = "WOZ TRKS data implausibly large (hostile header?)";
            loaded = false; return false;
        }
        expandedBytes += bitCount;

        auto& bits = bitStream[qt];
        bits.resize(bitCount);
        for (size_t b = 0; b < bitCount; ++b) {
            const size_t byteIdx   = b / 8;
            const int    bitInByte = 7 - static_cast<int>(b % 8);
            bits[b] = static_cast<uint8_t>(
                (buf[bitDataOff + byteIdx] >> bitInByte) & 1);
        }
        bitStreamValid[qt] = true;
        // Record the file offset / capacity / bit count for the
        // write-back path: saveDirty() re-packs the live bitStream[qt]
        // back into `wozRaw` at this exact location.
        wozQtByteOff[qt]  = bitDataOff;
        wozQtByteLen[qt]  = bitDataBytes;
        wozQtBitCount[qt] = bitCount;
        ++populatedSlots;
        if ((qt & 3) == 0) ++populatedWholeTracks;
    }

    if (populatedWholeTracks == 0) {
        lastError = "WOZ file has no usable whole tracks";
        loaded = false; return false;
    }

    // 13-sector routing. INFO+38 (v2) is authoritative; WOZ1 and v2
    // images with boot_sector_format 0 ("unknown") fall back to sniffing
    // track 0's bit stream for the 5-and-3 address prologue D5 AA B5 vs
    // the 6-and-2 D5 AA 96 — same spirit as the byte-level .nib sniff in
    // loadFile, but bit-aligned since WOZ tracks need not start on a
    // nibble boundary. DiskIICard keys serving13_/boot-PROM choice on
    // is13Sector(), which loadWoz previously never set.
    if (boot13) {
        sectorsPerTrack_ = kSectorsPerTrack13;
    } else if (bitStreamValid[0]) {
        static const uint8_t kPro13[24] = {1,1,0,1,0,1,0,1,   // D5
                                           1,0,1,0,1,0,1,0,   // AA
                                           1,0,1,1,0,1,0,1};  // B5
        static const uint8_t kPro16[24] = {1,1,0,1,0,1,0,1,   // D5
                                           1,0,1,0,1,0,1,0,   // AA
                                           1,0,0,1,0,1,1,0};  // 96
        const auto& bits = bitStream[0];
        bool found13 = false, found16 = false;
        for (size_t i = 0; i + 24 <= bits.size() && !(found13 || found16);
             ++i) {
            bool m13 = true, m16 = true;
            for (int k = 0; k < 24 && (m13 || m16); ++k) {
                if (bits[i + static_cast<size_t>(k)] != kPro13[k]) m13 = false;
                if (bits[i + static_cast<size_t>(k)] != kPro16[k]) m16 = false;
            }
            found13 = m13;
            found16 = m16;
        }
        if (found13 && !found16) sectorsPerTrack_ = kSectorsPerTrack13;
    }

    path        = imgPath;
    loaded      = true;
    nibFormat   = false;
    cnib2Format = false;
    wozFormat   = true;
    twoImgFormat = false;
    twoImgHeaderRaw.clear();
    twoImgTrailerRaw.clear();
    sectorOrder = SectorOrder::Dos33;     // not meaningful for .woz
    lastError.clear();
    // Move the entire WOZ file bytes into wozRaw — saveDirty() will
    // splice modified bitStream[qt] back into these bytes and rewrite
    // the file in one shot. `buf` is no longer needed after this.
    wozRaw = std::move(buf);
    // POM2 can read WOZ 2.1 FLUX tracks but cannot serialise their delta
    // streams yet. Mark the whole medium physically WP when any such track
    // is active; accepting writes and later skipping byteLen==0 reported a
    // successful save while discarding every modification on that track.
    if (populatedFluxSlots > 0) fileWriteProtected = true;
    // fileWriteProtected is folded into isWriteProtected(); WOZ now
    // participates in the same writeBackEnabled / fileWriteProtected
    // gate as .dsk/.nib (the `wozFormat ||` blanket was removed when
    // write-back landed).
    pom2::log().info("Disk II",
        std::string("Loaded ") + imgPath + " (.woz "
        + (isWoz2 ? "v2" : "v1")
        + ", info_v" + std::to_string(infoVersion)
        + ", " + std::to_string(populatedWholeTracks) + " tracks"
        + (populatedSlots > populatedWholeTracks
              ? " + " + std::to_string(populatedSlots - populatedWholeTracks)
                + " quarter-track slots"
              : "")
        + (populatedFluxSlots > 0
              ? " (" + std::to_string(populatedFluxSlots) + " FLUX)"
              : "")
        + (fileWriteProtected ? ", file-WP" : "")
        + ")");
    return true;
}

void DiskImage::eject()
{
    loaded = false;
    nibFormat = false;
    cnib2Format = false;
    wozFormat = false;
    twoImgFormat = false;
    twoImgHeaderRaw.clear();
    twoImgTrailerRaw.clear();
    fileWriteProtected = false;
    optimalBitTiming = 32;   // WOZ2 INFO value must not leak into the next image
    path.clear();
    // Half-framed nibble / destination slot from the ejected medium must not
    // continue into the next one (see writeFlux).
    writeFraming.fill(WriteFraming{});
    for (auto& t : tracks) t.fill(0xFF);
    dirty.fill(false);
    anyDirty = false;
    invalidateAllBitStreams();
    wozRaw.clear();
    wozQtByteOff.fill(0);
    wozQtByteLen.fill(0);
    wozQtBitCount.fill(0);
    wozQtDirty.fill(false);
}

void DiskImage::writeNibbleAt(int track, int index, uint8_t value)
{
    if (!loaded || track < 0 || track >= kTracks) return;
    if (fileWriteProtected) return;   // physical WP inhibits the write current
    const int n = ((index % kNibblesPerTrack) + kNibblesPerTrack) % kNibblesPerTrack;
    if (tracks[track][n] != value) {
        tracks[track][n] = value;
        dirty[track]     = true;
        anyDirty         = true;
        // Bit-cell cache for the whole track is now stale; next bitAt()
        // call rebuilds it from the new nibble buffer. Non-WOZ formats
        // only ever populate the slot at `qt = track*4`, so a single
        // invalidate covers all four aliased quarter-track positions.
        invalidateWholeTrack(track);
    }
}

// ── Media snapshot (rewind) ──────────────────────────────────────────────
void DiskImage::appendMediaSnapshot(std::vector<uint8_t>& out) const
{
    out.reserve(out.size() + kMediaSnapshotBytes);
    for (int t = 0; t < kTracks; ++t)
        out.insert(out.end(), tracks[t].begin(), tracks[t].end());
    for (int t = 0; t < kTracks; ++t)
        out.push_back(static_cast<uint8_t>(dirty[t] ? 1 : 0));
}

void DiskImage::loadMediaSnapshot(const uint8_t* data, std::size_t len)
{
    if (len < kMediaSnapshotBytes) return;
    std::size_t p = 0;
    for (int t = 0; t < kTracks; ++t) {
        std::memcpy(tracks[t].data(), data + p, kNibblesPerTrack);
        p += kNibblesPerTrack;
    }
    anyDirty = false;
    for (int t = 0; t < kTracks; ++t) {
        dirty[t] = data[p++] != 0;
        if (dirty[t]) anyDirty = true;
    }
    // Reads re-derive the bit/flux streams from the restored nibble buffers.
    invalidateAllBitStreams();
}

// ── LSS bit-cell stream expansion ────────────────────────────────────────
//
// Walks the 6656-byte nibble buffer once and emits the LSS-shaped bit
// stream into bitStream[track]. Sync $FF runs get 2 zero cells appended
// per byte so the byte boundary drifts +2 bits across each gap — that's
// the timing artefact real Disk II software uses to recover sync after
// the head crosses a track boundary or the controller drops alignment.
void DiskImage::computeCellWidths(int track, uint8_t* widths) const
{
    std::fill_n(widths, kNibblesPerTrack, uint8_t{8});
    if (nibFormat) return;          // .nib: raw nibbles, no sync semantics
    const auto& buf = tracks[track];

    // Circular maximal-run scan: walk once from just past a non-$FF
    // anchor; every maximal $FF run is then seen contiguously (including
    // the one wrapping the index seam), and members of runs ≥ kSyncMinRun
    // get the 10-cell width.
    int anchor = -1;
    for (int i = 0; i < kNibblesPerTrack; ++i) {
        if (buf[i] != 0xFF) { anchor = i; break; }
    }
    if (anchor < 0) {               // whole track is one giant sync run
        std::fill_n(widths, kNibblesPerTrack, uint8_t{10});
        return;
    }
    int runLen = 0, runBegin = 0;
    for (int k = 1; k <= kNibblesPerTrack; ++k) {
        const int idx = (anchor + k) % kNibblesPerTrack;
        if (buf[idx] == 0xFF) {
            if (runLen == 0) runBegin = idx;
            ++runLen;
        } else {
            if (runLen >= kSyncMinRun) {
                for (int j = 0; j < runLen; ++j)
                    widths[(runBegin + j) % kNibblesPerTrack] = 10;
            }
            runLen = 0;
        }
    }
    // The walk ends back on the non-$FF anchor, so the last run (if any)
    // was flushed by the loop's else branch.
}

void DiskImage::expandTrackBits(int qt) const
{
    if (qt < 0 || qt >= kQuarterTracks) return;
    const int slot = qtSlot(qt);
    // For WOZ images, slot == qt and the bit data was already populated
    // by `loadWoz`; `bitStreamValid[slot]` is true so callers short-
    // circuit before reaching here. Defensive bail in case of a refactor.
    if (wozFormat) {
        bitStreamValid[slot] = true;
        return;
    }
    auto& bits = bitStream[slot];
    bits.clear();
    bits.reserve(static_cast<size_t>(kNibblesPerTrack) * 9);

    // Non-WOZ: source from the whole-track nibble buffer that contains
    // this quarter-track position.
    const int wholeTrack = slot / 4;
    if (wholeTrack < 0 || wholeTrack >= kTracks) {
        bitStreamValid[slot] = true;     // empty
        return;
    }
    const auto& buf = tracks[wholeTrack];

    // Detect whether nibble[i] is part of a SYNC GAP $FF run vs an
    // in-field $FF that happens to be encoded as the on-disk byte $FF.
    //
    // The previous "any run of ≥2" rule was unsafe: the 4-and-4 address
    // field encodes its checksum as two consecutive disk $FF bytes
    // whenever `vol ^ track ^ sector == $FF`, and `mario.dsk` SHAMUS
    // (boot path) hits the same kind of in-field $FF-pair in its data
    // field via running-XOR coincidence. Inserting +2 zero cells per
    // byte for those pairs shifted the LSS bit stream out of phase
    // partway through a data field — visible as Shamus failing to boot
    // and as the cc65-Chess.po partial truncation pattern.
    //
    // `nibblizeTrack` always lays down ≥5-byte $FF gaps (5 between
    // address/data fields, 14 between sectors, and a long $FF tail
    // that wraps into the next-revolution leader). The run rule itself
    // (kSyncMinRun, tight against the encoder so naturally occurring
    // 2-3 byte in-field $FF runs don't get treated as sync) lives in
    // computeCellWidths — the SAME timeline writeFlux re-packs against.
    uint8_t widths[kNibblesPerTrack];
    computeCellWidths(wholeTrack, widths);

    for (int i = 0; i < kNibblesPerTrack; ++i) {
        const uint8_t b = buf[i];
        // 8 data cells, MSB-first.
        for (int bit = 7; bit >= 0; --bit) {
            bits.push_back(static_cast<uint8_t>((b >> bit) & 1));
        }
        // Sync padding: 2 trailing zero cells for $FF in a run.
        if (widths[i] == 10) {
            bits.push_back(0);
            bits.push_back(0);
        }
    }
    bitStreamValid[slot] = true;
}

int DiskImage::trackBitLength(int qt) const
{
    if (qt < 0 || qt >= kQuarterTracks) return 0;
    const int slot = qtSlot(qt);
    if (!bitStreamValid[slot]) expandTrackBits(qt);
    return static_cast<int>(bitStream[slot].size());
}

uint8_t DiskImage::bitAt(int qt, int bitIdx) const
{
    if (qt < 0 || qt >= kQuarterTracks) return 0;
    const int slot = qtSlot(qt);
    if (!bitStreamValid[slot]) expandTrackBits(qt);
    const auto& bits = bitStream[slot];
    if (bits.empty()) return 0;
    const int n = static_cast<int>(bits.size());
    return bits[((bitIdx % n) + n) % n];
}

// ── MAME flux event view ───────────────────────────────────────────────
//
// Verbatim port of `floppy_image_device::cache_fill` data layout, adapted
// to LSS cycles. For each "1" bit cell at index k in the bit-cell stream,
// we emit one flux event at LSS cycle `k * cyc + cyc/2` (cell center).
// For "0" cells we emit nothing — the LSS sees those as the gap between
// events, which is exactly MAME's continuous-flux model: flux changes
// are point events; absence of an event over a `cyc`-LSS-cycle window
// means a "0".
//
// `cyc` = LSS cycles per bit cell. Default 8 (= 4 µs at 2 MHz LSS clock,
// matching the standard 5.25" cell). WOZ2's optimal_bit_timing INFO
// field overrides this on a per-image basis: `cyc = optimalBitTiming /
// 4` (since optimal_bit_timing is in 125 ns units and 1 LSS cycle =
// 500 ns). Non-WOZ formats and WOZ1 keep the 32→8 default.
//
// Cells per byte mirrors `expandTrackBits` (8 cells/byte; sync $FF runs
// pad +2 zero cells per byte). So the total period in LSS cycles equals
// `bitStream.size() * cyc`, and PULSE timing under the flux model
// matches PULSE timing the bit-cell view would produce — by construction.
int DiskImage::lssCyclesPerCell() const
{
    // Integer division is exact when optimal_bit_timing is a multiple of 4
    // — universally the case in real-world WOZ images (Applesauce always
    // emits multiples of 4). Truncation on rare odd values still yields
    // a sane cell window; clamp to >= 1 so a degenerate INFO can't divide-
    // by-zero downstream.
    const int cyc = optimalBitTiming / 4;
    return cyc >= 1 ? cyc : 8;
}

void DiskImage::expandTrackFlux(int qt) const
{
    if (qt < 0 || qt >= kQuarterTracks) return;
    const int slot = qtSlot(qt);
    auto& flux = fluxStream[slot];
    flux.clear();
    if (!bitStreamValid[slot]) expandTrackBits(qt);
    const auto& bits = bitStream[slot];
    flux.reserve(bits.size() / 2);            // ~half the cells are 1
    const int cyc    = lssCyclesPerCell();
    const int centre = cyc / 2;
    for (int i = 0; i < static_cast<int>(bits.size()); ++i) {
        if (bits[i]) flux.push_back(i * cyc + centre);
    }
    fluxStreamValid[slot] = true;
}

int DiskImage::trackPeriod(int qt) const
{
    if (qt >= 0 && qt < kQuarterTracks) {
        // WOZ FLUX-chunk quarter-tracks carry their true (tick-sum)
        // period; the synthetic bit-cell product below would overshoot.
        const int fluxPeriod = fluxQtPeriodLss[qtSlot(qt)];
        if (fluxPeriod > 0) return fluxPeriod;
    }
    return trackBitLength(qt) * lssCyclesPerCell();
}

const std::vector<int>& DiskImage::fluxEvents(int qt) const
{
    static const std::vector<int> empty;
    if (qt < 0 || qt >= kQuarterTracks) return empty;
    const int slot = qtSlot(qt);
    if (!fluxStreamValid[slot]) expandTrackFlux(qt);
    return fluxStream[slot];
}

// MAME `floppy_image_device::get_next_transition` — returns the time of
// the next flux event at or after `fromLssCycle`. If none in the current
// revolution, wraps and returns the first event of the next revolution
// (offset by one period, so the result remains ≥ fromLssCycle). Only
// returns kFluxNever when the track has no flux events at all (blank
// disk), matching MAME's `attotime::never` for the empty-track case.
//
// `revolutionStart` anchors the disk's angular position. MAME
// `floppy_image_device::find_position(base, when)`:
//
//     base = m_revolution_start_time;
//     delta = when - base;
//     while (delta >= m_rev_time) { delta -= m_rev_time; base += m_rev_time; }
//     cell_idx = delta * m_angular_speed
//
// The POM2 equivalent expressed in LSS cycles is:
//
//     cell_offset = (fromLssCycle - revolutionStart) mod track_period
//     result      = revolutionStart + (#full_revolutions * track_period)
//                                   + cell_offset_of_next_event
//
// When `revolutionStart < 0` we fall back to the pre-anchor behaviour
// (`fromLssCycle mod track_period`) — used during boot before any
// motor-on (`mon_w(false)`) transition anchors the drive.
int64_t DiskImage::getNextTransition(int qt, int64_t fromLssCycle,
                                     int64_t revolutionStart) const
{
    if (qt < 0 || qt >= kQuarterTracks) return kFluxNever;
    const auto& flux = fluxEvents(qt);
    if (flux.empty()) return kFluxNever;
    const int period = trackPeriod(qt);
    if (period <= 0) return kFluxNever;

    // If the caller anchored the drive's revolution, shift fromLssCycle
    // into "angular-offset" space before the modulo. This matches
    // MAME's `find_position` walk: events that lie at angular position
    // P always sit at `revolutionStart + N*period + P`, regardless of
    // what `cycles` was when the controller last spun this drive up.
    //
    // Why this matters: the previous "lssCycle mod period" branch made
    // the disk's angular position depend on the *absolute* controller
    // clock at motor-on time. Two drives spinning at different start
    // times (or the same drive after a track step into a buffer with a
    // slightly different period) would land at unrelated angular slots
    // even though the head's physical position hadn't changed enough
    // to justify the jump.
    const bool anchored = (revolutionStart >= 0);
    const int64_t origin = anchored ? revolutionStart : int64_t{0};
    const int64_t delta  = fromLssCycle - origin;
    // Truncate toward minus infinity so the cell offset is always in
    // [0, period). Mirrors MAME's `while (delta < attotime::zero)` /
    // `while (delta >= m_rev_time)` walk. For typical operation
    // `delta >= 0` so this collapses to a single division.
    int64_t fullRevs = delta / period;
    int64_t cellOff  = delta - fullRevs * period;
    if (cellOff < 0) { cellOff += period; --fullRevs; }
    const int pos = static_cast<int>(cellOff);

    // ── lower_bound, resumed from the previous answer ────────────────────
    // `idx` must end up as the true std::lower_bound(flux, pos), i.e. the
    // smallest index with flux[idx] >= pos. The cursor only changes HOW it
    // is found (see the lbHint_ comment in the header): the hint is
    // *verified* against the lower-bound predicate, so a stale one costs a
    // failed test and nothing else. Output is identical by construction —
    // pinned by diskii_lss_smoke and disk_boot_smoke.
    const size_t n = flux.size();
    const int*   f = flux.data();
    size_t idx;
    size_t hint = (qt == lbHintQt_) ? lbHintIdx_ : 0;
    if (hint > n) hint = n;                       // array may have shrunk
    if ((hint == 0 || f[hint - 1] < pos) && (hint == n || f[hint] >= pos)) {
        idx = hint;                               // hit: 2 comparisons
    } else if (hint < n && f[hint] < pos) {
        // The usual miss: the head advanced past a handful of events since
        // the previous call. Walk forward a bounded number of steps before
        // giving up — an unbounded walk would be worse than the binary
        // search on a long seek.
        size_t j = hint;
        const size_t stop = (n - j) < 8 ? n : j + 8;
        while (j < stop && f[j] < pos) ++j;
        // f[j-1] < pos holds by construction, so j is the lower bound as
        // soon as it stops on an event at or past `pos`.
        idx = (j < n && f[j] >= pos)
                  ? j
                  : static_cast<size_t>(
                        std::lower_bound(flux.begin(), flux.end(), pos)
                        - flux.begin());
    } else {
        // Backwards jump (revolution wrap, track step, seek): full search.
        idx = static_cast<size_t>(
            std::lower_bound(flux.begin(), flux.end(), pos) - flux.begin());
    }
    lbHintQt_  = qt;
    lbHintIdx_ = idx;

    if (idx == n) {
        // Wrap to next revolution.
        return origin + (fullRevs + 1) * period + flux.front();
    }
    return origin + fullRevs * period + f[idx];
}

// MAME `floppy_image_device::write_flux` — splice `count` flux events
// (LSS-cycle timestamps in `transitions`) into the track over the LSS-
// cycle range `[startLssCycle, endLssCycle)`. The operation is
// idempotent: any prior flux events in that range are replaced.
//
// POM2 stores tracks as nibbles, not as a flux array, so the splice is
// realised by:
//   1. Computing the cell-index range covered by [start, end) (each cell
//      is 8 LSS cycles wide).
//   2. For each cell window, deciding bit = 1 iff any of the supplied
//      transitions falls strictly inside the cell's interior window
//      [k*8 + 1, k*8 + 7) — matches MAME's PULSE-detect window when the
//      LSS later reads back this region.
//   3. Re-packing 8 consecutive cells per byte into the nibble buffer at
//      the byte index `cell_start_byte = startCell / 8`. Cells past the
//      first byte boundary that don't fit a full byte are deferred until
//      the next splice — which mirrors how the LSS write side flushes in
//      32-event chunks (≥ ~1 byte each).
//
// The cell→nibble re-pack maps written cells onto nibbles through the
// PADDED cell timeline of the existing track (8 data cells per nibble,
// +2 trailing zero cells per sync $FF — the exact expansion
// `expandTrackBits` used to build the flux view whose angular positions
// the incoming transitions are expressed in). Sync $FFs written by DOS
// at 40-cycle pacing therefore land on 10-cell-wide gap slots and data
// nibbles written at 32-cycle pacing land on 8-cell-wide field slots,
// both staying in lock-step with the surface layout.
//
// `revolutionStart` mirrors MAME: `floppy_image_device::write_flux`
// maps start / end / every transition through `find_position(base,
// when)`, anchored on `m_revolution_start_time` (MAME
// `imagedev/floppy.cpp` write_flux ~:1050-1095, find_position
// ~:1100-1125) — the same anchor `get_next_transition` uses on the read
// side. The angular reduction here must subtract the same anchor before
// the modulo, or a drive whose revolution anchor isn't a multiple of
// the period writes at an angular offset the read path will never
// report (`< 0` = unanchored fallback, matching getNextTransition).
void DiskImage::writeFlux(int qt, int64_t startLssCycle, int64_t endLssCycle,
                          int count, const int64_t* transitions,
                          int64_t revolutionStart)
{
    if (!loaded || qt < 0 || qt >= kQuarterTracks) return;
    if (endLssCycle <= startLssCycle) return;
    // A PHYSICALLY write-protected medium (WOZ INFO+2 / 2IMG WP flag) inhibits
    // the write current on real hardware, so the bit-cell buffer must never be
    // mutated even if software ignores the $C0nD WP sense and writes anyway —
    // otherwise the user's source file is corrupted on the next saveDirty().
    // (The write-back TOGGLE is still honoured separately via saveDirty(); a
    // non-fileWriteProtected image can still splice in-memory for unit tests.)
    if (fileWriteProtected) return;
    if (wozFormat) {
        // WOZ canonical storage = bitStream[qt]. The flux→bit-cell
        // conversion is the same cell-window logic as the non-WOZ path
        // (PULSE timestamp / 8 → cell index), but we splice straight
        // into bitStream rather than re-deriving the whole track from
        // a nibble buffer. saveDirty() later re-packs the live bits
        // back into wozRaw at wozQtByteOff[qt].
        if (wozQtBitCount[qt] == 0) return;        // unpopulated qt
        const int period = trackPeriod(qt);
        if (period <= 0) return;

        // Reduce to angular offset through the revolution anchor —
        // MAME `find_position`: `(when - m_revolution_start_time) mod
        // m_rev_time`. The read path (`getNextTransition`) applies the
        // identical reduction, so spliced cells land where reads will
        // look for them.
        const int64_t origin = (revolutionStart >= 0) ? revolutionStart
                                                      : int64_t{0};
        int64_t startMod = (((startLssCycle - origin) % period) + period)
                           % period;
        int64_t endMod   = startMod + (endLssCycle - startLssCycle);
        // Handle revolution wrap by recursing on the two halves —
        // mirrors the non-WOZ split below.
        if (endMod > period) {
            std::vector<int64_t> firstHalf, secondHalf;
            firstHalf.reserve(count);
            secondHalf.reserve(count);
            const int64_t origBase = startLssCycle - startMod;
            const int64_t splitAt  = origBase + period;
            for (int i = 0; i < count; ++i) {
                if (transitions[i] < splitAt) firstHalf.push_back(transitions[i]);
                else                          secondHalf.push_back(transitions[i]);
            }
            writeFlux(qt, startLssCycle, splitAt,
                      static_cast<int>(firstHalf.size()), firstHalf.data(),
                      revolutionStart);
            writeFlux(qt, splitAt, endLssCycle,
                      static_cast<int>(secondHalf.size()), secondHalf.data(),
                      revolutionStart);
            return;
        }

        // Bit-cell width in LSS cycles. MUST match the read path
        // (expandTrackFlux: cell i is centred at `i*lssCyclesPerCell()`),
        // else a write-back to a WOZ2 image whose optimal_bit_timing != 32
        // (cyc != 8) scatters transitions into the wrong cells and
        // corrupts the track. Previously hard-coded to 8.
        const int cyc       = lssCyclesPerCell();
        const int firstCell = static_cast<int>(startMod / cyc);
        const int lastCell  = static_cast<int>((endMod + cyc - 1) / cyc);
        const int spanCells = lastCell - firstCell;
        std::vector<bool> newBits(spanCells, false);
        const int64_t origBase = startLssCycle - startMod;
        for (int i = 0; i < count; ++i) {
            const int64_t t = ((transitions[i] - origBase) % period + period) % period;
            const int cell  = static_cast<int>(t / cyc) - firstCell;
            if (cell >= 0 && cell < spanCells) newBits[cell] = true;
        }

        // Splice cell-by-cell into bitStream[qt]. The LSS writes a full
        // cell at a time (every PULSE/no-PULSE event is one cell), so
        // both 1s and 0s in `newBits` overwrite whatever was there
        // before. Partial cells at the edges are dropped — same policy
        // as the non-WOZ branch below (which drops partial bytes for
        // the same reason).
        auto& bits = bitStream[qt];
        const int bitCount = static_cast<int>(wozQtBitCount[qt]);
        bool changed = false;
        for (int c = 0; c < spanCells; ++c) {
            // Seam rule: the window's first/last cells may be only
            // PARTIALLY covered (startMod/endMod mid-cell) — routine
            // when DiskIICard flushes a long write in ~30-transition
            // chunks. A captured transition in a partial cell is
            // authoritative (sets the bit); the absence of one is NOT
            // (the adjacent chunk owns the rest of that cell) — clearing
            // here would erase the bit the previous chunk just spliced.
            const bool partial =
                (c == 0 && (startMod % cyc) != 0) ||
                (c == spanCells - 1 && (endMod % cyc) != 0);
            if (partial && !newBits[c]) continue;
            const int dst = ((firstCell + c) % bitCount + bitCount) % bitCount;
            const uint8_t v = newBits[c] ? 1 : 0;
            if (bits[dst] != v) { bits[dst] = v; changed = true; }
        }
        if (changed) {
            wozQtDirty[qt] = true;
            anyDirty       = true;
            // Flux cache for this qt is now stale — drop it. Do NOT
            // call invalidateBitStream(qt): that would clear the bit
            // stream we just edited.
            fluxStreamValid[qt] = false;
            fluxStream[qt].clear();
        }
        return;
    }
    // Non-WOZ: write-back lands on the whole track containing `qt`.
    // Quarter-track sub-positions on standard .dsk/.do/.po share their
    // nibble buffer with the parent whole track (`qtSlot` alias).
    const int track = qt / 4;
    if (track < 0 || track >= kTracks) return;

    const int period = trackPeriod(qt);
    if (period <= 0) return;

    // Cell width in LSS cycles — same accessor as the read/WOZ paths so
    // the two stay consistent (non-WOZ images keep the 32-default → 8).
    const int cyc = lssCyclesPerCell();

    // The cell grid for a WRITE is the WRITE's own clock, not the track's
    // revolution phase. The head emits one cell every `cyc` LSS cycles from
    // the moment write mode came on, so transitions from one burst are
    // always an exact multiple of `cyc` apart. Quantising them against the
    // revolution anchor instead (`(t - revolutionStart) mod period / cyc`)
    // put that grid at an arbitrary sub-cell phase: adjacent transitions
    // rounded into the SAME cell, one of the two was dropped, the nibble
    // lost a bit and the framing below slipped — a written data field came
    // back as garbage and DOS 3.3 answered I/O ERROR. The revolution anchor
    // is still what says WHERE on the track the burst starts, but it is
    // consulted once, when the burst is anchored to a nibble (below), not
    // per transition.
    WriteFraming& fr = writeFraming[track];
    const bool continues = (fr.nextCycle == startLssCycle && fr.nibbleIdx >= 0);
    if (!continues) fr.origin = startLssCycle;

    const int64_t firstCell64 = (startLssCycle - fr.origin) / cyc;
    const int64_t lastCell64  = (endLssCycle - fr.origin + cyc - 1) / cyc;
    if (lastCell64 <= firstCell64) return;
    const int firstCell = static_cast<int>(firstCell64);
    const int lastCell  = static_cast<int>(lastCell64);

    // Angular position of the burst start, used only to pick the nibble the
    // head is over when a NEW burst begins (MAME `find_position`).
    const int64_t anchorOrigin = (revolutionStart >= 0) ? revolutionStart
                                                        : int64_t{0};
    const int64_t startMod =
        (((startLssCycle - anchorOrigin) % period) + period) % period;

    // One bit per cell of this window, off the write clock.
    std::vector<bool> newBits(static_cast<size_t>(lastCell - firstCell), false);
    for (int i = 0; i < count; ++i) {
        const int64_t cell = (transitions[i] - fr.origin) / cyc - firstCell64;
        if (cell >= 0 && cell < static_cast<int64_t>(newBits.size())) {
            newBits[static_cast<size_t>(cell)] = true;
        }
    }

    // FRAME the written cells into nibbles the way the read sequencer does
    // — do NOT try to align them to the nibble grid of the track that was
    // there before. That was the old approach: walk the existing track's
    // PADDED cell timeline (8 data cells per nibble, +2 for each $FF in a
    // sync run of ≥ kSyncMinRun) and overwrite the nibbles the window
    // covers. It is only correct while the new content pads exactly like
    // the old one did, and a sector write never does: DOS lays its own
    // sync run down (a 40-cycle write loop = a 10-cell $FF) wherever it
    // likes, so from the first sync byte on, the old grid and the new
    // stream disagree and every subsequent nibble is assembled from cells
    // belonging to its neighbours. Rewriting a track with its own contents
    // was idempotent — which is why this survived so long — but writing an
    // actual data field mangled ~345 of its 353 nibbles and spilled into
    // the next sector's address field. DOS 3.3 answered I/O ERROR and the
    // track was unreadable from then on (CATALOG failed too).
    //
    // The head has no grid: it writes a continuous bit stream and the
    // reader self-syncs on it — skip 0-cells until a 1, then that 1 plus
    // the next seven cells are the nibble (the two 0-cells trailing a sync
    // $FF are simply skipped, which is exactly what makes them sync). So
    // frame the incoming cells and lay the resulting nibbles down
    // sequentially from wherever the head is. `writeFraming[track]` carries
    // the shift accumulator across flushes, because DiskIICard flushes
    // every ~30 transitions and a nibble straddles chunks routinely.
    // MAME stores flux natively (`imagedev/floppy.cpp` write_flux
    // ~:1050-1095) and needs no such step; this is POM2's nibble-store
    // equivalent of the same physics.
    auto& buf = tracks[track];

    // A window that doesn't continue the previous one starts a new burst:
    // re-anchor on the nibble the head is over — that lookup is ANGULAR, so
    // it uses the revolution-relative cell index, not the write-clock one.
    if (!continues) {
        const int angularCell = static_cast<int>(startMod / cyc);
        uint8_t cellWidth[kNibblesPerTrack];
        computeCellWidths(track, cellWidth);
        int cellStart = 0;
        int anchor    = 0;
        for (int n = 0; n < kNibblesPerTrack; ++n) {
            if (angularCell < cellStart + cellWidth[n]) {
                // Mid-nibble splice: the nibble the head is part-way
                // through keeps its old value (a real write splice leaves
                // exactly that stub on the medium) and the new stream is
                // framed into the following slot. Anchoring ON the straddled
                // nibble instead shifted the whole write one slot early.
                anchor = (cellStart == angularCell) ? n : n + 1;
                break;
            }
            cellStart += cellWidth[n];
        }
        fr.nibbleIdx  = anchor % kNibblesPerTrack;
        fr.acc        = 0;
        fr.accBits    = 0;
        fr.nextCell   = firstCell;
        fr.heldValid  = false;
    }

    // The held cell from the previous flush is this window's first cell —
    // merge the bit it carried, then frame it normally.
    if (fr.heldValid && fr.heldCell == firstCell && fr.heldBit && !newBits.empty())
        newBits[0] = true;
    fr.heldValid = false;

    // Frame only cells this burst hasn't consumed yet. `nextCell` outside
    // the window means the write wrapped the revolution boundary (writeFlux
    // recurses on the two halves, contiguous in time but not in cell index)
    // — re-sync on the new window instead of skipping it whole.
    int from = firstCell;
    if (continues && fr.nextCell >= firstCell && fr.nextCell <= lastCell)
        from = fr.nextCell;
    int to = lastCell;                       // exclusive
    // Partial-cell test on the WRITE-CLOCK grid — the same grid firstCell/
    // lastCell were computed on. Testing the revolution-anchor phase here
    // (`endMod % cyc`) held back a COMPLETE cell whenever the two grids
    // disagreed mod cyc (anchor is latched at motor-on, origin at Q7-on, so
    // they only agree by luck), and the next flush seam then discarded it —
    // one bit lost per ~30-transition chunk shredded the framed nibbles.
    if (((endLssCycle - fr.origin) % cyc) != 0 && to > from) {
        --to;
        fr.heldValid = true;
        fr.heldCell  = to;
        fr.heldBit   = newBits[static_cast<size_t>(to - firstCell)];
    }

    static const bool kTraceWf = std::getenv("POM2_TRACE_WRITEFLUX") != nullptr;
    const int anchorDbg = fr.nibbleIdx;
    bool changed = false;
    for (int c = from; c < to; ++c) {
        const bool bit = newBits[static_cast<size_t>(c - firstCell)];
        if (fr.accBits == 0) {
            if (!bit) continue;            // still in the sync gap
            fr.acc     = 1;                // leading 1 — ends up as bit 7
            fr.accBits = 1;
            continue;
        }
        fr.acc = static_cast<uint8_t>((fr.acc << 1) | (bit ? 1 : 0));
        if (++fr.accBits == 8) {
            if (buf[fr.nibbleIdx] != fr.acc) {
                buf[fr.nibbleIdx] = fr.acc;
                changed = true;
            }
            fr.nibbleIdx = (fr.nibbleIdx + 1) % kNibblesPerTrack;
            fr.acc       = 0;
            fr.accBits   = 0;
        }
    }
    fr.nextCell  = to;
    fr.nextCycle = endLssCycle;
    if (kTraceWf) {
        std::fprintf(stderr,
            "[WF] t%02d %s start=%lld end=%lld n=%d cells[%d,%d) "
            "anchor=%d -> nib=%d accBits=%d\n",
            track, continues ? "cont" : "NEW ",
            (long long)startLssCycle, (long long)endLssCycle, count,
            from, to, anchorDbg, fr.nibbleIdx, fr.accBits);
    }
    if (changed) {
        dirty[track]    = true;
        anyDirty        = true;
        invalidateWholeTrack(track);
    }
}

void DiskImage::nibblizeTrack(int track, const uint8_t* sectors, uint8_t volume,
                              const int* logicalForPhysical)
{
    auto& buf = tracks[track];
    buf.fill(0xFF);              // any unwritten tail stays as sync nibbles
    uint8_t* dst = buf.data();

    for (int physical = 0; physical < kSectorsPerTrack; ++physical) {
        // Sync gap before each sector. 14 × $FF is plenty for the Disk II
        // data separator to lock in (real disks vary 5-40).
        for (int i = 0; i < 14; ++i) *dst++ = 0xFF;

        writeAddressField(dst, volume, static_cast<uint8_t>(track),
                          static_cast<uint8_t>(physical));

        for (int i = 0; i < 5; ++i) *dst++ = 0xFF;

        const int logical = logicalForPhysical[physical];
        writeDataField(dst, sectors + logical * kSectorBytes);
    }
}

void DiskImage::writeAddressField(uint8_t*& dst, uint8_t volume,
                                  uint8_t track, uint8_t sector)
{
    *dst++ = 0xD5; *dst++ = 0xAA; *dst++ = 0x96;
    write4and4(dst, volume);
    write4and4(dst, track);
    write4and4(dst, sector);
    write4and4(dst, static_cast<uint8_t>(volume ^ track ^ sector));
    *dst++ = 0xDE; *dst++ = 0xAA; *dst++ = 0xEB;
}

// ── 13-sector (5-and-3) encoder — DOS 3.1/3.2/3.2.1 ──────────────────────
// Verbatim port of MAME `formats/ap2_dsk.cpp` a2_13sect_format::load.

void DiskImage::nibblizeTrack13(int track, const uint8_t* sectors, uint8_t volume)
{
    auto& buf = tracks[track];
    buf.fill(0xFF);
    uint8_t* dst = buf.data();

    // Physical position i holds sector S = (i*10)%13 (the 10:13 interleave,
    // MAME load() line ~467); the address field AND the file data are both
    // indexed by S. ~450 nibbles/sector × 13 = ~5850, fits kNibblesPerTrack.
    for (int i = 0; i < kSectorsPerTrack13; ++i) {
        const int s = (i * 10) % kSectorsPerTrack13;
        for (int k = 0; k < 14; ++k) *dst++ = 0xFF;   // sync gap
        writeAddressField13(dst, volume, static_cast<uint8_t>(track),
                            static_cast<uint8_t>(s));
        for (int k = 0; k < 5; ++k) *dst++ = 0xFF;
        writeDataField13(dst, sectors + s * kSectorBytes);
    }
}

void DiskImage::writeAddressField13(uint8_t*& dst, uint8_t volume,
                                    uint8_t track, uint8_t sector)
{
    *dst++ = 0xD5; *dst++ = 0xAA; *dst++ = 0xB5;   // 13-sector addr prologue
    write4and4(dst, volume);
    write4and4(dst, track);
    write4and4(dst, sector);
    write4and4(dst, static_cast<uint8_t>(volume ^ track ^ sector));
    *dst++ = 0xDE; *dst++ = 0xAA; *dst++ = 0xEB;
}

void DiskImage::writeDataField13(uint8_t*& dst, const uint8_t* src)
{
    *dst++ = 0xD5; *dst++ = 0xAA; *dst++ = 0xAD;   // data prologue

    uint8_t pval = 0;
    auto wr = [&](uint8_t nval) {
        nval &= 0x1F;
        *dst++ = kTranslate5[(pval ^ nval) & 0x1F];   // running-XOR through translate5
        pval = nval;
    };
    // 5-and-3 stream (MAME load lines 488-515): 154 "low" then 256 "high".
    wr(static_cast<uint8_t>(src[255] & 7));
    for (int k = 2; k >= 0; --k)
        for (int j = 0; j < 51; ++j)
            wr(static_cast<uint8_t>(
                  ((src[j * 5 + k] & 7) << 2)
                | (((src[j * 5 + 3] >> (2 - k)) & 1) << 1)
                |  ((src[j * 5 + 4] >> (2 - k)) & 1)));
    for (int k = 0; k < 5; ++k)
        for (int j = 50; j >= 0; --j)
            wr(static_cast<uint8_t>(src[j * 5 + k] >> 3));
    wr(static_cast<uint8_t>(src[255] >> 3));
    *dst++ = kTranslate5[pval & 0x1F];             // checksum nibble

    *dst++ = 0xDE; *dst++ = 0xAA; *dst++ = 0xEB;   // data epilogue
}

// ── Decoder ─────────────────────────────────────────────────────────────

bool DiskImage::decodeTrack(int track, uint8_t outSectors[kSectorsPerTrack][kSectorBytes]) const
{
    if (track < 0 || track >= kTracks) return false;
    const auto& buf = tracks[track];
    bool decodedAny = false;

    // Wrap-around scan: walk 2× the track length so a sector that
    // straddles the buffer end can be matched. Index modulo'd into buf.
    auto at = [&](int i) -> uint8_t {
        return buf[((i % kNibblesPerTrack) + kNibblesPerTrack) % kNibblesPerTrack];
    };

    int curSector = -1;
    for (int i = 0; i < 2 * kNibblesPerTrack; ++i) {
        // Address-field prologue: D5 AA 96.
        if (at(i) == 0xD5 && at(i + 1) == 0xAA && at(i + 2) == 0x96) {
            // 4-and-4 fields: vol, trk, sec, chk (8 nibbles).
            uint8_t addr[4];
            for (int k = 0; k < 4; ++k) {
                uint8_t pair[2] = { at(i + 3 + k * 2), at(i + 4 + k * 2) };
                addr[k] = decode4and4(pair);
            }
            curSector = addr[2];
            // Skip past the address field (3 + 8 + 3 epilogue = 14).
            i += 13;
            continue;
        }
        // Data-field prologue: D5 AA AD.
        if (at(i) == 0xD5 && at(i + 1) == 0xAA && at(i + 2) == 0xAD &&
            curSector >= 0 && curSector < kSectorsPerTrack) {
            // 86 secondary nibbles + 256 primary nibbles + 1 checksum.
            const int dataStart = i + 3;
            uint8_t low2[86];
            uint8_t high6[256];
            uint8_t prev = 0;
            bool ok = true;
            // The encoder's stream order is `for j=85 down to 0: write
            // low2_enc[j]`, where low2_enc[85-i] = src[i]'s packed low2.
            // After decoding disk[0..85] in stream order, decoded[j] =
            // low2_enc[85-j] = src[j]'s packed low2 bits. So our local
            // `low2[j]` (indexed by source byte slot) gets decoded[j].
            for (int j = 0; j < 86; ++j) {
                const uint8_t disk = at(dataStart + j);
                const uint8_t v6   = kGcrInverse[disk];
                if (v6 == 0xFF) { ok = false; break; }
                const uint8_t cur  = static_cast<uint8_t>(prev ^ v6);
                low2[j] = cur;
                prev = cur;
            }
            if (ok) {
                for (int j = 0; j < 256; ++j) {
                    const uint8_t disk = at(dataStart + 86 + j);
                    const uint8_t v6   = kGcrInverse[disk];
                    if (v6 == 0xFF) { ok = false; break; }
                    const uint8_t cur  = static_cast<uint8_t>(prev ^ v6);
                    high6[j] = cur;
                    prev = cur;
                }
            }
            if (ok) {
                // Checksum nibble must XOR to 0 with the running prev.
                const uint8_t chk = kGcrInverse[at(dataStart + 86 + 256)];
                if (chk == 0xFF || (prev ^ chk) != 0) ok = false;
            }
            if (ok) {
                // Recombine: byte[i] = (high6[i] << 2) | low2 bits for slot i.
                for (int b = 0; b < 256; ++b) {
                    const int slot = b % 86;
                    const uint8_t low2pack = low2[slot];
                    uint8_t lo;
                    if (b < 86)        lo = rev2(static_cast<uint8_t>( low2pack       & 3));
                    else if (b < 172)  lo = rev2(static_cast<uint8_t>((low2pack >> 2) & 3));
                    else                lo = rev2(static_cast<uint8_t>((low2pack >> 4) & 3));
                    outSectors[curSector][b] = static_cast<uint8_t>((high6[b] << 2) | lo);
                }
                decodedAny = true;
            }
            // Skip past the data field (3 + 343 + 3 epilogue = 349).
            i += 348;
            curSector = -1;
            continue;
        }
    }
    return decodedAny;
}

bool DiskImage::decodeTrack13(int track,
                              uint8_t outSectors[kSectorsPerTrack13][kSectorBytes]) const
{
    if (track < 0 || track >= kTracks) return false;
    const auto& buf = tracks[track];
    bool decodedAny = false;
    auto at = [&](int i) -> uint8_t {
        return buf[((i % kNibblesPerTrack) + kNibblesPerTrack) % kNibblesPerTrack];
    };

    int curSector = -1;
    for (int i = 0; i < 2 * kNibblesPerTrack; ++i) {
        // 13-sector address prologue: D5 AA B5.
        if (at(i) == 0xD5 && at(i + 1) == 0xAA && at(i + 2) == 0xB5) {
            uint8_t addr[4];
            for (int k = 0; k < 4; ++k) {
                uint8_t pair[2] = { at(i + 3 + k * 2), at(i + 4 + k * 2) };
                addr[k] = decode4and4(pair);
            }
            curSector = addr[2];
            i += 13;                       // 3 + 8 + 3 epilogue - 1
            continue;
        }
        // Data prologue: D5 AA AD.
        if (at(i) == 0xD5 && at(i + 1) == 0xAA && at(i + 2) == 0xAD &&
            curSector >= 0 && curSector < kSectorsPerTrack13) {
            const int d = i + 3;
            // 411 nibbles = 410 data (154 low + 256 high) + 1 checksum.
            // Running-XOR inverse of writeDataField13's `wr`: nv[n] = nval.
            uint8_t nv[410];
            uint8_t prev = 0;
            bool ok = true;
            for (int n = 0; n < 410; ++n) {
                const uint8_t v = kUntranslate5[at(d + n)];
                if (v == 0xFF) { ok = false; break; }
                const uint8_t cur = static_cast<uint8_t>(prev ^ v);
                nv[n] = cur;
                prev  = cur;
            }
            if (ok) {
                const uint8_t chk = kUntranslate5[at(d + 410)];
                if (chk == 0xFF || chk != prev) ok = false;   // checksum
            }
            if (ok) {
                // Inverse of writeDataField13. Stream layout:
                //   nv[0]                        = src[255] & 7
                //   nv[1 + (2-k)*51 + j], k=2..0 = low triple for (k,j)
                //   nv[154 + k*51 + (50-j)] k0..4= src[j*5+k] >> 3
                //   nv[409]                      = src[255] >> 3
                uint8_t* out = outSectors[curSector];
                auto tri = [&](int k, int j) { return nv[1 + (2 - k) * 51 + j]; };
                auto hi  = [&](int k, int j) { return nv[154 + k * 51 + (50 - j)]; };
                for (int j = 0; j <= 50; ++j) {
                    for (int k = 0; k < 3; ++k)
                        out[j*5+k] = static_cast<uint8_t>(
                            (hi(k, j) << 3) | ((tri(k, j) >> 2) & 7));
                    const uint8_t t0 = tri(0, j), t1 = tri(1, j), t2 = tri(2, j);
                    out[j*5+3] = static_cast<uint8_t>((hi(3, j) << 3) |
                        ((((t0 >> 1) & 1) << 2) | (((t1 >> 1) & 1) << 1) | ((t2 >> 1) & 1)));
                    out[j*5+4] = static_cast<uint8_t>((hi(4, j) << 3) |
                        (((t0 & 1) << 2) | ((t1 & 1) << 1) | (t2 & 1)));
                }
                out[255] = static_cast<uint8_t>((nv[409] << 3) | (nv[0] & 7));
                decodedAny = true;
            }
            i += 3 + 411 + 3 - 1;          // skip prologue + data + epilogue
            curSector = -1;
            continue;
        }
    }
    return decodedAny;
}

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
    const std::string tmp = path + ".pom2tmp";
    // The rename replaces the inode, so without this the image inherits the
    // temp file's umask-derived mode and the user's own permissions on their
    // disk image are silently rewritten on the first write-back. Capture
    // before the write (same discipline as `Disk35Image::saveDirty`).
    std::error_code permEc;
    const std::filesystem::perms origPerms =
        std::filesystem::status(path, permEc).permissions();
    const bool havePerms = !permEc;
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
    std::error_code ec;
    if (havePerms) {
        std::filesystem::permissions(tmp, origPerms, ec);
        ec.clear();
    }
    if (!pom2::replaceFileAtomic(tmp, path, ec)) {
        lastError = "Cannot replace " + path + ": " + ec.message();
        std::error_code ec2;
        std::filesystem::remove(tmp, ec2);
        return false;
    }
    return true;
}
}  // namespace

namespace {

/// A dirty track whose nibble stream no longer decodes cannot be written back
/// to a SECTOR format at all — .dsk/.do/.po hold decoded sectors, so there is
/// nowhere to put a stream the decoder cannot parse.
///
/// What used to happen was worse than failing: the track was skipped, the
/// remaining tracks were written, `dirty` was cleared and the function
/// returned true, logging "Saved 0 modified track(s)". `hasUnsavedChanges()`
/// went false and `getLastError()` stayed empty, so the eject path saw a clean
/// success — and the guest's sector silently reverted to its previous
/// contents. A guest whose write splice damaged the very data field it was
/// writing lost it with a success message rather than a retryable error.
///
/// Failing keeps the dirty flags set, so nothing is thrown away and the user
/// can retry. Turning write-back off is the escape hatch if the track is
/// permanently unparseable: saveDirty is then a no-op that succeeds, and the
/// disk ejects.
bool reportUndecodable(const std::vector<int>& tracks, std::string& lastError)
{
    std::string list;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (i) list += ", ";
        list += std::to_string(tracks[i]);
    }
    lastError = "track " + list + " was modified but its nibble stream no "
                "longer decodes into sectors, so it cannot be written back to "
                "this format. Nothing was saved and the changes are kept in "
                "memory; retry, or turn write-back off to eject without saving.";
    pom2::log().warn("Disk II", "Save refused — " + lastError);
    return false;
}

/// The WOZ counterpart of `reportUndecodable`: a dirty quarter-track whose
/// bit stream has no slot to go back into (no TRKS entry captured at load, an
/// empty stream, or a slot that would run past the buffer). Same reasoning —
/// skipping it clears the dirty flag and reports success, which loses the
/// guest's writes silently; failing keeps them in memory for a retry.
bool reportUnsplicableWoz(const std::vector<int>& qts, std::string& lastError)
{
    std::string list;
    for (std::size_t i = 0; i < qts.size(); ++i) {
        if (i) list += ", ";
        list += std::to_string(qts[i] / 4);
        switch (qts[i] % 4) {
            case 1: list += ".25"; break;
            case 2: list += ".5";  break;
            case 3: list += ".75"; break;
            default: break;
        }
    }
    lastError = "track " + list + " was modified but has no writable slot in "
                "this WOZ's TRKS table, so its bit cells cannot be written "
                "back. Nothing was saved and the changes are kept in memory; "
                "retry, or turn write-back off to eject without saving.";
    pom2::log().warn("Disk II", "Save refused — " + lastError);
    return false;
}

}  // namespace

bool DiskImage::saveDirty()
{
    if (!loaded || !anyDirty || !writeBackEnabled || fileWriteProtected) {
        return true;   // nothing to save, save disabled, or medium WP — no error
    }

    // .woz: splice each dirty quarter-track's bit cells back into wozRaw
    // at the offset captured at load time, then write the whole file out.
    // Per Applesauce WOZ 2.1 spec the header CRC32 is allowed to be zero
    // ("not computed by the imager"); we use that sentinel so a reader
    // that mismatches our recomputed CRC doesn't reject the file.
    if (wozFormat) {
        if (wozRaw.size() < 12) {
            lastError = "WOZ raw buffer missing (load did not populate)";
            return false;
        }
        // A dirty quarter-track with no usable slot in `wozRaw` — no TRKS
        // entry captured at load, an empty bit stream, or a slot that runs
        // past the buffer — cannot be spliced back. Refuse the WHOLE save
        // rather than skipping it: the old `continue` fell through to
        // `wozQtDirty.fill(false)` and `return true`, so the guest's writes to
        // that track were dropped while `hasUnsavedChanges()` went false and
        // the eject path saw a clean success. Same rule (and same escape
        // hatch — turn write-back off) as `reportUndecodable` for the sector
        // formats. Checked BEFORE any splice so a refusal leaves `wozRaw`
        // untouched too.
        std::vector<int> unsplicable;
        for (int qt = 0; qt < kQuarterTracks; ++qt) {
            if (!wozQtDirty[qt]) continue;
            const size_t bitCnt = wozQtBitCount[qt];
            if (wozQtByteLen[qt] == 0 || bitCnt == 0 ||
                bitStream[qt].size() < bitCnt ||
                wozQtByteOff[qt] + (bitCnt + 7) / 8 > wozRaw.size()) {
                unsplicable.push_back(qt);
            }
        }
        if (!unsplicable.empty())
            return reportUnsplicableWoz(unsplicable, lastError);

        int dirtyQts = 0;
        for (int qt = 0; qt < kQuarterTracks; ++qt) {
            if (!wozQtDirty[qt]) continue;
            const size_t byteOff = wozQtByteOff[qt];
            const size_t bitCnt  = wozQtBitCount[qt];
            const auto&  bits    = bitStream[qt];
            // Re-pack MSB-first within each byte — same encoding as
            // loadWoz's unpack loop. Untouched trailing bits in the
            // final byte and the rest of the slot stay at their
            // original wozRaw value (Applesauce zero-pads after
            // bitCount, but preserving the on-disk pad keeps the file
            // byte-identical when no writes happened on this track).
            for (size_t b = 0; b < bitCnt; ++b) {
                const size_t byteIdx   = b / 8;
                const int    bitInByte = 7 - static_cast<int>(b % 8);
                uint8_t&     dst       = wozRaw[byteOff + byteIdx];
                const uint8_t mask     = static_cast<uint8_t>(1u << bitInByte);
                if (bits[b]) dst |=  mask;
                else         dst &= static_cast<uint8_t>(~mask);
            }
            ++dirtyQts;
        }
        // Zero the CRC32 sentinel — readers will skip CRC validation
        // (matches MAME `as_dsk.cpp:275-277` and the loadWoz path here
        // which treats CRC==0 as "not computed").
        wozRaw[ 8] = 0; wozRaw[ 9] = 0; wozRaw[10] = 0; wozRaw[11] = 0;

        const bool wrote = writeFileAtomic(path, lastError,
            [&](std::ofstream& wf) {
                // Wrapper envelope (MacBinary) captured at load — wozRaw
                // holds the bare WOZ payload; re-emit the header around it.
                if (twoImgFormat && !twoImgHeaderRaw.empty()) {
                    wf.write(reinterpret_cast<const char*>(twoImgHeaderRaw.data()),
                             static_cast<std::streamsize>(twoImgHeaderRaw.size()));
                }
                wf.write(reinterpret_cast<const char*>(wozRaw.data()),
                         static_cast<std::streamsize>(wozRaw.size()));
                if (twoImgFormat && !twoImgTrailerRaw.empty()) {
                    wf.write(reinterpret_cast<const char*>(twoImgTrailerRaw.data()),
                             static_cast<std::streamsize>(twoImgTrailerRaw.size()));
                }
            });
        if (!wrote) return false;
        wozQtDirty.fill(false);
        anyDirty = false;
        pom2::log().info("Disk II",
            "Saved " + std::to_string(dirtyQts)
            + " modified quarter-track(s) to " + path + " (.woz, CRC zeroed)");
        return true;
    }

    // .nib: just write the raw nibble buffers verbatim. CNib2 source
    // images use 6384 bytes/track instead of 6656; the load path padded
    // each track up to the runtime width with $FF and the saveDirty
    // path truncates back so the round-trip preserves the source size.
    // If the source was wrapped in a 2IMG envelope, re-emit the captured
    // header bytes + payload + trailer so the file remains a valid 2IMG
    // after the round-trip.
    if (nibFormat) {
        // CNib2's 6384-byte tracks are padded to the 6656-nibble runtime width
        // at load, and only the first 6384 go back to the file — so nibbles
        // 6384..6655 are a WRITE BLACK HOLE covering ~4 % of every track. The
        // guest's write lands there, saveDirty reports success, and the bytes
        // are gone on reload with nothing said.
        //
        // The pad is $FF, which is also what sync bytes are, so a write that
        // happens to be sync loses nothing and must not fail the save. Compare
        // instead of guessing: only real, differing content is a loss, and a
        // loss is worth refusing over.
        if (cnib2Format) {
            std::vector<int> lossy;
            for (int t = 0; t < kTracks; ++t) {
                if (!dirty[t]) continue;
                for (std::size_t k = 6384; k < static_cast<std::size_t>(kNibblesPerTrack); ++k) {
                    if (tracks[t][k] != 0xFF) { lossy.push_back(t); break; }
                }
            }
            if (!lossy.empty()) {
                std::string list;
                for (std::size_t i = 0; i < lossy.size(); ++i) {
                    if (i) list += ", ";
                    list += std::to_string(lossy[i]);
                }
                lastError = "track " + list + " carries data past nibble 6384, "
                            "which this image's CNib2 6384-per-track layout has "
                            "nowhere to store. Nothing was saved and the changes "
                            "are kept in memory; convert the image to a full "
                            "6656-per-track .nib or to WOZ to keep writing to it.";
                pom2::log().warn("Disk II", "Save refused — " + lastError);
                return false;
            }
        }
        const bool wrote = writeFileAtomic(path, lastError,
            [&](std::ofstream& f) {
                if (twoImgFormat && !twoImgHeaderRaw.empty()) {
                    f.write(reinterpret_cast<const char*>(twoImgHeaderRaw.data()),
                            static_cast<std::streamsize>(twoImgHeaderRaw.size()));
                }
                const std::size_t bytesPerTrack =
                    cnib2Format ? static_cast<std::size_t>(6384)
                                : static_cast<std::size_t>(kNibblesPerTrack);
                for (int t = 0; t < kTracks; ++t) {
                    f.write(reinterpret_cast<const char*>(tracks[t].data()),
                            static_cast<std::streamsize>(bytesPerTrack));
                }
                if (twoImgFormat && !twoImgTrailerRaw.empty()) {
                    f.write(reinterpret_cast<const char*>(twoImgTrailerRaw.data()),
                            static_cast<std::streamsize>(twoImgTrailerRaw.size()));
                }
            });
        if (!wrote) return false;
        dirty.fill(false);
        anyDirty = false;
        pom2::log().info("Disk II",
            std::string("Saved (.nib") +
            (cnib2Format ? " CNib2 6384/track" : "") +
            (twoImgFormat ? ", 2IMG-wrapped" : "") + "): " + path);
        return true;
    }

    // 13-sector (.d13 / DOS 3.x): decode via the 5-and-3 path into a
    // 116480-byte image. Never 2IMG-wrapped, but a MacBinary envelope is
    // possible and rides the same captured-header plumbing. File offset =
    // (track*13 + S)*256 where S is the address-field sector number
    // (decodeTrack13 indexes by S).
    if (is13Sector()) {
        const std::size_t payloadStart13 =
            (twoImgFormat && !twoImgHeaderRaw.empty()) ? twoImgHeaderRaw.size()
                                                       : 0;
        std::vector<uint8_t> bytes(kBytesPerImage13, 0);
        {
            std::ifstream rf(path, std::ios::binary);
            if (!rf ||
                !rf.seekg(static_cast<std::streamoff>(payloadStart13)) ||
                !rf.read(reinterpret_cast<char*>(bytes.data()), kBytesPerImage13)) {
                lastError = "Cannot preserve unchanged tracks: source image "
                            "is missing or truncated: " + path;
                return false;
            }
        }
        int decodedTracks = 0;
        std::vector<int> undecodable;
        for (int t = 0; t < kTracks; ++t) {
            if (!dirty[t]) continue;
            uint8_t sectors[kSectorsPerTrack13][kSectorBytes];
            for (int s = 0; s < kSectorsPerTrack13; ++s)
                std::memcpy(sectors[s],
                    bytes.data() + (t * kSectorsPerTrack13 + s) * kSectorBytes,
                    kSectorBytes);
            if (!decodeTrack13(t, sectors)) { undecodable.push_back(t); continue; }
            for (int s = 0; s < kSectorsPerTrack13; ++s)
                std::memcpy(bytes.data() + (t * kSectorsPerTrack13 + s) * kSectorBytes,
                    sectors[s], kSectorBytes);
            ++decodedTracks;
        }
        if (!undecodable.empty()) return reportUndecodable(undecodable, lastError);
        const bool wrote = writeFileAtomic(path, lastError,
            [&](std::ofstream& wf) {
                if (twoImgFormat && !twoImgHeaderRaw.empty()) {
                    wf.write(reinterpret_cast<const char*>(twoImgHeaderRaw.data()),
                             static_cast<std::streamsize>(twoImgHeaderRaw.size()));
                }
                wf.write(reinterpret_cast<const char*>(bytes.data()),
                         kBytesPerImage13);
                if (twoImgFormat && !twoImgTrailerRaw.empty()) {
                    wf.write(reinterpret_cast<const char*>(twoImgTrailerRaw.data()),
                             static_cast<std::streamsize>(twoImgTrailerRaw.size()));
                }
            });
        if (!wrote) return false;
        dirty.fill(false);
        anyDirty = false;
        pom2::log().info("Disk II", "Saved " + std::to_string(decodedTracks) +
                         " modified track(s) to " + path + " (13-sector)");
        return true;
    }

    // .dsk/.do/.po: read existing file, decode dirty tracks, overwrite.
    // For a 2IMG-wrapped source, the existing payload starts past the
    // captured header (twoImgHeaderRaw.size() bytes in).
    const std::size_t payloadStart =
        (twoImgFormat && !twoImgHeaderRaw.empty()) ? twoImgHeaderRaw.size()
                                                   : 0;
    std::vector<uint8_t> bytes(kBytesPerImage, 0);
    {
        std::ifstream rf(path, std::ios::binary);
        if (!rf ||
            !rf.seekg(static_cast<std::streamoff>(payloadStart)) ||
            !rf.read(reinterpret_cast<char*>(bytes.data()), kBytesPerImage)) {
            lastError = "Cannot preserve unchanged tracks: source image "
                        "is missing or truncated: " + path;
            return false;
        }
    }

    const int* skew = (sectorOrder == SectorOrder::ProDOS)
                      ? kProDosLogicalForPhysical
                      : kDos33LogicalForPhysical;

    int decodedTracks = 0;
    std::vector<int> undecodable;
    for (int t = 0; t < kTracks; ++t) {
        if (!dirty[t]) continue;
        uint8_t sectors[kSectorsPerTrack][kSectorBytes];
        // Pre-fill with the existing file content so sectors that fail
        // to decode (or weren't rewritten by the guest) keep their
        // original bytes.
        for (int p = 0; p < kSectorsPerTrack; ++p) {
            const int logical = skew[p];
            const size_t off  = (t * kSectorsPerTrack + logical) * kSectorBytes;
            std::memcpy(sectors[p], bytes.data() + off, kSectorBytes);
        }
        if (!decodeTrack(t, sectors)) { undecodable.push_back(t); continue; }
        // Re-pack into the file at logical positions.
        for (int p = 0; p < kSectorsPerTrack; ++p) {
            const int logical = skew[p];
            const size_t off  = (t * kSectorsPerTrack + logical) * kSectorBytes;
            std::memcpy(bytes.data() + off, sectors[p], kSectorBytes);
        }
        ++decodedTracks;
    }

    if (!undecodable.empty()) return reportUndecodable(undecodable, lastError);

    const bool wrote = writeFileAtomic(path, lastError,
        [&](std::ofstream& wf) {
            if (twoImgFormat && !twoImgHeaderRaw.empty()) {
                wf.write(reinterpret_cast<const char*>(twoImgHeaderRaw.data()),
                         static_cast<std::streamsize>(twoImgHeaderRaw.size()));
            }
            wf.write(reinterpret_cast<const char*>(bytes.data()), kBytesPerImage);
            if (twoImgFormat && !twoImgTrailerRaw.empty()) {
                wf.write(reinterpret_cast<const char*>(twoImgTrailerRaw.data()),
                         static_cast<std::streamsize>(twoImgTrailerRaw.size()));
            }
        });
    if (!wrote) return false;
    dirty.fill(false);
    anyDirty = false;
    pom2::log().info("Disk II", "Saved " + std::to_string(decodedTracks) +
                     " modified track(s) to " + path +
                     (twoImgFormat ? " (2IMG envelope preserved)" : ""));
    return true;
}

// ────────────────────────────────────────────────────────────────────────

void DiskImage::writeDataField(uint8_t*& dst, const uint8_t* src)
{
    *dst++ = 0xD5; *dst++ = 0xAA; *dst++ = 0xAD;

    // Split the 256 input bytes into the 6-and-2 buffers. high6 holds
    // the top 6 bits in normal order; low2[i] packs the low 2 bits of
    // src[i], src[i+86], src[i+172] into one nibble (bit-pair-swapped).
    uint8_t high6[256];
    uint8_t low2[86];
    for (int i = 0; i < 256; ++i) high6[i] = static_cast<uint8_t>(src[i] >> 2);
    // Index reversal: the boot PROM's $0300+X buffer reads in the order
    // disk-nibble 0 → $0300+85, disk-nibble 1 → $0300+84, … so on the
    // combine pass it pulls byte[i]'s low-2 bits from low2[85 - i mod 86]
    // (slot i / 86), NOT low2[i mod 86]. Mirror that here so the on-disk
    // layout matches what the controller's RWTS reconstructs.
    for (int i = 0; i < 86; ++i) {
        uint8_t v = rev2(src[i] & 3);
        if (i + 86  < 256) v |= static_cast<uint8_t>(rev2(src[i + 86]  & 3) << 2);
        if (i + 172 < 256) v |= static_cast<uint8_t>(rev2(src[i + 172] & 3) << 4);
        low2[85 - i] = v;
    }

    // Stream out: low2 in REVERSE order (85→0), then high6 normally
    // (0→255), running an XOR checksum the whole way. Each nibble is
    // translated through kGcrTable on its way to the disk surface.
    uint8_t prev = 0;
    for (int j = 85; j >= 0; --j) {
        uint8_t cur = low2[j];
        *dst++ = kGcrTable[(prev ^ cur) & 0x3F];
        prev = cur;
    }
    for (int i = 0; i < 256; ++i) {
        uint8_t cur = high6[i];
        *dst++ = kGcrTable[(prev ^ cur) & 0x3F];
        prev = cur;
    }
    *dst++ = kGcrTable[prev & 0x3F];   // final XOR checksum nibble

    *dst++ = 0xDE; *dst++ = 0xAA; *dst++ = 0xEB;
}

// Read a 2IMG envelope's payload length (header bytes 28-31, little-endian),
// validating the magic and that the declared window actually fits the file.
// Returns nullopt when the file is not a readable 2IMG — callers then fall
// back to size heuristics rather than refusing the image. Mirrors the header
// parse in Block512Backing::loadImage.
static std::optional<uint64_t> read2mgPayloadLength(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    unsigned char hdr[32] = {0};
    f.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
    if (f.gcount() != static_cast<std::streamsize>(sizeof(hdr)))
        return std::nullopt;
    if (std::memcmp(hdr, "2IMG", 4) != 0) return std::nullopt;
    const auto rd32 = [&hdr](int o) -> uint64_t {
        return static_cast<uint64_t>(hdr[o])
             | (static_cast<uint64_t>(hdr[o + 1]) <<  8)
             | (static_cast<uint64_t>(hdr[o + 2]) << 16)
             | (static_cast<uint64_t>(hdr[o + 3]) << 24);
    };
    const uint64_t off = rd32(24);
    const uint64_t len = rd32(28);
    std::error_code ec;
    const uint64_t total =
        static_cast<uint64_t>(std::filesystem::file_size(path, ec));
    if (ec) return std::nullopt;
    // A declared window that runs past the end of the file is a malformed
    // envelope — let the size heuristics have it rather than trusting it.
    if (off < 64 || len == 0 || off > total || off + len > total)
        return std::nullopt;
    return len;
}

DiskSlotClass classifyDiskForSlot(const std::string& path)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return DiskSlotClass::Unknown;
    const std::uintmax_t szRaw = fs::file_size(path, ec);
    if (ec) return DiskSlotClass::Unknown;
    const uint64_t sz = static_cast<uint64_t>(szRaw);

    std::string ext = fs::path(path).extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // A WOZ says which drive it came off, so ask it rather than guessing:
    // `INFO.disk_type` is 1 for 5.25" and 2 for 3.5". Every other extension
    // below is sniffed by size, which cannot work for flux — the file size
    // is a property of the DUMP. Sending an 800K 3.5" WOZ to the Disk II
    // (which is what a bare `.woz` rule did) fails at mount time with an
    // error about the wrong drive.
    if (ext == ".woz") {
        std::ifstream f(path, std::ios::binary);
        char hdr[12] = {};
        if (f && f.read(hdr, sizeof(hdr)) &&
            hdr[0] == 'W' && hdr[1] == 'O' && hdr[2] == 'Z')
        {
            // Walk the chunks for INFO; its second byte is the disk type.
            char id[8];
            while (f.read(id, 8)) {
                const uint32_t len =
                    static_cast<uint8_t>(id[4])
                  | (static_cast<uint32_t>(static_cast<uint8_t>(id[5])) << 8)
                  | (static_cast<uint32_t>(static_cast<uint8_t>(id[6])) << 16)
                  | (static_cast<uint32_t>(static_cast<uint8_t>(id[7])) << 24);
                if (!std::memcmp(id, "INFO", 4)) {
                    char info[2] = {};
                    if (f.read(info, 2) && info[1] == 2)
                        return DiskSlotClass::Sony35;
                    break;
                }
                if (len > (1u << 30)) break;
                f.seekg(static_cast<std::streamoff>(len), std::ios::cur);
                if (!f) break;
            }
        }
        return DiskSlotClass::Floppy525;
    }

    // ── 2IMG: classify from the HEADER, never from the file size ────────
    // The envelope carries an arbitrary data offset and an optional comment
    // / creator trailer, so `sz` says nothing reliable about the payload —
    // exactly what the loaders key off (`Block512Backing` parses dataOff /
    // dataLen and only requires the PAYLOAD to be 512-aligned). Guessing by
    // size refused ordinary CiderPress output (a hard-disk .2mg with a
    // comment block landed in no bucket at all) and dead-ended every
    // ProDOS volume between 143 KB and 800 KB into the 5.25" loader, which
    // rejects it with "larger volumes belong on the HDV card" — a route
    // classification then made unreachable.
    if (ext == ".2mg") {
        if (const auto payload = read2mgPayloadLength(path)) {
            const uint64_t len = *payload;
            if (len == 143360 || len == 232960 || len == 223440)
                return DiskSlotClass::Floppy525;   // ProDOS/DOS floppy, or NIB
            if (len == 819200)
                return DiskSlotClass::Sony35;
            if (len >= 512 && (len % 512) == 0)
                return DiskSlotClass::Hdv;
            return DiskSlotClass::Unknown;
        }
        // Unreadable or not actually a 2IMG — fall through to the
        // size heuristics below so behaviour degrades rather than breaks.
    }

    // 5.25" Disk II — mirrors accept525(). The 800K `.po` is NOT caught
    // here (only the 143360-byte 5.25" ProDOS size); it falls through to
    // the Sony35 bucket below. A `.dsk` is likewise only 5.25" at a 5.25"
    // size: `Disk35Image` accepts a bare 800K payload under .po, .dsk AND
    // .image (it even special-cases .dsk for write protection), so an 800K
    // .dsk used to be handed to the 5.25" loader and refused with a message
    // listing only 5.25" sizes.
    if (ext == ".dsk" || ext == ".do" || ext == ".nib"
        || ext == ".d13") {
        if (ext == ".dsk" && sz == 819200) return DiskSlotClass::Sony35;
        return DiskSlotClass::Floppy525;
    }
    if (ext == ".po" && (sz == 143360 || sz == 143360 + 64))
        return DiskSlotClass::Floppy525;
    if (ext == ".2mg" && sz < 819200)
        return DiskSlotClass::Floppy525;

    // 800K Sony 3.5" — mirrors accept35() (819200 ± 2IMG header slack).
    if ((ext == ".po" || ext == ".2mg" || ext == ".image") &&
        (sz == 819200 || sz == 819200 + 64 ||
         (sz > 819200 && sz < 819200 + 4096)))
        return DiskSlotClass::Sony35;

    // ProDOS hard disk. A `.hdv` is UNAMBIGUOUSLY a hard-disk volume (AppleWin
    // lineage) — never a Sony 3.5" floppy — so it lands here at ANY valid
    // 512-aligned size, including exactly 800K (1600 blocks, e.g.
    // AppleWorks_AW.hdv). The old `> 819200` bound silently dropped exactly-800K
    // .hdv images into Unknown ("unrecognised disk image"). A `.2mg` IS
    // ambiguous, so it only reaches the HDV bucket when LARGER than 800K — a
    // ≤800K .2mg was already claimed by the Sony35 rule above.
    if (ext == ".hdv" && sz >= 512 &&
        ((sz % 512 == 0) || (sz > 64 && (sz - 64) % 512 == 0)))
        return DiskSlotClass::Hdv;
    if (ext == ".2mg" && sz > 819200 &&
        ((sz % 512 == 0) || ((sz - 64) % 512 == 0)))
        return DiskSlotClass::Hdv;

    return DiskSlotClass::Unknown;
}
