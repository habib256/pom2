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

// Cs8900aDevice — port of MAME `src/devices/machine/cs8900a.cpp`
// (GPL-2.0+; Rhett Aultman, from Spiro Trikaliotis' VICE model). See the
// header for the chip overview; every function below carries the MAME
// line range it mirrors.

#include "Cs8900aDevice.h"

#include <cstring>

namespace pom2 {
namespace {

// ── PacketPage register addresses (`cs8900a.cpp:146-201`) ─────────────
// Only the subset the model actually decodes is named here; the reserved
// ranges are handled by address arithmetic in read/writeRegister.
enum PpAddr : uint16_t {
    kPpProductId     = 0x0000,
    kPpIoBase        = 0x0020,
    kPpIntNo         = 0x0022,
    kPpDmaChan       = 0x0024,
    kPpConfCtrl      = 0x0100,
    kPpCcRxCfg       = 0x0102,
    kPpCcRxCtl       = 0x0104,
    kPpCcTxCfg       = 0x0106,
    kPpCcTxCmd       = 0x0108,
    kPpCcBufCfg      = 0x010A,
    kPpCcLineCtl     = 0x0112,
    kPpCcSelfCtl     = 0x0114,
    kPpCcBusCtl      = 0x0116,
    kPpCcTestCtl     = 0x0118,
    kPpSeIsq         = 0x0120,
    kPpSeRxEvent     = 0x0124,
    kPpSeTxEvent     = 0x0128,
    kPpSeBufEvent    = 0x012C,
    kPpSeRxMiss      = 0x0130,
    kPpSeTxCol       = 0x0132,
    kPpSeLineSt      = 0x0134,
    kPpSeSelfSt      = 0x0136,
    kPpSeBusSt       = 0x0138,
    kPpSeTdr         = 0x013C,
    kPpTxCmd         = 0x0144,
    kPpTxLength      = 0x0146,
    kPpLogAddrFilter = 0x0150,
    kPpMacAddr       = 0x0158,
    kPpRxStatus      = 0x0400,
    kPpRxLength      = 0x0402,
    kPpRxFrameLoc    = 0x0404,
    kPpTxFrameLoc    = 0x0A00,
};

// I/O-space register bases (`cs8900a.cpp:61-70`).
enum IoAddr : uint8_t {
    kIoRxTxData   = 0x00,
    kIoRxTxData2  = 0x02,
    kIoTxCmd      = 0x04,
    kIoTxLength   = 0x06,
    kIoIntStQueue = 0x08,
    kIoPpPtr      = 0x0A,
    kIoPpData     = 0x0C,
    kIoPpData2    = 0x0E,
};

// `cs8900a.cpp:210-220`
enum TxState : uint8_t { kTxIdle = 0, kTxGotCmd = 1, kTxGotLen = 2, kTxReadBusSt = 3 };
enum RxState : uint8_t { kRxIdle = 0, kRxGotFrame = 1 };

// `cs8900a.cpp:222-226`
constexpr uint16_t kPpPtrAutoIncrFlag = 0x8000;
constexpr uint16_t kPpPtrFlagMask     = 0xF000;
constexpr uint16_t kPpPtrAddrMask     = 0x0FFF;

// `cs8900a.cpp:203-208` — same bounds NetworkBackend advertises.
constexpr int kMaxTxLength = 1518;
constexpr int kMaxRxLength = 1518;
constexpr int kMinRxLength = 64;

constexpr uint8_t  loByte(uint16_t x) { return static_cast<uint8_t>(x & 0xFF); }
constexpr uint8_t  hiByte(uint16_t x) { return static_cast<uint8_t>((x >> 8) & 0xFF); }
constexpr uint16_t loHiWord(uint8_t lo, uint8_t hi)
{
    return static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8));
}

/// IEEE 802.3 CRC-32, the stand-in for MAME's
/// `util::crc32_creator::simple` used by the multicast hash filter
/// (`cs8900a.cpp:465`). Reflected, poly 0xEDB88320, init/final 0xFFFFFFFF.
uint32_t crc32Ieee(const uint8_t* data, size_t len)
{
    static bool built = false;
    static uint32_t table[256];
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// Snapshot blob framing — see SlotPeripheral::appendSnapshotState for why
// every card must tag its own magic + version.
constexpr uint32_t kSnapMagic   = 0x38393041;  // 'A908'
constexpr uint16_t kSnapVersion = 1;

void putU16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void putU32(std::vector<uint8_t>& out, uint32_t v)
{
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
void putU64(std::vector<uint8_t>& out, uint64_t v)
{
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
uint16_t getU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t getU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t getU64(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

} // namespace

Cs8900aDevice::Cs8900aDevice()
    : packetPage_(kPacketPageSize, 0)
{
    // `cs8900a.cpp:377-400` constructor initial values, then a reset.
    txBuffer_ = kPpTxFrameLoc;
    rxBuffer_ = kPpRxStatus;
    reset();
}

// ── PacketPage accessors (`cs8900a.cpp:99-144`, de-macro'd + clamped) ──

uint8_t Cs8900aDevice::ppRead8(uint16_t addr) const
{
    return (addr < kPacketPageSize) ? packetPage_[addr] : 0;
}

uint16_t Cs8900aDevice::ppRead16(uint16_t addr) const
{
    if (addr + 1 >= kPacketPageSize) return 0;
    return static_cast<uint16_t>(packetPage_[addr] |
                                 (static_cast<uint16_t>(packetPage_[addr + 1]) << 8));
}

void Cs8900aDevice::ppWrite8(uint16_t addr, uint8_t v)
{
    if (addr < kPacketPageSize) packetPage_[addr] = v;
}

void Cs8900aDevice::ppWrite16(uint16_t addr, uint16_t v)
{
    if (addr + 1 >= kPacketPageSize) return;
    packetPage_[addr]     = loByte(v);
    packetPage_[addr + 1] = hiByte(v);
}

// ── Transceiver state (`cs8900a.cpp:249-284`) ─────────────────────────

void Cs8900aDevice::setTxStatus(bool ready, bool error)
{
    const uint16_t oldStatus = ppRead16(kPpSeBusSt);
    // Mask out TxBidErr and Rdy4TxNOW, then re-apply.
    uint16_t newStatus = static_cast<uint16_t>(oldStatus & ~0x180);
    if (ready) newStatus |= 0x100;    // Rdy4TxNOW
    if (error) newStatus |= 0x080;    // TxBidErr
    if (newStatus != oldStatus) ppWrite16(kPpSeBusSt, newStatus);
}

void Cs8900aDevice::setReceiver(bool enabled)
{
    rxEnabled_ = enabled;
    rxState_   = kRxIdle;
    rxEventReadMask_ = 3;
}

void Cs8900aDevice::setTransmitter(bool enabled)
{
    txEnabled_ = enabled;
    txState_   = kTxIdle;
    setTxStatus(false, false);
}

// ── Reset (`cs8900a.cpp:286-342`) ─────────────────────────────────────

void Cs8900aDevice::reset()
{
    frameQueue_.clear();

    ioRegs_.fill(0);
    std::fill(packetPage_.begin(), packetPage_.end(), 0);

    // Per datasheet p.19 unless stated otherwise. ProductID is stored
    // little-endian-reversed exactly as MAME does: 0x0900630E.
    ppWrite16(kPpProductId,     0x630E);
    ppWrite16(kPpProductId + 2, 0x0900);
    ppWrite16(kPpIoBase,   0x0300);
    ppWrite16(kPpIntNo,    0x0004);
    ppWrite16(kPpDmaChan,  0x0003);

    // Each control/status register carries its own register number in the
    // low 6 bits — the chip enforces it, and readRegister asserts on it.
    ppWrite16(kPpCcRxCfg,   0x0003);
    ppWrite16(kPpCcRxCtl,   0x0005);
    ppWrite16(kPpCcTxCfg,   0x0007);
    ppWrite16(kPpCcTxCmd,   0x0009);
    ppWrite16(kPpCcBufCfg,  0x000B);
    ppWrite16(kPpCcLineCtl, 0x0013);
    ppWrite16(kPpCcSelfCtl, 0x0015);
    ppWrite16(kPpCcBusCtl,  0x0017);
    ppWrite16(kPpCcTestCtl, 0x0019);

    ppWrite16(kPpSeIsq,      0x0000);
    ppWrite16(kPpSeRxEvent,  0x0004);
    ppWrite16(kPpSeTxEvent,  0x0008);
    ppWrite16(kPpSeBufEvent, 0x000C);
    ppWrite16(kPpSeRxMiss,   0x0010);
    ppWrite16(kPpSeTxCol,    0x0012);
    ppWrite16(kPpSeLineSt,   0x0014);
    ppWrite16(kPpSeSelfSt,   0x0016);
    ppWrite16(kPpSeBusSt,    0x0018);
    ppWrite16(kPpSeTdr,      0x001C);

    ppWrite16(kPpTxCmd,      0x0009);

    // Self Status (datasheet §4.4.19 p.65): INITD (bit 7) must be set or
    // drivers spin waiting for the chip to come ready.
    ppWrite16(kPpSeSelfSt,   0x0896);

    recvControl_ = ppRead16(kPpCcRxCtl);

    // Spec says the MAC is undefined after reset; real hardware keeps the
    // last programmed address, and so do we.
    for (int i = 0; i < 6; ++i)
        ppWrite8(static_cast<uint16_t>(kPpMacAddr + i), mac_[static_cast<size_t>(i)]);

    setTransmitter(false);
    setReceiver(false);
}

void Cs8900aDevice::setMacAddress(const std::array<uint8_t, 6>& mac)
{
    mac_ = mac;
    for (int i = 0; i < 6; ++i)
        ppWrite8(static_cast<uint16_t>(kPpMacAddr + i), mac_[static_cast<size_t>(i)]);
}

// ── Address filter (`cs8900a.cpp:410-489`) ────────────────────────────

bool Cs8900aDevice::shouldAccept(const uint8_t* buffer, int length,
                                 bool* hashed, int* hashIndex, bool* correctMac,
                                 bool* broadcast, bool* multicast) const
{
    *hashed = false; *hashIndex = 0; *correctMac = false;
    *broadcast = false; *multicast = false;

    // The destination address alone is 6 octets — anything shorter can't
    // be classified. (MAME asserts; we reject.)
    if (length < 6) return false;

    if (std::memcmp(buffer, mac_.data(), 6) == 0) {
        *correctMac = true;
        // Even when "individual address" is off, the address may still
        // pass the hash filter, so don't return early unless it's a match
        // we're configured to take.
        if (recvMac_ || recvPromiscuous_) return true;
    }

    static const uint8_t kBroadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    if (std::memcmp(buffer, kBroadcast, 6) == 0) {
        *broadcast = true;
        // Broadcasts never go through the hash filter.
        return recvBroadcast_ || recvPromiscuous_;
    }

    const int hashReg = static_cast<int>((~crc32Ieee(buffer, 6) >> 26) & 0x3F);
    *hashed = (hashMask_[(hashReg >= 32) ? 1 : 0] & (1u << (hashReg & 0x1F))) != 0;

    if (*hashed) {
        *hashIndex = hashReg;
        if (buffer[0] & 0x80) {
            *multicast = true;
            // A multicast that fits the hash filter reports as multicast,
            // not hashed — the status word formats are mutually exclusive.
            *hashed = false;
            return recvMulticast_ || recvPromiscuous_;
        }
        return recvHashFilter_ || recvPromiscuous_;
    }

    return recvPromiscuous_;
}

// ── Frame reception (`cs8900a.cpp:491-618`) ───────────────────────────

uint16_t Cs8900aDevice::receiveFrame()
{
    uint16_t retVal = 0x0004;   // RxEvent's own register number
    uint8_t buffer[kMaxRxLength];

    bool ready = false;
    while (!ready) {
        ready = true;   // assume we'll find a good frame

        if (frameQueue_.empty()) break;

        const std::vector<uint8_t> frame = std::move(frameQueue_.front());
        frameQueue_.pop_front();
        // Keep the true length for the status word: MAME computes the
        // Extradata bit (0x4000) from the pre-clamp length and discards
        // the octets beyond MAX_RXLEN afterwards. Clamping first made the
        // bit dead code.
        const int rawLen = static_cast<int>(frame.size());
        int len = rawLen;
        if (len > kMaxRxLength) len = kMaxRxLength;
        std::memcpy(buffer, frame.data(), static_cast<size_t>(len));

        bool hashed = false, correctMac = false, broadcast = false, multicast = false;
        int  hashIndex = 0;
        constexpr bool crcError = false;   // the host stack already checked FCS

        if (!shouldAccept(buffer, len, &hashed, &hashIndex,
                          &correctMac, &broadcast, &multicast)) {
            ++framesFiltered_;
            ready = false;      // try the next frame
            continue;
        }

        retVal |= 0x0100;                       // RxOK
        retVal |= multicast ? 0x0200 : 0;
        if (!multicast) retVal |= hashed ? 0x0040 : 0;

        if (hashed) {
            // Second status format: hash index in bits 9..14.
            retVal |= static_cast<uint16_t>((hashIndex & 0x3F) << 9);
        } else {
            retVal |= correctMac ? 0x0400 : 0;
            retVal |= broadcast  ? 0x0800 : 0;
            retVal |= crcError   ? 0x1000 : 0;
            retVal |= (len    < kMinRxLength) ? 0x2000 : 0;
            retVal |= (rawLen > kMaxRxLength) ? 0x4000 : 0;
        }

        ppWrite16(kPpRxLength, static_cast<uint16_t>(len));
        for (int i = 0; i < len; ++i)
            ppWrite8(static_cast<uint16_t>(kPpRxFrameLoc + i), buffer[i]);

        // Reads start at RxStatus, then RxLength, then the payload
        // (datasheet §4.10.9 pp.76-77).
        rxBuffer_ = kPpRxStatus;
        rxLength_ = static_cast<uint16_t>(len);
        rxCount_  = 0;
        rxState_  = kRxGotFrame;
        ++framesReceived_;
    }

    return retVal;
}

void Cs8900aDevice::pumpBackend()
{
    if (!backend_) return;

    uint8_t buf[kMaxRxLength];
    // Bounded per call so a busy link can't stall the CPU thread inside
    // one advanceCycles(); leftovers are picked up on the next tick.
    for (int i = 0; i < 32; ++i) {
        const int len = backend_->receive(buf, sizeof(buf));
        if (len <= 0) break;

        // Pre-filter exactly as MAME's recv_start_cb does
        // (`cs8900a.cpp:1483-1512`) so rejected traffic never queues.
        bool hashed = false, correctMac = false, broadcast = false, multicast = false;
        int  hashIndex = 0;
        if (!shouldAccept(buf, len, &hashed, &hashIndex,
                          &correctMac, &broadcast, &multicast)) {
            ++framesFiltered_;
            continue;
        }

        if (frameQueue_.size() >= kMaxFrameQueue) frameQueue_.pop_front();
        frameQueue_.emplace_back(buf, buf + len);
    }
}

// ── TX / RX data windows (`cs8900a.cpp:641-765`) ──────────────────────

void Cs8900aDevice::writeTxBuffer(uint8_t value, bool oddAddress)
{
    if (txState_ != kTxReadBusSt) {
        // Write without a completed TxCMD/TxLength/BusST handshake.
        // Re-assert the "not ready" status (matters when a < 4 byte
        // transmit was started).
        setTxStatus(false, false);
        return;
    }

    // Bytes always land low-then-high into the transmit staging area.
    uint16_t addr = txBuffer_;
    if (oddAddress) {
        ++addr;
        txBuffer_ = static_cast<uint16_t>(txBuffer_ + 2);
    }
    ++txCount_;
    ppWrite8(addr, value);

    if (txCount_ != txLength_) return;

    // The register path bounds TxLength to 4..1518, but a restored
    // snapshot is untrusted input: re-check against the staging area so
    // no backend is ever handed a length past the PacketPage buffer.
    if (txEnabled_ && backend_ && txLength_ >= kMinEthFrame &&
        txLength_ <= kMaxEthFrame &&
        static_cast<size_t>(kPpTxFrameLoc) + txLength_ <= packetPage_.size()) {
        backend_->transmit(&packetPage_[kPpTxFrameLoc], txLength_);
        ++framesSent_;
    }
    // Whether or not it went out, the transmitter returns to idle.
    txState_ = kTxIdle;
    setTxStatus(false, false);
}

uint8_t Cs8900aDevice::readRxBuffer(bool oddAddress)
{
    // Reading with no frame staged returns zero on real hardware.
    if (rxState_ != kRxGotFrame) return 0;

    // Access pattern per datasheet: RxStatus H then L, RxLength H then L,
    // then the payload L then H per word. The pointer advances on the
    // even (low) half for the two status words and on the *following*
    // even half inside the payload.
    uint16_t addr = oddAddress ? 1 : 0;
    uint8_t  value;

    if (rxCount_ < 4) {
        addr = static_cast<uint16_t>(addr + rxBuffer_);
        value = ppRead8(addr);
        ++rxCount_;
        if (!oddAddress) rxBuffer_ = static_cast<uint16_t>(rxBuffer_ + 2);
    } else {
        // Advance before the read, but not for the first payload word.
        if ((rxCount_ >= 6) && !oddAddress)
            rxBuffer_ = static_cast<uint16_t>(rxBuffer_ + 2);
        addr = static_cast<uint16_t>(addr + rxBuffer_);
        value = ppRead8(addr);
        ++rxCount_;
    }

    // +4 = the RxStatus and RxLength words that precede the payload.
    if (rxCount_ >= rxLength_ + 4) rxState_ = kRxIdle;

    return value;
}

// ── Register side effects (`cs8900a.cpp:773-1007`) ────────────────────

void Cs8900aDevice::sideEffectsWritePp(uint16_t ppAddress, bool oddAddress)
{
    uint16_t content = ppRead16(ppAddress);

    switch (ppAddress) {
    case kPpCcRxCfg:
        // Skip_1: drop the partial TX frame and re-arm the transmitter.
        if (content & 0x40) {
            if (txState_ != kTxIdle) txState_ = kTxIdle;
            setTransmitter(txEnabled_);
            // "Act once" bit — clears itself.
            content = static_cast<uint16_t>(content & ~0x40);
            ppWrite16(ppAddress, content);
        }
        break;

    case kPpCcRxCtl:
        if (recvControl_ != content) {
            recvBroadcast_   = (content & 0x0800) != 0;
            recvMac_         = (content & 0x0400) != 0;
            recvMulticast_   = (content & 0x0200) != 0;
            recvCorrect_     = (content & 0x0100) != 0;
            recvPromiscuous_ = (content & 0x0080) != 0;
            recvHashFilter_  = (content & 0x0040) != 0;
            recvControl_     = content;
        }
        break;

    case kPpCcLineCtl: {
        const bool enableTx = (content & 0x0080) == 0x0080;
        const bool enableRx = (content & 0x0040) == 0x0040;
        if (enableTx != txEnabled_ || enableRx != rxEnabled_) {
            setTransmitter(enableTx);
            setReceiver(enableRx);
        }
        break;
    }

    case kPpCcSelfCtl:
        // RESET bit — full chip reset.
        if ((content & 0x40) == 0x40) reset();
        break;

    case kPpTxCmd:
        if (oddAddress) {
            const uint16_t txCommand = ppRead16(kPpTxCmd);
            // TxCmd status mirrors the last transmit command issued.
            ppWrite16(kPpCcTxCmd, txCommand);
            txState_ = kTxGotCmd;
            setTxStatus(false, false);
        }
        break;

    case kPpTxLength:
        if (oddAddress && txState_ == kTxGotCmd) {
            const uint16_t txLength  = ppRead16(kPpTxLength);
            const uint16_t txCommand = ppRead16(kPpCcTxCmd);

            if (txLength < 4) {
                // Too short: report space available but commit nothing.
                txState_ = kTxIdle;
                setTxStatus(true, false);
            } else if (txLength > kMaxTxLength ||
                       (txLength > kMaxTxLength - 4 && !(txCommand & 0x1000))) {
                // Too long (the -4 slack is the CRC the chip appends
                // unless the driver asked to supply its own).
                txState_ = kTxIdle;
                setTxStatus(false, true);
            } else {
                txBuffer_ = kPpTxFrameLoc;
                txCount_  = 0;
                txLength_ = txLength;
                txState_  = kTxGotLen;
                setTxStatus(true, false);
            }
        }
        break;

    case kPpLogAddrFilter:
    case kPpLogAddrFilter + 2:
    case kPpLogAddrFilter + 4:
    case kPpLogAddrFilter + 6: {
        const unsigned pos = 8u * static_cast<unsigned>(
            ppAddress - kPpLogAddrFilter + (oddAddress ? 1 : 0));
        uint32_t& word = hashMask_[(pos < 32) ? 0 : 1];
        const unsigned shift = pos & 31u;
        word &= ~(0xFFu << shift);
        word |= static_cast<uint32_t>(
            ppRead8(static_cast<uint16_t>(ppAddress + (oddAddress ? 1 : 0)))) << shift;
        break;
    }

    case kPpMacAddr:
    case kPpMacAddr + 2:
    case kPpMacAddr + 4: {
        const size_t idx = static_cast<size_t>(
            ppAddress - kPpMacAddr + (oddAddress ? 1 : 0));
        if (idx < mac_.size())
            mac_[idx] = ppRead8(static_cast<uint16_t>(ppAddress + (oddAddress ? 1 : 0)));
        break;
    }

    default:
        break;
    }
}

void Cs8900aDevice::sideEffectsReadPp(uint16_t ppAddress, bool oddAddress)
{
    switch (ppAddress) {
    case kPpSeRxEvent: {
        // Reading RxEvent before the staged frame is fully drained is an
        // "implied skip" — the pending frame is lost. MAME treats EVERY
        // completed status read as a new one, including re-reading the
        // same half ("L, L, L or H, H, H", cs8900a.cpp:942-979), so a
        // repeated same-half read pops the next frame too. Do NOT "fix"
        // this to pop only on a new half — that would break MAME parity.
        const int accessMask = oddAddress ? 1 : 2;

        if ((accessMask & rxEventReadMask_) != 0) {
            if (rxEnabled_) {
                const uint16_t retVal = receiveFrame();
                // RxStatus buffers the value; RxEvent re-evaluates on
                // every read. Both get the same word here.
                ppWrite16(kPpRxStatus,   retVal);
                ppWrite16(kPpSeRxEvent,  retVal);
            }
            rxEventReadMask_ = accessMask;
        } else {
            rxEventReadMask_ |= accessMask;
        }
        break;
    }

    case kPpSeBusSt:
        if (oddAddress && txState_ == kTxGotLen) {
            // Observing Rdy4TxNow is the third leg of the TX handshake.
            if ((ppRead16(kPpSeBusSt) & 0x100) == 0x100) txState_ = kTxReadBusSt;
        }
        break;

    default:
        break;
    }
}

// ── PacketPage register decode (`cs8900a.cpp:1013-1247`) ──────────────

uint16_t Cs8900aDevice::readRegister(uint16_t ppAddress) const
{
    const uint16_t value = ppRead16(ppAddress);

    if (ppAddress < 0x100) {
        // Reserved bus-interface range reads 0x0300 on real hardware.
        if (ppAddress >= 0x0004 && ppAddress < 0x0020) return 0x0300;
    } else if (ppAddress < 0x120) {
        uint16_t regNum = static_cast<uint16_t>(ppAddress - 0x100);
        regNum = static_cast<uint16_t>((regNum & ~1) + 1);
        if (regNum == 0x01 || regNum == 0x11 || regNum > 0x19) return 0x0300;
    } else if (ppAddress < 0x140) {
        uint16_t regNum = static_cast<uint16_t>((ppAddress - 0x120) & ~1);
        if (regNum == 0x02 || regNum == 0x06 || regNum == 0x0A ||
            regNum == 0x0E || regNum == 0x1A || regNum == 0x1E) return 0x0300;
    } else if (ppAddress < 0x150) {
        if (ppAddress != kPpTxCmd && ppAddress != kPpTxLength) return 0x0300;
    } else if (ppAddress < 0x160) {
        if (ppAddress >= 0x15E) return 0x0300;
    } else if (ppAddress < 0x400) {
        return 0x0300;
    } else {
        // RX ($0400-$09FF) and TX ($0A00-$0FFF) frame buffers are not
        // register-readable — only the RXTXDATA window reaches them.
        return 0x0000;
    }

    return value;
}

void Cs8900aDevice::writeRegister(uint16_t ppAddress, uint16_t value)
{
    if (ppAddress < 0x100) {
        // Read-only / reserved bus-interface registers.
        const bool ignore = (ppAddress < 0x20) ||
                            (ppAddress >= 0x26 && ppAddress < 0x2C) ||
                            (ppAddress == 0x38) ||
                            (ppAddress >= 0x44);
        if (ignore) return;
    } else if (ppAddress < 0x120) {
        uint16_t regNum = static_cast<uint16_t>(ppAddress - 0x100);
        regNum = static_cast<uint16_t>((regNum & ~1) + 1);
        // The chip forces its own register number into the low 6 bits.
        if ((value & 0x3F) != regNum) {
            value = static_cast<uint16_t>((value & ~0x3F) | regNum);
        }
        if (regNum == 0x01 || regNum == 0x11 || regNum > 0x19) return;
    } else if (ppAddress < 0x140) {
        return;   // status registers are read-only
    } else if (ppAddress < 0x150) {
        if (ppAddress == kPpTxCmd) {
            if ((value & 0x3F) != 0x09) value = static_cast<uint16_t>((value & ~0x3F) | 0x09);
            value &= 0x33FF;   // reserved bits
        } else if (ppAddress == kPpTxLength) {
            value &= 0x0FFF;   // hardware always masks
        } else if (ppAddress < kPpTxCmd || ppAddress > 0x147) {
            return;
        }
    } else if (ppAddress < 0x160) {
        if (ppAddress >= 0x15E) return;
    } else {
        // Everything from 0x160 up — reserved, RX buffer, TX buffer.
        return;
    }

    ppWrite16(ppAddress, value);
}

void Cs8900aDevice::autoIncrementPpPtr()
{
    if ((packetPagePtr_ & kPpPtrAutoIncrFlag) != kPpPtrAutoIncrFlag) return;
    // Real hardware increments by one, not two, even though registers are
    // word-aligned — an odd pointer is legal.
    const uint16_t ptr   = static_cast<uint16_t>((packetPagePtr_ & kPpPtrAddrMask) + 1);
    const uint16_t flags = static_cast<uint16_t>(packetPagePtr_ & kPpPtrFlagMask);
    packetPagePtr_ = static_cast<uint16_t>((ptr & kPpPtrAddrMask) | flags);
}

// ── I/O space (`cs8900a.cpp:1262-1481`) ───────────────────────────────

uint8_t Cs8900aDevice::read(uint8_t ioAddress)
{
    ioAddress = static_cast<uint8_t>(ioAddress & 0x0F);
    const uint8_t regBase = static_cast<uint8_t>(ioAddress & ~1);

    // The RX window reads straight out of the staged frame.
    if (regBase == kIoRxTxData || regBase == kIoRxTxData2) {
        const uint8_t v = readRxBuffer((ioAddress & 1) != 0);
        // Keep peek() honest: the data window bypasses the register-bank
        // cache below, and an un-updated cache made the debug panel show
        // a permanent $00 at $C0n0-$C0n3.
        ioRegs_[ioAddress] = v;
        return v;
    }

    uint16_t wordValue;

    if (regBase == kIoPpPtr) {
        wordValue = packetPagePtr_;
    } else {
        uint16_t ppAddress = kPpProductId;
        switch (regBase) {
        case kIoPpData:
        case kIoPpData2:
            // PP_DATA2 is a plain alias on real hardware — both windows
            // show whatever the pointer points at.
            ppAddress = static_cast<uint16_t>(packetPagePtr_ & kPpPtrAddrMask & ~1);
            autoIncrementPpPtr();
            break;
        case kIoIntStQueue: ppAddress = kPpSeIsq;    break;
        case kIoTxCmd:      ppAddress = kPpTxCmd;    break;
        case kIoTxLength:   ppAddress = kPpTxLength; break;
        default: break;
        }

        sideEffectsReadPp(ppAddress, (ioAddress & 1) != 0);
        wordValue = readRegister(ppAddress);
    }

    const uint8_t lo = loByte(wordValue);
    const uint8_t hi = hiByte(wordValue);

    // The visible register bank always caches the whole word.
    ioRegs_[regBase]     = lo;
    ioRegs_[regBase + 1] = hi;

    return (ioAddress & 1) ? hi : lo;
}

void Cs8900aDevice::write(uint8_t ioAddress, uint8_t value)
{
    ioAddress = static_cast<uint8_t>(ioAddress & 0x0F);
    const uint8_t regBase = static_cast<uint8_t>(ioAddress & ~1);

    // The TX window writes straight into the transmit staging area.
    if (regBase == kIoRxTxData || regBase == kIoRxTxData2) {
        ioRegs_[ioAddress] = value;    // for peek() only — see read()
        writeTxBuffer(value, (ioAddress & 1) != 0);
        return;
    }

    // Merge the byte into the cached word.
    uint16_t wordValue = (ioAddress & 1)
        ? loHiWord(ioRegs_[regBase], value)
        : loHiWord(value, ioRegs_[regBase + 1]);

    if (regBase == kIoPpPtr) {
        // The full pointer is kept — flag bits (0xF000) and address
        // (0x0FFF). Bits 0x3000 read back set on real hardware. Odd
        // values are legal; only register access is word-aligned.
        wordValue |= 0x3000;
        packetPagePtr_ = wordValue;
    } else {
        uint16_t ppAddress = kPpProductId;
        switch (regBase) {
        case kIoPpData:
        case kIoPpData2:
            ppAddress = static_cast<uint16_t>(packetPagePtr_ & (kPacketPageSize - 1) & ~1);
            autoIncrementPpPtr();
            break;
        case kIoTxCmd:      ppAddress = kPpTxCmd;    break;
        case kIoTxLength:   ppAddress = kPpTxLength; break;
        case kIoIntStQueue: ppAddress = kPpSeIsq;    break;
        default: break;
        }

        writeRegister(ppAddress, wordValue);
        sideEffectsWritePp(ppAddress, (ioAddress & 1) != 0);
        // The write or its side effect may have altered the register.
        wordValue = ppRead16(ppAddress);
    }

    ioRegs_[regBase]     = loByte(wordValue);
    ioRegs_[regBase + 1] = hiByte(wordValue);
}

uint8_t Cs8900aDevice::peek(uint8_t ioAddress) const
{
    ioAddress = static_cast<uint8_t>(ioAddress & 0x0F);
    const uint8_t regBase = static_cast<uint8_t>(ioAddress & ~1);

    // No frame pop, no pointer advance, no TX arming — just what the
    // cached bank or the pointed-at register currently holds.
    if (regBase == kIoRxTxData || regBase == kIoRxTxData2)
        return ioRegs_[ioAddress];

    uint16_t wordValue;
    if (regBase == kIoPpPtr) {
        wordValue = packetPagePtr_;
    } else {
        uint16_t ppAddress = kPpProductId;
        switch (regBase) {
        case kIoPpData:
        case kIoPpData2:
            ppAddress = static_cast<uint16_t>(packetPagePtr_ & kPpPtrAddrMask & ~1);
            break;
        case kIoIntStQueue: ppAddress = kPpSeIsq;    break;
        case kIoTxCmd:      ppAddress = kPpTxCmd;    break;
        case kIoTxLength:   ppAddress = kPpTxLength; break;
        default: break;
        }
        wordValue = readRegister(ppAddress);
    }
    return (ioAddress & 1) ? hiByte(wordValue) : loByte(wordValue);
}

// ── Snapshot / rewind ─────────────────────────────────────────────────

void Cs8900aDevice::appendSnapshotState(std::vector<uint8_t>& out) const
{
    putU32(out, kSnapMagic);
    putU16(out, kSnapVersion);

    out.insert(out.end(), mac_.begin(), mac_.end());
    putU32(out, hashMask_[0]);
    putU32(out, hashMask_[1]);
    out.insert(out.end(), ioRegs_.begin(), ioRegs_.end());
    // 4 KB, but the rewind ring XOR-deltas it (RewindBuffer.cpp:20-88),
    // so a mostly-idle NIC costs a handful of bytes per frame.
    out.insert(out.end(), packetPage_.begin(), packetPage_.end());

    putU16(out, packetPagePtr_);
    putU16(out, recvControl_);
    const uint8_t flags = static_cast<uint8_t>(
        (recvBroadcast_   ? 0x01 : 0) | (recvMac_        ? 0x02 : 0) |
        (recvMulticast_   ? 0x04 : 0) | (recvCorrect_    ? 0x08 : 0) |
        (recvPromiscuous_ ? 0x10 : 0) | (recvHashFilter_ ? 0x20 : 0) |
        (txEnabled_       ? 0x40 : 0) | (rxEnabled_      ? 0x80 : 0));
    out.push_back(flags);

    putU16(out, txBuffer_);
    putU16(out, rxBuffer_);
    putU16(out, txCount_);
    putU16(out, rxCount_);
    putU16(out, txLength_);
    putU16(out, rxLength_);
    out.push_back(txState_);
    out.push_back(rxState_);
    putU16(out, static_cast<uint16_t>(rxEventReadMask_));

    // Counters ride along so the status panel doesn't jump on rewind.
    putU64(out, framesSent_);
    putU64(out, framesReceived_);
    putU64(out, framesFiltered_);
    // The inbound frame queue is deliberately NOT saved: it mirrors host
    // network state that has moved on by the time a rewind replays, and
    // a stale queue would re-deliver packets the guest already consumed.
}

void Cs8900aDevice::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    // Fixed part: magic(4) + version(2) + mac(6) + hash(8) + ioregs(16)
    //           + packetpage(4096) + 2+2+1 + 6*2 + 1+1+2 + 3*8
    constexpr std::size_t kExpected =
        4 + 2 + 6 + 8 + kIoRegisterCount + kPacketPageSize +
        2 + 2 + 1 + 12 + 1 + 1 + 2 + 24;

    if (len < kExpected) return;
    if (getU32(data) != kSnapMagic) return;
    if (getU16(data + 4) != kSnapVersion) return;

    std::size_t p = 6;
    std::memcpy(mac_.data(), data + p, 6);                    p += 6;
    hashMask_[0] = getU32(data + p);                          p += 4;
    hashMask_[1] = getU32(data + p);                          p += 4;
    std::memcpy(ioRegs_.data(), data + p, kIoRegisterCount);  p += kIoRegisterCount;
    std::memcpy(packetPage_.data(), data + p, kPacketPageSize);
    p += kPacketPageSize;

    packetPagePtr_ = getU16(data + p); p += 2;
    recvControl_   = getU16(data + p); p += 2;
    const uint8_t flags = data[p++];
    recvBroadcast_   = (flags & 0x01) != 0;
    recvMac_         = (flags & 0x02) != 0;
    recvMulticast_   = (flags & 0x04) != 0;
    recvCorrect_     = (flags & 0x08) != 0;
    recvPromiscuous_ = (flags & 0x10) != 0;
    recvHashFilter_  = (flags & 0x20) != 0;
    txEnabled_       = (flags & 0x40) != 0;
    rxEnabled_       = (flags & 0x80) != 0;

    txBuffer_ = getU16(data + p); p += 2;
    rxBuffer_ = getU16(data + p); p += 2;
    txCount_  = getU16(data + p); p += 2;
    rxCount_  = getU16(data + p); p += 2;
    txLength_ = getU16(data + p); p += 2;
    rxLength_ = getU16(data + p); p += 2;
    txState_  = data[p++];
    rxState_  = data[p++];
    rxEventReadMask_ = static_cast<int>(getU16(data + p)); p += 2;

    framesSent_     = getU64(data + p); p += 8;
    framesReceived_ = getU64(data + p); p += 8;
    framesFiltered_ = getU64(data + p);

    // A snapshot is a FILE, and a corrupt or hand-edited one must not be able
    // to wedge the NIC. The transmit handshake is the state machine that can:
    // `transmitByte` releases the frame when `txCount_ == txLength_` exactly,
    // so a restored txCount_ ABOVE txLength_ never matches — every further
    // byte the guest pushes increments past it, the frame is never sent, and
    // the card looks alive while transmitting nothing, for the rest of the
    // session. The same for a txState_ outside the four-step enum: it is
    // compared for equality at each step, so a fifth value simply never
    // advances. Restore both to a coherent point rather than trusting them.
    if (txState_ > kTxReadBusSt) { txState_ = kTxIdle; txCount_ = 0; }
    if (txLength_ > kMaxEthFrame) { txLength_ = 0; txState_ = kTxIdle; }
    if (txCount_ > txLength_)     { txState_ = kTxIdle; txCount_ = 0; }
    // The receive side cannot wedge the same way (its end test is `>=`), but a
    // state byte outside the enum still leaves the model reading a frame that
    // is not there.
    if (rxState_ > kRxGotFrame) { rxState_ = kRxIdle; rxCount_ = 0; }

    frameQueue_.clear();
}

} // namespace pom2
