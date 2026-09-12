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

#include <cassert>
#include "SlotBus.h"

// ─── SlotPeripheral ↔ SlotBus wiring ────────────────────────────────────
// Definitions for SlotPeripheral live here so they have full visibility
// of SlotBus (forward-declared in SlotPeripheral.h to avoid a circular
// include). `attachToBus` and `detachFromBus` are private and called
// only by SlotBus::plug / unplug / clear.

void SlotPeripheral::attachToBus(SlotBus* bus, int slot)
{
    bus_ = bus;
    busSlot_ = slot;
    // Cards start with no IRQ contribution; if a constructor pre-set
    // `irqAsserted_` we don't honour it here — the bus is responsible
    // for re-asserting after attach if the card immediately requests
    // it via assertIrq().
    irqAsserted_ = false;
}

void SlotPeripheral::detachFromBus()
{
    // Auto-release any pending IRQ this card was contributing, so the
    // wire-OR aggregator doesn't keep the bit set after the card is
    // gone. Without this each card would have to remember to clear in
    // its own onUnplug() — easy to forget, and the cause of the legacy
    // "stuck IRQ across profile switch" bug.
    if (irqAsserted_ && bus_) bus_->forwardSlotIrq(busSlot_, false);
    irqAsserted_ = false;
    bus_ = nullptr;
    busSlot_ = -1;
}

uint8_t SlotPeripheral::openBus() const
{
    // Defined here, not in the header: SlotPeripheral.h only forward-declares
    // SlotBus (circular include), so the call needs the definition this file
    // already has.
    return bus_ ? bus_->openBus() : uint8_t{0xFF};
}

void SlotPeripheral::assertIrq(bool asserted)
{
    if (asserted == irqAsserted_) return;
    irqAsserted_ = asserted;
    if (bus_) bus_->forwardSlotIrq(busSlot_, asserted);
}

// ─── SlotBus ────────────────────────────────────────────────────────────

void SlotBus::plug(int slot, std::unique_ptr<SlotPeripheral> card)
{
    if (slot < 0 || slot >= kSlotCount) return;

    if (slots[slot]) {
        slots[slot]->onUnplug();
        slots[slot]->detachFromBus();
        if (activeExpansionSlot == slot) activeExpansionSlot = -1;
        slots[slot].reset();
    }
    if (card) {
        slots[slot] = std::move(card);
        slots[slot]->attachToBus(this, slot);
        slots[slot]->onPlug();
    }
    rebuildActiveCache();
}

std::unique_ptr<SlotPeripheral> SlotBus::unplug(int slot)
{
    if (slot < 0 || slot >= kSlotCount) return nullptr;
    if (!slots[slot]) return nullptr;
    slots[slot]->onUnplug();
    slots[slot]->detachFromBus();
    if (activeExpansionSlot == slot) activeExpansionSlot = -1;
    auto owned = std::move(slots[slot]);   // slots[slot] is now empty
    rebuildActiveCache();                  // ...so rebuild BEFORE returning
    return owned;
}

uint8_t SlotBus::deviceSelectRead(uint16_t addr)
{
    // $C080-$C0FF — 16 bytes per slot. Slot N starts at $C080 + N*16.
    if (addr < 0xC080 || addr > 0xC0FF) return 0xFF;
    // A snooper that CONSUMES the access takes it off the bus: the card in
    // the slot must not see it either (`SlotPeripheral::busSnoop`'s own
    // contract, which `Memory::softSwitchAccess` honours and these four
    // sites dropped until 2026-09-12).
    if (busSnooper_ && busSnooper_->busSnoop(addr, false, 0)) return openBus();
    const int slot = (addr - 0xC080) >> 4;
    const uint8_t low4 = static_cast<uint8_t>(addr & 0x0F);
    if (auto* p = slots[slot].get()) return p->deviceSelectRead(low4);
    // Empty slot: MAME `apple2e.cpp:2883-2918` `c080_r` falls through every
    // per-slot branch to `return read_floatingbus();`.
    return openBus();
}

void SlotBus::deviceSelectWrite(uint16_t addr, uint8_t v)
{
    if (busSnooper_ && busSnooper_->busSnoop(addr, true, v)) return;
    if (addr < 0xC080 || addr > 0xC0FF) return;
    const int slot = (addr - 0xC080) >> 4;
    const uint8_t low4 = static_cast<uint8_t>(addr & 0x0F);
    if (auto* p = slots[slot].get()) p->deviceSelectWrite(low4, v);
}

uint8_t SlotBus::slotRomRead(uint16_t addr)
{
    // $C100-$C7FF — slot N at $C(N)00-$C(N)FF, N=1..7.
    if (addr < 0xC100 || addr > 0xC7FF) return 0xFF;
    if (busSnooper_ && busSnooper_->busSnoop(addr, false, 0)) return openBus();
    const int slot = (addr >> 8) & 0x07;     // $CN00 → N
    if (slot < 1 || slot > 7) return openBus();

    // MAME `apple2e.cpp:2970-2987` `read_slot_rom`, verbatim: the claim is
    // `(m_cnxx_slot == CNXX_UNCLAIMED) && m_slotdevice[slotnum]->take_c800()`
    // — first-one-wins, but ONLY among cards that actually drive /IOSTB
    // (`a2bus.h:145`, default false). A card with no expansion ROM used to
    // latch the window here and hand $FF to the card that has one.
    if (auto* p = slots[slot].get()) {
        if (p->takesC800()) claimExpansion(slot);
        return p->slotRomRead(static_cast<uint8_t>(addr & 0xFF));
    }
    return openBus();
}

void SlotBus::slotRomWrite(uint16_t addr, uint8_t v)
{
    if (busSnooper_ && busSnooper_->busSnoop(addr, true, v)) return;
    // Mirror of slotRomRead's address decode. Forwards to the card's
    // `slotRomWrite` (default no-op for ROM-only cards) and latches the
    // slot as the active expansion-ROM owner — same Apple II semantics
    // as a read into the slot ROM window.
    if (addr < 0xC100 || addr > 0xC7FF) return;
    const int slot = (addr >> 8) & 0x07;
    if (slot < 1 || slot > 7) return;

    // MAME `apple2e.cpp:2989-3025` `write_slot_rom`: same first-one-wins
    // claim as the read, and likewise gated on `take_c800()` (`a2bus.h:145`)
    // — a populated slot is not enough, the card must serve /IOSTB.
    if (auto* p = slots[slot].get()) {
        if (p->takesC800()) claimExpansion(slot);
        p->slotRomWrite(static_cast<uint8_t>(addr & 0xFF), v);
    }
}

uint8_t SlotBus::expansionRomRead(uint16_t addr)
{
    if (addr < 0xC800 || addr > 0xCFFF) return openBus();
    if (addr == 0xCFFF) {
        // Read or write to $CFFF deactivates the expansion ROM (MAME
        // `apple2e.cpp:3137-3145`, `offset == 0x7ff`). The byte returned is
        // the floating bus — the same `read_floatingbus()` tail every other
        // unclaimed read takes.
        deactivateExpansion();
        return openBus();
    }
    const int slot = activeExpansionSlot;
    if (slot < 1 || slot > 7) return openBus();
    if (auto* p = slots[slot].get())
        return p->expansionRomRead(static_cast<uint16_t>(addr - 0xC800));
    return openBus();
}

void SlotBus::expansionRomWrite(uint16_t addr, uint8_t v)
{
    if (addr < 0xC800 || addr > 0xCFFF) return;
    if (addr == 0xCFFF) { deactivateExpansion(); return; }
    const int slot = activeExpansionSlot;
    if (slot < 1 || slot > 7) return;
    if (auto* p = slots[slot].get())
        p->expansionRomWrite(static_cast<uint16_t>(addr - 0xC800), v);
}

void SlotBus::rebuildActiveCache()
{
    activeCount_ = 0;
    for (auto& s : slots)
        if (s) activeCards_[static_cast<size_t>(activeCount_++)] = s.get();
    busSnooper_ = nullptr;
    for (auto& s : slots)
        if (s && s->snoopsBus()) { busSnooper_ = s.get(); break; }
}

void SlotBus::advanceCycles(int cycles)
{
    if (cycles <= 0) return;
#ifndef NDEBUG
    // The cache is raw pointers into `slots`; if a mutation ever forgets to
    // rebuild it, the failure would be a skipped card or a freed pointer, both
    // near-impossible to trace back here. Fail loudly in debug instead.
    {
        int n = 0;
        for (auto& s : slots) {
            if (!s) continue;
            assert(n < activeCount_ && activeCards_[static_cast<size_t>(n)] == s.get()
                   && "SlotBus active-card cache is stale — a mutation of slots[] "
                      "did not call rebuildActiveCache()");
            ++n;
        }
        assert(n == activeCount_ && "SlotBus active-card cache has extra entries");
    }
#endif
    for (int i = 0; i < activeCount_; ++i)
        activeCards_[static_cast<size_t>(i)]->advanceCycles(cycles);
}

void SlotBus::broadcastVideoSwitch(uint16_t addr)
{
    for (auto& s : slots) if (s) s->onVideoSoftSwitch(addr);
}

void SlotBus::broadcastVideoSwitchWrite(uint16_t addr, uint8_t value)
{
    for (auto& s : slots) if (s) s->onVideoSoftSwitchWrite(addr, value);
}

void SlotBus::reset()
{
    for (auto& s : slots) if (s) s->onReset();
    // Note: we deliberately do NOT clear activeExpansionSlot — Apple II
    // hardware reset doesn't drop the expansion-ROM enable, the latch is
    // cleared only by $CFFF or by losing the slot's $CnXX access.
}

void SlotBus::clear()
{
    for (auto& s : slots) {
        if (s) {
            s->onUnplug();
            s->detachFromBus();
            s.reset();
        }
    }
    activeExpansionSlot = -1;
    rebuildActiveCache();
}
