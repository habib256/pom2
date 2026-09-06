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

// FujiNetCard implementation — synthetic slot ROM, the $C0n2 trap, and the
// marshalling between emulated RAM/CPU state and SP-over-SLIP requests.
// See FujiNetCard.h for the architecture and the source citations.

#include "FujiNetCard.h"

#include <cstdlib>

#include "Logger.h"
#include "M6502.h"
#include "Memory.h"
#include "SlotRomAsm.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace pom2 {

namespace {

// ── ROM layout (offsets from $Cn00) ──────────────────────────────────────
// The signature bytes are real instructions so that a JMP $Cn00 — which is
// how both the autostart scan and PR#n enter a card — executes them
// harmlessly and falls straight into the boot code.
constexpr uint8_t kBootOff   = 0x08;
constexpr uint8_t kBootErr   = 0x30;
constexpr uint8_t kErrExit   = 0x42;
constexpr uint8_t kDriverOff = 0x60;
constexpr uint8_t kErrText   = 0xF0;

// Monitor entry points the boot fallback uses.
constexpr uint16_t kMonSloop  = 0xFABA;   // continue the autostart slot scan
constexpr uint16_t kMonSetKbd = 0xFE89;
constexpr uint16_t kMonSetScr = 0xFE93;
constexpr uint16_t kMonSetTxt = 0xFB39;
constexpr uint16_t kMonHome   = 0xFC58;
constexpr uint16_t kMonCout   = 0xFDED;
constexpr uint16_t kBasic     = 0xE000;
constexpr uint16_t kMslot     = 0x07F8;

/// ProDOS zero-page call block.
constexpr uint16_t kZpCommand = 0x42;
constexpr uint16_t kZpUnit    = 0x43;
constexpr uint16_t kZpBufLo   = 0x44;
constexpr uint16_t kZpBlkLo   = 0x46;

constexpr std::size_t kBlockBytes = 512;

} // namespace

FujiNetCard::FujiNetCard(int slot,
                         std::unique_ptr<FujiNetLink> link,
                         FujiNetTransport& transport,
                         std::unique_ptr<FujiNetNetwork> network)
    // Init order follows DECLARATION order (net_ is declared with the
    // built-in-N: members, well above slot_), not argument order.
    : net_(std::move(network)),
      slot_(slot),
      link_(std::move(link)),
      transport_(&transport)
{
    buildRom();
}

FujiNetCard::~FujiNetCard()
{
    // Order matters: stop the link first so the helper's peer goes away
    // cleanly, then terminate the helper. The other way round leaves the
    // link's worker chasing a socket whose far end just died.
    transport_->stop();

    // stopDetached(), not stop(): this destructor is called by
    // `SlotBus::plug()`, which POM2 runs with the emulator's state mutex held
    // on every slot rebuild and every profile switch. `stop()` polls for its
    // whole 2 s grace — on purpose, so a FujiNet flushing an SD-card image
    // gets to finish — and under that lock those two seconds are the CPU
    // worker blocked on its next chunk and the UI thread blocked trying to
    // paint: the freeze CLAUDE.md forbids, cancel button included. The
    // detached form sends the same SIGTERM from here, right now, and only
    // hands the WAITING to a guarded thread; the helper still gets its grace,
    // still gets the SIGKILL sweep, and is still reaped.
    //
    // The one case that shape does NOT cover on its own is the LAST teardown
    // of all: at quit the detached thread dies with the process, so a helper
    // that traps SIGTERM would outlive POM2 holding the loopback port. That
    // is why main() calls `ChildProcess::drainDetached()` on the way out —
    // it wakes every thread parked here and waits for the SIGKILL sweep.
    helper_.stopDetached();
}

bool FujiNetCard::startHelper(const std::string& exePath, std::string& errOut)
{
    std::string exe = exePath;
    if (exe.empty()) exe = ChildProcess::findOnPath("fujinet");
    if (exe.empty()) {
        errOut = "no FujiNet program found — set its path, or install one on "
                 "PATH as 'fujinet'";
        return false;
    }
    // No arguments: the firmware takes its Bus-over-IP target from its own
    // fnconfig.ini, whose Apple default is already 127.0.0.1:1985.
    return helper_.start(exe, {}, std::string{}, errOut);
}

// ─────────────────────────────────────────────────────────────────────────
// Slot ROM
// ─────────────────────────────────────────────────────────────────────────

void FujiNetCard::buildRom()
{
    rom_.fill(0xEA);                                    // NOP padding

    const uint8_t romHi   = static_cast<uint8_t>(0xC0 + slot_);
    const uint8_t unitDrv1 = static_cast<uint8_t>(slot_ << 4);
    // $C0(8+slot)2 — the device-select address the driver stores the magic to.
    const uint8_t trapLo  = static_cast<uint8_t>(0x80 + slot_ * 16 + 2);

    // ── 256-byte layout ──────────────────────────────────────────────────
    //   $Cn00-$Cn07  ProDOS/SmartPort signature bytes
    //   $Cn08-$Cn2F  boot
    //   $Cn30-$Cn41  boot failure — rejoin the autostart slot scan
    //   $Cn42-$Cn5F  PR#n with nothing to boot: print "FN ERROR"
    //   $Cn60-$CnEF  the driver (ProDOS entry, SmartPort entry at +3)
    //   $CnF0-$CnF7  "FN ERROR", stored reversed
    //   $CnFC-$CnFF  ProDOS identification tail
    pom2::SlotRomAsm a(rom_, slot_, "FujiNetCard");

    // ── Signature ($Cn00-$Cn07) ──────────────────────────────────────────
    // Real instructions, so a JMP $Cn00 — which is how both the autostart
    // scan and PR#n enter a card — executes them harmlessly and falls
    // straight into the boot code. CPX #$20 / LDX #$00 / CPX #$03 / CPX #$00
    // puts the ProDOS block-device signature $20/$00/$03 at $Cn01/$Cn03/$Cn05
    // — the one POM2's own bootFromSlot validates — and $Cn07 = $00 marks the
    // SmartPort class. X ends up 0.
    a.region("signature", 0x00, kBootOff)
     .emit({ 0xE0, 0x20,          // CPX #$20   → $Cn01 = $20
             0xA2, 0x00,          // LDX #$00   → $Cn03 = $00
             0xE0, 0x03,          // CPX #$03   → $Cn05 = $03
             0xE0, 0x00 });       // CPX #$00   → $Cn07 = $00 (SmartPort)

    // ── Boot ($Cn08) ─────────────────────────────────────────────────────
    // Read block 0 of unit 1 to $0800 through our own ProDOS entry, sanity
    // check it the way a ProDOS boot block is supposed to look, then run it.
    a.region("boot", kBootOff, kBootErr)
     .emit({ 0xA2, 0x00,                       // LDX #$00
             0x86, 0x46,                       // STX $46      block lo
             0x86, 0x47,                       // STX $47      block hi
             0x86, 0x44,                       // STX $44      buffer lo
             0xE8,                             // INX
             0x86, 0x42,                       // STX $42      command = 1
             0xA2, 0x08,                       // LDX #$08
             0x86, 0x45,                       // STX $45      buffer hi = $08
             0xA2, unitDrv1,                   // LDX #slot*16 unit, drive 1
             0x86, 0x43 })                     // STX $43
     .jsr("driver")
     .branch(0xB0, "bootErr")                  // BCS bootErr
     .emit({ 0xAE, 0x00, 0x08,                 // LDX $0800    boot block count
             0xCA })                           // DEX
     .branch(0xD0, "bootErr")                  // BNE bootErr  must be 1
     .emit({ 0xAE, 0x01, 0x08 })               // LDX $0801    first opcode
     .branch(0xF0, "bootErr")                  // BEQ bootErr  must not be BRK
     .emit({ 0xA2, unitDrv1,                   // LDX #slot*16 boot blocks want it
             0x4C, 0x01, 0x08 });              // JMP $0801

    // ── Boot failure ($Cn30) ─────────────────────────────────────────────
    // No FujiNet, or no bootable volume on unit 1. If we got here from the
    // autostart SLOT SCAN, continue it so the Disk II in slot 6 still boots —
    // without this, plugging a FujiNet card into slot 7 would break booting
    // whenever the FujiNet is not running. The scan is recognised the way the
    // Monitor leaves it: $00/$01 hold the $Cn00 it jumped to, and MSLOT
    // ($07F8) holds $Cn.
    a.region("bootErr", kBootErr, kErrExit)
     .emit({ 0xA6, 0x00 })                     // LDX $00
     .branch(0xD0, "errExit")                  // BNE errExit  low byte must be 0
     .emit({ 0xA6, 0x01,                       // LDX $01
             0xEC, static_cast<uint8_t>(kMslot & 0xFF),
                   static_cast<uint8_t>(kMslot >> 8) })    // CPX $07F8 (MSLOT)
     .branch(0xD0, "errExit")
     .emit({ 0xE0, romHi })                    // CPX #$Cn     and is it us?
     .branch(0xD0, "errExit")
     .emit({ 0x4C, static_cast<uint8_t>(kMonSloop & 0xFF),
                   static_cast<uint8_t>(kMonSloop >> 8) });  // JMP $FABA

    // ── Not a scan: somebody ran PR#n with nothing to boot ($Cn42) ───────
    a.region("errExit", kErrExit, kDriverOff)
     .emit({ 0x20, static_cast<uint8_t>(kMonSetScr & 0xFF), static_cast<uint8_t>(kMonSetScr >> 8),
             0x20, static_cast<uint8_t>(kMonSetKbd & 0xFF), static_cast<uint8_t>(kMonSetKbd >> 8),
             0x20, static_cast<uint8_t>(kMonSetTxt & 0xFF), static_cast<uint8_t>(kMonSetTxt >> 8),
             0x20, static_cast<uint8_t>(kMonHome   & 0xFF), static_cast<uint8_t>(kMonHome   >> 8),
             0xA2, 0x07 })                     // LDX #$07
     .label("errLoop")
     .emit({ 0xBD }).byteOf("errText").emit({ romHi })  // LDA errText,X
     .emit({ 0x20, static_cast<uint8_t>(kMonCout & 0xFF),
                   static_cast<uint8_t>(kMonCout >> 8),
             0xCA })                           // DEX
     .branch(0x10, "errLoop")                  // BPL errLoop
     .emit({ 0x4C, static_cast<uint8_t>(kBasic & 0xFF),
                   static_cast<uint8_t>(kBasic >> 8) });

    // ── The driver ($Cn60) ───────────────────────────────────────────────
    // THE WHOLE POINT OF THE CARD. Two entry points three bytes apart, as the
    // Apple convention requires (ProDOS entry at $CnFF's offset, SmartPort
    // entry at that + 3), both of which just tell the host to take over.
    //
    // `CMP #$01` after the trap turns the status byte the host left in A into
    // the carry flag ProDOS and SmartPort both expect: carry clear iff A == 0.
    // It also overwrites N/Z, which is why `finish()` setting them is a
    // courtesy for callers that enter mid-routine rather than a contract.
    a.region("driver", kDriverOff, kErrText)
     .emit({ 0x38 })                           // SEC              ProDOS entry
     .branch(0xB0, "drvProDos")                // BCS drvProDos
     .label("drvSmartPort")                    // ( = ProDOS entry + 3 )
     .emit({ 0xA9, kMagicSmartPort })          // LDA #$65
     .branch(0xD0, "drvStore")                 // BNE drvStore
     .label("drvProDos")
     .emit({ 0xA9, kMagicProDOS })             // LDA #$66
     .label("drvStore")
     .emit({ 0x8D, trapLo, 0xC0,               // STA $C0n2        ← the trap
             0xC9, 0x01,                       // CMP #$01   A != 0 → carry
             0x60 });                          // RTS

    // ── "FN ERROR", stored reversed because the printer counts X down ────
    a.region("errText", kErrText, kErrText + 8);
    const char text[8] = { 'R', 'O', 'R', 'R', 'E', ' ', 'N', 'F' };
    for (int i = 0; i < 8; ++i)
        a.poke(static_cast<unsigned>(kErrText + i),
               static_cast<uint8_t>(text[i] | 0x80));

    // ── ProDOS identification tail ───────────────────────────────────────
    // $CnFC/$CnFD = total blocks. ZERO ON PURPOSE: it makes ProDOS issue a
    // STATUS call to learn the size, which is the only correct answer for a
    // device whose media the user can change from the FujiNet's own web UI
    // while the machine is running.
    //
    // $CnFE capability byte (ProDOS 8 TN #21): removable, interruptible,
    // read + write + status. Same value the FujiNet AppleWin fork's ROM
    // publishes, so a guest that special-cases FujiNet sees what it expects.
    a.region("tail", 0xFC, pom2::kSlotRomBytes)
     .emit({ 0x00, 0x00, 0xF7 })
     .byteOf("driver");

    romLayoutError_ = !a.finish();
}

uint8_t FujiNetCard::slotRomRead(uint8_t low8) { return rom_[low8]; }

// ─────────────────────────────────────────────────────────────────────────
// Guest memory access
// ─────────────────────────────────────────────────────────────────────────

bool FujiNetCard::rangeIsSafe(uint16_t addr, std::size_t n)
{
    if (n == 0) return true;
    // Refuse anything touching the I/O page: reading $C0xx as if it were
    // memory toggles soft switches, so a malformed parameter list could flip
    // video mode or bank state instead of merely failing.
    const uint32_t end = static_cast<uint32_t>(addr) + n - 1;
    if (end > 0xFFFF) return false;                 // would wrap
    if (addr <= 0xC0FF && end >= 0xC000) return false;
    return true;
}

uint8_t FujiNetCard::readGuest(uint16_t addr) const
{
    if (!mem_ || !rangeIsSafe(addr, 1)) return 0;
    return mem_->memRead(addr);
}

void FujiNetCard::writeGuest(uint16_t addr, uint8_t v)
{
    if (!mem_ || !rangeIsSafe(addr, 1)) return;
    mem_->memWrite(addr, v);
}

uint16_t FujiNetCard::readGuest16(uint16_t addr) const
{
    return static_cast<uint16_t>(readGuest(addr)) |
           static_cast<uint16_t>(readGuest(static_cast<uint16_t>(addr + 1)) << 8);
}

bool FujiNetCard::writeGuestBlock(uint16_t addr, const uint8_t* p, std::size_t n)
{
    if (!mem_) return false;
    if (!rangeIsSafe(addr, n)) {
        if (!warnedUnsafeRange_) {
            warnedUnsafeRange_ = true;
            char hex[8];
            std::snprintf(hex, sizeof(hex), "%04X", addr);
            log().warn("FujiNet", std::string("refused a SmartPort transfer "
                                              "through the I/O page (buffer $") +
                                      hex + ")");
        }
        return false;
    }
    for (std::size_t i = 0; i < n; ++i)
        mem_->memWrite(static_cast<uint16_t>(addr + i), p[i]);
    return true;
}

bool FujiNetCard::readGuestBlock(uint16_t addr, uint8_t* p, std::size_t n) const
{
    if (!mem_ || !rangeIsSafe(addr, n)) return false;
    for (std::size_t i = 0; i < n; ++i)
        p[i] = mem_->memRead(static_cast<uint16_t>(addr + i));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────
// Result registers
// ─────────────────────────────────────────────────────────────────────────

void FujiNetCard::finish(uint8_t status, uint8_t x, uint8_t y)
{
    if (!cpu_) return;
    cpu_->setAccumulator(status);
    cpu_->setXRegister(x);
    cpu_->setYRegister(y);

    // The ROM's `CMP #$01` recomputes N/Z/C from A immediately after we
    // return, so these are for callers that enter the driver body directly.
    // Keeping them consistent with what the CMP will produce means the two
    // paths can never disagree.
    uint8_t p = cpu_->getStatusRegister();
    if (status == 0) p |=  M6502::Status::Z; else p &= static_cast<uint8_t>(~M6502::Status::Z);
    if (status == 0) p &= static_cast<uint8_t>(~M6502::Status::C); else p |= M6502::Status::C;
    cpu_->setStatusRegister(p);
}

// ─────────────────────────────────────────────────────────────────────────
// The trap
// ─────────────────────────────────────────────────────────────────────────

uint8_t FujiNetCard::deviceSelectRead(uint8_t) { return 0xFF; }

void FujiNetCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    if (low4 != 0x02) return;
    if (v == kMagicSmartPort) handleSmartPortCall();
    else if (v == kMagicProDOS) handleProDosCall();
}

namespace {
/// Turn a link response into the status byte the guest should see.
/// A peer that is attached but silent is an I/O error; no peer at all is
/// "no device", which is what makes a bus scan with nothing plugged in
/// terminate cleanly instead of reporting broken hardware.
uint8_t statusFor(const FujiNetLink::Response& r, bool connected)
{
    if (r.replied) return r.status;
    return connected ? kSpIoError : kSpNoDevice;
}
} // namespace

void FujiNetCard::handleSmartPortCall()
{
    if (!cpu_ || !mem_) return;
    ++callCount_;

    // The caller did `JSR $Cn0D` (or whatever $CnFF+3 resolves to) followed by
    // three inline bytes: the command, then a pointer to the parameter list.
    // The return address the JSR pushed points at the LAST BYTE OF THE JSR,
    // i.e. one before the command byte.
    //
    // NOTE for anyone comparing with the AppleWin fork: its `regs.sp` is
    // already a full $01xx address, so it indexes mem[regs.sp + 1] directly.
    // POM2's getStackPointer() is the 8-bit register, so the $0100 base and
    // the page-1 wrap are both mandatory here. Getting that wrong corrupts
    // whatever lives at $0001/$0002 instead — silently.
    const uint8_t sp = cpu_->getStackPointer();
    const uint16_t retLoAddr = static_cast<uint16_t>(0x0100 + ((sp + 1) & 0xFF));
    const uint16_t retHiAddr = static_cast<uint16_t>(0x0100 + ((sp + 2) & 0xFF));

    uint16_t ret = static_cast<uint16_t>(readGuest(retLoAddr)) |
                   static_cast<uint16_t>(readGuest(retHiAddr) << 8);
    const uint16_t callRet = ret;

    // Step the return address past the three inline bytes so the ROM's RTS
    // lands on the instruction after them.
    ret = static_cast<uint16_t>(ret + 3);
    writeGuest(retLoAddr, static_cast<uint8_t>(ret & 0xFF));
    writeGuest(retHiAddr, static_cast<uint8_t>(ret >> 8));

    // The three inline bytes and the four-byte command list must be contiguous
    // guest RAM. Validate before any uint16_t addition can wrap $FFFF->$0000.
    const uint32_t inlineWide = static_cast<uint32_t>(callRet) + 1;
    if (inlineWide > 0xFFFF ||
        !rangeIsSafe(static_cast<uint16_t>(inlineWide), 3)) {
        finish(kSpIoError);
        return;
    }
    const uint16_t inlineAddr = static_cast<uint16_t>(inlineWide);
    const uint8_t  command = readGuest(inlineAddr);
    const uint16_t cmdList = readGuest16(static_cast<uint16_t>(inlineAddr + 1));
    if (!rangeIsSafe(cmdList, 4)) { finish(kSpIoError); return; }

    const uint8_t  unit    = readGuest(static_cast<uint16_t>(cmdList + 1));
    const uint16_t payload = readGuest16(static_cast<uint16_t>(cmdList + 2));
    // Command-specific parameters follow the count/unit/pointer triple.
    const uint32_t paramsWide = static_cast<uint32_t>(cmdList) + 4;
    const auto paramsSafe = [paramsWide](std::size_t n) {
        return paramsWide <= 0xFFFF &&
               FujiNetCard::rangeIsSafe(static_cast<uint16_t>(paramsWide), n);
    };
    const uint16_t params = static_cast<uint16_t>(paramsWide);

    const bool connected = link().isConnected();

    // Trace what the GUEST asks for, not only what POM2 forwards. Without
    // this, a call served locally — or refused before it ever reached the
    // link — leaves no trace at all, and the log shows POM2 talking to itself
    // while the guest's real conversation is invisible.
    if (std::getenv("POM2_TRACE_FUJINET"))
        log().info("FujiNet", "guest: cmd=" + std::to_string(command) +
                              " unit=" + std::to_string(unit) +
                              " code=" + std::to_string(readGuest(params)));

    // POM2's own N:, when enabled, answers for the peer's NETWORK unit.
    // `paramsWide <= 0xFFFF` before the narrowing, exactly as the relay path
    // below checks it: a cmdList of $FFFE wraps to $0002, and without this the
    // built-in device would read its byte count out of zero page and answer
    // kSpOk with a bogus length where the relay answers kSpIoError.
    if (builtInNetwork_ && paramsWide <= 0xFFFF &&
        serveBuiltInNetwork(command, unit, params, payload))
        return;

    switch (command) {
    case kSpStatus: {
        if (!paramsSafe(1)) { finish(kSpIoError); return; }
        const uint8_t code = readGuest(params);
        // Unit 0, status code 0 = "how many devices?". Answered locally so a
        // scanning guest gets a sane answer with no peer attached, and does
        // not stall for a timeout per probe.
        if (unit == 0 && code == 0x00) { answerDeviceCount(payload); return; }

        const auto r = link().status(unit, code);
        if (!r.ok()) { finish(statusFor(r, connected)); return; }
        const std::size_t n = r.data.size();
        if (!writeGuestBlock(payload, r.data.data(), n)) { finish(kSpIoError); return; }
        finish(kSpOk, static_cast<uint8_t>(n & 0xFF),
                      static_cast<uint8_t>(n >> 8));
        return;
    }

    case kSpReadBlock: {
        if (!paramsSafe(3)) { finish(kSpIoError); return; }
        const uint32_t block = static_cast<uint32_t>(readGuest(params)) |
                               (static_cast<uint32_t>(readGuest(static_cast<uint16_t>(params + 1))) << 8) |
                               (static_cast<uint32_t>(readGuest(static_cast<uint16_t>(params + 2))) << 16);
        const auto r = link().readBlock(unit, block);
        if (!r.ok()) { finish(statusFor(r, connected)); return; }
        if (r.data.size() < kBlockBytes) { finish(kSpIoError); return; }
        if (!writeGuestBlock(payload, r.data.data(), kBlockBytes)) { finish(kSpIoError); return; }
        finish(kSpOk, 0x00, 0x02);                 // 512 bytes transferred
        return;
    }

    case kSpWriteBlock: {
        if (!paramsSafe(3)) { finish(kSpIoError); return; }
        const uint32_t block = static_cast<uint32_t>(readGuest(params)) |
                               (static_cast<uint32_t>(readGuest(static_cast<uint16_t>(params + 1))) << 8) |
                               (static_cast<uint32_t>(readGuest(static_cast<uint16_t>(params + 2))) << 16);
        uint8_t buf[kBlockBytes];
        if (!readGuestBlock(payload, buf, kBlockBytes)) { finish(kSpIoError); return; }
        const auto r = link().writeBlock(unit, block, buf, kBlockBytes);
        finish(statusFor(r, connected), 0x00, 0x02);
        return;
    }

    case kSpFormat: {
        const auto r = link().format(unit);
        finish(statusFor(r, connected));
        return;
    }

    case kSpControl: {
        if (!paramsSafe(1) || !rangeIsSafe(payload, 2)) {
            finish(kSpIoError);
            return;
        }
        const uint8_t code = readGuest(params);

        // RESET ($00) to the peer's PRINTER unit is answered here instead of
        // being forwarded, and that is a deliberate, narrow exception to
        // "relay everything".
        //
        // The peer ABORTS on it. Measured 2026-08-21, three runs out of
        // three: the request is byte-identical in shape to the ones its
        // neighbouring units answer normally (`04 03 0D 00 00 00 …`, empty
        // control list), yet the firmware throws std::length_error out of
        // Request::from_packet, does not catch it, and the whole FujiNet
        // process dies. It is the same unit whose DIB already advertises the
        // modem's type byte, so its device code is known-shaky upstream.
        //
        // Why this matters more than it looks: EVERY FujiNet program sweeps
        // the SmartPort chain with exactly this call at start-up — CONFIG,
        // NETCAT, the Contiki browser. So the peer died a few seconds into
        // every single session, and the guest then reported whatever it was
        // doing at the time ("connection error", "FujiNet not found") rather
        // than the truth. Answering the reset locally with success costs the
        // guest nothing — resetting a printer that has printed nothing is a
        // no-op — and keeps the peer alive for the rest of the session.
        //
        // Scoped to the printer unit AND to code $00 so every other control
        // call, including the printer's own, still goes to the peer
        // untouched. Remove it once upstream stops aborting.
        if (code == 0x00) {
            for (const auto& d : link().devices()) {
                if (d.unit == unit && d.isPrinter()) {
                    finish(kSpOk);
                    return;
                }
            }
        }
        // The control list is length-prefixed (2 bytes, little-endian) at the
        // pointer the parameter list carries.
        const uint16_t listLen = readGuest16(payload);
        std::vector<uint8_t> list(listLen);
        const uint32_t listAddr = static_cast<uint32_t>(payload) + 2;
        if (listLen && (listAddr > 0xFFFF ||
            !readGuestBlock(static_cast<uint16_t>(listAddr), list.data(), listLen))) {
            finish(kSpIoError);
            return;
        }
        const auto r = link().control(unit, code, list.data(), list.size());
        finish(statusFor(r, connected));
        return;
    }

    case kSpInit: {
        const auto r = link().init(unit);
        finish(statusFor(r, connected));
        return;
    }

    case kSpOpen: {
        const auto r = link().open(unit);
        finish(statusFor(r, connected));
        return;
    }

    case kSpClose: {
        const auto r = link().close(unit);
        finish(statusFor(r, connected));
        return;
    }

    case kSpRead: {
        if (!paramsSafe(5)) { finish(kSpIoError); return; }
        const uint16_t count = readGuest16(params);
        const uint32_t addr  = static_cast<uint32_t>(readGuest(static_cast<uint16_t>(params + 2))) |
                               (static_cast<uint32_t>(readGuest(static_cast<uint16_t>(params + 3))) << 8) |
                               (static_cast<uint32_t>(readGuest(static_cast<uint16_t>(params + 4))) << 16);
        const auto r = link().read(unit, count, addr);
        if (!r.ok()) { finish(statusFor(r, connected)); return; }
        const std::size_t n = std::min<std::size_t>(r.data.size(), count);
        if (n && !writeGuestBlock(payload, r.data.data(), n)) { finish(kSpIoError); return; }
        finish(kSpOk, static_cast<uint8_t>(n & 0xFF),
                      static_cast<uint8_t>(n >> 8));
        return;
    }

    case kSpWrite: {
        if (!paramsSafe(5)) { finish(kSpIoError); return; }
        const uint16_t count = readGuest16(params);
        const uint32_t addr  = static_cast<uint32_t>(readGuest(static_cast<uint16_t>(params + 2))) |
                               (static_cast<uint32_t>(readGuest(static_cast<uint16_t>(params + 3))) << 8) |
                               (static_cast<uint32_t>(readGuest(static_cast<uint16_t>(params + 4))) << 16);
        std::vector<uint8_t> data(count);
        if (count && !readGuestBlock(payload, data.data(), count)) {
            finish(kSpIoError);
            return;
        }
        const auto r = link().write(unit, count, addr, data.data(), data.size());
        // Printer tap: the peer prints its own copy, and POM2's ImageWriter
        // prints one too. Only on success — a write the FujiNet rejected did
        // not reach paper there and must not reach paper here either.
        if (r.ok() && !data.empty()) tapPrinterWrite(unit, data.data(), data.size());
        finish(statusFor(r, connected),
               static_cast<uint8_t>(count & 0xFF),
               static_cast<uint8_t>(count >> 8));
        return;
    }

    default:
        // Extended ($4x) and unknown calls. A //e never issues them, and
        // answering "bad command" is what a real controller that does not
        // implement them does.
        finish(kSpBadCommand);
        return;
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Printer tap
// ─────────────────────────────────────────────────────────────────────────

bool FujiNetCard::hasPrinterUnit() const
{
    for (const auto& d : link().devices())
        if (d.isPrinter()) return true;
    return false;
}

void FujiNetCard::tapPrinterWrite(uint8_t unit, const uint8_t* p, std::size_t n)
{
    // Ask the enumeration whether this unit is the printer. `devices()` takes
    // its own lock and copies, which is fine at printer rates (a page is a few
    // KB, arriving in ~80-byte writes) and keeps the check honest if the peer
    // re-enumerates.
    bool isPrinter = false;
    for (const auto& d : link().devices())
        if (d.unit == unit) { isPrinter = d.isPrinter(); break; }
    if (!isPrinter) return;

    std::lock_guard<std::mutex> lk(printerMtx_);
    if (n >= kMaxPrinterSpoolBytes) {
        printerSpool_.assign(p + (n - kMaxPrinterSpoolBytes), p + n);
        printerSpoolBase_ = printerSpoolTotal_ + n - kMaxPrinterSpoolBytes;
    } else {
        const size_t overflow = printerSpool_.size() + n > kMaxPrinterSpoolBytes
            ? printerSpool_.size() + n - kMaxPrinterSpoolBytes : 0;
        for (size_t i = 0; i < overflow; ++i) printerSpool_.pop_front();
        printerSpoolBase_ += overflow;
        printerSpool_.insert(printerSpool_.end(), p, p + n);
    }
    printerSpoolTotal_ += n;
}

size_t FujiNetCard::bytesWritten() const
{
    std::lock_guard<std::mutex> lk(printerMtx_);
    return printerSpoolTotal_;
}

size_t FujiNetCard::drainSpoolFrom(size_t from, std::vector<uint8_t>& out) const
{
    std::lock_guard<std::mutex> lk(printerMtx_);
    // Absolute indices let the UI keep streaming while old preview bytes are
    // evicted. A cursor from before clear() resynchronises at the new front.
    const size_t absolute = (from > printerSpoolTotal_)
        ? printerSpoolBase_ : std::max(from, printerSpoolBase_);
    const size_t start = absolute - printerSpoolBase_;
    out.insert(out.end(),
               printerSpool_.begin() + static_cast<std::ptrdiff_t>(start),
               printerSpool_.end());
    return printerSpoolTotal_;
}

void FujiNetCard::clearPrinterSpool()
{
    std::lock_guard<std::mutex> lk(printerMtx_);
    printerSpool_.clear();
    printerSpoolBase_ = 0;
    printerSpoolTotal_ = 0;
}

void FujiNetCard::answerDeviceCount(uint16_t payloadAddr)
{
    ++localCount_;
    // Status list for the unit-0 / code-0 call: device count, then reserved
    // bytes. Eight bytes total, which is what X/Y report.
    uint8_t list[8] = {};
    std::size_t count = link().deviceCount();
    // The built-in N: is a device the guest must be able to FIND. The count
    // comes from the peer's chain, and that chain is empty whenever no peer
    // is attached — so without this the guest is told "no devices", never
    // probes, and reports a network failure that is really an absent peer.
    // Claim at least up to our own unit so the scan reaches it.
    if (builtInNetwork_) count = std::max<std::size_t>(count, builtInNetUnit());
    list[0] = static_cast<uint8_t>(std::min<std::size_t>(count, 255));
    if (!writeGuestBlock(payloadAddr, list, sizeof(list))) { finish(kSpIoError); return; }
    finish(kSpOk, static_cast<uint8_t>(sizeof(list)), 0x00);
}

// ─────────────────────────────────────────────────────────────────────────
// ProDOS entry
// ─────────────────────────────────────────────────────────────────────────


// ─────────────────────────────────────────────────────────────────────────
// Built-in N:
// ─────────────────────────────────────────────────────────────────────────

namespace {
/// General-status byte for the built-in N:. Bits, per the IIgs Firmware
/// Reference ch. 7: b6 write allowed, b5 read allowed, b4 online. b7 (block
/// device) and b3 (format allowed) stay CLEAR — this is a character device
/// and there is nothing to format.
constexpr uint8_t kGeneralStatusChar = 0x70;
}  // namespace

uint8_t FujiNetCard::builtInNetUnit() const
{
    // A SmartPort chain is CONTIGUOUS 1..N: what unit 0 answers is a COUNT,
    // not a highest-unit-number, and every standard chain walk — POM2's own
    // included (SpOverSlipLink::enumerateDevices) — stops at the first unit
    // that answers "no device". So the built-in device has to sit INSIDE the
    // chain. Parking it at a fixed 11 made it unreachable by the very scan
    // meant to find it: with no peer, the guest probed unit 1, got nothing,
    // and never looked further.
    for (const auto& d : link().devices())
        if (d.name == "NETWORK" || d.type == kSpTypeNetwork) {
            const_cast<FujiNetCard*>(this)->netUnit_ = d.unit;   // override in place
            return netUnit_;
        }
    // The peer has no network device (or there is no peer): take the slot just
    // past its last one. Held steady while the guest has a session open,
    // though — moving the unit under its feet because the peer died mid-fetch
    // would strand it mid-page.
    if (net_->isOpen() && netUnit_) return netUnit_;
    const std::size_t next = link().deviceCount() + 1;
    const_cast<FujiNetCard*>(this)->netUnit_ =
        static_cast<uint8_t>(std::min<std::size_t>(next, 254));
    return netUnit_;
}

bool FujiNetCard::serveBuiltInNetwork(uint8_t command, uint8_t unit,
                                      uint16_t params, uint16_t payload)
{
    // `params` already fits 16 bits here (the caller narrowed it), so the
    // guard the relay path spells as a lambda is just a range check.
    const auto paramsSafe = [params](std::size_t n) {
        return FujiNetCard::rangeIsSafe(params, n);
    };

    // Only the unit the PEER calls its network device. Keyed on the device
    // table rather than a fixed number because the unit is wherever the
    // FujiNet's chain happens to put it (11 on the desktop build) — and if no
    // peer has enumerated yet there is nothing to shadow, so the call falls
    // through to the normal path and fails the way it always did.
    if (unit != builtInNetUnit()) return false;

    // While a CONNECTED peer is still enumerating, its chain is unknown and
    // devices() is empty — the list is cleared on peer loss and only
    // republished at the end of the whole INIT sweep. Claiming a unit in that
    // window would shadow whatever the peer is about to publish there, so
    // wait rather than guess. With no peer at all there is nothing to shadow
    // and the built-in device answers immediately, which is the case it
    // exists for.
    if (link().isConnected() && link().deviceCount() == 0) return false;

    // And never shadow a device the PEER really has at that unit. The chosen
    // unit is remembered across a peer loss, so a peer that comes back with a
    // different chain — a disk where our network device used to sit — would
    // otherwise have its blocks answered by a network device, and ProDOS
    // would report an I/O error on a perfectly good volume.
    for (const auto& d : link().devices())
        if (d.unit == unit && d.name != "NETWORK" && d.type != kSpTypeNetwork)
            return false;

    switch (command) {
    case kSpControl: {
        if (!paramsSafe(1) || !rangeIsSafe(payload, 2)) { finish(kSpIoError); return true; }
        const uint8_t  code    = readGuest(params);
        const uint16_t listLen = readGuest16(payload);
        std::vector<uint8_t> list(listLen);
        const uint32_t listAddr = static_cast<uint32_t>(payload) + 2;
        if (listLen && (listAddr > 0xFFFF ||
            !readGuestBlock(static_cast<uint16_t>(listAddr), list.data(), listLen))) {
            finish(kSpIoError);
            return true;
        }
        switch (code) {
        case kNetOpen: {
            if (std::getenv("POM2_TRACE_FUJINET")) {
                std::string hex;
                for (std::size_t i = 0; i < list.size() && i < 40; ++i) {
                    char b[4]; std::snprintf(b, sizeof b, "%02X ", list[i]); hex += b;
                }
                log().info("FujiNet", "N: OPEN listLen=" + std::to_string(listLen) +
                                      " payload=$" + std::to_string(payload) + " : " + hex);
            }
            // The control list is aux1 (open mode), aux2 (translation), THEN
            // the devicespec — measured off the wire from the FujiNet Contiki
            // browser: `04 00 4E 3A 68 74 74 70 ...` = mode 4, translation 0,
            // "N:http://…". Taking the whole list as the spec put two binary
            // bytes in front of the URL and every open failed with an empty
            // host. The guest may or may not terminate the spec, so trim at
            // the first NUL rather than trusting the declared length.
            const std::size_t specOff = (list.size() >= 2) ? 2 : 0;
            std::string spec(reinterpret_cast<const char*>(list.data() + specOff),
                             list.size() - specOff);
            const std::size_t nul = spec.find('\0');
            if (nul != std::string::npos) spec.resize(nul);
            finish(net_->open(spec) ? kSpOk : kSpIoError);
            return true;
        }
        case kNetClose:
            net_->close();
            finish(kSpOk);
            return true;
        default:
            // Everything else the guest asks of N: — channel mode, EOL
            // translation, parse/query — is accepted and ignored. Saying "no"
            // makes guest code give up on the device entirely; saying "fine"
            // costs a plain HTTP fetch nothing.
            finish(kSpOk);
            return true;
        }
    }

    case kSpStatus: {
        if (!paramsSafe(1)) { finish(kSpIoError); return true; }
        const uint8_t code = readGuest(params);
        if (code == 0x03) {
            // The DIB, answered here so the device exists for a guest that is
            // scanning the chain — including when no peer ever attached.
            // Layout as the spec lays it out: general status, 3-byte block
            // count (zero, this is a character device), name length, 16-byte
            // name, type, subtype, version.
            static const char kName[] = "NETWORK";
            uint8_t dib[25] = {};
            // General status bits (IIgs Firmware Ref. ch. 7): b7 block device,
            // b6 write allowed, b5 read allowed, b4 online, b3 format allowed.
            // This is a CHARACTER device, so b7 must be CLEAR — 0xF8 told the
            // guest it was a block device with a zero block count that could
            // be formatted, which is three claims at once and all wrong.
            dib[0] = kGeneralStatusChar;
            dib[4] = static_cast<uint8_t>(sizeof(kName) - 1);
            std::memset(dib + 5, ' ', 16);
            std::memcpy(dib + 5, kName, sizeof(kName) - 1);
            dib[21] = kSpTypeNetwork;
            dib[23] = 0x01;
            if (!writeGuestBlock(payload, dib, sizeof dib)) { finish(kSpIoError); return true; }
            finish(kSpOk, static_cast<uint8_t>(sizeof dib), 0x00);
            return true;
        }
        if (code == 0x00) {
            // The STANDARD general-status call, which is not the network
            // status. Answering the network reply here put a $00 in byte 0
            // whenever nothing was open, and byte 0 is the status byte: bit 4
            // clear reads as "device offline, no read, no write", so a chain
            // walker concluded N: was dead before ever opening anything.
            const uint8_t gen[4] = { kGeneralStatusChar, 0x00, 0x00, 0x00 };
            if (!writeGuestBlock(payload, gen, sizeof gen)) { finish(kSpIoError); return true; }
            finish(kSpOk, static_cast<uint8_t>(sizeof gen), 0x00);
            return true;
        }
        uint8_t st[4];
        net_->status(st);
        if (!writeGuestBlock(payload, st, sizeof st)) { finish(kSpIoError); return true; }
        finish(kSpOk, static_cast<uint8_t>(sizeof st), 0x00);
        return true;
    }

    case kSpRead: {
        if (!paramsSafe(5)) { finish(kSpIoError); return true; }
        const uint16_t count = readGuest16(params);
        // Check the DESTINATION before touching the cursor. net_->read()
        // CONSUMES, and a write refused afterwards left those bytes gone for
        // good: the guest retried at a good address, received the NEXT chunk,
        // and the page silently lost a piece with no error anywhere in sight.
        const std::size_t want = std::min<std::size_t>(count, net_->available());
        if (want && !rangeIsSafe(payload, want)) { finish(kSpIoError); return true; }
        std::vector<uint8_t> buf(want);
        const std::size_t n = net_->read(buf.data(), want);
        if (n && !writeGuestBlock(payload, buf.data(), n)) { finish(kSpIoError); return true; }
        finish(kSpOk, static_cast<uint8_t>(n & 0xFF), static_cast<uint8_t>(n >> 8));
        return true;
    }

    case kSpInit:
    case kSpOpen:
    case kSpClose:
        finish(kSpOk);
        return true;

    default:
        // Writes and anything else: not served here, and NOT forwarded
        // either — the peer's idea of this unit's state and ours would
        // diverge. Reported as a clean I/O error instead.
        finish(kSpIoError);
        return true;
    }
}

void FujiNetCard::handleProDosCall()
{
    if (!cpu_ || !mem_) return;
    ++callCount_;

    const uint8_t  command  = readGuest(kZpCommand);
    const uint8_t  unitByte = readGuest(kZpUnit);
    if (std::getenv("POM2_TRACE_FUJINET"))
        log().info("FujiNet", "guest(ProDOS): cmd=" + std::to_string(command) +
                              " unit=" + std::to_string(unitByte));
    const uint16_t buffer   = readGuest16(kZpBufLo);
    const uint16_t block    = readGuest16(kZpBlkLo);

    // ProDOS addresses two drives per slot; map them onto the first two
    // SmartPort units, which is what every FujiNet configuration expects.
    const uint8_t unit = (unitByte & 0x80) ? 2 : 1;

    const bool connected = link().isConnected();
    if (!connected) { finish(kSpNoDevice); return; }

    switch (command) {
    case 0x00: {                                   // STATUS
        const auto r = link().status(unit, 0x00);
        if (!r.ok()) { finish(statusFor(r, connected)); return; }
        // General status: status byte, then a 3-byte block count. ProDOS
        // wants the low 16 bits of that count in X/Y.
        const uint8_t lo = r.data.size() > 1 ? r.data[1] : 0;
        const uint8_t hi = r.data.size() > 2 ? r.data[2] : 0;
        finish(kSpOk, lo, hi);
        return;
    }

    case 0x01: {                                   // READ
        const auto r = link().readBlock(unit, block);
        if (!r.ok()) { finish(statusFor(r, connected)); return; }
        if (r.data.size() < kBlockBytes) { finish(kSpIoError); return; }
        if (!writeGuestBlock(buffer, r.data.data(), kBlockBytes)) { finish(kSpIoError); return; }
        finish(kSpOk, 0x00, 0x02);
        return;
    }

    case 0x02: {                                   // WRITE
        uint8_t buf[kBlockBytes];
        if (!readGuestBlock(buffer, buf, kBlockBytes)) { finish(kSpIoError); return; }
        const auto r = link().writeBlock(unit, block, buf, kBlockBytes);
        finish(statusFor(r, connected), 0x00, 0x02);
        return;
    }

    case 0x03: {                                   // FORMAT
        const auto r = link().format(unit);
        finish(statusFor(r, connected));
        return;
    }

    default:
        finish(kSpBadCommand);
        return;
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────

void FujiNetCard::onReset()
{
    // The spec asks the Apple II side to tell connected devices about a 6502
    // reset (Control code $00) so a modem drops its connection and a printer
    // ejects a partial page, and to make sure a response still in flight for
    // the pre-reset request cannot be mistaken for the next answer.
    link().notifyGuestReset();
}

void FujiNetCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    // 'FNET', version 1. There is no rewindable device state to save — the
    // devices are not in this process — so the blob exists mainly so that
    // LOADING one can resynchronise the link (see below).
    out.push_back('F'); out.push_back('N'); out.push_back('E'); out.push_back('T');
    out.push_back(0x01);
    out.push_back(link().isConnected() ? 1 : 0);
}

void FujiNetCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    // A foreign or older blob is ignored, per the SlotPeripheral contract:
    // the slot might have held a different card when the snapshot was taken.
    if (len < 6 || data[0] != 'F' || data[1] != 'N' || data[2] != 'E' ||
        data[3] != 'T' || data[4] != 0x01)
        return;

    // THE HONEST LIMITATION. A rewind moves the guest's clock backwards; the
    // FujiNet's does not move at all. Blocks it wrote stay written and HTTP
    // requests stay made. All this can do is make sure the LINK is coherent
    // afterwards: bump the sequence number so a response in flight for a
    // request that (from the guest's point of view) never happened is
    // rejected as stale.
    //
    // Deliberately NOT notifyGuestReset(): the user rewound, the machine did
    // not reset, and hanging up somebody's modem because they scrubbed the
    // timeline would be wrong.
    //
    // data[5] is the link state AT CAPTURE. `appendSnapshotState` has always
    // written it and this loader used to ignore it — a byte in the wire
    // format with no reader, which is how a format grows a field nobody can
    // change safely. It is worth exactly one diagnostic: if the link came or
    // went across the jump, the guest's SmartPort device map now describes a
    // relay that is not the one answering, and the user is the only one who
    // can act on that.
    const bool wasConnected = data[5] != 0;
    const bool nowConnected = link().isConnected();
    if (wasConnected != nowConnected) {
        pom2::log().warn("FujiNet",
            std::string("snapshot/rewind crossed a link change (was ") +
            (wasConnected ? "connected" : "disconnected") + ", now " +
            (nowConnected ? "connected" : "disconnected") +
            ") — the guest's device map may no longer match the relay");
    }
    link().resync();
}

} // namespace pom2
