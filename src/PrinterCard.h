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

// PrinterCard — synthetic Apple Parallel-style printer interface card.
// Captures every byte the Apple II "prints" through PR#n into a host-side
// spool buffer, which the UI saves to .txt / .pdf to match the macOS
// print-to-PDF workflow.
//
// Slot 1 by convention (the slot DOS / ProDOS / Pascal scan for the
// canonical printer device), but any slot 1-7 works; the slot ROM bakes
// the slot number into the CSWL/CSWH redirection and the device-select
// address inside the output trampoline.
//
// Wire model
// ----------
// Data port at $C0(8+s)0 AND $C0(8+s)1 — write = enqueue the byte to the
// spool; read = always 0xFF (printer permanently "ready"). All other
// device-select offsets read 0xFF / writes ignored.
//
// TWO offsets, because there are two ways in. Offset 1 is what our own slot
// ROM's trampoline writes (the `PR#n` + COUT path below). Offset 0 is the
// data latch on the real Apple Parallel Printer Interface, and it is what
// software that drives the card DIRECTLY writes — which is what graphics
// programs do instead of going through COUT. The Print Shop's "Apple
// Parallel Interface" driver never reads our ROM at all: character to
// offset 0, same byte to offset 2 as the strobe, poll offset 4 for ready.
// With offset 1 alone, its whole `ESC G` page went into the void while
// offset 4 kept answering "ready", so it reported the job printed and
// nothing came out (bug hunt, 2026-08-18). Offset 2 stays ignored on
// purpose: the strobe carries a copy of the byte, and taking it too would
// double every character.
//
// PR#n protocol (minimal — DOS/BASIC works, no Pascal driver needed)
// ------------------------------------------------------------------
// `PR#n` from Applesoft/Monitor JSRs to $Cn00. Our slot ROM hooks
// CSWL/CSWH ($36/$37) so subsequent COUT calls jump to our 3-byte
// output handler at $Cn31, which stores A into the data port and RTSes.
// A is preserved (STA doesn't clobber it), so callers that re-read A
// after COUT keep working.
//
// Slot ROM layout (s = slot number, slotHi = $C0+s):
//
//   $Cn00  4C 20 ss  JMP $Cn20                     (skip sig bytes)
//   $Cn05  38        Pascal 1.1 sig 1               (SEC)
//   $Cn07  18        Pascal 1.1 sig 2               (CLC)
//   $Cn0B  01        Pascal 1.1 firmware revision
//   $Cn0C  10        Pascal device class 1 = printer (kPascalPrinterClass)
//   $Cn20  A9 31     LDA #$31                       (CSWL low byte)
//   $Cn22  85 36     STA $36                        (= low byte of $Cn31)
//   $Cn24  A9 ss     LDA #slotHi                    (CSWH = $C0+s)
//   $Cn26  85 37     STA $37
//   $Cn28  60        RTS
//   $Cn31  8D 91 c0  STA $C0(8+s)1                  (data port write)
//   $Cn34  60        RTS
//
// Everything else is $EA (NOP) padding so a sloppy probe falling
// through doesn't trip on stale bytes.
//
// Why synthetic (no PROM dump)
// ----------------------------
// The real Apple Parallel Printer Card ROM had hardware-specific
// strobe/busy bit-banging on its 6821 PIA that doesn't translate to a
// host-side spool. Following the `ProDOSHardDiskCard` model (DEV.md §
// "ProDOSHardDiskCard"), we ship a synthetic ROM whose only job is to
// thread COUT bytes into our C++ buffer — no MAME parity to chase
// (MAME `a2parprn` exists but emulates a real bit-banged port that
// can't be observed without a physical printer). PDF rendering and
// raw .txt dump live in the UI, not in the card.

#ifndef POM2_PRINTER_CARD_H
#define POM2_PRINTER_CARD_H

#include "SlotPeripheral.h"

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class PrinterCard : public SlotPeripheral
{
public:
    static constexpr int kDefaultSlot = 1;
    static constexpr size_t kMaxSpoolBytes = 4u * 1024u * 1024u;

    explicit PrinterCard(int slot = kDefaultSlot);

    int getSlot() const { return slot_; }

    /// True when the hand-assembled slot ROM did not fit its declared layout.
    bool romLayoutError() const { return romLayoutError_; }

    // ─── SlotPeripheral overrides ────────────────────────────────────────
    std::string_view name() const override { return "Printer (Parallel)"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead(uint8_t low8) override { return rom_[low8]; }
    void    onReset() override {}   // spool survives Ctrl-Reset (user clears)

    // ─── Spool access ────────────────────────────────────────────────────

    /// Retained raw tail since the last clear (at most kMaxSpoolBytes).
    /// High bit NOT stripped.
    std::vector<uint8_t> spoolBytes() const;

    /// Spool rendered as a text string: high bit stripped, CR ($0D) mapped
    /// to LF ($0A) so it displays naturally in a host text widget /
    /// .txt file. Form-feed ($0C) preserved as-is.
    std::string spoolText() const;

    /// Append spool bytes at absolute index >= `from` and return the new
    /// monotonic total. Lets a streaming consumer pick up
    /// only what arrived since its last poll instead of re-copying the whole
    /// spool every frame. `from` beyond the end (spool cleared meanwhile)
    /// replays from 0.
    size_t drainSpoolFrom(size_t from, std::vector<uint8_t>& out) const;

    /// Number of bytes written since the last clear.
    size_t bytesWritten() const;
    bool spoolTruncated() const;

    /// Clear the spool buffer. Called by the UI "Clear" button and after
    /// a successful "Save spool as…" if the user asks to start fresh.
    void clearSpool();

private:
    int slot_;
    std::array<uint8_t, 256> rom_{};
    /// Set by buildRom() when a hand-assembled region overran its budget.
    bool                     romLayoutError_ = false;

    mutable std::mutex bufferMtx_;
    std::deque<uint8_t> spool_;
    size_t spoolBase_ = 0;   // absolute index of spool_.front()
    size_t spoolTotal_ = 0;  // accepted bytes since clear

    void buildRom();
};

#endif // POM2_PRINTER_CARD_H
