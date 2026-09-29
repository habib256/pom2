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

// ProDOS synth write-back path-traversal regression test.
//
// decodeVolumeToFolder walks a volume image's directory entries and writes
// each file back into a host folder. The image is GUEST-WRITABLE RAM, so a
// directory-entry name is untrusted: a crafted name like "../PWNED" was
// joined to the host folder unsanitized and escaped the jail when write-back
// was enabled. The decoder now rejects any name that isn't a safe single
// host component (isHostSafeProDOSName).
//
// Part 1 unit-tests the validator. Part 2 crafts a minimal ProDOS volume
// whose directory holds a malicious "../PWNED" file plus a benign "GOOD"
// file, decodes it into base/jail/inner, and asserts the escape file is NOT
// created in base/jail while the benign file IS written.
//
// Part 3 pins the OTHER untrusted-graph hazard: the same guest-writable
// image describes the directory GRAPH, and a subdir entry's key_pointer was
// only range-checked — nothing stopped it pointing at an ancestor block. The
// walk was then a cyclic graph explored to depth 16 with a fan-out of 13
// slots × 256 chained blocks per level, each visit doing a real
// fs::create_directories: a single block of 12 self-referential subdir
// entries is 12^17 visits, i.e. a permanent hang on the eject/save path that
// fills the host filesystem while it runs (measured: fan-out 2 → 262 142
// host directories in 7.2 s; fan-out 12 → still creating directories when
// killed at 25 s). Termination now rests on a per-decode set of expanded
// directory blocks plus a directory budget, not on the depth cap.

#include "ProDOSVolume.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr size_t kBlk = 512;

// Write a directory entry into image[block][slot].
void putEntry(std::vector<std::uint8_t>& img, size_t block, size_t slot,
              std::uint8_t storage, const std::string& name,
              std::uint8_t fileType, std::uint16_t keyPtr, std::uint32_t eof)
{
    std::uint8_t* e = img.data() + block * kBlk + 4 + slot * 39;
    e[0x00] = static_cast<std::uint8_t>((storage << 4) | (name.size() & 0x0F));
    for (size_t i = 0; i < name.size() && i < 15; ++i)
        e[1 + i] = static_cast<std::uint8_t>(name[i]);
    e[0x10] = fileType;
    e[0x11] = keyPtr & 0xFF;
    e[0x12] = (keyPtr >> 8) & 0xFF;
    e[0x15] = eof & 0xFF;
    e[0x16] = (eof >> 8) & 0xFF;
    e[0x17] = (eof >> 16) & 0xFF;
}

// Set a directory block's next-block pointer (offset 2 of every dir block).
void setNextBlock(std::vector<std::uint8_t>& img, size_t block, std::uint16_t next)
{
    img[block * kBlk + 2] = static_cast<std::uint8_t>(next & 0xFF);
    img[block * kBlk + 3] = static_cast<std::uint8_t>((next >> 8) & 0xFF);
}

// Total directories anywhere under `root`, and the deepest nesting level.
void treeStats(const fs::path& root, size_t& count, size_t& maxDepth)
{
    count = 0;
    maxDepth = 0;
    if (!fs::exists(root)) return;
    for (auto it = fs::recursive_directory_iterator(root);
         it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_directory()) continue;
        ++count;
        maxDepth = std::max<size_t>(maxDepth,
                                    static_cast<size_t>(it.depth()) + 1);
        // Never walk an explosion to the end: bail out loudly instead.
        if (count > 4096) return;
    }
}

} // namespace

int main()
{
    // ── Part 1: the validator ────────────────────────────────────────────
    using pom2::isHostSafeProDOSName;
    assert(isHostSafeProDOSName("GOOD"));
    assert(isHostSafeProDOSName("A"));
    assert(isHostSafeProDOSName("NOTES.DATA"));
    assert(isHostSafeProDOSName("HELLO.BIN"));
    assert(!isHostSafeProDOSName(""));
    assert(!isHostSafeProDOSName("."));
    assert(!isHostSafeProDOSName(".."));
    assert(!isHostSafeProDOSName("../X"));
    assert(!isHostSafeProDOSName("a/b"));
    assert(!isHostSafeProDOSName("a\\b"));
    assert(!isHostSafeProDOSName(std::string("a\0b", 3)));  // embedded NUL
    assert(!isHostSafeProDOSName("has space"));
    assert(!isHostSafeProDOSName("0123456789ABCDEF"));       // 16 chars > 15

    // ── Part 2: end-to-end decode escape attempt ─────────────────────────
    const fs::path base = fs::temp_directory_path() / "pom2_decode_safety";
    fs::remove_all(base);
    const fs::path jail  = base / "jail";
    const fs::path inner = jail / "inner";          // hostFolder for the decode
    const fs::path escapeTarget = jail / "PWNED";   // where "../PWNED" would land
    const fs::path benign       = inner / "GOOD";

    std::vector<std::uint8_t> img(8 * kBlk, 0);     // 8 blocks (≥ kFirstDataBlock)

    // Block 2 = volume directory key block. Slot 0 = volume header (storage
    // 0xF → skipped). Next-block pointer (offset 2) = 0 → stop after block 2.
    putEntry(img, 2, 0, /*storage=*/0xF, "VOL", 0x0F, 0, 0);
    // Slot 1: malicious seedling file "../PWNED" → data in block 6.
    putEntry(img, 2, 1, /*seedling=*/0x1, "../PWNED", 0x06, /*key=*/6, /*eof=*/4);
    // Slot 2: benign seedling file "GOOD" → data in block 5. file_type 0x00
    // (typeless) so the decoded host name is verbatim "GOOD" (no extension).
    putEntry(img, 2, 2, /*seedling=*/0x1, "GOOD", 0x00, /*key=*/5, /*eof=*/4);

    std::memcpy(img.data() + 6 * kBlk, "PWND", 4);
    std::memcpy(img.data() + 5 * kBlk, "DATA", 4);

    pom2::ProDOSDecodeResult r = pom2::decodeVolumeToFolder(img, inner.string());
    assert(r.ok);

    // The traversal must have been blocked: no file outside the jail folder.
    assert(!fs::exists(escapeTarget) && "‘../PWNED’ escaped the host folder!");
    // …and the benign file must still decode normally.
    assert(fs::exists(benign) && "benign file should still be written");
    assert(r.filesWritten == 1);
    assert(r.filesSkipped >= 1);

    // ── Part 3a: subdir entry whose key_pointer is its own block ─────────
    // One self-referential entry is the cheap, non-destructive shape of the
    // bomb: against the unbounded walk it still recreates D1/D1/D1/… down to
    // the depth cap. The decoder must create nothing at all — the target
    // block is already being walked.
    {
        const fs::path root = base / "cycle_self";
        std::vector<std::uint8_t> bad(16 * kBlk, 0);
        putEntry(bad, 2, 0, /*storage=*/0xF, "VOL", 0x0F, 0, 0);
        putEntry(bad, 2, 1, /*subdir entry=*/0xD, "D1", 0x0F, /*key=*/2, 0);

        const auto t0 = std::chrono::steady_clock::now();
        pom2::ProDOSDecodeResult c = pom2::decodeVolumeToFolder(bad, root.string());
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();

        size_t dirs = 0, deepest = 0;
        treeStats(root, dirs, deepest);
        assert(c.ok);
        assert(dirs == 0 && "self-referential subdir must not mint host dirs");
        assert(deepest == 0);
        assert(c.dirsCreated == 0);
        assert(c.dirsSkipped >= 1 && "the cycle must be reported, not silent");
        assert(ms < 2000 && "decode of a cyclic volume must terminate promptly");
    }

    // ── Part 3b: 12 sibling entries aliasing one directory block ─────────
    // The fan-out dimension. Every slot in the key block names the same
    // target block, so only the first may be expanded; the rest are aliases
    // and must be refused before they create anything.
    {
        const fs::path root = base / "cycle_alias";
        std::vector<std::uint8_t> bad(16 * kBlk, 0);
        putEntry(bad, 2, 0, /*storage=*/0xF, "VOL", 0x0F, 0, 0);
        for (int i = 1; i <= 12; ++i) {
            putEntry(bad, 2, static_cast<size_t>(i), /*subdir entry=*/0xD,
                     "D" + std::to_string(i), 0x0F, /*key=*/4, 0);
        }

        const auto t0 = std::chrono::steady_clock::now();
        pom2::ProDOSDecodeResult c = pom2::decodeVolumeToFolder(bad, root.string());
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();

        size_t dirs = 0, deepest = 0;
        treeStats(root, dirs, deepest);
        assert(c.ok);
        assert(dirs == 1 && "only the first entry may expand a given block");
        assert(c.dirsCreated == 1);
        assert(c.dirsSkipped == 11);
        assert(ms < 2000);
    }

    // ── Part 3c: directory block chain that loops back on itself ─────────
    // The 256-iteration `guard` bounded this one, but it still replayed both
    // blocks 128 times each. Revisiting a walked block now ends the chain.
    {
        const fs::path root = base / "cycle_chain";
        std::vector<std::uint8_t> bad(16 * kBlk, 0);
        putEntry(bad, 2, 0, /*storage=*/0xF, "VOL", 0x0F, 0, 0);
        putEntry(bad, 2, 1, /*seedling=*/0x1, "F1", 0x00, /*key=*/6, /*eof=*/4);
        putEntry(bad, 3, 1, /*seedling=*/0x1, "F2", 0x00, /*key=*/7, /*eof=*/4);
        setNextBlock(bad, 2, 3);
        setNextBlock(bad, 3, 2);          // ← loop
        std::memcpy(bad.data() + 6 * kBlk, "AAAA", 4);
        std::memcpy(bad.data() + 7 * kBlk, "BBBB", 4);

        pom2::ProDOSDecodeResult c = pom2::decodeVolumeToFolder(bad, root.string());
        assert(c.ok);
        assert(c.filesWritten == 2);
        assert(c.dirsSkipped == 1 && "the chain loop must be reported");
        assert(fs::exists(root / "F1") && fs::exists(root / "F2"));
    }

    // A host file the build HID from the volume (>128 KB) is not the
    // guest's new file of the same name: the decode must not overwrite it
    // (bug hunt 2026-09-29 — `BSAVE PHOTO` replaced a 200 KB photo.bin).
    {
        const fs::path served = base / "hidden_served";
        const fs::path guest  = base / "hidden_guest";
        fs::create_directories(served);
        fs::create_directories(guest);
        { std::FILE* f = std::fopen((served / "photo.bin").string().c_str(), "wb");
          std::string big(200000, 'H'); std::fwrite(big.data(), 1, big.size(), f); std::fclose(f); }
        { std::FILE* f = std::fopen((served / "a.txt").string().c_str(), "wb");
          std::fwrite("hello", 1, 5, f); std::fclose(f); }
        std::vector<std::uint8_t> vol;
        const auto b = pom2::buildVolumeFromFolder(served.string(), "HOST", vol);
        assert(b.ok && b.filesSkipped == 1);
        const auto mountTime = fs::file_time_type::clock::now();
        // The directory the guest ends with: a.txt, plus a small PHOTO.
        { std::FILE* f = std::fopen((guest / "photo.bin").string().c_str(), "wb");
          std::fwrite("tiny", 1, 4, f); std::fclose(f); }
        { std::FILE* f = std::fopen((guest / "a.txt").string().c_str(), "wb");
          std::fwrite("hello", 1, 5, f); std::fclose(f); }
        std::vector<std::uint8_t> after;
        assert(pom2::buildVolumeFromFolder(guest.string(), "HOST", after).ok);
        const auto r = pom2::decodeVolumeToFolder(after, served.string(), &mountTime);
        assert(r.ok);
        assert(fs::file_size(served / "photo.bin") == 200000 &&
               "the guest's new PHOTO overwrote a host file it never saw");
        bool saved = false;               // the guest's file landed beside it
        for (const auto& de : fs::directory_iterator(served))
            if (de.path().filename() != "photo.bin" && de.path().filename() != "a.txt" &&
                fs::file_size(de.path()) == 4)
                saved = true;
        assert(saved && "the guest's PHOTO was not written under a fresh name");
    }

    fs::remove_all(base);
    std::printf("OK prodos_decode_safety (validator + traversal blocked, "
                "benign OK, cyclic volume bounded)\n");
    return 0;
}
