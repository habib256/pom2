// The notch is on the disk (MediaNotch.h) — 2026-09-08.
//
// A 5.25" is write-protected by a sticker over its sleeve's notch and a
// 3.5" by its tab: the protection belongs to the DISK and follows it into
// any drive. POM2 models it as the image file's host write permission.
// Pins: (1) the helper sets/clears the bit and the loader sees it; (2) two
// disks in ONE Disk II card carry different protections, and the
// protection follows the disk when it changes drive; (3) the coordinator's
// setMediaNotch flips the file AND every mounted copy live — 5.25", HDV
// and on-board 3.5"; (4) a legacy per-card `disk_writeback_slotN = false`
// becomes the notch on that card's disk, once, and the flag reverts.

#include "DiskIICard.h"
#include "DiskImage.h"
#include "EmulationController.h"
#include "MediaNotch.h"
#include "MediaWritePolicy.h"
#include "ProDOSHardDiskCard.h"
#include "Settings.h"
#include "SlotBus.h"
#include "StorageCoordinator.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path scratch(const char* name, std::size_t bytes)
{
    const fs::path p = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove(p, ec);
    std::vector<char> zeros(bytes, 0);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    f.close();
    // A leftover read-only scratch from an aborted run must not skew us.
    std::string err;
    (void)pom2::setMediaNotch(p.string(), false, err);
    return p;
}

void testHelperAndLoader()
{
    const fs::path dsk = scratch("pom2_notch_a.dsk", 143360);
    std::string err;
    assert(!pom2::mediaFileIsReadOnly(dsk.string()));
    assert(pom2::setMediaNotch(dsk.string(), true, err) && err.empty());
    assert(pom2::mediaFileIsReadOnly(dsk.string()));
    {
        DiskImage img;
        assert(img.loadFile(dsk.string()));
        assert(img.isFileWriteProtected() && "the loader must see the notch");
        assert(img.isHostWriteProtected());
        // ...and the write current is inhibited, not just reported: the
        // first cut kept the write gates on the header flag alone.
        img.setWriteBackEnabled(true);
        img.writeNibbleAt(0, 0, 0xD5);
        assert(!img.hasUnsavedChanges() && "a notched disk took a write");
    }
    assert(pom2::setMediaNotch(dsk.string(), false, err));
    assert(!pom2::mediaFileIsReadOnly(dsk.string()));
    {
        DiskImage img;
        assert(img.loadFile(dsk.string()));
        assert(!img.isFileWriteProtected());
    }
    std::error_code ec; fs::remove(dsk, ec);
    std::puts("  ok: the helper sets and clears the bit, the loader honours it");
}

void testPerDiskNotPerDrive()
{
    const fs::path a = scratch("pom2_notch_prot.dsk", 143360);
    const fs::path b = scratch("pom2_notch_free.dsk", 143360);
    std::string err;
    assert(pom2::setMediaNotch(a.string(), true, err));

    DiskIICard card(6);
    assert(card.insertDisk(0, a.string()));
    assert(card.insertDisk(1, b.string()));
    assert(card.isFileWriteProtected(0) && "the sticker is on disk A");
    assert(!card.isFileWriteProtected(1) && "disk B in the SAME card is writable");
    // Swap drives: the protection moves with the disk, not the drive.
    assert(card.insertDisk(1, a.string()));
    assert(card.insertDisk(0, b.string()));
    assert(!card.isFileWriteProtected(0));
    assert(card.isFileWriteProtected(1) && "the sticker followed disk A to drive 2");
    card.ejectDisk(0); card.ejectDisk(1);

    std::error_code ec;
    (void)pom2::setMediaNotch(a.string(), false, err);
    fs::remove(a, ec); fs::remove(b, ec);
    std::puts("  ok: two disks in one card, two protections; it follows the disk");
}

void testLiveToggleThroughTheCoordinator()
{
    const fs::path dsk = scratch("pom2_notch_live.dsk", 143360);
    const fs::path hdv = scratch("pom2_notch_live.hdv", 512 * 64);
    const fs::path po  = scratch("pom2_notch_live.po",  819200);

    EmulationController controller;
    pom2::StorageCoordinator storage;
    DiskIICard* disk = nullptr;
    ProDOSHardDiskCard* block = nullptr;
    {
        auto st = controller.lockState();
        auto d = std::make_unique<DiskIICard>(6);  disk  = d.get();
        auto h = std::make_unique<ProDOSHardDiskCard>(7); block = h.get();
        st.memory().slotBus().plug(6, std::move(d));
        st.memory().slotBus().plug(7, std::move(h));
        assert(disk->insertDisk(0, dsk.string()));
        assert(block->loadImage(hdv.string()));
    }
    assert(controller.mount35(0, po.string()));
    assert(!disk->isFileWriteProtected(0));
    assert(!block->isWriteProtected());
    assert(!controller.disk35Internal().isFileWriteProtected());

    for (const fs::path* p : { &dsk, &hdv, &po }) {
        const auto r = storage.setMediaNotch(controller, p->string(), true);
        assert(r.ok && "setMediaNotch(protect) failed");
        assert(pom2::mediaFileIsReadOnly(p->string()) && "the file must carry it");
    }
    assert(disk->isFileWriteProtected(0) && "the mounted 5.25\" did not follow the file");
    assert(block->isWriteProtected() && "the mounted HDV did not follow the file");
    assert(controller.disk35Internal().isFileWriteProtected() && "the mounted 3.5\" did not follow");

    for (const fs::path* p : { &dsk, &hdv, &po }) {
        const auto r = storage.setMediaNotch(controller, p->string(), false);
        assert(r.ok);
        assert(!pom2::mediaFileIsReadOnly(p->string()));
    }
    assert(!disk->isFileWriteProtected(0));
    assert(!block->isWriteProtected());
    assert(!controller.disk35Internal().isFileWriteProtected());

    // A path that is not a file is refused, and nothing changes.
    assert(!storage.setMediaNotch(controller, "/nonexistent/pom2_notch.dsk", true).ok);
    assert(!storage.setMediaNotch(controller, "", true).ok);

    {
        auto st = controller.lockState();
        disk->ejectDisk(0);
        block->ejectImage();
    }
    controller.disk35Internal().eject();
    std::error_code ec;
    fs::remove(dsk, ec); fs::remove(hdv, ec); fs::remove(po, ec);
    std::puts("  ok: setMediaNotch flips the file and every mounted copy, both ways");
}

void testLegacyCardKeyBecomesTheNotch()
{
    // Product default (writable) — the migration is a no-op under the
    // suite's protected default, and this case is about the product.
    assert(pom2::mediaWritableByDefault());
    const fs::path dsk = scratch("pom2_notch_legacy.dsk", 143360);

    pom2::Settings settings;
    settings.setReadOnly(true);
    settings.setString("disk_path_slot6", dsk.string());
    settings.setBool("disk_writeback_slot6", false);   // the pre-notch opt-out

    SlotBus bus;
    bus.plug(6, std::make_unique<DiskIICard>(6));
    auto* card = dynamic_cast<DiskIICard*>(bus.peripheral(6));
    pom2::StorageCoordinator storage;
    const auto r = storage.restoreMediaFromSettings(bus, settings);
    for (const auto& w : r.warnings) std::printf("    warning: %s\n", w.c_str());
    assert(r.ok());
    assert(card->isDiskLoaded(0));
    assert(pom2::mediaFileIsReadOnly(dsk.string()) && "the old key must become the sticker");
    assert(card->isFileWriteProtected(0));
    assert(card->isWriteBackEnabled() && "the per-card flag reverts to the default");

    card->ejectDisk(0);
    std::string err; std::error_code ec;
    (void)pom2::setMediaNotch(dsk.string(), false, err);
    fs::remove(dsk, ec);
    std::puts("  ok: a legacy disk_writeback_slotN=false became the notch on its disk");
}

}  // namespace

int main()
{
#ifdef _WIN32
    _putenv_s("POM2_MEDIA_WRITE_DEFAULT", "");
#else
    unsetenv("POM2_MEDIA_WRITE_DEFAULT");
#endif
    testHelperAndLoader();
    testPerDiskNotPerDrive();
    testLiveToggleThroughTheCoordinator();
    testLegacyCardKeyBecomesTheNotch();
    std::puts("media_notch OK");
    return 0;
}
