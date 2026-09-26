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

// GrapplerCard — see header for ROM layout + protocol notes.

#include "GrapplerCard.h"
#include "SlotRomAsm.h"

#include "Logger.h"

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <iterator>

GrapplerCard::GrapplerCard(int slot) : slot_(slot)
{
    buildStubRom();
}

bool GrapplerCard::loadRom(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        pom2::log().warn("Grappler", "Cannot open Grappler+ ROM: " + path);
        return false;
    }
    std::vector<uint8_t> bytes(kRomBytes + 1);
    f.read(reinterpret_cast<char*>(bytes.data()),
           static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<size_t>(f.gcount()));
    if (bytes.size() != kRomBytes) {
        pom2::log().warn("Grappler",
            "Grappler+ ROM " + path + " has unexpected size " +
            std::to_string(bytes.size()) +
            " B (expected 4096) — using stub ROM");
        return false;
    }
    std::copy(bytes.begin(), bytes.end(), rom_.begin());
    romLoaded_ = true;
    romSource_ = path;
    pom2::log().info("Grappler", "Loaded Grappler+ ROM: " + path);
    return true;
}

const char* GrapplerCard::printerTypeName(PrinterType t)
{
    switch (t) {
        case PrinterType::Epson:          return "Epson series";
        case PrinterType::CItoh8510:      return "NEC 8023 / C. Itoh 8510 / DMP 85";
        case PrinterType::StarGemini:     return "Star Gemini";
        case PrinterType::Anadex:         return "Anadex";
        case PrinterType::Okidata:        return "Okidata 82A/83A/92/93/84";
        case PrinterType::AppleDotMatrix: return "Apple Dot Matrix / ImageWriter";
        case PrinterType::Okidata84:      return "Okidata 84 (no Step II graphics)";
        default:                          return "(invalid)";
    }
}

uint8_t GrapplerCard::deviceSelectRead(uint8_t /*low4*/)
{
    // MAME `a2bus_grapplerplus_device::read_c0nx` (grappler.cpp:699-709):
    // every device-select offset returns the same status byte —
    //   bit 7 IRQ pending · bits 6-4 DIP switches · bit 3 BUSY ·
    //   bit 2 PE (paper empty) · bit 1 SELECT · bit 0 ACK latch.
    // POM2's printer is on-line and never runs out of paper (PE=0,
    // SELECT=1); bits 6-4 report the S1 printer-type switches, which
    // default to 101 = Apple Dot Matrix, not MAME's 000 = Epson (see
    // `PrinterType` — POM2's printer IS an ImageWriter). The live lines
    // are BUSY and the ACK latch, both driven by the host-side printer
    // through
    // `setPrinterBusy`: a printer whose input buffer is full has not
    // acknowledged the byte yet, so the latch reads back clear.
    //
    // That bit — not BUSY — is what the genuine firmware's output
    // routine spins on (4 KB dump, low $C800 bank):
    //     $CD89  JSR $CDE1      ; read $C08n status
    //     $CD8C  AND #$02       ; SELECT? no → give up
    //     $CD93  AND #$01       ; ACK latch
    //     $CD95  BEQ $CD89      ; spin until the printer acknowledges
    // so holding it clear is what makes a printing guest wait for the
    // paper instead of blasting a page into a host queue.
    //
    // The previous $FF read decoded for real firmware as "busy AND out
    // of paper" — the worst possible value for that poll.
    const bool acked = ackEffective();
    if (acked != irqAsserted_ && !irqDisable_) updateIrq();
    return static_cast<uint8_t>((irqAsserted_ ? 0x80 : 0x00) |
                                ((static_cast<uint8_t>(printerType()) & 0x07)
                                     << 4) |
                                lineBits() |
                                (acked ? 0x01 : 0x00));
}

uint8_t GrapplerCard::lineBits() const
{
    // No printer: MAME's empty centronics port pulls every input up
    // (ctronics.cpp:56-73).
    if (!printerConnected()) return 0x08 | 0x04 | 0x02;
    const bool out = paperOut();
    return static_cast<uint8_t>(((printerBusy() || out) ? 0x08 : 0x00) |
                                (out ? 0x04 : 0x00) |
                                (online() ? 0x02 : 0x00));
}

void GrapplerCard::spoolByte(uint8_t latched)
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    if (spool_.size() == kMaxSpoolBytes) {
        spool_.pop_front();
        ++spoolBase_;
    }
    spool_.push_back(latched);
    ++spoolTotal_;
}

void GrapplerCard::releasePending()
{
    if (!pendingByte_ || !printerAccepts()) return;
    pendingByte_ = false;
    spoolByte(pendingValue_);
    ackLatch_ = true;     // the /ACK pulse the byte was waiting for
    updateIrq();
}

void GrapplerCard::setOnline(bool online)
{
    online_.store(online, std::memory_order_relaxed);
    releasePending();
}

void GrapplerCard::setPaperOut(bool out)
{
    paperOut_.store(out, std::memory_order_relaxed);
    releasePending();
}

void GrapplerCard::setPrinterConnected(bool connected)
{
    connected_.store(connected, std::memory_order_relaxed);
    releasePending();
}

void GrapplerCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    // MAME base `write_c0nx` (grappler.cpp:547-575) + Grappler+ overlay
    // (grappler.cpp:711-745):
    //   !(offset & 3)  → latch data + strobe (offsets $0/$4/$8/$C)
    //   A0 set         → select the high 2 KB ROM bank at $C800
    //   A1 set         → disable the ACK IRQ (and release a pending one)
    //   A2 set (¬A1)   → enable the ACK IRQ
    // Not modelled: the 7-clock /STROBE pulse timer (grappler.cpp:795-808,
    // 839-849) — the synthetic printer consumes the byte at latch time, so
    // the strobe edge has no observer; and MAME's ack-input gate on
    // clearing the latch (grappler.cpp:559-570) — with an instant printer
    // /ACK is never held asserted across a data write.
    // The previous decode spooled offset 1 ONLY — on real hardware that's
    // the bank select, so the genuine firmware's `STA $C0n0` printed into
    // the void. The synthetic printer consumes the byte and ACKs
    // instantly, so the ACK latch is re-set on the spot.
    if (!(low4 & 0x03)) {
        {
            // MAME `data_latched` (grappler.cpp:795-808): S1:1 open drops
            // bit 7 at the latch.
            const uint8_t latched = dipMsb_ ? v : static_cast<uint8_t>(v & 0x7F);
            if (printerAccepts()) {
                spoolByte(latched);
            } else {
                // Offline, out of paper or no printer: no /ACK. MAME's data
                // write clears the latch (grappler.cpp:559-570); the byte
                // waits for the printer (setOnline).
                pendingByte_ = true;
                pendingValue_ = latched;
                ackLatch_ = false;
                updateIrq();
            }
        }
        // The printer acknowledges as soon as it has room for the byte —
        // instantly unless the host-side ImageWriter reported a full
        // input buffer (`ackEffective`), in which case the firmware's
        // poll loop spins until it drains.
        if (!pendingByte_) {
            ackLatch_ = true;
            updateIrq();
        }
    }
    if (low4 & 0x01) romBankHigh_ = true;
    if (low4 & 0x02) {
        irqDisable_ = true;
        updateIrq();
    } else if (low4 & 0x04) {
        irqDisable_ = false;
        updateIrq();
    }
}

uint8_t GrapplerCard::slotRomRead(uint8_t low8)
{
    if (romLoaded_) {
        // MAME `read_cnxx` (grappler.cpp:578-583) side effects: any $CnXX
        // read resets the ROM bank to 0, and while the ACK latch is CLEAR,
        // address bit 6 is
        // forced low for $Cn80-$CnFF fetches — the firmware senses ACK
        // through ROM reads (`m_rom[(!ack && BIT(offset,7)) ? offset&0xBF
        // : offset]`). With the instant-ACK printer the latch is almost
        // always set, but the wiring is kept faithful.
        romBankHigh_ = false;
        const uint8_t idx = (!ackEffective() && (low8 & 0x80))
                                ? static_cast<uint8_t>(low8 & 0xBF)
                                : low8;
        return rom_[idx];
    }
    return stubRom_[low8];
}

uint8_t GrapplerCard::expansionRomRead(uint16_t offset)
{
    // The 4 KB Grappler EPROM appears in the shared $C800-$CFFF window
    // 2 KB at a time; A0 device-select writes choose the high bank and
    // any $CnXX read drops back to the low one (MAME `read_c800`
    // grappler.cpp:123-126 / `set_rom_bank` grappler.cpp:160-165). Stock
    // PR# / status entry points live in the low
    // bank; the graphics-dump code spills into the high one.
    if (!romLoaded_) return 0xFF;
    return rom_[(offset & 0x7FF) | (romBankHigh_ ? 0x800u : 0x000u)];
}

void GrapplerCard::onReset()
{
    // MAME `a2bus_grapplerplus_device_base::reset_from_bus`
    // (grappler.cpp:536-539) sets only the ACK latch back to its inactive
    // (set) level; `a2bus_grapplerplus_device::device_reset`
    // (grappler.cpp:777-787) disables the IRQ flip-flop and releases the
    // line. Neither touches the ROM bank — the U2D bank flip-flop is not
    // wired to bus RESET, so a reset mid-graphics-dump leaves the high
    // $C800 bank selected until the next $CnXX fetch drops it (an earlier
    // POM2 revision cleared it here, a silent divergence).
    ackLatch_    = true;
    irqDisable_  = true;
    pendingByte_ = false;   // a reset abandons the strobe no printer took
    updateIrq();
}

void GrapplerCard::slotRomWrite(uint8_t /*low8*/, uint8_t /*v*/)
{
    // MAME `a2bus_grapplerplus_device_base::write_cnxx`
    // (grappler.cpp:586-591): a write into the $CnXX ROM window is a bus
    // conflict on real hardware, but the address decode still clocks the
    // bank flip-flop back to the low 2 KB, same as a read.
    romBankHigh_ = false;
}

void GrapplerCard::updateIrq()
{
    // MAME models the IRQ as a flip-flop moved on each edge —
    // `ack_latch_set` raises it when enabled (grappler.cpp:811-819),
    // `ack_latch_cleared` (a data write) lowers it (grappler.cpp:822-831),
    // A1/A2 writes lower/raise it (grappler.cpp:715-745), reset lowers it
    // (grappler.cpp:777-787). The invariant those transitions maintain is
    // `m_irq == (ack_latch && !irq_disable)`, so POM2 derives the level
    // directly; the observable status bit 7 and slot line are identical.
    // A byte still sitting in a full printer buffer hasn't been acked, so
    // it can't raise the interrupt either (`ackEffective`).
    const bool want = ackEffective() && !irqDisable_;
    if (want != irqAsserted_) {
        irqAsserted_ = want;
        assertIrq(want);
    }
}

std::vector<uint8_t> GrapplerCard::spoolBytes() const
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    return {spool_.begin(), spool_.end()};
}

std::string GrapplerCard::spoolText() const
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    std::string out;
    out.reserve(spool_.size());
    for (uint8_t b : spool_) {
        const uint8_t c = b & 0x7F;
        if (c == 0x0D) out.push_back('\n');
        else if (c == 0x00) continue;
        else                out.push_back(static_cast<char>(c));
    }
    return out;
}

size_t GrapplerCard::drainSpoolFrom(size_t from, std::vector<uint8_t>& out) const
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    // Absolute cursors survive front eviction; a cursor beyond the new total
    // denotes clearSpool() and resynchronises from the retained front.
    const size_t absolute = (from > spoolTotal_) ? spoolBase_
                                                 : std::max(from, spoolBase_);
    const size_t start = absolute - spoolBase_;
    out.insert(out.end(), spool_.begin() + static_cast<std::ptrdiff_t>(start),
               spool_.end());
    return spoolTotal_;
}

size_t GrapplerCard::bytesWritten() const
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    return spoolTotal_;
}

void GrapplerCard::clearSpool()
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    spool_.clear();
    spoolBase_ = 0;
    spoolTotal_ = 0;
}

void GrapplerCard::buildStubRom()
{
    // Fallback ROM used until the user drops a Grappler+ dump in roms/.
    // Mirrors PrinterCard's PR#n trampoline so DOS/BASIC `PR#1` still
    // works; software that fingerprints the Grappler ROM sees an empty
    // card (matches "no Grappler installed" detection paths). Padding
    // is $EA (NOP) so a sloppy probe walking through doesn't hit garbage.
    stubRom_.fill(0xEA);
    // Data port = $C0(8+s)0 — the MAME-faithful data latch offset
    // (!(offset & 3)). The stub used to print through offset 1, which on
    // the real card (and now in deviceSelectWrite) is the ROM bank select.
    const uint8_t dataLo = static_cast<uint8_t>(0x80 + slot_ * 16);

    pom2::SlotRomAsm a(stubRom_, slot_, "GrapplerCard stub");

    a.region("entry", 0x00, 0x05).jmp("prn");   // $Cn00: skip the Pascal sig

    // Pascal 1.1 autodetect — same shape as PrinterCard.
    a.region("pascalId", 0x05, 0x0D)
     .poke(0x05, 0x38)      // SEC
     .poke(0x07, 0x18)      // CLC
     .poke(0x0B, 0x01)      // Pascal firmware rev
     .poke(0x0C, 0x00);     // device class = printer

    // PR#n CSWL/CSWH install.
    a.region("prn", 0x20, 0x31)
     .emit({ 0xA9 }).byteOf("cout")
     .emit({ 0x85, 0x36, 0xA9, a.pageHi(), 0x85, 0x37, 0x60 });

    // Output handler: STA $C0(8+s)0 / RTS.
    a.region("cout", 0x31, pom2::kSlotRomBytes)
     .emit({ 0x8D, dataLo, 0xC0, 0x60 });

    romLayoutError_ = !a.finish();
}

void GrapplerCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.push_back('G'); out.push_back('P'); out.push_back(1);
    out.push_back(romBankHigh_ ? 1 : 0);
    out.push_back(ackLatch_    ? 1 : 0);
    out.push_back(irqDisable_  ? 1 : 0);
}

void GrapplerCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (len < 6 || data[0] != 'G' || data[1] != 'P' || data[2] != 1) return;
    romBankHigh_ = data[3] != 0;
    ackLatch_    = data[4] != 0;
    irqDisable_  = data[5] != 0;
    updateIrq();   // re-derive the slot IRQ line from the restored state
}
