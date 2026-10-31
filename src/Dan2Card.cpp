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

// DAN ][ Controller — see Dan2Card.h.

#include "Dan2Card.h"

#include "SlotRomAsm.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace {
constexpr uint8_t kMagic = 0xAC;
constexpr uint8_t kOk = 0x00, kIoErr = 0x27, kNoDev = 0x28, kWriteProt = 0x2B;
// Snapshot: "DAN1", port C, flags (b0 in command), the command bytes (count +
// 6), the write in flight (unit + 1, block, count + bytes), the reply queue.
constexpr char kSnapMagic[4] = { 'D', 'A', 'N', '1' };
} // namespace

Dan2Card::Dan2Card(int slot) : slot_(slot), disks_(slot)
{
    buildOwnFirmware();
}

// POM2's page: the Dan ][ protocol as the EPROM speaks it, without its boot
// menu. ID bytes as the EPROM's (CPX #$20 / LDY #$00 / CPX #$03 / LDX #$3C:
// $Cn01/03/05/07 = $20/$00/$03/$3C, ProDOS and the Autostart ROM); boot =
// SAFEREAD of block 0 to $0800 and JMP $0801 with X = $n0, or on to the next
// slot (SLOOP, $FABA) when nothing is there; the ProDOS driver at $CnFF.
void Dan2Card::buildOwnFirmware()
{
    rom_.assign(256, 0x00);
    std::array<uint8_t, 256> page{};
    const uint8_t s = uint8_t(slot_ << 4);
    const uint8_t pA = uint8_t(0x80 + s), pC = uint8_t(0x82 + s), ctl = uint8_t(0x83 + s);
    pom2::SlotRomAsm a(page, slot_, "DAN ][ (POM2)");
    a.region("id", 0x00, 0x08)
     .emit({ 0xE0, 0x20, 0xA0, 0x00, 0xE0, 0x03, 0xA2, 0x3C });
    a.region("boot", 0x08, 0x30)
     .emit({ 0xA9, 0x0A, 0x85, 0x42 })               // SAFEREAD
     .emit({ 0xA9, s, 0x85, 0x43 })                  // unit: drive 1 of this slot
     .emit({ 0xA9, 0x00, 0x85, 0x44, 0x85, 0x46, 0x85, 0x47 })   // buffer $0800, block 0
     .emit({ 0xA9, 0x08, 0x85, 0x45 })
     .jsr("drv")
     .branch(0xB0, "next")                           // BCS: nothing to boot
     .emit({ 0xA2, s }).emit({ 0x4C, 0x01, 0x08 })   // LDX #$n0 / JMP $0801
     .label("next")
     .emit({ 0x4C, 0xBA, 0xFA });                    // SLOOP: the next slot
    a.region("drv", 0x30, 0xFC)
     .emit({ 0xA9, 0xFA, 0x8D, ctl, 0xC0 })          // 8255: mode 2 on port A
     .emit({ 0xA9, kMagic }).jsr("send")
     .emit({ 0xA0, 0x00 })
     .label("cmd")
     .emit({ 0xB9, 0x42, 0x00 }).jsr("send")         // LDA $0042,Y
     .emit({ 0xC8, 0xC0, 0x06 }).branch(0xD0, "cmd")
     .jsr("recv")
     .branch(0xF0, "ok")
     .emit({ 0x38, 0x60 })                           // an error: SEC / RTS
     .label("ok")
     .emit({ 0xA5, 0x42 }).branch(0xF0, "status")
     .emit({ 0xC9, 0x03 }).branch(0xF0, "done")      // FORMAT: nothing follows
     .emit({ 0xC9, 0x02 }).branch(0xF0, "write")
     .emit({ 0xA0, 0x00 })                           // READ / SAFEREAD: 512 bytes in
     .label("rd1").jsr("recv").emit({ 0x91, 0x44, 0xC8 }).branch(0xD0, "rd1")
     .emit({ 0xE6, 0x45 })
     .label("rd2").jsr("recv").emit({ 0x91, 0x44, 0xC8 }).branch(0xD0, "rd2")
     .emit({ 0xC6, 0x45 })
     .label("done")
     .emit({ 0xA9, 0x00, 0x18, 0x60 })               // LDA #0 / CLC / RTS
     .label("status")
     .emit({ 0xA2, 0xFF, 0xA0, 0xFF, 0xA9, 0x00, 0x18, 0x60 })   // $FFFF blocks, as the EPROM
     .label("write")
     .emit({ 0xA0, 0x00 })
     .label("wr1").emit({ 0xB1, 0x44 }).jsr("send").emit({ 0xC8 }).branch(0xD0, "wr1")
     .emit({ 0xE6, 0x45 })
     .label("wr2").emit({ 0xB1, 0x44 }).jsr("send").emit({ 0xC8 }).branch(0xD0, "wr2")
     .emit({ 0xC6, 0x45 })
     .jmp("done")
     .label("send")
     .emit({ 0x8D, pA, 0xC0 })                       // STA port A
     .label("sw").emit({ 0xAD, pC, 0xC0 }).branch(0x10, "sw")    // until /OBF
     .emit({ 0x60 })
     .label("recv")
     .emit({ 0xAD, pC, 0xC0, 0x29, 0x20 }).branch(0xF0, "recv")  // until IBF
     .emit({ 0xAD, pA, 0xC0, 0x60 });
    a.region("tail", 0xFC, 0x100)
     .emit({ 0x00, 0x00, 0xB7 }).byteOf("drv");      // 0 blocks = ask STATUS; $B7 as the EPROM
    romLayoutError_ = !a.finish();
    std::copy(page.begin(), page.end(), rom_.begin());
}

bool Dan2Card::loadEprom(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() != 512) return false;
    rom_ = bytes;
    epromLoaded_ = true;
    return true;
}

void Dan2Card::useOwnFirmware()
{
    epromLoaded_ = false;
    buildOwnFirmware();
}

uint8_t Dan2Card::slotRomRead(uint8_t low8)
{
    const std::size_t page = rom_.size() > 256 ? std::size_t(portC_ & 1) : 0;
    return rom_[page * 256 + low8];
}

bool Dan2Card::peekSlotRom(uint8_t low8, uint8_t& out) const
{
    const std::size_t page = rom_.size() > 256 ? std::size_t(portC_ & 1) : 0;
    out = rom_[page * 256 + low8];
    return true;
}

uint8_t Dan2Card::deviceSelectRead(uint8_t low4)
{
    switch (low4 & 3) {
    case 0: {
        if (out_.empty()) return 0;
        const uint8_t v = out_.front();
        out_.pop_front();
        return v;
    }
    case 2:   // /OBF always high (the ATmega takes every byte at once), IBF
        return uint8_t(0x80 | (out_.empty() ? 0x00 : 0x20));
    default:
        return 0;
    }
}

void Dan2Card::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    switch (low4 & 3) {
    case 0:
        command(v);
        break;
    case 3:
        if (!(v & 0x80)) {                       // port C bit set / reset
            const int bit = (v >> 1) & 7;
            if (v & 1) portC_ = uint8_t(portC_ | (1u << bit));
            else       portC_ = uint8_t(portC_ & ~(1u << bit));
        }                                        // else a mode word ($FA)
        break;
    default:
        break;
    }
}

void Dan2Card::command(uint8_t b)
{
    if (writeUnit_ >= 0) {                       // the 512 bytes of a write
        writeBuf_.push_back(b);
        if (writeBuf_.size() == 512) {
            disks_.backing(writeUnit_).writeBlock(writeBlock_, writeBuf_.data());
            writeUnit_ = -1;
            writeBuf_.clear();
        }
        return;
    }
    if (!inCommand_) {
        if (b == kMagic) { inCommand_ = true; cmd_.clear(); }
        return;                                  // anything else: firmware upload sync, ignored
    }
    cmd_.push_back(b);
    if (cmd_.size() == 1 && b == 0xFF) {         // the EPROM's sync probe: always $27
        reply(kIoErr);
        inCommand_ = false;
        return;
    }
    if (cmd_.size() == 6) {
        inCommand_ = false;
        execute();
    }
}

void Dan2Card::replyBlock(const uint8_t* data512)
{
    reply(kOk);
    out_.insert(out_.end(), data512, data512 + 512);
}

// Apple2Arduino.ino do_command, as izapple2 models it.
void Dan2Card::execute()
{
    const uint8_t cmd = cmd_[0], unit = cmd_[1];
    const uint32_t block = uint32_t(cmd_[4] | cmd_[5] << 8);
    const int bay = (unit & 0x80) ? 1 : 0;
    pom2::Block512Backing& b = disks_.backing(bay);
    std::array<uint8_t, 512> buf{};
    switch (cmd) {
    case 0: case 3:                              // STATUS, FORMAT
        reply(b.isLoaded() ? kOk : kNoDev);
        break;
    case 1: case 10:                             // READ, SAFEREAD
        if (!b.isLoaded())                 reply(kNoDev);
        else if (block >= b.blockCount() || !b.readBlock(block, buf.data())) reply(kIoErr);
        else                               replyBlock(buf.data());
        break;
    case 2:                                      // WRITE: status, then the data
        if (!b.isLoaded())                 reply(kNoDev);
        else if (b.isWriteProtected())     reply(kWriteProt);
        else if (block >= b.blockCount())  reply(kIoErr);
        else { reply(kOk); writeUnit_ = bay; writeBlock_ = uint16_t(block); writeBuf_.clear(); }
        break;
    case 4: case 8:                              // set volume, one-byte answer
        reply(kOk);
        break;
    case 6: case 7:                              // set volume, 512-byte answer
        replyBlock(buf.data());
        break;
    case 5: case 9:                              // get volume: one image per SD slot
        buf[0] = 1; buf[1] = 1;
        replyBlock(buf.data());
        break;
    case 11:                                     // version 3.4.2, Atmega328P, raw + FAT
        buf[0] = 3; buf[1] = 4; buf[2] = 2; buf[3] = 3; buf[4] = 0xC0;
        replyBlock(buf.data());
        break;
    default:                                     // incl. the boot-menu program's reads
        reply(kIoErr);
        break;
    }
}

// The 8255's RESET is the bus's: every port an input, port C cleared — the
// slot ROM back on page 0, which the EPROM boots from. A transfer in flight
// is abandoned, as the firmware's resync expects.
void Dan2Card::onReset()
{
    portC_ = 0;
    inCommand_ = false;
    cmd_.clear();
    writeUnit_ = -1;
    writeBuf_.clear();
    out_.clear();
}

void Dan2Card::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), kSnapMagic, kSnapMagic + 4);
    out.push_back(portC_);
    out.push_back(inCommand_ ? 1 : 0);
    out.push_back(uint8_t(cmd_.size()));
    for (std::size_t i = 0; i < 6; ++i) out.push_back(i < cmd_.size() ? cmd_[i] : 0);
    out.push_back(uint8_t(writeUnit_ + 1));
    out.push_back(uint8_t(writeBlock_)); out.push_back(uint8_t(writeBlock_ >> 8));
    out.push_back(uint8_t(writeBuf_.size())); out.push_back(uint8_t(writeBuf_.size() >> 8));
    out.insert(out.end(), writeBuf_.begin(), writeBuf_.end());
    out.push_back(uint8_t(out_.size())); out.push_back(uint8_t(out_.size() >> 8));
    out.insert(out.end(), out_.begin(), out_.end());
}

void Dan2Card::loadSnapshotState(const uint8_t* d, std::size_t len)
{
    if (!d || len < 4 + 3 + 6 + 3 + 2 || std::memcmp(d, kSnapMagic, 4) != 0) return;
    std::size_t p = 4;
    const uint8_t pc = d[p++], inCmd = d[p++], n = d[p++];
    if (n > 6) return;
    const uint8_t* c = d + p; p += 6;
    const int wu = int(d[p++]) - 1;
    const uint16_t wb = uint16_t(d[p] | d[p + 1] << 8); p += 2;
    if (p + 2 > len) return;
    const std::size_t wn = std::size_t(d[p] | d[p + 1] << 8); p += 2;
    if (wn > 512 || wu > 1 || p + wn + 2 > len) return;
    const uint8_t* w = d + p; p += wn;
    const std::size_t on = std::size_t(d[p] | d[p + 1] << 8); p += 2;
    if (p + on > len) return;
    portC_ = pc;
    inCommand_ = inCmd != 0;
    cmd_.assign(c, c + n);
    writeUnit_ = wu;
    writeBlock_ = wb;
    writeBuf_.assign(w, w + wn);
    out_.assign(d + p, d + p + on);
}
