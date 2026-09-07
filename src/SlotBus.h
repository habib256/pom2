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

// SlotBus — Apple II expansion bus dispatcher. Owns up to 8 SlotPeripheral
// instances (slots 0-7) and decodes the $C080-$CFFF address space:
//
//   $C080-$C0FF   16-byte device-select per slot (slot N = $C080+N*16).
//   $C100-$C7FF   256-byte slot ROM (slots 1-7). An access by a slot that
//                 HOLDS A CARD also claims the shared expansion window
//                 below, if nobody holds it yet.
//   $C800-$CFFE   2 KB expansion ROM, routed to the slot that claimed it.
//   $CFFF         Special "disable expansion ROM" switch — read or write
//                 releases the claim, so the next $CnXX can take it.
//
// **$C800 ownership is FIRST-one-wins, and only a populated slot claims.**
// MAME `apple2e.cpp:2970-2987` (`read_slot_rom`) / `:2989-3025`
// (`write_slot_rom`) claim only under `m_slotdevice[slotnum] != nullptr &&
// m_cnxx_slot == CNXX_UNCLAIMED && take_c800()`, and `c800_r` (`:3137-3155`)
// releases at offset $7FF. On real hardware each card latches its own
// $C800 enable off its I/O SELECT and only the $CFFF deselect clears them,
// so a second card touching its $CnXX cannot steal the window — it starts
// a bus fight, which MAME resolves as first-one-wins. POM2 used to latch
// last-one-wins AND to latch on empty slots.
//
// **Empty slots read the FLOATING BUS, not $FF.** Every one of the three
// upstream handlers above ends in `return read_floatingbus();`, as does
// `c080_r` (`:2883-2918`). Install the source with `setFloatingBusSource`
// (Memory does, at construction); without one the bus falls back to $FF.
//
// All entry points are called from the CPU thread under
// EmulationController's stateMutex. Plug/unplug/reset must run under the
// same lock so the dispatcher's internal state never races with a CPU
// fetch in flight.

#ifndef POM2_SLOT_BUS_H
#define POM2_SLOT_BUS_H

#include "SlotPeripheral.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>

class SlotBus
{
public:
    static constexpr int kSlotCount = 8;

    /// IRQ routing sink. Installed by Memory at `setCpu()` time; the
    /// closure forwards `(slot, asserted)` to `M6502::setIrqLine(slot,
    /// asserted)`. SlotBus stays decoupled from the CPU type — anyone
    /// (e.g. headless tests) can install their own sink. Set to a no-op
    /// or empty function to disconnect.
    using IrqRouter = std::function<void(int slot, bool asserted)>;

    /// Open-bus source for unclaimed reads. Memory installs a closure
    /// returning `Memory::floatingBus()`; tests may leave it unset, in
    /// which case `openBus()` answers $FF (the pre-2026-09-07 constant).
    /// Called only on the SLOW $Cxxx dispatch paths.
    using FloatingBusFn = std::function<uint8_t()>;
    void setFloatingBusSource(FloatingBusFn fn) { floatingBus_ = std::move(fn); }

    SlotBus() = default;
    ~SlotBus() = default;

    SlotBus(const SlotBus&) = delete;
    SlotBus& operator=(const SlotBus&) = delete;

    /// Insert a card into `slot` (0-7). Replaces and unplugs whatever was
    /// there. Calls `onPlug()` on the new card. Pass `nullptr` to clear
    /// (equivalent to `unplug`).
    void plug(int slot, std::unique_ptr<SlotPeripheral> card);

    /// Remove the card from `slot`, calling `onUnplug()` first. Returns
    /// the unique_ptr so the caller can hold on to it if needed (most
    /// callers just discard it). If the unplugged slot was driving the
    /// expansion ROM, the active expansion slot is cleared.
    std::unique_ptr<SlotPeripheral> unplug(int slot);

    /// Non-owning peek — useful for the UI to display "what's plugged".
    SlotPeripheral* peripheral(int slot) const {
        return (slot >= 0 && slot < kSlotCount) ? slots[slot].get() : nullptr;
    }
    bool isPlugged(int slot) const {
        return slot >= 0 && slot < kSlotCount && slots[slot] != nullptr;
    }

    /// CPU-side dispatch (called by Memory::memRead / memWrite).
    uint8_t deviceSelectRead (uint16_t addr);   // $C080-$C0FF
    void    deviceSelectWrite(uint16_t addr, uint8_t v);
    uint8_t slotRomRead      (uint16_t addr);   // $C100-$C7FF
    void    slotRomWrite     (uint16_t addr, uint8_t v); // for cards
                                                  // (e.g. Mockingboard) that
                                                  // decode MMIO inside the
                                                  // slot ROM window
    uint8_t expansionRomRead (uint16_t addr);   // $C800-$CFFF (CFFF disables)
    void    expansionRomWrite(uint16_t addr, uint8_t v);

    /// $CFFF read/write semantics: clears the active expansion slot.
    /// Exposed so callers (e.g. the Memory dispatcher) can short-circuit
    /// without going through the regular expansionRomRead path.
    void deactivateExpansion() { activeExpansionSlot = -1; }
    int  getActiveExpansionSlot() const { return activeExpansionSlot; }

    /// CPU pacing — forwarded from Memory::advanceCycles().
    void advanceCycles(int cycles);
    /// True when at least one card is plugged — lets Memory::advanceCycles()
    /// skip the out-of-line fan-out call on an empty bus (the
    /// pom2_bench banner shape, and a ][+ with nothing plugged).
    bool hasActiveCards() const { return activeCount_ > 0; }

    /// The card currently claiming DMA bus mastery (SoftCard Z80), or
    /// nullptr when the 6502 owns the bus — consulted by
    /// EmulationController before each CPU budget slice. Lowest slot
    /// wins if several claim (real hardware: the DMA daisy chain gives
    /// priority to the lowest slot; MAME a2bus does the same).
    SlotPeripheral* dmaClaimant() const {
        for (const auto& c : slots)
            if (c && c->dmaActive()) return c.get();
        return nullptr;
    }

    /// The CPU clock multiplier imposed by a plugged ACCELERATOR, or 1.0
    /// when nothing is accelerating. Lowest slot wins, same rule as the DMA
    /// daisy chain. Read once per frame by EmulationController.
    double cpuSpeedMultiplier() const {
        for (const auto& c : slots) {
            if (!c) continue;
            const double m = c->cpuSpeedMultiplier();
            if (m > 0.0 && m != 1.0) return m;
        }
        return 1.0;
    }

    /// The plugged accelerator that wants to SNOOP the bus, or nullptr.
    /// Cached at plug time rather than scanned, because the snoop sites
    /// (`Memory::softSwitchAccess` and the slot windows below) are on the
    /// path every $C0xx access takes — a null pointer test there is free,
    /// an eight-slot virtual scan would not be.
    SlotPeripheral* busSnooper() const { return busSnooper_; }

    /// System soft-switch broadcast — fan-out to every plugged card's
    /// onVideoSoftSwitch(). Used by Memory::softSwitchAccess() for the
    /// switches that aren't in the per-slot device-select range but that
    /// video cards (Le Chat Mauve, Video-7) still need to observe:
    /// $C00C/$C00D (80COL) and $C05E/$C05F (AN3).
    void broadcastVideoSwitch(uint16_t addr);
    /// Same fan-out for a WRITE, carrying the data byte (the Eve's CPREG).
    void broadcastVideoSwitchWrite(uint16_t addr, uint8_t value);

    /// Install (or replace) the IRQ router. Pass an empty function to
    /// disconnect — any subsequent `assertIrq()` from a card becomes a
    /// no-op until a new sink is installed.
    void setIrqRouter(IrqRouter sink) { irqRouter = std::move(sink); }

    /// Called by `SlotPeripheral::assertIrq` to forward an edge to the
    /// CPU. Public so the friend access on SlotPeripheral resolves
    /// cleanly; cards shouldn't call this directly — use `assertIrq()`
    /// in their base class instead.
    void forwardSlotIrq(int slot, bool asserted) {
        if (irqRouter) irqRouter(slot, asserted);
    }

    /// Apple II RESET line — calls onReset() on every plugged card.
    /// Does NOT touch activeExpansionSlot (matches hardware: RESET
    /// doesn't lift the expansion-ROM enable line).
    void reset();

    /// Unplug every card. Each is given an `onUnplug()` callback first.
    /// `activeExpansionSlot` is also cleared. Used by MainWindow's
    /// "restart emulation" path when the Slot Configuration panel
    /// applies a new mapping — no reason to keep a card whose slot or
    /// presence is about to change.
    void clear();

private:
    /// So a plugged card can reach `openBus()` through
    /// `SlotPeripheral::openBus()` — the MAME `get_open_bus()` tail several
    /// a2bus cards end their `read_c0nx` switch on. Nothing else in the
    /// private section is exposed by this.
    friend class SlotPeripheral;

    std::array<std::unique_ptr<SlotPeripheral>, kSlotCount> slots{};

    /// Compact list of the cards actually plugged, so the per-instruction
    /// `advanceCycles` fan-out does not walk eight `unique_ptr` slots to find
    /// the one or two that exist.
    ///
    /// A Callgrind profile (2026-07-30) measured `SlotBus::advanceCycles` at
    /// 65 host instructions per EMULATED instruction with a single Disk II
    /// plugged — 15.7 % of the whole emulation core, nearly all of it the
    /// eight-way scan and its `unique_ptr` dereferences (which showed up
    /// separately as another 1.9 % under `unique_ptr.h`).
    ///
    /// These are RAW, NON-OWNING pointers into `slots`, so they dangle the
    /// instant a card is destroyed. Every mutation of `slots` must therefore
    /// call `rebuildActiveCache()` — there are exactly three (`plug`,
    /// `unplug`, `clear`), `slots` is private, and no accessor hands out a way
    /// to reseat a slot from outside. `advanceCycles` asserts the cache matches
    /// in debug builds so a future fourth mutation point fails loudly rather
    /// than silently skipping a card or following a freed pointer.
    ///
    /// Safe without extra locking: mutations happen under `stateMutex` (the
    /// worker is additionally stopped across `applyProfile`), which is the same
    /// lock the CPU worker holds around `runCpuSlice` → `advanceCycles`.
    std::array<SlotPeripheral*, kSlotCount> activeCards_{};
    int activeCount_ = 0;
    // Set by rebuildActiveCache() — the lowest-slot card whose snoopsBus()
    // is true. Null on every ordinary machine, which is what keeps the
    // snoop sites free.
    SlotPeripheral* busSnooper_ = nullptr;
    void rebuildActiveCache();

    /// -1 = CNXX_UNCLAIMED. Claimed by the FIRST slot whose $CnXX window is
    /// touched **and whose card returns true from `takesC800()`** (MAME
    /// `a2bus.h:145` `take_c800()`, default false); released by $CFFF and by
    /// unplug() of the holder. MAME `apple2e.cpp:216` (`CNXX_UNCLAIMED`) +
    /// `:2977-2981`. The callers do the gating so `claimExpansion` stays the
    /// one place the first-one-wins rule lives.
    int activeExpansionSlot = -1;
    void claimExpansion(int slot) {
        if (activeExpansionSlot < 0) activeExpansionSlot = slot;
    }
    /// IRQ routing sink (see `IrqRouter`). Optional — left empty in
    /// tests that don't care, or before Memory wires the CPU.
    IrqRouter irqRouter;
    /// Open-bus source (see `FloatingBusFn`). Optional.
    FloatingBusFn floatingBus_;

    /// MAME's `read_floatingbus()` tail — what an unclaimed $Cxxx read
    /// puts on the data bus.
    uint8_t openBus() const { return floatingBus_ ? floatingBus_() : uint8_t{0xFF}; }
};

#endif // POM2_SLOT_BUS_H
