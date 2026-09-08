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

// CffaCard — MAME-faithful CFFA 2.0 (CompactFlash / IDE) slot card: the real
// 4 KB dumped firmware (cffa20ee02.bin / cffa20eec02.bin) executed over an
// emulated ATA bus chip (AtaBlockDevice), image stored as raw LBA behind it.
// Contrast with the synthetic ProDOSHardDiskCard (AppleWin lineage). See
// DEV.md § CffaCard and TODO.md § Cartes de stockage MAME-fidèles (P1).
//
// Ported from MAME src/devices/bus/a2bus/a2cffa.cpp (master). Cited line
// ranges below are approximate — re-pin against the exact revision on touch
// (TODO « MAME path drift refresher »).
//
//   $C0nX device-select  → read_c0nx / write_c0nx  : 8↔16-bit data latch + ATA
//   $CnXX slot ROM       → read_cnxx               : eeprom[off + slot*0x100]
//   $C800 expansion ROM  → read_c800 / write_c800  : eeprom[off + 0x800], WP-gated

#ifndef POM2_CFFA_CARD_H
#define POM2_CFFA_CARD_H

#include "AtaBlockDevice.h"
#include "ProDOSBlockCard.h"
#include "SlotPeripheral.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

namespace pom2 {

class CffaCard : public SlotPeripheral, public ProDOSBlockCard
{
public:
    static constexpr int    kDefaultSlot = 7; // conventional IDE/hard-disk slot
    static constexpr size_t kRomBytes    = 0x1000; // 4 KB EEPROM (MAME ROM_REGION)

    explicit CffaCard(int slot = kDefaultSlot) : slot_(slot) {}

    int getSlot() const override { return slot_; }

    /// Load the 4 KB CFFA firmware EEPROM dump. Must be exactly 4096 bytes.
    bool loadRom(const std::string& path);
    bool isRomLoaded() const { return romLoaded_; }

    /// Image management — forwarded to the ATA device's block backing so the
    /// HDV Library can mount .hdv/.2mg into the CFFA exactly like the HDV card.
    bool loadImage(const std::string& path) override;
    /// Two-phase mount, phase 2. Out-of-line and NOT a bare forward to the
    /// backing store: this is the PRIMARY mount path (pom2::mountBlockCard),
    /// so it owes the `ata_.reset()` `loadImage` does. Without it the ATA
    /// taskfile could stay in Phase::PioOut with the OLD lba_/sectorsLeft_,
    /// and the tail of the guest's interrupted write flushed into the image
    /// that had just been mounted.
    bool adoptImage(pom2::Block512Backing::PreparedImage&& p) override;
    bool loadImageFromBytes(std::vector<uint8_t> bytes, const std::string& label,
                            const std::string& hostFolder = std::string{}) override;
    bool ejectImage() override;
    bool detachImage(pom2::Block512Backing::PendingWriteBack& out) override
    {
        pom2::Block512Backing& b = ata_.backing();
        if (!(b.isLoaded() && b.hasUnsavedChanges() &&
              b.isWriteBackEnabled() && !b.isWriteProtected()))
            return true;
        out = b.takeWriteBack();
        return true;
    }
    void restoreDirtyBlocks(const std::vector<uint32_t>& indices) override
    { ata_.backing().restoreDirty(indices); }
    bool saveDirty() override;

    bool isImageLoaded()      const override { return ata_.backing().isLoaded(); }
    const std::string& getImagePath() const override { return ata_.backing().path(); }
    const std::string& getLastError() const override { return lastError_; }
    size_t getBlockCount()    const override { return ata_.backing().blockCount(); }
    bool isWriteProtected()   const override { return ata_.backing().isWriteProtected(); }
    bool isWriteBackEnabled() const override { return ata_.backing().isWriteBackEnabled(); }
    void setHostWriteProtected(bool on) override { ata_.backing().setHostWriteProtected(on); }
    void setWriteBackEnabled(bool on) override { ata_.backing().setWriteBackEnabled(on); }
    bool canWriteBack()       const override { return ata_.backing().canWriteBack(); }
    bool hasUnsavedChanges()  const override { return ata_.backing().hasUnsavedChanges(); }
    bool isBusy() const override { return ata_.backing().isBusy(); }
    void tickActivityDecay() override { ata_.backing().tickActivityDecay(); }

    std::string_view name() const override { return "CFFA 2.0"; }
    uint8_t deviceSelectRead (uint8_t low4) override;       // read_c0nx
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override; // write_c0nx
    uint8_t slotRomRead (uint8_t low8) override;            // read_cnxx
    uint8_t expansionRomRead (uint16_t offset) override;    // read_c800
    bool    takesC800() const override { return true; }     // MAME a2bus.h:145 take_c800
    void    expansionRomWrite(uint16_t offset, uint8_t v) override; // write_c800

    /// Snapshot/rewind: 'CFA1'-tagged blob wrapping the ATA chip state
    /// (taskfile + in-flight PIO) and the card latches. Media stays
    /// host-side; a rewind mid-transfer used to desync the 512-byte
    /// stream against the restored firmware cursor.
    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;
    void    onReset() override;

private:
    int slot_;
    std::array<uint8_t, kRomBytes> rom_{};
    bool romLoaded_ = false;
    std::string lastError_;

    AtaBlockDevice ata_;

    // 8↔16-bit data-port latch (a2cffa.cpp m_lastreaddata / m_lastdata).
    uint16_t lastReadData_  = 0;
    uint16_t lastWriteData_ = 0;
    // EEPROM write-enable (a2cffa.cpp m_writeprotect): default protected.
    bool writeProtect_ = true;
};

} // namespace pom2

#endif // POM2_CFFA_CARD_H
