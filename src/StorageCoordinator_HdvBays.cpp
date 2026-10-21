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
    bool blockTarget = false;
    int  busyUnits = 0, unitCount = 0;
    {
        auto state = controller.lockState();
        const auto cards = topology(state.memory().slotBus());
        // The dedicated block card first, and only while it has a free
        // drive: drive 1 is the boot device on a machine that has one, and
        // a ProDOS HD card has a drive 2 behind it (2026-09-11).
        int freeBlockBay = -1;
        auto* block = cards.preferredBlock();
        if (block)
            for (int b = 0; b < block->bayCount() && freeBlockBay < 0; ++b)
                if (!block->bayInfo(b).loaded) freeBlockBay = b;
        if (freeBlockBay >= 0) {
            slot = block->getSlot();
            bay  = freeBlockBay;
            blockTarget = true;
        } else if (auto* sp = cards.primarySmartPort) {
            unitCount = sp->unitCount();
            // Unit 1 is the boot unit, on every machine this card serves:
            // the //c firmware's boot call at $C52D-$C536 names unit 1 only
            // (param block $C570 = 01 50 00 08 00 00), and the synthetic
            // $Cn20 boot of a //e card reads drive 1 of the slot. The loop
            // below skips a unit that is not HDV-typed, so with unit 1 an
            // EMPTY 3.5" bay the first hard disk landed in unit 2 and the
            // machine never booted it ("Check Disk Drive." on a //c), while
            // "Mount + boot" (mountHdv, bay 0) retyped unit 1 and worked.
            // So when nothing on the chain holds a medium yet, an empty
            // non-HDV unit 1 is retyped and takes the disk. With a medium
            // anywhere, the user has a boot volume (or is building a chain
            // around one) and the next free HDV unit is still the answer.
            bool anyLoaded = false;
            for (int b = 0; b < unitCount && !anyLoaded; ++b)
                if (const SmartPortUnit* u = sp->unit(static_cast<std::size_t>(b)))
                    anyLoaded = u->isLoaded();
            const SmartPortUnit* u0 = unitCount > 0 ? sp->unit(0) : nullptr;
            bool u0TakesHdv = false;
            if (u0)
                for (const auto& option : sp->bayTypeOptions(0))
                    if (option.first == SmartPortHdvUnit::kKindKey) u0TakesHdv = true;
            if (!anyLoaded && u0 && !u0->isLoaded() && u0TakesHdv &&
                u0->kindKey() != SmartPortHdvUnit::kKindKey) {
                slot = sp->getSlot(); bay = 0; needsType = true;
            }
            for (int b = 0; b < unitCount && slot < 0; ++b) {
                const SmartPortUnit* u = sp->unit(static_cast<std::size_t>(b));
                if (!u) { slot = sp->getSlot(); bay = b; needsType = true; break; }
                if (u->kindKey() == SmartPortHdvUnit::kKindKey && !u->isLoaded()) {
                    slot = sp->getSlot(); bay = b; break;
                }
                ++busyUnits;
            }
        } else if (const int ls = lironSlot(state.memory().slotBus()); ls >= 0) {
            // A Liron's chain takes hard disks too (2026-09-11): the first
            // empty unit of the count the guest sees.
            const auto* liron = dynamic_cast<const MountableMediaCard*>(
                state.memory().slotBus().peripheral(ls));
            unitCount = liron->bayCount();
            for (int b = 0; b < unitCount && slot < 0; ++b) {
                if (!liron->bayInfo(b).loaded) { slot = ls; bay = b; blockTarget = true; }
                else ++busyUnits;
            }
        }
    }
    if (slot < 0) {
        if (unitCount > 0)
            result.error = "every SmartPort unit holds an image (" +
                           std::to_string(busyUnits) + " of " + std::to_string(unitCount) +
                           ") — eject one, or raise the card's unit count";
        else
            result.error = "no free HDV bay: plug an HDV, SmartPort or Liron card";
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
    result.usesSmartPort = !blockTarget && ((bay > 0) || needsType || unitCount > 0);
    return result;
}

}  // namespace pom2

namespace pom2 {

// A SmartPort bay: the fresh-install //e has one in slot 5 and no free slot
// for an HDV card (slot 3 is the 80 columns), and the //c's on-board one is
// the only drive its firmware boots. The bay must be empty — a host folder
// never replaces a disk.
std::string StorageCoordinator::mountHostFolderOnSmartPort(
    SmartPortCard& card, int bay, std::vector<std::uint8_t>&& bytes,
    const std::string& label, const std::string& hostFolder, bool convertText)
{
    if (bay < 0 || bay >= card.unitCount())
        return "invalid SmartPort unit " + std::to_string(bay + 1);
    const auto index = static_cast<std::size_t>(bay);
    if (card.unit(index) && card.unit(index)->isLoaded())
        return "SmartPort unit " + std::to_string(bay + 1) + " already holds a disk";
    // The bay's own kind (often an empty 3.5" unit) is lent for the session:
    // put back on failure, and the one the settings keep (borrowBayType).
    const std::string original = card.persistedBayKind(bay);
    if (!(card.unit(index) && card.unit(index)->blockBacking()))
        card.setBayType(bay, std::string(SmartPortHdvUnit::kKindKey));
    Block512Backing* backing = card.unit(index) ? card.unit(index)->blockBacking() : nullptr;
    if (!backing || !backing->loadFromBytes(std::move(bytes), label, hostFolder)) {
        const std::string why = backing ? backing->lastError() : std::string();
        card.setBayType(bay, original);
        return why.empty() ? std::string("host folder mount failed") : why;
    }
    if (original != SmartPortHdvUnit::kKindKey) card.borrowBayType(bay, original);
    if (!hostFolder.empty()) backing->setHostFolderTextConversion(convertText);
    return {};
}

} // namespace pom2
