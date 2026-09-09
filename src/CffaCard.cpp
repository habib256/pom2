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

#include "CffaCard.h"
#include "Logger.h"

#include <cstring>

#include <fstream>

namespace pom2 {

bool CffaCard::loadRom(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        lastError_ = "Cannot open CFFA ROM: " + path;
        pom2::log().warn("CFFA", lastError_);
        return false;
    }
    f.seekg(0, std::ios::end);
    const auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (sz != kRomBytes) {
        lastError_ = "CFFA ROM must be exactly 4096 bytes (got " +
                     std::to_string(sz) + "): " + path;
        pom2::log().warn("CFFA", lastError_);
        return false;
    }
    f.read(reinterpret_cast<char*>(rom_.data()),
           static_cast<std::streamsize>(rom_.size()));
    if (!f) {
        lastError_ = "Short read on CFFA ROM: " + path;
        pom2::log().warn("CFFA", lastError_);
        return false;
    }
    // MAME `a2cffa.cpp` device_start() patches two EEPROM config defaults
    // every boot, before the firmware's device scan runs: enable the slave
    // device and allow up to 13 devices on each IDE connector. The raw dump
    // ships 0x04 / 0x00 ($C801 = 0 disables the 2nd connector); without the
    // patch the firmware scans a different EEPROM than the MAME oracle.
    //   m_rom[0x800] = 13;  m_rom[0x801] = 13;   (a2cffa.cpp device_start)
    // rom_[0x800] is the first byte of the $C800 shared-ROM window.
    rom_[0x800] = 0x0D;
    rom_[0x801] = 0x0D;

    romLoaded_ = true;
    pom2::log().info("CFFA", "Loaded firmware ROM: " + path);
    return true;
}

bool CffaCard::loadImage(const std::string& path)
{
    const bool ok = ata_.backing().loadImage(path);
    ata_.reset();
    lastError_ = ok ? std::string{} : ata_.backing().lastError();
    return ok;
}

bool CffaCard::adoptImage(pom2::Block512Backing::PreparedImage&& p)
{
    // Mirrors loadImage. The reset is the load-bearing half: a mount that
    // lands while the guest is mid-PIO leaves phase_/lba_/sectorsLeft_
    // describing the OUTGOING medium, and the next 256 words the guest feeds
    // the data port would have been flushed into the new image at the old
    // LBA. `adoptImage` is the path pom2::mountBlockCard takes, so it was the
    // common one, not the exotic one.
    const bool ok = ata_.backing().adoptImage(std::move(p));
    ata_.reset();
    lastError_ = ok ? std::string{} : ata_.backing().lastError();
    return ok;
}

bool CffaCard::loadImageFromBytes(std::vector<uint8_t> bytes,
                                  const std::string& label,
                                  const std::string& hostFolder)
{
    const bool ok = ata_.backing().loadFromBytes(std::move(bytes), label, hostFolder);
    ata_.reset();
    lastError_ = ok ? std::string{} : ata_.backing().lastError();
    return ok;
}

bool CffaCard::saveDirty()
{
    const bool ok = ata_.backing().saveDirty();
    lastError_ = ok ? std::string{} : ata_.backing().lastError();
    return ok;
}

bool CffaCard::ejectImage()
{
    // Save-on-eject when the user opted into write-back and the medium allows.
    Block512Backing& b = ata_.backing();
    // `isMediumLocked()`, not `isWriteProtected()`: a notch flipped on the
    // mounted image must not make the eject drop blocks the guest already
    // wrote (Block512Backing::isMediumLocked).
    if (b.isLoaded() && b.hasUnsavedChanges() &&
        b.isWriteBackEnabled() && !b.isMediumLocked()) {
        if (!b.saveDirty()) {
            lastError_ = b.lastError();
            pom2::log().warn("CFFA", "Save-on-eject failed: " + b.lastError());
            return false;
        }
    }
    b.eject();
    ata_.reset();
    lastError_.clear();
    return true;
}

void CffaCard::onReset()
{
    ata_.reset();
    lastReadData_  = 0;
    lastWriteData_ = 0;
}

// ── $C0nX device-select — MAME a2cffa.cpp read_c0nx (~L117-147) ────────────
uint8_t CffaCard::deviceSelectRead(uint8_t low4)
{
    // MAME's read_c0nx ends every non-data path in `get_open_bus()` — the
    // switch cases that only flip the EEPROM write-enable latch `break` out
    // and fall into the shared tail, as does an offset the card does not
    // decode. POM2 answered a hard $00, which is a value a probing driver can
    // mistake for real data; the floating bus is what the 6502 actually sees
    // (Memory installs the source via SlotBus::setFloatingBusSource).
    switch (low4) {
        case 0x0: // high byte of the last 16-bit ATA data word read at $C0n8
            return static_cast<uint8_t>(lastReadData_ >> 8);
        case 0x3: // EEPROM write-enable off
            writeProtect_ = false;
            return openBus();
        case 0x4: // EEPROM write-enable on (protect)
            writeProtect_ = true;
            return openBus();
        case 0x8: { // ATA data register: 16-bit read, low byte to bus, high latched
            const uint16_t d = ata_.cs0_r(0);
            lastReadData_ = d;
            return static_cast<uint8_t>(d & 0xFF);
        }
        case 0x9: case 0xA: case 0xB: case 0xC:
        case 0xD: case 0xE: case 0xF: // ATA taskfile registers 1..7
            return static_cast<uint8_t>(ata_.cs0_r(static_cast<uint8_t>(low4 - 8)) & 0xFF);
        default:
            return openBus();
    }
}

// ── $C0nX device-select — MAME a2cffa.cpp write_c0nx (~L154-176) ───────────
void CffaCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    switch (low4) {
        case 0x0: // high byte of the 16-bit ATA data word (committed at $C0n8)
            lastWriteData_ = static_cast<uint16_t>((lastWriteData_ & 0x00FF) |
                                                   (static_cast<uint16_t>(v) << 8));
            break;
        case 0x3: writeProtect_ = false; break;
        case 0x4: writeProtect_ = true;  break;
        case 0x8: // ATA data register: combine latched high byte, write 16-bit
            lastWriteData_ = static_cast<uint16_t>((lastWriteData_ & 0xFF00) | v);
            ata_.cs0_w(0, lastWriteData_);
            break;
        case 0x9: case 0xA: case 0xB: case 0xC:
        case 0xD: case 0xE: case 0xF: // ATA taskfile registers 1..7
            ata_.cs0_w(static_cast<uint8_t>(low4 - 8), v);
            break;
        default:
            break;
    }
}

// ── $CnXX slot ROM — MAME a2cffa.cpp read_cnxx (~L179-183) ─────────────────
uint8_t CffaCard::slotRomRead(uint8_t low8)
{
    return rom_[static_cast<size_t>(low8) + static_cast<size_t>(slot_) * 0x100];
}

// ── $C800 expansion ROM — MAME a2cffa.cpp read_c800 (~L185-191) ────────────
uint8_t CffaCard::expansionRomRead(uint16_t offset)
{
    const size_t idx = 0x800 + offset;
    return (idx < rom_.size()) ? rom_[idx] : 0xFF;
}

void CffaCard::expansionRomWrite(uint16_t offset, uint8_t v)
{
    if (writeProtect_) return;          // EEPROM write-enable gate
    const size_t idx = 0x800 + offset;
    if (idx < rom_.size()) rom_[idx] = v;
}

namespace {
constexpr uint8_t kCffaSnapMagic[4] = { 'C', 'F', 'A', '1' };
}

void CffaCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), kCffaSnapMagic, kCffaSnapMagic + 4);
    out.push_back(static_cast<uint8_t>(lastReadData_));
    out.push_back(static_cast<uint8_t>(lastReadData_ >> 8));
    out.push_back(static_cast<uint8_t>(lastWriteData_));
    out.push_back(static_cast<uint8_t>(lastWriteData_ >> 8));
    out.push_back(writeProtect_ ? 1 : 0);
    ata_.appendSnapshotState(out);
}

void CffaCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    // Foreign/undersized blobs are ignored (a different card type sat in
    // this slot when the frame was recorded).
    if (data == nullptr ||
        len < 4 + 5 + pom2::AtaBlockDevice::kSnapshotBytes ||
        std::memcmp(data, kCffaSnapMagic, 4) != 0)
        return;
    size_t p = 4;
    lastReadData_  = static_cast<uint16_t>(data[p] | (data[p + 1] << 8)); p += 2;
    lastWriteData_ = static_cast<uint16_t>(data[p] | (data[p + 1] << 8)); p += 2;
    writeProtect_  = data[p++] != 0;
    ata_.loadSnapshotState(data + p, len - p);
}

} // namespace pom2
