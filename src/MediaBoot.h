// POM2 — GPL-3.0-or-later
#ifndef POM2_MEDIA_BOOT_H
#define POM2_MEDIA_BOOT_H
#include <string>
class EmulationController;
namespace pom2 {
// Boot the medium at an addressed mechanism, or refuse before resetting
// when the firmware cannot select it. Slot -1 addresses the //c+ Sonys.
// Unlike bootFromSlot(), success must not knowingly launch another medium.
bool bootMountedMedia(EmulationController& controller, int slot, int bay,
                      std::string& error);
}
#endif
