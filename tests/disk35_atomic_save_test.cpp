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

// Disk35Image write-back atomicity regression test.
//
// `Disk35Image::saveDirty` used to open the USER'S 800K image with
// std::ios::trunc and rewrite it in place. Save-on-eject writes 819 200
// bytes: an ENOSPC, a pulled removable medium or a dropped network share
// part-way through left the only copy of the disk truncated, since the rest
// of it lives in the in-RAM `blocks_` that dies with the process. That is the
// exact failure `DiskImage::saveDirty` was hardened against (temp file +
// rename). The 3.5" path now follows the same discipline.
//
// Part 1 proves the original file is never written in place: a hard link
// made before the save still sees the pre-save bytes afterwards, which is
// only possible if the save replaced the path (rename) instead of truncating
// its inode.
// Part 2 injects a failing save (read-only containing directory, so the
// sibling temp file cannot be created) and asserts the user's image is
// byte-identical to what it was before, the failure is reported, the image
// stays dirty for a later retry, and no temp file is left behind.
// Part 3 re-checks the 2IMG envelope (header + trailing comment) survives
// the rewritten save path.

#include "Disk35Image.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kImageBytes = pom2::Disk35Image::kBytesPerImage;
constexpr std::size_t kBlk        = pom2::Disk35Image::kBlockBytes;

std::vector<std::uint8_t> makeRawImage()
{
    std::vector<std::uint8_t> img(kImageBytes, 0x11);
    // Make block 2 look like a ProDOS volume-directory key block so loadFile
    // doesn't warn: prev-block pointer 0 and storage_type $F in the header.
    img[2 * kBlk + 0] = 0;
    img[2 * kBlk + 1] = 0;
    img[2 * kBlk + 4] = 0xF3;
    return img;
}

void writeFile(const fs::path& p, const std::vector<std::uint8_t>& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    assert(f);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    assert(f);
}

std::vector<std::uint8_t> readFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
}

} // namespace

int main()
{
    const fs::path base = fs::temp_directory_path() / "pom2_disk35_atomic";
    fs::remove_all(base);
    fs::create_directories(base);

    std::vector<std::uint8_t> block(kBlk, 0xAB);

    // ── Part 1: a successful save must not rewrite the original inode ────
    {
        const fs::path img  = base / "disk.po";
        const fs::path link = base / "disk.po.link";
        const std::vector<std::uint8_t> original = makeRawImage();
        writeFile(img, original);

        std::error_code linkEc;
        fs::create_hard_link(img, link, linkEc);

        pom2::Disk35Image d;
        assert(d.loadFile(img.string()));
        d.setWriteBackEnabled(true);
        assert(!d.isWriteProtected());
        assert(d.writeBlock(5, block.data()));
        assert(d.hasUnsavedChanges());
        assert(d.saveDirty());
        assert(!d.hasUnsavedChanges());

        const std::vector<std::uint8_t> saved = readFile(img);
        assert(saved.size() == kImageBytes);
        assert(std::memcmp(saved.data() + 5 * kBlk, block.data(), kBlk) == 0);
        assert(std::memcmp(saved.data(), original.data(), kBlk) == 0);

        if (!linkEc) {
            // The pre-save inode is still reachable through the hard link. If
            // saveDirty had truncated the original in place, this would carry
            // the new bytes too.
            const std::vector<std::uint8_t> viaLink = readFile(link);
            assert(viaLink.size() == kImageBytes);
            assert(viaLink == original &&
                   "saveDirty truncated the user's image in place");
        } else {
            std::printf("disk35_atomic_save: hard links unsupported here, "
                        "skipping the in-place check\n");
        }
        assert(!fs::exists(img.string() + ".pom2tmp"));
    }

    // ── Part 2: a failing save must leave the user's image untouched ─────
    // Injection: strip write permission from the containing directory, so the
    // sibling temp file cannot be created. Running as root defeats the
    // permission check, so skip there.
    bool ranFailureCase = false;
#ifndef _WIN32
    if (::geteuid() != 0) {
        const fs::path dir = base / "ro";
        fs::create_directories(dir);
        const fs::path img = dir / "disk.po";
        const std::vector<std::uint8_t> original = makeRawImage();
        writeFile(img, original);

        pom2::Disk35Image d;
        assert(d.loadFile(img.string()));
        d.setWriteBackEnabled(true);
        assert(d.writeBlock(9, block.data()));

        fs::permissions(dir,
                        fs::perms::owner_write | fs::perms::group_write |
                        fs::perms::others_write,
                        fs::perm_options::remove);

        const bool ok = d.saveDirty();

        fs::permissions(dir, fs::perms::owner_write, fs::perm_options::add);

        assert(!ok && "save into a read-only directory must be reported");
        assert(!d.lastError().empty());
        // The dirty flag must survive so the user can retry after fixing the
        // cause — and, above all, the on-disk image must be exactly as it was.
        assert(d.hasUnsavedChanges());
        assert(readFile(img) == original &&
               "a failed save must not disturb the user's image");
        assert(!fs::exists(img.string() + ".pom2tmp") &&
               "temp file must be cleaned up on failure");

        // With the obstacle removed the same dirty state saves normally.
        assert(d.saveDirty());
        const std::vector<std::uint8_t> saved = readFile(img);
        assert(std::memcmp(saved.data() + 9 * kBlk, block.data(), kBlk) == 0);
        ranFailureCase = true;
    }
#endif

    // ── Part 3: 2IMG envelope survives the atomic rewrite ────────────────
    {
        const fs::path img = base / "disk.2mg";
        std::vector<std::uint8_t> file(64, 0);
        std::memcpy(file.data(), "2IMG", 4);
        file[8]  = 64;                       // header length
        file[12] = 1;                        // format 1 = ProDOS order
        file[24] = 64;                       // data offset
        file[28] = static_cast<std::uint8_t>( kImageBytes        & 0xFF);
        file[29] = static_cast<std::uint8_t>((kImageBytes >>  8) & 0xFF);
        file[30] = static_cast<std::uint8_t>((kImageBytes >> 16) & 0xFF);
        file[31] = static_cast<std::uint8_t>((kImageBytes >> 24) & 0xFF);
        const std::vector<std::uint8_t> payload = makeRawImage();
        file.insert(file.end(), payload.begin(), payload.end());
        const std::string comment = "POM2 trailing comment";
        file.insert(file.end(), comment.begin(), comment.end());
        writeFile(img, file);

        pom2::Disk35Image d;
        assert(d.loadFile(img.string()));
        assert(d.kind() == pom2::Disk35Image::ImageKind::TwoImg800k);
        d.setWriteBackEnabled(true);
        assert(d.writeBlock(3, block.data()));
        assert(d.saveDirty());

        const std::vector<std::uint8_t> saved = readFile(img);
        assert(saved.size() == file.size());
        assert(std::memcmp(saved.data(), file.data(), 64) == 0);
        assert(std::memcmp(saved.data() + 64 + 3 * kBlk, block.data(), kBlk) == 0);
        assert(std::memcmp(saved.data() + 64 + kImageBytes,
                           comment.data(), comment.size()) == 0);
        assert(!fs::exists(img.string() + ".pom2tmp"));
    }

    // ── Part 4: writability comes from the FILE, not the extension ──────
    // All three names holding a bare 819 200-byte payload (.po, .dsk,
    // .image) take the identical save path, so all three must be writable
    // under the user's opt-in. `.dsk`/`.image` used to be marked PHYSICALLY
    // write-protected on the grounds that such dumps are "read-only by
    // convention" — which overrode setWriteBackEnabled, so the user could
    // ask for write-back, get it silently refused, and be told nothing.
    for (const char* name : { "bare.dsk", "bare.image", "bare.po" }) {
        const fs::path img = base / name;
        const std::vector<std::uint8_t> original = makeRawImage();
        writeFile(img, original);

        pom2::Disk35Image d;
        assert(d.loadFile(img.string()));
        assert(d.kind() == pom2::Disk35Image::ImageKind::Raw800k);
        // Still write-protected until the user opts in — that gate is the
        // one that must survive; only the extension rule went away.
        assert(d.isWriteProtected() && "opt-in must still be required");
        d.setWriteBackEnabled(true);
        assert(!d.isWriteProtected() && "extension must not force WP");
        assert(d.writeBlock(11, block.data()));
        assert(d.saveDirty());

        const std::vector<std::uint8_t> saved = readFile(img);
        assert(saved.size() == kImageBytes);
        assert(std::memcmp(saved.data() + 11 * kBlk, block.data(), kBlk) == 0
               && "write-back lost for this extension");
    }

    // A host file that is genuinely read-only still forces WP, whatever the
    // extension — that is the check the convention was standing in for.
#ifndef _WIN32
    if (::geteuid() != 0) {
        const fs::path img = base / "readonly.dsk";
        writeFile(img, makeRawImage());
        fs::permissions(img, fs::perms::owner_write | fs::perms::group_write |
                             fs::perms::others_write,
                        fs::perm_options::remove);
        pom2::Disk35Image d;
        assert(d.loadFile(img.string()));
        d.setWriteBackEnabled(true);
        assert(d.isWriteProtected() &&
               "a read-only host file must still mount write-protected");
        fs::permissions(img, fs::perms::owner_write, fs::perm_options::add);
    }
#endif

    // Apparent file size is attacker-controlled (including sparse files):
    // reject it before constructing a correspondingly huge vector.
    {
        const fs::path img = base / "oversized.po";
        { std::ofstream f(img, std::ios::binary); f.put('\0'); }
        fs::resize_file(img, 17u * 1024u * 1024u);
        pom2::Disk35Image d;
        assert(!d.loadFile(img.string()));
    }

    // A failed replacement must not eject the currently mounted disk. This
    // exercises the public Disk35Image API directly (not only the UI wrapper).
    {
        const fs::path current = base / "current.po";
        const fs::path invalid = base / "invalid.po";
        writeFile(current, makeRawImage());
        writeFile(invalid, std::vector<std::uint8_t>{1, 2, 3});

        pom2::Disk35Image d;
        assert(d.loadFile(current.string()));
        std::uint8_t before[kBlk]{};
        assert(d.readBlock(0, before));
        assert(!d.loadFile(invalid.string()));
        assert(d.isLoaded());
        assert(d.path() == current.string());
        std::uint8_t after[kBlk]{};
        assert(d.readBlock(0, after));
        assert(std::memcmp(before, after, kBlk) == 0);
    }

    // Write-back OFF with dirty blocks is a successful NO-OP (the mirror of
    // DiskImage::saveDirty), not an error. Erroring here wedged the 3.5"
    // paths for the rest of the session: once dirty, unchecking "Write-back
    // (save on eject)" made eject, disk swap and the //c+ mount35 flush gate
    // all fail forever with a misleading "image is write-protected".
    // dirty_ survives the no-op, so opting back in before eject still saves.
    {
        const fs::path img = base / "optout.po";
        writeFile(img, makeRawImage());
        const std::vector<std::uint8_t> pristine = readFile(img);

        pom2::Disk35Image d;
        assert(d.loadFile(img.string()));
        d.setWriteBackEnabled(true);
        std::vector<std::uint8_t> blk77(kBlk, 0x77);
        assert(d.writeBlock(5, blk77.data()));

        d.setWriteBackEnabled(false);        // user opts out, then ejects
        assert(d.saveDirty() && "opt-out must be a successful no-op");
        assert(readFile(img) == pristine && "opt-out must not write");
        assert(d.hasUnsavedChanges() &&
               "the no-op must not launder dirty_");

        d.setWriteBackEnabled(true);         // …or changes mind first
        assert(d.saveDirty());
        const std::vector<std::uint8_t> saved = readFile(img);
        assert(std::memcmp(saved.data() + 5 * kBlk, blk77.data(), kBlk) == 0);
    }

    // ── Two-phase write-back (bug hunt 2026-09-06 #13) ──────────────────
    // `saveDirty` is composed from `takeWriteBack` + `commitWriteBack` so a
    // caller holding `stateMutex` can split them in time: phase 1 is a memcpy
    // under the lock, phase 2 is the 800 KB write with the lock RELEASED.
    // The firmware-issued eject (`Sony35Drive` register 7) runs on the CPU
    // worker inside that lock and could not write at all before this split.
    {
        const fs::path img = base / "twophase.po";
        writeFile(img, makeRawImage());
        const std::vector<std::uint8_t> pristine = readFile(img);

        pom2::Disk35Image d;
        assert(d.loadFile(img.string()));
        d.setWriteBackEnabled(true);
        const std::vector<std::uint8_t> blkC3(kBlk, 0xC3);
        assert(d.writeBlock(9, blkC3.data()));
        assert(d.hasUnsavedChanges());

        // Phase 1: retires the dirty flag and touches no file.
        pom2::Disk35Image::PendingWriteBack pending = d.takeWriteBack();
        assert(pending.valid);
        assert(pending.path == img.string());
        assert(pending.bytes.size() == kImageBytes);
        assert(!d.hasUnsavedChanges() && "phase 1 retires what it captured");
        assert(readFile(img) == pristine && "phase 1 must not write");

        // The medium stays mounted and usable while phase 2 runs unlocked.
        assert(d.isLoaded());

        // Phase 2, off the lock.
        std::string error;
        assert(pom2::Disk35Image::commitWriteBack(std::move(pending), error));
        assert(error.empty());
        const std::vector<std::uint8_t> saved = readFile(img);
        assert(std::memcmp(saved.data() + 9 * kBlk, blkC3.data(), kBlk) == 0);

        // Phase-3 undo: a failed commit hands the writes back so a retry
        // re-captures them instead of losing them with a success return.
        const std::vector<std::uint8_t> blkD4(kBlk, 0xD4);
        assert(d.writeBlock(10, blkD4.data()));
        pom2::Disk35Image::PendingWriteBack second = d.takeWriteBack();
        assert(second.valid && !d.hasUnsavedChanges());
        second.path = (base / "no_such_directory" / "gone.po").string();
        std::string err2;
        assert(!pom2::Disk35Image::commitWriteBack(std::move(second), err2));
        assert(!err2.empty());
        d.restoreDirty();
        assert(d.hasUnsavedChanges() && "a failed commit must be retryable");
        assert(d.saveDirty());
        const std::vector<std::uint8_t> retried = readFile(img);
        assert(std::memcmp(retried.data() + 10 * kBlk, blkD4.data(), kBlk) == 0);
    }

    fs::remove_all(base);
    std::printf("OK disk35_atomic_save (rename-replace, 2IMG envelope kept%s, "
                "write-back opt-out no-ops)\n",
                ranFailureCase ? ", failed save leaves image intact" : "");
    return 0;
}
