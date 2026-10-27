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

#include "SmartPortCard.h"
#include <cstdio>
#include <cstdlib>
#include "SlotRomAsm.h"

#include "FloppySoundSink.h"
#include "Logger.h"
#include "SmartPort35Unit.h"
#include "SmartPortHdvUnit.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string_view>

namespace pom2 {

namespace {

constexpr uint8_t kBootOff   = 0x20;
constexpr uint8_t kDriverOff = 0x50;
constexpr uint8_t kReadOff   = 0x6F;
constexpr uint8_t kWriteOff  = 0xA2;
/// End of the write routine. $CnE0-$CnE2 held the boot-failure halt loop
/// until 2026-10-16; boot failure now continues the autostart scan instead
/// (see `bootErr` in buildRom), and the three bytes are NOP padding.
constexpr uint8_t kHaltOff   = 0xE0;

/// The ProDOS media pre-flight and STATUS routines live in the $C800 bank,
/// not in the slot page.
///
/// The slot page has been over budget since the write routine grew: boot (36)
/// + dispatch (31) + read (51) + write (62) + halt (3) + STATUS (25) is 208
/// bytes for the 195 that $Cn20-$CnE2 actually holds, which is how the write
/// routine came to overrun STATUS at $CnC0 in silence. And the gaps that look
/// free are not: with roms/liron.rom loaded, $Cn13-$Cn1F and $CnE3-$CnFF are
/// deliberately left as the REAL dump's identity bytes, so a routine placed
/// there works on the synthetic base and executes real Liron firmware on the
/// authentic one.
///
/// The $C800 bank has 1.5 KB free and is already where the SmartPort handler
/// lives ($CE00), reached exactly this way. Executing the dispatch in the slot
/// page is itself what points $C800 at this slot (SlotBus::slotRomRead sets
/// the active expansion slot), so these are directly callable from there.
constexpr size_t   kPreflightC800 = 0x500;      // → $CD00
constexpr size_t   kStatusC800    = 0x510;      // → $CD10
constexpr uint16_t kPreflightAddr = 0xC800 + kPreflightC800;
constexpr uint16_t kStatusAddr    = 0xC800 + kStatusC800;
/// Boot failure: "was this slot entered from the //e autostart scan?" —
/// continues the scan if so, else restores CSW/KSW and returns. $CD30.
constexpr size_t   kBootScanC800  = 0x530;
constexpr uint16_t kBootScanAddr  = 0xC800 + kBootScanC800;
/// Any ProDOS driver command other than STATUS/READ/WRITE: FORMAT, else $27.
/// $CD60.
constexpr size_t   kOtherCmdC800  = 0x560;
constexpr uint16_t kOtherCmdAddr  = 0xC800 + kOtherCmdC800;
/// ProDOS unit byte ($43) → bay index, latched in $C0n0. $CD80.
constexpr size_t   kUnitSelC800   = 0x580;
constexpr uint16_t kUnitSelAddr   = 0xC800 + kUnitSelC800;

} // anon namespace

// $C0n0 unit-select maps the written value with `% kMaxUnits` (deviceSelectWrite
// case 0x0); that is only well-defined for a non-zero unit count.
static_assert(SmartPortCard::kMaxUnits >= 1, "kMaxUnits must be >= 1");

SmartPortCard::SmartPortCard(int slot)
    : slot_(slot)
{
    selectedBlock_.fill(0);
    streamOffset_.fill(0);
    readCacheValid_.fill(false);
    readCacheBlock_.fill(0xFFFF);
    writeBufPrimed_.fill(false);
    buildRom();
}

void SmartPortCard::setSuperDriveMode(bool enabled)
{
    superDriveMode_ = enabled;
    for (auto& unit : units_)
        if (auto* disk = dynamic_cast<SmartPort35Unit*>(unit.get()))
            disk->setMfmCapable(enabled);
}

bool SmartPortCard::loadLironRom(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        pom2::log().warn("SmartPort", "Cannot open Liron ROM: " + path);
        return false;
    }
    std::vector<uint8_t> bytes(4097);
    f.read(reinterpret_cast<char*>(bytes.data()),
           static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<size_t>(f.gcount()));
    if (bytes.size() != 4096) {
        pom2::log().warn("SmartPort",
            "Liron ROM " + path + " has unexpected size " +
            std::to_string(bytes.size()) + " B (expected 4096) — ignored");
        return false;
    }
    lironRom_    = std::move(bytes);
    lironLoaded_ = true;
    buildRom();   // re-base the slot page on the real dump
    pom2::log().info("SmartPort",
        "Loaded real Liron controller ROM (" + path + ") — slot " +
        std::to_string(slot_) + " presents authentic identity ($Cn07=$00 "
        "SmartPort class, $CnFE=$BF, ProDOS entry $Cn0A)");
    return true;
}

std::unique_ptr<SmartPortUnit>
SmartPortCard::setUnit(size_t idx, std::unique_ptr<SmartPortUnit> u)
{
    if (idx >= kMaxUnits) return u;     // index out of range — return back
    // Reset any in-flight transfer state for this slot so the new unit
    // doesn't inherit a half-streamed block from the old one.
    selectedBlock_[idx]  = 0;
    streamOffset_[idx]   = 0;
    readCacheValid_[idx] = false;
    readCacheBlock_[idx] = 0xFFFF;
    writeBufPrimed_[idx] = false;
    borrowedKind_[idx].reset();
    auto old = std::move(units_[idx]);
    units_[idx] = std::move(u);
    if (auto* disk = dynamic_cast<SmartPort35Unit*>(units_[idx].get()))
        disk->setMfmCapable(superDriveMode_);
    return old;
}

void SmartPortCard::onReset()
{
    activeUnit_ = 0;
    selectedBlock_.fill(0);
    streamOffset_.fill(0);
    readCacheValid_.fill(false);
    readCacheBlock_.fill(0xFFFF);
    writeBufPrimed_.fill(false);
    ioError_.fill(false);
    if (audibleMotorOn_ && sound_) sound_->motor(false, true);
    audibleMotorOn_ = false;
    lastAccessCycle_ = 0;
}

namespace {
// FNV-1a 64 of a unit's image path — the media identity stamped next to the
// per-unit transfer state (v2). Same construction as DiskIICard's; an empty
// path (no media) hashes to the basis and compares equal across an
// empty→empty restore, which is what we want.
uint64_t spMediaIdentity(const SmartPortUnit* u)
{
    uint64_t h = 14695981039346656037ULL;
    if (u) {
        for (unsigned char c : u->path()) { h ^= c; h *= 1099511628211ULL; }
    }
    return h;
}
}  // namespace

void SmartPortCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.push_back('S'); out.push_back('P'); out.push_back(kSnapVersion);
    out.push_back(static_cast<uint8_t>(activeUnit_));
    for (size_t u = 0; u < kMaxUnits; ++u) {
        out.push_back(static_cast<uint8_t>(selectedBlock_[u]));
        out.push_back(static_cast<uint8_t>(selectedBlock_[u] >> 8));
        out.push_back(static_cast<uint8_t>(streamOffset_[u]));
        out.push_back(static_cast<uint8_t>(streamOffset_[u] >> 8));
        out.push_back(writeBufPrimed_[u] ? 1 : 0);
        out.push_back(ioError_[u] ? 1 : 0);
        out.insert(out.end(), writeBuf_[u].begin(), writeBuf_[u].end());
    }
    // v1.1 tail: the SmartPort-protocol call engine ($Cn0D → $CE00
    // handler). Omitting it left a rewind mid-STATUS/READ resuming with
    // the LIVE result stream and collect buffer — the restored firmware
    // pulled the wrong payload bytes out of reg 0x9. Old blobs simply end
    // before this tail (loader treats it as optional).
    out.insert(out.end(), spCollect_.begin(), spCollect_.end());
    out.push_back(static_cast<uint8_t>(spCollectN_));
    out.push_back(spPushPages_);
    out.push_back(spError_);
    const uint16_t rn = static_cast<uint16_t>(
        std::min<size_t>(spResult_.size(), 0xFFFF));
    out.push_back(static_cast<uint8_t>(rn));
    out.push_back(static_cast<uint8_t>(rn >> 8));
    out.push_back(static_cast<uint8_t>(spResultPos_));
    out.push_back(static_cast<uint8_t>(spResultPos_ >> 8));
    out.insert(out.end(), spResult_.begin(), spResult_.begin() + rn);
    // v2 tail: per-unit MEDIA IDENTITY. `writeBuf_`/`writeBufPrimed_` above
    // is a primed 512-byte block waiting for its $C0n3 flush — restoring it
    // onto a bay that now holds a DIFFERENT image (the user swapped disks
    // after the ring frame was recorded) committed the old volume's block
    // into the new one. DiskIICard has carried this guard since its v3; the
    // SmartPort card is the other place a whole block can be written.
    for (size_t u = 0; u < kMaxUnits; ++u) {
        const uint64_t h = spMediaIdentity(unit(u));
        for (int k = 0; k < 8; ++k)
            out.push_back(static_cast<uint8_t>(h >> (8 * k)));
    }
}

void SmartPortCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    constexpr size_t kPerUnit = 6 + kBlockBytes;
    if (len < 3 || data[0] != 'S' || data[1] != 'P') return;
    const uint8_t version = data[2];
    if (version < 1 || version > kSnapVersion) return;
    // v1/v2 carried two units, v3 eight, v4 fourteen. Units the blob does not
    // cover are reset below, not left with the live card's state.
    const size_t nUnits = version >= 4 ? kMaxUnits : version == 3 ? 8 : 2;
    const size_t kBase = 4 + nUnits * kPerUnit;
    if (len < kBase) return;
    // An absent tail is the original v1 layout. Once any tail byte exists,
    // require its full fixed header and declared result before touching the
    // live call engine or unit state.
    const size_t kTailHeader = spCollect_.size() + 3 + 4;
    size_t identOff = 0;              // v2: offset of the media-identity tail
    if (len != kBase) {
        if (len - kBase < kTailHeader) return;
        const size_t rnOff = kBase + spCollect_.size() + 3;
        const size_t rn = static_cast<size_t>(data[rnOff] |
                                              (data[rnOff + 1] << 8));
        if (rn > len - (kBase + kTailHeader)) return;
        if (version >= 2) {
            identOff = kBase + kTailHeader + rn;
            if (len - identOff < nUnits * 8) return;
        }
    }
    activeUnit_ = std::min<size_t>(data[3], kMaxUnits - 1);
    for (size_t u = nUnits; u < kMaxUnits; ++u) {
        selectedBlock_[u] = 0; streamOffset_[u] = 0;
        writeBufPrimed_[u] = false; ioError_[u] = false;
        writeBuf_[u].fill(0);
    }
    const uint8_t* p = data + 4;
    for (size_t u = 0; u < nUnits; ++u) {
        selectedBlock_[u] = static_cast<uint16_t>(p[0] | (p[1] << 8));
        streamOffset_[u]  = static_cast<size_t>(p[2] | (p[3] << 8)) % kBlockBytes;
        writeBufPrimed_[u] = p[4] != 0;
        ioError_[u]        = p[5] != 0;
        std::memcpy(writeBuf_[u].data(), p + 6, kBlockBytes);
        // v2: never resurrect a primed write block onto a bay whose media
        // changed since the capture. Dropping the prime costs the guest a
        // re-issued WRITE; keeping it corrupts the new volume.
        if (identOff) {
            const uint8_t* ip = data + identOff + u * 8;
            uint64_t want = 0;
            for (int k = 0; k < 8; ++k)
                want |= static_cast<uint64_t>(ip[k]) << (8 * k);
            if (want != spMediaIdentity(unit(u))) {
                writeBufPrimed_[u] = false;
                writeBuf_[u].fill(0);
                streamOffset_[u]   = 0;
            }
        }
        p += kPerUnit;
    }
    // Media didn't move; the read cache just re-fills from the same block.
    readCacheValid_.fill(false);

    // v1.1 tail (optional — absent in pre-fix blobs, which then keep a
    // RESET call engine rather than the live one leaking through).
    size_t pos = static_cast<size_t>(p - data);
    spCollect_.fill(0);
    spCollectN_  = 0;
    spPushPages_ = 0;
    spError_     = 0;
    spResult_.clear();
    spResultPos_ = 0;
    if (len != kBase) {
        std::memcpy(spCollect_.data(), data + pos, spCollect_.size());
        pos += spCollect_.size();
        spCollectN_  = std::min<size_t>(data[pos++], spCollect_.size());
        spPushPages_ = data[pos++];
        spError_     = data[pos++];
        const size_t rn  = static_cast<size_t>(data[pos] | (data[pos + 1] << 8));
        pos += 2;
        size_t rpos = static_cast<size_t>(data[pos] | (data[pos + 1] << 8));
        pos += 2;
        if (len - pos >= rn) {
            spResult_.assign(data + pos, data + pos + rn);
            spResultPos_ = std::min(rpos, spResult_.size());
        }
    }
}

void SmartPortCard::advanceCycles(int cycles)
{
    cpuCycleTotal_ += static_cast<uint64_t>(cycles);
    if (audibleMotorOn_ &&
        cpuCycleTotal_ - lastAccessCycle_ > kSpinDownCycles) {
        if (sound_) sound_->motor(false, true);
        audibleMotorOn_ = false;
    }
}

void SmartPortCard::noteAccess(SmartPortUnit* unit)
{
    // Access light BEFORE the sound early-out: `sound_` is optional (no
    // FloppySoundDevice on a headless run, and the user can disable the
    // samples), but the LED is not.
    if (!unit) unit = units_[activeUnit_].get();
    if (unit) unit->bumpActivity();

    if (!sound_) return;
    // The UniDisk spin and steps are a 3.5" mechanism's: an HDV unit on this
    // card sounded like a floppy drive, while the same HDV on the hdv, CFFA
    // or Liron card is silent (bug hunt 2026-10-16).
    if (!dynamic_cast<SmartPort35Unit*>(unit)) return;
    if (!audibleMotorOn_) {
        sound_->motor(true, true);
        audibleMotorOn_ = true;
    }
    // One step event per accessed block. The sound device classifies
    // gap in emulated CPU cycles, so back-to-back ProDOS block reads
    // (~tens of ms apart at native speed) land in the seek-rate band
    // and the user hears a continuous seek; isolated accesses sound
    // like a single click.
    sound_->step(static_cast<int>(selectedBlock_[activeUnit_]),
                 cpuCycleTotal_);
    lastAccessCycle_ = cpuCycleTotal_;
}

uint8_t SmartPortCard::slotRomRead(uint8_t low8)
{
    return rom_[low8];
}

// ── SmartPort bus units (the //c's external port) ───────────────────────

int SmartPortCard::smartPortBusUnitCount() const
{
    return unitCount_;
}

void SmartPortCard::setUnitCount(int n)
{
    if (n < 2) n = 2;
    if (n > static_cast<int>(kMaxUnits)) n = static_cast<int>(kMaxUnits);
    if (n & 1) ++n;                      // ProDOS drives come in pairs
    // A selected bay can become invisible while a byte stream is in flight.
    // Discard its partial transfer, but keep the selection so subsequent data
    // accesses report no device instead of silently targeting another bay.
    if (n < unitCount_) {
        for (size_t u = static_cast<size_t>(n); u < kMaxUnits; ++u) {
            streamOffset_[u] = 0;
            writeBufPrimed_[u] = false;
            readCacheValid_[u] = false;
            ioError_[u] = false;
        }
        if (activeUnit_ >= static_cast<size_t>(n)) spPushPages_ = 0;
    }
    unitCount_ = n;
}

pom2::SmartPortBusUnit* SmartPortCard::smartPortBusUnit(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= kMaxUnits) return nullptr;
    BusUnit& u = busUnits_[static_cast<size_t>(index)];
    u.bind(this, static_cast<size_t>(index));
    return &u;
}

bool SmartPortCard::BusUnit::hasMedia() const
{
    return card_ && card_->guestLoaded(bay_);
}

uint32_t SmartPortCard::BusUnit::blockCount() const
{
    const SmartPortUnit* u = target();
    return (u && card_->guestLoaded(bay_)) ? u->blockCount() : 0;
}

bool SmartPortCard::BusUnit::writeProtected() const
{
    const SmartPortUnit* u = target();
    return !u || u->isWriteProtected();
}

bool SmartPortCard::BusUnit::readBlock(uint32_t block, uint8_t out[512])
{
    SmartPortUnit* u = target();
    if (!u || !card_->guestLoaded(bay_) || !u->readBlock(block, out)) return false;
    if (card_) card_->noteAccess(u);
    return true;
}

bool SmartPortCard::BusUnit::writeBlock(uint32_t block, const uint8_t in[512])
{
    SmartPortUnit* u = target();
    if (!u || !card_->guestLoaded(bay_) || !u->writeBlock(block, in)) return false;
    if (card_) card_->noteAccess(u);
    return true;
}

bool SmartPortCard::BusUnit::isUnidisk35() const
{
    const SmartPortUnit* u = target();
    // The unit object is the kind, media or not. A cleared bay (no object)
    // is the card's default drive on this bus: a UniDisk with the door open.
    return !u || u->kindKey() == SmartPort35Unit::kKindKey;
}

void SmartPortCard::BusUnit::requestEject()
{
    // The //c rear port's units are this card's bays (IIcExternalSmartPort).
    if (card_) card_->requestGuestEject(static_cast<int>(bay_), unitMediaId(target()));
}

uint64_t SmartPortCard::BusUnit::mediaIdentity() const
{
    return unitMediaId(target());
}

uint64_t SmartPortCard::bayMediaIdentity(int bay) const
{
    if (bay < 0 || bay >= static_cast<int>(kMaxUnits)) return 0;
    const SmartPortUnit* u = units_[static_cast<size_t>(bay)].get();
    return (u && u->isLoaded()) ? unitMediaId(u) : 0;
}

uint64_t SmartPortCard::unitMediaId(const SmartPortUnit* u)
{
    if (!u) return 0;
    if (const auto* b = u->blockBacking()) return b->mountId();
    if (const auto* u35 = dynamic_cast<const SmartPort35Unit*>(u))
        return u35->image().mountId();
    return 0;
}

bool SmartPortCard::exposesIicOnboardRom() const
{
    // //c-class memory map masks all slot ROM behind the forced INTCXROM;
    // Memory punches a hole for this card's $Cn00 firmware ONLY while a unit
    // holds media, so the //c autostart never JMPs into an empty SmartPort.
    // See SlotPeripheral::exposesIicOnboardRom + project_iic_smartport_boot.
    for (const auto& u : units_)
        if (u && u->isLoaded()) return true;
    return false;
}

uint8_t SmartPortCard::deviceSelectRead(uint8_t low4)
{
    switch (low4) {
        case 0x3: return readDataByte();
        case 0x4: return statusByte();
        case 0x5: return blockCountByte(0);   // STATUS block count, low
        case 0x6: return blockCountByte(1);   // STATUS block count, high
        // ── SmartPort-protocol call engine (see buildC800) ──────────
        case 0x9:                             // pull next result byte
            return spResultPos_ < spResult_.size()
                 ? spResult_[spResultPos_++] : uint8_t{0x00};
        case 0xB: return static_cast<uint8_t>(spResult_.size() & 0xFF);
        case 0xC: return static_cast<uint8_t>(spResult_.size() >> 8);
        case 0xD: return spPushPages_;        // WRITE data pages (0 or 2)
        case 0xE: return spExecute();         // EXECUTE → error code
        case 0xF:                             // post-stream error re-poll
            // Only a stream THIS call armed may be reported here. `ioError_`
            // is the legacy $C0n3 latch, shared with the ProDOS driver path,
            // and the only things that clear it are the $C0n0/$C0n1/$C0n2
            // register writes — which a SmartPort call never makes. So a
            // ProDOS-path transfer that legitimately failed (a volume scanner
            // reading past the end of the volume, ProDOS ONLINE on an empty
            // bay) left the latch set, and the $CE00 handler — which ends with
            // `LDA $C0nF` on EVERY successful path, not just after a write
            // stream — turned the next SmartPort READ/STATUS into carry-set +
            // $27 with the payload already delivered in the caller's buffer.
            // `spPushPages_` is non-zero only in the WRITE BLOCK branch, the
            // only branch that arms a stream, and that branch sets
            // `activeUnit_` and clears `ioError_` for its own unit first — so
            // gating on it makes this register report exactly this call's
            // outcome and nothing else.
            return (spPushPages_ != 0 && activeUnit_ < kMaxUnits &&
                    ioError_[activeUnit_])
                 ? uint8_t{0x27} : uint8_t{0x00};
        default:  return 0xFF;
    }
}

uint8_t SmartPortCard::blockCountByte(int which) const
{
    // The ProDOS STATUS driver call (cmd $00) must return the device's total
    // block count in X (low) / Y (high). The ROM status routine reads it from
    // these two registers. A driver that left X/Y unset returned garbage,
    // which crashed a ProDOS volume scanner (e.g. BITSY) that enumerated this
    // device after booting from another slot — the //c "Disk II + on-board
    // SmartPort" garble (see project_iic_smartport_boot).
    const SmartPortUnit* u =
        (activeUnit_ < static_cast<size_t>(unitCount_))
        ? units_[activeUnit_].get() : nullptr;
    uint32_t blocks = (u && guestLoaded(activeUnit_)) ? u->blockCount() : 0u;
    // ProDOS STATUS returns a 16-bit count: an exactly-32 MiB volume
    // (65536 blocks — Block512Backing::kMaxBlocks deliberately admits
    // it, since block INDEXES stay 16-bit) must clamp to $FFFF, not
    // truncate to 0 — a 0-block STATUS makes ProDOS treat the volume
    // as empty/offline.
    if (blocks > 0xFFFFu) blocks = 0xFFFFu;
    return static_cast<uint8_t>((blocks >> (which ? 8 : 0)) & 0xFF);
}

void SmartPortCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    switch (low4) {
        case 0x0: {                             // drive / unit select
            // Modulo (not a bitmask) so the mapping stays correct if the unit
            // count is odd. Modulo the count the card ANSWERS FOR, not
            // kMaxUnits: bays past `unitCount_` hold media but are documented
            // "invisible to the guest", and the SmartPort call engine's own
            // `unitFor` already refuses them with $28. Folding on kMaxUnits
            // (8 since 2026-09-08, 2 before) let `LDA #3 / STA $C0n0` read and
            // COMMIT 512-byte blocks on a bay outside the count — outside
            // `bayCount()` too, so the Slot Manager could not even show it.
            const size_t live = static_cast<size_t>(unitCount_ > 0 ? unitCount_ : 1);
            const size_t u = static_cast<size_t>(v) % live;
            activeUnit_ = u;
            // A unit-select starts a fresh transfer: drop any half-streamed
            // write buffer / stale read cache / error so the next op is clean.
            streamOffset_[u]   = 0;
            writeBufPrimed_[u] = false;
            readCacheValid_[u] = false;
            ioError_[u]        = false;
            break;
        }
        case 0x1: {                             // block LO of active unit
            const size_t u = activeUnit_;
            selectedBlock_[u] = static_cast<uint16_t>(
                (selectedBlock_[u] & 0xFF00u) | v);
            streamOffset_[u]   = 0;
            readCacheValid_[u] = false;
            writeBufPrimed_[u] = false;
            ioError_[u]        = false;
            break;
        }
        case 0x2: {                             // block HI of active unit
            const size_t u = activeUnit_;
            selectedBlock_[u] = static_cast<uint16_t>(
                (selectedBlock_[u] & 0x00FFu) |
                (static_cast<uint16_t>(v) << 8));
            streamOffset_[u]   = 0;
            readCacheValid_[u] = false;
            writeBufPrimed_[u] = false;
            ioError_[u]        = false;
            break;
        }
        case 0x3:                               // streaming write
            writeDataByte(v);
            break;
        case 0x7:                               // SmartPort param push
            if (spCollectN_ < spCollect_.size())
                spCollect_[spCollectN_++] = v;
            break;
        case 0xE:                               // SmartPort BEGIN
            spCollectN_ = 0;
            spCollect_.fill(0);
            spResult_.clear();
            spResultPos_ = 0;
            spPushPages_ = 0;
            spError_     = 0;
            break;
        default:
            break;
    }
}

void SmartPortCard::noteIoError(size_t u)
{
    if (u >= kMaxUnits) return;
    const bool rising = !ioError_[u];
    ioError_[u] = true;
    if (rising && sound_) sound_->ioError();
}

uint8_t SmartPortCard::statusByte() const
{
    const SmartPortUnit* u = (activeUnit_ < static_cast<size_t>(unitCount_))
        ? units_[activeUnit_].get() : nullptr;
    uint8_t s = (u && guestLoaded(activeUnit_)) ? 0x00 : 0x80;
    if (!u || u->isWriteProtected()) s |= 0x40;
    if (activeUnit_ < kMaxUnits && ioError_[activeUnit_]) s |= 0x01;  // I/O error
    return s;
}

uint8_t SmartPortCard::readDataByte()
{
    const size_t u = activeUnit_;
    if (u >= static_cast<size_t>(unitCount_)) {
        noteIoError(u);
        return 0xFF;
    }
    SmartPortUnit* unit = units_[u].get();
    if (!unit || !guestLoaded(u)) {
        // No unit / no media: latch the error so the ROM routine returns
        // carry-set instead of CLC "success" with a $FF buffer. Without
        // this, ProDOS ONLINE on an empty bay 2 got a garbage volume and
        // PR#5 with no media booted 512 bytes of $FF into $0800 and
        // jumped into it. (Real driver convention: SEC + $28 "no device
        // connected", ProDOS 8 TRM.)
        if (u < kMaxUnits) ioError_[u] = true;
        return 0xFF;
    }

    // Lazy per-unit read cache. The driver issues 512 byte-reads per
    // ProDOS block; we hit the underlying SmartPortUnit::readBlock once
    // when streamOffset_ wraps to 0 (or the cached block doesn't match)
    // and return cached bytes for the remaining 511.
    if (streamOffset_[u] == 0 ||
        !readCacheValid_[u] ||
        readCacheBlock_[u] != selectedBlock_[u])
    {
        if (!unit->readBlock(selectedBlock_[u], readCache_[u].data())) {
            // Out-of-range / failed read: latch an I/O error so the ROM read
            // routine returns carry-set (ProDOS $27) instead of CLC "success"
            // with a garbage buffer, AND keep the byte stream in phase by
            // serving a 0xFF-filled cache — a mid-transfer failure must not
            // desync the remaining 511 reads of the block.
            noteIoError(u);
            readCache_[u].fill(0xFF);
        }
        readCacheBlock_[u] = selectedBlock_[u];
        readCacheValid_[u] = true;
        noteAccess();
    }
    const uint8_t out = readCache_[u][streamOffset_[u]];
    streamOffset_[u] = (streamOffset_[u] + 1) % kBlockBytes;
    return out;
}

void SmartPortCard::writeDataByte(uint8_t v)
{
    const size_t u = activeUnit_;
    if (u >= static_cast<size_t>(unitCount_)) {
        noteIoError(u);
        return;
    }
    SmartPortUnit* unit = units_[u].get();
    if (!unit || !guestLoaded(u)) {
        // Same as readDataByte: a write to an empty bay must error, not
        // silently drop 512 bytes and return "success" to the driver.
        if (u < kMaxUnits) ioError_[u] = true;
        return;
    }
    if (unit->isWriteProtected()) { ioError_[u] = true; return; }  // surface WP

    // Mirror the read cache for writes: accumulate 512 bytes in
    // `writeBuf_[u]`, commit when streamOffset_ wraps back to 0.
    if (streamOffset_[u] == 0 && !writeBufPrimed_[u]) {
        // Partial-block update: pre-fill with existing contents so the
        // un-written tail isn't zeroed.
        if (!unit->readBlock(selectedBlock_[u], writeBuf_[u].data())) {
            std::memset(writeBuf_[u].data(), 0, kBlockBytes);
        }
        writeBufPrimed_[u] = true;
    }
    writeBuf_[u][streamOffset_[u]] = v;
    streamOffset_[u] = (streamOffset_[u] + 1) % kBlockBytes;
    if (streamOffset_[u] == 0) {
        if (!unit->writeBlock(selectedBlock_[u], writeBuf_[u].data())) {
            // Out-of-range / rejected commit → report failure to ProDOS
            // (ROM write routine tests $C0n4 bit 0 and returns carry-set).
            noteIoError(u);
        }
        writeBufPrimed_[u] = false;
        // The just-committed block is no longer the most-recently-read
        // one; invalidate the read cache so the next read pulls fresh.
        readCacheValid_[u] = false;
        noteAccess();
    }
}

void SmartPortCard::buildRom()
{
    rom_.fill(0xEA);                            // NOP padding

    const uint16_t kDeviceBase = static_cast<uint16_t>(0xC080 + slot_ * 16);
    const uint8_t  kSlotRomHi  = static_cast<uint8_t>(0xC0 + slot_);
    const uint8_t  kUnitDrv1   = static_cast<uint8_t>(slot_ << 4);

    const uint8_t blkLoReg = static_cast<uint8_t>(kDeviceBase + 0x01);
    const uint8_t blkHiReg = static_cast<uint8_t>(kDeviceBase + 0x02);
    const uint8_t dataReg  = static_cast<uint8_t>(kDeviceBase + 0x03);
    const uint8_t statReg  = static_cast<uint8_t>(kDeviceBase + 0x04);

    // ── 256-byte layout ────────────────────────────────────────────────
    //   $Cn00-$Cn02  PR#n / autostart entry
    //   $Cn03-$Cn09  ProDOS + controller-class signature bytes
    //   $Cn0A-$Cn0C  real-hardware ProDOS driver entry
    //   $Cn0D-$Cn12  SmartPort entry (ProDOS entry + 3)
    //   $Cn13-$Cn1F  LEFT ALONE — the real Liron dump's identity bytes
    //   $Cn20-$Cn4F  boot routine
    //   $Cn50-$Cn6E  ProDOS driver dispatch
    //   $Cn6F-$CnA1  read block
    //   $CnA2-$CnDF  write block
    //   $CnE0-$CnE2  spare (the old boot-failure halt loop)
    //   $CnE3-$CnFB  LEFT ALONE — real dump ID bytes
    //   $CnFC-$CnFD  size in blocks: $0000 = ask STATUS (as the dump)
    //   $CnFE-$CnFF  capability + driver-entry bytes
    //
    // Every region declares where it starts and ends, and every address in
    // the code is a LABEL. That combination is what this card cost to learn:
    // the write-block routine once ran from $Cn9C to $CnD5, straight THROUGH
    // the STATUS routine that `JMP $CnC0` pointed at, so ProDOS STATUS
    // executed the middle of the write loop and answered $27 on a healthy
    // bay for weeks. Both halves of that are now unrepresentable — regions
    // may not overlap, and no displacement is typed.
    pom2::SlotRomAsm a(rom_, slot_, "SmartPortCard");

    a.region("entry", 0x00, 0x03).jmp("boot");

    // ── ProDOS / controller-class signature ────────────────────────────
    // $Cn07 identifies the controller class to the //c-class boot firmware.
    // $3C = Disk II (the //c internal $C607!) — claiming it makes a //c see
    // TWO Disk II controllers (slot 5 + the internal slot 6) and its boot
    // scan corrupts. $00 = SmartPort, which triggers a SmartPort enumeration
    // this block-only stub can't service. $01 = plain ProDOS block device
    // (non-removable, like ProDOSHardDiskCard) → the //c boots it via the
    // standard JMP $Cn00 path with no Disk II / SmartPort confusion, and //e
    // boot (bootFromSlot, ProDOS via $CnFF) is unaffected. See
    // project_iic_smartport_boot.
    a.region("signature", 0x03, 0x0A)
     .poke(0x03, 0x00)
     .poke(0x05, 0x03)
     .poke(0x07, 0x01);                         // ProDOS block device

    // ── Real-hardware driver entry at $Cn0A ────────────────────────────
    // The Apple Disk 3.5 / Liron ("Unidisk") firmware exposes its block
    // driver at a FIXED $Cn0A, and software that talks to it directly —
    // rather than via the ProDOS $CnFF indirection — hardcodes `JSR $Cn0A`
    // (e.g. French Touch DIX `boot_unidisk.a`: `modread JSR $C50A`, with the
    // ProDOS-style ZP param block at $42-$47). POM2 synthesises its dispatch
    // at $Cn50 instead, so a bare `JSR $Cn0A` used to land on an unimplemented
    // $00 = BRK → the caller (with LC RAM read enabled) stormed the BRK vector
    // out of cold Language-Card RAM. Redirect $Cn0A → the existing $Cn50
    // dispatch; the $42-$47 calling convention is identical, so block reads/
    // writes/status all work unchanged. ($CnFF stays $Cn50 so the //e/c boot
    // and the ProDOS tests are untouched.)
    a.region("driverEntry", 0x0A, 0x0D).jmp("driver");

    // SmartPort-convention entry = ProDOS entry + 3 = $Cn0D (real Liron:
    // $CnFF=$0A, SmartPort dispatch $Cn0D). Routes to the full SmartPort
    // call handler in the $C800 bank (see buildC800) — STATUS/DIB, READ,
    // WRITE, FORMAT, CONTROL, INIT with real error codes. Callers that
    // hardcode entry+3 used to fall through NOP padding INTO the boot
    // routine at $Cn20 (booting block 0 over $0800).
    //
    // `BIT $CFFF` first, exactly like the real Liron firmware, and it is not
    // decoration: on a //e with SLOTC3ROM off (the default), ANY read in
    // $C300-$C3FF latches the MMU's INTC8ROM flip-flop, after which
    // $C800-$CFFF answers from the INTERNAL ROM instead of the slot's
    // expansion bank (Memory::memRead, MAME `apple2e.cpp:c300_int_r`). The
    // 80-column firmware touches $C3xx constantly, so a bare `JMP $CE00`
    // would regularly fetch motherboard bytes and run them as if they were
    // the SmartPort handler. Reading $CFFF clears INTC8ROM and releases the
    // expansion owner; fetching the JMP that follows, at $Cn10, re-claims
    // the window for THIS slot (SlotBus::slotRomRead latches the owner on
    // any access to the slot's page), so the target is live by the time the
    // JMP takes it.
    a.region("smartPortEntry", 0x0D, 0x13)
     .emit({ 0x2C, 0xFF, 0xCF,                  // BIT $CFFF
             0x4C, 0x00, 0xCE });               // JMP $CE00

    // ── Boot routine ($Cn20) ───────────────────────────────────────────
    a.region("boot", kBootOff, kDriverOff)
     .emit({ 0xA9, 0x01,            // LDA #$01           ; ProDOS read cmd
             0x85, 0x42,
             0xA9, kUnitDrv1,       // unit = slot×16, drive 1
             0x85, 0x43,
             0xA9, 0x00,
             0x85, 0x44,            // buffer LO = $00
             0xA9, 0x08,
             0x85, 0x45,            // buffer HI = $08
             0xA9, 0x00,
             0x85, 0x46,            // block LO = 0
             0x85, 0x47 })          // block HI = 0
     .jsr("driver")
     .branch(0xB0, "bootErr")       // BCS bootErr
     .emit({ 0xA2, kUnitDrv1,       // X = unit
             0xA9, 0x00,
             0x4C, 0x01, 0x08 })    // JMP $0801
     // Boot failure (empty unit 1, unreadable block 0). Not a halt any more:
     // a //e with an EMPTY SmartPort card in slot 7 used to park here and
     // never reach the Disk II in slot 6. $CD30 does what the real Liron
     // firmware does (roms/liron.rom $E90-$ED3): when the //e autostart
     // scan brought us here it JMPs $FABA, and the scan tries the next slot
     // down; it never returns in that case. Otherwise it puts CSW/KSW back
     // and returns, and the Monitor calls happen HERE, in the $Cn page: on
     // a //c the card's $C800 window closes at the first foreign $C0xx
     // access (Memory::memRead, iicCardWindow_), so returning into $C800
     // from COUT would run the //c's internal ROM. No BIT $CFFF first: the
     // driver's pre-flight at $CD00 just ran, so the window is ours.
     .label("bootErr")
     .emit({ 0x20, static_cast<uint8_t>(kBootScanAddr & 0xFF),
                   static_cast<uint8_t>(kBootScanAddr >> 8),  // JSR $CD30
             0x20, 0x8E, 0xFD,      // JSR $FD8E  CROUT
             0x20, 0x2D, 0xFF,      // JSR $FF2D  PRERR ("ERR" + bell)
             0x4C, 0x00, 0xE0 });   // JMP $E000  BASIC, like the Liron

    // ── ProDOS driver dispatch ($Cn50) ─────────────────────────────────
    // ProDOS calls here with $42 = command, $43 = unit, $44/$45 = buffer,
    // $46/$47 = block. The unit byte names a slot as well as a drive; the
    // routine at $CD80 turns both into a bay (units 3/4 are slot 2's).
    // `BIT $CFFF` en tete, comme le vrai firmware Liron sur SON entree — et
    // ce n'est pas un ornement : l'entree ProDOS est appelee par le noyau
    // APRES que le firmware 80 colonnes a verrouille la fenetre interne
    // (tout passage par $C3xx latche INTC8ROM). Sans cette liberation, les
    // JSR du driver vers $CD00/$CD10 executaient la banque INTERNE — c'est
    // ainsi que le scan //c de ProDOS 2.4.3 partait dans le decor et que la
    // table des devices restait vide (RESTART SYSTEM-$0A au premier MLI du
    // programme charge, 2026-08-30). L'entree SmartPort $Cn0D avait deja le
    // sien. Les 3 octets viennent du dispatch : tester cmd=0 par un BEQ
    // direct economise le CMP #$00, et le pad historique fournit le reste.
    a.region("driver", kDriverOff, kReadOff)
     .emit({ 0x2C, 0xFF, 0xCF,      // BIT $CFFF         ; libere INTC8ROM
             0x20, static_cast<uint8_t>(kUnitSelAddr & 0xFF),
                   static_cast<uint8_t>(kUnitSelAddr >> 8),
                                    // JSR $CD80         ; $43 → $C0n0
             0xA5, 0x42 })          // LDA $42           ; command
     .branch(0xF0, "dispStatus")    // BEQ — STATUS ($00)
     .emit({ 0xC9, 0x01 })          // CMP #$01
     .branch(0xF0, "read")
     .emit({ 0xC9, 0x02 })          // CMP #$02
     .branch(0xF0, "write")
     .emit({ 0x4C, static_cast<uint8_t>(kOtherCmdAddr & 0xFF),
                   static_cast<uint8_t>(kOtherCmdAddr >> 8) })  // JMP $CD60
                                    // FORMAT ($03) or a bad command ($27)
     .label("dispStatus")
     .emit({ 0x4C, static_cast<uint8_t>(kStatusAddr & 0xFF),
                   static_cast<uint8_t>(kStatusAddr >> 8) });  // JMP $CD10

    // ── Read block ($Cn6F) ─────────────────────────────────────────────
    // Pre-flight the bay, stream 512 bytes, then test $C0n4 bit 0 (I/O
    // error) so a failed / out-of-range readBlock returns carry-set (ProDOS
    // $27) rather than CLC "success" over a $FF-filled buffer.
    a.region("read", kReadOff, kWriteOff)
     .emit({ 0x20, static_cast<uint8_t>(kPreflightAddr & 0xFF),
                   static_cast<uint8_t>(kPreflightAddr >> 8) }) // JSR $CD00
     .branch(0x90, "rdGo")          // BCC rdGo    ; media present
     .emit({ 0x60 })                // RTS         ; else return $28 + SEC
     .label("rdGo")
     .emit({ 0xA5, 0x46,            // LDA $46
             0x8D, blkLoReg, 0xC0,
             0xA5, 0x47,            // LDA $47
             0x8D, blkHiReg, 0xC0,
             0xA0, 0x00 })          // LDY #$00
     .label("rdPage1")
     .emit({ 0xAD, dataReg, 0xC0,   // LDA $C0n3
             0x91, 0x44,            // STA ($44),Y
             0xC8 })                // INY
     .branch(0xD0, "rdPage1")
     .emit({ 0xE6, 0x45 })          // INC $45
     .label("rdPage2")
     .emit({ 0xAD, dataReg, 0xC0,   // LDA $C0n3
             0x91, 0x44,            // STA ($44),Y
             0xC8 })                // INY
     .branch(0xD0, "rdPage2")
     .emit({ 0xC6, 0x45,            // DEC $45
             0xAD, statReg, 0xC0,   // LDA $C0n4   ; status
             0x29, 0x01 })          // AND #$01    ; I/O error bit
     .branch(0xD0, "rdErr")
     .emit({ 0x18,                  // CLC         ; success
             0x60 })                // RTS
     .label("rdErr")
     .emit({ 0xA9, 0x27,            // LDA #$27 (ProDOS I/O error)
             0x38,                  // SEC
             0x60 });               // RTS

    // ── Write block ($CnA2) ────────────────────────────────────────────
    // Media pre-check first ($28 — an empty bay is not "write protected",
    // which is what it used to answer), then WP ($2B); after streaming 512
    // bytes, tests $C0n4 bit 0 so an out-of-range / rejected writeBlock
    // returns carry-set ($27) instead of the old unconditional CLC.
    a.region("write", kWriteOff, kHaltOff)
     .emit({ 0x20, static_cast<uint8_t>(kPreflightAddr & 0xFF),
                   static_cast<uint8_t>(kPreflightAddr >> 8) }) // JSR $CD00
     .branch(0x90, "wrGo")
     .emit({ 0x60 })                // RTS   ; empty: $28, NOT "write protected"
     .label("wrGo")
     .emit({ 0x2C, statReg, 0xC0 }) // BIT $C0n4   ; V = WP bit
     .branch(0x50, "wrXfer")        // BVC wrXfer  (not WP → proceed)
     .emit({ 0xA9, 0x2B,            // LDA #$2B    ; write-protected error
             0x38,                  // SEC
             0x60 })                // RTS
     .label("wrXfer")
     .emit({ 0xA5, 0x46,            // LDA $46
             0x8D, blkLoReg, 0xC0,
             0xA5, 0x47,            // LDA $47
             0x8D, blkHiReg, 0xC0,
             0xA0, 0x00 })          // LDY #$00
     .label("wrPage1")
     .emit({ 0xB1, 0x44,            // LDA ($44),Y
             0x8D, dataReg, 0xC0,   // STA $C0n3
             0xC8 })                // INY
     .branch(0xD0, "wrPage1")
     .emit({ 0xE6, 0x45 })          // INC $45
     .label("wrPage2")
     .emit({ 0xB1, 0x44,            // LDA ($44),Y
             0x8D, dataReg, 0xC0,   // STA $C0n3
             0xC8 })                // INY
     .branch(0xD0, "wrPage2")
     .emit({ 0xC6, 0x45,            // DEC $45
             0xAD, statReg, 0xC0,   // LDA $C0n4   ; re-read status
             0x29, 0x01 })          // AND #$01    ; I/O error bit
     .branch(0xD0, "wrErr")
     .emit({ 0xA9, 0x00,            // LDA #$00    ; success
             0x18,                  // CLC
             0x60 })                // RTS
     .label("wrErr")
     .emit({ 0xA9, 0x27,            // LDA #$27 (ProDOS I/O error)
             0x38,                  // SEC
             0x60 });               // RTS

    // $CnFE capability byte (ProDOS 8 TN #21): bit3 format, bit2 WRITE,
    // bit1 read, bit0 status, bits5-4 volume count. Was $13 — read+status
    // only — which advertised a READ-ONLY device to capability-inspecting
    // utilities despite the write path being fully implemented; then $17,
    // with FORMAT refused. FORMAT is answered now ($CD60), so bit 3 is set,
    // like the real Liron's $BF.
    // $CnFC-$CnFD: the device's size in blocks, $0000 = "ask STATUS" (ProDOS
    // 8 TRM, protocol converter / block-device signature), which is what the
    // real Liron page holds (roms/liron.rom: 00 00 BF 0A). Left as NOP fill,
    // this page claimed $EAEA = 60 138 blocks for every bay: A2osX reads the
    // size there for a $Cn07 != 0 device and listed s5d1/s5d2 at 60 138,
    // empty bay included (2026-10-23). The bays' size varies with the disk
    // in them, so STATUS it is.
    a.region("size", 0xFC, 0xFE)
     .poke(0xFC, 0x00)
     .poke(0xFD, 0x00);
    a.region("tail", 0xFE, pom2::kSlotRomBytes)
     .emit({ 0x1F })                // format+read+WRITE+status, 2 units
     .byteOf("driver");

    // A page that does not assemble is a build error in hand-written code,
    // not a runtime condition: the ROM would ship with a routine truncated,
    // a neighbour eaten or a branch pointing nowhere — exactly the failure
    // this layout was rewritten to undo. Surface it loudly and let
    // `smartport_rom_layout` fail on it.
    romLayoutError_ = !a.finish();
    if (romLayoutError_)
        log().error("SmartPort", "slot ROM did not assemble: " + a.error());

    // ── Real Liron base (roms/liron.rom present) ───────────────────────
    // Re-base the page on the real dump (per-slot page at slot×256) for
    // authentic identity — $Cn07=$00 (SmartPort class), $CnFB=$00,
    // $CnFE=$BF, $CnFF=$0A — then overlay POM2's HLE service entries on
    // top: the real firmware's IWM/UniDisk code can't run without the
    // drive-side 65C02, so every serviceable entry routes to the block
    // backend instead. Kept real: $03-$09 (signature/class), $13-$1F,
    // $E3-$FF (ID bytes). The synthetic $Cn00 "JMP $Cn20" reproduces the
    // real page's own $Cn01=$20 signature byte by construction.
    if (lironLoaded_ && slot_ >= 1 && slot_ <= 7) {
        const std::array<uint8_t, 256> synth = rom_;
        std::memcpy(rom_.data(),
                    lironRom_.data() + static_cast<size_t>(slot_) * 256, 256);
        rom_[0x00] = 0x4C; rom_[0x01] = kBootOff; rom_[0x02] = kSlotRomHi;
        // $Cn0A (ProDOS driver) + $Cn0D-$Cn12 (BIT $CFFF / JMP $CE00). The
        // overlay used to run to $1F and so covered the dump's own bytes
        // there with NOP padding, contradicting the "kept real" line above;
        // it now stops where the dispatch stub actually ends.
        for (int i = 0x0A; i <= 0x12; ++i) rom_[i] = synth[i];
        for (int i = kBootOff; i <= 0xE2; ++i) rom_[i] = synth[i];
    }

    buildC800();
}

// ── $C800 bank + the SmartPort-protocol dispatch handler ($CE00) ────────
// $Cn0D (SmartPort entry = ProDOS entry + 3, matching the real Liron's
// $CnFF=$0A / dispatch $Cn0D convention) jumps here. The 6502 stub does
// ALL guest-memory movement — cards have no Memory access — and drives
// the C++ engine through device-select registers:
//   write 0xE  BEGIN (resets the collector)
//   write 0x7  push byte (cmd first, then the 10 param-list bytes)
//   read  0xE  EXECUTE → A = SmartPort error code ($00 = ok)
//   read  0x9  pull next result byte (STATUS payloads, READ data)
//   read  0xB/0xC  result count lo / hi (bytes to pull via 0x9)
//   read  0xD  push page count (2 for WRITE — data streams into reg 0x3,
//              the legacy write machinery commits + latches errors)
//   read  0xF  post-stream error re-poll ($27 on a failed WRITE commit)
// ZP $42-$45 are saved/restored around the call (SmartPort firmware
// convention). Assembled offline with verified branch offsets; the reg
// operand lo-bytes are emitted as 0x80+reg and patched to the slot's
// device-select base below.
void SmartPortCard::buildC800()
{
    c800_.fill(0xFF);
    if (lironLoaded_)
        std::memcpy(c800_.data(), lironRom_.data() + 2048, 2048);

    static constexpr uint8_t kHandler[] = {
        0xA5, 0x42, 0x48, 0xA5, 0x43, 0x48, 0xA5, 0x44, 0x48, 0xA5, 0x45, 0x48,
        0xBA, 0xBD, 0x05, 0x01, 0x85, 0x42, 0xBD, 0x06, 0x01, 0x85, 0x43, 0x8D,
        0x8E, 0xC0, 0xA0, 0x01, 0xB1, 0x42, 0x8D, 0x87, 0xC0, 0xC8, 0xB1, 0x42,
        0x85, 0x44, 0xC8, 0xB1, 0x42, 0x85, 0x45, 0xBA, 0x18, 0xBD, 0x05, 0x01,
        0x69, 0x03, 0x9D, 0x05, 0x01, 0xBD, 0x06, 0x01, 0x69, 0x00, 0x9D, 0x06,
        0x01, 0xA0, 0x00, 0xB1, 0x44, 0x8D, 0x87, 0xC0, 0xC8, 0xC0, 0x0A, 0xD0,
        0xF6, 0xAD, 0x8E, 0xC0, 0xD0, 0x49, 0xA0, 0x02, 0xB1, 0x44, 0xAA, 0xC8,
        0xB1, 0x44, 0x85, 0x45, 0x86, 0x44, 0xAE, 0x8C, 0xC0, 0xA0, 0x00, 0xE0,
        0x00, 0xF0, 0x0D, 0xAD, 0x89, 0xC0, 0x91, 0x44, 0xC8, 0xD0, 0xF8, 0xE6,
        0x45, 0xCA, 0xD0, 0xF3, 0xAE, 0x8B, 0xC0, 0xF0, 0x09, 0xAD, 0x89, 0xC0,
        0x91, 0x44, 0xC8, 0xCA, 0xD0, 0xF7, 0xAE, 0x8D, 0xC0, 0xE0, 0x00, 0xF0,
        0x0F, 0xA0, 0x00, 0xB1, 0x44, 0x8D, 0x83, 0xC0, 0xC8, 0xD0, 0xF8, 0xE6,
        0x45, 0xCA, 0xD0, 0xF3, 0xAD, 0x8F, 0xC0, 0xAA, 0x68, 0x85, 0x45, 0x68,
        0x85, 0x44, 0x68, 0x85, 0x43, 0x68, 0x85, 0x42,
        // The exit, as the Liron's own ($CDC1 in liron-if.asm): A = error,
        // carry set iff A != 0, and X / Y = the byte count transferred, low /
        // high ($C0nB/$C0nC, the reply's size). The handler used to return
        // the error in X too and whatever the copy loop left in Y — 25 after
        // a DIB, read by a caller as a count of 6400 (bug hunt 2026-10-23).
        0x8A, 0x48,               // TXA / PHA         ; the error code
        0xAE, 0x8B, 0xC0,         // LDX $C0nB         ; count, low
        0xAC, 0x8C, 0xC0,         // LDY $C0nC         ; count, high
        0x68, 0xC9, 0x01, 0x60,   // PLA / CMP #1 / RTS
    };
    // Register-operand lo bytes (0x80+reg placeholders) → this slot's base.
    static constexpr size_t kRegPatch[] =
        { 24, 31, 66, 74, 91, 100, 113, 118, 127, 138, 149, 167, 170 };

    // ── ProDOS media pre-flight ($CD00) and STATUS ($CD10) ─────────────
    // Both are reached from the slot page's ProDOS driver, which is what
    // points the $C800 window at this slot in the first place. They live here
    // rather than in the slot page because the slot page has no room for them
    // and the gaps that look free are the real Liron dump's identity bytes.
    //
    // $C0n4 is the side-effect-free status byte, and it puts no-media at bit 7
    // and write-protect at bit 6 precisely so ONE BIT answers both: BIT sets N
    // from bit 7 and V from bit 6 without touching A.
    {
        const uint8_t statReg = static_cast<uint8_t>(0x80 + slot_ * 16 + 0x04);
        const uint8_t blk5    = static_cast<uint8_t>(0x80 + slot_ * 16 + 0x05);
        const uint8_t blk6    = static_cast<uint8_t>(0x80 + slot_ * 16 + 0x06);

        // Pre-flight: carry set + A = $28 ("no device connected") when the bay
        // is empty, carry clear otherwise. Shared by READ and WRITE so both
        // give the same answer — READ used to fall through to the transfer and
        // report $27 "I/O error", WRITE tested write-protect first and reported
        // $2B "write protected". An empty bay is neither.
        const uint8_t pf[] = {
            0x2C, statReg, 0xC0,   // BIT $C0n4   ; N = no media, V = WP
            0x10, 0x04,            // BPL +4      ; media present
            0xA9, 0x28,            // LDA #$28    ; no device connected
            0x38,                  // SEC
            0x60,                  // RTS
            0x18,                  // CLC
            0x60                   // RTS
        };
        std::memcpy(c800_.data() + kPreflightC800, pf, sizeof pf);

        // STATUS (ProDOS cmd $00): no media → $28, else CLC with the total
        // block count in X (low) / Y (high) so a volume scanner (BITSY,
        // ONLINE) can size the device. Write-protect is WRITE's job ($2B)
        // and a bit in $C0n4; answering it here made ON_LINE skip a locked
        // 3.5" the way the HDV driver used to. Only bit 7 is tested: bit 6
        // is the WP flag, and BIT already put it in V without our asking.
        const uint8_t st[] = {
            0x2C, statReg, 0xC0,   // BIT $C0n4   ; N = no media
            0x10, 0x04,            // BPL +4      ; media present
            0xA9, 0x28,            // LDA #$28    ; no device connected
            0x38,                  // SEC
            0x60,                  // RTS
            0xAE, blk5, 0xC0,      // LDX $C0n5
            0xAC, blk6, 0xC0,      // LDY $C0n6
            0xA9, 0x00,            // LDA #$00
            0x18,                  // CLC
            0x60                   // RTS
        };
        static_assert(sizeof st <= kStatusC800 - kPreflightC800 + 0x100,
                      "STATUS must not run into the handler at $CE00");
        std::memcpy(c800_.data() + kStatusC800, st, sizeof st);
    }

    // ── Boot failure ($CD30) ────────────────────────────────────────────
    // The real Liron's answer to "nothing to boot" ($CE90 in its bank,
    // roms/liron.rom $E96-$EA1):
    //     LDX $00 / BNE err / LDX $01 / CPX $07F8 / BNE err / JMP $FABA
    // $00 = 0 and $01 = $07F8 = $Cn is the state the //e autostart scan loop
    // ($FAB4-$FAD2) leaves when it JMP ($0000)s into a slot; $FABA re-enters
    // the loop, which DECs $01 and tries the next slot. One extra test the
    // Liron, being card-resident on slot machines only, never needed: $FABA
    // must hold the scan's LDY #imm ($A0). On a //c it is LDA $AC and on an
    // original Apple ][ it is not code, so there the scan path is refused.
    // Not the scan: CSW = COUT1 ($FDF0), KSW = KEYIN ($FD1B) — what SETVID /
    // SETKBD store, written inline because a Monitor call from this bank is
    // exactly what a //c cannot return from — then RTS to the $Cn page,
    // which prints and goes to BASIC.
    {
        const uint8_t bs[] = {
            0xA6, 0x00,            // LDX $00
            0xD0, 0x11,            // BNE notScan  (+17)
            0xA6, 0x01,            // LDX $01
            0xEC, 0xF8, 0x07,      // CPX $07F8
            0xD0, 0x0A,            // BNE notScan  (+10)
            0xAD, 0xBA, 0xFA,      // LDA $FABA
            0xC9, 0xA0,            // CMP #$A0   ; the scan's LDY #imm
            0xD0, 0x03,            // BNE notScan  (+3)
            0x4C, 0xBA, 0xFA,      // JMP $FABA  ; next slot down
            // notScan:
            0xA9, 0xF0, 0x85, 0x36,   // LDA #$F0 / STA $36   CSWL
            0xA9, 0xFD, 0x85, 0x37,   // LDA #$FD / STA $37   CSWH
            0xA9, 0x1B, 0x85, 0x38,   // LDA #$1B / STA $38   KSWL
            0xA9, 0xFD, 0x85, 0x39,   // LDA #$FD / STA $39   KSWH
            0x60                      // RTS
        };
        static_assert(kBootScanC800 + sizeof bs <= 0x600,
                      "boot-scan stub must not run into the handler at $CE00");
        std::memcpy(c800_.data() + kBootScanC800, bs, sizeof bs);
    }

    // ── Any other ProDOS driver command ($CD60) ───────────────────────────
    // FORMAT ($03) is a ProDOS driver command too (TN ProDOS #16: a
    // formatter calls it through the driver), and a block device "need only
    // lay down marks", which an image does not need: $28 for an empty bay,
    // $2B for a protected one, success otherwise — the same answers the
    // SmartPort FORMAT call gives. $CnFE has advertised format since the
    // real Liron page went in ($BF), and the driver used to answer $27.
    // Anything else is a bad command: $27.
    {
        const uint8_t statReg = static_cast<uint8_t>(0x80 + slot_ * 16 + 0x04);
        const uint8_t oc[] = {
            0xC9, 0x03,            // CMP #$03    ; FORMAT?
            0xD0, 0x13,            // BNE bad     (+19)
            0x2C, statReg, 0xC0,   // BIT $C0n4   ; N = no media, V = WP
            0x10, 0x04,            // BPL +4      ; media present
            0xA9, 0x28,            // LDA #$28    ; no device connected
            0x38,                  // SEC
            0x60,                  // RTS
            0x50, 0x04,            // BVC +4      ; writable
            0xA9, 0x2B,            // LDA #$2B    ; write protected
            0x38,                  // SEC
            0x60,                  // RTS
            0xA9, 0x00,            // LDA #$00    ; formatted
            0x18,                  // CLC
            0x60,                  // RTS
            // bad:
            0xA9, 0x27,            // LDA #$27    ; ProDOS I/O error
            0x38,                  // SEC
            0x60                   // RTS
        };
        static_assert(kOtherCmdC800 + sizeof oc <= 0x600,
                      "the command fallback must not run into the handler at $CE00");
        std::memcpy(c800_.data() + kOtherCmdC800, oc, sizeof oc);
    }

    // ── ProDOS unit byte → bay ($CD80) ─────────────────────────────────
    // $43 is DSSS0000, and the slot bits matter: ProDOS 8 mirrors SmartPort
    // units 3 and 4 of a slot-5 card into slot 2 and calls THIS driver with
    // $43 = $20/$A0. The Liron's own decode ($CC1E in roms/liron.rom) is
    // bay = D + 2 x (S2 of $43 != S2 of its own slot). Taking bit 7 alone
    // made S2,D1 read unit 1 again, so one image was listed twice (S5,D1 and
    // S2,D1) by A2FileCmd on a fresh browser profile (2026-10-25).
    {
        const uint8_t unitReg = static_cast<uint8_t>(0x80 + slot_ * 16 + 0x00);
        const uint8_t us[] = {
            0xA5, 0x43,                              // LDA $43
            0x49, static_cast<uint8_t>(slot_ << 4),  // EOR #slot<<4
            0x0A,                  // ASL A       ; C = D, bit 7 = S2 differs
            0x08,                  // PHP
            0x0A,                  // ASL A       ; C = S2 differs
            0xA9, 0x00,            // LDA #$00
            0x2A,                  // ROL A       ; A = 0 or 1
            0x28,                  // PLP         ; C = D
            0x2A,                  // ROL A       ; A = 2 x S2 differs + D
            0x8D, unitReg, 0xC0,   // STA $C0n0   ; latch unit
            0x60                   // RTS
        };
        static_assert(kUnitSelC800 + sizeof us <= 0x600,
                      "the unit decode must not run into the handler at $CE00");
        std::memcpy(c800_.data() + kUnitSelC800, us, sizeof us);
    }

    constexpr size_t kHandlerOff = 0x600;   // $CE00
    std::memcpy(c800_.data() + kHandlerOff, kHandler, sizeof(kHandler));
    const uint8_t base = static_cast<uint8_t>(0x80 + slot_ * 16);
    for (size_t off : kRegPatch) {
        const uint8_t reg = c800_[kHandlerOff + off] & 0x0F;
        c800_[kHandlerOff + off] = static_cast<uint8_t>(base + reg);
    }
}

uint8_t SmartPortCard::expansionRomRead(uint16_t offset)
{
    return offset < c800_.size() ? c800_[offset] : 0xFF;
}

// SmartPort call semantics (Apple IIc Tech Ref / Liron dump $CBE6).
// spCollect_: [0]=cmd, [1]=pcount, [2]=unit, [3]/[4]=buffer/status-list
// pointer, [5..] cmd-specific (statcode, or 3-byte block number).
// The dump indexes $CDE3 with CPX #$0A at $CC5B: cmd >= $0A is $01.
uint8_t SmartPortCard::spExecute()
{
    spResult_.clear();
    spResultPos_ = 0;
    spPushPages_ = 0;
    auto fail = [&](uint8_t e) { spError_ = e; return e; };
    auto ok   = [&]()          { spError_ = 0;  return uint8_t{0}; };

    if (spCollectN_ < 3) return fail(0x01);            // bad command
    const uint8_t cmd    = spCollect_[0];
    const uint8_t pcount = spCollect_[1];
    const uint8_t unitNo = spCollect_[2];
    // Trace de chantier, armee par l'environnement — voir 2026-08-30.
    static const bool trace = std::getenv("POM2_SP_TRACE") != nullptr;
    if (trace)
        std::fprintf(stderr, "[sp] cmd=%02X pcount=%u unit=%u collectN=%u "
                     "p=[%02X %02X %02X %02X]\n", cmd, pcount, unitNo,
                     unsigned(spCollectN_), spCollect_[3], spCollect_[4],
                     spCollect_[5], spCollect_[6]);

    auto unitFor = [&](uint8_t n) -> SmartPortUnit* {
        if (n == 0 || n > static_cast<uint8_t>(unitCount_)) return nullptr;
        return units_[n - 1].get();
    };

    // What the host firmware decides before a packet reaches a drive, as the
    // real Liron EPROM does it (smartport_liron_parity runs both): a command
    // past $09 is $01 ($CC5B CPX #$0A); unit 0 is the controller, which only
    // STATUS, CONTROL and INIT may address — anything else is $11 BADUNIT
    // (Apple IIc Technical Reference, Protocol Converter error codes); and
    // the caller's parameter count is never checked — the firmware sends
    // its own count from the table at $CDE3, so a wrong one is no error.
    if (cmd >= 0x0A) return fail(0x01);
    if (unitNo == 0 && cmd != 0x00 && cmd != 0x04 && cmd != 0x05)
        return fail(0x11);

    switch (cmd) {
        case 0x00: {                                   // STATUS
            const uint8_t code = spCollect_[5];
            if (unitNo == 0) {
                if (code != 0x00) return fail(0x21);   // bad status code
                // Controller status (TN SmartPort #2): device count, then the
                // interrupt status — bit 6 SET means "no interrupt", and no
                // unit here ever interrupts (the UniDisk 3.5 sets it too);
                // a zero told an interrupt handler polling the chain that
                // this card wanted service — then vendor $0000
                // ("undetermined"), interface version, two reserved.
                spResult_ = { static_cast<uint8_t>(unitCount_),
                              0x40, 0, 0, 0, 0, 0, 0 };
                return ok();
            }
            SmartPortUnit* u = unitFor(unitNo);
            if (!u) return fail(0x28);                 // no device connected
            const bool     loaded = guestLoaded(unitNo - 1u);
            const uint32_t blocks = loaded ? u->blockCount() : 0;
            const bool     is35   = u->kindKey() == SmartPort35Unit::kKindKey;
            // General status byte: bit7 block dev, bit6 write allowed,
            // bit5 read allowed, bit4 online, bit3 format allowed,
            // bit2 write-protected, bit1 interrupting, bit0 open. Format
            // is the device's capability, set with or without a disk, as
            // the bus responder (SmartPortBusDevice) answers it.
            uint8_t g = 0xA8;                          // block, read, format
            if (loaded) {
                g |= 0x10;                             // online
                if (u->isWriteProtected()) g |= 0x04;
                else                       g |= 0x40;  // writable
            }
            if (code == 0x00) {
                spResult_ = { g,
                              static_cast<uint8_t>(blocks),
                              static_cast<uint8_t>(blocks >> 8),
                              static_cast<uint8_t>(blocks >> 16) };
                return ok();
            }
            if (code == 0x03) {                        // DIB
                spResult_ = { g,
                              static_cast<uint8_t>(blocks),
                              static_cast<uint8_t>(blocks >> 8),
                              static_cast<uint8_t>(blocks >> 16) };
                // The name: "DISK 3.5" is what both the UniDisk 3.5 and the
                // Apple 3.5 Drive answer (TN SmartPort #7), and what a
                // program identifying drives by name looks for.
                const std::string_view kId = is35 ? "DISK 3.5" : "POM2 HARDDISK";
                spResult_.push_back(static_cast<uint8_t>(kId.size()));
                for (size_t i = 0; i < 16; ++i)
                    spResult_.push_back(i < kId.size()
                        ? static_cast<uint8_t>(kId[i]) : uint8_t{' '});
                spResult_.push_back(is35 ? 0x01 : 0x02);  // type: 3.5 / disk
                // UniDisk 3.5 #5 / SmartPort #7: type $01 subtype $00
                // (no extended SmartPort, no disk-switched). $C0 is the
                // Apple 3.5 Drive on the IIgs, not this card. HDV is type
                // $02 with bit 5 set (not removable) and bit 7 clear.
                spResult_.push_back(is35 ? 0x00 : 0x20);
                spResult_.push_back(0x00);                // firmware 1.0,
                spResult_.push_back(0x01);                // as the bus responder
                return ok();
            }
            if (code == 0x05 && is35) {
                // "Return UniDisk 3.5 status": $00, soft error, retries,
                // $00, A/X/Y/P after a CONTROL EXECUTE — all zero here.
                spResult_.assign(8, 0x00);
                return ok();
            }
            // No DCB ($01), not a character device ($02), nothing else
            // defined: BADCTL, as the manual gives it for the UniDisk 3.5.
            return fail(0x21);
        }
        case 0x01:                                     // READ BLOCK
        case 0x02: {                                   // WRITE BLOCK
            SmartPortUnit* u = unitFor(unitNo);
            if (!u) return fail(0x28);
            if (!guestLoaded(unitNo - 1u)) return fail(0x2F);   // device offline
            const uint32_t block = static_cast<uint32_t>(spCollect_[5])
                                 | static_cast<uint32_t>(spCollect_[6]) << 8
                                 | static_cast<uint32_t>(spCollect_[7]) << 16;
            if (block >= u->blockCount()) return fail(0x2D);  // bad block
            if (cmd == 0x01) {
                spResult_.resize(kBlockBytes);
                if (!u->readBlock(block, spResult_.data())) {
                    spResult_.clear();
                    return fail(0x27);                 // I/O error
                }
                noteAccess(u);
                return ok();
            }
            if (u->isWriteProtected()) return fail(0x2B);
            // Arm the legacy reg-0x3 streaming machinery: the 6502 pushes
            // 512 bytes, writeDataByte commits at wrap + latches ioError_.
            const size_t idx = unitNo - 1;
            activeUnit_          = idx;
            selectedBlock_[idx]  = static_cast<uint16_t>(block);
            streamOffset_[idx]   = 0;
            writeBufPrimed_[idx] = false;
            ioError_[idx]        = false;
            spPushPages_         = 2;                  // 512 bytes
            return ok();
        }
        case 0x03: {                                   // FORMAT
            // Apple IIgs Firmware Reference, SmartPort FORMAT / INIT
            // (pp. 7-14, 7-17): one parameter, the unit number.
            SmartPortUnit* u = unitFor(unitNo);
            if (!u) return fail(0x28);
            if (!guestLoaded(unitNo - 1u)) return fail(0x2F);
            if (u->isWriteProtected()) return fail(0x2B);
            return ok();  // block devices "need only lay down marks" — no-op
        }
        case 0x04: {                                   // CONTROL
            // Code 0 (device reset) is a no-op success. Code 4 is EJECT
            // since May 1988 (TN SmartPort #2): "all SmartPort devices which
            // support removable media must support this call", and a fixed
            // one "should simply return from this call without an error".
            // A 3.5" disk really leaves the drive: the request goes to the
            // UI thread, which ejects through the three-phase path (the
            // write-back cannot run here, under the machine lock).
            // Unit 0 is the controller: codes 0 and 1 (enable / disable its
            // interrupts, which on a //c are the ACIA's) answer $1F — this
            // card, like the Liron, has none — and any other code $21, as
            // the Liron firmware answers them. A number past the chain is
            // no device ($28). Every other code is $21 — no DCB, newline or
            // interrupts on these units, and the UniDisk's own code
            // download/EXECUTE is not modelled (SmartPortBusDevice agrees).
            const uint8_t code = spCollect_[5];
            if (unitNo == 0) return fail(code <= 0x01 ? 0x1F : 0x21);
            if (unitNo > static_cast<uint8_t>(unitCount_)) return fail(0x28);
            if (code != 0x00 && code != 0x04) return fail(0x21);
            SmartPortUnit* u = unitFor(unitNo);
            if (code == 0x04 && u && guestLoaded(unitNo - 1u) &&
                u->kindKey() == SmartPort35Unit::kKindKey)
                requestGuestEject(unitNo - 1, unitMediaId(u));
            return ok();
        }
        case 0x05:                                     // INIT
            // Unit 0 only. The Liron firmware passes another number on to
            // the drive as an INIT and reports what that drive answers,
            // which depends on the chain; $28 here is POM2's own choice.
            if (unitNo != 0) return fail(0x28);
            return ok();
        default:
            return fail(0x01);                         // bad command
    }
}

// ── MountableMediaCard ──────────────────────────────────────────────────
// Each unit is one media bay. The bay's media kind is user-selectable
// (empty / 3.5" / HDV); mounting requires a kind to have been chosen first
// (the Slot Manager surfaces the type combo next to the mount control).

MediaBayInfo SmartPortCard::bayInfo(int bay) const
{
    MediaBayInfo info;
    const SmartPortUnit* u = unit(static_cast<size_t>(bay));
    if (!u) return info;  // empty bay → "(empty)" type, no media
    info.kindLabel         = std::string(u->kindLabel());
    info.typeKey           = std::string(u->kindKey());
    info.path              = u->path();
    info.lastError         = u->lastError();
    info.blockCount        = u->blockCount();
    info.loaded            = u->isLoaded();
    info.busy              = u->isBusy();
    info.writeProtected    = u->isWriteProtected();
    info.fileWriteProtected = u->isFileWriteProtected();
    info.writeBackEnabled  = u->isWriteBackEnabled();
    info.hasUnsavedChanges = u->hasUnsavedChanges();
    if (auto* b = u->blockBacking()) {
        info.persistenceState = b->persistenceState();
        info.persistenceError = b->persistenceError();
        info.mountId          = b->mountId();
    } else if (auto* u35 = dynamic_cast<const SmartPort35Unit*>(u)) {
        const auto p = u35->image().persistence();
        info.mountId          = u35->image().mountId();
        info.persistenceState = p.state;
        info.persistenceError = p.error;
    }
    info.supportsWriteBack = true;
    info.supportsTypeSelect = true;
    return info;
}

void SmartPortCard::setBayHostWriteProtected(int bay, bool on)
{
    if (bay < 0 || bay >= bayCount()) return;
    if (auto* u = unit(static_cast<std::size_t>(bay)))
        u->setHostWriteProtected(on);
}

bool SmartPortCard::adoptBay(int bay, Block512Backing::PreparedImage&& prepared,
                             std::string& errOut)
{
    errOut.clear();
    SmartPortUnit* u = unit(static_cast<size_t>(bay));
    if (!u) {
        errOut = "select a media type for this unit first";
        return false;
    }
    // A unit whose kind has no block backing — the 3.5" unit — reports that
    // by returning false without setting lastError(). Leave errOut empty so
    // the caller falls back to the one-phase mountBay() for it, the same
    // discrimination MediaMount.cpp's mountBlockLike makes.
    if (!u->adoptImage(std::move(prepared))) {
        errOut = u->lastError();
        return false;
    }
    return true;
}

bool SmartPortCard::mountBay(int bay, const std::string& path,
                             std::string& errOut)
{
    SmartPortUnit* u = unit(static_cast<size_t>(bay));
    if (!u) {
        errOut = "select a media type for this unit first";
        return false;
    }
    if (!u->loadImage(path)) {
        errOut = u->lastError();
        return false;
    }
    return true;
}

bool SmartPortCard::ejectBay(int bay)
{
    if (SmartPortUnit* u = unit(static_cast<size_t>(bay))) return u->eject();
    return false;
}

bool SmartPortCard::flushBay(int bay, std::string& errOut)
{
    // Inline fallback for callers that do not ask `prepareFlushBay`.
    // Hunt #21: the coordinator's four two-phase sites already ask, and
    // a 3.5" unit now answers; this path remains for `flushAll` (worker
    // stopped) and for any HDV bay whose `prepareEjectBay` already ran.
    errOut.clear();
    SmartPortUnit* u = unit(static_cast<size_t>(bay));
    if (!u || !u->hasUnsavedChanges()) return true;
    if (u->saveDirty()) return true;
    errOut = u->lastError();
    if (errOut.empty()) errOut = "the image could not be saved";
    return false;
}

bool SmartPortCard::prepareFlushBay(int bay, PendingBayFlush& out,
                                   std::string& errOut)
{
    errOut.clear();
    out = PendingBayFlush{};
    SmartPortUnit* u = unit(static_cast<size_t>(bay));
    if (!u) return false;
    // HDV bays are block-backed: empty error → the caller uses
    // prepareEjectBay. A 3.5" unit is the Liron case hunt #20 closed
    // on that card and this one never answered.
    auto* u35 = dynamic_cast<SmartPort35Unit*>(u);
    if (!u35) return false;
    Disk35Image::PendingWriteBack pending = u35->image().takeWriteBack();
    if (!pending.error.empty()) {
        errOut = pending.error;
        u35->image().restoreDirtyFor(pending.lineage);
        return false;
    }
    out.valid = pending.valid;
    out.seq   = pending.seq;
    out.lineage = pending.lineage;
    out.path  = std::move(pending.path);
    out.bytes = std::move(pending.bytes);
    out.bitstream = std::move(pending.bitstream);
    return true;
}

void SmartPortCard::restoreFlushBayDirty(int bay, uint64_t lineage)
{
    if (auto* u35 = dynamic_cast<SmartPort35Unit*>(unit(static_cast<size_t>(bay))))
        (void)u35->image().restoreDirtyFor(lineage);
}

bool SmartPortCard::prepareEjectBay(int bay,
                                    Block512Backing::PendingWriteBack& out,
                                    std::string& errOut)
{
    errOut.clear();
    SmartPortUnit* u = unit(static_cast<size_t>(bay));
    // No unit, or a unit kind with no block backing (the 3.5" one): empty
    // errOut tells the caller to fall back to the inline eject().
    if (!u) return false;
    return u->detachImage(out);
}

void SmartPortCard::restoreBayDirty(int bay,
                                    const std::vector<uint32_t>& indices)
{
    if (SmartPortUnit* u = unit(static_cast<size_t>(bay)))
        u->restoreDirtyBlocks(indices);
}

void SmartPortCard::setBayWriteBack(int bay, bool on)
{
    if (SmartPortUnit* u = unit(static_cast<size_t>(bay)))
        u->setWriteBackEnabled(on);
}

std::vector<std::pair<std::string, std::string>>
SmartPortCard::bayTypeOptions(int /*bay*/) const
{
    return {
        { std::string(),                              "(empty)" },
        { std::string(SmartPort35Unit::kKindKey),     "3.5\" 800K" },
        { std::string(SmartPortHdvUnit::kKindKey),    "ProDOS HDV" },
    };
}

void SmartPortCard::setBayType(int bay, const std::string& kindKey)
{
    if (bay < 0 || static_cast<size_t>(bay) >= kMaxUnits) return;
    if (kindKey.empty()) {
        setUnit(static_cast<size_t>(bay), nullptr);   // clear the bay
        return;
    }
    if (auto unit = makeSmartPortUnit(kindKey))
        setUnit(static_cast<size_t>(bay), std::move(unit));
}

void SmartPortCard::borrowBayType(int bay, std::string originalKind)
{
    if (bay < 0 || static_cast<size_t>(bay) >= kMaxUnits) return;
    borrowedKind_[static_cast<size_t>(bay)] = std::move(originalKind);
}

std::string SmartPortCard::persistedBayKind(int bay) const
{
    if (bay < 0 || static_cast<size_t>(bay) >= kMaxUnits) return {};
    const auto idx = static_cast<size_t>(bay);
    if (borrowedKind_[idx]) return *borrowedKind_[idx];
    return units_[idx] ? std::string(units_[idx]->kindKey()) : std::string();
}

} // namespace pom2
