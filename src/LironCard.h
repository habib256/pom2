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

// LironCard — the Apple II 3.5" Disk Controller (Apple 670-0186, "Liron"),
// running its OWN firmware over a real IWM.
//
// POM2 already had a card called SmartPortCard on this hardware's name, and
// the two are not rivals: that one answers ProDOS's block calls from the
// host with an invented streaming port, borrowing only the Liron dump's
// identity bytes. It works, it needs no bit-cell emulation, and it is the
// right thing for a user who wants a 3.5" volume on a //e. This is the other
// half — the card as silicon:
//
//   * the real 4 KB EPROM (`roms/liron.rom`, the BMOW/Yellowstone dump)
//     executing on the 6502, both its $Cn00 page and its $C800 half;
//   * a real `IWMDevice` behind $C0nX, which the firmware drives itself;
//   * `Sony35Drive` mechanisms with zoned GCR under the head.
//
// Nothing is served from the host: ProDOS boots because the firmware read
// the sectors. That is the whole point, and it is only worth having because
// it is now possible — the IWM's bit-cell walker could not recover a Sony
// sector until 2026-09-01 (see `sony35_iwm_read_path`), so a card written
// before that would have been a card that did not boot.
//
// **Wiring, and the one thing that differs from the //c+.** On a //c+ the
// MIG gate array selects the drive and drives head-select. A Liron has no
// MIG: the IWM's own SEL line (control bit 5, $C0nA/$C0nB) is head select,
// and it is also bit 3 of the Sony's register address — `regSelect()` is
// `{ HDSEL, CA2, CA1, CA0 }`. So SEL is forwarded to `Sony35Drive::ssW`
// here, where `SmartPortHub` forwards the MIG's $C240/$C260 instead.
//
// Deliberately out of scope, as TODO § Storage has always said: the UniDisk
// 3.5's drive-side 65C02. This card drives the *dumb* Apple 3.5 Drive, which
// is what the firmware's GCR path talks to; an intelligent UniDisk would
// need its own processor emulated inside the drive.

#ifndef POM2_LIRON_CARD_H
#define POM2_LIRON_CARD_H

#include "Disk35Image.h"
#include "IWMDevice.h"
#include "MountableMediaCard.h"
#include "SlotPeripheral.h"
#include "SmartPortBusDevice.h"
#include "Sony35Drive.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class FloppySoundSink;

namespace pom2 {

class LironCard : public SlotPeripheral, public MountableMediaCard
{
public:
    static constexpr int kDefaultSlot = 5;
    /// Sony mechanisms behind the IWM — the dumb-drive path, which only the
    /// bus-responder-off configuration reaches (`setBusResponderEnabled`).
    static constexpr int kDrives      = 2;
    /// Units on the SmartPort BUS, the path the firmware actually boots
    /// from: up to FOURTEEN (2026-09-11) — the most ProDOS 8 can list. The
    /// firmware's INIT scan numbers sixteen without complaint; ProDOS 8 2.4
    /// fills its 14-entry device table and stops (`liron_chain`).
    static constexpr int kMaxUnits    = 14;
    static_assert(kMaxUnits <= SmartPortBusDevice::kMaxUnits,
                  "the bus responder must carry the whole chain");
    /// The whole chain by default: a fresh Liron answers for every unit it
    /// can carry. A count saved under `media_slotN_bays` still wins; 2 is one
    /// click away in the media panel for a ProDOS older than 2.4, which does
    /// not remap units 3+ anyway.
    static constexpr int kDefaultUnits = kMaxUnits;

    explicit LironCard(int slot = kDefaultSlot);

    /// Flushes both bays' write-back, like `~DiskIICard`. A card is destroyed
    /// by `SlotBus::plug`/`unplug` on every slot rebuild and profile switch,
    /// and by the machine teardown at quit; without this the medium's dirty
    /// blocks died with the object and the remount read the untouched file
    /// back, reverting everything the guest had written since the mount.
    ~LironCard() override;

    // ── SlotPeripheral ───────────────────────────────────────────────────
    std::string_view name() const override { return "Apple II 3.5\" (Liron)"; }

    /// $C0nX — the IWM, all sixteen registers, no interception. The
    /// firmware's mode/status/handshake dance is its own business.
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;

    /// $Cn00 — the dump's per-slot page. The EPROM carries eight of them
    /// (offsets 0x100..0x7FF) that differ only in the slot number they load
    /// into X, so the firmware knows where it lives without self-modifying
    /// code. Offset 0x000 is a copyright string, not a page.
    uint8_t slotRomRead(uint8_t low8) override;

    /// $C800-$CFFF — the upper 2 KB of the dump, where the GCR routines and
    /// the SmartPort dispatcher live.
    uint8_t expansionRomRead(uint16_t offset) override;
    /// The upper 2 KB of the Liron EPROM IS the /IOSTB window, so the card
    /// takes it — MAME `a2bus.h:145` `take_c800()` defaults to false and
    /// only a card with an expansion ROM overrides it (`a2ssc.cpp:50` etc.).
    bool takesC800() const override { return true; }

    void advanceCycles(int cycles) override;
    void onReset() override;
    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

    // ── MountableMediaCard ───────────────────────────────────────────────
    // One bay per unit on the daisy chain: 2 to 14 of them (`setUnitCount`).
    // A unit is whatever disk is put in it (2026-09-11): a 3.5" 800K image
    // (`Disk35Image` — the UniDisk 3.5 the chain was built for, and the only
    // kind the dumb Sony path behind bays 0-1 can read) or a ProDOS hard-disk
    // image up to 32 MiB (`Block512Backing` — a SmartPort hard disk on the
    // same bus, which is all a block device is to this firmware). No type
    // select: the FILE decides — a `.woz`, or anything that loads as an 800K
    // 3.5" image, is a 3.5"; everything else is a hard disk. Hard disks take
    // the two-phase mount, eject and the background autosave; 3.5" bays keep
    // the one-phase mount and the two-phase whole-file flush they had.
    int          bayCount() const override { return unitCount_; }
    std::vector<int> bayCountChoices() const override
    { return { 2, 4, 6, 8, 10, 12, 14 }; }
    void         setBayCount(int n) override { setUnitCount(n); }
    MediaBayInfo bayInfo(int bay) const override;
    bool         mountBay(int bay, const std::string& path,
                          std::string& errOut) override;
    /// Two-phase mount, phase 2 — hard disks only. An image the size of an
    /// 800K disk is declined with an empty error, so the caller falls back to
    /// `mountBay` and it mounts as a 3.5" (the kind the dumb path can read).
    bool         adoptBay(int bay, Block512Backing::PreparedImage&& prepared,
                          std::string& errOut) override;
    bool         ejectBay(int bay) override;
    /// Two-phase eject — hard disks only (3.5" bays decline: empty error).
    bool         prepareEjectBay(int bay, Block512Backing::PendingWriteBack& out,
                                 std::string& errOut) override;
    void         restoreBayDirty(int bay,
                                 const std::vector<uint32_t>& indices) override;
    bool         flushBay(int bay, std::string& errOut) override;
    /// Two-phase flush — 3.5" bays (800 KB whole-file images), and `flushAll`
    /// runs under the machine lock. A hard disk declines (empty error) and is
    /// flushed inline, its dirty blocks only, as every block card's is — and
    /// the background autosave has normally committed them already.
    bool         prepareFlushBay(int bay, PendingBayFlush& out,
                                 std::string& errOut) override;
    void         restoreFlushBayDirty(int bay) override;
    void         setBayWriteBack(int bay, bool on) override;
    void         setBayHostWriteProtected(int bay, bool on) override;

    /// The hard-disk bays, index = bay, for the background autosave
    /// (`EmulationController::pollBlockWriteBacks`). A 3.5" bay's entry is an
    /// unloaded backing, which the executor skips.
    std::vector<Block512Backing*> blockBackings() override;

    /// True once the EPROM was found and loaded. Without it the card is
    /// inert — there is no synthetic fallback here on purpose: a synthesised
    /// ROM would be a different card, and POM2 already has that card
    /// (`SmartPortCard`).
    bool romLoaded() const { return romLoaded_; }

    /// How many units the firmware's INIT scan finds on the chain — 2 to 14
    /// (odd counts round up: ProDOS drives come in pairs), exactly
    /// `SmartPortCard::setUnitCount`'s rule. The guest sees a change at its
    /// next INIT, i.e. the next boot; ProDOS 8 2.4+ remaps units 3+ onto
    /// slots with no disk device of their own. Configuration, not state: the
    /// snapshot does not carry it, as the SmartPort card's does not.
    ///
    /// Shrinking does not eject: a bay past the new count keeps its medium,
    /// unseen by the guest but still flushed by the destructor. The host
    /// (`StorageCoordinator::setMediaBayCount`) refuses to shrink over a
    /// loaded bay, so that state is only reachable by hand.
    void setUnitCount(int n);
    int  unitCount() const { return unitCount_; }
    const std::string& lastError() const { return lastError_; }

    /// The byte-level SmartPort **bus** responder (`SmartPortBusDevice`).
    ///
    /// ON by default: with it, the real firmware finds an intelligent device
    /// on its port, enumerates it, and boots from it — which is what a Liron
    /// with a UniDisk 3.5 attached does. It only answers while a bay holds
    /// media; an empty chain is silence on the wire, the scan reports $28
    /// and a //c-class autostart falls through to its internal drive. Off,
    /// the card is a Liron with nothing plugged in, and the dumb Sony models
    /// behind the IWM see the phase lines instead (`POM2_TRACE_SMARTPORT_BUS
    /// =1` prints every byte in both directions either way).
    void setBusResponderEnabled(bool on) { busEnabled_ = on; }
    bool busResponderEnabled() const { return busEnabled_; }

    /// How far the last bus exchange got, for tests and diagnostics.
    using BusProgress = SmartPortBusDevice::Progress;
    BusProgress busProgress() const { return bus_.progress(); }

    /// Mechanical sound sink, shared with the rest of the 3.5" stack.
    void setFloppySound(FloppySoundSink* fs);

    int slot() const { return slot_; }

    // Diagnostics for tests and the inspector.
    const IWMDevice&   iwm()      const { return iwm_; }
    const Sony35Drive& drive(int i) const { return drives_[i]; }

private:
    int  slot_       = kDefaultSlot;
    bool romLoaded_  = false;
    std::string lastError_;
    std::vector<uint8_t> rom_;          // the 4 KB dump, verbatim

    IWMDevice                          iwm_;
    std::array<Disk35Image, kMaxUnits> images_;   // a bay's 3.5" medium…
    std::array<Block512Backing, kMaxUnits> blocks_; // …or its hard disk; never both
    std::array<Sony35Drive, kDrives>   drives_;   // bays 0-1 only
    int                                unitCount_ = kDefaultUnits;

    // ── The SmartPort bus ────────────────────────────────────────────────
    // The firmware's device scan is not talking to a disk: it drives PH1 and
    // LSTRB high, then exchanges BYTES through the IWM's data register with
    // an intelligent device (a UniDisk 3.5 carries its own 65C02). POM2 has
    // no such drive and will not emulate that processor; `SmartPortBusDevice`
    // answers the protocol instead, at the byte level, which is the same seam
    // `SmartPortCard` already uses one layer up (docs/lle_vs_hle.md). This
    // card owns the IWM registers, so it is the one that decides which
    // accesses are the bus's: PH1 + LSTRB high with the port enabled is what
    // the scan asserts and no disk transaction ever does, and a transaction
    // once begun stays routed until its reply is consumed.
    /// One unit on the bus: the bay's 3.5" image or its hard disk, whichever
    /// is loaded (the card keeps them exclusive).
    class ImageUnit final : public SmartPortBusUnit {
    public:
        void bind(Disk35Image* img, Block512Backing* blk) { img_ = img; blk_ = blk; }
        bool hasMedia() const override { return disk() || hard(); }
        /// An empty bay has no blocks. Answering the 800K geometry with no
        /// media let a STATUS on an empty unit report a 1600-block volume the
        /// firmware could then try to read — every sibling unit
        /// (`SmartPort35Unit`, `SmartPortHdvUnit`) gates this on the medium.
        uint32_t blockCount() const override
        {
            if (disk()) return Disk35Image::kBlockCount;
            return hard() ? static_cast<uint32_t>(blk_->blockCount()) : 0u;
        }
        bool writeProtected() const override
        {
            if (disk()) return img_->isWriteProtected();
            return !hard() || blk_->isWriteProtected();
        }
        bool readBlock(uint32_t b, uint8_t out[512]) override
        {
            if (disk()) return img_->readBlock(b, out);
            return hard() && blk_->readBlock(b, out);
        }
        bool writeBlock(uint32_t b, const uint8_t in[512]) override
        {
            if (disk()) return img_->writeBlock(b, in);
            return hard() && blk_->writeBlock(b, in);
        }
    private:
        bool disk() const { return img_ && img_->isLoaded(); }
        bool hard() const { return blk_ && blk_->isLoaded(); }
        Disk35Image*     img_ = nullptr;
        Block512Backing* blk_ = nullptr;
    };
    bool busEnabled_ = true;
    mutable SmartPortBusDevice   bus_;
    mutable unsigned             busMediaMask_ = 0;   // which bays held media last look
    std::array<ImageUnit, kMaxUnits> busUnits_;

    /// Enabled, and a bay holds media — the device is on the port.
    bool busLive() const;
    bool bayLoaded(int bay) const
    {
        const auto b = static_cast<std::size_t>(bay);
        return images_[b].isLoaded() || blocks_[b].isLoaded();
    }
    /// Save and drop whichever medium `bay` holds; false (medium kept) when
    /// the save fails, like every sibling eject.
    bool dropBay(int bay);
    /// PH1 + LSTRB high with the port enabled: the host is addressing the
    /// bus, not a drive.
    bool busAddressed() const;

    /// Which drive the IWM's devsel currently points at, or -1 for none.
    /// The Liron's port is a daisy chain: devsel 1 is the first drive, 2 the
    /// second, and 0 means the IWM has dropped both (motor-off drain).
    int      active_ = -1;
    uint64_t cycles_ = 0;

    void onPhases(uint8_t phases);
    void onDevsel(uint8_t devsel);
    void retargetIwm();
};

}  // namespace pom2

#endif  // POM2_LIRON_CARD_H
