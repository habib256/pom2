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

// SlotPeripheral — abstract interface for cards plugged into the Apple II
// expansion bus (slots 0-7). Each card sees three address windows:
//
//   $C0(8+N)X        16-byte device-select per slot
//                    (slot 1 = $C090-$C09F, ..., slot 7 = $C0F0-$C0FF.
//                     slot 0 = $C080-$C08F = language card slot.)
//   $C(N)XX          256-byte slot ROM (slots 1-7 only — slot 0 has no
//                    slot ROM space; $C000-$C0FF is the I/O page).
//   $C800-$CFFF      2 KB expansion ROM, shared between all slots —
//                    one slot active at a time, switched by the SlotBus
//                    based on which slot's ROM was last touched.
//
// All callbacks are invoked from the CPU thread under EmulationController's
// stateMutex; cards don't need to lock state for those. Lifecycle hooks
// (onPlug / onUnplug / onReset) are also called under that lock so card
// initialisation can mutate the bus freely.
//
// Default implementations match an empty/inactive socket (read = $FF "open
// bus", writes silently dropped). Override only the methods your card
// actually implements.

#ifndef POM2_SLOT_PERIPHERAL_H
#define POM2_SLOT_PERIPHERAL_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace pom2 { class SmartPortBusUnit; }

class SlotBus;

class SlotPeripheral
{
public:
    virtual ~SlotPeripheral() = default;

    /// Short human-readable name (e.g. "Disk II", "Language Card", "80col").
    /// Surfaced in the Hardware menu / status bar.
    virtual std::string_view name() const = 0;

    /// $C0(8+N)X — 16-byte device-select range. `low4` is the low nibble
    /// of the address (0..15). Reads and writes are dispatched separately
    /// so a card can implement asymmetric semantics (e.g. Disk II's $C0nE
    /// is "read mode" on read, "set Q6" on write — both legal accesses).
    virtual uint8_t deviceSelectRead (uint8_t /*low4*/)              { return 0xFF; }
    virtual void    deviceSelectWrite(uint8_t /*low4*/, uint8_t /*v*/) {}

    /// $C(N)XX — 256-byte slot ROM. `low8` is the low byte of the address
    /// (0..255). On real hardware most cards leave this read-only, but a
    /// handful (notably the Sweet Microsystems Mockingboard, which decodes
    /// its 6522 VIAs at $Cn00 / $Cn80 instead of in the 16-byte device-
    /// select range) treat $CnXX as memory-mapped I/O. The SlotBus
    /// forwards both reads and writes; cards that don't override
    /// `slotRomWrite` get the default no-op (matching ROM behaviour).
    virtual uint8_t slotRomRead (uint8_t /*low8*/)              { return 0xFF; }
    virtual void    slotRomWrite(uint8_t /*low8*/, uint8_t /*v*/) {}

    /// $C800-$CFFF — 2 KB expansion ROM. `offset` is the byte offset into
    /// the 2 KB window (0..0x7FE; 0x7FF is intercepted by SlotBus as the
    /// "$CFFF disable" switch and never reaches the card). Expansion ROM
    /// is conventionally read-only, but we forward writes anyway so cards
    /// that hide soft switches in this window (rare) keep working.
    virtual uint8_t expansionRomRead (uint16_t /*offset*/) { return 0xFF; }
    virtual void    expansionRomWrite(uint16_t /*offset*/, uint8_t /*v*/) {}

    /// Lifecycle. `onPlug` / `onUnplug` flank a card insertion; `onReset`
    /// fires on Apple II hardware reset (Ctrl-Reset).
    virtual void onPlug()   {}
    virtual void onUnplug() {}
    virtual void onReset()  {}

    /// Rewind / snapshot hooks. `appendSnapshotState` serializes the card's
    /// volatile runtime state — NOT its ROMs or mounted media — by appending
    /// to `out`; `loadSnapshotState` restores from a blob a previous
    /// `appendSnapshotState` produced. Cards with no rewindable state keep
    /// the default no-ops (and write no per-slot section). A card MUST tag
    /// its blob with its own magic/version and ignore foreign/old data on
    /// load, because the slot it lands in might hold a different card than
    /// when the snapshot was taken. The rewind ring buffer and the
    /// AI-control /snapshot API drive these via per-slot "SLOTn" sections
    /// (see MachineSnapshot). Restoring drive position keeps an in-progress
    /// disk read from corrupting after a rewind.
    /// Emulated CPU clock changed (NTSC ↔ PAL profile switch). Cards whose
    /// internal timebase is a REAL-TIME reference expressed in CPU cycles
    /// must re-derive it here — otherwise they run 0.7 % off under PAL.
    /// Default: no-op (most cards count guest cycles, which is already
    /// standard-correct). Implemented by MockingboardCard (the audio
    /// thread's emuCycles replay cursor), PhasorCard (same body),
    /// ClockCard (uPD1990AC TP) and WorkstationCard (its own 65C02 and
    /// SCC timebases). Since 2026-09-07 `setVideoStandard` fans this out to
    /// EVERY plugged card plus the speaker and the cassette, not only the
    /// Mockingboard: a card plugged later via Slot Config used to keep the
    /// NTSC constant under PAL.
    virtual void setCpuClock(double /*hz*/) {}

    virtual void appendSnapshotState(std::vector<uint8_t>& /*out*/) const {}
    virtual void loadSnapshotState(const uint8_t* /*data*/, std::size_t /*len*/) {}

    /// Cycle pacing — for cycle-driven peripherals (Disk II's stepper, a
    /// future serial card's UART, …). Forwarded by SlotBus from
    /// Memory::advanceCycles().
    virtual void advanceCycles(int /*cycles*/) {}

    /// System soft-switch broadcast — fires for switches outside the
    /// per-slot device-select range that some cards still need to observe
    /// (Le Chat Mauve / Video-7 sniff $C00C/$C00D 80COL and $C05E/$C05F
    /// AN3 to clock their 2-bit FIFO mode register). Forwarded by
    /// Memory::softSwitchAccess() via SlotBus::broadcastVideoSwitch().
    virtual void onVideoSoftSwitch(uint16_t /*addr*/) {}
    /// The WRITE side of the same broadcast, with the data byte — for a card
    /// that latches data off a shared window (the Chat Mauve Eve loads its
    /// CPREG from every write to $C0B0-$C0BF). Defaults to the address-only
    /// hook so cards that only decode addresses see both R and W as before.
    virtual void onVideoSoftSwitchWrite(uint16_t addr, uint8_t /*value*/) { onVideoSoftSwitch(addr); }

    /// DMA bus mastery — for cards that halt the 6502 and drive the bus
    /// with their own processor (Microsoft SoftCard's Z80; MAME models
    /// this as the a2bus DMA daisy chain). While `dmaActive()` returns
    /// true, EmulationController's frame loop hands each CPU budget
    /// slice to `dmaRun(cycles)` instead of `M6502::run` — `cycles` is
    /// in **6502 cycles** and the card must keep `Memory::advanceCycles`
    /// fed in that same domain (emuCycles never leaves the 6502 clock).
    /// Return the cycles actually consumed (overshoot allowed, same
    /// contract as M6502::run). At most one card should claim DMA;
    /// SlotBus::dmaClaimant() picks the lowest slot if several do.
    ///
    /// Hand-over latency contract: the claimant scan happens once per
    /// ~4096-cycle chunk. For an instruction-precise grant a claiming
    /// card must ALSO end the 6502's in-flight run() chunk by calling
    /// M6502::stop() from the access that claims the bus (SoftCardZ80
    /// does; see its slotRomWrite) — a card that only flips dmaActive()
    /// gets chunk-granular arbitration, up to 4096 cycles late.
    /// CPU clock multiplier this card imposes on the host 6502 — for
    /// ACCELERATORS (Applied Engineering TransWarp, Zip Chip, …), which
    /// speed up the machine's own processor rather than adding one of
    /// their own. EmulationController multiplies its per-frame cycle
    /// budget by this once per frame, so a card may vary it freely; see
    /// TranswarpCard.h for why frame-rate sampling of a sub-frame duty
    /// cycle is exact in aggregate. 1.0 = stock speed, and any value
    /// <= 0 is ignored. SlotBus takes the lowest slot that is not 1.0.
    virtual double cpuSpeedMultiplier() const { return 1.0; }

    /// Bus snooping — for cards that watch addresses OUTSIDE their own slot
    /// windows. On real hardware every card sees every cycle; POM2 only
    /// forwards to cards that ask, because the snoop sites sit on the path
    /// of every $C0xx and slot-ROM access. `snoopsBus()` is read at plug
    /// time and cached by SlotBus, so it must not change over a card's life.
    ///
    /// Return true from `busSnoop` to CONSUME the access — the machine then
    /// skips its own handling of it. Only a card that genuinely takes the
    /// address off the bus should do that (the TransWarp does, for its
    /// $C074 speed register, which never reaches the Apple).
    virtual bool snoopsBus() const { return false; }
    virtual bool busSnoop(uint16_t /*addr*/, bool /*isWrite*/, uint8_t /*value*/)
    {
        return false;
    }

    virtual bool dmaActive() const { return false; }
    virtual int  dmaRun(int /*cycles6502*/) { return 0; }

    /// On //c-class machines the forced INTCXROM masks ALL slot ROM
    /// ($C100-$CFFF). POM2 punches a single hole for a built-in SmartPort
    /// whose $Cn00 firmware substitutes for the machine's own — on the 16 KB
    /// //c, which has no 3.5" firmware, and for the //c+'s host-served HDV.
    /// Only while the card actually holds bootable media, so an empty
    /// SmartPort never presents a half-working bootable signature to the
    /// //c autostart (which would JMP $0801 into garbage). The 32 KB //c
    /// does not use this at all: its real firmware serves the port through
    /// `smartPortBusUnit()` below. Default: ROM stays masked. See
    /// Memory::memRead + SmartPortCard.
    virtual bool exposesIicOnboardRom() const { return false; }

    /// SmartPort **bus** units this card can put on a //c's external disk
    /// port. The 32 KB //c's own firmware talks to its 3.5" drive as an
    /// intelligent device over the IWM (`SmartPortBusDevice`); a card that
    /// answers here is what that firmware finds, enumerates and boots from,
    /// with no ROM substitution at all. Default: nothing on the port.
    virtual int smartPortBusUnitCount() const { return 0; }
    virtual pom2::SmartPortBusUnit* smartPortBusUnit(int /*index*/) { return nullptr; }

    /// Slot number assigned by the bus at plug-time (1..7), or -1 before
    /// `SlotBus::plug()` adopts the card. Concrete cards may still carry
    /// their own constructor-time slot field for ROM addressing reasons
    /// (the SSC bakes slot into its PR# trampolines), but `busSlot()` is
    /// the authoritative source once attached.
    int busSlot() const { return busSlot_; }

    /// Whether this slot's contribution to the wire-OR'd CPU IRQ line is
    /// currently asserted. Mirrors what `assertIrq()` last published to
    /// the bus — useful for debug panels and tests.
    bool slotIrqAsserted() const { return irqAsserted_; }

protected:
    /// Assert (true) or release (false) this slot's contribution to the
    /// CPU's wire-OR'd IRQ line. Idempotent — repeated true→true or
    /// false→false calls are no-ops, so cards don't need their own edge
    /// tracking. Safe to call before plug (becomes a no-op until SlotBus
    /// attaches the card). `SlotBus::plug()` and `SlotBus::unplug()`
    /// auto-release any still-asserted bit before letting the card go,
    /// so cards rarely need to clear in `onUnplug()` themselves.
    void assertIrq(bool asserted);

private:
    friend class SlotBus;
    /// Called by SlotBus::plug() right after onPlug(). Wires the card to
    /// its bus + slot number so `assertIrq()` can fan out.
    void attachToBus(SlotBus* bus, int slot);
    /// Called by SlotBus::plug()/unplug() right before onUnplug(). Drops
    /// any pending IRQ contribution and clears the bus pointer so a
    /// post-unplug stray assertIrq() is a no-op.
    void detachFromBus();

    SlotBus* bus_       = nullptr;
    int      busSlot_   = -1;
    bool     irqAsserted_ = false;
};

#endif // POM2_SLOT_PERIPHERAL_H
