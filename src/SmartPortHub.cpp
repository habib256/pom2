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

// Port of MAME `apple2e.cpp:638-679 apple2e_state::recalc_active_device`
// + the surrounding `phases_w / devsel_w / sel35_w` callback wiring
// (lines 625-637).

#include "SmartPortHub.h"
#include "IWMDevice.h"
#include "Sony35Drive.h"
#include "Logger.h"

namespace pom2 {

SmartPortHub::SmartPortHub() = default;

void SmartPortHub::attach(IWMDevice* iwm)
{
    iwm_ = iwm;
    if (!iwm_) return;
    iwm_->setPhasesCallback([this](uint8_t p) { onIwmPhases(p); });
    iwm_->setDevselCallback([this](uint8_t d) { onIwmDevsel(d); });
    // sel35Cb_ is unused by MAME (`apple2e.cpp:633-635` defines
    // sel35_w as an empty stub — the MIG's $C240/$C260 toggles do
    // the work). We keep the callback wired for symmetry but ignore.
    iwm_->setSel35Callback([](bool) {});
}

void SmartPortHub::setSony35(Sony35Drive* internal, Sony35Drive* external)
{
    drive35Internal_ = internal;
    drive35External_ = external;
    recalcActiveDevice();
}

void SmartPortHub::setMig35Sel(bool sel35)
{
    if (sel35_ == sel35) return;
    sel35_ = sel35;
    recalcActiveDevice();
}

void SmartPortHub::setMigIntDrive(bool intDrive)
{
    if (intDrive_ == intDrive) return;
    intDrive_ = intDrive;
    recalcActiveDevice();
}

void SmartPortHub::setMigHdSel(bool hdSel)
{
    hdSel_ = hdSel;
    // Head select is forwarded to the *currently* active 3.5" drive
    // (MAME line 676 `m_cur_floppy->ss_w(m_hdsel ? 1 : 0)` runs at the
    // end of recalc_active_device, so head changes always re-evaluate
    // on the current floppy).
    if (active35_) active35_->ssW(hdSel_);
}

void SmartPortHub::reset()
{
    devsel_   = 0;
    sel35_    = false;
    intDrive_ = false;
    hdSel_    = false;
    active35Selected_ = false;
    active35_         = nullptr;
    if (drive35Internal_) drive35Internal_->reset();
    if (drive35External_) drive35External_->reset();
}

void SmartPortHub::recalcActiveDevice()
{
    // MAME line 644-666 routing table.
    bool         is35  = false;
    Sony35Drive* drive = nullptr;

    if (devsel_ == 1) {
        if (!sel35_) {
            // 5.25" internal (MAME `m_floppy[0]`) — DiskIICard handles.
        } else {
            // 3.5" external (MAME `m_floppy[3]`, line 650).
            is35  = true;
            drive = drive35External_;
        }
    } else if (devsel_ == 2) {
        if (intDrive_) {
            // On-board 3.5" (MAME `m_floppy[2]`, line 659). This is
            // the //c+'s integrated drive — by far the common case.
            is35  = true;
            drive = drive35Internal_;
        } else if (!sel35_) {
            // 5.25" external (MAME `m_floppy[1]`, line 663) —
            // DiskIICard handles.
        } else {
            // MAME line 665: "should be external 3.5 #2, for a 3rd
            // drive" — set to nullptr. POM2 mirrors.
        }
    }

    // MAME `recalc_active_device` ends with `m_iwm->set_floppy(...)`.
    // POM2 splits the IWM target between 5.25" (DiskIICard's DiskImage)
    // and 3.5" (Sony35Drive); when a 3.5" drive is selected we point
    // the IWM at it via `setSony35`. When the active drive is 5.25"
    // the IWM stays on whatever DiskIICard's seekPhaseW path last
    // configured (i.e. we do NOT call setFloppy(nullptr) here — that
    // would knock the 5.25" path out from under DiskIICard).
    active35Selected_ = is35;
    active35_         = is35 ? drive : nullptr;

    if (iwm_) {
        if (active35_) iwm_->setSony35(active35_);
        // MAME's set_floppy(nullptr) equivalent. NOT `setSony35(nullptr)`:
        // that would also clear `disk_`, which DiskIICard owns. Leaving the
        // stale Sony attached let a 5.25"-era write burst land on the last
        // 3.5" disk (DiskIICard::pushIwmFloppy only fires on a real drive
        // change or head move, so it did not close the window).
        else           iwm_->releaseSony35();
    }

    if (active35_) {
        active35_->setSel(iwm_ && iwm_->sel());
        active35_->ssW(hdSel_);
    }
}

void SmartPortHub::onIwmPhases(uint8_t phases)
{
    // MAME `apple2e.cpp:625-631 phases_w`: forward to the current
    // floppy. POM2's 5.25" path is owned by DiskIICard (which has its
    // own seekPhaseW hook from the slot-6 mux); 3.5" goes through
    // Sony35Drive.
    if (active35_) {
        active35_->setSel(iwm_ && iwm_->sel());
        // Stamp the strobe with the IWM's last-tick cycle so the Sony
        // drive can timestamp mechanical-sound events (step / motor)
        // in emulated time — same pattern as DiskIICard::seekPhaseW.
        active35_->seekPhaseW(phases, iwm_ ? iwm_->emuCycles() : 0);
    }
}

void SmartPortHub::onIwmDevsel(uint8_t devsel)
{
    if (devsel_ == devsel) return;
    devsel_ = devsel;
    recalcActiveDevice();
}

}  // namespace pom2
