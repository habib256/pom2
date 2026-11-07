// POM2 — GPL-3.0-or-later
#include "SessionSettings.h"
#include "NoSlotClock.h"
#include "Settings.h"
#include "SlotBus.h"
#include "SuperSerialCard.h"
#include <string>

namespace pom2 {
void persistSerialAndClockSettings(Settings& settings, SlotBus& bus,
                                   const NoSlotClock& clock, bool sessionMachineActive)
{
    if (sessionMachineActive) return;
    // Per-slot keys, so the //c's two ports (printer sl1 + modem sl2) each
    // keep their own state; the legacy global keys mirror the primary (the
    // lowest-slot) card for older settings files and the AI control path.
    SuperSerialCard* primary = nullptr;
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        auto* ssc = dynamic_cast<SuperSerialCard*>(bus.peripheral(slot));
        if (!ssc) continue;
        if (!primary) primary = ssc;
        const std::string sk = "_slot" + std::to_string(ssc->getSlot());
        settings.setBool("ssc_listening" + sk, ssc->isListening());
        settings.setInt ("ssc_port"      + sk, ssc->getPort());
        settings.setBool("ssc_raw_mode"  + sk, ssc->rawMode());
        settings.setBool("vsdrive_enabled" + sk, ssc->vsDriveActive());
        if (ssc->vsDriveActive()) {
            for (int drive = 0; drive < 2; ++drive) {
                settings.setString("vsdrive_path" + std::to_string(drive + 1) + sk,
                                   ssc->vsDrivePath(drive));
                settings.setBool("vsdrive_read_only" + std::to_string(drive + 1) + sk,
                                 ssc->vsDriveReadOnly(drive));
            }
        }
        settings.setBool("ssc_printer_tap" + sk, ssc->printerTap());
        settings.setString("ssc_cable" + sk, SuperSerialCard::cableKey(ssc->cable()));
        if (!ssc->builtInPort()) {
            settings.setInt("ssc_dsw1" + sk, ssc->dipSwitch1());
            settings.setInt("ssc_dsw2" + sk, ssc->dipSwitch2());
        }
    }
    if (primary) {
        settings.setBool("ssc_listening", primary->isListening());
        settings.setInt ("ssc_port",      primary->getPort());
        settings.setBool("ssc_raw_mode",  primary->rawMode());
    }
    settings.setBool("nsclock_enable", clock.isEnabled());
    settings.setInt ("nsclock_slot",   clock.slot());
}
}  // namespace pom2
