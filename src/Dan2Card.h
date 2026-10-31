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

// Dan2Card — the DAN ][ Controller (Daniel L. Marks, 2022; improved firmware
// Thorsten Brehm): an 82C55 in front of an ATmega328P that serves ProDOS
// blocks from SD cards. Catalog key `dan2`. github.com/profdc9/Apple2Card,
// github.com/ThorstenBr/Apple2Card (zlib licence); izapple2's
// cardDan2Controller.go for the emulated protocol.
//
// The bus: the 8255 at $C0n0-$C0n3 (A0-A1 only). Port A is the data byte
// both ways; port C reads bit 7 = /OBF high (the ATmega took the last byte;
// always, here) and bit 5 = IBF (a byte waits); the control word $FA sets
// mode 2 on port A, and a bit set/reset word (bit 7 clear) on port C bit 0
// pages the slot ROM. The firmware sends $AC then the six ProDOS bytes
// $42-$47 (command, unit, buffer, block), and reads a status byte — $00,
// then 512 bytes for a read, or 512 bytes for it to send for a write.
//
// The ATmega is modelled at the protocol (as izapple2 does): STATUS, READ,
// WRITE, FORMAT, SAFEREAD, the volume commands (accepted, answered with
// volume 1 — POM2 mounts one image per SD slot), VERSION. Unit bit 7 picks
// SD slot A or B, POM2's two bays. The images are the HDV card's
// (Block512Backing, held by an unplugged ProDOSHardDiskCard): two-phase
// mount, autosave, notch, eject — and the generic per-slot media keys
// (media_slotN_bayM), so it never collides with an HDV card's. The boot
// menu program (`bootpg`, served from the ATmega's flash) is not modelled:
// its "read boot block" commands answer $27.
//
// The 6502 firmware: the Dan ][ EPROM (512 bytes, two pages) when
// roms/dan2_eprom.bin is there — the reference; else POM2's own page,
// written to the same protocol: the ProDOS signature, boot by SAFEREAD of
// block 0 to $0800, the block driver. `dan2_card` boots ProDOS on both.

#ifndef POM2_DAN2_CARD_H
#define POM2_DAN2_CARD_H

#include "MountableMediaCard.h"
#include "ProDOSHardDiskCard.h"
#include "SlotPeripheral.h"

#include <cstdint>
#include <deque>
#include <vector>

class Dan2Card : public SlotPeripheral, public pom2::MountableMediaCard
{
public:
    static constexpr const char* kEpromPath = "roms/dan2_eprom.bin";

    explicit Dan2Card(int slot);

    /// The Dan ][ EPROM (512 bytes). False, keeping POM2's page, otherwise.
    bool loadEprom(const std::vector<uint8_t>& bytes);
    bool epromLoaded() const { return epromLoaded_; }
    void useOwnFirmware();
    bool romLayoutError() const { return romLayoutError_; }
    int  romPage() const { return portC_ & 1; }

    // ─── MountableMediaCard: SD slot A and B ────────────────────────────
    int  bayCount() const override { return 2; }
    pom2::MediaBayInfo bayInfo(int bay) const override { return disks_.bayInfo(bay); }
    bool mountBay(int bay, const std::string& path, std::string& err) override
    { return disks_.mountBay(bay, path, err); }
    bool adoptBay(int bay, pom2::Block512Backing::PreparedImage&& p, std::string& err) override
    { return disks_.adoptBay(bay, std::move(p), err); }
    bool ejectBay(int bay) override { return disks_.ejectBay(bay); }
    bool flushBay(int bay, std::string& err) override { return disks_.flushBay(bay, err); }
    bool prepareEjectBay(int bay, pom2::Block512Backing::PendingWriteBack& out, std::string& err) override
    { return disks_.prepareEjectBay(bay, out, err); }
    void restoreBayDirty(int bay, const std::vector<uint32_t>& idx) override { disks_.restoreBayDirty(bay, idx); }
    void setBayWriteBack(int bay, bool on) override { disks_.setBayWriteBack(bay, on); }
    void setBayHostWriteProtected(int bay, bool on) override { disks_.setBayHostWriteProtected(bay, on); }
    std::vector<pom2::Block512Backing*> blockBackings() override { return disks_.blockBackings(); }
    pom2::Block512Backing& backing(int bay) { return disks_.backing(bay); }

    // ─── SlotPeripheral ─────────────────────────────────────────────────
    std::string_view name() const override { return "DAN ][ Controller"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead(uint8_t low8) override;
    bool    peekSlotRom(uint8_t low8, uint8_t& out) const override;
    void    onReset() override;
    void    appendSnapshotState(std::vector<uint8_t>& out) const override;
    void    loadSnapshotState(const uint8_t* data, std::size_t len) override;

private:
    void buildOwnFirmware();
    void command(uint8_t b);
    void execute();
    void reply(uint8_t code) { out_.push_back(code); }
    void replyBlock(const uint8_t* data512);

    int slot_;
    ProDOSHardDiskCard disks_;           // the two images, never plugged
    std::vector<uint8_t> rom_;           // 256 (POM2) or 512 (EPROM) bytes
    bool epromLoaded_ = false;
    bool romLayoutError_ = false;

    uint8_t portC_ = 0;                  // the bit set/reset latch (page = bit 0)
    bool    inCommand_ = false;          // $AC seen
    std::vector<uint8_t> cmd_;           // the six bytes after it
    int     writeUnit_ = -1;             // a write in flight: its bay
    uint16_t writeBlock_ = 0;
    std::vector<uint8_t> writeBuf_;
    std::deque<uint8_t> out_;            // bytes for the 6502
};

#endif // POM2_DAN2_CARD_H
