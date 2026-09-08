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

// AtaBlockDevice — minimal ATA/IDE taskfile device over a flat 512-byte block
// backing. The subset the Apple II IDE-class slot cards actually drive: IDENTIFY
// DEVICE, READ SECTOR(S), WRITE SECTOR(S), LBA28. Reusable across CFFA / Vulcan
// / Zip / Focus (TODO.md § Cartes de stockage MAME-fidèles, P1).
//
// Register interface is isomorphic to MAME's ata_interface_device cs0 access
// (machine/ataintf): cs0_r/cs0_w take the taskfile register index 0..7 and the
// data register (0) is 16-bit. The 8-bit↔16-bit data latch split lives in the
// owning slot card (CffaCard), exactly as MAME's a2cffa.cpp does it.
//
//   reg 0  Data            (16-bit, DRQ-gated PIO)
//   reg 1  Error (R) / Features (W)
//   reg 2  Sector count    (0 ⇒ 256)
//   reg 3  LBA  0..7
//   reg 4  LBA  8..15
//   reg 5  LBA 16..23
//   reg 6  Drive/Head: bits0-3 LBA24..27, bit4 drive, bit6 LBA, bits5,7 obsolete
//   reg 7  Status (R) / Command (W)
//
// CHS addressing for I/O follows MAME `ata_mass_storage_device_base::
// lba_address()` (atastorage.cpp:44-53): devHead bit 6 picks LBA28, else the
// cyl/head/sector registers translate through the latched geometry (default
// 16 heads × 63 sectors = the IDENTIFY page; INITIALIZE DEVICE PARAMETERS $91
// re-latches it). NOT modelled: DMA, interrupts, security/SMART. CHD backing
// is P1-phase-2.

#ifndef POM2_ATA_BLOCK_DEVICE_H
#define POM2_ATA_BLOCK_DEVICE_H

#include "Block512Backing.h"

#include <array>
#include <cstdint>

namespace pom2 {

class AtaBlockDevice
{
public:
    // ATA status register bits.
    static constexpr uint8_t kStBSY  = 0x80; // busy
    static constexpr uint8_t kStDRDY = 0x40; // device ready
    static constexpr uint8_t kStDF   = 0x20; // device fault
    static constexpr uint8_t kStDSC  = 0x10; // seek complete
    static constexpr uint8_t kStDRQ  = 0x08; // data request
    static constexpr uint8_t kStERR  = 0x01; // error

    // ATA Error-register bit: aborted command (used for a write to a
    // write-protected device — ATA-1 §9.1).
    static constexpr uint8_t kErrABRT = 0x04;
    // ATA Error-register bit: ID NOT FOUND — the addressed sector is not on
    // this device (ATA-1 §9.1, ATA-2 §7.2.6 "the requested sector could not
    // be found"). That is what an out-of-range LBA is, on READ as much as on
    // WRITE; READ used to answer a zero-filled sector with a clean status.
    static constexpr uint8_t kErrIDNF = 0x10;

    // ATA commands we honour explicitly; everything else completes as a no-op.
    static constexpr uint8_t kCmdRead       = 0x20;
    static constexpr uint8_t kCmdReadMulti  = 0xC4;
    static constexpr uint8_t kCmdWrite      = 0x30;
    static constexpr uint8_t kCmdWriteMulti = 0xC5;
    static constexpr uint8_t kCmdIdentify   = 0xEC;
    // INITIALIZE DEVICE PARAMETERS — latches the CHS translation geometry
    // (MAME `IDE_COMMAND_SET_CONFIG`, atahle.h:143 / atastorage.cpp:267-269).
    static constexpr uint8_t kCmdInitParams = 0x91;

    /// The backing this device serves. CffaCard mounts images through it.
    Block512Backing&       backing()       { return backing_; }
    const Block512Backing& backing() const { return backing_; }

    /// Hard reset (power-on / SRST): clears the taskfile and any in-flight PIO.
    void reset();

    /// Is THIS device the one the host last addressed? Register 6 bit 4 (DRV
    /// / IDE_DEVICE_HEAD_DRV) picks master (0) or slave (1) on the shared
    /// cable, and POM2 fits exactly one device — the master. MAME
    /// `ata_hle_device::device_selected()` (machine/atahle.cpp) compares that
    /// bit against the device's own cable position and `read_cs0` answers 0
    /// when they differ; POM2 read the bit nowhere, so the CFFA firmware's
    /// slave scan (roms/cffa20ee02.bin $CCC: LDA $05F8,Y / EOR #$10 /
    /// STA $C08E,X — the scan MAME enables by patching m_rom[0x800/0x801]
    /// to 0x0D, a2cffa.cpp device_start) found a SECOND copy of the same
    /// medium, and a write addressed to it landed on the master's image.
    bool selected() const { return (devHead_ & 0x10) == 0; }

    /// cs0 (command block) register access. reg 0 is the 16-bit data port; the
    /// other registers carry an 8-bit value in the low byte.
    uint16_t cs0_r(uint8_t reg);
    void     cs0_w(uint8_t reg, uint16_t val);

    /// cs1 (control block) register access — only the alternate status (read)
    /// and device control (write) at offset 6 are meaningful here.
    uint16_t cs1_r(uint8_t reg);
    void     cs1_w(uint8_t reg, uint16_t val);

    /// Snapshot of the guest-visible chip state (taskfile + in-flight PIO
    /// phase + sector word buffer + CHS geometry). Backing media is
    /// host-side and NOT serialized — same policy as every other card.
    /// Raw layout, no magic: the owning card wraps it.
    void appendSnapshotState(std::vector<uint8_t>& out) const;
    /// Restores a blob appendSnapshotState produced. Returns bytes
    /// consumed, or 0 on malformed input (device left untouched).
    size_t loadSnapshotState(const uint8_t* data, size_t len);
    /// Fixed serialized size (see the .cpp): 9 regs + phase + lba(4) +
    /// sectorsLeft(2) + wordIdx(2) + wordBuf(512) + CHS(2).
    static constexpr size_t kSnapshotBytes = 9 + 1 + 4 + 2 + 2 + 512 + 2;

private:
    enum class Phase { Idle, PioIn, PioOut };

    void startCommand(uint8_t cmd);
    void loadSectorToBuffer();   // backing block at lba_ → wordBuf_
    bool flushBufferToSector();  // wordBuf_ → backing block at lba_
    void fillIdentify();
    uint32_t currentLba() const;
    /// MAME `ata_hle_device_base::next_sector()`: after every sector of a
    /// READ/WRITE the address registers step to the next sector (LBA28 with
    /// its carry into the device/head nibble, or CHS through the latched
    /// geometry) and the sector count counts down, so a driver that reads
    /// them back after a multi-sector transfer sees where the head stopped.
    void nextSector();

    Block512Backing backing_;

    // Taskfile registers (8-bit each except data).
    uint8_t error_       = 0x00;
    uint8_t features_    = 0x00;
    uint8_t sectorCount_ = 0x00;
    uint8_t lba0_        = 0x00;
    uint8_t lba1_        = 0x00;
    uint8_t lba2_        = 0x00;
    uint8_t devHead_     = 0xA0; // bits 5,7 obsolete-1, LBA bit clear initially
    uint8_t status_      = kStDRDY | kStDSC;
    uint8_t control_     = 0x00;

    Phase    phase_       = Phase::Idle;
    uint32_t lba_         = 0;   // LBA latched at command start
    uint16_t sectorsLeft_ = 0;   // sectors remaining in the current transfer
    bool     advanceRegs_ = false; // READ/WRITE step the taskfile; IDENTIFY does not
    size_t   wordIdx_     = 0;   // current word within wordBuf_ (0..256)
    std::array<uint16_t, 256> wordBuf_{}; // one 512-byte sector as 256 LE words

    // CHS translation geometry, used when devHead_ bit 6 (LBA select) is
    // CLEAR — MAME `ata_mass_storage_device_base::lba_address()`
    // (atastorage.cpp:44-53) falls back to standard CHS with
    // m_num_heads/m_num_sectors. Defaults match the IDENTIFY geometry
    // (16 heads × 63 sectors); INITIALIZE DEVICE PARAMETERS ($91)
    // re-latches them like MAME's set_geometry (atastorage.cpp:267-269).
    uint8_t numSectors_ = 63;
    uint8_t numHeads_   = 16;
};

} // namespace pom2

#endif // POM2_ATA_BLOCK_DEVICE_H
