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


// SmartPortBusDevice on the bench: the frames the firmware sends, built by
// hand, and what comes back — no ROM, no IWM. The boot tests prove the wire
// against real firmware; this pins the parts a boot never exercises:
//   * chain numbers come from the host (a //c+ starts at 2) — a device that
//     assumed 1 refused every //c+ READ with $2F;
//   * a frame whose checksum does not match gets NO reply (bug hunt 3: a
//     frame spliced from two transactions could otherwise write garbage);
//   * WRITE is two packets, command then data, and the block lands;
//   * a command packet that arrives INSTEAD of the promised data packet wins
//     (bug hunt 5: the abandoned WRITE latched `active()` for ever, and the
//     //c external port then owned $C0E0-$C0EF with the Disk II dead behind
//     it — and its stale block could still take a later data packet);
//   * FORMAT is gated like a write: offline with no media, $2B when
//     protected (bug hunt 6: it answered $00 to both);
//   * STATUS answers four bytes with the block count.

#include "SmartPortBusDevice.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct RamUnit final : pom2::SmartPortBusUnit {
    std::vector<uint8_t> blocks = std::vector<uint8_t>(512 * 4, 0);
    bool media = true, wp = false;
    bool     hasMedia()       const override { return media; }
    uint32_t blockCount()     const override { return 4; }
    bool     writeProtected() const override { return wp; }
    bool readBlock(uint32_t b, uint8_t out[512]) override
    { if (b >= 4) return false; std::memcpy(out, &blocks[b * 512], 512); return true; }
    bool writeBlock(uint32_t b, const uint8_t in[512]) override
    { if (b >= 4) return false; std::memcpy(&blocks[b * 512], in, 512); return true; }
};

// The sender's encoding ($C800 in the Liron dump): odd section then groups
// of seven behind a high-bits marker; checksum over header WIRE bytes and
// decoded contents, 4-and-4; every byte with bit 7.
std::vector<uint8_t> frame(uint8_t dest, uint8_t type, const std::vector<uint8_t>& body,
                           bool corruptChecksum = false)
{
    std::vector<uint8_t> w = { 0xFF, 0xFF, 0xC3 };
    const uint8_t odd = static_cast<uint8_t>(body.size() % 7);
    const uint8_t grp = static_cast<uint8_t>(body.size() / 7);
    const uint8_t hdr[7] = { dest, 0x00, type, 0x00, 0x00, odd, grp };
    uint8_t sum = 0;
    for (uint8_t h : hdr) { w.push_back(h | 0x80); sum ^= (h | 0x80); }
    auto section = [&](const uint8_t* p, int n) {
        uint8_t high = 0x80;
        for (int k = 0; k < n; ++k) if (p[k] & 0x80) high |= 0x40 >> k;
        w.push_back(high);
        for (int k = 0; k < n; ++k) { w.push_back(p[k] | 0x80); sum ^= p[k]; }
    };
    if (odd) section(body.data(), odd);
    for (int g = 0; g < grp; ++g) section(body.data() + odd + g * 7, 7);
    if (corruptChecksum) sum ^= 0x01;
    w.push_back(sum | 0xAA);
    w.push_back((sum >> 1) | 0xAA);
    w.push_back(0xC8);
    return w;
}

// One transaction as the firmware drives the lines: REQ up, bytes, REQ down
// (the ack), REQ up, read the reply, REQ down.
std::vector<uint8_t> transact(pom2::SmartPortBusDevice& d, const std::vector<uint8_t>& wire,
                              bool expectReply = true)
{
    d.reqChanged(true);
    for (uint8_t b : wire) d.hostWrote(b);
    d.hostWrote(0x00);                 // the drained-handshake store after $C8
    assert(!d.sense() && "a completed frame drops ACK");
    d.reqChanged(false);
    assert(d.sense());
    std::vector<uint8_t> reply;
    d.reqChanged(true);
    uint8_t b;
    while (d.hostReads(b)) reply.push_back(b);
    if (expectReply) assert(!reply.empty());
    d.reqChanged(false);
    return reply;
}

// Header status byte of a reply: after sync + $C3, byte index 4.
uint8_t replyStatus(const std::vector<uint8_t>& r)
{
    size_t i = 0; while (i < r.size() && r[i] != 0xC3) ++i;
    assert(i + 7 < r.size());
    return static_cast<uint8_t>(r[i + 5] & 0x7F);
}

}  // namespace

int main()
{
    RamUnit u0, u1;
    for (int i = 0; i < 512; ++i) u0.blocks[512 + i] = static_cast<uint8_t>(i);   // block 1 pattern
    pom2::SmartPortBusDevice d;
    d.setUnit(0, &u0); d.setUnit(1, &u1); d.setUnitCount(2);
    d.reset();

    // A //c+ scan: INIT for device 2, then 3. First answers "more", second "last".
    assert(replyStatus(transact(d, frame(2, 0x00, {0x05, 0x02}))) == 0x00);
    assert(replyStatus(transact(d, frame(3, 0x00, {0x05, 0x02}))) == 0x7F);   // $FF & $7F
    // READ block 1 of unit 2 (= u0): contents = cmd, params, buf lo/hi, block lo/mid/hi, 2 pad
    auto r = transact(d, frame(2, 0x00, {0x01, 0x03, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00}));
    assert(replyStatus(r) == 0x00 && "READ on the host's number for u0 succeeds");
    assert(d.progress().blocksRead == 1);
    // Unit 1 is device 3 now; device 1 does not exist on this chain.
    r = transact(d, frame(1, 0x00, {0x01, 0x03, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00}));
    assert(replyStatus(r) == 0x11 && "no unit was assigned number 1");

    // A garbled frame: same READ, checksum off by one bit → ack, but NO reply.
    d.reqChanged(true);
    for (uint8_t b : frame(2, 0x00, {0x01, 0x03, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00}, true))
        d.hostWrote(b);
    assert(!d.sense());
    d.reqChanged(false);
    d.reqChanged(true);
    uint8_t junk;
    assert(!d.hostReads(junk) && "a frame with a bad checksum is not served");
    d.reqChanged(false);
    assert(d.progress().badChecksums == 1);
    assert(d.progress().blocksRead == 1 && "…and it did not read a block");

    // WRITE block 2 of unit 3 (= u1): command packet, then a $82 data packet.
    transact(d, frame(3, 0x00, {0x02, 0x03, 0x00, 0x08, 0x02, 0x00, 0x00, 0x00, 0x00}),
             /*expectReply=*/false);
    std::vector<uint8_t> data(512);
    for (int i = 0; i < 512; ++i) data[i] = static_cast<uint8_t>(0xA5 ^ i);
    r = transact(d, frame(3, 0x02, data));
    assert(replyStatus(r) == 0x00);
    assert(std::memcmp(&u1.blocks[2 * 512], data.data(), 512) == 0 && "the block landed");
    assert(d.progress().blocksWritten == 1);

    // STATUS code 0 on unit 2: four bytes, online, 4 blocks.
    r = transact(d, frame(2, 0x00, {0x00, 0x03, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00}));
    assert(replyStatus(r) == 0x00);
    {
        size_t i = 0; while (r[i] != 0xC3) ++i;
        const uint8_t odd = static_cast<uint8_t>(r[i + 6] & 0x7F);
        assert(odd == 4);
        const uint8_t high = r[i + 8];
        const uint8_t gen  = static_cast<uint8_t>((r[i + 9] & 0x7F) | ((high << 1) & 0x80));
        assert((gen & 0x10) && "online: media in the unit");
        assert((r[i + 10] & 0x7F) == 4 && "block count low byte");
    }

    // A command packet ends whatever came before it. A WRITE whose data
    // packet never arrives — the firmware abandons one on a bad checksum and
    // retries without resetting the bus — used to latch `pendingWrite_` for
    // ever: `active()` stayed true and the //c external port went on claiming
    // every $C0E0-$C0EF access, with the Disk II dead behind it.
    {
        uint8_t before[512];
        std::memcpy(before, &u1.blocks[3 * 512], 512);
        // WRITE block 3 of unit 3 (= u1), command packet only…
        transact(d, frame(3, 0x00, {0x02, 0x03, 0x00, 0x08, 0x03, 0x00, 0x00, 0x00, 0x00}),
                 /*expectReply=*/false);
        assert(d.active() && "the bus is busy between the two packets");
        // …then a READ instead of the data packet the drive was promised.
        r = transact(d, frame(2, 0x00, {0x01, 0x03, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00}));
        assert(replyStatus(r) == 0x00 && "the new command is served");
        assert(!d.active() && "…and the abandoned WRITE does not latch the bus");
        // A data packet arriving now belongs to nothing: the stale unit,
        // command and block went with the WRITE.
        std::vector<uint8_t> stray(512, 0x5A);
        transact(d, frame(3, 0x02, stray), /*expectReply=*/false);
        assert(std::memcmp(&u1.blocks[3 * 512], before, 512) == 0 &&
               "a stray data packet must not write the abandoned block");
        assert(d.progress().blocksWritten == 1 && "…and writes nothing at all");
    }

    // FORMAT is a write of the whole medium, so it answers the same refusals
    // as one: offline with no media, write-protected when protected.
    {
        const std::vector<uint8_t> fmt = {0x03, 0x01, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00};
        u1.media = false;
        assert(replyStatus(transact(d, frame(3, 0x00, fmt))) == 0x2F &&
               "FORMAT on an empty bay is offline");
        u1.media = true;
        u1.wp = true;
        assert(replyStatus(transact(d, frame(3, 0x00, fmt))) == 0x2B &&
               "FORMAT on a write-protected unit is refused");
        u1.wp = false;
        assert(replyStatus(transact(d, frame(3, 0x00, fmt))) == 0x00);
        assert(replyStatus(transact(d, frame(1, 0x00, fmt))) == 0x11 &&
               "FORMAT on a number nobody was assigned is a bad unit");
    }

    // Snapshot in the middle of a transaction: the command is in, the ack
    // given, REQ not yet released. A restored device must still put the
    // reply on the wire when REQ drops — the rewind ring lands here.
    {
        d.reqChanged(true);
        for (uint8_t b : frame(2, 0x00, {0x01, 0x03, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00}))
            d.hostWrote(b);
        assert(!d.sense());
        std::vector<uint8_t> blob;
        d.appendSnapshotState(blob);

        pom2::SmartPortBusDevice restored;
        restored.setUnit(0, &u0); restored.setUnit(1, &u1); restored.setUnitCount(2);
        assert(restored.loadSnapshotState(blob.data(), blob.size()) == blob.size());
        assert(!restored.sense() && "mid-transaction: ACK still low");
        restored.reqChanged(false);
        assert(restored.sense());
        restored.reqChanged(true);
        std::vector<uint8_t> reply; uint8_t rb;
        while (restored.hostReads(rb)) reply.push_back(rb);
        restored.reqChanged(false);
        assert(replyStatus(reply) == 0x00 && "the READ armed before the snapshot is served after it");
        // the chain numbers travelled too: device 3 is still u1
        assert(replyStatus(transact(restored,
            frame(3, 0x00, {0x00, 0x03, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00}))) == 0x00);
        // and a truncated blob is refused cleanly
        pom2::SmartPortBusDevice bad;
        assert(bad.loadSnapshotState(blob.data(), blob.size() / 2) == 0);
        // finish the original's transaction so the device below starts idle
        d.reqChanged(false); d.reqChanged(true);
        while (d.hostReads(rb)) {}
        d.reqChanged(false);
    }

    // A bus reset forgets the numbers: device 1 is assignable again.
    d.busReset();
    assert(replyStatus(transact(d, frame(1, 0x00, {0x05, 0x02}))) == 0x00);
    r = transact(d, frame(1, 0x00, {0x01, 0x03, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00}));
    assert(replyStatus(r) == 0x00);

    std::printf("smartport_bus_device: OK — host-assigned numbers, checksum "
                "enforced, two-packet WRITE, a new command drops the pending "
                "one, FORMAT gated like a write, STATUS bytes, snapshot "
                "mid-transaction\n");
    return 0;
}
