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

// SmartPortBusDevice — an intelligent SmartPort drive, answered at the byte
// level of the bus.
//
// The Liron card's firmware and the //c's bank-1 firmware (the same code,
// byte for byte) do not talk to a 3.5" mechanism. They talk to a UniDisk 3.5,
// which carries its own 65C02 and speaks a packet protocol over the disk
// port: PH1 + PH3 select the bus, PH0 is REQ, the IWM's data register carries
// the bytes, and the SENSE line is the device's ACK. POM2 will not emulate
// the drive-side processor; it answers the protocol instead, which is the
// same seam `SmartPortCard` uses one layer up (docs/lle_vs_hle.md).
//
// Everything here was read out of `roms/liron.rom` with POM2's own
// disassembler — the addresses in the comments are that dump's:
//
//   $C800  send      PH1+PH3 up, mode $07, SEL, motor; poll SENSE HIGH 50x
//                    (timeout → $28 "no device"); REQ up; sync `3F CF F3 FC
//                    FF C3`; seven header bytes; odd section; groups; two
//                    checksum bytes; $C8. Then wait for SENSE LOW (the ack)
//                    within ten polls (else $01), and drop REQ ($C949).
//   $C960  receive   PH1+PH3 up; wait SENSE HIGH (reply ready); REQ up; hunt
//                    for $C3 in thirty reads; header into $0051 down to $004B
//                    (dest, src, type, aux, status, odd count, group count,
//                    each AND #$7F); odd bytes; groups via the slot page's
//                    reader at $Cn21; checksum; $C8; then wait SENSE LOW
//                    ($C5E8) and drop REQ ($C5ED).
//   $CE00  INIT      one INIT per device, dest = $81, $82…; the reply's
//                    status byte ($004D) zero means "more devices follow",
//                    anything else ends the scan; the count lands in $07F8,n.
//   $CBE6  calls     command packet contents = $42..$4A: command, device
//                    number on the chain, buffer, block (24-bit) — nine
//                    bytes; a WRITE follows it with a $82 data packet of 512;
//                    the reply's status byte is the ProDOS result ($00 = OK,
//                    and for STATUS the four status bytes land at $45).
//
// The ACK line, which is where the first responder got stuck: it is HIGH
// whenever REQ is low (device ready), and goes LOW when a packet has been
// taken or a reply fully read — the host waits for that edge before it
// releases REQ, and the release is what makes the device ready again. It is
// a REQ/ACK handshake, not a "presence" flag, and modelling it as one meant
// the second transaction of every session found a device that had gone away.
//
// Wire encoding: every byte carries bit 7 (that is "byte ready" to the IWM
// shifter). Contents travel as an odd section — a high-bits byte then up to
// six bytes — and groups of seven behind their own high-bits byte, most
// significant first (bit 6 of the marker is bit 7 of the first byte). The
// checksum is the XOR of the header WIRE bytes and the decoded contents,
// sent as two 4-and-4 bytes; the receiver recovers ((chk2 << 1) | 1) & chk1.

#ifndef POM2_SMARTPORT_BUS_DEVICE_H
#define POM2_SMARTPORT_BUS_DEVICE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pom2 {

/// One drive behind the responder. The bus does not care what backs it.
class SmartPortBusUnit {
public:
    virtual ~SmartPortBusUnit() = default;
    virtual bool     hasMedia()       const = 0;
    virtual uint32_t blockCount()     const = 0;
    virtual bool     writeProtected() const = 0;
    virtual bool     readBlock (uint32_t block, uint8_t out[512]) = 0;
    virtual bool     writeBlock(uint32_t block, const uint8_t in[512]) = 0;
};

class SmartPortBusDevice {
public:
    static constexpr int kMaxUnits = 4;

    /// Units are non-owning and 0-based; the bus numbers them 1-based, in
    /// chain order. `count` is how many the INIT scan will find.
    void setUnit(int index, SmartPortBusUnit* unit);
    void setUnitCount(int count);
    int  unitCount() const { return unitCount_; }

    /// True when at least one unit holds media. The owner uses this to decide
    /// whether a device is on the port at all: an empty chain answers the
    /// presence poll with silence, so the firmware reports $28 and a //c
    /// autostart falls through to its internal drive.
    bool anyMedia() const;
    bool unitHasMedia(int index) const;

    // ── Bus-side events, from whoever owns the IWM registers ─────────────
    /// Machine reset, or the bus reset the firmware issues before an INIT
    /// scan (PH0 + PH2 raised together, $C9E5).
    void reset();
    /// The bus reset alone (PH0 + PH2 raised together, $C9E5): protocol
    /// state AND the host-assigned chain numbers go, the diagnostics
    /// counters stay. Only the host's own reset line may call this.
    void busReset();
    /// Drop whatever exchange is in flight — a half-received frame, a reply
    /// nobody read, a WRITE waiting for its data packet — WITHOUT forgetting
    /// the chain numbers. This is what a media change under a transaction
    /// needs: the frame must not be spliced with the next one, but the host
    /// has not re-run its INIT scan, so its numbering still stands.
    void abortTransaction();
    /// A byte the host wrote to the data register in write mode while the
    /// device was addressed.
    void hostWrote(uint8_t wire);
    /// The host read the data register in read mode. Returns true and the
    /// byte when the device is driving one.
    bool hostReads(uint8_t& out);
    /// The ACK line — bit 7 of the status register while addressed.
    bool sense();
    /// REQ (PH0) changed level.
    void reqChanged(bool high);
    /// True while a transaction is in flight, so the owner keeps routing the
    /// data register here even after the phase lines have moved.
    bool active() const;

    /// How far the last exchange got, for tests and diagnostics.
    struct Progress {
        bool     probeAnswered  = false;  // SENSE was read high while idle
        bool     commandTaken   = false;  // a command packet completed
        bool     packetParsed   = false;  // …and decoded
        bool     replyDelivered = false;  // a reply was read to its last byte
        std::size_t commandBytes = 0;     // wire bytes of the last packet
        std::size_t bodyBytes    = 0;     // decoded contents of the last one
        uint8_t     commandByte  = 0xFF;  // last command number
        int         transactions = 0;     // replies armed
        int         blocksRead   = 0;
        int         blocksWritten = 0;
        int         badChecksums = 0;    // frames refused for their checksum
    };
    const Progress& progress() const { return progress_; }

    // ── Snapshot / rewind ────────────────────────────────────────────────
    /// Everything a transaction in flight needs to resume after a restore:
    /// the frame being received, the reply being read and where in it, the
    /// REQ/ACK state, the pending WRITE, the host-assigned chain numbers.
    /// The rewind ring snapshots every frame and a block transfer spans
    /// several, so "start clean on restore" meant a rewind landing inside
    /// one handed the firmware an empty reply and an I/O error.
    void   appendSnapshotState(std::vector<uint8_t>& out) const;
    /// Returns the bytes consumed, 0 when the blob is absent or malformed
    /// (state is left reset in that case).
    std::size_t loadSnapshotState(const uint8_t* data, std::size_t n);

    static bool trace();

private:
    std::array<SmartPortBusUnit*, kMaxUnits> units_{};
    int unitCount_ = 0;
    // Chain numbers are ASSIGNED by the host: each INIT it sends names the
    // next device down the chain, and the number it carries is whatever the
    // host's own scan reached — 1 on a Liron or a //c, 2 on a //c+ whose
    // internal MIG drive is device 1. A device does not know its number
    // until told, so neither does this one.
    std::array<uint8_t, kMaxUnits> ids_{};   // 0 = not yet assigned
    int assigned_ = 0;

    // Receive side: raw wire bytes since the last packet boundary. The frame
    // is delimited by its own counts — $C8 can also be a data byte ($48 with
    // bit 7) — so completion is decided from the header, not by hunting.
    std::vector<uint8_t> rx_;
    // A WRITE arrives as two packets: the command, then the data. Between
    // them the device acks, the host drops REQ, and sends the second with a
    // fresh probe — which needs ACK high again with no reply to offer.
    bool    pendingWrite_ = false;
    uint8_t pendingUnit_  = 0;
    uint8_t pendingCmd_   = 0;
    uint32_t pendingBlock_ = 0;

    // Reply side.
    std::vector<uint8_t> reply_;
    std::size_t          replyPos_    = 0;
    bool                 replyArmed_  = false;   // built, waiting for REQ low
    bool                 replyExposed_ = false;  // readable
    bool                 sense_ = true;
    bool                 req_   = false;

    Progress progress_;

    void onPacketComplete();
    void buildReply(uint8_t status, const uint8_t* contents, std::size_t n,
                    bool dataPacket);
    void serveCommand(const std::array<uint8_t, 7>& header,
                      const std::vector<uint8_t>& body);
    void serveWriteData(const std::vector<uint8_t>& body);
    SmartPortBusUnit* unitFor(uint8_t chainNumber) const;
};

}  // namespace pom2

#endif  // POM2_SMARTPORT_BUS_DEVICE_H
