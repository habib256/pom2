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
    static constexpr int kDrives      = 2;   // the port daisy-chains two

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

    void advanceCycles(int cycles) override;
    void onReset() override;
    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

    // ── MountableMediaCard ───────────────────────────────────────────────
    // Two fixed 3.5" bays, the daisy chain the real port carries. No type
    // select (a Liron drives 3.5" mechanisms and nothing else) and no
    // two-phase mount: a `Disk35Image` has no `Block512Backing` to prepare
    // off the lock, so the base class's opt-in defaults decline and callers
    // fall back to `mountBay` — the same path `SmartPortCard`'s 3.5" unit
    // takes today.
    int          bayCount() const override { return kDrives; }
    MediaBayInfo bayInfo(int bay) const override;
    bool         mountBay(int bay, const std::string& path,
                          std::string& errOut) override;
    bool         ejectBay(int bay) override;
    bool         flushBay(int bay, std::string& errOut) override;
    /// Two-phase flush — the bays hold 800 KB images, and `flushAll` runs
    /// under the machine lock. See MountableMediaCard::prepareFlushBay.
    bool         prepareFlushBay(int bay, PendingBayFlush& out,
                                 std::string& errOut) override;
    void         restoreFlushBayDirty(int bay) override;
    void         setBayWriteBack(int bay, bool on) override;

    /// True once the EPROM was found and loaded. Without it the card is
    /// inert — there is no synthetic fallback here on purpose: a synthesised
    /// ROM would be a different card, and POM2 already has that card
    /// (`SmartPortCard`).
    bool romLoaded() const { return romLoaded_; }
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
    std::array<Disk35Image, kDrives>   images_;
    std::array<Sony35Drive, kDrives>   drives_;

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
    class ImageUnit final : public SmartPortBusUnit {
    public:
        void bind(Disk35Image* img) { img_ = img; }
        bool     hasMedia()       const override { return img_ && img_->isLoaded(); }
        /// An empty bay has no blocks. Answering the 800K geometry with no
        /// media let a STATUS on an empty unit report a 1600-block volume the
        /// firmware could then try to read — every sibling unit
        /// (`SmartPort35Unit`, `SmartPortHdvUnit`) gates this on the medium.
        uint32_t blockCount()     const override
        { return hasMedia() ? Disk35Image::kBlockCount : 0u; }
        bool     writeProtected() const override { return !img_ || img_->isWriteProtected(); }
        bool readBlock (uint32_t b, uint8_t out[512]) override
        { return img_ && img_->readBlock(b, out); }
        bool writeBlock(uint32_t b, const uint8_t in[512]) override
        { return img_ && img_->writeBlock(b, in); }
    private:
        Disk35Image* img_ = nullptr;
    };
    bool busEnabled_ = true;
    mutable SmartPortBusDevice   bus_;
    mutable unsigned             busMediaMask_ = 0;   // which bays held media last look
    std::array<ImageUnit, kDrives> busUnits_;

    /// Enabled, and a bay holds media — the device is on the port.
    bool busLive() const;
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
