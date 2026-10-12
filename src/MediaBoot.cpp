// POM2 — GPL-3.0-or-later
#include "MediaBoot.h"
#include "DiskIICard.h"
#include "EmulationController.h"
#include "Memory.h"
#include "MountableMediaCard.h"
#include "Sony35Drive.h"

namespace pom2 {
bool bootMountedMedia(EmulationController& controller, int slot, int bay,
                      std::string& error) {
    error.clear();
    {
        auto state = controller.lockState();
        if (slot < 0) {
            if (slot != -1 || !state.memory().isIIcPlus() ||
                bay < 0 || bay >= EmulationController::kSony35Drives ||
                !controller.sony35(bay).isInserted()) {
                error = "No disk in the selected Sony drive.";
                return false;
            }
            // The real //c+ reset scans Sony mechanisms in physical order.
            // It cannot explicitly select an external Sony ahead of one
            // already holding media; leave the machine running as it was.
            for (int earlier = 0; earlier < bay; ++earlier) {
                if (controller.sony35(earlier).isInserted()) {
                    error = "Eject disks from earlier Sony drives before booting this drive.";
                    return false;
                }
            }
        } else {
            if (slot == 3 && state.memory().isIIE()) {
                error = "Slot 3 storage firmware is masked at reset on this machine. Use another storage slot.";
                return false;
            }
            auto* card = state.memory().slotBus().peripheral(slot);
            const auto* floppy = dynamic_cast<const DiskIICard*>(card);
            const auto* media = dynamic_cast<const MountableMediaCard*>(card);
            const bool loaded = floppy ? DiskIICard::validDrive(bay) && floppy->isDiskLoaded(bay)
                : media && bay >= 0 && bay < media->bayCount() && media->bayInfo(bay).loaded;
            if (!loaded) {
                error = "No disk in the selected drive.";
                return false;
            }
            // The slot entry points start drive 1 / unit 1. Moving the
            // image or reordering the chain would change the user's media.
            if (bay != 0) {
                error = "This controller's boot entry selects drive 1 / unit 1. Mount the boot image there.";
                return false;
            }
            if (slot == 5 && state.memory().iicPlusBootsSlot5ByReset()) {
                // bootFromSlot(5) must use the //c+ reset scan to initialise
                // the real SmartPort firmware. Its higher-priority Sony
                // disks would otherwise override this HD/UniDisk.
                for (int drive = 0; drive < EmulationController::kSony35Drives; ++drive) {
                    if (controller.sony35(drive).isInserted()) {
                        error = "Eject Sony disks before booting the external SmartPort image on //c+.";
                        return false;
                    }
                }
            }
        }
    }
    if (slot < 0) {
        controller.coldBoot();
        controller.setMode(EmulationController::Mode::Running);
        return true;
    }
    if (controller.bootFromSlot(slot)) return true;
    error = "The selected slot has no boot entry; the machine restarted through its normal ROM scan.";
    return false;
}
}
