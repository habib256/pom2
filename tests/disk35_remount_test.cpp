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

// The //c+ on-board 3.5" drives' two host paths that the slot-card bays had
// already been fixed for (bug hunt 2026-10-06):
//
//   1. mount35 over the SAME file under another spelling of its path (the
//      Disk Library's relative path against a file dialog's absolute one,
//      `dir/./x.po`, a symlink). A string compare decided "different file",
//      so phase 1 read the pre-flush bytes, phase 2 flushed the guest's
//      blocks and then installed that stale copy — and the next autosave
//      wrote it back over the file. `Block512Backing::sameFile` decides now.
//
//   2. eject35 while the guest keeps writing: phase 1 captured and cleared
//      the dirty flag, phase 2 committed unlocked, phase 3 ejected without
//      looking again. A block written during the commit was gone, and the
//      eject reported success. Phase 3 captures again and commits inline.

#include "Disk35Image.h"
#include "EmulationController.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::vector<std::uint8_t> readBlockFromFile(const fs::path& po, std::uint32_t idx)
{
    std::vector<std::uint8_t> out(pom2::Disk35Image::kBlockBytes, 0);
    std::ifstream f(po, std::ios::binary);
    f.seekg(static_cast<std::streamoff>(idx) * pom2::Disk35Image::kBlockBytes);
    f.read(reinterpret_cast<char*>(out.data()),
           static_cast<std::streamsize>(out.size()));
    return out;
}

void writeBlankImage(const fs::path& po, char fill)
{
    std::vector<char> bytes(pom2::Disk35Image::kBytesPerImage, fill);
    std::ofstream f(po, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

// A MacBinary-wrapped 800K image (bug hunt 2026-11-01 round 4): only the
// 5.25" loader unwrapped the envelope, and a 3.5" one was refused on its
// size. It mounts now, read-only whatever the notch says — a save would write
// the bare payload over the wrapped file — and an ordinary image whose first
// bytes happen to look like a header is not stripped.
void testMacBinaryWrapped35()
{
    const fs::path dir = fs::temp_directory_path() / "pom2_macbinary35";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::vector<uint8_t> payload(pom2::Disk35Image::kBytesPerImage);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 7 + 3);
    std::vector<uint8_t> wrapped(128, 0);
    wrapped[1] = 5;                                     // name length
    std::memcpy(wrapped.data() + 2, "DISK8", 5);        // [7] = 0 terminator
    const uint32_t fork = static_cast<uint32_t>(payload.size());
    wrapped[83] = uint8_t(fork >> 24); wrapped[84] = uint8_t(fork >> 16);
    wrapped[85] = uint8_t(fork >> 8);  wrapped[86] = uint8_t(fork);
    wrapped.insert(wrapped.end(), payload.begin(), payload.end());
    const fs::path path = dir / "wrapped.po";
    { std::ofstream f(path, std::ios::binary);
      f.write(reinterpret_cast<const char*>(wrapped.data()), std::streamsize(wrapped.size())); }

    pom2::Disk35Image img;
    img.setWriteBackEnabled(true);
    assert(img.loadFile(path.string()) && "a MacBinary-wrapped 800K image mounts");
    assert(img.isMacBinaryWrapped() && img.isWriteProtected());
    uint8_t block[512];
    assert(img.readBlock(1, block));
    assert(std::memcmp(block, payload.data() + 512, 512) == 0 && "the payload, not the header");
    img.setHostWriteProtected(false);                   // the notch cannot lift it
    assert(img.isWriteProtected());

    // An ordinary 800K image is never stripped, whatever its first bytes.
    std::vector<uint8_t> plain = payload;
    plain[0] = 0; plain[1] = 5; plain[7] = 0; plain[122] = 0; plain[123] = 0;
    const fs::path plainPath = dir / "plain.po";
    { std::ofstream f(plainPath, std::ios::binary);
      f.write(reinterpret_cast<const char*>(plain.data()), std::streamsize(plain.size())); }
    pom2::Disk35Image img2;
    assert(img2.loadFile(plainPath.string()) && !img2.isMacBinaryWrapped());
    fs::remove_all(dir, ec);
    std::printf("disk35: a MacBinary-wrapped image mounts read-only: OK\n");
}

int main()
{
    testMacBinaryWrapped35();
    const fs::path dir = fs::temp_directory_path() / "pom2_disk35_remount";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const fs::path po = dir / "x.po";
    writeBlankImage(po, 0x11);

    // ── 1. Re-insert under another spelling ──────────────────────────────
    {
        EmulationController controller;
        assert(controller.mount35(0, po.string()));
        const std::vector<std::uint8_t> block(pom2::Disk35Image::kBlockBytes, 0x9B);
        {
            auto st = controller.lockState();
            pom2::Disk35Image& image = controller.disk35Internal();
            image.setWriteBackEnabled(true);
            assert(image.writeBlock(4, block.data()));
            assert(image.hasUnsavedChanges());
        }
        // The same file, spelled through a "." component: equal to
        // std::filesystem::equivalent, not to operator==.
        const std::string otherSpelling = (dir / "." / "x.po").string();
        assert(otherSpelling != po.string());
        assert(controller.mount35(0, otherSpelling));
        {
            auto st = controller.lockState();
            pom2::Disk35Image& image = controller.disk35Internal();
            std::vector<std::uint8_t> got(pom2::Disk35Image::kBlockBytes, 0);
            assert(image.isLoaded());
            assert(image.readBlock(4, got.data()));
            if (got != block) {
                std::puts("FAIL: re-inserting the disk under another spelling "
                          "of its path installed the pre-flush image");
                return 1;
            }
            assert(!image.hasUnsavedChanges() && "the flush landed first");
        }
        assert(readBlockFromFile(po, 4) == block && "the flush reached the file");
    }

    // ── 2. Eject while the guest writes ──────────────────────────────────
    // A writer thread stamps block 4 with a running counter under the
    // machine lock for as long as the medium accepts it; the host eject runs
    // on this thread. Whatever the LAST accepted write was, it was in
    // memory when the eject retired the medium, so the file must hold it:
    // the pre-fix phase 3 dropped every write that landed during the
    // unlocked commit. Several rounds, because the window is the commit's
    // own duration (an 800 KB write plus fsync).
    for (int round = 0; round < 6; ++round) {
        writeBlankImage(po, 0x11);
        EmulationController controller;
        assert(controller.mount35(0, po.string()));
        {
            auto st = controller.lockState();
            controller.disk35Internal().setWriteBackEnabled(true);
        }
        std::atomic<bool> stop{false};
        std::atomic<int>  lastAccepted{-1};
        std::thread writer([&] {
            for (int n = 0; !stop.load(std::memory_order_relaxed); ++n) {
                // The write's serial, little-endian in the block's first
                // two bytes (the rest is filler), so no wrap can alias it.
                std::vector<std::uint8_t> b(pom2::Disk35Image::kBlockBytes, 0xEE);
                b[0] = static_cast<std::uint8_t>(n & 0xFF);
                b[1] = static_cast<std::uint8_t>((n >> 8) & 0xFF);
                auto st = controller.lockState();
                pom2::Disk35Image& image = controller.disk35Internal();
                if (!image.isLoaded()) break;
                if (image.writeBlock(4, b.data()))
                    lastAccepted.store(n, std::memory_order_relaxed);
            }
        });
        // Let the writer dirty the medium at least once before the eject.
        while (lastAccepted.load(std::memory_order_relaxed) < 0)
            std::this_thread::yield();
        const bool ejected = controller.eject35(0);
        stop.store(true, std::memory_order_relaxed);
        writer.join();
        controller.drainDeferredWriteBacks();

        auto st = controller.lockState();
        pom2::Disk35Image& image = controller.disk35Internal();
        const int last = lastAccepted.load(std::memory_order_relaxed);
        if (!ejected) {
            // Refused only when the late commit failed — then the medium
            // must still be there with its writes.
            if (!image.isLoaded() || !image.hasUnsavedChanges()) {
                std::puts("FAIL: eject refused and the medium is gone");
                return 1;
            }
            std::printf("round %d: eject refused, medium kept dirty\n", round);
            continue;
        }
        assert(!image.isLoaded());
        const auto onDisk = readBlockFromFile(po, 4);
        // The block carries the serial's low 16 bits; compare on those.
        const int inFile = onDisk[0] | (onDisk[1] << 8);
        if (inFile != (last & 0xFFFF)) {
            std::printf("FAIL: round %d — last accepted write was #%d, the "
                        "file holds #%d: a block written during the eject's "
                        "commit was dropped\n", round, last, inFile);
            return 1;
        }
    }

    fs::remove_all(dir, ec);
    std::puts("disk35_remount_test: OK");
    return 0;
}
