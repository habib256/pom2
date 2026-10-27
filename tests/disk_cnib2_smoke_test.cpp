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

// Pins the CNib2 (6384-byte/track NIB variant) detection + load path.
//
// CNib2 is a rarer NIB encoding where each track is 6384 nibbles on disk
// instead of the standard 6656. AppleWin recognises it as a distinct
// class; we accept the 223 440-byte total size and pad each track up to
// the 6656-wide runtime buffer with $FF (sync gap). After load:
//   - tracks[t][0 .. 6383]    = source bytes
//   - tracks[t][6384 .. 6655] = $FF (the inserted sync pad)
// The padding lets the LSS see a normal sync run at the wrap-around
// point so existing controller code (which assumes 6656-wide tracks)
// stays oblivious.

#include "DiskImage.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

constexpr int kSrcBytesPerTrack = 6384;
constexpr int kCNib2TotalBytes  = DiskImage::kTracks * kSrcBytesPerTrack;

bool runSyntheticLoad()
{
    // Pattern: byte = (track ^ position) so an off-by-one in track
    // indexing OR a padding/offset bug both surface immediately.
    std::vector<uint8_t> buf(static_cast<std::size_t>(kCNib2TotalBytes));
    for (int t = 0; t < DiskImage::kTracks; ++t) {
        for (int i = 0; i < kSrcBytesPerTrack; ++i) {
            buf[static_cast<std::size_t>(t) * kSrcBytesPerTrack + i] =
                static_cast<uint8_t>((t * 13 + i) & 0xFF);
        }
    }

    const std::string path = "cnib2_synthetic.nib";
    std::ofstream wf(path, std::ios::binary | std::ios::trunc);
    if (!wf) {
        std::fprintf(stderr, "synthetic: cannot create temp file\n");
        return false;
    }
    wf.write(reinterpret_cast<const char*>(buf.data()),
             static_cast<std::streamsize>(buf.size()));
    wf.close();

    DiskImage img;
    const bool ok = img.loadFile(path);
    std::remove(path.c_str());
    if (!ok) {
        std::fprintf(stderr, "synthetic: loadFile failed: %s\n",
                     img.getLastError().c_str());
        return false;
    }

    // The runtime track buffer width is kNibblesPerTrack=6656. Spot-check
    // the first byte of every track and the last source byte (index 6383)
    // plus the first padding byte (index 6384, must be $FF).
    for (int t = 0; t < DiskImage::kTracks; ++t) {
        const uint8_t firstExpected = static_cast<uint8_t>((t * 13 + 0) & 0xFF);
        const uint8_t lastSrcExpected =
            static_cast<uint8_t>((t * 13 + 6383) & 0xFF);
        const uint8_t firstByte = img.nibbleAt(t, 0);
        const uint8_t lastSrcByte = img.nibbleAt(t, 6383);
        const uint8_t firstPadByte = img.nibbleAt(t, 6384);
        const uint8_t lastPadByte =
            img.nibbleAt(t, DiskImage::kNibblesPerTrack - 1);
        if (firstByte != firstExpected) {
            std::fprintf(stderr,
                "synthetic: track %d byte 0 = 0x%02X, want 0x%02X\n",
                t, firstByte, firstExpected);
            return false;
        }
        if (lastSrcByte != lastSrcExpected) {
            std::fprintf(stderr,
                "synthetic: track %d byte 6383 = 0x%02X, want 0x%02X\n",
                t, lastSrcByte, lastSrcExpected);
            return false;
        }
        if (firstPadByte != 0xFF || lastPadByte != 0xFF) {
            std::fprintf(stderr,
                "synthetic: track %d padding wrong "
                "(byte 6384=0x%02X, last=0x%02X) — both must be $FF\n",
                t, firstPadByte, lastPadByte);
            return false;
        }
    }
    return true;
}

// Sanity: a 223 440-byte file with bytes not matching anything special
// is recognised by SIZE alone — no extension required. This catches the
// "rename .nib → .x" case where the extension lies but the content is OK.
bool runSizeOnlyDetection()
{
    std::vector<uint8_t> buf(static_cast<std::size_t>(kCNib2TotalBytes), 0xAA);
    const std::string path = "cnib2_no_ext_hint";   // deliberately no .nib
    std::ofstream wf(path, std::ios::binary | std::ios::trunc);
    if (!wf) return false;
    wf.write(reinterpret_cast<const char*>(buf.data()),
             static_cast<std::streamsize>(buf.size()));
    wf.close();

    DiskImage img;
    const bool ok = img.loadFile(path);
    std::remove(path.c_str());
    if (!ok) {
        std::fprintf(stderr, "size-only: loadFile refused %s: %s\n",
                     path.c_str(), img.getLastError().c_str());
        return false;
    }
    if (img.nibbleAt(0, 0) != 0xAA) {
        std::fprintf(stderr,
            "size-only: track 0 byte 0 = 0x%02X, want 0xAA\n",
            img.nibbleAt(0, 0));
        return false;
    }
    return true;
}

// Write-back. The load path pads each 6384-nibble source track up to the
// 6656 runtime width; saveDirty has to truncate back, or the round trip
// grows the user's file by 9 520 bytes and the next load — which detects
// CNib2 BY SIZE — no longer recognises it. So the size assertion below is
// not cosmetic: getting it wrong converts the image to an unreadable one.
bool runWriteBackRoundTrip()
{
    std::vector<uint8_t> buf(static_cast<std::size_t>(kCNib2TotalBytes));
    for (int t = 0; t < DiskImage::kTracks; ++t)
        for (int i = 0; i < kSrcBytesPerTrack; ++i)
            buf[static_cast<std::size_t>(t) * kSrcBytesPerTrack + i] =
                static_cast<uint8_t>((t * 13 + i) & 0xFF);

    const std::string path = "cnib2_writeback.nib";
    {
        std::ofstream wf(path, std::ios::binary | std::ios::trunc);
        if (!wf) return false;
        wf.write(reinterpret_cast<const char*>(buf.data()),
                 static_cast<std::streamsize>(buf.size()));
    }

    {
        DiskImage img;
        if (!img.loadFile(path)) {
            std::fprintf(stderr, "writeback: load failed: %s\n",
                         img.getLastError().c_str());
            std::remove(path.c_str());
            return false;
        }
        // A .nib mounts writable only under the user's opt-in.
        if (!img.isWriteProtected()) {
            std::fprintf(stderr, "writeback: expected WP before opt-in\n");
            std::remove(path.c_str());
            return false;
        }
        img.setWriteBackEnabled(true);
        img.writeNibbleAt(7, 100, 0xA5);
        if (!img.saveDirty()) {
            std::fprintf(stderr, "writeback: saveDirty failed: %s\n",
                         img.getLastError().c_str());
            std::remove(path.c_str());
            return false;
        }
    }

    std::ifstream rf(path, std::ios::binary | std::ios::ate);
    const auto sz = static_cast<long long>(rf.tellg());
    rf.close();
    if (sz != kCNib2TotalBytes) {
        std::fprintf(stderr,
            "writeback: file is %lld bytes after save, want %d "
            "(pad not truncated → next load misdetects the format)\n",
            sz, kCNib2TotalBytes);
        std::remove(path.c_str());
        return false;
    }

    DiskImage back;
    const bool reloaded = back.loadFile(path);
    const uint8_t got = reloaded ? back.nibbleAt(7, 100) : 0x00;
    std::remove(path.c_str());
    if (!reloaded) {
        std::fprintf(stderr, "writeback: reload failed: %s\n",
                     back.getLastError().c_str());
        return false;
    }
    if (got != 0xA5) {
        std::fprintf(stderr,
            "writeback: track 7 nibble 100 = 0x%02X after round trip, "
            "want 0xA5\n", got);
        return false;
    }
    return true;
}

}  // namespace

// STORAGE-010: a .nib's self-sync bytes are inferred by MAME's rule
// (ap2_dsk.cpp a2_nib_format): only "FF … FF D5" is sync, 10 cells per $FF;
// other $FF stay 8; a bit-7-clear byte neither ends nor joins a run. Every
// .nib byte used to be 8 cells.
bool runNibSyncWidths()
{
    std::vector<uint8_t> nib(static_cast<size_t>(DiskImage::kTracks) * DiskImage::kNibblesPerTrack, 0x96);
    auto put = [&](int at, std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) nib[static_cast<size_t>(at++)] = b;
    };
    put(100, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xD5});   // 5 syncs
    put(200, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x96});   // not D5: data
    put(300, {0xFF, 0xFF, 0x00, 0xFF, 0xD5});          // 3 syncs, $00 transparent
    put(DiskImage::kNibblesPerTrack - 2, {0xFF, 0xFF}); // wraps the seam…
    put(0, {0xFF, 0xD5});                               // …into 3 syncs
    const std::string path = "pom2_nib_sync.nib";
    { std::ofstream f(path, std::ios::binary);
      f.write(reinterpret_cast<const char*>(nib.data()), static_cast<std::streamsize>(nib.size())); }
    DiskImage img;
    if (!img.loadFile(path)) { std::fprintf(stderr, "nib sync: load failed\n"); return false; }
    std::remove(path.c_str());
    const int want = DiskImage::kNibblesPerTrack * 8 + 2 * (5 + 3 + 3);
    if (img.trackBitLength(0) != want) {
        std::fprintf(stderr, "nib sync: track 0 is %d cells, want %d\n",
                     img.trackBitLength(0), want);
        return false;
    }
    std::printf("  ok: .nib FF…FF D5 runs are 10-cell sync (MAME a2_nib_format)\n");
    return true;
}

int main()
{
    bool ok = true;
    ok &= runNibSyncWidths();
    ok &= runSyntheticLoad();
    ok &= runSizeOnlyDetection();
    ok &= runWriteBackRoundTrip();
    if (!ok) return 1;
    std::printf("disk_cnib2_smoke OK: 35 × 6384 → padded 35 × 6656, "
                "write-back truncates back\n");
    return 0;
}
