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

#include "SmartPortBusDevice.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pom2 {

namespace {

constexpr std::size_t kBlockBytes = 512;

// SmartPort call numbers (Apple II SmartPort spec), and which of them the
// firmware follows with a data packet — its own table at $CDE3, bit 7.
constexpr uint8_t kCmdStatus  = 0x00;
constexpr uint8_t kCmdRead    = 0x01;
constexpr uint8_t kCmdWrite   = 0x02;
constexpr uint8_t kCmdFormat  = 0x03;
constexpr uint8_t kCmdControl = 0x04;
constexpr uint8_t kCmdInit    = 0x05;
constexpr uint8_t kCmdWriteCh = 0x09;

bool dataFollows(uint8_t cmd)
{
    return cmd == kCmdWrite || cmd == kCmdControl || cmd == kCmdWriteCh;
}

// SmartPort result codes.
constexpr uint8_t kErrBadCmd    = 0x01;
constexpr uint8_t kErrBadUnit   = 0x11;
constexpr uint8_t kErrIo        = 0x27;
constexpr uint8_t kErrWriteProt = 0x2B;
constexpr uint8_t kErrBadBlock  = 0x2D;
constexpr uint8_t kErrOffline   = 0x2F;

// Packet types, as they appear in the header before the wire's bit 7.
constexpr uint8_t kTypeCommand = 0x00;
constexpr uint8_t kTypeStatus  = 0x01;
constexpr uint8_t kTypeData    = 0x02;

constexpr uint8_t kPacketBegin = 0xC3;
constexpr uint8_t kPacketEnd   = 0xC8;

/// Decode one packet's contents from its wire bytes (header already known).
/// `p` points just past the seven header bytes.
bool decodeContents(const uint8_t* p, std::size_t avail, uint8_t oddCount,
                    uint8_t groupCount, std::vector<uint8_t>& body,
                    uint8_t& checksum)
{
    body.clear();
    std::size_t i = 0;
    if (oddCount) {
        if (i >= avail) return false;
        const uint8_t high = p[i++];
        for (int k = 0; k < oddCount; ++k) {
            if (i >= avail) return false;
            const uint8_t bit = static_cast<uint8_t>((high << (k + 1)) & 0x80);
            const uint8_t b   = static_cast<uint8_t>((p[i++] & 0x7F) | bit);
            body.push_back(b);
            checksum ^= b;
        }
    }
    for (int g = 0; g < groupCount; ++g) {
        if (i >= avail) return false;
        const uint8_t high = p[i++];
        for (int k = 0; k < 7; ++k) {
            if (i >= avail) return false;
            const uint8_t bit = static_cast<uint8_t>((high << (k + 1)) & 0x80);
            const uint8_t b   = static_cast<uint8_t>((p[i++] & 0x7F) | bit);
            body.push_back(b);
            checksum ^= b;
        }
    }
    return true;
}

/// Append `n` (1..7) bytes as one section: the high-bits marker, then the
/// bytes with bit 7 forced. The firmware's own tables at $CA27/$CA37/$CA47/
/// $CA57 hold nothing but $80/$00 masks keyed on the marker's bits — the
/// same statement in silicon.
void appendSection(std::vector<uint8_t>& out, const uint8_t* data, int n)
{
    uint8_t high = 0x80;
    for (int k = 0; k < n; ++k)
        if (data[k] & 0x80) high |= static_cast<uint8_t>(0x40 >> k);
    out.push_back(high);
    for (int k = 0; k < n; ++k)
        out.push_back(static_cast<uint8_t>(data[k] | 0x80));
}

}  // namespace

bool SmartPortBusDevice::trace()
{
    static const bool on = std::getenv("POM2_TRACE_SMARTPORT_BUS") != nullptr;
    return on;
}

void SmartPortBusDevice::setUnit(int index, SmartPortBusUnit* unit)
{
    if (index < 0 || index >= kMaxUnits) return;
    units_[static_cast<std::size_t>(index)] = unit;
}

void SmartPortBusDevice::setUnitCount(int count)
{
    if (count < 0) count = 0;
    if (count > kMaxUnits) count = kMaxUnits;
    unitCount_ = count;
}

bool SmartPortBusDevice::anyMedia() const
{
    for (int i = 0; i < unitCount_; ++i) {
        const SmartPortBusUnit* u = units_[static_cast<std::size_t>(i)];
        if (u && u->hasMedia()) return true;
    }
    return false;
}

bool SmartPortBusDevice::unitHasMedia(int index) const
{
    if (index < 0 || index >= unitCount_) return false;
    const SmartPortBusUnit* u = units_[static_cast<std::size_t>(index)];
    return u && u->hasMedia();
}

SmartPortBusUnit* SmartPortBusDevice::unitFor(uint8_t chainNumber) const
{
    for (int i = 0; i < unitCount_; ++i)
        if (ids_[static_cast<std::size_t>(i)] == chainNumber)
            return units_[static_cast<std::size_t>(i)];
    // No INIT seen (a host that skips the scan): count from 1, as a chain
    // freshly powered next to a Liron would be numbered.
    if (assigned_ == 0 && chainNumber >= 1 && chainNumber <= unitCount_)
        return units_[static_cast<std::size_t>(chainNumber - 1)];
    return nullptr;
}

void SmartPortBusDevice::reset()
{
    busReset();
    progress_ = Progress{};
}

void SmartPortBusDevice::busReset()
{
    ids_.fill(0);
    assigned_ = 0;
    rx_.clear();
    reply_.clear();
    replyPos_     = 0;
    replyArmed_   = false;
    replyExposed_ = false;
    pendingWrite_ = false;
    sense_        = true;
    req_          = false;
}

bool SmartPortBusDevice::active() const
{
    return !rx_.empty() || replyArmed_ || replyExposed_ || pendingWrite_ ||
           !sense_;
}

bool SmartPortBusDevice::sense()
{
    if (sense_ && !active()) progress_.probeAnswered = true;
    return sense_;
}

void SmartPortBusDevice::reqChanged(bool high)
{
    if (high == req_) return;
    req_ = high;
    if (high) return;
    // REQ released. Whatever the host was doing is over: a packet it sent
    // has been acknowledged, or a reply it read is consumed. Either way the
    // device is ready again, and if it has an answer waiting, this is when
    // it goes on the wire — the receive routine's first act is to wait for
    // SENSE HIGH ($C97D), then raise REQ and start reading.
    if (replyExposed_ && replyPos_ >= reply_.size()) {
        reply_.clear();
        replyPos_     = 0;
        replyExposed_ = false;
    }
    if (replyArmed_) {
        replyArmed_   = false;
        replyExposed_ = true;
        replyPos_     = 0;
    }
    sense_ = true;
    if (trace()) std::fprintf(stderr, "[SPBUS] REQ low -> ready%s\n",
                              replyExposed_ ? ", reply on the wire" : "");
}

void SmartPortBusDevice::hostWrote(uint8_t wire)
{
    // Between the end marker and REQ dropping the sender writes one more
    // byte ($C933 stores the drained handshake). Nothing after a completed
    // frame belongs to it — and that includes the reply just armed for it,
    // which an earlier draft threw away here.
    if (!sense_ && rx_.empty()) return;
    // A reply the host abandoned part-way dies with the next packet; the
    // firmware's retry paths do exactly that.
    if (replyExposed_) {
        reply_.clear();
        replyPos_ = 0;
        replyArmed_ = replyExposed_ = false;
    }

    rx_.push_back(wire);
    if (trace()) std::fprintf(stderr, "[SPBUS] host -> %02X\n", wire);

    // Frame complete? Find the begin marker, then let the header say how
    // long the rest is.
    std::size_t start = 0;
    while (start < rx_.size() && rx_[start] != kPacketBegin) ++start;
    if (start >= rx_.size()) {
        // Only sync bytes so far. Keep the buffer from growing on a host
        // that streams $FF for ever.
        if (rx_.size() > 16) rx_.erase(rx_.begin(), rx_.end() - 8);
        return;
    }
    const std::size_t have = rx_.size() - start - 1;
    if (have < 7) return;
    const uint8_t oddCount   = static_cast<uint8_t>(rx_[start + 6] & 0x7F);
    const uint8_t groupCount = static_cast<uint8_t>(rx_[start + 7] & 0x7F);
    const std::size_t expected =
        7 + (oddCount ? 1 + oddCount : 0) + static_cast<std::size_t>(groupCount) * 8
        + 2 + 1;
    if (have < expected) return;
    if (start) rx_.erase(rx_.begin(), rx_.begin() + static_cast<long>(start));
    onPacketComplete();
}

void SmartPortBusDevice::onPacketComplete()
{
    // rx_[0] == $C3, then the header.
    std::array<uint8_t, 7> header{};
    uint8_t checksum = 0;
    for (std::size_t h = 0; h < 7; ++h) {
        header[h] = static_cast<uint8_t>(rx_[1 + h] & 0x7F);
        checksum ^= rx_[1 + h];                  // header folds in as WIRE bytes
    }
    std::vector<uint8_t> body;
    const std::size_t contentsAt = 8;
    const bool ok = decodeContents(rx_.data() + contentsAt,
                                   rx_.size() - contentsAt, header[5],
                                   header[6], body, checksum);
    const std::size_t tail = rx_.size() - 3;     // chk1 chk2 $C8
    const uint8_t chk1 = rx_[tail], chk2 = rx_[tail + 1];
    const uint8_t got  = static_cast<uint8_t>(((chk2 << 1) | 1) & chk1);
    const bool endOk   = rx_[tail + 2] == kPacketEnd;

    progress_.commandTaken = true;
    progress_.commandBytes = rx_.size();
    progress_.packetParsed = progress_.packetParsed || (ok && endOk);
    progress_.bodyBytes    = body.size();

    if (trace())
        std::fprintf(stderr,
                     "[SPBUS] packet dest=%02X src=%02X type=%02X aux=%02X "
                     "stat=%02X odd=%u grp=%u body=%zu chk=%s end=%s\n",
                     header[0], header[1], header[2], header[3], header[4],
                     header[5], header[6], body.size(),
                     (got == checksum) ? "ok" : "BAD", endOk ? "ok" : "BAD");

    rx_.clear();
    // The ack: the host is polling for SENSE LOW at $C943 before it drops
    // REQ. A frame that failed to decode — or whose checksum does not match
    // — gets no reply; the firmware's retry loop re-sends, which is the
    // right recovery for a garbled packet. Serving it instead would let a
    // frame spliced from two transactions write a block of garbage.
    sense_ = false;
    if (got != checksum) ++progress_.badChecksums;
    if (!ok || !endOk || got != checksum) return;

    const uint8_t type = header[2];
    if (type == kTypeCommand) {
        serveCommand(header, body);
    } else if (type == kTypeData && pendingWrite_) {
        serveWriteData(body);
    } else if (trace()) {
        std::fprintf(stderr, "[SPBUS] unexpected packet type %02X ignored\n",
                     type);
    }
}

void SmartPortBusDevice::serveCommand(const std::array<uint8_t, 7>& header,
                                      const std::vector<uint8_t>& body)
{
    // A command packet ends whatever came before it. The drive is holding
    // one transaction, not two: the packet in its hands wins, and any data
    // packet it was still expecting is gone. Only the data packet and a bus
    // reset used to clear `pendingWrite_`, so a WRITE whose data packet never
    // arrived — the firmware abandons one on a bad checksum and retries the
    // command without resetting the bus — latched `active()` for ever, and
    // the //c external port then claimed every $C0E0-$C0EF access with the
    // Disk II dead behind it until the next machine reset. The stale unit,
    // command and block go with it, so a later stray data packet cannot land
    // on the block a long-abandoned WRITE named.
    pendingWrite_ = false;
    pendingCmd_   = 0;
    pendingUnit_  = 0;
    pendingBlock_ = 0;

    if (body.empty()) return;
    // Contents are $42..$4A as the firmware sends them: command, parameter
    // count (its table at $CDE3 — not the unit), buffer, 24-bit block. The
    // unit is the packet's DESTINATION: $CD02 swaps the chain number into
    // $5A, and $C837 puts $5A on the wire as dest.
    const uint8_t cmd  = body[0];
    const uint8_t dev  = header[0];
    const uint32_t block = body.size() >= 7
        ? (static_cast<uint32_t>(body[4]) |
           (static_cast<uint32_t>(body[5]) << 8) |
           (static_cast<uint32_t>(body[6]) << 16))
        : 0;
    progress_.commandByte = cmd;
    if (trace())
        std::fprintf(stderr, "[SPBUS] cmd=$%02X unit=%u params=%u block=%u\n",
                     cmd, dev, body.size() > 1 ? body[1] : 0, block);

    // A command that carries data is answered after the data packet; the
    // firmware sends that next without reading anything in between.
    if (dataFollows(cmd)) {
        pendingWrite_ = true;
        pendingUnit_  = dev;
        pendingBlock_ = block;
        // Remember the command for the reply's sake: CONTROL and the
        // character WRITE are accepted and discarded.
        progress_.commandByte = cmd;
        pendingCmd_ = cmd;
        return;
    }

    switch (cmd) {
    case kCmdInit: {
        // The scan names the devices in chain order: each INIT's destination
        // is the number the next unit takes. Status zero says "more behind
        // me"; the last unit answers non-zero and the scan stops ($CE30-
        // $CE34 on a Liron). The count the host keeps is what it later
        // checks every unit number against ($CCD1). The number itself is
        // the host's: a //c+ starts at 2, its MIG drive being device 1.
        if (assigned_ < unitCount_)
            ids_[static_cast<std::size_t>(assigned_++)] = header[0];
        const uint8_t status = (assigned_ >= unitCount_) ? 0xFF : 0x00;
        buildReply(status, nullptr, 0, false);
        break;
    }
    case kCmdStatus: {
        SmartPortBusUnit* u = unitFor(dev);
        if (!u) { buildReply(kErrBadUnit, nullptr, 0, false); break; }
        const uint8_t code = body.size() > 4 ? body[4] : 0;
        const bool media = u->hasMedia();
        const uint32_t blocks = media ? u->blockCount() : 0;
        // General status byte: block device, read allowed, format allowed;
        // write allowed unless protected; online iff media (bit 4 — the
        // firmware's own $CD93 test, which returns $2F without it).
        uint8_t gen = 0x80 | 0x20 | 0x08;
        if (media) gen |= 0x10;
        if (media && !u->writeProtected()) gen |= 0x40;
        if (media && u->writeProtected())  gen |= 0x04;
        uint8_t st[25] = {};
        st[0] = gen;
        st[1] = static_cast<uint8_t>(blocks & 0xFF);
        st[2] = static_cast<uint8_t>((blocks >> 8) & 0xFF);
        st[3] = static_cast<uint8_t>((blocks >> 16) & 0xFF);
        if (code == 0x03) {
            // Device Information Block: name (Pascal string in 16 bytes),
            // type, subtype, firmware version. An 800K unit is a UniDisk
            // 3.5 (type $01, subtype $C0 — extended calls, disk-switched
            // errors); anything else on this bus is a hard disk (type $02,
            // subtype $80 — extended calls, not removable).
            const bool floppy = (blocks == 1600) || !media;
            static const char kFloppy[16] = "POM2 UNIDISK3.5";
            static const char kHard[16]   = "POM2 HARDDISK  ";
            st[4] = floppy ? 15 : 13;
            std::memcpy(st + 5, floppy ? kFloppy : kHard, 16);
            st[21] = floppy ? 0x01 : 0x02;
            st[22] = floppy ? 0xC0 : 0x80;
            st[23] = 0x00; st[24] = 0x01;         // firmware 1.0
            buildReply(0x00, st, 25, false);
        } else {
            buildReply(0x00, st, 4, false);
        }
        break;
    }
    case kCmdRead: {
        SmartPortBusUnit* u = unitFor(dev);
        uint8_t sector[kBlockBytes];
        if (!u)                          { buildReply(kErrBadUnit, nullptr, 0, false); break; }
        if (!u->hasMedia())              { buildReply(kErrOffline, nullptr, 0, false); break; }
        if (block >= u->blockCount())    { buildReply(kErrBadBlock, nullptr, 0, false); break; }
        if (!u->readBlock(block, sector)){ buildReply(kErrIo, nullptr, 0, false); break; }
        ++progress_.blocksRead;
        buildReply(0x00, sector, kBlockBytes, true);
        break;
    }
    case kCmdFormat: {
        // Same gate as a WRITE, and for the same reason: a FORMAT is a write
        // of the whole medium. Answering $00 on an empty bay told the
        // firmware a disk that is not there had just been formatted, and
        // answering it on a write-protected one broke the promise the STATUS
        // byte had already made (bit 2 set, bit 6 clear).
        SmartPortBusUnit* u = unitFor(dev);
        if (!u)                  { buildReply(kErrBadUnit, nullptr, 0, false); break; }
        if (!u->hasMedia())      { buildReply(kErrOffline, nullptr, 0, false); break; }
        if (u->writeProtected()) { buildReply(kErrWriteProt, nullptr, 0, false); break; }
        // Nothing else to do: POM2's units are backed by an image whose
        // geometry is fixed, and the blocks a format would zero are the ones
        // the filesystem writes next anyway.
        buildReply(0x00, nullptr, 0, false);
        break;
    }
    default:
        buildReply(kErrBadCmd, nullptr, 0, false);
        break;
    }
}

void SmartPortBusDevice::serveWriteData(const std::vector<uint8_t>& body)
{
    pendingWrite_ = false;
    if (pendingCmd_ != kCmdWrite) {
        // CONTROL / character WRITE: taken, nothing to do with it.
        buildReply(0x00, nullptr, 0, false);
        return;
    }
    SmartPortBusUnit* u = unitFor(pendingUnit_);
    if (!u)                                { buildReply(kErrBadUnit, nullptr, 0, false); return; }
    if (!u->hasMedia())                    { buildReply(kErrOffline, nullptr, 0, false); return; }
    if (pendingBlock_ >= u->blockCount())  { buildReply(kErrBadBlock, nullptr, 0, false); return; }
    if (u->writeProtected())               { buildReply(kErrWriteProt, nullptr, 0, false); return; }
    if (body.size() < kBlockBytes)         { buildReply(kErrIo, nullptr, 0, false); return; }
    if (!u->writeBlock(pendingBlock_, body.data())) {
        buildReply(kErrIo, nullptr, 0, false);
        return;
    }
    ++progress_.blocksWritten;
    buildReply(0x00, nullptr, 0, false);
}

void SmartPortBusDevice::buildReply(uint8_t status, const uint8_t* contents,
                                    std::size_t n, bool dataPacket)
{
    reply_.clear();
    replyPos_ = 0;
    reply_.insert(reply_.end(), { 0xFF, 0xFF, 0xFF, kPacketBegin });

    const int odd    = static_cast<int>(n % 7);
    const int groups = static_cast<int>(n / 7);
    const std::array<uint8_t, 7> header = {
        0x00,                                   // dest: the host    → $0051
        0x01,                                   // src: this device  → $0050
        dataPacket ? kTypeData : kTypeStatus,   // type              → $004F
        0x00,                                   // aux               → $004E
        status,                                 // status / result   → $004D
        static_cast<uint8_t>(odd),              // odd count         → $004C
        static_cast<uint8_t>(groups),           // group count       → $004B
    };
    uint8_t checksum = 0;
    for (uint8_t b : header) {
        const uint8_t wire = static_cast<uint8_t>(b | 0x80);
        reply_.push_back(wire);
        checksum ^= wire;                       // header folds in as WIRE bytes
    }
    // Odd section first, then the groups: the receiver puts the odd bytes at
    // the head of the buffer and the groups behind them ($C9AC).
    if (odd) {
        appendSection(reply_, contents, odd);
        for (int k = 0; k < odd; ++k) checksum ^= contents[k];
    }
    for (int g = 0; g < groups; ++g) {
        const uint8_t* p = contents + odd + g * 7;
        appendSection(reply_, p, 7);
        for (int k = 0; k < 7; ++k) checksum ^= p[k];
    }
    reply_.push_back(static_cast<uint8_t>(checksum | 0xAA));
    reply_.push_back(static_cast<uint8_t>((checksum >> 1) | 0xAA));
    reply_.push_back(kPacketEnd);

    replyArmed_   = true;
    replyExposed_ = false;
    ++progress_.transactions;
    if (trace())
        std::fprintf(stderr, "[SPBUS] reply armed: status=$%02X contents=%zu "
                     "(%zu wire bytes)\n", status, n, reply_.size());
}

namespace {
constexpr uint8_t kBusBlobMagic[4] = { 'S', 'P', 'B', '1' };
void put16(std::vector<uint8_t>& o, std::size_t v)
{ o.push_back(static_cast<uint8_t>(v)); o.push_back(static_cast<uint8_t>(v >> 8)); }
std::size_t get16(const uint8_t* p) { return p[0] | (static_cast<std::size_t>(p[1]) << 8); }
}  // namespace

void SmartPortBusDevice::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), kBusBlobMagic, kBusBlobMagic + 4);
    put16(out, rx_.size());
    out.insert(out.end(), rx_.begin(), rx_.end());
    put16(out, reply_.size());
    out.insert(out.end(), reply_.begin(), reply_.end());
    put16(out, replyPos_);
    uint8_t flags = 0;
    if (replyArmed_)   flags |= 0x01;
    if (replyExposed_) flags |= 0x02;
    if (sense_)        flags |= 0x04;
    if (req_)          flags |= 0x08;
    if (pendingWrite_) flags |= 0x10;
    out.push_back(flags);
    out.push_back(pendingUnit_);
    out.push_back(pendingCmd_);
    out.push_back(static_cast<uint8_t>(pendingBlock_));
    out.push_back(static_cast<uint8_t>(pendingBlock_ >> 8));
    out.push_back(static_cast<uint8_t>(pendingBlock_ >> 16));
    out.push_back(static_cast<uint8_t>(pendingBlock_ >> 24));
    out.push_back(static_cast<uint8_t>(assigned_));
    for (int i = 0; i < kMaxUnits; ++i) out.push_back(ids_[static_cast<std::size_t>(i)]);
}

std::size_t SmartPortBusDevice::loadSnapshotState(const uint8_t* data, std::size_t n)
{
    busReset();
    if (!data || n < 4 || std::memcmp(data, kBusBlobMagic, 4) != 0) return 0;
    std::size_t i = 4;
    auto need = [&](std::size_t k) { return i + k <= n; };
    if (!need(2)) return 0;
    const std::size_t rxLen = get16(data + i); i += 2;
    if (rxLen > 1024 || !need(rxLen)) return 0;
    rx_.assign(data + i, data + i + rxLen); i += rxLen;
    if (!need(2)) return 0;
    const std::size_t replyLen = get16(data + i); i += 2;
    if (replyLen > 1024 || !need(replyLen)) return 0;
    reply_.assign(data + i, data + i + replyLen); i += replyLen;
    if (!need(2 + 1 + 1 + 1 + 4 + 1 + kMaxUnits)) { busReset(); return 0; }
    replyPos_ = get16(data + i); i += 2;
    if (replyPos_ > reply_.size()) replyPos_ = reply_.size();
    const uint8_t flags = data[i++];
    replyArmed_   = flags & 0x01;
    replyExposed_ = flags & 0x02;
    sense_        = flags & 0x04;
    req_          = flags & 0x08;
    pendingWrite_ = flags & 0x10;
    pendingUnit_  = data[i++];
    pendingCmd_   = data[i++];
    pendingBlock_ = static_cast<uint32_t>(data[i]) |
                    (static_cast<uint32_t>(data[i + 1]) << 8) |
                    (static_cast<uint32_t>(data[i + 2]) << 16) |
                    (static_cast<uint32_t>(data[i + 3]) << 24);
    i += 4;
    assigned_ = data[i++];
    if (assigned_ > kMaxUnits) assigned_ = kMaxUnits;
    for (int k = 0; k < kMaxUnits; ++k) ids_[static_cast<std::size_t>(k)] = data[i++];
    return i;
}

bool SmartPortBusDevice::hostReads(uint8_t& out)
{
    if (!replyExposed_ || replyPos_ >= reply_.size()) return false;
    out = reply_[replyPos_++];
    if (replyPos_ >= reply_.size()) {
        // Last byte taken: drop ACK. The receiver waits for exactly that
        // ($C5E8) before it releases REQ.
        sense_ = false;
        progress_.replyDelivered = true;
        if (trace()) std::fprintf(stderr, "[SPBUS] reply fully read\n");
    }
    return true;
}

}  // namespace pom2
