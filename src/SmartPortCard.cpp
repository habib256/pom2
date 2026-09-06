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

namespace pom2 {

namespace {

constexpr uint8_t kBootOff   = 0x20;
constexpr uint8_t kDriverOff = 0x50;
constexpr uint8_t kReadOff   = 0x6F;
constexpr uint8_t kWriteOff  = 0xA2;
/// Halt loop the boot routine jumps to when the boot block will not load.
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
    auto old = std::move(units_[idx]);
    units_[idx] = std::move(u);
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
    constexpr size_t kBase = 4 + kMaxUnits * kPerUnit;
    if (len < kBase || data[0] != 'S' || data[1] != 'P') return;
    const uint8_t version = data[2];
    if (version < 1 || version > kSnapVersion) return;
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
            if (len - identOff < kMaxUnits * 8) return;
        }
    }
    activeUnit_ = std::min<size_t>(data[3], kMaxUnits - 1);
    const uint8_t* p = data + 4;
    for (size_t u = 0; u < kMaxUnits; ++u) {
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
    return static_cast<int>(kMaxUnits);
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
    const SmartPortUnit* u = target();
    return u && u->isLoaded();
}

uint32_t SmartPortCard::BusUnit::blockCount() const
{
    const SmartPortUnit* u = target();
    return (u && u->isLoaded()) ? u->blockCount() : 0;
}

bool SmartPortCard::BusUnit::writeProtected() const
{
    const SmartPortUnit* u = target();
    return !u || u->isWriteProtected();
}

bool SmartPortCard::BusUnit::readBlock(uint32_t block, uint8_t out[512])
{
    SmartPortUnit* u = target();
    if (!u || !u->isLoaded() || !u->readBlock(block, out)) return false;
    if (card_) card_->noteAccess(u);
    return true;
}

bool SmartPortCard::BusUnit::writeBlock(uint32_t block, const uint8_t in[512])
{
    SmartPortUnit* u = target();
    if (!u || !u->isLoaded() || !u->writeBlock(block, in)) return false;
    if (card_) card_->noteAccess(u);
    return true;
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
            return (activeUnit_ < kMaxUnits && ioError_[activeUnit_])
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
        (activeUnit_ < kMaxUnits) ? units_[activeUnit_].get() : nullptr;
    uint32_t blocks = (u && u->isLoaded()) ? u->blockCount() : 0u;
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
            // Modulo (not a bitmask) so the mapping stays correct if kMaxUnits
            // is ever raised to a non-power-of-two for extended SmartPort.
            const size_t u = static_cast<size_t>(v) % kMaxUnits;
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

uint8_t SmartPortCard::statusByte() const
{
    const SmartPortUnit* u = (activeUnit_ < kMaxUnits)
        ? units_[activeUnit_].get() : nullptr;
    uint8_t s = (u && u->isLoaded()) ? 0x00 : 0x80;
    if (!u || u->isWriteProtected()) s |= 0x40;
    if (activeUnit_ < kMaxUnits && ioError_[activeUnit_]) s |= 0x01;  // I/O error
    return s;
}

uint8_t SmartPortCard::readDataByte()
{
    const size_t u = activeUnit_;
    SmartPortUnit* unit = units_[u].get();
    if (!unit || !unit->isLoaded()) {
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
            ioError_[u] = true;
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
    SmartPortUnit* unit = units_[u].get();
    if (!unit || !unit->isLoaded()) {
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
            ioError_[u] = true;
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
    //   $CnE0-$CnE2  boot-failure halt loop
    //   $CnE3-$CnFD  LEFT ALONE — real dump ID bytes
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
     .label("bootErr").jmp("halt");

    // ── ProDOS driver dispatch ($Cn50) ─────────────────────────────────
    // ProDOS calls here with $42 = command, $43 = unit, $44/$45 = buffer,
    // $46/$47 = block. Unit byte bit 7 = drive (0 = drive 1, 1 = drive 2).
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
             0xA5, 0x43,            // LDA $43           ; unit byte
             0x0A,                  // ASL A             ; bit7 → carry
             0xA9, 0x00,
             0x2A,                  // ROL A             ; A = drive (0 or 1)
             0x8D, static_cast<uint8_t>(kDeviceBase + 0x00), 0xC0,
                                    // STA $C0n0         ; latch unit
             0xA5, 0x42 })          // LDA $42           ; command
     .branch(0xF0, "dispStatus")    // BEQ — STATUS ($00)
     .emit({ 0xC9, 0x01 })          // CMP #$01
     .branch(0xF0, "read")
     .emit({ 0xC9, 0x02 })          // CMP #$02
     .branch(0xF0, "write")
     .emit({ 0xA9, 0x27,            // bad cmd (incl. FORMAT $03): LDA #$27 —
                                    // a REAL driver error code; the old $01
                                    // is not in the ProDOS $27/$28/$2B set
                                    // and surfaced as a bogus code in Filer.
             0x38,                  // SEC
             0x60 })                // RTS
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

    a.region("halt", kHaltOff, kHaltOff + 3).jmp("halt");

    // $CnFE capability byte (ProDOS 8 TN #21): bit3 format, bit2 WRITE,
    // bit1 read, bit0 status, bits5-4 volume count. Was $13 — read+status
    // only — which advertised a READ-ONLY device to capability-inspecting
    // utilities despite the write path being fully implemented.
    a.region("tail", 0xFE, pom2::kSlotRomBytes)
     .emit({ 0x17 })                // read+WRITE+status, 2 units
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
        0x85, 0x44, 0x68, 0x85, 0x43, 0x68, 0x85, 0x42, 0x8A, 0xC9, 0x01, 0x60,
    };
    // Register-operand lo bytes (0x80+reg placeholders) → this slot's base.
    static constexpr size_t kRegPatch[] =
        { 24, 31, 66, 74, 91, 100, 113, 118, 127, 138, 149 };

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

        // STATUS (ProDOS cmd $00): no media → $28, write-protected → $2B, else
        // CLC with the total block count in X (low) / Y (high) so a volume
        // scanner (BITSY, ONLINE) can size the device. Formatters pre-flight
        // through here too.
        const uint8_t st[] = {
            0x2C, statReg, 0xC0,   // BIT $C0n4
            0x10, 0x04,            // BPL +4      ; media present
            0xA9, 0x28,            // LDA #$28    ; no device connected
            0x38,                  // SEC
            0x60,                  // RTS
            0x50, 0x04,            // BVC +4      ; not write-protected
            0xA9, 0x2B,            // LDA #$2B    ; write protected
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

// SmartPort call semantics (Apple IIGS Firmware Reference / Tech Notes).
// spCollect_: [0]=cmd, [1]=pcount, [2]=unit, [3]/[4]=buffer/status-list
// pointer, [5..] cmd-specific (statcode, or 3-byte block number).
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
        if (n == 0 || n > kMaxUnits) return nullptr;
        return units_[n - 1].get();
    };

    switch (cmd) {
        case 0x00: {                                   // STATUS
            if (pcount != 3) return fail(0x04);        // bad param count
            const uint8_t code = spCollect_[5];
            if (unitNo == 0) {
                if (code != 0x00) return fail(0x21);   // bad status code
                // Controller status: device count + 7 reserved bytes.
                spResult_ = { static_cast<uint8_t>(kMaxUnits),
                              0, 0, 0, 0, 0, 0, 0 };
                return ok();
            }
            SmartPortUnit* u = unitFor(unitNo);
            if (!u) return fail(0x28);                 // no device connected
            const bool     loaded = u->isLoaded();
            const uint32_t blocks = loaded ? u->blockCount() : 0;
            // General status byte: bit7 block dev, bit6 write allowed,
            // bit5 read allowed, bit4 online, bit3 format allowed,
            // bit2 write-protected, bit1 interrupting, bit0 open.
            uint8_t g = 0xA0;                          // block + readable
            if (loaded) {
                g |= 0x10;                             // online
                if (u->isWriteProtected()) g |= 0x04;
                else                       g |= 0x48;  // writable+formattable
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
                static constexpr char kId[] = "POM2 SMARTPORT";
                const size_t idLen = sizeof(kId) - 1;
                spResult_.push_back(static_cast<uint8_t>(idLen));
                for (size_t i = 0; i < 16; ++i)
                    spResult_.push_back(i < idLen
                        ? static_cast<uint8_t>(kId[i]) : uint8_t{' '});
                const bool is35 = u->kindKey() == SmartPort35Unit::kKindKey;
                spResult_.push_back(is35 ? 0x01 : 0x02);  // type: 3.5 / disk
                spResult_.push_back(is35 ? 0x80 : 0x20);  // subtype
                spResult_.push_back(0x01);                // firmware version
                spResult_.push_back(0x00);
                return ok();
            }
            return fail(0x21);
        }
        case 0x01:                                     // READ BLOCK
        case 0x02: {                                   // WRITE BLOCK
            if (pcount != 3) return fail(0x04);
            SmartPortUnit* u = unitFor(unitNo);
            if (!u) return fail(0x28);
            if (!u->isLoaded()) return fail(0x2F);     // device offline
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
            SmartPortUnit* u = unitFor(unitNo);
            if (!u) return fail(0x28);
            if (!u->isLoaded()) return fail(0x2F);
            if (u->isWriteProtected()) return fail(0x2B);
            return ok();  // block devices "need only lay down marks" — no-op
        }
        case 0x04: {                                   // CONTROL
            if (pcount != 3) return fail(0x04);
            // Code 0 (device reset) is a no-op success; anything needing
            // the control list's data is unsupported (the stub can't copy
            // guest→device lists) → bad control code.
            return spCollect_[5] == 0x00 ? ok() : fail(0x21);
        }
        case 0x05:                                     // INIT
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
    info.writeBackEnabled  = u->isWriteBackEnabled();
    info.hasUnsavedChanges = u->hasUnsavedChanges();
    info.supportsWriteBack = true;
    info.supportsTypeSelect = true;
    return info;
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

} // namespace pom2
