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

// MouseCard — Apple II Mouse Interface card. Verbatim port of MAME
// `src/devices/bus/a2bus/mouse.cpp`. The card is a small PCB with:
//
//   * MC68705P3 microcontroller — the on-card CPU running the mouse
//     firmware (Apple part 341-0269), responsible for sampling host
//     mouse motion + buttons and serving the Apple II side.
//   * MC6821 PIA — the bus-side interface. The Apple II 6502 talks to
//     four registers at $C0nX; the PIA bridges them to the MCU.
//   * 8516 EPROM — 2 KB, the Apple II-side firmware (Apple part
//     341-0270-c) bank-switched into the slot ROM window via PIA Port B.
//
// MAME wiring (PCB schematic at apple2.org.za, MAME mouse.cpp lines 42-58):
//
//   PIA Port A      ↔ MCU Port A      (bidirectional, internal pull-ups)
//   PIA PB4..PB7    ↔ MCU PC0..PC3    (bidirectional)
//   PIA PB1..PB3    →  EPROM A8..A10  (8 banks of 256-byte slot ROM)
//   PIA PB0         →  sync latch     (not modelled — used by firmware
//                                      for read/write pacing)
//   PIA CB1         tied high (10 kΩ pull-up)
//
//   MCU PB0/PB1     ←  mouse X direction / X gate   (POM2 labels X0/X1 ;
//                                                    MAME labels these X1/X0)
//   MCU PB2/PB3     ←  mouse Y direction / Y gate   (Y0/Y1, matches MAME)
//   MCU PB4/PB5     ←  N/C
//   MCU PB6         →  slot IRQ       (active low — firmware writes
//                                      0 to PB6 to assert)
//   MCU PB7         ←  mouse button   (active low: 0 = pressed)
//
//   MCU crystal = 2 043 600 Hz (MAME's `M68705P3(config, m_mcu,
//   2043600)`), divided by 4 inside the 68705: 510 900 machine cycles a
//   second, half the Apple II clock. POM2 paces the MCU from the Apple II's
//   `advanceCycles()` budget at that 1:2 ratio.
//
// Slot ROM bank-select. The 2 KB EPROM holds 8 banks of 256 bytes.
// The active bank is `(PIA Port B & 0x0E) << 7` — i.e. PB1, PB2, PB3
// drive A8, A9, A10 of the EPROM. The Apple II reads the slot ROM via
// the standard $C100-$C7FF window (16 reads return the same byte; the
// firmware writes PIA Port B, then the next slot-ROM read reflects the
// new bank). MAME `read_cnxx`: `m_rom[offset | m_rom_bank]`.
//
// IRQ generation. The MCU drives PB6 low to assert the slot IRQ. POM2
// forwards this to the M6502's IRQ line via the SlotPeripheral
// advance/reset hooks. PIA's IRQ-A and IRQ-B outputs are wired in MAME
// but go unused (the firmware doesn't enable PIA-side interrupts on the
// stock Mouse Card).
//
// What's NOT modelled (deliberate, scope-bounded omissions):
//
//   * The PAL16R4 sequencer at U2A. It glues E-clock and read/write
//     strobes into the PIA's chip-select path. We dispatch directly via
//     the SlotBus → PIA bridge, which is functionally equivalent for
//     all firmware-visible behaviour.
//   * PIA Port B bit 0 ("sync latch") — the firmware uses this to
//     pace its scan loop against the Apple II video timing. We
//     enable IRQs unconditionally; the firmware's own MCU timer + the
//     bus's natural pacing keep things synchronised enough for
//     mouse-driven applications (MousePaint, MouseCalc, etc.) to work.
//   * Mouse motion clamping ranges from `ClampMouse`. The MCU firmware
//     handles all clamping; POM2 just feeds raw host pixel deltas.

#ifndef POM2_MOUSE_CARD_H
#define POM2_MOUSE_CARD_H

#include "M68705P3.h"
#include "MC6821.h"
#include "SlotPeripheral.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

class MouseCard : public SlotPeripheral
{
public:
    static constexpr int kDefaultSlot = 4;

    /// Construct in `slot` (default 4). Call `loadRoms()` before plugging
    /// the card into the slot bus. IRQ routing is auto-wired by SlotBus
    /// via `SlotPeripheral::assertIrq()` — no manual CPU wiring required.
    explicit MouseCard(int slot = kDefaultSlot);

    int getSlot() const { return slot_; }

    /// Load both Apple-copyright ROMs. `slotRomPath` must be exactly
    /// 2048 bytes (the EPROM dump, Apple 341-0270-c.4b); `mcuRomPath`
    /// must also be 2048 bytes (the MCU mask ROM, Apple 341-0269.2b).
    /// Returns false on size mismatch or open failure. Without both
    /// ROMs the card refuses to plug — MainWindow's
    /// `plugSlotsFromSettings()` checks the result.
    bool loadRoms(const std::string& slotRomPath, const std::string& mcuRomPath);

    bool isReady() const { return slotRomLoaded && mcu.isRomLoaded(); }

    /// Host-mouse position update. `rawX` and `rawY` are running
    /// counters in Apple II screen-coordinate units (0..0xFF wrap is
    /// fine; the MCU firmware computes deltas via 8-bit subtraction
    /// with wrap). `button` is true while the mouse button is held.
    /// Called from the UI thread once per frame.
    void setHostMouse(uint8_t rawX, uint8_t rawY, bool button);

    /// Telemetry: total MCU machine cycles actually retired since the last
    /// `onReset()`. Divided by the CPU cycles handed to `advanceCycles()`
    /// this must converge on MCU_CLOCK_NUMERATOR / MCU_CLOCK_DENOMINATOR
    /// (0.5), i.e. MAME's `M68705P3(config, m_mcu, 2043600)` divided by 4
    /// against the 1.02 MHz bus. Pinned by tests/mouse_card_smoke_test.cpp — dropping
    /// the run overshoot instead of billing it used to clock the MCU
    /// 26-50 % fast. Never affects emulation.
    uint64_t mcuCyclesRun() const { return mcuCyclesRun_; }

    // ─── SlotPeripheral overrides ──────────────────────────────────────
    std::string_view name() const override { return "Mouse"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead(uint8_t low8) override;
    void    advanceCycles(int cycles) override;
    void    onReset() override;

    /// Snapshot/rewind: 'MSE1'-tagged blob wrapping the embedded
    /// M68705P3 MCU (registers + 112 B RAM + ports + timer + interrupt
    /// latches) and the MC6821 PIA, plus the card's own bridge state
    /// (ROM bank, PIA→MCU port shadows, quadrature counters, pacing).
    /// The MCU is the reason this card was the last one without
    /// serialization: a rewind mid-mouse-interrupt used to drop the
    /// restored firmware into the LIVE MCU's handshake.
    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;
    void    onUnplug() override;

private:
    int slot_;
    M68705P3 mcu;
    MC6821   pia;
    std::array<uint8_t, 0x800> slotRom{};
    bool     slotRomLoaded = false;

    // EPROM bank-select: 0..0x700 in steps of 0x100 (8 banks of 256 B).
    uint16_t romBank = 0;

    // PIA → MCU bridge state. The PIA's writepa / writepb callbacks
    // marshal here, then push to the MCU via setPortInput.
    uint8_t portAtoMcu = 0xFF;     // Port A: pulled up
    uint8_t portCtoMcu = 0x00;     // PB4-7 / PC0-3: NOT pulled up (MAME tspb=0)

    // Slot IRQ contribution is tracked by SlotPeripheral (edge-only
    // delivery via assertIrq); MAME-faithful set/clear semantics emerge
    // automatically from the base class's irqAsserted_ cache.

    // Host mouse state. Updated from setHostMouse (UI thread); read
    // from mcuPortBRead (CPU thread). std::atomic keeps the writes
    // visible across threads without a mutex — the values are scalar.
    std::atomic<uint8_t> hostX { 0 };
    std::atomic<uint8_t> hostY { 0 };
    std::atomic<bool>    hostButton { false };

    // Per-axis running quadrature state, lifted from MAME's m_last /
    // m_count arrays. Each call to mcuPortBRead emits at most one
    // quadrature edge per axis (CLK toggle + DIR set/clear).
    int16_t lastAxis[2] = { 0, 0 };
    int16_t countAxis[2] = { 0, 0 };
    uint8_t portBState = 0xFF;     // running snapshot of MCU PB pins

    // MCU pacing — the 68705 runs at 2× the Apple II clock. Holds the
    // signed cycle budget owed to the MCU: advanceCycles credits it and
    // debits what `mcu.run()` actually retired, so the instruction the
    // MCU was mid-way through when the budget expired is paid for out of
    // the next tick instead of being run for free. Goes negative while a
    // long instruction is being amortised.
    int mcuCycleAccum = 0;

    // Telemetry only (see mcuCyclesRun()); not part of the snapshot blob.
    uint64_t mcuCyclesRun_ = 0;

    // ── Internal hooks ────────────────────────────────────────────────
    void onPiaPortAOut(uint8_t v);
    void onPiaPortBOut(uint8_t v);
    void onMcuPortWrite(int port);
    uint8_t onMcuPortRead(int port);
    uint8_t mcuPortBRead();
    void    updateAxis(int axis, uint8_t dirBit, uint8_t clkBit);
};

#endif // POM2_MOUSE_CARD_H
