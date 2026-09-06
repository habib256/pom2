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

#include "LironCard.h"

#include "FloppySoundSink.h"
#include "Logger.h"
#include "ResourcePaths.h"

#include <cstdio>
#include <array>
#include <cstring>
#include <cstdlib>
#include <fstream>

namespace pom2 {

namespace {

// The BMOW / Yellowstone dump, whose layout the loader below assumes:
//   0x000-0x0FF  copyright string ("Firmware written by …"), NOT a slot page
//   0x100-0x7FF  seven $Cn00 pages, one per slot 1..7
//   0x800-0xFFF  the $C800-$CFFF expansion half
constexpr std::size_t kRomBytes      = 4096;
constexpr std::size_t kExpansionBase = 2048;
constexpr std::size_t kExpansionSize = 2048;

}  // namespace

LironCard::LironCard(int slot)
    : slot_(slot)
{
    for (int i = 0; i < kDrives; ++i) {
        drives_[i].setImage(&images_[i]);
        busUnits_[static_cast<std::size_t>(i)].bind(&images_[i]);
        bus_.setUnit(i, &busUnits_[static_cast<std::size_t>(i)]);
    }
    bus_.setUnitCount(kDrives);

    // MAME-shaped callbacks, minus the MIG. `SmartPortHub` does the same job
    // for the //c+; a Liron's chain is simpler, so the card is its own hub.
    iwm_.setPhasesCallback([this](uint8_t p) { onPhases(p); });
    iwm_.setDevselCallback([this](uint8_t d) { onDevsel(d); });
    iwm_.setSel35Callback([](bool) {});   // no MIG, nothing to route

    const std::string path = pom2::findResource("roms/liron.rom");
    if (path.empty()) {
        lastError_ = "roms/liron.rom not found";
        pom2::log().warn("Liron", lastError_ + " — card is inert");
        return;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        lastError_ = "cannot open " + path;
        pom2::log().warn("Liron", lastError_);
        return;
    }
    rom_.resize(kRomBytes);
    f.read(reinterpret_cast<char*>(rom_.data()),
           static_cast<std::streamsize>(kRomBytes));
    if (static_cast<std::size_t>(f.gcount()) != kRomBytes) {
        lastError_ = path + " is not a 4 KB dump";
        pom2::log().warn("Liron", lastError_);
        rom_.clear();
        return;
    }
    romLoaded_ = true;
    pom2::log().info("Liron",
        "Loaded real controller ROM (" + path + ") — slot " +
        std::to_string(slot_) + ", firmware-driven IWM");
}

LironCard::~LironCard()
{
    // Same contract as `~DiskIICard`: the card is the last owner of the
    // guest's writes, and `SlotBus` destroys it on every slot rebuild /
    // profile switch as well as at quit. `saveDirty` is a successful no-op
    // when write-back is off or nothing is dirty.
    //
    // LAST RESORT, and normally a no-op: every host path that destroys a card
    // flushes first (`StorageCoordinator::flushAll`, which since bug hunt #2
    // captures here and writes with the machine lock RELEASED), so reaching
    // this with something dirty means either a host that never flushed
    // (headless, tests, a crash-time teardown) or a guest write that landed
    // after the flush. Both are worth an inline 800 KB write — there is no
    // later — but neither is the common case, which is why the profile-switch
    // path no longer pays for it under `stateMutex`.
    for (int bay = 0; bay < kDrives; ++bay) {
        std::string err;
        if (!flushBay(bay, err) && !err.empty()) {
            pom2::log().warn("Liron", "Shutdown flush failed on bay " +
                             std::to_string(bay + 1) + ": " + err);
        }
    }
}

void LironCard::setFloppySound(FloppySoundSink* fs)
{
    for (auto& d : drives_) d.setFloppySound(fs);
}

// ── The bus ──────────────────────────────────────────────────────────────

bool LironCard::busAddressed() const
{
    // PH1 (CA1) and PH3 (LSTRB) both high, with the port enabled. That is
    // what $C800's scan asserts before polling SENSE, and holding LSTRB high
    // through a status read is something no disk transaction ever does — so
    // it is an unambiguous marker for "talking to the bus, not the disk".
    const uint8_t ph = iwm_.phases();
    return (ph & 0x02) && (ph & 0x08) && (iwm_.control() & 0x10);
}

bool LironCard::busLive() const
{
    // A bay changing state under a transaction (eject mid-WRITE, a mount
    // right after) must not leave half a frame in the responder to be
    // spliced with the next one: any change in which bays hold media starts
    // the protocol over.
    unsigned mask = 0;
    for (int i = 0; i < kDrives; ++i)
        if (images_[static_cast<std::size_t>(i)].isLoaded()) mask |= 1u << i;
    if (mask != busMediaMask_) {
        busMediaMask_ = mask;
        bus_.busReset();
    }
    return busEnabled_ && mask != 0;
}

uint8_t LironCard::deviceSelectRead(uint8_t low4)
{
    if (!romLoaded_) return 0xFF;
    iwm_.tick(cycles_);
    // SEL is sampled on every access rather than only on a devsel edge: the
    // firmware flips it ($C0nA / $C0nB) to pick the head *without* changing
    // the selected drive, and on a Sony that same line is bit 3 of the
    // register address. Reading a register with a stale SEL answers for the
    // wrong side of the disk.
    if (active_ >= 0) {
        drives_[active_].setSel(iwm_.sel());
        drives_[active_].ssW(iwm_.sel());
    }
    const uint8_t v = iwm_.read(low4);
    if (!busLive()) return v;
    // The phase pattern ADDRESSES the device; it does not gate every byte.
    // The firmware drops PH1 as soon as it starts reading the reply ($C982
    // `LDA $C081,X`), so a per-access gate on the handshake would hand the
    // rest of the packet back to an empty IWM. A transaction stays routed
    // from its first byte until the reply is consumed and REQ released.
    if (!busAddressed() && !bus_.active()) return v;

    switch (iwm_.control() & 0xC0) {
    case 0x00: {
        // Q6/Q7 both low = the data register in read mode: the device's next
        // byte, bit 7 meaning "there is one" — which is why every SmartPort
        // byte on the wire has it set. Nothing to say reads as $00.
        uint8_t b = 0;
        return bus_.hostReads(b) ? b : uint8_t{0x00};
    }
    case 0x80:
        // Write handshake. Bit 7 = "latch free, send the next byte"; bit 6 is
        // the underrun flag the firmware waits to see CLEAR after its last
        // byte ("has the shifter drained?"). POM2 takes bytes instantly and
        // has nothing in flight, so both answers are "yes, go on" — leaving
        // bit 6 set parks the firmware in the drain loop at $C92C for ever.
        return 0x80;
    case 0x40:
        // Status: bit 7 is SENSE, which on this bus is the device's ACK line
        // rather than a disk's write-protect. See SmartPortBusDevice for the
        // handshake it follows.
        return static_cast<uint8_t>((v & 0x7F) | (bus_.sense() ? 0x80 : 0x00));
    default:
        return v;
    }
}

void LironCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    if (!romLoaded_) return;
    iwm_.tick(cycles_);
    if (active_ >= 0) {
        drives_[active_].setSel(iwm_.sel());
        drives_[active_].ssW(iwm_.sel());
    }
    // The access itself moves Q6/Q7, so what makes a write a DATA write is
    // the control register AFTER it — which is how the IWM decides too
    // (controlAccess → dataW). Testing it before missed every byte whose own
    // access completed the Q6+Q7 pair, and that includes the packet's first
    // bytes: the sender establishes write mode with `STA $C08F,X` carrying
    // the first sync byte.
    // Decided BEFORE the access, on the state the byte arrives in: the
    // firmware establishes write mode with `STA $C08F,X` carrying the first
    // sync byte, so the state after the access is what tells a data write
    // from a mode write (the IWM's own test), but whether the byte is the
    // BUS's is known before it — PH1 + LSTRB up, or a transaction in flight.
    // Capture keeps it out of the shifter: bus bytes are not flux, and left
    // in they raised "write underrun" on every packet and, with write-back
    // on, ran a track decode per block over garbage (bug hunt 3).
    const bool forBus = busLive() && (busAddressed() || bus_.active());
    iwm_.setBusCapture(forBus);
    iwm_.write(low4, v);
    // …and only while the drive is enabled: an odd-offset write with Q6+Q7 is
    // a DATA byte when the device is active and the MODE register otherwise.
    if (forBus && !iwm_.isIdle() &&
        (iwm_.control() & 0xC0) == 0xC0 && (low4 & 1))
        bus_.hostWrote(v);
}

uint8_t LironCard::slotRomRead(uint8_t low8)
{
    if (!romLoaded_ || slot_ < 1 || slot_ > 7) return 0xFF;
    return rom_[static_cast<std::size_t>(slot_) * 256 + low8];
}

uint8_t LironCard::expansionRomRead(uint16_t offset)
{
    if (!romLoaded_ || offset >= kExpansionSize) return 0xFF;
    return rom_[kExpansionBase + offset];
}

void LironCard::advanceCycles(int cycles)
{
    if (!romLoaded_ || cycles <= 0) return;
    cycles_ += static_cast<uint64_t>(cycles);
    // Same reason EmulationController ticks the //c+'s IWM once a frame: the
    // drive-disable timer has to drain even while the firmware is off doing
    // something else, or the motor never stops.
    iwm_.tick(cycles_);
}

void LironCard::onReset()
{
    iwm_.reset();
    for (auto& d : drives_) d.reset();
    active_ = -1;
    bus_.reset();
    iwm_.setBusCapture(false);
    busMediaMask_ = 0;
    retargetIwm();
}

// ── Snapshot / rewind ────────────────────────────────────────────────────
// The IWM's registers and state machine, and the bus transaction in flight.
//
// The Sony MECHANISMS are carried too, as an optional tail (2026-09-06).
// The old note here said they had nothing to resume because the bus
// responder bypasses them — true for a SmartPort session, but the card also
// runs its bays as dumb drives, and there the restored IWM walked cells at a
// head position, side and motor state left over from the abandoned future
// (I/O ERROR until the firmware recalibrated). Only the mechanism travels:
// the MEDIA is never captured, the controller clears the rewind ring on a
// 3.5" write instead. Tail is length-prefixed per bay, so a blob without it
// (older build) still loads.

namespace {
constexpr uint8_t kLironBlobMagic[4] = { 'L', 'I', 'R', '1' };
}

void LironCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), kLironBlobMagic, kLironBlobMagic + 4);
    std::vector<uint8_t> iwm;
    iwm_.appendSnapshotState(iwm);
    for (int k = 0; k < 4; ++k) out.push_back(static_cast<uint8_t>(iwm.size() >> (8 * k)));
    out.insert(out.end(), iwm.begin(), iwm.end());
    bus_.appendSnapshotState(out);
    out.push_back(static_cast<uint8_t>(active_ + 1));   // -1 → 0
    out.push_back(static_cast<uint8_t>(busMediaMask_));
    for (int d = 0; d < kDrives; ++d) {
        std::vector<uint8_t> mech;
        drives_[static_cast<std::size_t>(d)].appendSnapshotState(mech);
        for (int k = 0; k < 4; ++k)
            out.push_back(static_cast<uint8_t>(mech.size() >> (8 * k)));
        out.insert(out.end(), mech.begin(), mech.end());
    }
}

void LironCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    // Identify the blob BEFORE touching the card. `onReset()` used to run
    // first, so a foreign or absent section (another card's SLOTn blob,
    // an older build's) wiped a live Liron mid-transaction instead of being
    // ignored — the contract MachineSnapshot documents is that a card
    // tolerates a blob it does not recognise by leaving itself alone.
    if (!data || len < 8 || std::memcmp(data, kLironBlobMagic, 4) != 0) return;
    onReset();
    std::size_t i = 4, iwmLen = 0;
    for (int k = 0; k < 4; ++k) iwmLen |= static_cast<std::size_t>(data[i + k]) << (8 * k);
    i += 4;
    if (iwmLen > len - i) return;
    if (iwmLen && !iwm_.loadSnapshotState(data + i, iwmLen)) { onReset(); return; }
    i += iwmLen;
    const std::size_t busLen = bus_.loadSnapshotState(data + i, len - i);
    if (busLen == 0) { onReset(); return; }
    i += busLen;
    if (i + 2 > len) { onReset(); return; }
    const int act = static_cast<int>(data[i++]) - 1;
    active_ = (act >= 0 && act < kDrives) ? act : -1;
    busMediaMask_ = data[i++];
    // Optional per-bay mechanism tail. Absent (older blob) → the live
    // mechanisms are left alone, which is the pre-2026-09-06 behaviour.
    for (int d = 0; d < kDrives && i + 4 <= len; ++d) {
        std::size_t mechLen = 0;
        for (int k = 0; k < 4; ++k)
            mechLen |= static_cast<std::size_t>(data[i + k]) << (8 * k);
        i += 4;
        if (mechLen > len - i) break;
        if (mechLen)
            drives_[static_cast<std::size_t>(d)].loadSnapshotState(data + i, mechLen);
        i += mechLen;
    }
    retargetIwm();
}

// ── The chain ────────────────────────────────────────────────────────────

void LironCard::onDevsel(uint8_t devsel)
{
    // EXPERIMENT: SEL is head select on this card, so devsel must not pick
    // the drive. Both enables land on the first bay.
    const int want = (devsel != 0) ? 0 : -1;
    if (want == active_) return;
    active_ = want;
    retargetIwm();
}

void LironCard::retargetIwm()
{
    // Unlike the //c+ hub there is no 5.25" path to protect here, so the
    // deselected case really does clear the IWM's floppy: a Liron with no
    // drive enabled must read as an empty spindle, not as whatever was
    // under the head last time.
    iwm_.setSony35(active_ >= 0 ? &drives_[active_] : nullptr);
    if (active_ >= 0) {
        drives_[active_].setSel(iwm_.sel());
        drives_[active_].ssW(iwm_.sel());
    }
}

void LironCard::onPhases(uint8_t phases)
{
    if (busLive()) {
        // An intelligent drive on the chain: the phase lines are the bus's
        // control lines, not a stepper's. PH0 is REQ; PH0 + PH2 together is
        // the bus reset the firmware issues before an INIT scan ($C9E5).
        if ((phases & 0x05) == 0x05) bus_.busReset();
        bus_.reqChanged((phases & 0x01) != 0);
        return;
    }
    // Every drive on the chain, selected or not. CA0-CA2 and LSTRB are wired
    // straight through to the connector, so a drive sees them whether or not
    // its /ENBL is asserted — and the firmware sets the register address up
    // BEFORE it enables the drive. Forwarding only to the active drive threw
    // that address away and left every sense read answering for register 0.
    for (auto& d : drives_) {
        d.setSel(iwm_.sel());
        d.seekPhaseW(phases, iwm_.emuCycles());
    }
}

// ── Media bays ───────────────────────────────────────────────────────────

MediaBayInfo LironCard::bayInfo(int bay) const
{
    MediaBayInfo info;
    if (bay < 0 || bay >= kDrives) return info;
    const Disk35Image& img = images_[static_cast<std::size_t>(bay)];
    info.kindLabel         = "3.5\" 800K";
    info.path              = img.path();
    info.lastError         = img.lastError();
    info.blockCount        = img.isLoaded() ? Disk35Image::kBlockCount : 0;
    info.loaded            = img.isLoaded();
    info.writeProtected    = img.isWriteProtected();
    info.writeBackEnabled  = img.isWriteBackEnabled();
    info.hasUnsavedChanges = img.hasUnsavedChanges();
    info.supportsWriteBack = true;
    return info;
}

bool LironCard::mountBay(int bay, const std::string& path, std::string& errOut)
{
    if (bay < 0 || bay >= kDrives) { errOut = "no such bay"; return false; }
    Disk35Image& img = images_[static_cast<std::size_t>(bay)];
    if (!img.loadFile(path)) {
        errOut = img.lastError();
        return false;
    }
    // The drive has to be told, or its cached bit-cell stream still holds the
    // previous disk — and the firmware's media-change probe never fires.
    drives_[static_cast<std::size_t>(bay)].notifyMediaChange();
    errOut.clear();
    return true;
}

bool LironCard::ejectBay(int bay)
{
    if (bay < 0 || bay >= kDrives) return false;
    Disk35Image& img = images_[static_cast<std::size_t>(bay)];
    // Refuse the eject when the save fails, exactly as every sibling does
    // (`DiskIICard::ejectDisk`, `SmartPort35Unit::eject`,
    // `EmulationController::eject35`): `Disk35Image::eject()` drops `blocks_`,
    // which is the ONLY copy of everything the guest wrote since the mount, so
    // ejecting anyway destroys it with a success return. Keeping the medium
    // mounted and dirty lets the user fix the cause and retry — or turn
    // write-back off, which makes `saveDirty` a successful no-op.
    if (img.hasUnsavedChanges() && !img.isWriteProtected() && !img.saveDirty())
        return false;
    img.eject();
    drives_[static_cast<std::size_t>(bay)].notifyMediaChange();
    return true;
}

bool LironCard::prepareFlushBay(int bay, PendingBayFlush& out,
                                std::string& errOut)
{
    errOut.clear();
    out = PendingBayFlush{};
    if (bay < 0 || bay >= kDrives) { errOut = "no such bay"; return false; }
    Disk35Image& img = images_[static_cast<std::size_t>(bay)];
    // The move half of "move out": `takeWriteBack` serialises the whole file
    // and retires the dirty flag under the caller's lock, atomically with the
    // capture. Nothing dirty (or write-back off) is a successful no-op with
    // `valid == false`, exactly as `flushBay` returning true is.
    Disk35Image::PendingWriteBack pending = img.takeWriteBack();
    out.valid = pending.valid;
    out.path  = std::move(pending.path);
    out.bytes = std::move(pending.bytes);
    return true;
}

void LironCard::restoreFlushBayDirty(int bay)
{
    if (bay < 0 || bay >= kDrives) return;
    images_[static_cast<std::size_t>(bay)].restoreDirty();
}

bool LironCard::flushBay(int bay, std::string& errOut)
{
    errOut.clear();
    if (bay < 0 || bay >= kDrives) { errOut = "no such bay"; return false; }
    Disk35Image& img = images_[static_cast<std::size_t>(bay)];
    if (!img.isLoaded() || !img.hasUnsavedChanges()) return true;
    if (!img.saveDirty()) {
        errOut = img.lastError();
        if (errOut.empty()) errOut = "3.5-inch write-back failed";
        return false;
    }
    return true;
}

void LironCard::setBayWriteBack(int bay, bool on)
{
    if (bay < 0 || bay >= kDrives) return;
    images_[static_cast<std::size_t>(bay)].setWriteBackEnabled(on);
}

}  // namespace pom2
