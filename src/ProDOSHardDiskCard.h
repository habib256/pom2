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

// ProDOSHardDiskCard — minimal ProDOS block-device card for raw .hdv images.
// Slot 5 by convention: ProDOS gives SmartPort / hard-disk devices there
// priority over the Disk II in slot 6 while leaving video cards in slot 7.
//
// The card exposes Apple's ProDOS disk ID bytes in its slot ROM:
//
//   $Cn01 = $20, $Cn03 = $00, $Cn05 = $03
//   $CnFE = $07       status + read + WRITE (ProDOS 8 TN.PDOS.021 bit 2).
//                     It used to read $03, which advertises a READ-ONLY
//                     device — untrue of a card whose ROM has always
//                     carried a working WRITE_BLOCK.
//   $CnFF             ProDOS block driver entry offset (derived from the
//                     assembled layout, not typed — see buildRom)
//
// The 6502 ROM translates the standard ProDOS device parameter block
// ($42 command, $43 unit, $44/$45 buffer, $46/$47 block) into a byte-stream
// read from three soft switches in the slot's device-select window.
//
// This is the SYNTHETIC-block model (AppleWin HardDisk.cpp lineage) — a
// deliberate divergence from the MAME-faithful CffaCard (see DEV.md). Its
// storage now lives in the shared pom2::Block512Backing (extracted 2026-05-24),
// which CffaCard / AtaBlockDevice also use. Going forward this card's role is
// ProDOS volume management + host-folder ↔ ProDOS bridging accessible from
// POM2, while CffaCard owns hardware fidelity.

#ifndef POM2_PRODOS_HARD_DISK_CARD_H
#define POM2_PRODOS_HARD_DISK_CARD_H

#include "Block512Backing.h"
#include "ProDOSBlockCard.h"
#include "SlotPeripheral.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

class ProDOSHardDiskCard : public SlotPeripheral, public pom2::ProDOSBlockCard
{
public:
    static constexpr int    kDefaultSlot = 5;
    static constexpr size_t kBlockBytes  = pom2::Block512Backing::kBlockBytes;

    /// Construct with the slot number this card will be plugged into.
    /// The slot is baked into the slot ROM (signature byte $Cn02, ProDOS
    /// driver entry $Cn50, soft-switch addresses inside the read/write
    /// trampolines) so changing it after construction would require
    /// rebuilding the ROM.
    explicit ProDOSHardDiskCard(int slot = kDefaultSlot);

    int getSlot() const override { return slot; }

    /// True when the hand-assembled slot ROM did not fit its declared layout.
    /// Always false in a healthy build — `hdv_rom_layout` asserts it.
    bool romLayoutError() const { return romLayoutError_; }

    bool loadImage(const std::string& path) override;
    /// Two-phase mount, phase 2. Out-of-line and NOT a bare forward to the
    /// backing store: this is the PRIMARY mount path (pom2::mountBlockCard),
    /// so it owes everything `loadImage` does — including resetting the
    /// firmware's block/byte cursor. Leaving them meant a mount that landed
    /// mid-transfer kept the outgoing image's cursor over the incoming one.
    bool adoptImage(pom2::Block512Backing::PreparedImage&& p) override;
    /// Replace the in-memory image with synthesised bytes (e.g. produced by
    /// pom2::buildVolumeFromFolder). `label` is what the UI shows; it does
    /// not have to be a real filesystem path. `hostFolder`, when non-empty,
    /// flags the volume as a synth from a host folder so save-on-eject can
    /// decode the modified volume back into that folder. Returns false if
    /// `bytes` is empty or not a multiple of 512.
    bool loadImageFromBytes(std::vector<uint8_t> bytes,
                            const std::string& label,
                            const std::string& hostFolder = std::string{}) override;
    bool ejectImage() override;
    /// Two-phase eject (MountableMediaCard). MOVES out the payload the
    /// save-on-eject policy in ejectImage() would write — dirty flags are
    /// retired at capture (Block512Backing::takeWriteBack), so writes racing
    /// the unlocked commit keep theirs — leaving the medium mounted;
    /// restoreDirtyBlocks() puts the captured flags back if the commit fails.
    bool detachImage(pom2::Block512Backing::PendingWriteBack& out) override
    {
        if (!(backing_.isLoaded() && backing_.hasUnsavedChanges() &&
              backing_.isWriteBackEnabled() && !backing_.isMediumLocked()))
            return true;                 // nothing to write: out stays invalid
        out = backing_.takeWriteBack();
        return true;
    }
    void restoreDirtyBlocks(const std::vector<uint32_t>& indices) override
    { backing_.restoreDirty(indices); }

    bool isImageLoaded() const override { return backing_.isLoaded(); }
    const std::string& getImagePath() const override { return backing_.path(); }
    const std::string& getLastError() const override { return backing_.lastError(); }
    size_t getBlockCount() const override { return backing_.blockCount(); }

    /// Hardware write-protect, as seen by the emulated ProDOS driver — reflects
    /// ONLY the real medium's WP state (the 2MG header flag), NOT the host-file
    /// write-back preference. See Block512Backing.
    bool isWriteProtected()  const override { return backing_.isWriteProtected(); }
    /// User opt-in for persisting RAM writes back to the host .hdv/.2mg file.
    /// Default off — the in-session volume is fully writable either way.
    bool isWriteBackEnabled() const override { return backing_.isWriteBackEnabled(); }
    void setHostWriteProtected(bool on) override { backing_.setHostWriteProtected(on); }
    void setWriteBackEnabled(bool on) override { backing_.setWriteBackEnabled(on); }
    bool canWriteBack()       const override { return backing_.canWriteBack(); }
    bool hasUnsavedChanges()  const override { return backing_.hasUnsavedChanges(); }
    bool isSynthVolumeMounted() const { return backing_.isSynthVolume(); }

    /// Recent block-I/O activity, used by MainWindow's auto-turbo (forwarded
    /// to the shared backing's busy signal).
    bool isBusy() const override { return backing_.isBusy(); }
    void tickActivityDecay() override { backing_.tickActivityDecay(); }

    /// Persist all dirty 512-byte blocks back to the source file (.hdv/.2mg)
    /// preserving the 2MG container header verbatim, OR for synth volumes,
    /// decode the modified volume back to the host folder.
    std::vector<pom2::Block512Backing*> blockBackings() override { return {&backing_}; }
    const pom2::Block512Backing* blockBacking() const override { return &backing_; }
    pom2::Block512Backing* blockBacking() override { return &backing_; }
    bool saveDirty() override { return backing_.saveDirty(); }

    /// Direct backing access for ProDOS volume management features (host-folder
    /// bridging, library tooling). Hardware paths use the methods above.
    pom2::Block512Backing&       backing()       { return backing_; }
    const pom2::Block512Backing& backing() const { return backing_; }

    std::string_view name() const override { return "ProDOS HDV"; }
    uint8_t deviceSelectRead(uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead(uint8_t low8) override;

    /// Snapshot/rewind: 'HDV1'-tagged blob carrying the selected block +
    /// the byte cursor within it. A rewind mid-transfer used to leave the
    /// live cursor under the restored firmware's loop — the rest of the
    /// 512-byte stream came out of the wrong offset.
    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;
    void    onReset() override;

private:
    int slot;
    std::array<uint8_t, 256> rom{};
    /// Set by buildRom() when a hand-assembled region overran its budget or
    /// stopped ending where the dispatch table's branch offsets assume.
    bool                     romLayoutError_ = false;
    pom2::Block512Backing backing_;

    uint16_t selectedBlock = 0;
    size_t   streamOffset  = 0;  // byte offset within the selected 512-byte block

    void buildRom();
    uint8_t readDataByte();
    void    writeDataByte(uint8_t v);
};

#endif // POM2_PRODOS_HARD_DISK_CARD_H
