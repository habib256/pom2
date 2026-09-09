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

// Two paging latches Memory's snapshot blob and its CPU view used to get
// wrong. Bug hunt #13 (2026-09-09).
//
//  1. The $C800-$CFFE expansion-window OWNER (`SlotBus::activeExpansionSlot`).
//     `intC8Rom` — the flag that says the MOTHERBOARD holds the window — has
//     been in the MEX blob since 2026-09-06, but the slot-side half of the
//     same decision was in no section at all. Seven cards take the window
//     (ClockCard, GrapplerCard, LironCard, WorkstationCard, CffaCard,
//     SmartPortCard, SuperSerialCard), so a rewind or a snapshot load whose
//     PC sat inside a card's expansion ROM — a SmartPort/Liron driver, the
//     SSC firmware — came back reading the FLOATING BUS across the whole
//     2 KB window. Now carried as the tenth byte of the paging/IOU section
//     (0 = unclaimed, 1-7 = slot), grow-at-the-end so older blobs still load.
//
//  2. `peekCpuWriteTarget` over $D000-$FFFF. The language card has two
//     independent latches, and their POWER-ON combination is precisely the
//     one where they disagree: every reset leaves write-enable SET with ROM
//     still mapped for reads (Sather, Understanding the Apple //e, fig. 5.13;
//     MAME `apple2e.cpp:1227-1232` + `:1492-1497` set `m_lcwriteenable`).
//     Reads then see Applesoft/Monitor ROM while writes land in LC RAM.
//     `peekCpuWriteTarget` answered with the read view, so the Memory
//     Viewer's UNDO record (DebugCoordinator.cpp) held a ROM byte and undoing
//     an edit pushed that ROM opcode into Language-Card RAM.
//
// Core test: no ImGui, no CPU — drives Memory directly.

#include "Memory.h"
#include "SlotPeripheral.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>
#include <vector>

namespace {

int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::printf("FAIL: %s\n", msg); ++failures; } } while (0)

// A card with a $Cn00 page AND a $C800 expansion ROM, so the window's owner
// is observable: $C900 reads $60 through this card, $00 through the (zero
// filled) motherboard internal ROM, and the floating bus when unclaimed.
class ExpansionCard : public SlotPeripheral {
public:
    std::string_view name() const override { return "ExpansionCard"; }
    uint8_t slotRomRead(uint8_t low8) override {
        return static_cast<uint8_t>(0x50 ^ low8);
    }
    uint8_t expansionRomRead(uint16_t off) override {
        return static_cast<uint8_t>(0x60 + (off & 0xFF));
    }
    bool takesC800() const override { return true; }
};

// A card with no expansion ROM: must never claim the window (MAME
// `a2bus.h:145` take_c800() default false).
class NoWindowCard : public SlotPeripheral {
public:
    std::string_view name() const override { return "NoWindowCard"; }
    uint8_t slotRomRead(uint8_t) override { return 0x11; }
};

void testExpansionOwnerSurvivesSnapshot()
{
    Memory m;
    m.setIIEMode(true);
    m.resetSoftSwitches();
    m.slotBus().plug(5, std::make_unique<ExpansionCard>());
    // SETSLOTC3ROM: keep the //e 80-column firmware from latching INTC8ROM
    // and taking the window for the motherboard — this test is about the
    // SLOT-side latch.
    m.memWrite(0xC00B, 0);

    (void)m.memRead(0xC500);                  // slot 5 claims the window
    const uint8_t owned = m.memRead(0xC900);
    CHECK(owned == 0x60, "slot 5 must serve $C900 through its expansion ROM");
    CHECK(m.slotBus().getActiveExpansionSlot() == 5,
          "slot 5 must be the expansion-ROM owner");

    std::vector<uint8_t> blob;
    m.appendSnapshotState(blob);

    (void)m.memRead(0xCFFF);                  // release the window
    CHECK(m.slotBus().getActiveExpansionSlot() == -1,
          "$CFFF must release the expansion-ROM owner");
    CHECK(m.memRead(0xC900) != 0x60,
          "with the window released $C900 must not read the card");

    uint32_t savedBanks = 0;
    CHECK(m.loadSnapshotState(blob.data(), blob.size(), &savedBanks),
          "the MEX blob must reload");
    CHECK(m.slotBus().getActiveExpansionSlot() == 5,
          "restore must put the $C800 window back on slot 5");
    CHECK(m.memRead(0xC900) == 0x60,
          "after the restore $C900 must read the card's expansion ROM again");
}

void testUnclaimedOwnerSurvivesSnapshot()
{
    // The mirror case: a blob captured with the window UNCLAIMED must not
    // leave a live owner in place, or a restored PC at $C800 would run a
    // card's ROM the captured machine was not running.
    Memory m;
    m.setIIEMode(true);
    m.resetSoftSwitches();
    m.slotBus().plug(5, std::make_unique<ExpansionCard>());
    m.memWrite(0xC00B, 0);

    std::vector<uint8_t> blob;
    m.appendSnapshotState(blob);              // captured unclaimed

    (void)m.memRead(0xC500);                  // slot 5 claims it afterwards
    CHECK(m.slotBus().getActiveExpansionSlot() == 5, "owner claimed post-capture");

    uint32_t savedBanks = 0;
    CHECK(m.loadSnapshotState(blob.data(), blob.size(), &savedBanks), "reload");
    CHECK(m.slotBus().getActiveExpansionSlot() == -1,
          "restore must return the window to unclaimed");
}

void testOwnerRestoreIsValidated()
{
    // A blob naming a slot whose card cannot serve the window must restore
    // as unclaimed rather than pointing expansionRomRead at it.
    Memory m;
    m.setIIEMode(true);
    m.resetSoftSwitches();
    m.slotBus().plug(5, std::make_unique<ExpansionCard>());
    m.memWrite(0xC00B, 0);
    (void)m.memRead(0xC500);

    std::vector<uint8_t> blob;
    m.appendSnapshotState(blob);

    // Same blob, different machine: slot 5 now holds a card with no $C800.
    Memory other;
    other.setIIEMode(true);
    other.resetSoftSwitches();
    other.slotBus().plug(5, std::make_unique<NoWindowCard>());
    other.memWrite(0xC00B, 0);
    uint32_t savedBanks = 0;
    CHECK(other.loadSnapshotState(blob.data(), blob.size(), &savedBanks), "reload");
    CHECK(other.slotBus().getActiveExpansionSlot() == -1,
          "a card that does not take $C800 must not become the owner");
}

void testWriteTargetIsLanguageCardRam()
{
    Memory m;
    m.setIIEMode(true);
    m.resetSoftSwitches();

    // Prime LC bank 2 (the reset default bank) with markers, through the bus.
    (void)m.memRead(0xC083);                  // odd read #1: arm prewrite
    (void)m.memRead(0xC083);                  // odd read #2: commit write-enable
    m.memWrite(0xD000, 0x11);
    m.memWrite(0xE000, 0x22);
    CHECK(m.memRead(0xD000) == 0x11 && m.memRead(0xE000) == 0x22,
          "LC RAM must accept the markers");

    // Back to the power-on latch pair: ROM mapped for reads, RAM write-enabled.
    m.resetSoftSwitches();
    CHECK(m.peekCpuWriteTarget(0xD000) == 0x11,
          "$D000 write target must be LC RAM, not the ROM the read view shows");
    CHECK(m.peekCpuWriteTarget(0xE000) == 0x22,
          "$E000 write target must be LC RAM");

    // The consequence this exists for: the Memory Viewer's undo record.
    const uint8_t replaced = m.peekCpuWriteTarget(0xD000);
    m.memWrite(0xD000, 0x99);                 // the user's edit
    m.memWrite(0xD000, replaced);             // UNDO
    (void)m.memRead(0xC083);
    (void)m.memRead(0xC083);
    CHECK(m.memRead(0xD000) == 0x11,
          "undo must restore the LC RAM byte, not drop a ROM byte into it");
}

void testWriteTargetFollowsAltzpAndBank()
{
    Memory m;
    m.setIIEMode(true);
    m.resetSoftSwitches();

    // Aux LC (ALTZP) bank 1 at $D000.
    m.memWrite(0xC009, 0);                    // ALTZP on
    (void)m.memRead(0xC08B);                  // bank 1, RAM read, arm
    (void)m.memRead(0xC08B);                  // commit write-enable
    m.memWrite(0xD000, 0x77);
    // Main LC bank 2 at the same address, so a wrong bank/bank-side is visible.
    m.memWrite(0xC008, 0);                    // ALTZP off
    (void)m.memRead(0xC083);
    (void)m.memRead(0xC083);
    m.memWrite(0xD000, 0x33);

    m.resetSoftSwitches();                    // read ROM / write RAM / bank 2
    CHECK(m.peekCpuWriteTarget(0xD000) == 0x33,
          "main LC bank 2 must be the write target after a reset");
    m.memWrite(0xC009, 0);                    // ALTZP on -> aux LC trio
    // Bank 1 is where the aux marker went; the reset selected bank 2, so
    // select bank 1 again (two odd reads also re-commit write-enable).
    (void)m.memRead(0xC08B);
    (void)m.memRead(0xC08B);
    CHECK(m.peekCpuWriteTarget(0xD000) == 0x77,
          "aux LC bank 1 must be the write target under ALTZP");
}

void testWriteProtectedLanguageCardKeepsReadView()
{
    // With writes DISABLED the store is dropped, so the read view is the
    // right "previous value" — and an undo built from it is dropped too.
    Memory m;
    m.setIIEMode(true);
    m.resetSoftSwitches();
    (void)m.memRead(0xC082);                  // even: ROM read, write-protect
    const uint8_t view = m.peekCpuView(0xD000);
    CHECK(m.peekCpuWriteTarget(0xD000) == view,
          "a write-protected LC must report the read view as the write target");
}

} // namespace

int main()
{
    testExpansionOwnerSurvivesSnapshot();
    testUnclaimedOwnerSurvivesSnapshot();
    testOwnerRestoreIsValidated();
    testWriteTargetIsLanguageCardRam();
    testWriteTargetFollowsAltzpAndBank();
    testWriteProtectedLanguageCardKeepsReadView();

    if (failures) {
        std::printf("mem_paging_latch_snapshot: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("mem_paging_latch_snapshot: all checks passed\n");
    return 0;
}
