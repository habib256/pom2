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

#include "ProDOSHardDiskCard.h"
#include "Logger.h"
#include "SlotRomAsm.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// ── 256-byte slot ROM layout (offsets from $Cn00) ─────────────────────────
// The page is assembled (SlotRomAsm.h): regions declare where they start and
// end, and every address in the code is a label. Before that, the write
// routine here was ONE byte from the SmartPort failure — it ended at $CnBF
// with STATUS starting at $CnC0.
constexpr uint8_t  kBootOff    = 0x20;
/// Shared error tail, in the free bytes between the boot routine (which ends
/// at $Cn44) and the ProDOS driver. Both transfer routines BRANCH here rather
/// than carrying their own `LDA #err / SEC / RTS`, which is what buys the room
/// for WRITE to pre-flight the bay at all — see the layout note in buildRom().
/// It grew from 11 to 19 bytes when it took on the second question ("is the
/// block ON the medium?"), and everything below it moved down.
constexpr uint8_t  kErrNoDev   = 0x45;   // → A = $27 / $28 / $2B, SEC, RTS
constexpr uint8_t  kDriverOff  = 0x58;
constexpr uint8_t  kReadOff    = 0x6E;
constexpr uint8_t  kWriteOff   = 0x9C;
constexpr uint8_t  kStatusOff  = 0xD0;
constexpr uint8_t  kHaltOff    = 0xE8;   // boot-failure halt loop
/// The ProDOS entry ($CnFF points here, not at the dispatch): latch ProDOS's
/// unit byte into $C0n6 so the card knows which drive, then JMP to the
/// dispatch. It lives in the free bytes above the halt loop because the
/// dispatch region is exactly full (22 of 22).
constexpr uint8_t  kEntryOff   = 0xEB;

// Block-level I/O trace, gated by POM2_TRACE_HDV=1 (mirrors the env-var
// diagnostics in DiskIICard.cpp / Memory.cpp). One line per 512-byte block
// transfer — enough to see the read/write sequence around a game crash
// without drowning in 512 lines per block.
bool hdvTraceOn()
{
    // POM2_TRACE_HANG implies HDV tracing too, so a single env var captures
    // both the frozen-loop dump and the block-read sequence that led to it.
    static const bool on = std::getenv("POM2_TRACE_HDV")  != nullptr ||
                           std::getenv("POM2_TRACE_HANG") != nullptr;
    return on;
}

} // namespace

ProDOSHardDiskCard::ProDOSHardDiskCard(int slotNum)
    : slot(slotNum)
{
    buildRom();
}

void ProDOSHardDiskCard::mediumChanged(int drive)
{
    // The firmware-visible cursor ($C0n0/$C0n1 block select + the byte
    // offset inside it) belongs to the selected drive. adoptDrive is what
    // pom2::mountBlockCard calls — the path a GUI mount actually takes — and
    // it used to forward to the backing alone, leaving the OUTGOING image's
    // cursor pointed into the incoming one.
    if (drive != drive_) return;
    selectedBlock = 0;
    streamOffset  = 0;
}

bool ProDOSHardDiskCard::loadDrive(int drive, const std::string& path)
{
    if (!validDrive(drive)) { lastError_ = "no such drive"; return false; }
    const bool ok = backings_[drive].loadImage(path);
    lastError_ = ok ? std::string{} : backings_[drive].lastError();
    mediumChanged(drive);
    return ok;
}

bool ProDOSHardDiskCard::adoptDrive(int drive,
                                    pom2::Block512Backing::PreparedImage&& p)
{
    if (!validDrive(drive)) { lastError_ = "no such drive"; return false; }
    const bool ok = backings_[drive].adoptImage(std::move(p));
    lastError_ = ok ? std::string{} : backings_[drive].lastError();
    mediumChanged(drive);
    return ok;
}

bool ProDOSHardDiskCard::loadImageFromBytes(std::vector<uint8_t> bytes,
                                            const std::string& label,
                                            const std::string& hostFolder)
{
    // Drive 1 only: a synthesised host-folder volume is the boot volume.
    const bool ok = backings_[0].loadFromBytes(std::move(bytes), label, hostFolder);
    lastError_ = ok ? std::string{} : backings_[0].lastError();
    mediumChanged(0);
    return ok;
}

bool ProDOSHardDiskCard::ejectDrive(int drive)
{
    if (!validDrive(drive)) return false;
    pom2::Block512Backing& b = backings_[drive];
    // Save-on-eject policy lives here (the card owns the user-facing eject):
    // flush dirty blocks first when write-back is on and the medium allows it,
    // then drop the image. b.saveDirty() is itself a guarded no-op.
    // `isMediumLocked()`, not `isWriteProtected()`: a notch flipped on the
    // mounted image must not make the eject drop blocks the guest already
    // wrote (Block512Backing::isMediumLocked).
    if (b.isLoaded() && b.hasUnsavedChanges() &&
        b.isWriteBackEnabled() && !b.isMediumLocked()) {
        if (!b.saveDirty()) {
            lastError_ = b.lastError();
            pom2::log().warn("HDV", "Save-on-eject failed on drive " +
                             std::to_string(drive + 1) + ": " + lastError_);
            return false;
        }
    }
    b.eject();
    lastError_.clear();
    mediumChanged(drive);
    return true;
}

bool ProDOSHardDiskCard::detachDrive(int drive,
                                     pom2::Block512Backing::PendingWriteBack& out)
{
    if (!validDrive(drive)) return false;
    pom2::Block512Backing& b = backings_[drive];
    if (!(b.isLoaded() && b.hasUnsavedChanges() &&
          b.isWriteBackEnabled() && !b.isMediumLocked()))
        return true;                     // nothing to write: out stays invalid
    out = b.takeWriteBack();
    return true;
}

bool ProDOSHardDiskCard::saveDirty()
{
    bool ok = true;
    lastError_.clear();
    for (int d = 0; d < kDrives; ++d) {
        if (backings_[d].saveDirty()) continue;
        ok = false;
        if (!lastError_.empty()) lastError_ += "; ";
        lastError_ += "drive " + std::to_string(d + 1) + ": " +
                      backings_[d].lastError();
    }
    return ok;
}

// ── Bays ─────────────────────────────────────────────────────────────────

pom2::MediaBayInfo ProDOSHardDiskCard::bayInfo(int bay) const
{
    pom2::MediaBayInfo info;
    if (!validDrive(bay)) return info;
    const pom2::Block512Backing& b = backings_[bay];
    info.loaded            = b.isLoaded();
    info.busy              = b.isBusy();
    info.path              = b.path();
    info.lastError         = b.lastError();
    info.blockCount        = static_cast<uint32_t>(b.blockCount());
    info.writeProtected    = b.isWriteProtected();
    info.writeBackEnabled  = b.isWriteBackEnabled();
    info.hasUnsavedChanges = b.hasUnsavedChanges();
    info.supportsWriteBack = b.canWriteBack();
    info.persistenceState  = b.persistenceState();
    info.persistenceError  = b.persistenceError();
    return info;
}

bool ProDOSHardDiskCard::mountBay(int bay, const std::string& path,
                                  std::string& errOut)
{
    if (!validDrive(bay)) { errOut = "invalid bay"; return false; }
    if (!loadDrive(bay, path)) { errOut = lastError_; return false; }
    errOut.clear();
    return true;
}

bool ProDOSHardDiskCard::adoptBay(int bay,
                                  pom2::Block512Backing::PreparedImage&& prepared,
                                  std::string& errOut)
{
    if (!validDrive(bay)) { errOut = "invalid bay"; return false; }
    if (!adoptDrive(bay, std::move(prepared))) {
        // Every bay here has block backing: a false return is a real
        // failure, and must never read as "fall back and retry".
        errOut = lastError_.empty() ? std::string("image could not be adopted")
                                    : lastError_;
        return false;
    }
    errOut.clear();
    return true;
}

bool ProDOSHardDiskCard::flushBay(int bay, std::string& errOut)
{
    errOut.clear();
    if (!validDrive(bay)) { errOut = "no such bay"; return false; }
    pom2::Block512Backing& b = backings_[bay];
    if (!b.isLoaded() || !b.hasUnsavedChanges()) return true;
    if (b.saveDirty()) return true;
    errOut = b.lastError().empty() ? std::string("write-back failed") : b.lastError();
    return false;
}

bool ProDOSHardDiskCard::prepareEjectBay(
    int bay, pom2::Block512Backing::PendingWriteBack& out, std::string& errOut)
{
    errOut.clear();
    if (!validDrive(bay)) return false;  // empty errOut → caller falls back
    return detachDrive(bay, out);
}

void ProDOSHardDiskCard::onReset()
{
    selectedBlock = 0;
    streamOffset = 0;
    drive_ = 0;
}

uint8_t ProDOSHardDiskCard::slotRomRead(uint8_t low8)
{
    return rom[low8];
}

void ProDOSHardDiskCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    // Firmware protocol:
    //   $C0D0 write = block low byte
    //   $C0D1 write = block high byte
    //   $C0D2 read  = next byte from selected 512-byte block
    //   $C0D2 write = next byte INTO selected block (write-back enabled)
    //   $C0D3 read  = bit-7 = imageLoaded, bit-6 = isWriteProtected
    //   $C0D6 write = drive select, bit 7 (ProDOS's unit byte, verbatim)
    if (low4 == 0x6) {
        drive_ = (v & 0x80) ? 1 : 0;
        if (hdvTraceOn())
            std::fprintf(stderr, "[HDV] DRIVE %d\n", drive_ + 1);
    } else if (low4 == 0x0) {
        selectedBlock = static_cast<uint16_t>((selectedBlock & 0xFF00u) | v);
        streamOffset = 0;
        if (hdvTraceOn())
            std::fprintf(stderr, "[HDV] SETLO blk=%u\n",
                         static_cast<unsigned>(selectedBlock));
    } else if (low4 == 0x1) {
        selectedBlock = static_cast<uint16_t>((selectedBlock & 0x00FFu) |
                                              (static_cast<uint16_t>(v) << 8));
        streamOffset = 0;
        if (hdvTraceOn())
            std::fprintf(stderr, "[HDV] SETHI blk=%u\n",
                         static_cast<unsigned>(selectedBlock));
    } else if (low4 == 0x2) {
        writeDataByte(v);
    }
}

uint8_t ProDOSHardDiskCard::deviceSelectRead(uint8_t low4)
{
    if (low4 == 0x2) return readDataByte();
    if (low4 == 0x6) return static_cast<uint8_t>(drive_ << 7);
    if (low4 == 0x3) {
        // Status byte. Preserves the original encoding for backward compat:
        //   bit-7 = 0 when image loaded, 1 when missing (legacy).
        //   bit-6 = 1 when write-protected (used by the write driver in the
        //           ROM to gate ProDOS WRITE_BLOCK and return $2B without
        //           touching the in-memory image).
        //   bit-5 = 1 when the SELECTED block is past the end of the medium.
        //
        // Bit 5 is what makes the driver able to fail an out-of-range
        // transfer at all (bug hunt 4 #5). Before it, a READ past the end
        // streamed 512 × $FF and a WRITE dropped its bytes, and BOTH returned
        // CLC "success" — so a truncated .hdv (or a 2MG whose header claims
        // more blocks than the file carries) looked to ProDOS like a healthy
        // volume full of $FF. The ATA/CFFA path has always refused
        // (AtaBlockDevice::startCommand); this is the synthetic card catching
        // up. Only meaningful with media present — the ROM's error tail reads
        // "bit 5 clear" as "the bay is empty", so an empty bay must not set it.
        if (!cur().isLoaded()) return 0x80;
        uint8_t s = 0x00;
        if (cur().isWriteProtected()) s |= 0x40;
        if (static_cast<size_t>(selectedBlock) >= cur().blockCount())
            s |= 0x20;
        return s;
    }
    if (low4 == 0x4 || low4 == 0x5) {
        // STATUS block count, low ($C0n4) / high ($C0n5). The ProDOS
        // STATUS driver call (cmd $00) must return the device's total
        // block count in X (low) / Y (high); the ROM STATUS routine
        // reads it from this register pair — same scheme (and same
        // BITSY-crash lesson) as SmartPortCard::blockCountByte. Counts
        // above $FFFF clamp (an exactly-65536-block 32 MiB image must
        // report $FFFF, not truncate to 0 = "empty volume").
        size_t blocks = cur().isLoaded() ? cur().blockCount() : 0u;
        if (blocks > 0xFFFFu) blocks = 0xFFFFu;
        return static_cast<uint8_t>(
            (blocks >> (low4 == 0x5 ? 8 : 0)) & 0xFF);
    }
    return 0xFF;
}

uint8_t ProDOSHardDiskCard::readDataByte()
{
    if (!cur().isLoaded()) return 0xFF;

    if (hdvTraceOn() && streamOffset == 0) {
        const bool inRange = (selectedBlock + 1u) <= cur().blockCount();
        std::fprintf(stderr, "[HDV] READ  blk=%u%s\n",
                     static_cast<unsigned>(selectedBlock),
                     inRange ? "" : " (OUT-OF-RANGE -> $FF)");
    }

    const size_t absolute =
        static_cast<size_t>(selectedBlock) * kBlockBytes + streamOffset;
    const uint8_t out = cur().readByte(absolute);
    streamOffset = (streamOffset + 1) % kBlockBytes;
    return out;
}

void ProDOSHardDiskCard::writeDataByte(uint8_t v)
{
    // Writes always land in the in-memory image so the running session sees a
    // fully writable volume (a real hard disk is read/write to ProDOS). Only
    // the real medium WP flag (2MG header) blocks the write. Persisting those
    // RAM changes to the host .hdv/.2mg file is a SEPARATE opt-in handled by
    // writeBackEnabled in saveDirty()/ejectImage().
    if (!cur().isLoaded() || cur().isWriteProtected()) return;

    if (hdvTraceOn() && streamOffset == 0) {
        const bool inRange = (selectedBlock + 1u) <= cur().blockCount();
        std::fprintf(stderr, "[HDV] WRITE blk=%u wb=%d%s\n",
                     static_cast<unsigned>(selectedBlock),
                     cur().isWriteBackEnabled() ? 1 : 0,
                     inRange ? "" : " (OUT-OF-RANGE -> dropped)");
    }

    const size_t absolute =
        static_cast<size_t>(selectedBlock) * kBlockBytes + streamOffset;
    cur().writeByte(absolute, v);
    streamOffset = (streamOffset + 1) % kBlockBytes;
}

void ProDOSHardDiskCard::buildRom()
{
    rom.fill(0xEA); // NOP padding

    const uint16_t kDeviceBase = static_cast<uint16_t>(0xC080 + slot * 16);
    const uint8_t  kUnitNumber = static_cast<uint8_t>(slot << 4);
    const uint8_t  dataReg = static_cast<uint8_t>(kDeviceBase + 0x02);
    const uint8_t  statReg = static_cast<uint8_t>(kDeviceBase + 0x03);

    // ── Layout ──────────────────────────────────────────────────────────
    //   $Cn00..$Cn07  ProDOS signature + the PR#n entry
    //   $Cn20..$Cn44  boot
    //   $Cn45..$Cn57  shared error tail  ($27 / $28 / $2B)
    //   $Cn58..$Cn6D  ProDOS driver dispatch
    //   $Cn6E..$Cn96  read block
    //   $Cn9C..$CnCB  write block
    //   $CnD0..$CnE2  STATUS
    //   $CnE8..$CnEA  boot-failure halt
    //   $CnEB..$CnF2  ProDOS entry: latch the drive, JMP dispatch
    //   $CnFE..$CnFF  capability + driver-entry bytes
    //
    // Every address in the page below is a LABEL. The dispatch's three
    // displacements used to be hand-computed literals, and the middle one
    // carried a comment recording that somebody had already re-counted it
    // once ("read grew 9 B"); $CnFF was the driver's offset typed a second
    // time. Both are now derived, so moving a routine cannot leave anything
    // pointing at where it used to be.
    pom2::SlotRomAsm a(rom, slot, "ProDOSHardDiskCard");

    // Entry used by PR#n / direct boot: load block 0 at $0800, then jump to
    // the universal ProDOS boot loader's real entry at $0801 with X=unit.
    // The signature bytes ProDOS scans for are $Cn01/$Cn03/$Cn05/$Cn07, and
    // $Cn01 doubles as the JMP's low byte — which is exactly why boot lives
    // at $Cn20 and not somewhere more convenient.
    a.region("entry", 0x00, 0x08)
     .jmp("boot")                 // $Cn01 = $20 falls out of the JMP operand
     .poke(0x03, 0x00)            // ProDOS signature byte
     .poke(0x05, 0x03)            // ProDOS signature byte
     .poke(0x07, 0x01);           // non-zero: plain block device, not SmartPort

    a.region("boot", kBootOff, kErrNoDev)
     .emit({ 0xA9, 0x01,       // LDA #$01        ; read command
             0x85, 0x42,       // STA $42
             0xA9, kUnitNumber,
             0x85, 0x43,       // STA $43         ; slot N, drive 1
             0xA9, 0x00,
             0x85, 0x44,       // STA $44         ; buffer low = $00
             0xA9, 0x08,
             0x85, 0x45,       // STA $45         ; buffer high = $08
             0xA9, 0x00,
             0x85, 0x46,       // STA $46         ; block low = 0
             0x85, 0x47 })     // STA $47         ; block high = 0
     .jsr("entry2")            // through the latch: drive 1, whatever was last
     .branch(0xB0, "bootErr")  // BCS bootErr
     .emit({ 0xA2, kUnitNumber,// LDX #unit
             0xA9, 0x00,       // LDA #$00
             0x4C, 0x01, 0x08 })                  // JMP $0801
     .label("bootErr").jmp("halt");

    // ── Shared error tail ($Cn45) ──────────────────────────────────────
    // Three codes, one exit. Both transfer routines branch here instead of
    // each carrying `LDA #err / SEC / RTS`, and that is not tidiness: it is
    // the bytes that let them ask anything about the bay at all. The routine
    // had ZERO slack before — the write routine ended at $CnBF with STATUS at
    // $CnC0 — so the first pre-flight had to be paid for by removing bytes.
    //
    // `errNoDev` is now a DISCRIMINATOR, not a constant: the caller has
    // already established that $C0n3 said "something is wrong" (bit 7 or
    // bit 5 set), and the second read separates the two. Bit 5 is only ever
    // set with media present, so "bit 5 clear" is unambiguously "empty bay".
    //   $27 = I/O error        — block past the end of the medium
    //   $28 = no device        — nothing in the bay
    //   $2B = write protected  — WRITE only, entered at `errWProt`
    //
    // Lives in the gap the boot routine leaves. Unlike SmartPortCard, whose
    // identical first attempt had to be undone, this ROM has no authentic
    // dump overlaid on it, so the gap really is free.
    a.region("errNoDev", kErrNoDev, kDriverOff)
     .emit({ 0xAD, statReg, 0xC0,   // LDA $C0n3
             0x29, 0x20 })          // AND #$20   ; block out of range?
     .branch(0xF0, "errNoMedia")    // BEQ → empty bay
     .emit({ 0xA9, 0x27 })          // LDA #$27  I/O error
     .branch(0xD0, "errExit")       // BNE errExit  (always: A != 0)
     .label("errNoMedia")
     .emit({ 0xA9, 0x28 })          // LDA #$28  no device connected
     .branch(0xD0, "errExit")       // BNE errExit  (always: A != 0)
     .label("errWProt")
     .emit({ 0xA9, 0x2B })          // LDA #$2B  write protected
     .label("errExit")
     .emit({ 0x38,                  // SEC
             0x60 });               // RTS

    // ── ProDOS driver dispatch ($Cn50) ─────────────────────────────────
    //   $00 status → JMP $CnC0 (returns the block count in X/Y)
    //   $01 read   → read block
    //   $02 write  → write block
    //   any other  → A=$01 (bad command), SEC, RTS
    //
    // Both transfer routines PRE-FLIGHT the bay before touching anything.
    // The block registers are latched FIRST, then one `LDA $C0n3 / AND #$A0`
    // asks both media questions at once — bit 7 "no media", bit 5 "that block
    // is not on this medium" — and the shared tail separates them. WRITE adds
    // a `BIT $C0n3 / BVS` for bit 6 "write protected" AFTER that, so media is
    // still asked about first: an empty bay is $28 "no device connected", not
    // $2B "write protected", which is what WRITE used to say because it
    // tested the WP bit and never tested media at all.
    a.region("driver", kDriverOff, kReadOff)
     .emit({ 0xA5, 0x42,       // LDA $42         ; command
             0xC9, 0x01 })     // CMP #$01
     .branch(0xF0, "read")
     .emit({ 0xC9, 0x02 })     // CMP #$02
     .branch(0xF0, "write")
     .emit({ 0xC9, 0x00 })     // CMP #$00
     .branch(0xF0, "dispStatus")
     .emit({ 0xA9, 0x01,       // LDA #$01    ; bad-command error
             0x38,             // SEC
             0x60 })           // RTS
     // The STATUS arm is a JMP rather than the routine itself because
     // STATUS lives above the transfer routines; keeping it 4 bytes (JMP +
     // pad) is what the BITSY crash in SmartPortCard::buildRom cost to
     // learn. The pad is no longer load-bearing — the branches above are
     // computed — but removing it would change the page for no reason.
     .label("dispStatus").jmp("status").emit({ 0xEA });

    a.region("read", kReadOff, kWriteOff)
     .emit({ 0xA5, 0x46,              // LDA $46     ; block low
             0x8D, static_cast<uint8_t>(kDeviceBase + 0x00), 0xC0,
             0xA5, 0x47,              // LDA $47     ; block high
             0x8D, static_cast<uint8_t>(kDeviceBase + 0x01), 0xC0,
             0xAD, statReg, 0xC0,     // LDA $C0n3   ; after the block latch,
             0x29, 0xA0 })            // AND #$A0    ; so bit 5 is about THIS
     .branch(0xD0, "errNoDev")        // BNE → $28 (no media) or $27 (range)
     .emit({ 0xA0, 0x00 })            // LDY #$00
     .label("readPage1")
     .emit({ 0xAD, dataReg, 0xC0,     // LDA $C0n2
             0x91, 0x44,              // STA ($44),Y
             0xC8 })                  // INY
     .branch(0xD0, "readPage1")
     .emit({ 0xE6, 0x45 })            // INC $45
     .label("readPage2")
     .emit({ 0xAD, dataReg, 0xC0,     // LDA $C0n2
             0x91, 0x44,              // STA ($44),Y
             0xC8 })                  // INY
     .branch(0xD0, "readPage2")
     .emit({ 0xC6, 0x45,              // DEC $45
             // ProDOS 8 Technical Reference § 6.3 (Calling a Device Driver):
             // "the driver returns with the carry flag clear and with the
             // accumulator containing zero". READ was the one arm of this
             // ROM that did not — it fell out of the loop with A = the LAST
             // BYTE OF THE BLOCK and only cleared the carry, so a caller
             // that reads the accumulator on a successful READ_BLOCK gets a
             // byte of user data where an error code belongs (the probe
             // that found it read $2B — "write protected" — off a perfectly
             // healthy block). WRITE and STATUS in this same page already
             // load #$00, as does SmartPortCard's read arm; the region has
             // five spare bytes.
             0xA9, 0x00,              // LDA #$00
             0x18,                    // CLC
             0x60 });                 // RTS

    a.region("write", kWriteOff, kStatusOff)
     .emit({ 0xA5, 0x46,              // LDA $46
             0x8D, static_cast<uint8_t>(kDeviceBase + 0x00), 0xC0,
             0xA5, 0x47,              // LDA $47
             0x8D, static_cast<uint8_t>(kDeviceBase + 0x01), 0xC0,
             0xAD, statReg, 0xC0,     // LDA $C0n3
             0x29, 0xA0 })            // AND #$A0    ; media + range, FIRST
     .branch(0xD0, "errNoDev")        // BNE → $28 / $27
     .emit({ 0x2C, statReg, 0xC0 })   // BIT $C0n3   ; V = write protected
     .branch(0x70, "errWProt")        // BVS → $2B
     .emit({ 0xA0, 0x00 })            // LDY #$00
     .label("writePage1")
     .emit({ 0xB1, 0x44,              // LDA ($44),Y
             0x8D, dataReg, 0xC0,     // STA $C0n2
             0xC8 })                  // INY
     .branch(0xD0, "writePage1")
     .emit({ 0xE6, 0x45 })            // INC $45
     .label("writePage2")
     .emit({ 0xB1, 0x44,              // LDA ($44),Y
             0x8D, dataReg, 0xC0,     // STA $C0n2
             0xC8 })                  // INY
     .branch(0xD0, "writePage2")
     .emit({ 0xC6, 0x45,              // DEC $45
             0xA9, 0x00,              // LDA #$00
             0x18,                    // CLC
             0x60 });                 // RTS

    // ── STATUS ($CnD0) ─────────────────────────────────────────────────
    // ProDOS STATUS (cmd $00) must return total blocks in X (low) / Y (high)
    // so a volume scanner (BITSY, ProDOS ONLINE) can size the device. The
    // count comes from $C0n4/$C0n5 (deviceSelectRead 0x4/0x5, clamped to
    // $FFFF).
    //
    // It pre-flights the bay like the transfer routines do (bug hunt 4 #14).
    // An empty bay used to answer CLC with X=Y=0 — "there IS a device here
    // and it has zero blocks" — which is a different claim from "no device",
    // and the one a ProDOS scanner records as a live unit. $28 is the answer
    // READ and WRITE already gave. Only bit 7 is tested here: bit 5 is about
    // whatever block was last selected and has nothing to do with STATUS, so
    // `BIT` (N = bit 7, V = bit 6) is exactly the right instruction.
    a.region("status", kStatusOff, kHaltOff)
     .emit({ 0x2C, statReg, 0xC0 })   // BIT $C0n3   ; N = no media
     .branch(0x10, "statusOk")        // BPL statusOk
     .emit({ 0xA9, 0x28,              // LDA #$28  no device connected
             0x38,                    // SEC
             0x60 })                  // RTS
     .label("statusOk")
     .emit({ 0xAE, static_cast<uint8_t>(kDeviceBase + 0x04), 0xC0, // LDX $C0n4
             0xAC, static_cast<uint8_t>(kDeviceBase + 0x05), 0xC0, // LDY $C0n5
             0xA9, 0x00,        // LDA #$00
             0x18,              // CLC
             0x60 });           // RTS

    // Boot failure: a plain infinite loop. Monitor routines are deliberately
    // avoided — a failed HD boot must be safe even if the main ROM is not
    // fully initialised yet.
    a.region("halt", kHaltOff, kHaltOff + 3).jmp("halt");

    // ── ProDOS entry ($CnEB) ───────────────────────────────────────────
    // ProDOS passes the unit in $43: bits 6-4 the slot, bit 7 the DRIVE. The
    // card has two (2026-09-11), so the drive bit has to reach it before
    // anything else does — and STATUS, READ and WRITE all start by asking
    // $C0n3 about the bay, which is now "the bay of the drive latched here".
    // Written verbatim: the card looks at bit 7 only.
    a.region("entry2", kEntryOff, kEntryOff + 8)
     .emit({ 0xA5, 0x43,                                   // LDA $43
             0x8D, static_cast<uint8_t>(kDeviceBase + 0x06), 0xC0 }) // STA $C0n6
     .jmp("driver");

    // $CnFE = the ProDOS device-characteristics byte (ProDOS 8 Technical
    // Reference / TN.PDOS.021): bit 0 = status, bit 1 = read, bit 2 = WRITE,
    // bit 3 = format, bits 4-6 = extra units, bit 7 = removable. It said $03
    // — "status + read", i.e. a READ-ONLY device — while the ROM has carried
    // a working WRITE_BLOCK all along and ProDOS's own SAVE goes through it.
    // $07 adds the write bit. Same fix, same reason, as SmartPortCard's
    // $13 → $17 (SmartPortCard.cpp).
    //
    // Bit 4 since 2026-09-11: TWO volumes, drive 1 and drive 2 — $17, the
    // byte SmartPortCard already carries. ProDOS installs S<n>,D2 from it, so
    // drive 2 is in DEVLST from boot (empty until mounted, like a Disk II's
    // second drive), and a disk mounted there later needs no reboot.
    a.region("tail", 0xFE, pom2::kSlotRomBytes)
     .emit({ 0x17 })      // status + read + write; bits 5-4 = 1 → two units
     .byteOf("entry2");   // ProDOS driver entry offset: the drive latch

    romLayoutError_ = !a.finish();
}

// ── Snapshot / rewind ─────────────────────────────────────────────────────

namespace {
constexpr uint8_t kHdvSnapMagicV1[4] = { 'H', 'D', 'V', '1' };
constexpr uint8_t kHdvSnapMagic[4]   = { 'H', 'D', 'V', '2' };   // + drive
}

void ProDOSHardDiskCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), kHdvSnapMagic, kHdvSnapMagic + 4);
    out.push_back(static_cast<uint8_t>(selectedBlock));
    out.push_back(static_cast<uint8_t>(selectedBlock >> 8));
    // streamOffset ∈ [0, 512]; two bytes are plenty.
    out.push_back(static_cast<uint8_t>(streamOffset));
    out.push_back(static_cast<uint8_t>(streamOffset >> 8));
    // v2: the latched drive. A rewind into a drive-2 transfer that came back
    // on drive 1 would stream the rest of the block out of the other volume.
    out.push_back(static_cast<uint8_t>(drive_));
}

void ProDOSHardDiskCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (data == nullptr || len < 8) return;
    const bool v2 = std::memcmp(data, kHdvSnapMagic, 4) == 0;
    if (!v2 && std::memcmp(data, kHdvSnapMagicV1, 4) != 0)
        return;   // foreign blob — a different card sat here
    if (v2 && len < 9) return;
    selectedBlock = static_cast<uint16_t>(data[4] | (data[5] << 8));
    streamOffset  = static_cast<size_t>(data[6] | (data[7] << 8));
    // An HDV1 blob predates drive 2: everything it recorded was drive 1.
    drive_ = (v2 && data[8] != 0) ? 1 : 0;
    // >=: a restored 512 would address the FIRST byte of the next block
    // before the modulo wrap, handing the guest one wrong byte (and
    // corrupting one byte of the wrong block on the write path).
    if (streamOffset >= 512) streamOffset = 0;   // untrusted input
}
