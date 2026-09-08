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

#include "AtaBlockDevice.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pom2 {

namespace {
bool ataTraceOn()
{
    static const bool on = std::getenv("POM2_TRACE_CFFA") != nullptr;
    return on;
}
} // namespace

void AtaBlockDevice::reset()
{
    error_       = 0x00;
    features_    = 0x00;
    sectorCount_ = 0x00;
    lba0_ = lba1_ = lba2_ = 0x00;
    devHead_     = 0xA0;
    status_      = kStDRDY | kStDSC;
    control_     = 0x00;
    phase_       = Phase::Idle;
    lba_         = 0;
    sectorsLeft_ = 0;
    wordIdx_     = 0;
    wordBuf_.fill(0);
    // Power-on default translation geometry. POM2's reset() models the
    // power-on path (the machine constructs/resets the card with it), so
    // an odd geometry latched by a previous guest's INITIALIZE DEVICE
    // PARAMETERS ($91) must not poison the next boot's CHS decode. (On
    // real drives $91 survives a SOFT reset — ATA-2 §9.16 — but POM2 has
    // no separate soft-reset entry for the card.)
    numSectors_ = 63;
    numHeads_   = 16;
}

uint32_t AtaBlockDevice::currentLba() const
{
    // MAME `ata_mass_storage_device_base::lba_address()`
    // (atastorage.cpp:44-53): devHead bit 6 (IDE_DEVICE_HEAD_L) selects
    // LBA28 direct; clear = standard CHS through the latched geometry.
    // The CFFA firmware always runs LBA, but legacy callers (and the
    // power-on devHead_ = $A0 default) are CHS — decoding their
    // cyl/head/sector registers as a raw LBA28 read the wrong block.
    if (devHead_ & 0x40) {
        return (static_cast<uint32_t>(devHead_ & 0x0F) << 24) |
               (static_cast<uint32_t>(lba2_) << 16) |
               (static_cast<uint32_t>(lba1_) << 8) |
                static_cast<uint32_t>(lba0_);
    }
    // CHS: lba2/lba1 = cylinder high/low, lba0 = 1-based sector number.
    // Sector number 0 is illegal in CHS (ATA-2 §8: sectors are 1-based,
    // and 0 is the power-on register value): `+ 0 - 1` underflowed to
    // 0xFFFFFFFF, which the backing bounds-check silently turned into a
    // zero-filled read / dropped write with SUCCESS status. Map it to
    // block 0 of the cylinder/head instead — out-of-range commands still
    // fail the backing bounds check downstream.
    const uint32_t sec1 = (lba0_ != 0) ? static_cast<uint32_t>(lba0_) - 1 : 0;
    return ((((static_cast<uint32_t>(lba2_) << 8) |
               static_cast<uint32_t>(lba1_)) * numHeads_ +
             (devHead_ & 0x0F)) * numSectors_) +
           sec1;
}

void AtaBlockDevice::loadSectorToBuffer()
{
    uint8_t sector[Block512Backing::kBlockBytes];
    if (!backing_.readBlock(lba_, sector)) {
        std::memset(sector, 0, sizeof(sector)); // out-of-range LBA reads as zeros
    }
    for (size_t i = 0; i < 256; ++i) {
        wordBuf_[i] = static_cast<uint16_t>(sector[2 * i]) |
                      (static_cast<uint16_t>(sector[2 * i + 1]) << 8);
    }
    wordIdx_ = 0;
}

bool AtaBlockDevice::flushBufferToSector()
{
    uint8_t sector[Block512Backing::kBlockBytes];
    for (size_t i = 0; i < 256; ++i) {
        sector[2 * i]     = static_cast<uint8_t>(wordBuf_[i] & 0xFF);
        sector[2 * i + 1] = static_cast<uint8_t>(wordBuf_[i] >> 8);
    }
    const bool ok = backing_.writeBlock(lba_, sector);
    wordIdx_ = 0;
    return ok;
}

void AtaBlockDevice::fillIdentify()
{
    wordBuf_.fill(0);
    const uint32_t total = static_cast<uint32_t>(backing_.blockCount());

    // Plausible CHS geometry for legacy callers (16 heads × 63 sectors).
    const uint32_t spt   = 63;
    const uint32_t heads = 16;
    uint32_t cyls = total / (spt * heads);
    if (cyls > 0xFFFF) cyls = 0xFFFF;

    auto putString = [&](size_t firstWord, size_t wordLen, const char* s) {
        // ATA strings are space-padded and byte-swapped within each word.
        for (size_t w = 0; w < wordLen; ++w) {
            char hi = *s ? *s++ : ' ';
            char lo = *s ? *s++ : ' ';
            wordBuf_[firstWord + w] =
                static_cast<uint16_t>((static_cast<uint8_t>(hi) << 8) |
                                       static_cast<uint8_t>(lo));
        }
    };

    wordBuf_[0]  = 0x0040;                 // general config: fixed device
    wordBuf_[1]  = static_cast<uint16_t>(cyls);
    wordBuf_[3]  = static_cast<uint16_t>(heads);
    wordBuf_[6]  = static_cast<uint16_t>(spt);
    putString(10, 10, "POM2-0001");        // serial number  (words 10..19)
    putString(23, 4,  "1.0");              // firmware rev   (words 23..26)
    putString(27, 20, "POM2 ATA Block Device"); // model      (words 27..46)
    wordBuf_[47] = 0x8001;                 // max sectors per READ/WRITE MULTIPLE = 1
    wordBuf_[49] = 0x0200;                 // capabilities: LBA supported (bit 9)
    wordBuf_[53] = 0x0001;                 // words 54-58 (current CHS/capacity) valid
    // Words 54-56 = the CURRENT (post-INITIALIZE DEVICE PARAMETERS) logical
    // geometry, and word 53 bit 0 above is the flag that says they mean
    // something (ATA-1 §6.2.1.6). They were left at zero while the flag
    // claimed them valid (bug hunt 4 #25) — a host that trusts the flag then
    // computes a zero-cylinder, zero-head drive. Report the geometry the
    // taskfile is actually decoding through, which after a $91 is the latched
    // pair and before it the power-on default.
    const uint32_t curSpt   = (numSectors_ != 0) ? numSectors_ : 63u;
    const uint32_t curHeads = (numHeads_   != 0) ? numHeads_   : 16u;
    uint32_t curCyls = total / (curSpt * curHeads);
    if (curCyls > 0xFFFF) curCyls = 0xFFFF;
    wordBuf_[54] = static_cast<uint16_t>(curCyls);
    wordBuf_[55] = static_cast<uint16_t>(curHeads);
    wordBuf_[56] = static_cast<uint16_t>(curSpt);
    // Current capacity in sectors (words 57-58, 32-bit low-word-first). The
    // CFFA firmware reads THIS field — not 60-61 — to size its partitions
    // (a2cffa firmware $CD35-$CD52); leaving it zero ⇒ 0 partitions ⇒ Err $28.
    wordBuf_[57] = static_cast<uint16_t>(total & 0xFFFF);
    wordBuf_[58] = static_cast<uint16_t>((total >> 16) & 0xFFFF);
    wordBuf_[60] = static_cast<uint16_t>(total & 0xFFFF);        // LBA28 total,
    wordBuf_[61] = static_cast<uint16_t>((total >> 16) & 0xFFFF);// low word first

    wordIdx_ = 0;
}

void AtaBlockDevice::nextSector()
{
    // The register file is the driver's view of the head. It used to stand
    // still through a multi-sector transfer: after a 3-sector READ from LBA 5
    // the registers still said 5 and the count still said 3, where real
    // silicon (and MAME) leave 8 and 0. No POM2-supported driver reads them
    // back today, which is why it never bit; a conformance gap all the same.
    --sectorCount_;                                   // 0 wraps: 256 → 255 … → 0
    if (devHead_ & 0x40) {                            // LBA28
        const uint32_t lba = currentLba() + 1;
        lba0_    = static_cast<uint8_t>(lba);
        lba1_    = static_cast<uint8_t>(lba >> 8);
        lba2_    = static_cast<uint8_t>(lba >> 16);
        devHead_ = static_cast<uint8_t>((devHead_ & 0xF0) | ((lba >> 24) & 0x0F));
        return;
    }
    uint8_t  sector = lba0_;                          // CHS, 1-based sectors
    uint8_t  head   = devHead_ & 0x0F;
    uint16_t cyl    = static_cast<uint16_t>(lba1_ | (lba2_ << 8));
    if (++sector > numSectors_) {
        sector = 1;
        if (++head >= numHeads_) { head = 0; ++cyl; }
    }
    lba0_    = sector;
    lba1_    = static_cast<uint8_t>(cyl);
    lba2_    = static_cast<uint8_t>(cyl >> 8);
    devHead_ = static_cast<uint8_t>((devHead_ & 0xF0) | head);
}

void AtaBlockDevice::startCommand(uint8_t cmd)
{
    // A command written while the host has the OTHER device selected is not
    // ours to run — MAME `ata_hle_device::write_cs0` only starts a command on
    // the selected device (machine/atahle.cpp), and the CFFA firmware probes
    // the slave by setting register 6 bit 4 and issuing IDENTIFY. Answering
    // it advertised a second drive that was the same medium.
    if (!selected()) return;

    error_  = 0x00;
    status_ = kStDRDY | kStDSC; // BSY would pulse on real silicon; we settle instantly
    advanceRegs_ = (cmd == kCmdRead || cmd == kCmdReadMulti ||
                    cmd == kCmdWrite || cmd == kCmdWriteMulti);

    if (ataTraceOn()) {
        std::fprintf(stderr,
            "[ATA] cmd=$%02X lba=%u count=%u devHead=$%02X blocks=%zu\n",
            cmd, currentLba(),
            (sectorCount_ == 0) ? 256u : sectorCount_,
            devHead_, backing_.blockCount());
    }

    switch (cmd) {
        case kCmdIdentify:
            fillIdentify();
            sectorsLeft_ = 1;
            phase_  = Phase::PioIn;
            status_ = kStDRDY | kStDSC | kStDRQ;
            break;

        case kCmdRead:
        case kCmdReadMulti: {
            // A READ whose range leaves the medium is ID NOT FOUND, not a
            // zero-filled sector with a clean status (bug hunt 4 #11). ATA-1
            // §9.1 / ATA-2 §7.2.6: IDNF = "the requested sector could not be
            // found". WRITE already refused; READ handed the caller 512 zeros
            // and CLC, so a driver reading past a truncated image saw a valid
            // (empty) block instead of an error.
            const uint32_t requestedLba = currentLba();
            const uint32_t requestedCount =
                (sectorCount_ == 0) ? 256u : sectorCount_;
            const size_t blocks = backing_.blockCount();
            if (requestedLba >= blocks ||
                requestedCount > blocks - static_cast<size_t>(requestedLba)) {
                error_  = kErrIDNF;
                status_ = kStDRDY | kStDSC | kStERR;
                phase_  = Phase::Idle;
                break;
            }
            lba_         = requestedLba;
            sectorsLeft_ = static_cast<uint16_t>(requestedCount);
            loadSectorToBuffer();
            phase_  = Phase::PioIn;
            status_ = kStDRDY | kStDSC | kStDRQ;
            break;
        }

        case kCmdWrite:
        case kCmdWriteMulti: {
            // ATA-1: a WRITE to a write-protected device must abort — ERR set
            // in Status, ABRT in the Error register, no DRQ — rather than
            // accept the data and silently drop it in flushBufferToSector
            // (which returns "success" to ProDOS). The HDV synthetic card
            // surfaces WP via its own status path; the ATA/CFFA path does it
            // here so a WP 2IMG can't be "written" without an error.
            const uint32_t requestedLba = currentLba();
            const uint32_t requestedCount =
                (sectorCount_ == 0) ? 256u : sectorCount_;
            const size_t blocks = backing_.blockCount();
            const bool outside = requestedLba >= blocks ||
                requestedCount > blocks - static_cast<size_t>(requestedLba);
            if (backing_.isWriteProtected() || outside) {
                // Two different faults, two different Error-register codes
                // (ATA-1 §9.1): a write-protected medium ABORTS the command,
                // an address off the end is ID NOT FOUND. They used to share
                // ABRT, so a driver could not tell "locked disk" from "block
                // past the end" — and READ reported neither.
                error_  = outside ? kErrIDNF : kErrABRT;
                status_ = kStDRDY | kStDSC | kStDF | kStERR;
                phase_  = Phase::Idle;
                break;
            }
            lba_         = requestedLba;
            sectorsLeft_ = static_cast<uint16_t>(requestedCount);
            wordIdx_     = 0;
            phase_  = Phase::PioOut;
            status_ = kStDRDY | kStDSC | kStDRQ; // host now feeds the first sector
            break;
        }

        case kCmdInitParams:
            // INITIALIZE DEVICE PARAMETERS: latch the CHS translation
            // geometry from sector-count / devHead, exactly MAME's
            // `set_geometry(m_sector_count, (m_device_head &
            // IDE_DEVICE_HEAD_HS) + 1)` (atastorage.cpp:267-269).
            if (sectorCount_ != 0) numSectors_ = sectorCount_;
            numHeads_ = static_cast<uint8_t>((devHead_ & 0x0F) + 1);
            phase_  = Phase::Idle;
            status_ = kStDRDY | kStDSC;
            break;

        default:
            // Set-features / flush / standby / etc. — accept and
            // complete with no data phase.
            phase_  = Phase::Idle;
            status_ = kStDRDY | kStDSC;
            break;
    }
}

uint16_t AtaBlockDevice::cs0_r(uint8_t reg)
{
    // An unselected device drives nothing onto the bus: MAME's
    // `ata_hle_device::read_cs0` returns 0 unless `device_selected()`
    // (machine/atahle.cpp). Status = $00 has DRDY clear, which is exactly how
    // a driver decides "no device at this address" — and is what makes the
    // CFFA slave scan come back empty instead of cloning the master.
    if (!selected()) return 0x0000;
    switch (reg & 0x07) {
        case 0: { // data port (PIO in)
            if (phase_ != Phase::PioIn) return 0xFFFF;
            const uint16_t w = wordBuf_[wordIdx_++];
            if (wordIdx_ >= 256) {
                // Sector complete.
                if (advanceRegs_) nextSector();
                if (sectorsLeft_ > 0) --sectorsLeft_;
                if (sectorsLeft_ > 0) {
                    ++lba_;
                    loadSectorToBuffer();      // DRQ stays set for the next sector
                } else {
                    phase_  = Phase::Idle;
                    status_ = kStDRDY | kStDSC; // DRQ cleared
                }
            }
            return w;
        }
        case 1: return error_;
        case 2: return sectorCount_;
        case 3: return lba0_;
        case 4: return lba1_;
        case 5: return lba2_;
        case 6: return devHead_;
        case 7: return status_;
    }
    return 0xFF;
}

void AtaBlockDevice::cs0_w(uint8_t reg, uint16_t val)
{
    const uint8_t v = static_cast<uint8_t>(val & 0xFF);
    switch (reg & 0x07) {
        case 0: { // data port (PIO out)
            if (phase_ != Phase::PioOut) return;
            wordBuf_[wordIdx_++] = val;
            if (wordIdx_ >= 256) {
                if (!flushBufferToSector()) {
                    error_       = kErrABRT;
                    status_      = kStDRDY | kStDSC | kStDF | kStERR;
                    phase_       = Phase::Idle;
                    sectorsLeft_ = 0;
                    return;
                }
                if (advanceRegs_) nextSector();
                if (sectorsLeft_ > 0) --sectorsLeft_;
                if (sectorsLeft_ > 0) {
                    ++lba_;                    // DRQ stays set for the next sector
                } else {
                    phase_  = Phase::Idle;
                    status_ = kStDRDY | kStDSC; // DRQ cleared
                }
            }
            return;
        }
        case 1: features_    = v; return;
        case 2: sectorCount_ = v; return;
        case 3: lba0_        = v; return;
        case 4: lba1_        = v; return;
        case 5: lba2_        = v; return;
        case 6: devHead_     = v; return;
        case 7: startCommand(v);  return;
    }
}

uint16_t AtaBlockDevice::cs1_r(uint8_t reg)
{
    // Offset 6 = alternate status (same value as the primary status register,
    // but reading it does not clear a pending interrupt — moot, no IRQ here).
    // Same device-select gate as cs0_r: an unselected device is silent.
    if (!selected()) return 0x0000;
    if ((reg & 0x07) == 6) return status_;
    return 0xFF;
}

void AtaBlockDevice::cs1_w(uint8_t reg, uint16_t val)
{
    // Offset 6 = device control: bit 2 = SRST (software reset).
    if ((reg & 0x07) == 6) {
        control_ = static_cast<uint8_t>(val & 0xFF);
        if (control_ & 0x04) reset();
    }
}

void AtaBlockDevice::appendSnapshotState(std::vector<uint8_t>& out) const
{
    // A rewind mid-PIO used to desync the 512-byte stream: the guest's
    // firmware resumed pulling data words from wherever the LIVE wordIdx_
    // happened to sit, not where the restored machine's firmware was.
    out.push_back(error_);
    out.push_back(features_);
    out.push_back(sectorCount_);
    out.push_back(lba0_);
    out.push_back(lba1_);
    out.push_back(lba2_);
    out.push_back(devHead_);
    out.push_back(status_);
    out.push_back(control_);
    out.push_back(static_cast<uint8_t>(phase_));
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>(lba_ >> (8 * i)));
    out.push_back(static_cast<uint8_t>(sectorsLeft_));
    out.push_back(static_cast<uint8_t>(sectorsLeft_ >> 8));
    out.push_back(static_cast<uint8_t>(wordIdx_));
    out.push_back(static_cast<uint8_t>(wordIdx_ >> 8));
    for (uint16_t w : wordBuf_) {
        out.push_back(static_cast<uint8_t>(w));
        out.push_back(static_cast<uint8_t>(w >> 8));
    }
    out.push_back(numSectors_);
    out.push_back(numHeads_);
}

size_t AtaBlockDevice::loadSnapshotState(const uint8_t* data, size_t len)
{
    if (data == nullptr || len < kSnapshotBytes) return 0;
    size_t p = 0;
    error_       = data[p++];
    features_    = data[p++];
    sectorCount_ = data[p++];
    lba0_        = data[p++];
    lba1_        = data[p++];
    lba2_        = data[p++];
    devHead_     = data[p++];
    status_      = data[p++];
    control_     = data[p++];
    const uint8_t ph = data[p++];
    phase_ = (ph == 1) ? Phase::PioIn : (ph == 2) ? Phase::PioOut
                                                  : Phase::Idle;
    // Not in the blob (the layout is fixed): a transfer in flight after a
    // restore is a READ/WRITE unless it is the one-sector IDENTIFY, which the
    // CFFA firmware re-issues from scratch anyway.
    advanceRegs_ = (phase_ != Phase::Idle);
    lba_ = 0;
    for (int i = 0; i < 4; ++i)
        lba_ |= static_cast<uint32_t>(data[p++]) << (8 * i);
    sectorsLeft_ = static_cast<uint16_t>(data[p] | (data[p + 1] << 8)); p += 2;
    wordIdx_     = static_cast<size_t>(data[p] | (data[p + 1] << 8));   p += 2;
    // >=, not >: wordIdx_ == 256 with phase_ restored to PioIn/PioOut lets
    // the next data-port access index one past the array — an OOB read,
    // and an OOB WRITE on the PioOut path.
    if (wordIdx_ >= wordBuf_.size()) wordIdx_ = 0;   // untrusted
    for (auto& w : wordBuf_) {
        w = static_cast<uint16_t>(data[p] | (data[p + 1] << 8));
        p += 2;
    }
    numSectors_ = data[p++];
    numHeads_   = data[p++];
    if (numSectors_ == 0) numSectors_ = 63;   // CHS divide-by-zero guard
    if (numHeads_   == 0) numHeads_   = 16;
    return p;
}

} // namespace pom2
