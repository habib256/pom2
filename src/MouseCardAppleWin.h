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

// MouseCardAppleWin — high-level emulation of the Apple II Mouse Interface
// card, ported from AppleWin's `source/MouseInterface.cpp` (class
// CMouseInterface). The MAME-faithful sibling `MouseCard` emulates the
// MC68705P3 microcontroller cycle-by-cycle from the Apple 341-0269 mask
// ROM; this variant follows AppleWin's approach instead: the MC6821 PIA
// is real (shared MC6821 chip model) but the MCU side is *synthesised* in
// C++ — reads/writes through the PIA are interpreted as a command byte
// stream and replied to with mouse state computed directly from host
// input. As a result this card only needs the slot EPROM
// (`mouse_341-0270-c.bin`); no 341-0269 MCU mask ROM required.
//
// Protocol mirrored from AppleWin's OnCommand / OnWrite:
//
//   $00 MOUSE_SET     1-byte    Set mode (firmware writes MODE_MOUSE_ON /
//                                MODE_INT_VBL / MODE_INT_BUTTON / etc.)
//   $10 MOUSE_READ    6-byte    Read X-lo, X-hi, Y-lo, Y-hi, status
//   $20 MOUSE_SERV    2-byte    Read pending IRQ source + clear CPU IRQ
//   $30 MOUSE_CLEAR   1-byte    Clear position + state
//   $40 MOUSE_POS     5-byte    Set absolute position
//   $50 MOUSE_INIT    3-byte    Init (clamp 0..1023, pos = 0)
//   $60 MOUSE_CLAMP   5-byte    Set X or Y clamp window (LSB of cmd byte
//                                = axis select: 0 = X, 1 = Y)
//   $70 MOUSE_HOME    1-byte    Re-home to (0, 0)
//   $90 MOUSE_TIME    1..4 byte VBL-time command (no-op in HLE)
//
// PIA Port B handshake (AppleWin's On6821_B): the firmware uses BIT5 of
// Port B as a write-strobe (1→0 = "byte on Port A is for the MCU") and
// BIT4 as a read-strobe (1→0 = "advance to next reply byte"). The
// command buffer fills up command-then-data; the first byte's high
// nibble selects the command and sets `nDataLen`. PIA Port B bits 1..3
// drive the slot-ROM bank (8 banks × 256 B), exactly like the real card.
// BIT6 and BIT7 are status bits driven *back* to the firmware (read-ack
// + write-ack) so the firmware's polling loops complete.
//
// Interrupt model: `OnMouseEvent` is called on every host input change
// and (optionally) once per emulated VBL — it sets the matching bits of
// `byState` (movement / button / VBL) under the current mode mask and
// raises the slot IRQ. The next MOUSE_SERV command clears it. AppleWin's
// `CpuIrqAssert(IS_MOUSE)` maps to `SlotPeripheral::assertIrq(true)`.

#ifndef POM2_MOUSE_CARD_APPLEWIN_H
#define POM2_MOUSE_CARD_APPLEWIN_H

#include "MC6821.h"
#include "SlotPeripheral.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class MouseCardAppleWin : public SlotPeripheral
{
public:
    static constexpr int kDefaultSlot = 4;

    explicit MouseCardAppleWin(int slot = kDefaultSlot);

    int getSlot() const { return slot_; }

    /// Load the Apple 341-0270-c slot EPROM (2048 bytes). No second ROM
    /// needed — the MCU side is HLE'd. Returns false on size mismatch or
    /// open failure.
    bool loadRom(const std::string& slotRomPath);
    bool isReady() const { return slotRomLoaded; }

    /// Host-mouse position update — same signature as MouseCard so the UI
    /// layer in MainWindow can drive either variant. `rawX`/`rawY` are
    /// running 8-bit counters (the screen-hole closed-loop in MainWindow
    /// drives this). Internally we compute signed 8-bit deltas with wrap
    /// correction and apply them via the firmware's absolute clamp window.
    /// Header-inline on purpose: AiControlServer's /mouse endpoint drives
    /// this card without linking MouseCardAppleWin.o (see kCardName below),
    /// so the call must not need a symbol from this TU.
    void setHostMouse(uint8_t rawX, uint8_t rawY, bool button)
    {
        hostX.store(rawX, std::memory_order_relaxed);
        hostY.store(rawY, std::memory_order_relaxed);
        hostButton.store(button, std::memory_order_relaxed);
        // Bumped LAST, and read first by advanceCycles: the CPU thread uses
        // it to skip the whole drain while the pointer is still. Relaxed is
        // enough — the three shadows above are themselves atomics, so the
        // worst a reordering can do is drain one instruction early with the
        // previous sample, which the next generation change corrects.
        hostGen.fetch_add(1, std::memory_order_relaxed);
    }

    /// VBL pacing in CPU cycles between MODE_INT_VBL events. Defaults to
    /// the NTSC frame (17045 cycles ≈ 60 Hz); PAL profiles pass their 50 Hz
    /// frame budget (20313) at plug time so MODE_INT_VBL stays locked to
    /// the machine's actual frame rate instead of drifting against the
    /// 312-line beam. Mirrors AppleWin, which fires its VBL hook from the
    /// host frame loop rather than a hard-wired constant.
    void setVblCycles(int cycles)
    {
        if (cycles > 0) vblCycles_ = cycles;
    }
    int  vblCycles() const { return vblCycles_; }

    /// Snapshot of internal mouse-firmware state for the Mouse Inspector
    /// panel. Not part of the AppleWin protocol — POM2-only diagnostic
    /// view of the HLE'd MCU's working set: clamp window, current
    /// position iX/iY (firmware-resolved cursor inside the clamp),
    /// last-MOUSE_READ snapshot nX/nY, button shadows, mode/state bytes,
    /// and the PIA port latches. Read on the UI thread; the underlying
    /// fields are scalars touched by the CPU thread, so values may be
    /// momentarily stale but never torn for the purposes of a UI panel.
    struct DebugSnapshot {
        int iX, iY;
        int nX, nY;
        int iMinX, iMaxX;
        int iMinY, iMaxY;
        bool bBtn0, bBtn1;
        bool bPrevBtn0, bPrevBtn1;
        uint8_t byMode;
        uint8_t byState;
        uint8_t by6821A;
        uint8_t by6821B;
        int     buffPos;
        int     dataLen;
        uint8_t lastCmd;       // byBuff[0]
    };
    DebugSnapshot debugSnapshot() const;

    /// Stable identity tag, also returned by name(). AiControlServer's
    /// /mouse endpoint identifies this card by comparing name() against
    /// kCardName and then static_cast'ing, instead of dynamic_cast — a
    /// dynamic_cast would reference this class's typeinfo (emitted in
    /// MouseCardAppleWin.o) and force every headless binary that links
    /// AiControlServer.cpp to link this TU too. Keep name() returning
    /// exactly this constant.
    static constexpr std::string_view kCardName{"Mouse (AppleWin HLE)"};

    // ─── SlotPeripheral overrides ──────────────────────────────────────
    std::string_view name() const override { return kCardName; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead(uint8_t low8) override;
    void    advanceCycles(int cycles) override;
    void    onReset() override;

    /// Snapshot/rewind: 'MAW1'-tagged blob carrying the HLE'd MCU's whole
    /// working set — mode/state bytes, position + clamp window, button
    /// shadows, the command-byte cursor (byBuff/nBuffPos/nDataLen), the
    /// Port A/B latch shadows, the VBL pacer and the slot IRQ level —
    /// wrapping the real MC6821's own blob. Without this the card was
    /// skipped entirely by MachineSnapshot (empty blob ⇒ no SLOTn
    /// section), so a rewind past a MOUSE_SET left `byMode` holding
    /// MODE_INT_VBL and the card kept interrupting a guest whose handler
    /// no longer existed, with only a MOUSE_SERV that would never come to
    /// release the line.
    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;
    // //c-class punches the forced INTCXROM mask for this card's $Cn00
    // firmware so PR#4 runs the AppleWin EPROM (which drives our PIA at
    // $C0C0) instead of the //c's on-board mouse firmware (which would
    // poke IOU hardware POM2 doesn't model — making the mouse a no-op).
    bool    exposesIicOnboardRom() const override { return slotRomLoaded; }

private:
    int      slot_;
    MC6821   pia;
    std::array<uint8_t, 0x800> slotRom{};
    bool     slotRomLoaded = false;

    // ── PIA Port A/B latch shadows (AppleWin m_by6821A / m_by6821B). ──
    uint8_t  by6821A = 0;
    uint8_t  by6821B = 0x40;        // BIT6 starts set — matches AppleWin Reset

    // ── Command-byte buffer (AppleWin m_byBuff / m_nBuffPos / m_nDataLen).
    uint8_t  byBuff[8] = { 0 };
    int      nBuffPos  = 0;
    int      nDataLen  = 1;

    // ── HLE'd MCU state (AppleWin m_byState / m_byMode / m_iX/Y / clamps).
    uint8_t  byState   = 0;
    uint8_t  byMode    = 0;
    int      iX = 0, iY = 0;
    int      nX = 0, nY = 0;
    int      iMinX = 0, iMaxX = 1023;
    int      iMinY = 0, iMaxY = 1023;
    bool     bButtons[2] = { false, false };
    bool     bBtn0 = false, bBtn1 = false;

    // ── Host input shadow (UI → CPU thread). ──────────────────────────
    std::atomic<uint8_t> hostX     { 0 };
    std::atomic<uint8_t> hostY     { 0 };
    std::atomic<bool>    hostButton{ false };
    /// Bumped by setHostMouse, read by advanceCycles: while it is unchanged
    /// there is nothing to drain and the per-instruction poll is skipped.
    std::atomic<uint32_t> hostGen  { 0 };
    uint32_t lastHostGen_ = 0;
    uint8_t  lastHostX = 0;
    uint8_t  lastHostY = 0;
    bool     lastHostButton = false;
    bool     hostPrimed = false;

    // ── VBL pacing. Cycles per MODE_INT_VBL event; profile-plumbed via
    //    setVblCycles (17045 NTSC default / 20313 PAL). ─────────────────
    int      vblCycles_     = 17045;
    int      vblCycleAccum  = 0;

    // ── Internal hooks (AppleWin parity) ─────────────────────────────
    void onPiaPortAOut(uint8_t v);    // = On6821_A
    void onPiaPortBOut(uint8_t v);    // = On6821_B
    void onCommand();
    void onWrite();
    void onMouseEvent(bool vbl);
    void clearState();
    int  clampX();
    int  clampY();
    void setClampX(int lo, int hi);
    void setClampY(int lo, int hi);
    void setPositionAbs(int x, int y);
    void setPositionRel(int dx, int dy);
    void setButton(int idx, bool down);
    void pollHostInput();             // pump atomics → setPositionRel/Button
};

#endif // POM2_MOUSE_CARD_APPLEWIN_H
