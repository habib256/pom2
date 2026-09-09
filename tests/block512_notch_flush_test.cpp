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

// The NOTCH must never strand blocks the guest already wrote.
//
// CLAUDE.md's media rule makes write-protect a property of the DISK, modelled
// as the image file's host read-only bit, and the media panels / Disk Library
// flip it on a MOUNTED image (`StorageCoordinator::setMediaNotch`: chmod the
// file, then `setHostWriteProtected(true)` on every mounted copy). That put a
// silent data-loss path in the middle of the write-back:
//
//   guest writes block 2  → dirty
//   user ticks "Write-protected"
//   `takeWriteBack()` refuses (it gated on `isWriteProtected()`)
//   `saveDirty()` returns TRUE with `hasUnsavedChanges()` still true
//   the card's eject guard (`!isWriteProtected()`) skips the flush
//   `eject()` drops the image — and the guest's 512 bytes with it.
//
// The refusal was never necessary: the commit writes a temp SIBLING and
// renames it, which a chmod-read-only file accepts (the rename needs the
// directory, and `syncFileContents` opens O_RDONLY for exactly this reason —
// AtomicFileReplace.h). And a medium protected at MOUNT time can hold no
// dirty block at all, because `writeBlock`/`writeByte` refuse from the first
// access — so gating the FLUSH on the medium's own lock (the 2IMG header bit,
// `isMediumLocked()`) rather than on the notch changes exactly the case above.
//
// Pins, in order:
//   1. an image protected at mount stays unwritable and never goes dirty
//      (the pre-existing behaviour, which must not regress);
//   2. a 2IMG whose header says locked is likewise unwritable;
//   3. a notch applied to a DIRTY mounted image still lets the write-back
//      land, through both `saveDirty()` and the card's eject path;
//   4. `saveDirty()` never reports success while changes remain unsaved.

#include "Block512Backing.h"
#include "MediaNotch.h"
#include "ProDOSHardDiskCard.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr size_t kBlk = pom2::Block512Backing::kBlockBytes;

int failures = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); ++failures; }
}

fs::path scratchDir()
{
    const fs::path d = fs::temp_directory_path() / "pom2_block512_notch_flush";
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

void writeRaw(const fs::path& p, size_t blocks, uint8_t fill)
{
    const std::vector<uint8_t> img(blocks * kBlk, fill);
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(img.data()),
            static_cast<std::streamsize>(img.size()));
}

void writeLocked2mg(const fs::path& p, size_t blocks)
{
    std::vector<uint8_t> img(64 + blocks * kBlk, 0x00);
    std::memcpy(img.data(), "2IMG", 4);
    auto w32 = [&](size_t o, uint32_t v) {
        img[o + 0] = static_cast<uint8_t>(v);
        img[o + 1] = static_cast<uint8_t>(v >> 8);
        img[o + 2] = static_cast<uint8_t>(v >> 16);
        img[o + 3] = static_cast<uint8_t>(v >> 24);
    };
    w32(12, 1);                                  // ProDOS block order
    w32(16, 0x80000000u);                        // flags bit 31 = locked
    w32(24, 64);                                 // data offset
    w32(28, static_cast<uint32_t>(blocks * kBlk));
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(img.data()),
            static_cast<std::streamsize>(img.size()));
}

uint8_t firstByteOfBlock(const fs::path& p, size_t block, size_t dataOffset = 0)
{
    std::ifstream f(p, std::ios::binary);
    f.seekg(static_cast<std::streamoff>(dataOffset + block * kBlk));
    char c = 0;
    f.read(&c, 1);
    return static_cast<uint8_t>(c);
}

void unprotect(const fs::path& p)
{
    std::error_code ec;
    fs::permissions(p, fs::perms::owner_write, fs::perm_options::add, ec);
}

}  // namespace

int main()
{
    const fs::path dir = scratchDir();

    // ── 1. Protected AT MOUNT: unwritable, and never dirty ───────────────
    {
        const fs::path p = dir / "mounted-protected.hdv";
        writeRaw(p, 8, 0x11);
        std::string err;
        check(pom2::setMediaNotch(p.string(), true, err),
              "the notch could be put on the file: " + err);

        pom2::Block512Backing b;
        b.setWriteBackEnabled(true);
        check(b.loadImage(p.string()), "a protected image still mounts");
        check(b.isWriteProtected(),
              "a host read-only image mounts write-protected");

        uint8_t blk[kBlk];
        std::memset(blk, 0xEE, kBlk);
        check(!b.writeBlock(2, blk), "a write to it is refused");
        check(!b.hasUnsavedChanges(), "and leaves nothing dirty");
        check(b.saveDirty(), "saveDirty() is a clean no-op");
        check(firstByteOfBlock(p, 2) == 0x11, "the file is untouched");
        unprotect(p);
    }

    // ── 2. A 2IMG the header itself locks ────────────────────────────────
    {
        const fs::path p = dir / "header-locked.2mg";
        writeLocked2mg(p, 5);
        pom2::Block512Backing b;
        b.setWriteBackEnabled(true);
        check(b.loadImage(p.string()), "a locked 2IMG mounts");
        check(b.isWriteProtected(), "and reports write-protected");
        uint8_t blk[kBlk];
        std::memset(blk, 0xEE, kBlk);
        check(!b.writeBlock(1, blk), "a write to it is refused");
        check(!b.hasUnsavedChanges(), "and leaves nothing dirty");
        check(firstByteOfBlock(p, 1, 64) == 0x00, "the file is untouched");
    }

    // ── 3. The notch put on a DIRTY mounted image ────────────────────────
    // This is the regression. `saveDirty()` must still land the blocks the
    // guest wrote BEFORE the notch, and must not claim success while they
    // are still only in RAM.
    {
        const fs::path p = dir / "notched-while-dirty.hdv";
        writeRaw(p, 8, 0x11);

        pom2::Block512Backing b;
        b.setWriteBackEnabled(true);
        check(b.loadImage(p.string()), "image mounts writable");
        check(!b.isWriteProtected(), "and is not protected");

        uint8_t blk[kBlk];
        std::memset(blk, 0xEE, kBlk);
        check(b.writeBlock(2, blk), "the guest writes block 2");
        check(b.hasUnsavedChanges(), "block 2 is dirty");

        // What StorageCoordinator::setMediaNotch does, in its order.
        std::string err;
        check(pom2::setMediaNotch(p.string(), true, err),
              "the file takes the notch: " + err);
        b.setHostWriteProtected(true);
        check(b.isWriteProtected(),
              "the mounted copy now reports write-protected");

        // New writes are refused — the notch is doing its job.
        uint8_t other[kBlk];
        std::memset(other, 0x77, kBlk);
        check(!b.writeBlock(3, other),
              "a write AFTER the notch is refused");

        const bool saved = b.saveDirty();
        check(saved, "saveDirty() succeeds");
        check(!b.hasUnsavedChanges(),
              "saveDirty() must not report success while block 2 is still "
              "only in RAM");
        check(firstByteOfBlock(p, 2) == 0xEE,
              "block 2 reached the file (the guest's write was not lost)");
        check(firstByteOfBlock(p, 3) == 0x11,
              "the refused write did NOT reach the file");
        unprotect(p);
    }

    // ── 4. The same through the card's eject path ────────────────────────
    // `ProDOSHardDiskCard::ejectImage` owns the save-on-eject policy; its
    // guard used to be `!isWriteProtected()`, so a notched-while-dirty image
    // was dropped without a flush and without an error.
    {
        const fs::path p = dir / "eject-after-notch.hdv";
        writeRaw(p, 8, 0x11);

        ProDOSHardDiskCard card(5);
        check(card.loadImage(p.string()), "card mounts the image");
        card.setWriteBackEnabled(true);
        uint8_t written[kBlk];
        std::memset(written, 0xAB, kBlk);
        check(card.backing().writeBlock(4, written),
              "the guest writes block 4 through the card");
        check(card.hasUnsavedChanges(), "the card reports unsaved changes");

        std::string err;
        check(pom2::setMediaNotch(p.string(), true, err),
              "the file takes the notch: " + err);
        card.setHostWriteProtected(true);

        check(card.ejectImage(), "eject succeeds");
        check(!card.isImageLoaded(), "the bay is empty");
        check(firstByteOfBlock(p, 4) == 0xAB,
              "the eject flushed block 4 instead of discarding it");
        unprotect(p);
    }

    // ── 5. Two-phase eject (detachImage) sees the same payload ───────────
    {
        const fs::path p = dir / "detach-after-notch.hdv";
        writeRaw(p, 8, 0x11);

        ProDOSHardDiskCard card(5);
        check(card.loadImage(p.string()), "card mounts the image");
        card.setWriteBackEnabled(true);
        uint8_t v[kBlk];
        std::memset(v, 0x5C, kBlk);
        check(card.backing().writeBlock(6, v), "the guest writes block 6");

        std::string err;
        check(pom2::setMediaNotch(p.string(), true, err),
              "the file takes the notch: " + err);
        card.setHostWriteProtected(true);

        pom2::Block512Backing::PendingWriteBack pending;
        check(card.detachImage(pending), "detachImage succeeds");
        check(pending.valid,
              "detachImage captured a payload (it used to report 'nothing to "
              "write' and the caller then ejected the block away)");
        std::string commitError;
        check(pom2::Block512Backing::commitWriteBack(std::move(pending),
                                                     commitError),
              "the unlocked commit lands: " + commitError);
        check(firstByteOfBlock(p, 6) == 0x5C, "block 6 reached the file");
        unprotect(p);
    }

    std::error_code ec;
    fs::remove_all(dir, ec);

    if (failures) {
        std::printf("block512_notch_flush: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("block512_notch_flush: OK\n");
    return 0;
}
