// The SmartPort bus blob is self-delimiting — including the old one.
//
// Every owner of a `SmartPortBusDevice` hands `loadSnapshotState` the whole
// REMAINDER of its own blob and then continues reading at the offset the bus
// returns (LironCard.cpp:319, IIcExternalSmartPort.cpp:208). The id table was
// four entries until 2026-09-08 and eight after, and the loader that grew it
// read "up to kMaxUnits ids while bytes remain": on a pre-2026-09-08 snapshot
// it ate four bytes of the OWNER's tail, put them in ids_[4..7] and returned
// an offset four bytes too far. The owner's `active_` / `busMediaMask_` /
// per-drive mechanism sections then all came from the wrong place, and the
// stolen bytes became host-assigned chain numbers nobody assigned — so a
// packet addressed to a device that is not on the chain was served, and a
// WRITE to it landed on a real disk.
//
// This pins the contract instead: whatever the blob's vintage, the bus
// consumes exactly its own bytes and never invents a chain number.

#include "SmartPortBusDevice.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace pom2;

namespace {

int g_failures = 0;
void check(bool ok, const std::string& what)
{
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

struct Unit final : SmartPortBusUnit {
    std::vector<uint8_t> data = std::vector<uint8_t>(512u * 4u, 0x11);
    int  lastWrite = -1;
    bool     hasMedia()       const override { return true; }
    uint32_t blockCount()     const override { return 4; }
    bool     writeProtected() const override { return false; }
    bool     readBlock(uint32_t b, uint8_t out[512]) override
    { std::memcpy(out, data.data() + b * 512, 512); return true; }
    bool     writeBlock(uint32_t b, const uint8_t in[512]) override
    { std::memcpy(data.data() + b * 512, in, 512); lastWrite = int(b); return true; }
};

void appendSection(std::vector<uint8_t>& out, const uint8_t* d, int n)
{
    uint8_t high = 0x80;
    for (int k = 0; k < n; ++k) if (d[k] & 0x80) high |= uint8_t(0x40 >> k);
    out.push_back(high);
    for (int k = 0; k < n; ++k) out.push_back(uint8_t(d[k] | 0x80));
}

std::vector<uint8_t> frame(uint8_t dest, uint8_t type,
                           const std::vector<uint8_t>& body)
{
    const int odd    = int(body.size() % 7);
    const int groups = int(body.size() / 7);
    std::vector<uint8_t> f = { 0xFF, 0xFF, 0xFF, 0xC3 };
    const uint8_t hdr[7] = { dest, 0x00, type, 0x00, 0x00,
                             uint8_t(odd), uint8_t(groups) };
    uint8_t chk = 0;
    for (uint8_t b : hdr) { const uint8_t w = uint8_t(b | 0x80); f.push_back(w); chk ^= w; }
    if (odd) { appendSection(f, body.data(), odd); for (int k = 0; k < odd; ++k) chk ^= body[k]; }
    for (int g = 0; g < groups; ++g) {
        const uint8_t* p = body.data() + odd + g * 7;
        appendSection(f, p, 7);
        for (int k = 0; k < 7; ++k) chk ^= p[k];
    }
    f.push_back(uint8_t(chk | 0xAA));
    f.push_back(uint8_t((chk >> 1) | 0xAA));
    f.push_back(0xC8);
    return f;
}

/// One command packet through the REQ/ACK handshake; returns the reply's
/// status byte, or -1 when the device answered nothing.
int transact(SmartPortBusDevice& bus, const std::vector<uint8_t>& f)
{
    bus.reqChanged(false);
    bus.sense();
    bus.reqChanged(true);
    for (uint8_t b : f) bus.hostWrote(b);
    bus.sense();
    bus.reqChanged(false);
    bus.reqChanged(true);
    std::vector<uint8_t> reply;
    uint8_t byte = 0;
    while (bus.hostReads(byte)) reply.push_back(byte);
    bus.reqChanged(false);
    for (std::size_t i = 0; i + 7 < reply.size(); ++i)
        if (reply[i] == 0xC3) return reply[i + 5] & 0x7F;
    return -1;
}

/// The bytes a pre-2026-09-08 build wrote for an idle bus whose host had
/// assigned chain numbers 2 and 3 (a //c+: its internal MIG drive is 1).
/// Hand-written, NOT derived from the current writer — that is the point.
std::vector<uint8_t> legacyBlobV1()
{
    return {
        'S','P','B','1',
        0x00, 0x00,                 // rx length
        0x00, 0x00,                 // reply length
        0x00, 0x00,                 // replyPos
        0x04,                       // flags: sense high, nothing pending
        0x00,                       // pendingUnit
        0x00,                       // pendingCmd
        0x00, 0x00, 0x00, 0x00,     // pendingBlock
        0x02,                       // assigned_
        0x02, 0x03, 0x00, 0x00,     // ids_[0..3]
    };
}

}  // namespace

int main()
{
    // Owner tail bytes, chosen to look exactly like chain numbers: this is
    // LironCard's `active_ + 1`, `busMediaMask_`, then a 32-bit mechanism
    // blob length.
    const std::vector<uint8_t> tail = { 0x02, 0x03, 0x04, 0x00, 0x00, 0x00 };

    // ── 1. a legacy (4-entry) blob ──────────────────────────────────────
    {
        const std::vector<uint8_t> blob = legacyBlobV1();
        std::vector<uint8_t> stream = blob;
        stream.insert(stream.end(), tail.begin(), tail.end());

        Unit units[SmartPortBusDevice::kMaxUnits];
        SmartPortBusDevice bus;
        for (int i = 0; i < SmartPortBusDevice::kMaxUnits; ++i) bus.setUnit(i, &units[i]);
        bus.setUnitCount(SmartPortBusDevice::kMaxUnits);

        const std::size_t used = bus.loadSnapshotState(stream.data(), stream.size());
        check(used == blob.size(),
              "legacy blob consumes exactly " + std::to_string(blob.size()) +
              " bytes (got " + std::to_string(used) + ")");

        // The numbers the host really assigned still work…
        check(transact(bus, frame(0x02, 0x00, { 0x00, 0x03, 0x00, 0x00, 0x00 })) == 0x00,
              "chain device $02 (assigned) answers STATUS");
        // …and the owner's tail bytes did NOT become chain numbers.
        check(transact(bus, frame(0x04, 0x00, { 0x00, 0x03, 0x00, 0x00, 0x00 })) == 0x11,
              "chain device $04 (never assigned) answers $11 bad unit");

        // The damaging consequence: a WRITE to that number must write nothing.
        {
            const std::vector<uint8_t> cmd = { 0x02, 0x03, 0x00, 0x00, 0x01, 0x00, 0x00 };
            const std::vector<uint8_t> payload(512, 0xA5);
            bus.reqChanged(false); bus.sense(); bus.reqChanged(true);
            for (uint8_t b : frame(0x04, 0x00, cmd)) bus.hostWrote(b);
            bus.sense(); bus.reqChanged(false);
            uint8_t d; bus.reqChanged(true); while (bus.hostReads(d)) {}
            bus.reqChanged(false); bus.reqChanged(true);
            for (uint8_t b : frame(0x04, 0x02, payload)) bus.hostWrote(b);
            bus.sense(); bus.reqChanged(false);
            bus.reqChanged(true); while (bus.hostReads(d)) {}
            bus.reqChanged(false);
        }
        int hit = -1;
        for (int i = 0; i < SmartPortBusDevice::kMaxUnits; ++i)
            if (units[i].lastWrite >= 0) hit = i;
        check(hit == -1 && bus.progress().blocksWritten == 0,
              "a WRITE to an unassigned chain number writes no block (landed on "
              "unit index " + std::to_string(hit) + ")");
    }

    // ── 2. a blob this build wrote, in the same owner framing ───────────
    {
        Unit units[SmartPortBusDevice::kMaxUnits];
        SmartPortBusDevice bus;
        for (int i = 0; i < SmartPortBusDevice::kMaxUnits; ++i) bus.setUnit(i, &units[i]);
        bus.setUnitCount(SmartPortBusDevice::kMaxUnits);
        bus.reset();
        transact(bus, frame(0x82, 0x00, { 0x05, 0x01, 0x00 }));   // INIT -> device 2
        transact(bus, frame(0x83, 0x00, { 0x05, 0x01, 0x00 }));   // INIT -> device 3

        std::vector<uint8_t> blob;
        bus.appendSnapshotState(blob);
        std::vector<uint8_t> stream = blob;
        stream.insert(stream.end(), tail.begin(), tail.end());

        SmartPortBusDevice restored;
        Unit runits[SmartPortBusDevice::kMaxUnits];
        for (int i = 0; i < SmartPortBusDevice::kMaxUnits; ++i) restored.setUnit(i, &runits[i]);
        restored.setUnitCount(SmartPortBusDevice::kMaxUnits);
        const std::size_t used = restored.loadSnapshotState(stream.data(), stream.size());
        check(used == blob.size(),
              "current blob consumes exactly " + std::to_string(blob.size()) +
              " bytes (got " + std::to_string(used) + ")");
        check(transact(restored, frame(0x03, 0x00, { 0x00, 0x03, 0x00, 0x00, 0x00 })) == 0x00,
              "the restored chain still answers for device $03");
        check(transact(restored, frame(0x04, 0x00, { 0x00, 0x03, 0x00, 0x00, 0x00 })) == 0x11,
              "…and still refuses device $04");
    }

    // ── 3. a truncated table is refused, not half-read ──────────────────
    {
        std::vector<uint8_t> blob = legacyBlobV1();
        blob.pop_back();                              // one id short
        SmartPortBusDevice bus;
        Unit u;
        bus.setUnit(0, &u);
        bus.setUnitCount(1);
        check(bus.loadSnapshotState(blob.data(), blob.size()) == 0,
              "a truncated id table is rejected");
    }

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASS\n", g_failures);
    return g_failures ? 1 : 0;
}
