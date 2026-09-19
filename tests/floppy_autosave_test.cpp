// POM2 — GPL-3.0-or-later
//
// floppy_autosave — a mounted floppy's host file follows the guest's writes
// without an eject (MediaAutosave.h). Every check below reads the FILE while
// the disk is still in the drive: that is the property this pins. Before it,
// a 5.25" or 3.5" image reached its file only on eject / swap / quit, and any
// check of its bytes had to eject first or it verified nothing.
//
// Covers: the quiet-period autosave (5.25" .dsk and 3.5" .po), dirty state
// kept until the commit lands and not retired over a write that raced it, the
// newest capture winning over an older one committed late, two mounts of one
// image merging instead of superseding, a failed save staying dirty and
// retrying, the capture closing the rewind ring, a restored snapshot that
// predates the file being re-saved, and the controller's poll
// covering a Disk II card and the //c+ on-board drive with the machine paused.

#include "Block512Backing.h"   // pom2::mediaWriteEpoch
#include "Disk35Image.h"
#include "DiskIICard.h"
#include "DiskImage.h"
#include "EmulationController.h"
#include "BlockWriteBackExecutor.h"   // mediaCommitExecutor, drainMediaCommits
#include "MediaAutosave.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

void writeFile(const fs::path& p, const std::vector<uint8_t>& data)
{
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    assert(f.good());
}

std::vector<uint8_t> readFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

std::vector<uint8_t> dskPattern(uint8_t seed)
{
    std::vector<uint8_t> bytes(DiskImage::kBytesPerImage);
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>(i * 13 + seed);
    return bytes;
}

// Track `t` of a DOS-order .dsk: 16 logical sectors, contiguous.
std::vector<uint8_t> trackBytes(const std::vector<uint8_t>& dsk, int t)
{
    const size_t off = static_cast<size_t>(t) * 16 * 256;
    return {dsk.begin() + static_cast<std::ptrdiff_t>(off),
            dsk.begin() + static_cast<std::ptrdiff_t>(off + 16 * 256)};
}

// A decodable guest write: lay the donor's nibbles for track `t` over `img`.
void copyTrack(DiskImage& img, const DiskImage& donor, int t)
{
    for (int n = 0; n < DiskImage::kNibblesPerTrack; ++n)
        img.writeNibbleAt(t, n, donor.nibbleAt(t, n));
}

template <class Pred>
void waitFor(Pred&& done, int seconds)
{
    const auto deadline = Clock::now() + std::chrono::seconds(seconds);
    while (!done()) {
        assert(Clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

}  // namespace

int main()
{
    pom2::MediaCommitExecutor& ex = pom2::mediaCommitExecutor();
    const auto dir = fs::temp_directory_path() /
        ("pom2-floppy-autosave-" +
         std::to_string(Clock::now().time_since_epoch().count()));
    fs::create_directories(dir);

    const auto donorPath = dir / "donor.dsk";
    writeFile(donorPath, dskPattern(0x40));
    const auto donorBytes = readFile(donorPath);
    DiskImage donor;
    assert(donor.loadFile(donorPath.string()));

    // ── 5.25": the quiet-period autosave, disk still mounted ───────────────
    {
        const auto path = dir / "a.dsk";
        writeFile(path, dskPattern(0x07));
        const auto original = readFile(path);
        DiskImage img;
        img.setWriteBackEnabled(true);
        assert(img.loadFile(path.string()));
        copyTrack(img, donor, 3);
        assert(img.hasUnsavedChanges());
        assert(img.persistence().state == "pending");

        // Not before the medium has been quiet for a moment: a DOS SAVE is
        // one burst of sectors and should land as one commit.
        assert(!img.pollAutosave(ex, false));
        assert(readFile(path) == original);

        const uint64_t epoch = pom2::mediaWriteEpoch().load();
        waitFor([&] { img.pollAutosave(ex, false); return !img.hasUnsavedChanges(); }, 5);
        assert(pom2::mediaWriteEpoch().load() != epoch);   // the ring cannot span it
        const auto saved = readFile(path);
        assert(trackBytes(saved, 3) == trackBytes(donorBytes, 3));
        assert(trackBytes(saved, 4) == trackBytes(original, 4));
        assert(img.persistence().state == "saved");
        std::printf("floppy_autosave: .dsk saved while mounted OK\n");

        // A write racing the commit is not retired by its completion. The
        // commit is held deterministically on the ordering lock.
        std::shared_ptr<pom2::MediaCommitOperation> op;
        {
            std::unique_lock<std::mutex> hold(pom2::detail::MediaCommitOrder::instance().m);
            copyTrack(img, donor, 5);
            op = img.pollAutosave(ex, true);
            assert(op && !op->ready());
            assert(img.persistence().state == "saving");
            copyTrack(img, donor, 6);
        }
        assert(op->wait().ok);
        img.pollAutosave(ex, false);
        assert(img.hasUnsavedChanges());                  // track 6 is still owed
        assert(trackBytes(readFile(path), 5) == trackBytes(donorBytes, 5));
        assert(trackBytes(readFile(path), 6) != trackBytes(donorBytes, 6));
        assert(img.pollAutosave(ex, true)->wait().ok);
        img.pollAutosave(ex, false);
        assert(!img.hasUnsavedChanges());
        assert(trackBytes(readFile(path), 6) == trackBytes(donorBytes, 6));

        // An older capture committed after a newer one must not win.
        copyTrack(img, donor, 8);
        DiskImage::PendingWriteBack older;
        assert(img.takeWriteBack(older) && older.valid);
        DiskImage other;                                   // different content
        const auto otherPath = dir / "other.dsk";
        writeFile(otherPath, dskPattern(0x99));
        assert(other.loadFile(otherPath.string()));
        copyTrack(img, other, 8);
        assert(img.pollAutosave(ex, true)->wait().ok);
        std::string error;
        assert(DiskImage::commitWriteBack(std::move(older), error));  // superseded
        assert(trackBytes(readFile(path), 8) ==
               trackBytes(readFile(otherPath), 8));
        std::printf("floppy_autosave: ordering OK\n");

        // A failed save stays dirty, reports, and retries on its own.
        img.pollAutosave(ex, false);
        copyTrack(img, donor, 9);
        fs::rename(path, dir / "hidden.dsk");
        assert(!img.pollAutosave(ex, true)->wait().ok);
        img.pollAutosave(ex, false);
        assert(img.hasUnsavedChanges());
        assert(img.persistence().state == "error");
        assert(!img.persistence().error.empty());
        fs::rename(dir / "hidden.dsk", path);
        waitFor([&] { img.pollAutosave(ex, false); return !img.hasUnsavedChanges(); }, 9);
        assert(trackBytes(readFile(path), 9) == trackBytes(donorBytes, 9));
        assert(img.persistence().error.empty());

        // Write-back off: nothing is written, and the panel can say so.
        img.setWriteBackEnabled(false);
        const auto before = readFile(path);
        img.writeNibbleAt(10, 0, static_cast<uint8_t>(img.nibbleAt(10, 0) ^ 1));
        assert(!img.pollAutosave(ex, true));
        assert(img.persistence().state == "disabled");
        assert(readFile(path) == before);
        std::printf("floppy_autosave: failure / retry / disabled OK\n");
    }

    // ── A restored snapshot older than the file re-saves what it changes ───
    // The file keeps the last capture; a machine snapshot taken before it
    // restores tracks the snapshot calls clean. Left clean, the next save
    // would write only later tracks: a file made of two timelines.
    {
        const auto path = dir / "snap.dsk";
        writeFile(path, dskPattern(0x33));
        const auto original = readFile(path);
        DiskImage img;
        img.setWriteBackEnabled(true);
        assert(img.loadFile(path.string()));
        std::vector<uint8_t> snapshot;
        img.appendMediaSnapshot(snapshot);                // clean, original
        copyTrack(img, donor, 20);
        assert(img.pollAutosave(ex, true)->wait().ok);
        img.pollAutosave(ex, false);
        assert(!img.hasUnsavedChanges());
        assert(trackBytes(readFile(path), 20) == trackBytes(donorBytes, 20));
        img.loadMediaSnapshot(snapshot.data(), snapshot.size());
        assert(img.hasUnsavedChanges());
        assert(img.pollAutosave(ex, true)->wait().ok);
        img.pollAutosave(ex, false);
        assert(!img.hasUnsavedChanges());
        assert(readFile(path) == original);
        std::printf("floppy_autosave: snapshot restore re-saves OK\n");
    }

    // ── Two mounts of one image merge rather than supersede ────────────────
    {
        const auto path = dir / "shared.dsk";
        writeFile(path, dskPattern(0x11));
        DiskImage first, second;
        for (auto* d : {&first, &second}) {
            d->setWriteBackEnabled(true);
            assert(d->loadFile(path.string()));
        }
        copyTrack(first, donor, 2);
        copyTrack(second, donor, 12);
        auto a = first.pollAutosave(ex, true);
        auto b = second.pollAutosave(ex, true);
        assert(a->wait().ok && b->wait().ok);
        const auto saved = readFile(path);
        assert(trackBytes(saved, 2) == trackBytes(donorBytes, 2));
        assert(trackBytes(saved, 12) == trackBytes(donorBytes, 12));
        std::printf("floppy_autosave: two mounts merge OK\n");
    }

    // ── 3.5": same policy for the 800K image ────────────────────────────────
    {
        const auto path = dir / "b.po";
        writeFile(path, std::vector<uint8_t>(pom2::Disk35Image::kBytesPerImage));
        pom2::Disk35Image img;
        img.setWriteBackEnabled(true);
        assert(img.loadFile(path.string()));
        std::vector<uint8_t> block(512, 0x5A);
        assert(img.writeBlock(7, block.data()));
        assert(!img.pollAutosave(ex, false));
        assert(readFile(path)[7 * 512] == 0);
        waitFor([&] { img.pollAutosave(ex, false); return !img.hasUnsavedChanges(); }, 5);
        assert(readFile(path)[7 * 512] == 0x5A);
        assert(img.persistence().state == "saved");
        // The eject-style capture and the autosave share the ordering.
        block.assign(512, 0x6B);
        assert(img.writeBlock(7, block.data()));
        auto older = img.takeWriteBack();
        img.restoreDirty();
        block.assign(512, 0x7C);
        assert(img.writeBlock(7, block.data()));
        assert(img.pollAutosave(ex, true)->wait().ok);
        std::string error;
        assert(pom2::Disk35Image::commitWriteBack(std::move(older), error));
        assert(readFile(path)[7 * 512] == 0x7C);
        std::printf("floppy_autosave: 3.5\" saved while mounted OK\n");
    }

    // ── The controller polls a slot card and the //c+ drive, paused ────────
    {
        const auto path525 = dir / "slot.dsk";
        const auto path35  = dir / "onboard.po";
        writeFile(path525, dskPattern(0x22));
        writeFile(path35, std::vector<uint8_t>(pom2::Disk35Image::kBytesPerImage));
        EmulationController c;
        c.start();
        c.stop();
        auto card = std::make_unique<DiskIICard>(6);
        assert(card->insertDisk(0, path525.string()));
        card->setWriteBackEnabled(true);
        copyTrack(card->driveImage(0), donor, 17);
        {
            auto st = c.lockState();
            st.memory().slotBus().plug(6, std::move(card));
            auto& img35 = c.disk35Internal();
            img35.setWriteBackEnabled(true);
            assert(img35.loadFile(path35.string()));
            std::vector<uint8_t> block(512, 0xE1);
            assert(img35.writeBlock(0, block.data()));
        }
        waitFor([&] {
            return trackBytes(readFile(path525), 17) == trackBytes(donorBytes, 17) &&
                   readFile(path35)[0] == 0xE1;
        }, 5);
        std::string error;
        assert(c.syncFloppyMedia(error));
        assert(error.empty());
        const auto status = c.floppyPersistence();
        assert(status.size() == 2);
        for (const auto& s : status) assert(s.state == "saved" && !s.pending);

        // The sync API waits for a capture taken NOW, not the next quiet
        // period, and the machine lock is free while it waits.
        {
            auto st = c.lockState();
            auto* disk = static_cast<DiskIICard*>(st.memory().slotBus().peripheral(6));
            copyTrack(disk->driveImage(0), donor, 18);
        }
        assert(c.syncFloppyMedia(error));
        assert(trackBytes(readFile(path525), 18) == trackBytes(donorBytes, 18));
        std::printf("floppy_autosave: controller poll + sync OK\n");
    }

    pom2::drainMediaCommits();
    fs::remove_all(dir);
    std::printf("floppy_autosave: all OK\n");
    return 0;
}
