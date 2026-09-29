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

// PrinterCard — see header for ROM layout + protocol notes.

#include "PrinterCard.h"
#include "PascalPrinterRom.h"
#include "SlotRomAsm.h"

#include <algorithm>
#include <initializer_list>

PrinterCard::PrinterCard(int slot) : slot_(slot)
{
    buildRom();
}

uint8_t PrinterCard::deviceSelectRead(uint8_t /*low4*/)
{
    // Synthetic always-ready printer. Real Apple Parallel Cards used a
    // BUSY/READY bit, but a host-side spool never blocks.
    return 0xFF;
}

void PrinterCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    // Data on $C0(8+s)0 OR $C0(8+s)1; every other offset is strobe/status
    // and carries no payload.
    //
    // Offset 1 is what this card's own slot-ROM trampoline writes, and for a
    // long time it was the ONLY offset decoded. That is enough for `PR#n` +
    // COUT — which is how BASIC, DOS and the Monitor print — and silently
    // wrong for every program that drives the interface DIRECTLY, which is
    // what graphics software does. The Print Shop's "Apple Parallel
    // Interface" driver never touches our ROM at all: it writes the
    // character to the card's data latch at offset 0, pulses the strobe by
    // writing the same byte to offset 2, and polls offset 4 for ready. So a
    // whole `ESC G` page — 702 bytes, traced — went into the void while
    // offset 4 kept answering 0xFF, and Print Shop reported the job printed.
    // A printer that reports success and prints nothing is the worst of the
    // available behaviours.
    //
    // Offset 0 is the data latch on the real Apple Parallel Printer
    // Interface this card is modelled on, so accepting it costs nothing and
    // makes the direct-drive path work. Offset 2 is deliberately NOT taken:
    // the same byte appears there as the strobe, and spooling both would
    // double every character.
    if (low4 != 0 && low4 != 1) return;
    std::lock_guard<std::mutex> lk(bufferMtx_);
    if (spool_.size() == kMaxSpoolBytes) {
        spool_.pop_front();
        ++spoolBase_;
    }
    spool_.push_back(v);
    ++spoolTotal_;
}

std::vector<uint8_t> PrinterCard::spoolBytes() const
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    return {spool_.begin(), spool_.end()};
}

std::string PrinterCard::spoolText() const
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    std::string out;
    out.reserve(spool_.size());
    for (uint8_t b : spool_) {
        const uint8_t c = b & 0x7F;        // Apple II output sets bit 7
        if (c == 0x0D) out.push_back('\n'); // CR → LF for host display
        else if (c == 0x00) continue;       // drop NULs (paddle / strobe noise)
        else                out.push_back(static_cast<char>(c));
    }
    return out;
}

size_t PrinterCard::drainSpoolFrom(size_t from, std::vector<uint8_t>& out) const
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    // Indices are absolute so evicting old preview bytes cannot make a live
    // ImageWriter replay the retained tail. A clear resets total to zero;
    // an older cursor then resynchronises from the new front.
    const size_t absolute = (from > spoolTotal_) ? spoolBase_
                                                 : std::max(from, spoolBase_);
    const size_t start = absolute - spoolBase_;
    out.insert(out.end(), spool_.begin() + static_cast<std::ptrdiff_t>(start),
               spool_.end());
    return spoolTotal_;
}

size_t PrinterCard::bytesWritten() const
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    return spoolTotal_;
}

bool PrinterCard::spoolTruncated() const
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    return spoolBase_ != 0;
}

void PrinterCard::clearSpool()
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    spool_.clear();
    spoolBase_ = 0;
    spoolTotal_ = 0;
}

void PrinterCard::buildRom()
{
    rom_.fill(0xEA);     // NOP padding — see header for layout

    const uint8_t dataLo = static_cast<uint8_t>(0x80 + slot_ * 16 + 1);

    pom2::SlotRomAsm a(rom_, slot_, "PrinterCard");

    // PR#n entry at $Cn00 — jump over the Pascal signature region.
    a.region("entry", 0x00, 0x05).jmp("prn");

    // Pascal 1.1 autodetect signature — ProDOS scans for these to publish
    // the card in its device list, and a Pascal 1.1 caller that trusts it
    // dispatches through the entry table at $Cn0D-$Cn10, so the table and
    // its four routines are real (PascalPrinterRom.h). These addresses are
    // mandated, hence poke() inside a declared region rather than emit().
    a.region("pascalId", 0x05, 0x0D)
     .poke(0x05, 0x38)      // SEC
     .poke(0x07, 0x18)      // CLC
     .poke(0x0B, 0x01)      // Pascal firmware revision
     .poke(0x0C, pom2::kPascalPrinterClass);   // class 1 = printer
    pom2::assemblePascalPrinterEntries(a, dataLo, 0x40, 0x60);

    // PR#n trampoline — hook CSWL/CSWH ($36/$37) to point at the output
    // handler. The next COUT call lands there instead of the standard
    // screen routine. The handler's address is a LABEL: it used to be the
    // literal $31 written twice, here and in the region below it.
    a.region("prn", 0x20, 0x31)
     .emit({ 0xA9 }).byteOf("cout")   // LDA #<cout
     .emit({ 0x85, 0x36,              // STA CSWL
             0xA9, a.pageHi(),        // LDA #>cout
             0x85, 0x37,              // STA CSWH
             0x60 });                 // RTS

    // Output handler — write A to the data port. A is preserved (STA does
    // not modify it), satisfying COUT's "A unchanged on exit" convention.
    // The CPU's existing flags/X/Y are untouched.
    a.region("cout", 0x31, 0x40)
     .emit({ 0x8D, dataLo, 0xC0,      // STA $C0(8+s)1
             0x60 });                 // RTS

    romLayoutError_ = !a.finish();
}
