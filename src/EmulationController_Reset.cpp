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

// EmulationController_Reset — the three reset verbs (soft, hard, cold),
// moved out of EmulationController.cpp on 2026-11-27. Each has a Locked
// body, so a playback of recorded host input can apply a reset at the cycle
// it was made at from inside the CPU slice, which already holds the lock.

#include "EmulationController.h"

#include "Logger.h"

void EmulationController::hardReset()
{
    std::lock_guard<std::mutex> lk(stateMtx);
    // A recording keeps it, at this cycle; a playback drives the machine
    // and a reset by hand ends it (EmulationController_Input.cpp).
    noteResetForInputJournalLocked(pom2::InputEvent::Kind::HardReset);
    hardResetLocked();
}

void EmulationController::hardResetLocked()
{
    mem.setIicSmartPortArmed(false);   // reboot → //c sees its real $C500 firmware
    // Same soft-switch policy as softReset (MAME reset_w / machine_reset):
    // II/II+ preserve LC + display switches; IIe-class wipes MMU/IOU/LC.
    // Pre-fix hardReset called resetSoftSwitches() unconditionally, which
    // forced TEXT+no NTSC on every F12 even on II/II+.
    mem.resetSoftSwitchesWarm();
    mem.slotBus().reset();
    if (spk)    spk->reset();
    if (iwmDev) iwmDev->reset();
    // The Disk II keeps a shadow of the IWM's mode register; it goes with
    // the chip's (cold boot clears it through onPowerCycle). Unconditional:
    // a card's shadow does not depend on this controller owning an IWM.
    mem.slotBus().iwmReset();
    if (hub)    hub->reset();
    if (tape)   tape->resetCpuSide();
    // A step-over / run-to-cursor transient is armed at an address in the
    // code that WAS running. After a reset (F12, and applyProfile step 11,
    // which is a profile switch) that address means nothing, but the
    // transient stays armed and fires the first time the PC happens past it
    // — minutes later, with no visible cause. Disarm it and re-sync, because
    // `armed()` counts the transient: dropping it may be what detaches the
    // hook and puts the CPU back on its fast loop.
    //
    // clearForTimeJump, not clearTransient: a reset is a jump in the timeline
    // for run-control state, and the LATCHED HIT is the piece that bit. It
    // survived every reset, so the next Run consumed it in runCpuSlice via
    // debugResume() — which grants a one-instruction amnesty at the POST-RESET
    // pc. A breakpoint on the reset/boot entry point (break there, then hit
    // Reset — the commonest move there is) was therefore skipped, and missed
    // outright when that instruction is only reached once. Same set of state
    // a rewind drops, and for the same reason (Debugger.h; bug hunt #9).
    if (debugger_) { debugger_->clearForTimeJump(); syncDebugHook(); markReplayBarrier(); }
    processor.hardReset();
    pom2::log().info("Emul", "Hard reset");
}

void EmulationController::softReset()
{
    std::lock_guard<std::mutex> lk(stateMtx);
    // A recording keeps it, at this cycle; a playback drives the machine
    // and a reset by hand ends it (EmulationController_Input.cpp).
    noteResetForInputJournalLocked(pom2::InputEvent::Kind::SoftReset);
    softResetLocked();
}

void EmulationController::softResetLocked()
{
    // Real Apple II Ctrl-Reset: the reset line is asserted briefly. The
    // 6502 latches PC from $FFFC, sets the I flag, and decrements SP by 3
    // (the reset sequence simulates a BRK push of PC + P without storing).
    // RAM, A/X/Y, and the zero page survive. Slot cards see their reset
    // line too — Disk II spins down, Le Chat Mauve FIFO returns to its
    // power-on default, etc. Soft-switch policy depends on the profile:
    // II/II+ machine_reset preserves LC + display (MAME `apple2.cpp:325-
    // 331`); IIe/IIc/IIc+ reset_w wipes the MMU/IOU/LC list (MAME
    // `apple2e.cpp:1453-1508`). resetSoftSwitchesWarm() applies the
    // right one based on iieMode.
    mem.setIicSmartPortArmed(false);   // reboot → //c sees its real $C500 firmware
    mem.resetSoftSwitchesWarm();
    mem.slotBus().reset();
    // SmartPort hub: the MIG selection goes with the profile's copy of it
    // (E-3-1), the Sony mechanisms and the IWM stay — MAME's `reset_w`
    // touches no floppy state (its `m_35sel = false` is machine_reset's,
    // power-on). See SmartPortHub::warmReset.
    if (hub) hub->warmReset();
    // Parity with hardReset() / coldBoot() / bootFromSlot(): a step-over or
    // run-to-cursor transient names an address in the code that was running,
    // and Ctrl-Reset restarts the machine from $FFFC. Leaving it armed made a
    // one-shot breakpoint fire minutes later with no visible cause — and
    // `armed()` counts the transient, so dropping it is also what lets
    // syncDebugHook put the CPU back on its undebugged loop. clearForTimeJump
    // drops the latched hit too — see hardReset().
    if (debugger_) { debugger_->clearForTimeJump(); syncDebugHook(); markReplayBarrier(); }
    processor.softReset();
    pom2::log().info("Emul", "Soft reset (Ctrl-Reset)");
}

void EmulationController::coldBoot()
{
    std::lock_guard<std::mutex> lk(stateMtx);
    // A recording keeps it, at this cycle; a playback drives the machine
    // and a reset by hand ends it (EmulationController_Input.cpp).
    noteResetForInputJournalLocked(pom2::InputEvent::Kind::ColdBoot);
    coldBootLocked();
}

void EmulationController::coldBootLocked()
{
    mem.setIicSmartPortArmed(false);   // reboot → //c sees its real $C500 firmware
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.slotBus().reset(/*powerCycle=*/true);
    if (spk)    spk->reset();   // F-1-3: parity with hardReset
    if (iwmDev) iwmDev->reset();
    if (hub)    hub->reset();
    if (tape)   tape->resetCpuSide();
    // As in hardReset(): the transient names an address in a program that no
    // longer exists — here its RAM has literally been wiped. The latched hit
    // goes with it (clearForTimeJump) — see hardReset().
    if (debugger_) { debugger_->clearForTimeJump(); syncDebugHook(); markReplayBarrier(); }
    processor.hardReset();
    rewind_.clear();   // RAM wiped → the recorded timeline is a different machine
    scrubIndex_.store(pom2::RewindBuffer::kNoFrame);
    // Virtual ][ power switch. No-op on the MAME bank. The first cold boot
    // of a process is PopOn; a later one (the toolbar) is PopOff then PopOn.
    if (floppy525) floppy525->powerSwitch();
    pom2::log().info("Emul", "Cold boot (RAM wiped)");
}
