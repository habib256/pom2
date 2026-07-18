// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "SmartPortCard.h"

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

void emit(std::array<uint8_t, 256>& rom, uint8_t& pc,
          std::initializer_list<uint8_t> bytes)
{
    for (uint8_t b : bytes) rom[pc++] = b;
}

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
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
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
    // SmartPort call engine: Ctrl-Reset mid-call must not leave the
    // engine half-armed (stale $C0n9 result replay, $C0nD still
    // advertising push pages).
    spCollectN_ = 0;
    spCollect_.fill(0);
    spResult_.clear();
    spResultPos_ = 0;
    spPushPages_ = 0;
    spError_     = 0;
    spArmed_     = false;
    if (audibleMotorOn_ && sound_) sound_->motor(false, true);
    audibleMotorOn_ = false;
    lastAccessCycle_ = 0;
}

void SmartPortCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.push_back('S'); out.push_back('P'); out.push_back(2);
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
    // v2 trailer: the SmartPort call engine. A dispatch (BEGIN → EXECUTE
    // → 512-byte pull) spans thousands of cycles and can straddle a
    // rewind-ring frame boundary; restoring without this resumed the
    // 6502 inside the $CE00 pull loop against an EMPTY spResult_ —
    // reg $C0n9 then streamed 512 × $00 into the guest buffer.
    out.push_back(static_cast<uint8_t>(spCollectN_));
    out.insert(out.end(), spCollect_.begin(), spCollect_.end());
    out.push_back(static_cast<uint8_t>(spResult_.size() & 0xFF));
    out.push_back(static_cast<uint8_t>(spResult_.size() >> 8));
    out.insert(out.end(), spResult_.begin(), spResult_.end());
    out.push_back(static_cast<uint8_t>(spResultPos_ & 0xFF));
    out.push_back(static_cast<uint8_t>(spResultPos_ >> 8));
    out.push_back(spPushPages_);
    out.push_back(spError_);
    out.push_back(spArmed_ ? 1 : 0);
}

void SmartPortCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    constexpr size_t kPerUnit = 6 + kBlockBytes;
    if (len < 4 + kMaxUnits * kPerUnit ||
        data[0] != 'S' || data[1] != 'P' ||
        (data[2] != 1 && data[2] != 2))
        return;
    const uint8_t ver = data[2];
    activeUnit_ = std::min<size_t>(data[3], kMaxUnits - 1);
    const uint8_t* p = data + 4;
    for (size_t u = 0; u < kMaxUnits; ++u) {
        selectedBlock_[u] = static_cast<uint16_t>(p[0] | (p[1] << 8));
        streamOffset_[u]  = static_cast<size_t>(p[2] | (p[3] << 8)) % kBlockBytes;
        writeBufPrimed_[u] = p[4] != 0;
        ioError_[u]        = p[5] != 0;
        std::memcpy(writeBuf_[u].data(), p + 6, kBlockBytes);
        p += kPerUnit;
    }
    // Engine defaults first — v1 snapshots (and truncated v2 chunks)
    // restore with the engine idle, which at worst re-fails a call the
    // guest will retry.
    spCollectN_ = 0; spCollect_.fill(0);
    spResult_.clear(); spResultPos_ = 0;
    spPushPages_ = 0; spError_ = 0; spArmed_ = false;
    if (ver == 2) {
        const uint8_t* end = data + len;
        auto have = [&](size_t n) {
            return static_cast<size_t>(end - p) >= n;
        };
        if (!have(1 + spCollect_.size() + 2)) return;
        spCollectN_ = std::min<size_t>(*p++, spCollect_.size());
        std::memcpy(spCollect_.data(), p, spCollect_.size());
        p += spCollect_.size();
        const size_t rn = static_cast<size_t>(p[0] | (p[1] << 8));
        p += 2;
        if (!have(rn + 5)) return;
        spResult_.assign(p, p + rn);
        p += rn;
        spResultPos_ = std::min<size_t>(
            static_cast<size_t>(p[0] | (p[1] << 8)), spResult_.size());
        p += 2;
        spPushPages_ = *p++;
        spError_     = *p++;
        spArmed_     = *p != 0;
    }
    // Media didn't move; the read cache just re-fills from the same block.
    readCacheValid_.fill(false);
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

void SmartPortCard::noteAccess()
{
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
            spArmed_     = true;                // arms exactly one EXECUTE
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

    // ── ProDOS / SmartPort signature ───────────────────────────────────
    rom_[0x00] = 0x4C;                          // JMP $Cn20
    rom_[0x01] = kBootOff;
    rom_[0x02] = kSlotRomHi;
    rom_[0x03] = 0x00;
    rom_[0x05] = 0x03;
    // $Cn07 identifies the controller class to the //c-class boot firmware.
    // $3C = Disk II (the //c internal $C607!) — claiming it makes a //c see
    // TWO Disk II controllers (slot 5 + the internal slot 6) and its boot
    // scan corrupts. $00 = SmartPort, which triggers a SmartPort enumeration
    // this block-only stub can't service. $01 = plain ProDOS block device
    // (non-removable, like ProDOSHardDiskCard) → the //c boots it via the
    // standard JMP $Cn00 path with no Disk II / SmartPort confusion, and //e
    // boot (bootFromSlot, ProDOS via $CnFF) is unaffected. See
    // project_iic_smartport_boot.
    rom_[0x07] = 0x01;                          // ProDOS block device
    // $CnFE capability byte (ProDOS 8 TN #21): bit3 format, bit2 WRITE,
    // bit1 read, bit0 status, bits5-4 volume count. Was $13 — read+status
    // only — which advertised a READ-ONLY device to capability-inspecting
    // utilities despite the write path being fully implemented.
    rom_[0xFE] = 0x17;                          // read+WRITE+status, 2 units
    rom_[0xFF] = kDriverOff;

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
    rom_[0x0A] = 0x4C;                           // JMP $Cn50
    rom_[0x0B] = kDriverOff;
    rom_[0x0C] = kSlotRomHi;
    // SmartPort-convention entry = ProDOS entry + 3 = $Cn0D (real Liron:
    // $CnFF=$0A, SmartPort dispatch $Cn0D). Routes to the full SmartPort
    // call handler in the $C800 bank (see buildC800) — STATUS/DIB, READ,
    // WRITE, FORMAT, CONTROL, INIT with real error codes. Callers that
    // hardcode entry+3 used to fall through NOP padding INTO the boot
    // routine at $Cn20 (booting block 0 over $0800).
    //
    // BIT $CFFF first — mandatory expansion-window release, same dance
    // as the real Liron firmware. On a IIe any $C3xx access with
    // SLOTC3ROM off latches `intC8Rom` (80-col COUT, ProDOS slot scans),
    // and while latched a bare JMP $CE00 fetches INTERNAL ROM at $CE00
    // (Memory::memRead intC8Rom branch) — arbitrary firmware bytes
    // instead of our handler. $CFFF clears the latch + releases any
    // other card's claim; fetching the JMP at $Cn10 then re-claims the
    // window for THIS slot (SlotBus::slotRomRead), so $CE00 is live.
    rom_[0x0D] = 0x2C;                           // BIT $CFFF
    rom_[0x0E] = 0xFF;
    rom_[0x0F] = 0xCF;
    rom_[0x10] = 0x4C;                           // JMP $CE00
    rom_[0x11] = 0x00;
    rom_[0x12] = 0xCE;

    // ── Boot routine ($Cn20) ───────────────────────────────────────────
    uint8_t pc = kBootOff;
    emit(rom_, pc, {
        0xA9, 0x01,            // LDA #$01           ; ProDOS read cmd
        0x85, 0x42,
        0xA9, kUnitDrv1,       // unit = slot×16, drive 1
        0x85, 0x43,
        0xA9, 0x00,
        0x85, 0x44,            // buffer LO = $00
        0xA9, 0x08,
        0x85, 0x45,            // buffer HI = $08
        0xA9, 0x00,
        0x85, 0x46,            // block LO = 0
        0x85, 0x47,            // block HI = 0
        0x20, kDriverOff, kSlotRomHi, // JSR $Cn50 (driver)
        0xB0, 0x07,            // BCS  error
        0xA2, kUnitDrv1,       // X = unit
        0xA9, 0x00,
        0x4C, 0x01, 0x08,      // JMP $0801
        0x4C, 0xE0, kSlotRomHi // error: JMP $CnE0 (halt loop)
    });

    rom_[0xE0] = 0x4C;
    rom_[0xE1] = 0xE0;
    rom_[0xE2] = kSlotRomHi;

    // ── ProDOS driver dispatch ($Cn50) ─────────────────────────────────
    // ProDOS calls here with $42 = command, $43 = unit, $44/$45 = buffer,
    // $46/$47 = block. Unit byte bit 7 = drive (0 = drive 1, 1 = drive 2).
    pc = kDriverOff;
    emit(rom_, pc, {
        0xA5, 0x43,            // LDA $43           ; unit byte
        0x0A,                  // ASL A             ; bit7 → carry
        0xA9, 0x00,
        0x2A,                  // ROL A             ; A = drive (0 or 1)
        0x8D, static_cast<uint8_t>(kDeviceBase + 0x00), 0xC0,
                               // STA $C0n0         ; latch unit
        0xA5, 0x42,            // LDA $42           ; command
        0xC9, 0x01,            // CMP #$01
        0xF0, 0x10,            // BEQ read   (+16)
        0xC9, 0x02,            // CMP #$02
        0xF0, 0x39,            // BEQ write  (+57: skip the 45-byte read block)
        0xC9, 0x00,            // CMP #$00
        0xF0, 0x04,            // BEQ status (+4)
        0xA9, 0x27,            // bad cmd (incl. FORMAT $03): LDA #$27 —
                               // a REAL driver error code; the old $01 is
                               // not in the ProDOS $27/$28/$2B set and
                               // surfaced as a bogus code in Filer.
        0x38,                  // SEC
        0x60,                  // RTS
        // status: jump to the full STATUS routine at $CnC0 (returns the
        // block count in X/Y). Kept 4 bytes (JMP + NOP pad) so the BEQ
        // read/write offsets above stay valid — pinned by
        // tests/smartport_write_dispatch_test.cpp.
        0x4C, 0xC0, kSlotRomHi, // status: JMP $CnC0
        0xEA                    // pad
    });

    // ── STATUS routine ($CnC0) ─────────────────────────────────────────
    // ProDOS STATUS (cmd $00) pre-flights the device (TRM driver
    // conventions): no media → SEC + $28 "no device connected", write-
    // protected → SEC + $2B, else CLC with total blocks in X (low) /
    // Y (high) from $C0n5/$C0n6 so a volume scanner (BITSY, ONLINE) can
    // size it. The unit was latched via $C0n0 at the top of the dispatch;
    // $C0n4 is the side-effect-free status byte (bit7 no-media, bit6 WP).
    // Formatters that pre-flight STATUS used to get CLC on an empty/WP
    // bay. Exactly 32 bytes — fills $CnC0-$CnDF up to the $CnE0 halt loop.
    {
        uint8_t sp = 0xC0;
        emit(rom_, sp, {
            0xAD, static_cast<uint8_t>(kDeviceBase + 0x04), 0xC0, // LDA $C0n4
            0x29, 0x80,        // AND #$80    ; no-media bit
            0xF0, 0x04,        // BEQ +4 (media present)
            0xA9, 0x28,        // LDA #$28    ; no device connected
            0x38,              // SEC
            0x60,              // RTS
            0xAD, static_cast<uint8_t>(kDeviceBase + 0x04), 0xC0, // LDA $C0n4
            0x29, 0x40,        // AND #$40    ; WP bit
            0xF0, 0x04,        // BEQ +4 (writable)
            0xA9, 0x2B,        // LDA #$2B    ; write protected
            0x38,              // SEC
            0x60,              // RTS
            0xAE, static_cast<uint8_t>(kDeviceBase + 0x05), 0xC0, // LDX $C0n5
            0xAC, static_cast<uint8_t>(kDeviceBase + 0x06), 0xC0, // LDY $C0n6
            0xA9, 0x00,        // LDA #$00
            0x18,              // CLC
            0x60               // RTS
        });
    }

    const uint8_t blkLoReg = static_cast<uint8_t>(kDeviceBase + 0x01);
    const uint8_t blkHiReg = static_cast<uint8_t>(kDeviceBase + 0x02);
    const uint8_t dataReg  = static_cast<uint8_t>(kDeviceBase + 0x03);
    const uint8_t statReg  = static_cast<uint8_t>(kDeviceBase + 0x04);

    // ── Read block (45 bytes) ──────────────────────────────────────────
    // Streams 512 bytes, then tests $C0n4 bit 0 (I/O error) so a failed /
    // out-of-range readBlock returns carry-set (ProDOS $27) rather than CLC
    // "success" over a 0xFF-filled buffer. Lengthening this block past the
    // original 34 bytes is why the dispatch `BEQ write` operand above is $39
    // (was $2E) — pinned by tests/smartport_write_dispatch_test.cpp.
    emit(rom_, pc, {
        0xA5, 0x46,            // LDA $46
        0x8D, blkLoReg, 0xC0,
        0xA5, 0x47,            // LDA $47
        0x8D, blkHiReg, 0xC0,
        0xA0, 0x00,            // LDY #$00
        0xAD, dataReg, 0xC0,   // LDA $C0n3
        0x91, 0x44,            // STA ($44),Y
        0xC8,                  // INY
        0xD0, 0xF8,            // BNE -8
        0xE6, 0x45,            // INC $45
        0xAD, dataReg, 0xC0,   // LDA $C0n3
        0x91, 0x44,            // STA ($44),Y
        0xC8,                  // INY
        0xD0, 0xF8,            // BNE -8
        0xC6, 0x45,            // DEC $45
        0xAD, statReg, 0xC0,   // LDA $C0n4   ; status
        0x29, 0x01,            // AND #$01    ; I/O error bit
        0xD0, 0x02,            // BNE rderr
        0x18,                  // CLC         ; success
        0x60,                  // RTS
        0xA9, 0x27,            // rderr: LDA #$27 (ProDOS I/O error)
        0x38,                  // SEC
        0x60                   // RTS
    });

    // ── Write block ────────────────────────────────────────────────────
    // WP pre-check up front ($2B); after streaming 512 bytes, tests $C0n4
    // bit 0 so an out-of-range / rejected writeBlock returns carry-set
    // ($27) instead of the old unconditional CLC "success".
    emit(rom_, pc, {
        0xAD, statReg, 0xC0,   // LDA $C0n4
        0x29, 0x40,            // AND #$40    ; WP bit
        0xF0, 0x04,            // BEQ +4 (not WP → proceed)
        0xA9, 0x2B,            // LDA #$2B    ; write-protected error
        0x38,                  // SEC
        0x60,                  // RTS
        0xA5, 0x46,            // LDA $46
        0x8D, blkLoReg, 0xC0,
        0xA5, 0x47,            // LDA $47
        0x8D, blkHiReg, 0xC0,
        0xA0, 0x00,            // LDY #$00
        0xB1, 0x44,            // LDA ($44),Y
        0x8D, dataReg, 0xC0,   // STA $C0n3
        0xC8,                  // INY
        0xD0, 0xF8,            // BNE -8
        0xE6, 0x45,            // INC $45
        0xB1, 0x44,            // LDA ($44),Y
        0x8D, dataReg, 0xC0,   // STA $C0n3
        0xC8,                  // INY
        0xD0, 0xF8,            // BNE -8
        0xC6, 0x45,            // DEC $45
        0xAD, statReg, 0xC0,   // LDA $C0n4   ; re-read status
        0x29, 0x01,            // AND #$01    ; I/O error bit
        0xD0, 0x04,            // BNE wrerr
        0xA9, 0x00,            // LDA #$00    ; success
        0x18,                  // CLC
        0x60,                  // RTS
        0xA9, 0x27,            // wrerr: LDA #$27 (ProDOS I/O error)
        0x38,                  // SEC
        0x60                   // RTS
    });

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
        // $Cn0A-$Cn12 = our driver + SmartPort entries (BIT $CFFF +
        // JMP $CE00 spill to $Cn12); an earlier `i <= 0x1F` clobbered
        // the real page's $10-$1F with synthetic NOP padding despite
        // the "kept real" doc above.
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
    // One EXECUTE per BEGIN. Reading reg 0xE has side effects, so a stray
    // read without a fresh BEGIN (probing scans, debugger views) used to
    // replay the PREVIOUS command in full — re-arming the push machinery
    // and desyncing an in-flight legacy stream. Disarmed: just report the
    // last error code, leave all engine state (incl. a half-pulled
    // result) untouched.
    if (!spArmed_) return spError_;
    spArmed_ = false;

    spResult_.clear();
    spResultPos_ = 0;
    spPushPages_ = 0;
    auto fail = [&](uint8_t e) { spError_ = e; return e; };
    auto ok   = [&]()          { spError_ = 0;  return uint8_t{0}; };

    if (spCollectN_ < 3) return fail(0x01);            // bad command
    const uint8_t cmd    = spCollect_[0];
    const uint8_t pcount = spCollect_[1];
    const uint8_t unitNo = spCollect_[2];

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
                noteAccess();
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
    info.writeProtected    = u->isWriteProtected();
    info.writeBackEnabled  = u->isWriteBackEnabled();
    info.supportsWriteBack = true;
    info.supportsTypeSelect = true;
    return info;
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

void SmartPortCard::ejectBay(int bay)
{
    if (SmartPortUnit* u = unit(static_cast<size_t>(bay))) u->eject();
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
