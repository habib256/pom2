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

// A FAILED host-folder write-back must not freeze the files it did write.
//
// `decodeVolumeToFolder` is not atomic across a tree: it writes file by file
// and stops at the first I/O failure, so a failed pass usually leaves part of
// the folder already rewritten — with an mtime NEWER than the volume's mount
// stamp. `preserveNewerThan` reads "newer than the mount" as "the user edited
// this behind POM2's back, keep it", so on the retry those files were skipped
// and the guest's own saves were discarded. Permanently: nothing ever moved
// the stamp forward, so every later flush skipped them again.
//
// Two halves of the same fix (bug hunt 4 #4):
//   * `commitWriteBack` publishes `completedAt` BEFORE the `!r.ok` return
//     (ProDOSVolume.cpp:1409-1410 fills it unconditionally),
//   * `saveDirty` adopts the stamp on the FAILURE path too.
//
// Forcing a PARTIAL failure without root: the volume carries two files, and
// the host folder has a non-empty DIRECTORY sitting where the second one's
// file belongs. `writeFileAtomically`'s rename over it fails, the first file
// is already on disk, and the walk reports ioFailed.

#include "Block512Backing.h"
#include "ProDOSVolume.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void writeHostFile(const fs::path& p, const std::string& body)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
}

std::string readHostFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

/// Overwrite the first `text.size()` bytes of the file whose ProDOS name is
/// `name`, straight through the block backing — the guest's own write path.
/// Returns false when the name is not in the volume's first data blocks.
bool guestPatch(pom2::Block512Backing& backing, const std::string& needle,
                const std::string& replacement)
{
    assert(needle.size() == replacement.size());
    const std::size_t blocks = backing.blockCount();
    std::vector<std::uint8_t> block(pom2::Block512Backing::kBlockBytes);
    for (std::uint32_t b = 0; b < blocks; ++b) {
        if (!backing.readBlock(b, block.data())) continue;
        const std::string text(reinterpret_cast<const char*>(block.data()),
                               block.size());
        const auto at = text.find(needle);
        if (at == std::string::npos) continue;
        std::memcpy(block.data() + at, replacement.data(), replacement.size());
        return backing.writeBlock(b, block.data());
    }
    return false;
}

}  // namespace

int main()
{
    std::error_code ec;
    const fs::path root = fs::temp_directory_path() / "pom2_synth_failed_flush";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    assert(!ec);

    // Two files. "AFILE" sorts first, so the decode reaches it before the
    // entry the host is going to refuse.
    writeHostFile(root / "AFILE.TXT",  "AAAAAAAAAAAAAAAA");
    writeHostFile(root / "BFILE.TXT",  "BBBBBBBBBBBBBBBB");

    std::vector<std::uint8_t> image;
    const auto built = pom2::buildVolumeFromFolder(root.string(), "HOST", image);
    if (!built.ok) {
        std::printf("SKIP prodos_synth_failed_flush: cannot build volume (%s)\n",
                    built.error.c_str());
        return 0;
    }
    assert(built.filesIncluded == 2);

    pom2::Block512Backing backing;
    assert(backing.loadFromBytes(image, "[host folder] failed-flush",
                                 root.string()));
    backing.setWriteBackEnabled(true);
    assert(backing.isSynthVolume());

    // The guest edits both files.
    assert(guestPatch(backing, "AAAAAAAAAAAAAAAA", "GUESTWROTETHISA1"));
    assert(guestPatch(backing, "BBBBBBBBBBBBBBBB", "GUESTWROTETHISB1"));
    assert(backing.hasUnsavedChanges());

    // Booby-trap the SECOND destination: a non-empty directory where the
    // decode wants to rename its temp file into place. The rename fails, the
    // walk reports an I/O failure — and AFILE.TXT is already rewritten.
    fs::remove(root / "BFILE.TXT", ec);
    fs::create_directories(root / "BFILE.TXT" / "occupied", ec);
    writeHostFile(root / "BFILE.TXT" / "occupied" / "x", "x");
    // Backdate it: a destination NEWER than the mount stamp is "preserved"
    // rather than written, which would side-step the failure this test needs.
    fs::last_write_time(root / "BFILE.TXT",
                        fs::file_time_type::clock::now() -
                            std::chrono::hours(24), ec);

    const bool firstOk = backing.saveDirty();
    if (firstOk) {
        std::printf("SKIP prodos_synth_failed_flush: the decode did not fail "
                    "on a directory in the way (platform difference)\n");
        fs::remove_all(root, ec);
        return 0;
    }
    // The half that landed.
    if (readHostFile(root / "AFILE.TXT") != "GUESTWROTETHISA1") {
        std::printf("SKIP prodos_synth_failed_flush: the failed pass wrote "
                    "nothing, so there is no partial state to freeze\n");
        fs::remove_all(root, ec);
        return 0;
    }
    assert(backing.hasUnsavedChanges());   // failure re-marked the blocks

    // Clear the trap and retry. The retry is what the defect broke: AFILE.TXT
    // now carries an mtime newer than the (never-updated) mount stamp, so the
    // decode preserved it as a "host edit" — and the SECOND guest write below
    // was thrown away for the rest of the session.
    fs::remove_all(root / "BFILE.TXT", ec);
    assert(guestPatch(backing, "GUESTWROTETHISA1", "GUESTWROTETHISA2"));

    const bool secondOk = backing.saveDirty();
    if (!secondOk) {
        std::printf("FAIL: the retry still failed (%s)\n",
                    backing.lastError().c_str());
        return 1;
    }
    const std::string afile = readHostFile(root / "AFILE.TXT");
    if (afile.rfind("GUESTWROTETHISA2", 0) != 0) {
        std::printf("FAIL: the guest's second save was discarded — AFILE.TXT "
                    "still reads '%s'. The failed pass left the file newer "
                    "than the mount stamp and nothing restamped it, so "
                    "preserveNewerThan mistook POM2's own output for a host "
                    "edit (bug hunt 4 #4).\n",
                    afile.substr(0, 16).c_str());
        return 1;
    }

    // And a third round, to say the stamp keeps tracking rather than being
    // advanced once by luck.
    assert(guestPatch(backing, "GUESTWROTETHISA2", "GUESTWROTETHISA3"));
    assert(backing.saveDirty());
    if (readHostFile(root / "AFILE.TXT").rfind("GUESTWROTETHISA3", 0) != 0) {
        std::printf("FAIL: the third save was discarded\n");
        return 1;
    }

    fs::remove_all(root, ec);
    std::printf("OK prodos_synth_failed_flush (a partial write-back failure "
                "no longer freezes the files it wrote)\n");
    return 0;
}
