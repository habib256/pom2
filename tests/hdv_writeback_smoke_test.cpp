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

// HDV write-back smoke test. Pins:
//   - .hdv plain round-trip: write a block, eject (auto-save), reopen,
//     bytes match.
//   - .2mg header preservation: 64-byte 2IMG header + data + trailing
//     creator/comment chunk all survive a save bit-for-bit; only the
//     data window is updated.
//   - 2MG write-protected flag (offset 16 bit 0) is honoured even when
//     the user opts in via setWriteBackEnabled(true) — only the real medium
//     WP flag blocks writes outright.
//   - write-back-OFF: writes still land in RAM (image becomes dirty and
//     reads back the new bytes — the running session sees a read/write
//     volume) but are NOT flushed to the host file on eject; the file stays
//     byte-for-byte unchanged. This is the fix for games that write state on
//     the fly (e.g. Nox Archaist entering a city) crashing on a default-
//     mounted HDV that used to surface as write-protected to ProDOS.

#include "ProDOSHardDiskCard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr size_t kBlock = ProDOSHardDiskCard::kBlockBytes;

fs::path tmpFile(const std::string& tag, const char* ext)
{
    return fs::temp_directory_path() /
           ("pom2_hdv_wb_" + tag + ext);
}

std::vector<uint8_t> readAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const size_t sz = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> out(sz);
    f.read(reinterpret_cast<char*>(out.data()),
           static_cast<std::streamsize>(sz));
    return out;
}

void writeFile(const fs::path& p, const std::vector<uint8_t>& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    assert(f.good());
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    assert(f.good());
}

// Push 512 pattern bytes into block `block` of the card via the streaming
// data port at $C0D2 (low4=0x2). Caller is responsible for setting up the
// block address registers ($C0D0/$C0D1) and write-back opt-in.
void cardWriteBlock(ProDOSHardDiskCard& card, uint16_t block,
                    const uint8_t* pattern)
{
    card.deviceSelectWrite(0x0, static_cast<uint8_t>(block & 0xFF));
    card.deviceSelectWrite(0x1, static_cast<uint8_t>((block >> 8) & 0xFF));
    for (size_t i = 0; i < kBlock; ++i) {
        card.deviceSelectWrite(0x2, pattern[i]);
    }
}

}  // namespace

int main()
{
    // ── Case A: .hdv plain round-trip ───────────────────────────────────
    {
        const fs::path p = tmpFile("a", ".hdv");
        std::vector<uint8_t> file(4 * kBlock, 0);
        writeFile(p, file);

        ProDOSHardDiskCard card;
        assert(card.loadImage(p.string()));
        assert(card.getBlockCount() == 4);
        assert(card.canWriteBack());                 // not WP (no header flag)
        assert(!card.isWriteProtected());            // no medium WP → R/W in session
        card.setWriteBackEnabled(true);
        assert(!card.isWriteProtected());            // write-back doesn't drive WP

        uint8_t pattern[kBlock];
        for (size_t i = 0; i < kBlock; ++i) {
            pattern[i] = static_cast<uint8_t>((i * 13u + 17u) & 0xFFu);
        }
        cardWriteBlock(card, 1, pattern);
        assert(card.hasUnsavedChanges());
        card.ejectImage();                           // saves on eject

        const auto have = readAll(p);
        assert(have.size() == file.size());
        // Block 0 untouched.
        for (size_t i = 0; i < kBlock; ++i) assert(have[i] == 0);
        // Block 1 = pattern.
        for (size_t i = 0; i < kBlock; ++i) assert(have[kBlock + i] == pattern[i]);
        // Blocks 2-3 untouched.
        for (size_t i = 2 * kBlock; i < 4 * kBlock; ++i) assert(have[i] == 0);

        fs::remove(p);
        std::printf("hdv_writeback: .hdv round-trip OK\n");
    }

    // ── Case B: .2mg header AND trailing chunk preservation ─────────────
    {
        const fs::path p = tmpFile("b", ".2mg");
        constexpr size_t kData = 2 * kBlock;
        constexpr size_t kTail = 16;
        std::vector<uint8_t> file(64 + kData + kTail, 0);

        // 2IMG header.
        file[0] = '2'; file[1] = 'I'; file[2] = 'M'; file[3] = 'G';
        file[4] = 'P'; file[5] = 'O'; file[6] = 'M'; file[7] = '2';   // creator
        file[8] = 64;                          // header_len = 64
        file[12] = 1;                          // format = ProDOS
        // flags = 0 (writable)
        file[24] = 64;                         // data_offset
        const uint32_t dlen = static_cast<uint32_t>(kData);
        file[28] = static_cast<uint8_t>(dlen & 0xFF);
        file[29] = static_cast<uint8_t>((dlen >> 8) & 0xFF);
        // Trailing "comment" — every byte distinct so a misalignment shows.
        for (size_t i = 0; i < kTail; ++i) {
            file[64 + kData + i] = static_cast<uint8_t>(0xC0u + i);
        }
        // Data blocks pre-filled with a different pattern so we can spot
        // either the unmodified blocks staying or block 0 being overwritten.
        for (size_t i = 0; i < kData; ++i) {
            file[64 + i] = static_cast<uint8_t>((i * 5u + 1u) & 0xFFu);
        }
        writeFile(p, file);

        // Snapshot what must NOT change.
        std::vector<uint8_t> originalHeader(file.begin(), file.begin() + 64);
        std::vector<uint8_t> originalTail(file.begin() + 64 + kData,
                                          file.begin() + 64 + kData + kTail);
        std::vector<uint8_t> originalBlock1(file.begin() + 64 + kBlock,
                                            file.begin() + 64 + kBlock * 2);

        ProDOSHardDiskCard card;
        assert(card.loadImage(p.string()));
        assert(card.canWriteBack());
        card.setWriteBackEnabled(true);

        uint8_t pattern[kBlock];
        for (size_t i = 0; i < kBlock; ++i) {
            pattern[i] = static_cast<uint8_t>((i * 23u + 7u) & 0xFFu);
        }
        cardWriteBlock(card, 0, pattern);
        assert(card.hasUnsavedChanges());
        card.ejectImage();

        const auto have = readAll(p);
        assert(have.size() == file.size());
        // Header survives bit-for-bit.
        for (size_t i = 0; i < 64; ++i) assert(have[i] == originalHeader[i]);
        // Block 0 was modified.
        for (size_t i = 0; i < kBlock; ++i) assert(have[64 + i] == pattern[i]);
        // Block 1 untouched.
        for (size_t i = 0; i < kBlock; ++i) {
            assert(have[64 + kBlock + i] == originalBlock1[i]);
        }
        // Trailing comment chunk survives bit-for-bit.
        for (size_t i = 0; i < kTail; ++i) {
            assert(have[64 + kData + i] == originalTail[i]);
        }

        fs::remove(p);
        std::printf("hdv_writeback: .2mg header+tail preservation OK\n");
    }

    // ── Case C: 2MG WP flag honoured even when user opts in ─────────────
    {
        const fs::path p = tmpFile("c", ".2mg");
        std::vector<uint8_t> file(64 + 2 * kBlock, 0);
        file[0] = '2'; file[1] = 'I'; file[2] = 'M'; file[3] = 'G';
        file[8] = 64;
        file[12] = 1;                         // format = ProDOS
        file[16] = 1;                         // flags bit 0 = write-protected
        file[24] = 64;
        const uint32_t dlen = 2 * static_cast<uint32_t>(kBlock);
        file[28] = static_cast<uint8_t>(dlen & 0xFF);
        file[29] = static_cast<uint8_t>((dlen >> 8) & 0xFF);
        writeFile(p, file);

        const auto original = readAll(p);

        ProDOSHardDiskCard card;
        assert(card.loadImage(p.string()));
        assert(!card.canWriteBack());                 // WP overrides everything
        card.setWriteBackEnabled(true);
        assert(card.isWriteProtected());              // still WP
        assert(!card.canWriteBack());

        // Try to write — silently dropped because WP gate fires first.
        uint8_t pattern[kBlock];
        std::memset(pattern, 0xCC, sizeof(pattern));
        cardWriteBlock(card, 0, pattern);
        assert(!card.hasUnsavedChanges());
        card.ejectImage();

        const auto after = readAll(p);
        assert(after == original);                    // file untouched

        fs::remove(p);
        std::printf("hdv_writeback: 2MG WP flag honoured OK\n");
    }

    // ── Case D: write-back OFF — writes land in RAM, host file stays clean ─
    // New contract: a hard disk with no medium WP flag is read/write to the
    // running session even with write-back off; only PERSISTING those RAM
    // changes to the host file is gated by the opt-in.
    {
        const fs::path p = tmpFile("d", ".hdv");
        std::vector<uint8_t> file(2 * kBlock, 0);
        for (size_t i = 0; i < file.size(); ++i) {
            file[i] = static_cast<uint8_t>(i & 0xFF);
        }
        writeFile(p, file);
        const auto original = readAll(p);

        ProDOSHardDiskCard card;
        assert(card.loadImage(p.string()));
        assert(card.canWriteBack());
        // Don't call setWriteBackEnabled(true).
        assert(!card.isWriteProtected());            // R/W in session

        uint8_t pattern[kBlock];
        std::memset(pattern, 0xAA, sizeof(pattern));
        cardWriteBlock(card, 0, pattern);
        assert(card.hasUnsavedChanges());            // write reached RAM

        // Read-back through the card returns the freshly written bytes
        // (re-select block 0 to rewind streamOffset to 0).
        card.deviceSelectWrite(0x0, 0x00);
        card.deviceSelectWrite(0x1, 0x00);
        for (size_t i = 0; i < kBlock; ++i) {
            assert(card.deviceSelectRead(0x2) == pattern[i]);
        }

        card.ejectImage();                           // write-back off → no flush

        const auto after = readAll(p);
        assert(after == original);                   // host file untouched

        fs::remove(p);
        std::printf("hdv_writeback: write-back OFF = RAM-only OK\n");
    }

    // ── Case E: failed save refuses replacement and preserves dirty RAM ──
    {
        const fs::path oldPath = tmpFile("replace_old", ".hdv");
        const fs::path newPath = tmpFile("replace_new", ".hdv");
        writeFile(oldPath, std::vector<uint8_t>(2 * kBlock, 0));
        writeFile(newPath, std::vector<uint8_t>(2 * kBlock, 0x55));

        ProDOSHardDiskCard card;
        assert(card.loadImage(oldPath.string()));
        card.setWriteBackEnabled(true);
        uint8_t pattern[kBlock];
        std::memset(pattern, 0xA7, sizeof(pattern));
        cardWriteBlock(card, 0, pattern);
        assert(card.hasUnsavedChanges());

        // Deterministic flush failure on every platform: saveDirty requires
        // the source envelope to keep its original size.
        writeFile(oldPath, std::vector<uint8_t>(kBlock, 0));
        assert(!card.loadImage(newPath.string()));
        assert(card.getImagePath() == oldPath.string());
        assert(card.hasUnsavedChanges());
        card.deviceSelectWrite(0x0, 0);
        card.deviceSelectWrite(0x1, 0);
        for (size_t i = 0; i < kBlock; ++i)
            assert(card.deviceSelectRead(0x2) == 0xA7);

        fs::remove(oldPath);
        fs::remove(newPath);
        std::printf("hdv_writeback: failed flush preserves mounted media OK\n");
    }

    // ── Case F: <image>.pom2tmp is ours by construction ─────────────────
    // Same defect, same fix as `DiskImage::saveDirty` — the target was
    // validated at mount, the sibling temp name was not, and
    // ofstream(trunc) follows symlinks. Before `commitWriteBack` called
    // prepareTempPath, a link planted at <image>.pom2tmp took the whole
    // rewrite and the rename then moved the link over the user's image.
    // (Bug hunt 2026-09-06 #3.)
    {
        const fs::path p      = tmpFile("tmppath", ".hdv");
        const fs::path victim = tmpFile("tmppath_victim", ".bin");
        const fs::path tmp    = fs::path(p.string() + ".pom2tmp");
        writeFile(p, std::vector<uint8_t>(2 * kBlock, 0x11));
        writeFile(victim, std::vector<uint8_t>(4, 0x7E));
        std::error_code ec;
        fs::remove(tmp, ec);
        fs::create_symlink(victim, tmp, ec);
        assert(!ec && "symlink support required for this case");

        ProDOSHardDiskCard card;
        assert(card.loadImage(p.string()));
        card.setWriteBackEnabled(true);
        uint8_t pattern[kBlock];
        std::memset(pattern, 0x5C, sizeof(pattern));
        cardWriteBlock(card, 1, pattern);
        assert(card.hasUnsavedChanges());
        assert(card.ejectImage());                   // save-on-eject

        assert(fs::file_size(victim) == 4);          // victim untouched
        assert(fs::symlink_status(p).type() == fs::file_type::regular);
        const auto saved = readAll(p);
        assert(saved.size() == 2 * kBlock);
        assert(saved[kBlock] == 0x5C);               // the block did land

        fs::remove(victim, ec);
        fs::remove(p, ec);
        std::printf("hdv_writeback: .pom2tmp symlink not followed OK\n");
    }

    // ...and a temp name we may not simply delete refuses the save, keeping
    // the dirty blocks in RAM for a retry.
    {
        const fs::path p   = tmpFile("tmppath_dir", ".hdv");
        const fs::path tmp = fs::path(p.string() + ".pom2tmp");
        writeFile(p, std::vector<uint8_t>(2 * kBlock, 0x22));
        const auto original = readAll(p);
        std::error_code ec;
        fs::remove_all(tmp, ec);
        fs::create_directory(tmp, ec);
        assert(!ec);

        ProDOSHardDiskCard card;
        assert(card.loadImage(p.string()));
        card.setWriteBackEnabled(true);
        uint8_t pattern[kBlock];
        std::memset(pattern, 0x6D, sizeof(pattern));
        cardWriteBlock(card, 0, pattern);
        assert(card.hasUnsavedChanges());
        assert(!card.saveDirty());                   // refused
        assert(card.hasUnsavedChanges());            // retryable
        assert(readAll(p) == original);              // image untouched
        assert(fs::is_directory(tmp));               // refused, not deleted

        fs::remove_all(tmp, ec);
        fs::remove(p, ec);
        std::printf("hdv_writeback: .pom2tmp directory refuses save OK\n");
    }

    std::printf("hdv_writeback_smoke OK\n");
    return 0;
}
