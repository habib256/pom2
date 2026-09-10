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

// SmartPortHdvUnit — block-level ProDOS HDV / 2MG image as a `SmartPortUnit`.
// A thin adapter over the shared `pom2::Block512Backing` (the same store the
// HDV-class cards use), so the 2IMG envelope parsing, dirty tracking, medium
// write-protect, and host-file write-back live in ONE tested place rather
// than being re-implemented per consumer. This unit only maps the
// SmartPortUnit interface onto that store so it can plug into a SmartPortCard
// chain alongside 3.5" units.
//
// Supports:
//   * Raw .hdv  — whole file is a stream of 512-byte ProDOS blocks
//   * .2mg      — 64-byte 2IMG header + ProDOS block data (format must = 1)
//
// Hard caps: ≥ 1 block, ≤ 65536 blocks (32 MB ProDOS-8 ceiling). The 2MG
// write-back path preserves the original header verbatim. Synth-from-folder
// volumes are deliberately NOT exposed here; for that, plug a separate
// `ProDOSHardDiskCard` (which keeps the existing `prodos_folder/` UX).

#ifndef POM2_SMARTPORT_HDV_UNIT_H
#define POM2_SMARTPORT_HDV_UNIT_H

#include "Block512Backing.h"
#include "SmartPortUnit.h"

#include <cstdint>
#include <string>
#include <utility>

namespace pom2 {

class SmartPortHdvUnit : public SmartPortUnit
{
public:
    static constexpr std::string_view kKindKey   = "hdv";
    static constexpr std::string_view kKindLabel = "ProDOS HDV";

    SmartPortHdvUnit();
    ~SmartPortHdvUnit() override;

    std::string_view kindKey()   const override { return kKindKey; }
    std::string_view kindLabel() const override { return kKindLabel; }

    bool     isLoaded()         const override { return backing_.isLoaded(); }
    /// The unit contract (`SmartPortUnit.h`): physically write-protected OR
    /// no write-back opt-in — what `SmartPort35Unit` answers through
    /// `Disk35Image`, and what `DiskImage` answers for the Disk II. This
    /// unit used to report the medium flag alone, so two bays of ONE card
    /// gave opposite answers to the same toggle (TODO.md R1 / G5-1): the
    /// 3.5" refused a write with write-back off, the HDV took it into RAM
    /// and dropped it at eject. The HDV-class *cards* (ProDOSHardDiskCard,
    /// CffaCard) keep their documented in-session-writable policy; inside
    /// a SmartPort card the rule is the card's.
    bool     isWriteProtected() const override
    { return backing_.isWriteProtected() || !backing_.isWriteBackEnabled(); }
    uint32_t blockCount() const override {
        return static_cast<uint32_t>(backing_.blockCount());
    }
    bool     readBlock (uint32_t idx, uint8_t* out) const override;
    bool     writeBlock(uint32_t idx, const uint8_t* in) override;

    bool     loadImage(const std::string& path) override;
    /// Two-phase mount, phase 2 — forwards to the backing store.
    bool     adoptImage(Block512Backing::PreparedImage&& p) override
    { return backing_.adoptImage(std::move(p)); }
    bool     detachImage(Block512Backing::PendingWriteBack& out) override
    {
        if (!(backing_.isLoaded() && backing_.hasUnsavedChanges() &&
              backing_.isWriteBackEnabled() && !backing_.isWriteProtected()))
            return true;                 // nothing to write: out stays invalid
        out = backing_.takeWriteBack();
        return true;
    }
    void     restoreDirtyBlocks(const std::vector<uint32_t>& indices) override
    { backing_.restoreDirty(indices); }
    bool     eject() override;
    const std::string& path()      const override { return backing_.path(); }
    const std::string& lastError() const override { return backing_.lastError(); }

    bool     isFileWriteProtected() const override { return backing_.isWriteProtected(); }
    void     setHostWriteProtected(bool on) override { backing_.setHostWriteProtected(on); }
    bool     isWriteBackEnabled() const override { return backing_.isWriteBackEnabled(); }
    void     setWriteBackEnabled(bool on) override { backing_.setWriteBackEnabled(on); }
    const Block512Backing* blockBacking() const override { return &backing_; }
    Block512Backing* blockBacking() override { return &backing_; }
    bool     saveDirty() override { return backing_.saveDirty(); }
    bool     hasUnsavedChanges() const override { return backing_.hasUnsavedChanges(); }

private:
    Block512Backing backing_;
};

} // namespace pom2

#endif // POM2_SMARTPORT_HDV_UNIT_H
