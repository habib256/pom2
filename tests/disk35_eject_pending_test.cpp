// A host eject must not overtake a firmware eject still being saved.
//
// The firmware eject (`Sony35Drive` register 7) runs on the CPU worker under
// `stateMutex`, so it only CAPTURES the medium's writes and queues them on
// `EmulationController`'s write-back thread; the medium stays in the bay
// until the commit reports, so a failed commit can hand the writes back
// (`restoreDirty`). `EmulationController::eject35` did not look at that
// pending state: it found a clean medium (the writes were already lifted
// out), dropped it on the spot, and when the queued commit then failed there
// was nothing left to hand them back to — the only copy, gone. Bug hunt
// 2026-09-16. The host eject now waits for the queued commit first.
//
// The failure is a read-only parent directory: the atomic replace cannot
// create its scratch file beside the image. Skips (77) where that does not
// fail (running as root).

#include "Disk35Image.h"
#include "EmulationController.h"
#include "Sony35Drive.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main()
{
    const fs::path dir = fs::temp_directory_path() / "pom2_eject_pending";
    std::error_code ec;
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::add, ec);
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const fs::path po = dir / "pending.po";
    {
        std::vector<char> bytes(pom2::Disk35Image::kBytesPerImage, 0x11);
        std::ofstream f(po, std::ios::binary | std::ios::trunc);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    const auto cleanup = [&] {
        fs::permissions(dir, fs::perms::owner_all, fs::perm_options::add, ec);
        fs::remove_all(dir, ec);
    };

    // Probe the failure before relying on it.
    fs::permissions(dir, fs::perms::owner_write | fs::perms::group_write |
                         fs::perms::others_write,
                    fs::perm_options::remove, ec);
    {
        std::ofstream probe(dir / "probe");
        if (probe) {
            std::puts("SKIP: a read-only directory is writable here (root?)");
            cleanup();
            return 77;
        }
    }
    fs::permissions(dir, fs::perms::owner_write, fs::perm_options::add, ec);

    EmulationController controller;
    assert(controller.mount35(0, po.string()));
    {
        auto st = controller.lockState();
        pom2::Disk35Image& image = controller.disk35Internal();
        image.setWriteBackEnabled(true);
        const std::vector<std::uint8_t> block(pom2::Disk35Image::kBlockBytes, 0x9B);
        assert(image.writeBlock(4, block.data()));
        assert(image.hasUnsavedChanges());

        // Make the commit fail, then strobe EjectOn the way the IWM does.
        fs::permissions(dir, fs::perms::owner_write | fs::perms::group_write |
                             fs::perms::others_write,
                        fs::perm_options::remove, ec);
        pom2::Sony35Drive& drive = controller.sony35Internal();
        drive.seekPhaseW(0x07, 0);
        drive.seekPhaseW(0x0F, 0);
        assert(drive.isEjectPending() && "the firmware eject is queued");
    }

    // The host eject lands while that commit is (most likely) still queued.
    const bool ejected = controller.eject35(0);
    controller.drainDeferredWriteBacks();
    {
        auto st = controller.lockState();
        pom2::Disk35Image& image = controller.disk35Internal();
        if (ejected || !image.isLoaded() || !image.hasUnsavedChanges()) {
            std::printf("FAIL: ejected=%d loaded=%d dirty=%d — the host eject "
                        "overtook a failing firmware eject and the guest's "
                        "writes are gone\n",
                        int(ejected), int(image.isLoaded()),
                        int(image.hasUnsavedChanges()));
            cleanup();
            return 1;
        }
    }

    // A host MOUNT over a queued firmware eject (2026-09-17): the same hole
    // from the other side. The medium looked clean, so disk B replaced it
    // with no flush, and when the queued commit then failed its completion
    // found another disk in the bay — the writes were dropped.
    {
        const fs::path other = fs::temp_directory_path() / "pom2_eject_pending_other.po";
        {
            std::vector<char> bytes(pom2::Disk35Image::kBytesPerImage, 0x22);
            std::ofstream f(other, std::ios::binary | std::ios::trunc);
            f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        {
            auto st = controller.lockState();
            pom2::Sony35Drive& drive = controller.sony35Internal();
            drive.seekPhaseW(0x07, 0);
            drive.seekPhaseW(0x0F, 0);
            assert(drive.isEjectPending() && "the firmware eject is queued again");
        }
        const bool mounted = controller.mount35(0, other.string());
        controller.drainDeferredWriteBacks();
        {
            auto st = controller.lockState();
            pom2::Disk35Image& image = controller.disk35Internal();
            if (mounted || !image.isLoaded() || image.path() != po.string() ||
                !image.hasUnsavedChanges()) {
                std::printf("FAIL: mounted=%d path=%s dirty=%d — a host mount "
                            "overtook a failing firmware eject\n",
                            int(mounted), image.path().c_str(),
                            int(image.hasUnsavedChanges()));
                fs::remove(other, ec);
                cleanup();
                return 1;
            }
        }
        fs::remove(other, ec);
    }

    // Fix the cause: the retry saves and ejects.
    fs::permissions(dir, fs::perms::owner_write, fs::perm_options::add, ec);
    assert(controller.eject35(0));
    assert(!controller.disk35Internal().isLoaded());
    {
        std::ifstream f(po, std::ios::binary);
        std::vector<char> bytes(pom2::Disk35Image::kBytesPerImage);
        f.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        assert(static_cast<unsigned char>(
                   bytes[4 * pom2::Disk35Image::kBlockBytes]) == 0x9B);
    }
    cleanup();
    std::puts("OK: a host eject or mount waits for the firmware eject's commit");
    return 0;
}
