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

// FujiNetCard — a SmartPort controller whose devices live outside POM2.
//
// FujiNet (https://fujinet.online/) is an ESP32 peripheral for 8-bit machines
// whose headline feature is the `N:` NETWORK DEVICE: a deported TCP/IP stack
// the guest drives with simple commands, so an Apple II gets HTTP, TNFS, FTP,
// SSH, Telnet and a JSON parser without running a byte of TCP/IP itself. On
// the Apple II EVERY FujiNet function is a SmartPort unit — block storage, the
// network device, the clock, the printer, the modem, CP/M — so relaying the
// SmartPort protocol relays all of them at once. That is why this is one card
// and not six.
//
// This card is a RELAY, not an emulation. It presents a SmartPort controller
// to the guest and forwards every call, verbatim, to a real FujiNet over
// SP-over-SLIP (SpOverSlipLink) — either a FujiNet desktop build over loopback
// TCP, or a physical board over USB CDC-ACM.
//
// ── Sources ───────────────────────────────────────────────────────────────
//
// MAME HAS NO FUJINET DEVICE, so the project's usual "MAME = source of truth"
// rule cannot apply here. The references are:
//
//   * the protocol spec: FujiNet wiki "Apple II SP over SLIP", revision of
//     2025-01-25 — normative for framing and the request/response tables;
//   * the working reference implementation in the FujiNet fork of AppleWin
//     (`source/SmartPortOverSlip.cpp`, `source/devrelay/**`,
//     `firmware/SPoverSLIP/spoverslip.s`), GPL-2.0-or-later, so
//     GPLv3-compatible — consulted, not copied;
//   * Apple IIc Technical Reference ch. 6 (Block Device I/O) and Apple IIgs
//     Firmware Reference ch. 7 (SmartPort Firmware) for the call convention
//     the spec supplements.
//
// ── How the trap works ────────────────────────────────────────────────────
//
// The slot ROM is synthesised by POM2 (no dump needed, same H1 pattern as
// ProDOSHardDiskCard) and does almost nothing. Both entry points are three
// instructions:
//
//     LDA #$65        ; $66 for the ProDOS entry
//     STA $C0n2       ; ← the trap: control passes to the host here
//     CMP #$01        ; turns A != 0 into carry-set
//     RTS
//
// `deviceSelectWrite` sees that store and does everything on the host: it
// reads the SmartPort parameter list out of emulated RAM, REWRITES THE RETURN
// ADDRESS ON THE STACK so the ROM's RTS lands after the three inline bytes a
// SmartPort call carries, performs the SP-over-SLIP round trip, writes the
// response back into emulated RAM, and sets A/X/Y and the flags. The guest
// cannot tell the difference from a fast SmartPort controller.
//
// Memory and CPU access go through Memory::memRead/memWrite and the M6502
// accessors, injected by the host at plug time — the same pattern SoftCardZ80
// uses, and for the same reason: only the real dispatcher knows whether a
// ProDOS buffer currently resolves to main or aux RAM.
//
// ── What this card does NOT do ────────────────────────────────────────────
//
//   * //c-class machines. Their forced INTCXROM masks all slot ROM, so the
//     card is meaningful on II+ / //e only. On a real //c the FujiNet *is*
//     the SmartPort on the disk port, which would mean hanging the relay off
//     the on-board $C500 hole instead — a separate piece of work.
//   * Rewind. The peer is a live external device; rewinding the guest past a
//     WriteBlock does not un-write the peer's SD card, and past an HTTP POST
//     does not un-post it. The ring keeps working; this card simply does not
//     roll back with it. See loadSnapshotState.

#ifndef POM2_FUJINET_CARD_H
#define POM2_FUJINET_CARD_H

#include "ChildProcess.h"
#include "SlotPeripheral.h"
#include "FujiNetLink.h"
#include "FujiNetNetwork.h"
#include "FujiNetTransport.h"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class Memory;
class M6502;

namespace pom2 {

class FujiNetCard : public SlotPeripheral
{
public:
    bool sessionReplacementReady() const override { return false; }
    /// Slot 7 by default: the //e autostart scans it BEFORE the Disk II in
    /// slot 6, so a machine with a FujiNet attached boots straight into its
    /// CONFIG program — the arrangement fujinet-go-apple2-desktop ships.
    /// When no FujiNet answers, the ROM's boot path continues the scan
    /// ($FABA) so slot 6 still boots normally.
    static constexpr int kDefaultSlot = 7;

    /// Magic values the ROM stores to $C0n2 to enter the host. Two, because
    /// the ProDOS and SmartPort calling conventions carry their parameters in
    /// completely different places.
    static constexpr uint8_t kMagicSmartPort = 0x65;
    static constexpr uint8_t kMagicProDOS    = 0x66;
    static constexpr size_t kMaxPrinterSpoolBytes = 4u * 1024u * 1024u;

    /// The card is a DEVICE: it issues SmartPort commands and serves `N:`
    /// calls, and it must not know that the far end is a socket, a USB CDC
    /// device or a helper process. Both host implementations are therefore
    /// injected rather than constructed here — see `makeFujiNetCard()`
    /// (FujiNetCardFactory.h, RUNTIME) for the production wiring, which is
    /// the only place that names SpOverSlipLink and FujiNetNetDevice.
    ///
    /// `transport` is a reference because there is always one, and it
    /// normally aliases `link` (SpOverSlipLink implements both interfaces).
    /// Passing it separately is what keeps the protocol surface and the host
    /// lifecycle surface independent — a test can hand over canned SmartPort
    /// replies while still driving a real transport, or the reverse.
    FujiNetCard(int slot,
                std::unique_ptr<FujiNetLink> link,
                FujiNetTransport& transport,
                std::unique_ptr<FujiNetNetwork> network);
    ~FujiNetCard() override;

    int getSlot() const { return slot_; }

    /// Host wiring, at plug time. Without both of these the card answers
    /// every call with "no device" rather than misbehaving.
    void setMemory(Memory* m) { mem_ = m; }
    void setCpu(M6502* c)     { cpu_ = c; }

    /// The link is owned by the card and exposed so the panel and the host
    /// can configure the transport, read counters and enumerate devices.
    /// The command surface the card actually uses. Returning the interface
    /// rather than the concrete link is what lets a test drive the card with
    /// canned SmartPort replies and no socket, helper process or peer.
    FujiNetLink&       link()       { return *link_; }
    const FujiNetLink& link() const { return *link_; }


    /// Transport lifecycle — arming a transport, starting and stopping the
    /// worker, counters — is NOT part of the command surface, so it is a
    /// separate interface. Never null.
    FujiNetTransport&       transportLink()       { return *transport_; }
    const FujiNetTransport& transportLink() const { return *transport_; }

    // ── Built-in N: (POM2's own network device) ───────────────────────────
    //
    // With this on, calls addressed to the peer's NETWORK unit are served by
    // POM2 out of host sockets instead of being forwarded. Everything else —
    // disks, CONFIG, the clock — still goes to the peer.
    //
    // It exists because the FujiNet desktop build's own N: answers the
    // guest's open with success and then never opens a socket, so relaying
    // faithfully to it means the guest can never fetch anything. A real
    // FujiNet board over USB has a working N:; leave this OFF for one.
    void setBuiltInNetwork(bool on) { builtInNetwork_ = on; }
    bool builtInNetwork() const     { return builtInNetwork_; }

    // ── Printer tap (phase 2) ─────────────────────────────────────────────
    //
    // Bytes the guest WRITEs to the peer's printer unit are also spooled
    // here, so POM2's own ImageWriter renders the job on its paper tray
    // instead of the user having to fetch a PDF off the FujiNet's web UI.
    // The peer still prints its own copy — this is a tap, not a diversion.
    //
    // Same `bytesWritten()` / `drainSpoolFrom()` contract as PrinterCard,
    // GrapplerCard and the SSC tap, so MainWindow::pumpImageWriter drives it
    // through the shared `printerFeedCursor` handover rules unchanged.
    //
    // WHICH UNIT IS THE PRINTER: by its DIB **name**, not its type byte.
    // The FujiNet firmware's `iwmPrinter::create_dib_reply_packet`
    // (lib/device/iwm/printer.cpp:32) sets `dib.type =
    // SP_TYPE_BYTE_FUJINET_MODEM` — the printer advertises itself as a
    // MODEM, which is an upstream copy-paste bug. The name is "PRINTER" and
    // is right, so that is what POM2 keys on; the correct type byte ($14) is
    // accepted too, so this keeps working when upstream fixes it.
    /// Locked: written on the CPU thread, read on the UI thread.
    size_t bytesWritten() const;
    size_t drainSpoolFrom(size_t from, std::vector<uint8_t>& out) const;
    bool   drainPrinterSpool(std::size_t from, std::vector<uint8_t>& out,
                             std::size_t& next) const override
    { if (!hasPrinterUnit()) return false; next = drainSpoolFrom(from, out); return true; }
    void   clearPrinterSpool();
    /// Whether a printer unit was found in the peer's enumeration. Drives the
    /// panel's hint and lets MainWindow skip the card as a printer source.
    bool   hasPrinterUnit() const;

    // ── Helper process (phase 3) ──────────────────────────────────────────
    //
    // POM2 can START a FujiNet desktop build for the user rather than making
    // them run it by hand, and reaps it on exit — a helper left behind holds
    // the loopback port the next session wants to listen on.
    //
    // POM2 does NOT touch the helper's `fnconfig.ini`. That file holds the
    // user's WiFi credentials, and it does not need touching: the firmware's
    // Apple default for Bus-over-IP is already 127.0.0.1:1985
    // (`CONFIG_DEFAULT_BOIP_PORT` in lib/config/fnConfig.h), which is exactly
    // what this card listens on. If a user has pointed their FujiNet
    // somewhere else, the panel says the helper started but never connected
    // rather than silently rewriting their configuration.
    //
    // NOTE: this is deliberately NOT the "build the firmware into POM2"
    // design sketched in docs/fujinet_plan.md § 8. See that section for why
    // vendoring it was rejected.
    ChildProcess&       helper()       { return helper_; }
    const ChildProcess& helper() const { return helper_; }

    /// Start the configured helper. `exePath` empty = look for `fujinet` on
    /// PATH and in the usual install locations.
    bool startHelper(const std::string& exePath, std::string& errOut);

    // ── SlotPeripheral ────────────────────────────────────────────────────
    std::string_view name() const override { return "FujiNet"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead      (uint8_t low8) override;
    void    onReset() override;

    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

    // ── Exposed for the smoke test ────────────────────────────────────────
    /// Number of SmartPort calls the card has serviced, and how many were
    /// answered locally (device-count queries) rather than forwarded.
    uint64_t callCount()  const { return callCount_; }
    uint64_t localCount() const { return localCount_; }

    /// True when the hand-assembled slot ROM did not fit its declared layout.
    /// Always false in a healthy build — `fujinet_rom_layout` asserts it, and
    /// mutation-checks that shrinking a region's budget trips it.
    bool romLayoutError() const { return romLayoutError_; }

private:
    /// Which unit the built-in N: answers on: whatever the peer calls its
    /// network device, or 11 — where every current FujiNet build puts it —
    /// when no peer has enumerated. Sticky, because the device list is
    /// cleared when a peer dies and the built-in N: must not die with it.
    uint8_t builtInNetUnit() const;

    /// Serve one SmartPort call from the built-in N: instead of the peer.
    /// Returns false when this call is not ours to answer.
    bool serveBuiltInNetwork(uint8_t command, uint8_t unit, uint16_t params,
                             uint16_t payload);

    std::unique_ptr<FujiNetNetwork> net_;
    bool                            builtInNetwork_ = false;
    /// Which unit the peer called its network device, REMEMBERED. The device
    /// list is cleared when a peer dies, and the peer dies easily; without
    /// this the built-in N: would stop answering at exactly the moment it is
    /// most needed, and the guest would report a network failure that is
    /// really a bookkeeping one. 0 = not seen yet.
    uint8_t          netUnit_ = 0;

    // ── ROM ───────────────────────────────────────────────────────────────
    void buildRom();

    // ── Trap handlers ─────────────────────────────────────────────────────
    void handleSmartPortCall();
    void handleProDosCall();

    /// STATUS to unit 0, code 0 — "how many devices are on this bus?". Answered
    /// from the link's enumeration WITHOUT a round trip, because a guest
    /// scanning the bus must get a sane answer even with no peer attached.
    void answerDeviceCount(uint16_t payloadAddr);

    // ── Guest memory helpers ──────────────────────────────────────────────
    // Everything goes through Memory so the current paging state decides
    // whether an address means main or aux RAM. Accesses to $C000-$CFFF are
    // refused: the range maps soft switches, slot ROM and expansion ROM.
    uint8_t  readGuest(uint16_t addr) const;
    void     writeGuest(uint16_t addr, uint8_t v);
    uint16_t readGuest16(uint16_t addr) const;
    /// Copy `n` bytes from the response into guest RAM at `addr`. Returns
    /// false (and copies nothing) if the range touches peripheral space.
    bool     writeGuestBlock(uint16_t addr, const uint8_t* p, std::size_t n);
    bool     readGuestBlock(uint16_t addr, uint8_t* p, std::size_t n) const;
    static bool rangeIsSafe(uint16_t addr, std::size_t n);

    /// Spool a successful printer WRITE for the host ImageWriter. No-op when
    /// `unit` is not the peer's printer.
    void tapPrinterWrite(uint8_t unit, const uint8_t* p, std::size_t n);

    /// Set the 6502 result registers and flags for a completed call.
    /// `status` is the SmartPort error code ($00 = success); `x`/`y` carry the
    /// transfer length where the command defines one.
    void finish(uint8_t status, uint8_t x = 0, uint8_t y = 0);

    int      slot_;
    Memory*  mem_ = nullptr;
    M6502*   cpu_ = nullptr;

    std::array<uint8_t, 256> rom_{};
    /// Set by buildRom() when a hand-assembled region overran its budget or
    /// stopped ending where the layout's hard-coded branch offsets assume.
    /// Read by `fujinet_card_smoke`; see SlotRomAsm.h for why it exists.
    bool                     romLayoutError_ = false;
    /// The owned command surface, and the host lifecycle surface that
    /// normally aliases it. `transport_` is non-owning by design: it points
    /// into `link_` in the production wiring.
    std::unique_ptr<FujiNetLink> link_;
    FujiNetTransport*            transport_ = nullptr;
    ChildProcess             helper_;

    uint64_t callCount_  = 0;
    uint64_t localCount_ = 0;

    /// Printer tap spool. Written on the CPU thread (inside a SmartPort
    /// call) and drained on the UI thread once per frame, so it needs its
    /// own lock — every other member here is CPU-thread-only.
    mutable std::mutex   printerMtx_;
    std::deque<uint8_t> printerSpool_;
    size_t printerSpoolBase_ = 0;
    size_t printerSpoolTotal_ = 0;
    /// Logged once, not once per access: a program poking a bad parameter
    /// list would otherwise flood the log.
    bool     warnedUnsafeRange_ = false;
};

} // namespace pom2

#endif // POM2_FUJINET_CARD_H
