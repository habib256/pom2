// StorageCoordinator_HdvBays.cpp — an HDV mounted from the library ADDS a
// volume; it does not replace the one in unit 0.
//
// A user with a SmartPort card set to four, six or eight units (2026-09-08)
// mounts a second hard disk the way the real machine takes one: into the
// next free bay. The library's "Mount only" used to route every HDV to the
// dedicated block card or to SmartPort unit 0, so the second mount silently
// replaced the first. Own translation unit for the file-size ratchet:
// StorageCoordinator.cpp is a god-object already.

#include "StorageCoordinator.h"

#include "EmulationController.h"
#include "ProDOSBlockCard.h"
#include "SlotBus.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "SmartPortUnit.h"

#include <string>

namespace pom2 {

StorageCoordinator::RoutedMediaCommandResult
StorageCoordinator::mountHdvIntoFreeBay(EmulationController& controller,
                                        Settings& settings,
                                        const std::string& path) const
{
    RoutedMediaCommandResult result;
    int  slot = -1, bay = 0;
    bool needsType = false;
    int  busyUnits = 0, unitCount = 0;
    {
        auto state = controller.lockState();
        const auto cards = topology(state.memory().slotBus());
        // The dedicated block card first, and only while it is empty: it is
        // the boot device on a machine that has one.
        if (auto* block = cards.preferredBlock(); block && !block->isImageLoaded()) {
            slot = block->getSlot();
        } else if (auto* sp = cards.primarySmartPort) {
            unitCount = sp->unitCount();
            for (int b = 0; b < unitCount; ++b) {
                const SmartPortUnit* u = sp->unit(static_cast<std::size_t>(b));
                if (!u) { slot = sp->getSlot(); bay = b; needsType = true; break; }
                if (u->kindKey() == SmartPortHdvUnit::kKindKey && !u->isLoaded()) {
                    slot = sp->getSlot(); bay = b; break;
                }
                ++busyUnits;
            }
        }
    }
    if (slot < 0) {
        if (unitCount > 0)
            result.error = "every SmartPort unit holds an image (" +
                           std::to_string(busyUnits) + " of " + std::to_string(unitCount) +
                           ") — eject one, or raise the unit count in the SmartPort panel";
        else
            result.error = "no free HDV bay: plug an HDV or SmartPort card";
        return result;
    }
    if (needsType) {
        const auto t = setMediaBayType(controller, settings, slot, bay,
                                       std::string(SmartPortHdvUnit::kKindKey));
        if (!t.ok) { result.error = t.error; return result; }
    }
    const auto m = mountMediaBay(controller, settings, slot, bay, path);
    if (!m.ok) { result.error = m.error; return result; }
    result.ok = true;
    result.bootSlot = slot;
    result.usesSmartPort = (bay > 0) || needsType || unitCount > 0;
    return result;
}

}  // namespace pom2
