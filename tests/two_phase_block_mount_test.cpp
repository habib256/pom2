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

// Two-phase block mount — pins Block512Backing::readImageFile / adoptImage.
//
// This is the storage WRITE-BACK path, where a mistake costs somebody their
// disk, so the cases here are not about the speed the split was done for. They
// are about the three ways splitting a 130-line loadImage() into two halves
// could have quietly changed what gets mounted:
//
//   1. The 2IMG header must still be parsed. This is the trap the whole change
//      was designed around: `loadImageFromBytes` already existed and looked
//      like a ready-made phase 2, and it is for SYNTHESISED volumes — it skips
//      the 2IMG parse, forces synth_, and ties write-back to it. Routing a real
//      .2mg through it would mount 64 bytes of header as block data and turn
//      write-protect and write-back into lies. Case 2 is what would catch a
//      future "simplification" back onto it.
//   2. The host-writability probe must still happen. It moved from the adopt
//      half to the read half (it is a syscall, and the adopt half runs under
//      the lock), so a chmod-read-only image must still come up WP rather
//      than accept a session of writes and fail at flush time.
//   3. Re-mounting the SAME file while the guest has unsaved writes must not
//      roll them back. Phase 1 reads before phase 2 flushes, which inverts the
//      order loadImage() had — the same hazard as the Disk II two-phase mount,
//      and the same fix.

#include "Block512Backing.h"

#include <algorithm>
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

constexpr std::size_t kBlk = pom2::Block512Backing::kBlockBytes;

fs::path tmpPath(const std::string& leaf)
{
    return fs::temp_directory_path() / leaf;
}

void writeFile(const fs::path& p, const std::vector<uint8_t>& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

std::vector<uint8_t> readFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
}

/// A raw .hdv of `blocks` blocks, each filled with its own index so a mounted
/// block can be identified without ambiguity.
std::vector<uint8_t> rawImage(std::size_t blocks)
{
    std::vector<uint8_t> v(blocks * kBlk);
    for (std::size_t b = 0; b < blocks; ++b)
        std::memset(v.data() + b * kBlk, static_cast<int>(b & 0xFF), kBlk);
    return v;
}

void put32(std::vector<uint8_t>& v, std::size_t at, uint32_t x)
{
    v[at + 0] = static_cast<uint8_t>(x        & 0xFF);
    v[at + 1] = static_cast<uint8_t>((x >>  8) & 0xFF);
    v[at + 2] = static_cast<uint8_t>((x >> 16) & 0xFF);
    v[at + 3] = static_cast<uint8_t>((x >> 24) & 0xFF);
}

/// A 2IMG container: 64-byte header, ProDOS order (format 1), `blocks` blocks
/// of payload. `locked` sets the write-protect bit CiderPress uses.
std::vector<uint8_t> twoImgImage(std::size_t blocks, bool locked)
{
    const std::vector<uint8_t> payload = rawImage(blocks);
    std::vector<uint8_t> v(64 + payload.size(), 0);
    v[0] = '2'; v[1] = 'I'; v[2] = 'M'; v[3] = 'G';
    put32(v, 12, 1);                                   // format: ProDOS order
    put32(v, 16, locked ? 0x80000000u : 0u);           // flags: locked bit
    put32(v, 24, 64);                                  // data offset
    put32(v, 28, static_cast<uint32_t>(payload.size()));
    std::memcpy(v.data() + 64, payload.data(), payload.size());
    return v;
}

/// Mount through the two-phase pair, the way pom2::mountBlockCard does.
bool twoPhaseMount(pom2::Block512Backing& b, const fs::path& p, std::string& err)
{
    pom2::Block512Backing::PreparedImage prepared;
    if (!pom2::Block512Backing::readImageFile(p.string(), prepared, err))
        return false;
    return b.adoptImage(std::move(prepared));
}

uint8_t firstByteOfBlock(const pom2::Block512Backing& b, uint32_t blk)
{
    uint8_t buf[kBlk] = {0};
    assert(b.readBlock(blk, buf));
    return buf[0];
}

// ── 1. A raw .hdv mounts identically both ways ───────────────────────────
void testRawImageMatchesInlineLoad()
{
    const fs::path p = tmpPath("pom2_2pb_raw.hdv");
    writeFile(p, rawImage(8));

    pom2::Block512Backing inlineB;
    assert(inlineB.loadImage(p.string()));

    pom2::Block512Backing twoPhaseB;
    std::string err;
    assert(twoPhaseMount(twoPhaseB, p, err));
    assert(err.empty());

    assert(twoPhaseB.blockCount() == inlineB.blockCount());
    assert(twoPhaseB.blockCount() == 8);
    assert(twoPhaseB.path() == inlineB.path());
    assert(twoPhaseB.isWriteProtected() == inlineB.isWriteProtected());
    assert(twoPhaseB.isSynthVolume() == false &&
           twoPhaseB.isSynthVolume() == inlineB.isSynthVolume());
    for (uint32_t b = 0; b < 8; ++b)
        assert(firstByteOfBlock(twoPhaseB, b) == firstByteOfBlock(inlineB, b));

    std::printf("[ OK ] a raw .hdv mounts identically both ways\n");
    std::error_code ec; fs::remove(p, ec);
}

// ── 2. THE trap: 2IMG must still be parsed ───────────────────────────────
void testTwoImgHeaderIsStillParsed()
{
    const fs::path p = tmpPath("pom2_2pb_container.2mg");
    writeFile(p, twoImgImage(/*blocks=*/6, /*locked=*/false));

    pom2::Block512Backing b;
    std::string err;
    assert(twoPhaseMount(b, p, err));

    // 6 blocks, NOT 6 + the header rounded up. A phase 2 that skipped the
    // parse would mount 64 extra bytes and either refuse the image (not a
    // multiple of 512) or shift every block by the header.
    assert(b.blockCount() == 6 &&
           "the 2IMG header was not stripped — 64 bytes of it are block data");
    // Block 0 must be the payload's block 0, not the "2IMG" magic.
    assert(firstByteOfBlock(b, 0) == 0x00);
    assert(firstByteOfBlock(b, 3) == 0x03 &&
           "the blocks are shifted — the data offset was ignored");
    assert(!b.isSynthVolume() &&
           "mounted as a synthesised volume — that is loadImageFromBytes's job, "
           "and it would break write-back");
    assert(b.canWriteBack() &&
           "a writable 2IMG must support write-back after a two-phase mount");

    // And the locked bit must still reach write-protect.
    const fs::path pl = tmpPath("pom2_2pb_locked.2mg");
    writeFile(pl, twoImgImage(6, /*locked=*/true));
    pom2::Block512Backing lockedB;
    assert(twoPhaseMount(lockedB, pl, err));
    assert(lockedB.isWriteProtected() &&
           "the 2IMG locked flag was dropped by the two-phase path");
    assert(!lockedB.canWriteBack());

    std::printf("[ OK ] the 2IMG header is still parsed (offset, blocks, WP)\n");
    std::error_code ec; fs::remove(p, ec); fs::remove(pl, ec);
}

// ── 3. The host-writability probe survived the move to phase 1 ───────────
void testReadOnlyFileMountsWriteProtected()
{
    const fs::path p = tmpPath("pom2_2pb_ro.hdv");
    writeFile(p, rawImage(4));

    std::error_code ec;
    fs::permissions(p, fs::perms::owner_read, fs::perm_options::replace, ec);
    if (ec) { std::printf("[SKIP] cannot chmod here\n"); return; }

    // Root ignores the mode bits, so confirm the probe would actually fail
    // before asserting on it — the same guard disk_media_state uses.
    {
        std::ofstream probe(p, std::ios::in | std::ios::out | std::ios::binary);
        if (probe) {
            std::printf("[SKIP] running with mode bits bypassed (root?)\n");
            fs::permissions(p, fs::perms::owner_all, fs::perm_options::replace, ec);
            fs::remove(p, ec);
            return;
        }
    }

    pom2::Block512Backing b;
    std::string err;
    assert(twoPhaseMount(b, p, err));
    assert(b.isWriteProtected() &&
           "a chmod-read-only image mounted writable — the probe was lost when "
           "it moved into phase 1");
    assert(!b.canWriteBack());

    fs::permissions(p, fs::perms::owner_all, fs::perm_options::replace, ec);
    fs::remove(p, ec);
    std::printf("[ OK ] a read-only file still mounts write-protected\n");
}

// ── 4. Re-mounting the same dirty file must not roll writes back ─────────
void testSameFileWithUnsavedWritesIsNotRolledBack()
{
    const fs::path p = tmpPath("pom2_2pb_samefile.hdv");
    writeFile(p, rawImage(4));
    const std::vector<uint8_t> pristine = readFile(p);

    pom2::Block512Backing b;
    assert(b.loadImage(p.string()));
    b.setWriteBackEnabled(true);

    // The guest writes block 2.
    std::vector<uint8_t> block(kBlk, 0xEE);
    assert(b.writeBlock(2, block.data()));
    assert(b.hasUnsavedChanges());

    // Phase 1 reads the file as it is ON DISK — still pristine, because the
    // write lives only in the backing store.
    pom2::Block512Backing::PreparedImage prepared;
    std::string err;
    assert(pom2::Block512Backing::readImageFile(p.string(), prepared, err));

    // Phase 2 must notice the collision, flush, and re-read rather than
    // adopting the stale bytes it was handed.
    assert(b.adoptImage(std::move(prepared)));

    const std::vector<uint8_t> onDisk = readFile(p);
    assert(onDisk.size() == pristine.size());
    assert(onDisk != pristine &&
           "the guest's write never reached the file");
    assert(onDisk[2 * kBlk] == 0xEE);

    // And the medium now mounted is the flushed file, not the stale copy.
    assert(!b.hasUnsavedChanges());
    assert(firstByteOfBlock(b, 2) == 0xEE &&
           "the stale prepared image was adopted — the guest's write was rolled "
           "back in memory");

    std::printf("[ OK ] re-mounting the same dirty file keeps the writes\n");
    std::error_code ec; fs::remove(p, ec);
}

// ── 5. Write-back still works through a two-phase mount ──────────────────
void testWriteBackAfterTwoPhaseMount()
{
    const fs::path p = tmpPath("pom2_2pb_writeback.2mg");
    writeFile(p, twoImgImage(4, false));

    pom2::Block512Backing b;
    std::string err;
    assert(twoPhaseMount(b, p, err));
    b.setWriteBackEnabled(true);

    std::vector<uint8_t> block(kBlk, 0x5A);
    assert(b.writeBlock(1, block.data()));
    assert(b.saveDirty());
    assert(!b.hasUnsavedChanges());

    // The header must have survived the round trip — a write-back that
    // rewrote only the payload would truncate the container.
    const std::vector<uint8_t> after = readFile(p);
    assert(after.size() == 64 + 4 * kBlk &&
           "the 2IMG header was lost on write-back");
    assert(after[0] == '2' && after[1] == 'I');
    assert(after[64 + 1 * kBlk] == 0x5A);

    std::printf("[ OK ] write-back preserves the 2IMG container\n");
    std::error_code ec; fs::remove(p, ec);
}

// ── 6. Failure paths ─────────────────────────────────────────────────────
void testFailurePaths()
{
    pom2::Block512Backing b;
    std::string err;

    pom2::Block512Backing::PreparedImage missing;
    assert(!pom2::Block512Backing::readImageFile(
        tmpPath("pom2_2pb_absent.hdv").string(), missing, err));
    assert(!err.empty() && "phase 1 failure must say why");
    assert(!b.isLoaded());

    // An unprepared image is refused rather than mounted as an empty volume.
    pom2::Block512Backing::PreparedImage unprepared;
    assert(!b.adoptImage(std::move(unprepared)));
    assert(!b.isLoaded());

    // A file that is not a whole number of blocks is refused by phase 2, not
    // by phase 1 — the size gates live with the parse, which is the point of
    // splitting there.
    const fs::path ragged = tmpPath("pom2_2pb_ragged.hdv");
    writeFile(ragged, std::vector<uint8_t>(kBlk + 3, 0));
    pom2::Block512Backing::PreparedImage p2;
    assert(pom2::Block512Backing::readImageFile(ragged.string(), p2, err));
    assert(!b.adoptImage(std::move(p2)));
    assert(!b.isLoaded());
    assert(!b.lastError().empty());

    std::printf("[ OK ] failure paths report and mount nothing\n");
    std::error_code ec; fs::remove(ragged, ec);
}

// ── 7. Writes racing the two-phase EJECT survive ─────────────────────────
// takeWriteBack() MOVES the dirty set out: flags retired at capture, so a
// block the guest writes while commitWriteBack runs unlocked (the medium is
// still mounted, the CPU worker still running) keeps its flag and reaches
// the file through the eject's own inline flush. The pre-fix blanket
// clearDirty() in phase 3 wiped exactly those flags — guest writes racing
// the commit were silently dropped from the user's only host copy.
void testWritesDuringPendingWriteBackSurvive()
{
    const fs::path p = tmpPath("pom2_2pb_eject_race.2mg");
    writeFile(p, twoImgImage(4, false));

    pom2::Block512Backing b;
    std::string err;
    assert(twoPhaseMount(b, p, err));
    b.setWriteBackEnabled(true);

    std::vector<uint8_t> blk(kBlk, 0xAA);
    assert(b.writeBlock(1, blk.data()));

    // Phase 1: the capture retires the flags it captured…
    pom2::Block512Backing::PendingWriteBack pending = b.takeWriteBack();
    assert(pending.valid);
    assert(!b.hasUnsavedChanges() &&
           "capture must retire the flags it captured");

    // …and the guest keeps writing against the still-mounted medium: a NEW
    // block, and a REWRITE of the captured one (the overlap case an
    // index-based phase-3 clear would also have lost).
    std::fill(blk.begin(), blk.end(), 0xBB);
    assert(b.writeBlock(2, blk.data()));
    std::fill(blk.begin(), blk.end(), 0xCC);
    assert(b.writeBlock(1, blk.data()));
    assert(b.hasUnsavedChanges() &&
           "writes racing the commit must stay dirty");

    // Phase 2 commits the captured payload only.
    assert(pom2::Block512Backing::commitWriteBack(std::move(pending), err));
    std::vector<uint8_t> onDisk = readFile(p);
    assert(onDisk[64 + 1 * kBlk] == 0xAA);

    // Phase 3 = the eject's own inline flush of the remainder.
    assert(b.saveDirty());
    onDisk = readFile(p);
    assert(onDisk[64 + 1 * kBlk] == 0xCC && "racing rewrite lost");
    assert(onDisk[64 + 2 * kBlk] == 0xBB && "racing write lost");
    assert(!b.hasUnsavedChanges());

    std::printf("[ OK ] writes racing a two-phase eject survive\n");
    std::error_code ec; fs::remove(p, ec);
}

// ── 8. A failed commit restores the captured dirty set ───────────────────
void testRestoreDirtyAfterFailedCommit()
{
    const fs::path p = tmpPath("pom2_2pb_restore.2mg");
    writeFile(p, twoImgImage(4, false));

    pom2::Block512Backing b;
    std::string err;
    assert(twoPhaseMount(b, p, err));
    b.setWriteBackEnabled(true);

    std::vector<uint8_t> blk(kBlk, 0xD1);
    assert(b.writeBlock(3, blk.data()));

    pom2::Block512Backing::PendingWriteBack pending = b.takeWriteBack();
    assert(pending.valid && !b.hasUnsavedChanges());

    // The failure path's undo: the medium is dirty again, and the next save
    // re-captures and writes the block (pre-split behaviour: a failed save
    // loses nothing).
    b.restoreDirty(pending.dirtyIndices);
    assert(b.hasUnsavedChanges());
    assert(b.saveDirty());
    const std::vector<uint8_t> onDisk = readFile(p);
    assert(onDisk[64 + 3 * kBlk] == 0xD1);

    std::printf("[ OK ] a failed commit's restore keeps the blocks dirty\n");
    std::error_code ec; fs::remove(p, ec);
}

}  // namespace

int main()
{
    testRawImageMatchesInlineLoad();
    testTwoImgHeaderIsStillParsed();
    testReadOnlyFileMountsWriteProtected();
    testSameFileWithUnsavedWritesIsNotRolledBack();
    testWriteBackAfterTwoPhaseMount();
    testFailurePaths();
    testWritesDuringPendingWriteBackSurvive();
    testRestoreDirtyAfterFailedCommit();
    std::printf("two_phase_block_mount: all assertions passed\n");
    return 0;
}
