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

// ProDOSBlockCard — the image-management interface shared by the two HDV-class
// block-device cards: the synthetic ProDOSHardDiskCard (AppleWin lineage) and
// the MAME-faithful CffaCard (real firmware over an emulated ATA chip). The
// HDV Library, the disk-turbo poller, and settings persistence target a card
// through this interface so both kinds plug into the same UI uniformly.
//
// Both implementers are also SlotPeripherals; this is an orthogonal mix-in for
// the host (MainWindow) side, not the bus side.

#ifndef POM2_PRODOS_BLOCK_CARD_H
#define POM2_PRODOS_BLOCK_CARD_H

#include "Block512Backing.h"
#include "MountableMediaCard.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

class ProDOSBlockCard : public MountableMediaCard
{
public:
    virtual ~ProDOSBlockCard() = default;

    virtual int  getSlot() const = 0;

    virtual bool loadImage(const std::string& path) = 0;

    /// Phase 2 of the two-phase mount. Phase 1 is the static
    /// `Block512Backing::readImageFile`, which needs no card at all — it is
    /// pure file I/O and is what the caller runs WITHOUT `stateMutex`.
    ///
    /// Not to be confused with `loadImageFromBytes` below: that one is for
    /// SYNTHESISED volumes and is the wrong tool here, loudly. It skips the
    /// 2IMG header parse, forces `synth_`, and ties write-back to it — so a
    /// real .hdv/.2mg routed through it would mount with its header bytes
    /// treated as data, and with write-protect and write-back quietly wrong.
    virtual bool adoptImage(Block512Backing::PreparedImage&& prepared) = 0;
    virtual bool loadImageFromBytes(std::vector<uint8_t> bytes,
                                    const std::string& label,
                                    const std::string& hostFolder) = 0;
    virtual bool ejectImage() = 0;
    virtual bool saveDirty() = 0;
    virtual Block512Backing* blockBacking() { return nullptr; }
    virtual const Block512Backing* blockBacking() const { return nullptr; }

    /// Two-phase eject, phases 1 and 3 (see MountableMediaCard). Default is
    /// "not supported" rather than pure virtual so an implementor that has no
    /// Block512Backing under it keeps compiling and falls back to ejectImage.
    virtual bool detachImage(Block512Backing::PendingWriteBack& /*out*/)
    { return false; }
    virtual void restoreDirtyBlocks(const std::vector<uint32_t>& /*indices*/) {}

    virtual bool isImageLoaded() const = 0;
    virtual const std::string& getImagePath() const = 0;
    virtual const std::string& getLastError() const = 0;
    virtual size_t getBlockCount() const = 0;

    virtual bool isWriteProtected()   const = 0;
    virtual bool isWriteBackEnabled() const = 0;
    virtual void setWriteBackEnabled(bool on) = 0;
    /// Re-apply the notch (MediaNotch.h) on the mounted image after the
    /// user flipped the file's read-only bit. Default: no file, no notch.
    virtual void setHostWriteProtected(bool /*on*/) {}
    virtual bool canWriteBack()       const = 0;
    virtual bool hasUnsavedChanges()  const = 0;

    virtual bool isBusy() const = 0;
    virtual void tickActivityDecay() = 0;

    // ── MountableMediaCard: one fixed bay over the single-image API. ────
    // Both HDV-class cards (ProDOSHardDiskCard, CffaCard) implement the
    // pure virtuals above, so they gain the bay interface here for free —
    // the Slot Manager renders them generically alongside SmartPort units.
    int bayCount() const override { return 1; }

    MediaBayInfo bayInfo(int bay) const override
    {
        MediaBayInfo info;
        if (bay != 0) return info;
        info.loaded            = isImageLoaded();
        info.busy              = isBusy();
        info.path              = getImagePath();
        info.lastError         = getLastError();
        info.blockCount        = static_cast<uint32_t>(getBlockCount());
        info.writeProtected    = isWriteProtected();
        info.writeBackEnabled  = isWriteBackEnabled();
        info.hasUnsavedChanges = hasUnsavedChanges();
        info.supportsWriteBack = canWriteBack();
        if (auto* backing = blockBacking()) {
            info.persistenceState = backing->persistenceState();
            info.persistenceError = backing->persistenceError();
        }
        return info;
    }

    bool mountBay(int bay, const std::string& path, std::string& errOut) override
    {
        if (bay != 0) { errOut = "invalid bay"; return false; }
        if (!loadImage(path)) { errOut = getLastError(); return false; }
        return true;
    }

    /// Two-phase phase 2. Every ProDOSBlockCard has block backing, so this
    /// never reports "unsupported" — a false return is a real failure.
    bool adoptBay(int bay, pom2::Block512Backing::PreparedImage&& prepared,
                  std::string& errOut) override
    {
        if (bay != 0) { errOut = "invalid bay"; return false; }
        if (!adoptImage(std::move(prepared))) {
            errOut = getLastError();
            // adoptImage only fails for real reasons here; make sure the
            // caller never reads an empty string as "fall back and retry".
            if (errOut.empty()) errOut = "image could not be adopted";
            return false;
        }
        return true;
    }

    bool ejectBay(int bay) override { return bay == 0 && ejectImage(); }

    bool prepareEjectBay(int bay, pom2::Block512Backing::PendingWriteBack& out,
                         std::string& errOut) override
    {
        errOut.clear();
        if (bay != 0) return false;      // empty errOut → caller falls back
        return detachImage(out);
    }
    void restoreBayDirty(int bay,
                         const std::vector<uint32_t>& indices) override
    { if (bay == 0) restoreDirtyBlocks(indices); }
    void setBayWriteBack(int bay, bool on) override
    {
        if (bay == 0) setWriteBackEnabled(on);
    }
};

} // namespace pom2

#endif // POM2_PRODOS_BLOCK_CARD_H
