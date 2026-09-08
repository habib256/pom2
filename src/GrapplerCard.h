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

// GrapplerCard — Orange Micro Grappler+ parallel printer interface.
//
// What it adds over the synthetic `PrinterCard`
// ---------------------------------------------
// * A real 4 KB ROM dump (markadev/AppleII-RevEng/Orange-Micro-Grappler+).
//   The first 256 B sit at $CnXX (slot ROM); the full 4 KB is mirrored
//   into the shared expansion-ROM window at $C800-$CFFF so Grappler-aware
//   software detects the card via its ROM fingerprint.
// * MAME-faithful $C0(8+s)X register decode (`a2bus_grapplerplus_device`,
//   MAME `bus/a2bus/grappler.cpp` — pinned line ranges cited at each
//   ported block in the .cpp; smoke test `grappler_card_smoke`): data
//   latch + strobe on offsets with
//   `!(offset & 3)` ($0/$4/$8/$C — spooled to the host buffer), A0 selects
//   the high 2 KB ROM bank at $C800, A1/A2 disable/enable the ACK IRQ.
//   Status reads return IRQ|DIP|BUSY|PE|SELECT|ACK — the host printer is
//   always on-line (PE=0, SELECT=1) and ACKs instantly *unless* its input
//   buffer is full, which `setPrinterBusy` reports back (the firmware
//   spins on the ACK bit, so that is what throttles a printing guest to
//   the mechanism's real speed). Bytes the
//   Grappler firmware emits for its graphic-dump commands (^I G / ^I H)
//   are captured verbatim into the spool exactly as a real Epson-class
//   printer would have seen them. (An earlier revision spooled offset 1
//   only — on real hardware that's the BANK SELECT, so the genuine
//   firmware's `STA $C0n0` output was silently dropped and its status
//   polls read $FF = busy + paper out.)
// * Catalog key `"grappler"`. Defaults to slot 1 (the DOS / ProDOS
//   printer-scan slot).
//
// ROM-gated
// ---------
// Requires `roms/grappler_plus.bin` (4 KB). Without the dump the card
// still plugs but its slot ROM is a tiny stub — only the PR#n
// trampoline + the always-ready data port work; software that fingerprints
// the Grappler ROM (e.g. AppleWorks's "Printer = Grappler+") sees a
// blank card. This mirrors the CFFA's ROM-gated approach.

#ifndef POM2_GRAPPLER_CARD_H
#define POM2_GRAPPLER_CARD_H

#include "SlotPeripheral.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class GrapplerCard : public SlotPeripheral
{
public:
    static constexpr int    kDefaultSlot = 1;
    static constexpr size_t kRomBytes    = 0x1000;   // 4 KB EPROM
    static constexpr size_t kMaxSpoolBytes = 4u * 1024u * 1024u;

    explicit GrapplerCard(int slot = kDefaultSlot);

    int getSlot() const { return slot_; }

    /// True when the hand-assembled FALLBACK stub did not fit its layout.
    /// A real Grappler+ dump is copied verbatim and cannot trip this.
    bool romLayoutError() const { return romLayoutError_; }

    // ─── ROM loading ─────────────────────────────────────────────────────
    /// Load the 4 KB Grappler+ ROM dump. Must be exactly 4096 bytes.
    /// Without it the card falls back to a minimal synthetic slot ROM
    /// that supports PR#n but lacks the graphics dump entry points.
    bool loadRom(const std::string& path);
    bool isRomLoaded() const { return romLoaded_; }
    const std::string& romSource() const { return romSource_; }

    // ─── SlotPeripheral overrides ────────────────────────────────────────
    std::string_view name() const override { return "Grappler+ (Orange Micro)"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead     (uint8_t low8) override;
    /// $CnXX writes are a bus conflict on the real card but still reset
    /// the ROM bank (MAME `write_cnxx`, grappler.cpp:586-591).
    void    slotRomWrite    (uint8_t low8, uint8_t v) override;
    uint8_t expansionRomRead(uint16_t offset) override;
    /// Only a card that can actually SERVE the shared /IOSTB window may
    /// latch it (MAME `a2bus.h:145` take_c800 + `apple2e.cpp:2977`). MAME's
    /// Grappler+ (`grappler.cpp:64`) always has its ROM region, so `true`
    /// is unconditional there; POM2 supports a ROM-LESS Grappler
    /// (SlotCardFactory's documented "PR#n still works" fallback) whose
    /// `expansionRomRead` answers $FF for the whole of $C800-$CFFF, and
    /// claiming first-one-wins with nothing to serve starved the card that
    /// does have an expansion ROM (bug hunt #9).
    bool takesC800() const override { return romLoaded_; }
    void    onReset() override;

    /// Rewind/snapshot hooks — the ROM bank / ACK latch / IRQ-enable
    /// flip-flops are guest-visible state (a rewound guest mid-print may
    /// expect the other $C800 bank or a pending ACK IRQ). The host-side
    /// spool is deliberately NOT rewound (it is an output log, like the
    /// PrinterCard's).
    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

    // ─── Spool access (shared shape with PrinterCard for UI reuse) ───────
    std::vector<uint8_t> spoolBytes() const;
    std::string          spoolText()  const;
    size_t               bytesWritten() const;
    void                 clearSpool();
    /// Streaming variant used by the host-side ImageWriter — see
    /// `PrinterCard::drainSpoolFrom`.
    size_t drainSpoolFrom(size_t from, std::vector<uint8_t>& out) const;

    // ─── S1 DIP block (MAME INPUT_PORTS_START(grapplerplus),
    //      grappler.cpp:498-511) ─────────────────────────────────────────
    // Bits 2-0 = printer type, read back at status bits 6-4; the firmware
    // branches on it to pick which dialect of control codes it emits for
    // its graphics dump and its style commands. Bit 3 = "Most Significant
    // Bit": Software Control (pass all 8 bits) or Not Transmitted (mask to
    // 7 bits at the latch, MAME `data_latched`).
    //
    // MAME defaults to 0 (Epson) because it wires the card to a generic
    // centronics printer. POM2's printer is an Apple ImageWriter II, so
    // the factory default here is `AppleDotMatrix` — an Epson-configured
    // Grappler sends Epson escape codes (`ESC K n1 n2` + binary graphics,
    // `ESC W 1` for double width) that an ImageWriter parses as colour
    // selection and stray characters. Same wrong-DIP garbage you'd get on
    // a real desk.
    enum class PrinterType : uint8_t {
        Epson = 0, CItoh8510 = 1, StarGemini = 2, Anadex = 3,
        Okidata = 4, AppleDotMatrix = 5, Okidata84 = 6, Invalid = 7,
    };
    static const char* printerTypeName(PrinterType t);

    /// The panel lets the user flip these switches while the guest runs,
    /// so `dipType_` crosses threads exactly like `busy_` below: written
    /// by the UI thread (ImageWriter panel → `onCardDipChanged`), read by
    /// the CPU thread in `deviceSelectRead`. Atomic for the same reason.
    void        setPrinterType(PrinterType t)
    {
        dipType_.store(t, std::memory_order_relaxed);
    }
    PrinterType printerType() const
    {
        return dipType_.load(std::memory_order_relaxed);
    }

    /// S1:1 — off means the latch drops bit 7 of every byte. Unlike the
    /// printer type this one is only set at plug time, before the card
    /// reaches the bus, so it needs no synchronisation.
    void setMsbSoftwareControl(bool on) { dipMsb_ = on; }
    bool msbSoftwareControl() const { return dipMsb_; }

    // ─── Printer BUSY input (connector pin 11) ───────────────────────────
    /// MAME reads BUSY live off the centronics device
    /// (`a2bus_grapplerplus_device::read_c0nx` → `busy_in()`); POM2's
    /// printer is host-side, so the UI pushes the line whenever the
    /// ImageWriter's input buffer fills. The Grappler firmware polls the
    /// status port before every byte, so this is what makes a guest that
    /// prints a long job actually wait for the paper to move — exactly
    /// like a real Apple II wired to a real printer.
    /// Written by the UI thread, read by the CPU thread.
    ///
    /// MAME raises the ACK IRQ on the printer's asynchronous /ACK edge
    /// (`ack_latch_set`, grappler.cpp:811-819); POM2's equivalent edge is
    /// BUSY clearing while a byte's ACK is latched, so the line is
    /// re-derived on every transition — an IRQ-mode driver parked in WAI
    /// must wake when the printer drains, not only on its next status
    /// read. The caller (PrinterCoordinator::setGrapplerBusy) holds the
    /// machine lock, which is what updateIrq's slot-IRQ wiring expects.
    void setPrinterBusy(bool busy)
    {
        const bool prev = busy_.exchange(busy, std::memory_order_relaxed);
        if (prev != busy) updateIrq();
    }
    bool printerBusy() const { return busy_.load(std::memory_order_relaxed); }

    /// The ACK latch as the firmware sees it. `ackLatch_` is the MAME
    /// flip-flop (cleared by a data write, set by the printer's /ACK
    /// pulse); a printer with no room in its buffer simply hasn't pulsed
    /// yet, so a busy printer reads back as "not acknowledged".
    /// CPU-thread side of `setPrinterBusy`.
    bool ackEffective() const { return ackLatch_ && !printerBusy(); }

private:
    int slot_;
    std::array<uint8_t, kRomBytes> rom_{};
    std::array<uint8_t, 256>       stubRom_{};      // used until loadRom
    /// Set by buildStubRom() when a region overran its budget. Only the STUB
    /// is hand-assembled; a real roms/ dump is copied verbatim.
    bool                           romLayoutError_ = false;
    bool romLoaded_ = false;
    std::string romSource_;

    mutable std::mutex   bufferMtx_;
    std::deque<uint8_t> spool_;
    size_t spoolBase_ = 0;
    size_t spoolTotal_ = 0;

    // ─── MAME a2bus_grapplerplus register state (grappler.cpp) ──────────
    bool romBankHigh_ = false;  // A0 write sets; any $CnXX read clears
    bool ackLatch_    = true;   // reset = 1 (MAME m_ack_latch); instant ACK
    bool irqDisable_  = true;   // A1 disables / A2 enables the ACK IRQ
    bool irqAsserted_ = false;
    std::atomic<bool> busy_{false};   // printer BUSY input (see setPrinterBusy)
    std::atomic<PrinterType> dipType_{PrinterType::AppleDotMatrix};  // S1:4,3,2
    bool        dipMsb_  = true;                          // S1:1

    void updateIrq();

    void buildStubRom();
};

#endif // POM2_GRAPPLER_CARD_H
