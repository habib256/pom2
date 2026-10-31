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

// CCS 7710 serial card — see Ccs7710Card.h.

#include "Ccs7710Card.h"

#include "SlotRomAsm.h"

#include <algorithm>
#include <cstring>

namespace {
// Snapshot blob: "C771", control, rdr, flags (b0 rdrf, b1 overrun), queue
// length (2), queue bytes. The spool is printed output, not state.
constexpr char kMagic[4] = { 'C', '7', '7', '1' };
constexpr uint8_t kStatusRdrf = 0x01, kStatusTdre = 0x02, kStatusOvrn = 0x20, kStatusIrq = 0x80;
} // namespace

Ccs7710Card::Ccs7710Card(int slot) : slot_(slot)
{
    buildOwnFirmware();
}

// POM2's page, written to behave as CCS's 7710-03 does (disassembled from
// the PROMs; the differential test runs both). The first nine bytes are the
// serial-card ID, kept byte for byte because they ARE the interface:
// $Cn00 BIT $FF58 sets V (an RTS there) and branches to the common entry —
// the first call after PR#n / IN#n; $Cn05 CLC (BCS never taken) is the
// output entry; $Cn07 is the operand $38, a SEC — the input entry; both
// fall through CLV. $Cn05/$Cn07 = $18/$38 is what Pascal 1.1's scan and the
// SoftCard II loader read.
//
// The first call (V set) master-resets the 6850 ($A3), keeps its mode $51
// (÷16, 8 bits, 2 stop) in the slot's screen hole $07F8+n, sets the echo
// flag byte $06F8+n, and moves the hook it came through to $Cn05 (CSW) or
// $Cn07 (KSW). Every call writes the mode with RTS on, then: output waits
// for TDRE, sends the byte as it is (8 bits), and echoes it on the screen
// through COUT1 ($FDF0) unless bit 7 of the flag is set; input waits for
// RDRF, sets bit 7, drops line feeds. A, X and Y come back as they went in
// (A = the character). Not modelled: the PROM's terminal mode — Ctrl-A
// commands typed at the keyboard while input waits, Ctrl-R / Ctrl-T from the
// line — and its flashing cursor.
void Ccs7710Card::buildOwnFirmware()
{
    rom_.fill(0xFF);
    const uint8_t n = uint8_t(slot_), s = uint8_t(slot_ << 4);
    const uint8_t mode = uint8_t(0xF8 + n), flags = uint8_t(0xF8 + n);   // $07F8+n, $06F8+n
    pom2::SlotRomAsm a(rom_, slot_, "CCS 7710 (POM2)");
    a.region("id", 0x00, 0x09)
     .emit({ 0x2C, 0x58, 0xFF })          // BIT $FF58: V = 1
     .branch(0x70, "common")              // BVS common
     .emit({ 0x18, 0xB0, 0x38, 0xB8 });   // CLC / BCS (never) / [SEC] / CLV
    a.region("common", 0x09, 0xC0)
     .emit({ 0x8D, 0x78, 0x05 })          // STA $0578: the character
     .emit({ 0x98, 0x48, 0x8A, 0x48 })    // TYA PHA TXA PHA
     .emit({ 0x08, 0xA9, a.pageHi(), 0x8D, 0xF8, 0x07 })   // PHP / LDA #$Cn / STA MSLOT
     .emit({ 0xA2, s, 0x28 })             // LDX #$n0 / PLP
     .branch(0x50, "run")                 // BVC run
     // First call: reset the 6850, keep its mode, echo on, move the hook.
     .emit({ 0xA9, 0x20, 0x8D, flags, 0x06 })        // LDA #$20 / STA $06F8+n
     .emit({ 0xA9, 0xA3, 0x9D, 0x80, 0xC0 })         // LDA #$A3 / STA $C080,X
     .emit({ 0x4A, 0x8D, mode, 0x07 })               // LSR ($51) / STA $07F8+n
     .emit({ 0xA5, 0x36 }).branch(0xD0, "ksw")       // LDA CSWL / BNE ksw
     .emit({ 0xA5, 0x37, 0xC9, a.pageHi() }).branch(0xD0, "ksw")   // CSWH = $Cn?
     .emit({ 0xA9, 0x05, 0x85, 0x36, 0x18 })         // CSWL = $05 / CLC (output)
     .branch(0x90, "run")
     .label("ksw")
     .emit({ 0xA9, 0x07, 0x85, 0x38, 0x38 })         // KSWL = $07 / SEC (input)
     .label("run")
     .emit({ 0xAD, mode, 0x07, 0x29, 0xBF, 0x9D, 0x80, 0xC0 })   // mode, RTS on
     .branch(0xB0, "input")
     .label("txwait")
     .emit({ 0xBD, 0x80, 0xC0, 0x29, 0x02 })         // LDA $C080,X / AND #TDRE
     .branch(0xF0, "txwait")
     .emit({ 0xAD, 0x78, 0x05, 0x9D, 0x81, 0xC0 })   // LDA char / STA $C081,X
     .emit({ 0xAD, flags, 0x06 }).branch(0x30, "done")   // flag bit 7: no echo
     .emit({ 0xAD, 0x78, 0x05, 0x20, 0xF0, 0xFD })   // LDA char / JSR COUT1
     .jmp("done")                                    // COUT1 may move any flag
     .label("input")
     .emit({ 0xBD, 0x80, 0xC0, 0x4A })               // LDA $C080,X / LSR: RDRF -> C
     .branch(0x90, "input")
     .emit({ 0xBD, 0x81, 0xC0, 0x09, 0x80, 0xC9, 0x8A })   // data | $80 / CMP LF
     .branch(0xF0, "input")
     .emit({ 0x8D, 0x78, 0x05 })                     // STA $0578
     .label("done")
     .emit({ 0x68, 0xAA, 0x68, 0xA8, 0xAD, 0x78, 0x05, 0x60 });   // PLA TAX PLA TAY / LDA / RTS
    romLayoutError_ = !a.finish();
}

void Ccs7710Card::useOwnFirmware()
{
    promsLoaded_ = false;
    buildOwnFirmware();
}

bool Ccs7710Card::loadProms(const std::vector<uint8_t>& hi, const std::vector<uint8_t>& lo)
{
    if (hi.size() != 256 || lo.size() != 256) return false;
    // MAME ROM_NIBBLE | ROM_SHIFT_NIBBLE_HI / _LO: each PROM holds one
    // nibble of every byte, in its low four bits.
    for (std::size_t i = 0; i < 256; ++i)
        rom_[i] = uint8_t((hi[i] & 0x0F) << 4 | (lo[i] & 0x0F));
    promsLoaded_ = true;
    return true;
}

uint8_t Ccs7710Card::status() const
{
    uint8_t st = kStatusTdre;            // /DCD and /CTS low: wired
    if (rdrf_) st |= kStatusRdrf;
    if (overrun_) st |= kStatusOvrn;
    const bool rie = (control_ & 0x80) != 0;
    const bool tie = (control_ & 0x60) == 0x20;
    if ((control_ & 0x03) != 0x03 && ((rie && rdrf_) || tie)) st |= kStatusIrq;
    return st;
}

void Ccs7710Card::updateIrq()
{
    assertIrq((status() & kStatusIrq) != 0);
}

void Ccs7710Card::masterReset()
{
    rdrf_ = false;
    overrun_ = false;
    rdr_ = 0;
}

void Ccs7710Card::receive(uint8_t b)
{
    rx_.push_back(b);
    if (!rdrf_ && (control_ & 0x03) != 0x03) {
        rdr_ = rx_.front();
        rx_.pop_front();
        rdrf_ = true;
    }
    updateIrq();
}

uint8_t Ccs7710Card::deviceSelectRead(uint8_t low4)
{
    if (!(low4 & 1)) return status();
    const uint8_t v = rdr_;
    rdrf_ = false;
    overrun_ = false;
    if (!rx_.empty() && (control_ & 0x03) != 0x03) {   // the next byte arrives
        rdr_ = rx_.front();
        rx_.pop_front();
        rdrf_ = true;
    }
    updateIrq();
    return v;
}

void Ccs7710Card::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    if (low4 & 1) {
        if ((control_ & 0x03) != 0x03) spool_.push_back(v);
        updateIrq();
        return;
    }
    control_ = v;
    if ((v & 0x03) == 0x03) masterReset();
    else if (!rdrf_ && !rx_.empty()) {
        rdr_ = rx_.front();
        rx_.pop_front();
        rdrf_ = true;
    }
    updateIrq();
}

std::size_t Ccs7710Card::drainSpoolFrom(std::size_t from, std::vector<uint8_t>& out) const
{
    if (from < spool_.size()) out.insert(out.end(), spool_.begin() + std::ptrdiff_t(from), spool_.end());
    return spool_.size();
}

// The ACIA has no reset pin: Ctrl-Reset leaves it alone. Power-on holds it
// in master reset.
void Ccs7710Card::onPowerCycle()
{
    control_ = 0x03;
    masterReset();
    rx_.clear();
    updateIrq();
}

void Ccs7710Card::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), kMagic, kMagic + 4);
    out.push_back(control_);
    out.push_back(rdr_);
    out.push_back(uint8_t((rdrf_ ? 1 : 0) | (overrun_ ? 2 : 0)));
    const std::size_t n = std::min<std::size_t>(rx_.size(), 0xFFFF);
    out.push_back(uint8_t(n));
    out.push_back(uint8_t(n >> 8));
    for (std::size_t i = 0; i < n; ++i) out.push_back(rx_[i]);
}

void Ccs7710Card::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (!data || len < 9 || std::memcmp(data, kMagic, 4) != 0) return;
    const std::size_t n = std::size_t(data[7] | data[8] << 8);
    if (len < 9 + n) return;          // trailing bytes are not ours: ignored
    control_ = data[4];
    rdr_ = data[5];
    rdrf_ = (data[6] & 1) != 0;
    overrun_ = (data[6] & 2) != 0;
    rx_.assign(data + 9, data + 9 + n);
    updateIrq();
}
