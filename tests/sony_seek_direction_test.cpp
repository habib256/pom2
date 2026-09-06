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

// Sony 3.5" head step-direction regression test.
//
// MAME `mac_floppy_device::seek_phase_w`: register 0x0 "DirNext" sets the
// step direction OUTWARD (cyl+1), register 0x4 "DirPrev" sets it INWARD
// (toward track 0), register 0x1 issues a step pulse. POM2 previously
// mapped 0x0→inward and 0x4→eject with NO outward path, so `directionIn_`
// could never become false and the head could only ever step toward track
// 0 (the cyl+1 branch was dead code) — 3.5" data tracks 1-79 unreachable.
//
// Drives Sony35Drive directly through its public seekPhaseW() the way the
// IWM would (CA0-2 = register, PH3/LSTRB = strobe), and checks track().

#include "Disk35Image.h"
#include "Sony35Drive.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using pom2::Sony35Drive;

namespace {

// Strobe drive register `reg` (0..7): drop LSTRB (bit 3) with CA bits =
// reg, then raise LSTRB → fires on the rising edge (matches MAME).
void strobe(Sony35Drive& d, uint8_t reg) {
    d.seekPhaseW(static_cast<uint8_t>(reg & 0x07), 0);          // LSTRB low
    d.seekPhaseW(static_cast<uint8_t>((reg & 0x07) | 0x08), 0); // LSTRB high → strobe
}

constexpr uint8_t kDirOut = 0x0;   // DirNext  → cyl+1 (outward)
constexpr uint8_t kStep   = 0x1;   // StepOn
constexpr uint8_t kDirIn  = 0x4;   // DirPrev  → cyl-1 (toward track 0)
constexpr uint8_t kEject  = 0x7;   // EjectOn

/// Collects what the firmware eject hands over, standing in for
/// `EmulationController`'s committer thread — including the completion it
/// owes back, which is what decides whether the disk actually leaves the bay.
class CapturingSink final : public pom2::Disk35WriteBackSink
{
public:
    void submit(pom2::Disk35Image::PendingWriteBack&& p,
                Completion onDone = {}) override
    {
        got.push_back(std::move(p));
        done.push_back(std::move(onDone));
    }
    std::vector<pom2::Disk35Image::PendingWriteBack> got;
    std::vector<Completion>                          done;
};

/// A sink whose commit always fails, reported the way the real queue reports
/// it: on the completion, with the machine lock held.
class FailingSink final : public pom2::Disk35WriteBackSink
{
public:
    void submit(pom2::Disk35Image::PendingWriteBack&& p,
                Completion onDone = {}) override
    {
        captured = std::move(p);
        if (onDone) onDone(false, "disk full (test)");
    }
    pom2::Disk35Image::PendingWriteBack captured;
};

/// The firmware-issued eject runs on the CPU worker with `stateMutex` held,
/// so it must not touch the filesystem: it captures the payload and lets the
/// host commit it off the lock. (Bug hunt 2026-09-06 #13.)
bool testFirmwareEjectDefersItsWriteBack()
{
    namespace fs = std::filesystem;
    const fs::path img = fs::temp_directory_path() / "pom2_sony_eject.po";
    {
        std::vector<uint8_t> bytes(pom2::Disk35Image::kBytesPerImage, 0x11);
        std::ofstream f(img, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    std::vector<uint8_t> pristine(pom2::Disk35Image::kBytesPerImage);
    {
        std::ifstream f(img, std::ios::binary);
        f.read(reinterpret_cast<char*>(pristine.data()),
               static_cast<std::streamsize>(pristine.size()));
    }

    pom2::Disk35Image image;
    if (!image.loadFile(img.string())) {
        std::printf("FAIL: could not load the 800K fixture\n"); return false;
    }
    image.setWriteBackEnabled(true);
    const std::vector<uint8_t> block(pom2::Disk35Image::kBlockBytes, 0x9B);
    if (!image.writeBlock(4, block.data())) {
        std::printf("FAIL: guest write refused\n"); return false;
    }

    CapturingSink sink;
    Sony35Drive drv;
    drv.setImage(&image);
    drv.setWriteBackSink(&sink);
    strobe(drv, kEject);

    if (sink.got.size() != 1 || !sink.got[0].valid) {
        std::printf("FAIL: firmware eject did not hand over a payload\n");
        return false;
    }
    // Queued is NOT saved: the disk stays in the bay until the sink reports.
    // It used to leave on the spot, so a commit that failed logged a line
    // about a disk that no longer existed. (Bug hunt #2 R1.)
    if (!image.isLoaded()) {
        std::printf("FAIL: the medium left the bay before the commit\n");
        return false;
    }
    // The whole point: nothing was written while the (notional) lock was held.
    std::vector<uint8_t> afterEject(pom2::Disk35Image::kBytesPerImage);
    {
        std::ifstream f(img, std::ios::binary);
        f.read(reinterpret_cast<char*>(afterEject.data()),
               static_cast<std::streamsize>(afterEject.size()));
    }
    if (afterEject != pristine) {
        std::printf("FAIL: the eject strobe wrote to the file itself\n");
        return false;
    }

    std::string error;
    if (!pom2::Disk35Image::commitWriteBack(std::move(sink.got[0]), error)) {
        std::printf("FAIL: deferred commit: %s\n", error.c_str());
        return false;
    }
    // …and the completion is what finishes the eject.
    if (sink.done.empty() || !sink.done[0]) {
        std::printf("FAIL: no completion was handed back\n");
        return false;
    }
    sink.done[0](true, std::string());
    if (image.isLoaded()) {
        std::printf("FAIL: a committed write-back did not eject the disk\n");
        return false;
    }
    std::vector<uint8_t> committed(pom2::Disk35Image::kBytesPerImage);
    {
        std::ifstream f(img, std::ios::binary);
        f.read(reinterpret_cast<char*>(committed.data()),
               static_cast<std::streamsize>(committed.size()));
    }
    if (std::memcmp(committed.data() + 4 * pom2::Disk35Image::kBlockBytes,
                    block.data(), block.size()) != 0) {
        std::printf("FAIL: the deferred commit lost the guest's block\n");
        return false;
    }

    // ── A commit that FAILS must not cost the user the disk ──────────────
    // The sinkless branch has always refused the eject on a failed save; the
    // sink path ejected unconditionally and only logged. Now the medium stays
    // in the bay AND stays dirty, so the next eject (or the shutdown flush)
    // tries again.
    if (!image.loadFile(img.string())) {
        std::printf("FAIL: could not re-load the fixture\n"); return false;
    }
    image.setWriteBackEnabled(true);
    const std::vector<uint8_t> block2(pom2::Disk35Image::kBlockBytes, 0x5C);
    if (!image.writeBlock(6, block2.data())) {
        std::printf("FAIL: guest write refused (retry case)\n"); return false;
    }
    FailingSink failing;
    Sony35Drive drv2;
    drv2.setImage(&image);
    drv2.setWriteBackSink(&failing);
    strobe(drv2, kEject);
    if (!failing.captured.valid) {
        std::printf("FAIL: no payload reached the failing sink\n");
        return false;
    }
    if (!image.isLoaded()) {
        std::printf("FAIL: a FAILED write-back still ejected the disk\n");
        return false;
    }
    if (!image.hasUnsavedChanges()) {
        std::printf("FAIL: a failed write-back left the medium clean — the "
                    "session's writes would never be retried\n");
        return false;
    }

    std::error_code ec;
    fs::remove(img, ec);
    std::printf("OK sony firmware eject defers its write-back off the lock, "
                "and a failed commit keeps the disk\n");
    return true;
}

}  // namespace

int main()
{
    Sony35Drive drv;
    assert(drv.track() == 0);

    // Outward: 5 steps → track 5.
    strobe(drv, kDirOut);
    for (int i = 0; i < 5; ++i) strobe(drv, kStep);
    if (drv.track() != 5) {
        std::printf("FAIL: outward 5 steps → track %d (want 5)\n", drv.track());
        return 1;
    }

    // Inward: 3 steps → track 2.
    strobe(drv, kDirIn);
    for (int i = 0; i < 3; ++i) strobe(drv, kStep);
    if (drv.track() != 2) {
        std::printf("FAIL: inward 3 steps → track %d (want 2)\n", drv.track());
        return 1;
    }

    // Inward past 0 clamps at track 0.
    for (int i = 0; i < 10; ++i) strobe(drv, kStep);
    if (drv.track() != 0) {
        std::printf("FAIL: inward clamp → track %d (want 0)\n", drv.track());
        return 1;
    }

    // Outward past 79 clamps at track 79.
    strobe(drv, kDirOut);
    for (int i = 0; i < 200; ++i) strobe(drv, kStep);
    if (drv.track() != 79) {
        std::printf("FAIL: outward clamp → track %d (want 79)\n", drv.track());
        return 1;
    }

    if (!testFirmwareEjectDefersItsWriteBack()) return 1;

    std::printf("OK sony_seek_direction (outward + inward + clamps)\n");
    return 0;
}
