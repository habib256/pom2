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

// Cs8900aDevice — Crystal Semiconductor CS8900A "Crystal LAN" 10Base-T
// Ethernet MAC. This is the chip on the a2RetroSystems **Uthernet I**.
//
// Port of MAME `src/devices/machine/cs8900a.cpp` (GPL-2.0+, Rhett Aultman,
// itself a port of Spiro Trikaliotis' VICE CS8900 model). Line citations
// below are against that file; POM2 is GPL-3.0, which GPL-2.0+ permits.
//
// Chip model in one paragraph
// ---------------------------
// The CS8900A exposes just 16 bytes of I/O space (mapped straight onto a
// slot's $C0nX device-select window by UthernetCard) and hides everything
// else behind a 4 KB indirect register file called the **PacketPage**.
// The host sets a PacketPage pointer at $C0nA/B, then reads or writes the
// data window at $C0nC/D; setting bit 15 of the pointer makes it
// auto-increment. Frames are not DMA'd anywhere — a received frame is
// deposited at PacketPage $0400 and read out a byte at a time through the
// RXTXDATA window at $C0n0/1, and a frame to send is written into that
// same window after arming TxCMD/TxLength.
//
// I/O space map (MAME `cs8900a.cpp:50-70`, datasheet §4.10 p.75):
//
//   $C0n0/1   RXTXDATA   RW  receive-data pop / transmit-data push
//   $C0n2/3   RXTXDATA2  RW  32-bit alias of the above
//   $C0n4/5   TXCMD      -W  transmit command   → PacketPage $0144
//   $C0n6/7   TXLENGTH   -W  transmit length    → PacketPage $0146
//   $C0n8/9   INTSTQUEUE R-  interrupt status Q → PacketPage $0120
//   $C0nA/B   PP_PTR     RW  PacketPage pointer (bit 15 = auto-increment)
//   $C0nC/D   PP_DATA0   RW  PacketPage data window
//   $C0nE/F   PP_DATA1   RW  32-bit alias of the above
//
// Transmit is a four-step handshake the model tracks in `txState_`
// (`cs8900a.cpp:210-215`): write TxCMD (→ GOT_CMD), write TxLength
// (→ GOT_LEN, if 4 <= len <= 1518), read BusST and observe Rdy4TxNOW
// (→ READ_BUSST), then push `len` bytes through RXTXDATA. The last byte
// releases the frame to the host backend.
//
// Receive is polled: reading the RxEvent status register at PacketPage
// $0124 pulls the next accepted frame out of the queue into PacketPage
// $0400 and flips `rxState_` to GOT_FRAME (`cs8900a.cpp:938-980`). The
// host then reads RxStatus, RxLength and the payload back through
// RXTXDATA. Reading RxEvent again before the payload is drained is an
// "implied skip" and discards it — that is real hardware behaviour.
//
// What POM2 changes vs MAME
// -------------------------
//   * MAME is *pushed* frames by `device_network_interface::recv_start_cb`
//     (`cs8900a.cpp:1483-1512`). POM2 has no such bus, so `pumpBackend()`
//     pulls from a `NetworkBackend` on the card's cycle hook and applies
//     the same `shouldAccept()` pre-filter before queueing.
//   * MAME's `assert()`-heavy accessors are replaced by clamped indexing:
//     a mis-decoded $C0nX must never take the emulator down.
//   * `machine().side_effects_disabled()` (MAME's debugger read guard) has
//     no POM2 equivalent; `peek()` provides the same side-effect-free read
//     for the debug panel.

#ifndef POM2_CS8900A_DEVICE_H
#define POM2_CS8900A_DEVICE_H

#include "NetworkBackend.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace pom2 {

class Cs8900aDevice
{
public:
    /// 16 visible I/O registers (`cs8900a.cpp` `CS8900_COUNT_IO_REGISTER`).
    static constexpr uint16_t kIoRegisterCount = 0x10;
    /// 4 KB PacketPage (`MAX_PACKETPAGE_ARRAY`).
    static constexpr uint16_t kPacketPageSize = 0x1000;
    /// Inbound queue backstop (`cs8900a.cpp:39` MAX_FRAME_QUEUE_ENTRIES).
    static constexpr size_t kMaxFrameQueue = 4096;

    Cs8900aDevice();

    /// Full chip reset — the power-on register defaults of
    /// `cs8900a.cpp:286-342`. The MAC address survives, matching real
    /// hardware (the spec says undefined; the part keeps the last value).
    void reset();

    /// $C0nX read / write. `ioAddress` is the low nibble of the slot's
    /// device-select address (0..15).
    uint8_t read(uint8_t ioAddress);
    void    write(uint8_t ioAddress, uint8_t value);

    /// Side-effect-free read for the debug panel — never advances the
    /// PacketPage pointer, never pops a frame, never arms a transmit.
    uint8_t peek(uint8_t ioAddress) const;

    /// Host transport. Not owned; may be null (the chip then behaves like
    /// a card with the cable unplugged).
    void setBackend(NetworkBackend* backend) { backend_ = backend; }
    NetworkBackend* backend() const { return backend_; }

    /// Drain the backend into the inbound queue, applying the chip's
    /// address filter first. Stands in for MAME's push-mode
    /// `recv_start_cb` (`cs8900a.cpp:1483-1512`).
    void pumpBackend();

    /// The individual address (IA) the guest programmed at PacketPage
    /// $0158. Zero until the driver writes one.
    const std::array<uint8_t, 6>& macAddress() const { return mac_; }
    /// Pre-seed the IA so a card has a plausible MAC before any driver
    /// runs (the real card reads one from its EEPROM at power-on).
    void setMacAddress(const std::array<uint8_t, 6>& mac);

    // ── Debug / UI introspection ──────────────────────────────────────
    bool     receiverEnabled()    const { return rxEnabled_; }
    bool     transmitterEnabled() const { return txEnabled_; }
    bool     promiscuous()        const { return recvPromiscuous_; }
    uint16_t packetPagePointer()  const { return packetPagePtr_; }
    size_t   queuedFrames()       const { return frameQueue_.size(); }
    uint64_t framesSent()         const { return framesSent_; }
    uint64_t framesReceived()     const { return framesReceived_; }
    uint64_t framesFiltered()     const { return framesFiltered_; }
    /// The transmit handshake's own state, so a test can assert that a
    /// restored snapshot left it COHERENT: `txState` is the four-step enum
    /// (0 = idle), and `txCount` may never exceed `txLength` — the release
    /// test is an equality, so a count above the length is a frame that can
    /// never be sent. See loadSnapshotState.
    uint8_t  txState()            const { return txState_; }
    uint16_t txCount()            const { return txCount_; }
    uint16_t txLength()           const { return txLength_; }
    uint8_t  rxState()            const { return rxState_; }
    /// Raw PacketPage window for the hex view. `len` is clamped.
    const uint8_t* packetPage() const { return packetPage_.data(); }

    // ── Snapshot / rewind ─────────────────────────────────────────────
    void appendSnapshotState(std::vector<uint8_t>& out) const;
    void loadSnapshotState(const uint8_t* data, std::size_t len);

private:
    // PacketPage accessors — clamped equivalents of MAME's GET_PP_*/
    // SET_PP_* macros (`cs8900a.cpp:99-144`).
    uint8_t  ppRead8 (uint16_t addr) const;
    uint16_t ppRead16(uint16_t addr) const;
    void     ppWrite8 (uint16_t addr, uint8_t  v);
    void     ppWrite16(uint16_t addr, uint16_t v);

    // `cs8900a.cpp:249-284`
    void setTxStatus(bool ready, bool error);
    void setReceiver(bool enabled);
    void setTransmitter(bool enabled);

    // `cs8900a.cpp:410-489` — address filter.
    bool shouldAccept(const uint8_t* buffer, int length,
                      bool* hashed, int* hashIndex, bool* correctMac,
                      bool* broadcast, bool* multicast) const;

    // `cs8900a.cpp:491-618` — pop one accepted frame into PacketPage.
    uint16_t receiveFrame();

    // `cs8900a.cpp:641-765` — the RXTXDATA windows.
    void    writeTxBuffer(uint8_t value, bool oddAddress);
    uint8_t readRxBuffer(bool oddAddress);

    // `cs8900a.cpp:773-1007` — register side effects.
    void sideEffectsWritePp(uint16_t ppAddress, bool oddAddress);
    void sideEffectsReadPp (uint16_t ppAddress, bool oddAddress);

    // `cs8900a.cpp:1013-1247` — register-range decode.
    uint16_t readRegister(uint16_t ppAddress) const;
    void     writeRegister(uint16_t ppAddress, uint16_t value);

    // `cs8900a.cpp:1249-1260`
    void autoIncrementPpPtr();

    std::array<uint8_t, 6>                    mac_{};
    std::array<uint32_t, 2>                   hashMask_{};
    std::array<uint8_t, kIoRegisterCount>     ioRegs_{};
    std::vector<uint8_t>                      packetPage_;
    uint16_t packetPagePtr_ = 0;

    // Receive filter — the decoded copy of CC_RXCTL that
    // `sideEffectsWritePp` maintains (`cs8900a.cpp:802-814`).
    uint16_t recvControl_     = 0;
    bool     recvBroadcast_   = false;
    bool     recvMac_         = false;
    bool     recvMulticast_   = false;
    bool     recvCorrect_     = false;
    bool     recvPromiscuous_ = false;
    bool     recvHashFilter_  = false;

    uint16_t txBuffer_ = 0;
    uint16_t rxBuffer_ = 0;
    uint16_t txCount_  = 0;
    uint16_t rxCount_  = 0;
    uint16_t txLength_ = 0;
    uint16_t rxLength_ = 0;

    uint8_t txState_ = 0;   // cs8900_tx_state_e
    uint8_t rxState_ = 0;   // cs8900_rx_state_e
    bool    txEnabled_ = false;
    bool    rxEnabled_ = false;

    /// Which halves of RxEvent have been read since the last frame pop
    /// (`cs8900a.cpp:399`, `:942-979`).
    int rxEventReadMask_ = 3;

    std::deque<std::vector<uint8_t>> frameQueue_;
    NetworkBackend* backend_ = nullptr;

    uint64_t framesSent_     = 0;
    uint64_t framesReceived_ = 0;
    uint64_t framesFiltered_ = 0;
};

} // namespace pom2

#endif // POM2_CS8900A_DEVICE_H
