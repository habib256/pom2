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
#include "Disk35Image.h"
#include "EmulationController.h"
#include "LironCard.h"
#include "MediaNotch.h"
#include "MediaWritePolicy.h"
#include "ProDOSHardDiskCard.h"
#include "Settings.h"
#include "SlotBus.h"
#include "StorageCoordinator.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

    // ONCE: the user unprotects the disk, then a profile switch or Slot
    // Apply restores again from a store that still says `false` (nothing is
    // written until quit). That re-notched the disk silently (bug hunt
    // 2026-10-16).
    {
        SlotBus again;
        again.plug(6, std::make_unique<DiskIICard>(6));
        (void)storage.restoreMediaFromSettings(again, settings);
        auto* card2 = dynamic_cast<DiskIICard*>(again.peripheral(6));
        assert(card2->isDiskLoaded(0));
        assert(!pom2::mediaFileIsReadOnly(dsk.string()) &&
               "a second restore re-protected a disk the user unprotected");
        assert(card2->isWriteBackEnabled());
        card2->ejectDisk(0);
    }
    fs::remove(dsk, ec);
    std::puts("  ok: a legacy disk_writeback_slotN=false became the notch on its disk, once");
}

void testNotchOnDirtyFloppyStillFlushes()
{
    // The Block512Backing half is pinned by block512_notch_flush. The same
    // gate lived on DiskImage::saveDirty (isFileWriteProtected, which
    // includes the notch) and Disk35Image::takeWriteBack (isWriteProtected).
    // Guest SAVE → user ticks Write-protected → eject dropped the only copy.
    std::string err;

    {
        const fs::path dsk = scratch("pom2_notch_dirty.dsk", 143360);
        DiskImage img;
        img.setWriteBackEnabled(true);
        assert(img.loadFile(dsk.string()));
        img.writeNibbleAt(0, 0, 0xD5);
        assert(img.hasUnsavedChanges());
        assert(pom2::setMediaNotch(dsk.string(), true, err) && err.empty());
        img.setHostWriteProtected(true);
        assert(img.isFileWriteProtected());
        assert(img.saveDirty() && "saveDirty must succeed with the notch on");
        assert(!img.hasUnsavedChanges() &&
               "must not report success while the nibble is still only in RAM");
        (void)pom2::setMediaNotch(dsk.string(), false, err);
        std::error_code ec; fs::remove(dsk, ec);
        std::puts("  ok: a 5.25\" notched while dirty still flushes");
    }

    {
        const fs::path po = scratch("pom2_notch_dirty.po", 819200);
        pom2::Disk35Image img;
        img.setWriteBackEnabled(true);
        assert(img.loadFile(po.string()));
        uint8_t blk[pom2::Disk35Image::kBlockBytes];
        std::memset(blk, 0xEE, sizeof(blk));
        assert(img.writeBlock(2, blk));
        assert(img.hasUnsavedChanges());
        assert(pom2::setMediaNotch(po.string(), true, err) && err.empty());
        img.setHostWriteProtected(true);
        assert(img.isWriteProtected());
        assert(img.saveDirty());
        assert(!img.hasUnsavedChanges());
        pom2::Disk35Image reload;
        reload.setWriteBackEnabled(true);
        assert(reload.loadFile(po.string()));
        uint8_t got[pom2::Disk35Image::kBlockBytes];
        assert(reload.readBlock(2, got));
        assert(got[0] == 0xEE && "block 2 reached the file");
        (void)pom2::setMediaNotch(po.string(), false, err);
        std::error_code ec; fs::remove(po, ec);
        std::puts("  ok: a 3.5\" notched while dirty still flushes");
    }
}

// A legacy `*_writeback = false` on a bay that is EMPTY, or whose file has
// moved, is migrated too. The generic-bay restore skipped the migration on
// those paths, so the Liron bay stayed write-back-off for good and every disk
// mounted there later came up write-protected (bug hunt 2026-09-29).
void testLegacyKeyOnEmptyBayIsMigrated()
{
    pom2::Settings s;
    s.setReadOnly(true);
    s.setBool("media_slot5_bay0_writeback", false);                // empty bay
    s.setBool("media_slot5_bay1_writeback", false);
    s.setString("media_slot5_bay1_path", "/nonexistent/moved.po");  // moved file
    SlotBus bus;
    bus.plug(5, std::make_unique<pom2::LironCard>(5));
    auto* liron = dynamic_cast<pom2::LironCard*>(bus.peripheral(5));
    pom2::StorageCoordinator sc;
    (void)sc.restoreMediaFromSettings(bus, s);
    assert(liron->bayInfo(0).writeBackEnabled &&
           "a legacy false on an empty Liron bay was not migrated");
    assert(liron->bayInfo(1).writeBackEnabled &&
           "a legacy false on a Liron bay whose file moved was not migrated");
    std::puts("  ok: a legacy write-back key on an empty bay is migrated");
}

}  // namespace

// A configured disk whose file is not there at launch (an external drive not
// plugged in yet) leaves its bay empty; the quit-time writer used to record
// that empty bay as "", so the configured disk was gone for good. The restore
// reports the key, the caller holds it, and the writers keep the path.
void testUnresolvedPathSurvivesQuit()
{
    const std::string gone = "/nonexistent-pom2-volume/game.dsk";
    const std::string goneHdv = "/nonexistent-pom2-volume/big.hdv";
    pom2::StorageCoordinator coord;
    pom2::Settings settings;
    settings.setReadOnly(true);
    const std::string key = pom2::diskIIPathSettingKey(6, 0);
    settings.setString(key, gone);
    settings.setString("disk_path", gone);
    settings.setString("hdv_path", goneHdv);
    SlotBus bus;
    bus.plug(6, std::make_unique<DiskIICard>(6));
    bus.plug(5, std::make_unique<ProDOSHardDiskCard>(5));
    const auto restored = coord.restoreMediaFromSettings(bus, settings);
    assert(restored.unresolvedPathKeys.size() == 3);
    for (const auto& k : restored.unresolvedPathKeys)
        settings.holdUnresolvedPath(k);
    coord.persistSessionSettings(settings, coord.captureRebuildSnapshot(bus));
    assert(settings.getString(key, "") == gone &&
           "an unreachable Disk II path survives the quit writer");
    assert(settings.getString("disk_path", "") == gone);
    assert(settings.getString("hdv_path", "") == goneHdv &&
           "an unreachable HDV path survives the quit writer");
    // A real mount releases the hold; a later eject may then clear it.
    settings.setString(key, "/elsewhere/new.dsk");
    settings.setString(key, "");
    assert(settings.getString(key, "x").empty());
    std::puts("  ok: a path not found at launch is kept at quit");
}

int main()
{
#ifdef _WIN32
    _putenv_s("POM2_MEDIA_WRITE_DEFAULT", "");
#else
    unsetenv("POM2_MEDIA_WRITE_DEFAULT");
#endif
    testHelperAndLoader();
    testUnresolvedPathSurvivesQuit();
    testPerDiskNotPerDrive();
    testLiveToggleThroughTheCoordinator();
    testLegacyCardKeyBecomesTheNotch();
    testNotchOnDirtyFloppyStillFlushes();
    testLegacyKeyOnEmptyBayIsMigrated();
    std::puts("media_notch OK");
    return 0;
}
