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

// SmartPortHub — //c+ drive-selection hub between the IWM, the MIG,
// and the four floppy slots (2× internal/external 5.25" + 2× internal/
// external 3.5"). Port of MAME `apple2e.cpp:638-679
// apple2e_state::recalc_active_device` plus the surrounding `phases_w
// / devsel_w / sel35_w` host callbacks.
//
// State inputs (mirrored from MAME `apple2e_state`):
//   m_devsel    — IWM control bit 5 derived (1 or 2 when active, 0 idle)
//   m_35sel     — MIG $C240/$C260 toggle (3.5" daisy-chain select)
//   m_intdrive  — MIG $C080/$C0C0 toggle (internal drive select)
//   m_hdsel     — MIG $C240/$C260 read-side head select
//
// Drive routing table (MAME line 644-666):
//
//   devsel  35sel  intdrive   →  selected slot
//   ─────────────────────────────────────────
//      1       0       *      →  floppy[0]  (5.25" internal)
//      1       1       *      →  floppy[3]  (3.5"  external #2)
//      2       *       1      →  floppy[2]  (3.5"  internal)   ← //c+ on-board
//      2       0       0      →  floppy[1]  (5.25" external)
//      2       1       0      →  nullptr    (3.5"  external #1, not modelled)
//
// The hub maintains the four slot pointers and recalculates the active
// drive on any of the state inputs changing. The active drive's
// `seekPhaseW` and `ssW` receive the IWM strobes. The IWM's `setFloppy`
// gets the new target so its bit-cell walker drains/restarts at the
// transition.
//
// Phase 1 scope (this file):
//   * Tracks state + active 3.5" routing for Sony35Drive sense reads.
//   * Forwards IWM phases callback to the active 3.5" drive.
//   * The IWM's `setFloppy(DiskImage*)` continues to point at the 5.25"
//     LSS for 5.25" drives — fully MAME-faithful Sony 3.5" data clocking
//     lands in Phase 2.

#ifndef POM2_SMARTPORT_HUB_H
#define POM2_SMARTPORT_HUB_H

#include <cstdint>

namespace pom2 {

class IWMDevice;
class Sony35Drive;

class SmartPortHub
{
public:
    SmartPortHub();

    /// Wire the IWM. The hub installs `phasesCb` and `devselCb` on
    /// the device so it can forward strobes to the active drive.
    void attach(IWMDevice* iwm);

    /// Mount the two 3.5" Sony drives. Slots 2 and 3 in MAME's
    /// `m_floppy[]`. Either may be nullptr if not configured.
    void setSony35(Sony35Drive* internal, Sony35Drive* external);

    Sony35Drive* internal35() const { return drive35Internal_; }
    Sony35Drive* external35() const { return drive35External_; }

    /// MIG state setters (called from Memory::migWrite / migRead).
    void setMig35Sel(bool sel35);
    void setMigIntDrive(bool intDrive);
    void setMigHdSel(bool hdSel);

    // NOTE deliberately no motor-broadcast helper here: MAME's IWM fires
    // `mon_w` on m_cur_floppy ONLY, and moves the motor with the
    // selection in `set_floppy` (iwm.cpp:99-115) — POM2 mirrors that in
    // IWMDevice::setSony35/notifyMonW. A previous `onIwmMotor` that
    // forwarded motor state to BOTH Sony drives made an idle external
    // drive report /MOTORON "running" and broke the //c+ firmware's
    // boot drive-scan.

    /// True when the currently-active drive is one of the 3.5" Sony
    /// drives (otherwise the active drive is a 5.25" Disk II under
    /// DiskIICard's control).
    bool active35Selected() const { return active35Selected_; }
    /// The MIG's external-3.5" select ($C240/$C260), as last pushed.
    bool mig35Sel() const { return sel35_; }

    /// Pointer to the active 3.5" drive (nullptr if no 3.5" drive is
    /// active OR none is mounted in the resolved slot).
    Sony35Drive* active35() const { return active35_; }

    /// Reset hub state (called on profile switch / cold boot).
    void reset();

private:
    IWMDevice*   iwm_              = nullptr;
    Sony35Drive* drive35Internal_  = nullptr;
    Sony35Drive* drive35External_  = nullptr;

    // Latched MIG / IWM state.
    uint8_t devsel_     = 0;       // 1 / 2 when active, 0 idle
    bool    sel35_      = false;
    bool    intDrive_   = false;
    bool    hdSel_      = false;

    bool         active35Selected_ = false;
    Sony35Drive* active35_         = nullptr;

    void recalcActiveDevice();
    void onIwmPhases(uint8_t phases);
    void onIwmDevsel(uint8_t devsel);
};

}  // namespace pom2

#endif // POM2_SMARTPORT_HUB_H
