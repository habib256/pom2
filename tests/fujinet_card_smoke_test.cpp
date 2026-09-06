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

// FujiNet card smoke test — pins src/FujiNetCard.cpp.
//
// Runs a REAL 6502 through the card's synthesised slot ROM against a FAKE
// FUJINET on loopback, so the whole path is exercised end to end: guest
// instruction → $C0n2 trap → SP-over-SLIP round trip → response written back
// into emulated RAM → registers and flags → RTS.
//
// What is pinned, worst-consequence first:
//
//   1. THE STACK FIXUP, INCLUDING THE PAGE-1 WRAP. A SmartPort call is
//      `JSR $Cn0D` followed by THREE INLINE BYTES (command, then a pointer to
//      the parameter list). The host has to rewrite the pushed return address
//      to step over them, or the ROM's RTS returns straight into the command
//      byte and the guest executes its own parameter list as code.
//
//      The wrap is a separate hazard with the same blast radius: the FujiNet
//      AppleWin fork's `regs.sp` is already a full $01xx address, so it
//      indexes mem[regs.sp + 1]; POM2's getStackPointer() is the 8-bit
//      register, so the $0100 base and the & $FF are mandatory. Written
//      without the mask, a call made with SP near the bottom of the page
//      writes the fixed-up address to $0200 instead of $0100 — corrupting
//      somebody else's memory and leaving the real return address stale.
//      testStackWrapAtPageBoundary is what catches that.
//
//   2. THE BOOT PATH WORKS AND FAILS SAFE. With a peer, block 0 loads to
//      $0800 and runs. WITHOUT one — the common case, since the FujiNet
//      desktop build is a separate program the user may not have started —
//      the ROM must CONTINUE THE AUTOSTART SLOT SCAN instead of erroring,
//      or plugging this card into slot 7 would break booting from the Disk II
//      in slot 6 whenever no FujiNet is running.
//
//   3. A BUS SCAN TERMINATES WITH NO PEER. STATUS unit 0 / code 0 is answered
//      locally, so a guest enumerating the bus gets "0 devices" immediately
//      rather than one full timeout per probe.
//
//   4. THE I/O PAGE IS NEVER USED AS A BUFFER. A parameter list pointing at
//      $C0xx must be refused: "reading memory" there toggles soft switches,
//      so a malformed call could flip the machine's video mode or bank state
//      as a side effect.

#include "FujiNetCard.h"
#include "FujiNetCardFactory.h"
#include "M6502.h"
#include "Memory.h"
#include "SlipFramer.h"
#include "SocketCompat.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>

#if !POM2_HAS_SOCKETS

int main()
{
    std::puts("SKIP: built without host sockets");
    return 77;   // ctest SKIP_RETURN_CODE
}

#else

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace pom2;

constexpr int      kSlot     = 7;
constexpr uint16_t kSlotRom  = 0xC000 + kSlot * 0x100;
/// $C0(8+slot)2 — where the ROM stores its magic byte.
constexpr uint16_t kTrapAddr = 0xC080 + kSlot * 16 + 2;

// ── Fake FujiNet ─────────────────────────────────────────────────────────

using Handler = std::function<void(const std::vector<uint8_t>&,
                                   std::vector<uint8_t>&)>;

class FakePeer
{
public:
    FakePeer(uint16_t port, Handler h) : handler_(std::move(h))
    {
        ensureSocketStack();
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(isValidSocket(fd_));
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = hostToNet16(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        th_ = std::thread(&FakePeer::run, this);
    }
    ~FakePeer() { stop(); }

    void stop()
    {
        if (stopped_.exchange(true)) return;
        shutdownBoth(fd_);
        if (th_.joinable()) th_.join();
        closeHostSocket(fd_);
    }

private:
    void run()
    {
        SlipFramer rx;
        uint8_t    buf[1024];
        while (!stopped_.load()) {
            if (waitSocket(fd_, SocketWait::Read, 100) != WaitResult::Ready) continue;
            const iolen_t got = recvSocket(fd_, buf, sizeof(buf));
            if (got <= 0) break;
            for (iolen_t i = 0; i < got; ++i) {
                if (rx.feed(buf[i]) != SlipFramer::Feed::Frame) continue;
                std::vector<uint8_t> wire;
                handler_(rx.frame(), wire);
                if (!wire.empty()) sendSocket(fd_, wire.data(), wire.size());
            }
        }
    }

    Handler           handler_;
    socket_t          fd_ = kInvalidSocket;
    std::thread       th_;
    std::atomic<bool> stopped_{false};
};

void pushResponse(std::vector<uint8_t>& wire, uint8_t seq, uint8_t status,
                  const std::vector<uint8_t>& payload = {})
{
    std::vector<uint8_t> body{ seq, status };
    body.insert(body.end(), payload.begin(), payload.end());
    SlipFramer::encode(body, wire);
}

/// Block 0 shaped like a ProDOS boot block: byte 0 = block count (must be 1),
/// byte 1 = first opcode (must not be BRK). We make it `JMP $0801`, so a
/// successful boot parks the CPU there and the test can see it.
std::vector<uint8_t> bootBlock()
{
    std::vector<uint8_t> b(512, 0x00);
    b[0] = 0x01;                      // block count
    b[1] = 0x4C; b[2] = 0x01; b[3] = 0x08;    // JMP $0801
    for (size_t i = 4; i < b.size(); ++i) b[i] = static_cast<uint8_t>(i & 0xFF);
    return b;
}

/// One block device on unit 1, with a recognisable STATUS payload.
void standardHandler(const std::vector<uint8_t>& req, std::vector<uint8_t>& wire)
{
    if (req.size() < 11) return;
    const uint8_t seq = req[0], cmd = req[1], unit = req[3];

    switch (cmd) {
    case kSpInit:
        pushResponse(wire, seq, unit <= 2 ? 0x00 : 0x01);
        return;
    case kSpStatus: {
        const uint8_t code = req[6];
        if (code == 0x03) {
            // Unit 2 is the PRINTER, and it reproduces the firmware's own
            // mislabelling: iwmPrinter::create_dib_reply_packet sets
            // dib.type = SP_TYPE_BYTE_FUJINET_MODEM ($15). If POM2 keyed the
            // printer tap on the type byte instead of the name, this unit
            // would be taken for a modem and nothing would ever reach paper.
            const bool printer = (unit == 2);
            const char* nm = printer ? "PRINTER" : "FUJINET";
            const uint8_t nlen = static_cast<uint8_t>(std::strlen(nm));
            std::vector<uint8_t> dib{ 0xF8, 0x40, 0x06, 0x00, nlen };
            for (int i = 0; i < 16; ++i)
                dib.push_back(i < nlen ? static_cast<uint8_t>(nm[i]) : ' ');
            dib.push_back(printer ? 0x15 : 0x02);      // ← the upstream bug
            dib.push_back(0x00);
            dib.push_back(0x01); dib.push_back(0x00);
            pushResponse(wire, seq, 0x00, dib);
        } else {
            // General status: status byte + 3-byte block count ($0640).
            pushResponse(wire, seq, 0x00, { 0xF8, 0x40, 0x06, 0x00 });
        }
        return;
    }
    case kSpReadBlock: {
        const uint32_t block = static_cast<uint32_t>(req[6]) |
                               (static_cast<uint32_t>(req[7]) << 8) |
                               (static_cast<uint32_t>(req[8]) << 16);
        if (block == 0) { pushResponse(wire, seq, 0x00, bootBlock()); return; }
        std::vector<uint8_t> d(512);
        for (size_t i = 0; i < d.size(); ++i)
            d[i] = static_cast<uint8_t>((block * 5 + i) & 0xFF);
        pushResponse(wire, seq, 0x00, d);
        return;
    }
    case kSpWrite:
        // Accept whatever the guest sends to any character device.
        pushResponse(wire, seq, 0x00);
        return;
    default:
        pushResponse(wire, seq, 0x00);
        return;
    }
}

// ── Machine harness ──────────────────────────────────────────────────────

struct Machine {
    Memory       mem;
    std::unique_ptr<M6502> cpu;
    FujiNetCard* card = nullptr;
    uint16_t     port = 0;

    Machine()
    {
        mem.clearRam();
        mem.resetSoftSwitches();
        cpu = std::make_unique<M6502>(&mem);
        mem.setCpu(cpu.get());
        cpu->hardReset();

        auto c = pom2::makeFujiNetCard(kSlot);
        card = c.get();
        card->setMemory(&mem);
        card->setCpu(cpu.get());
        mem.slotBus().plug(kSlot, std::move(c));
    }

    /// Bring the link up on a free loopback port.
    void startLink()
    {
        std::string err;
        for (uint16_t p = 19900; p < 19940; ++p) {
            card->transportLink().setTcpMode(p);
            if (card->transportLink().start(err)) { port = p; return; }
        }
        assert(false && "no free test port");
    }

    void run(int cycles)
    {
        int done = 0;
        while (done < cycles) done += cpu->run(64);
    }

    /// Step instruction by instruction until the PC hits `target`.
    ///
    /// Needed rather than "run, then look at the PC" for any target in ROM
    /// space: no Apple II ROM is loaded in this harness, so $D000-$FFFF reads
    /// as zeroes and the Language Card swallows writes — a landmark cannot be
    /// planted at $FABA to make the CPU park there.
    bool runUntilPc(uint16_t target, int maxSteps = 20000)
    {
        for (int i = 0; i < maxSteps; ++i) {
            cpu->run(1);
            if (cpu->getProgramCounter() == target) return true;
        }
        return false;
    }
};

template <typename F>
bool waitFor(F pred, int timeoutMs = 4000)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

/// Lay down `JSR spEntry / cmd / paramPtr` at $0300 with an endless loop as
/// the landing site, and point the CPU at it.
void placeSmartPortCall(Machine& m, uint8_t command, uint16_t paramList)
{
    const uint16_t prodosEntry = kSlotRom + m.mem.memRead(kSlotRom + 0xFF);
    const uint16_t spEntry     = static_cast<uint16_t>(prodosEntry + 3);

    m.mem.memWrite(0x0300, 0x20);                                  // JSR
    m.mem.memWrite(0x0301, static_cast<uint8_t>(spEntry & 0xFF));
    m.mem.memWrite(0x0302, static_cast<uint8_t>(spEntry >> 8));
    m.mem.memWrite(0x0303, command);                               // inline
    m.mem.memWrite(0x0304, static_cast<uint8_t>(paramList & 0xFF));
    m.mem.memWrite(0x0305, static_cast<uint8_t>(paramList >> 8));
    // The landing site: if the fixup is wrong we never get here.
    m.mem.memWrite(0x0306, 0x4C);
    m.mem.memWrite(0x0307, 0x06);
    m.mem.memWrite(0x0308, 0x03);                                  // JMP $0306
    m.cpu->setProgramCounter(0x0300);
}

// ═════════════════════════════════════════════════════════════════════════
// 1. The ROM
// ═════════════════════════════════════════════════════════════════════════
void testRomSignature()
{
    Machine m;

    // The 256-byte page is hand-assembled, and every region declares where it
    // ends (SlotRomAsm.h). A routine that outgrew its budget used to overwrite
    // its neighbour in silence — that is how SmartPortCard's ProDOS STATUS
    // became dead code. The flag says nothing here fits by accident; the byte
    // checks below say the layout is also the layout the callers assume.
    assert(!m.card->romLayoutError());

    // The ProDOS block-device signature POM2's own bootFromSlot validates.
    assert(m.mem.memRead(kSlotRom + 0x01) == 0x20);
    assert(m.mem.memRead(kSlotRom + 0x03) == 0x00);
    assert(m.mem.memRead(kSlotRom + 0x05) == 0x03);
    // $Cn07 = $00 — SmartPort class.
    assert(m.mem.memRead(kSlotRom + 0x07) == 0x00);

    // ProDOS driver entry, and the SmartPort entry EXACTLY three bytes later
    // (the Apple convention every SmartPort caller relies on).
    const uint8_t drvOff = m.mem.memRead(kSlotRom + 0xFF);
    assert(m.mem.memRead(kSlotRom + drvOff) == 0x38);              // SEC
    assert(m.mem.memRead(kSlotRom + drvOff + 3) == 0xA9);          // LDA #imm
    assert(m.mem.memRead(kSlotRom + drvOff + 4) == FujiNetCard::kMagicSmartPort);

    // The trap store must target THIS slot's device-select window.
    bool foundTrap = false;
    for (int i = drvOff; i < drvOff + 16; ++i) {
        if (m.mem.memRead(kSlotRom + i) == 0x8D &&
            m.mem.memRead(kSlotRom + i + 2) == 0xC0) {
            assert(m.mem.memRead(kSlotRom + i + 1) ==
                   static_cast<uint8_t>(kTrapAddr & 0xFF));
            foundTrap = true;
            break;
        }
    }
    assert(foundTrap);

    // Total blocks zeroed on purpose → ProDOS must ask via STATUS.
    assert(m.mem.memRead(kSlotRom + 0xFC) == 0x00);
    assert(m.mem.memRead(kSlotRom + 0xFD) == 0x00);

    std::printf("  ok: ROM signature, entries and trap address\n");
}

// ═════════════════════════════════════════════════════════════════════════
// 2. Boot: with a peer, and (crucially) without one
// ═════════════════════════════════════════════════════════════════════════
void testBootWithPeer()
{
    Machine m;
    m.startLink();
    FakePeer peer(m.port, standardHandler);
    assert(waitFor([&] { return m.card->link().deviceCount() == 2; }));

    // Enter the card the way the autostart scan does.
    m.cpu->setProgramCounter(kSlotRom);
    m.run(20000);

    // Block 0 landed at $0800...
    const auto expect = bootBlock();
    for (size_t i = 0; i < 8; ++i)
        assert(m.mem.memRead(static_cast<uint16_t>(0x0800 + i)) == expect[i]);
    // ...and the boot block is running.
    assert(m.cpu->getProgramCounter() == 0x0801);
    // X must carry the unit byte the ProDOS boot convention expects.
    assert(m.cpu->getXRegister() == static_cast<uint8_t>(kSlot << 4));

    peer.stop();
    m.card->transportLink().stop();
    std::printf("  ok: boots block 0 into $0800 and runs it\n");
}

void testBootWithNoPeerContinuesSlotScan()
{
    Machine m;
    m.startLink();                       // listening, but nobody connects

    // Stage the machine the way the //e autostart scan leaves it just before
    // it JMPs to $Cn00: $00/$01 hold that address and MSLOT holds $Cn.
    m.mem.memWrite(0x0000, 0x00);
    m.mem.memWrite(0x0001, static_cast<uint8_t>(kSlotRom >> 8));
    m.mem.memWrite(0x07F8, static_cast<uint8_t>(kSlotRom >> 8));

    m.cpu->setProgramCounter(kSlotRom);

    // THE assertion: with no FujiNet answering, the card hands the scan back
    // rather than taking over the machine. Without this, a FujiNet card in
    // slot 7 would stop slot 6 from booting whenever the FujiNet is off.
    //
    // $FABA is the Monitor's "keep scanning" entry, so it lives in ROM space
    // that this harness has not loaded — hence stepping to catch the PC on
    // its way through rather than expecting the CPU to settle there.
    assert(m.runUntilPc(0xFABA));

    m.card->transportLink().stop();
    std::printf("  ok: no peer → autostart slot scan continues ($FABA)\n");
}

// ═════════════════════════════════════════════════════════════════════════
// 3. The SmartPort call: stack fixup, registers, flags
// ═════════════════════════════════════════════════════════════════════════
void testSmartPortStatusCall()
{
    Machine m;
    m.startLink();
    FakePeer peer(m.port, standardHandler);
    assert(waitFor([&] { return m.card->link().deviceCount() == 2; }));

    // Parameter list at $0310: count, unit, payload pointer, status code.
    m.mem.memWrite(0x0310, 0x03);
    m.mem.memWrite(0x0311, 0x01);        // unit 1
    m.mem.memWrite(0x0312, 0x00);        // payload → $4000
    m.mem.memWrite(0x0313, 0x40);
    m.mem.memWrite(0x0314, 0x00);        // status code 0
    placeSmartPortCall(m, kSpStatus, 0x0310);

    m.run(20000);

    // THE stack fixup: execution resumed AFTER the three inline bytes.
    assert(m.cpu->getProgramCounter() == 0x0306);
    // Success, with the transfer length in X/Y and carry clear.
    assert(m.cpu->getAccumulator() == 0x00);
    assert(m.cpu->getXRegister() == 4);
    assert(m.cpu->getYRegister() == 0);
    assert((m.cpu->getStatusRegister() & M6502::Status::C) == 0);
    // The peer's status payload really landed in guest RAM.
    assert(m.mem.memRead(0x4000) == 0xF8);
    assert(m.mem.memRead(0x4001) == 0x40);
    assert(m.mem.memRead(0x4002) == 0x06);

    peer.stop();
    m.card->transportLink().stop();
    std::printf("  ok: SmartPort STATUS — stack fixup, payload, A/X/Y, carry\n");
}

void testSmartPortReadBlock()
{
    Machine m;
    m.startLink();
    FakePeer peer(m.port, standardHandler);
    assert(waitFor([&] { return m.card->link().deviceCount() == 2; }));

    m.mem.memWrite(0x0310, 0x03);
    m.mem.memWrite(0x0311, 0x01);
    m.mem.memWrite(0x0312, 0x00);        // buffer → $2000
    m.mem.memWrite(0x0313, 0x20);
    m.mem.memWrite(0x0314, 0x07);        // block $000007, little-endian
    m.mem.memWrite(0x0315, 0x00);
    m.mem.memWrite(0x0316, 0x00);
    placeSmartPortCall(m, kSpReadBlock, 0x0310);

    m.run(20000);

    assert(m.cpu->getProgramCounter() == 0x0306);
    assert(m.cpu->getAccumulator() == 0x00);
    assert(m.cpu->getXRegister() == 0x00);
    assert(m.cpu->getYRegister() == 0x02);      // 512 bytes
    for (int i = 0; i < 512; ++i)
        assert(m.mem.memRead(static_cast<uint16_t>(0x2000 + i)) ==
               static_cast<uint8_t>((7 * 5 + i) & 0xFF));

    peer.stop();
    m.card->transportLink().stop();
    std::printf("  ok: SmartPort READ BLOCK — 512 bytes into guest RAM\n");
}

void testStackWrapAtPageBoundary()
{
    Machine m;
    m.startLink();
    FakePeer peer(m.port, standardHandler);
    assert(waitFor([&] { return m.card->link().deviceCount() == 2; }));

    m.mem.memWrite(0x0310, 0x03);
    m.mem.memWrite(0x0311, 0x01);
    m.mem.memWrite(0x0312, 0x00);
    m.mem.memWrite(0x0313, 0x40);
    m.mem.memWrite(0x0314, 0x00);
    placeSmartPortCall(m, kSpStatus, 0x0310);

    // SP = $01: the JSR pushes across the bottom of page 1, so the return
    // address ends up at $0100/$0101 and the handler's index wraps. Computed
    // without the & $FF mask, the fixup would be written to $0200 instead —
    // clobbering RAM and leaving the stale address on the stack.
    m.cpu->setStackPointer(0x01);
    // Anything at $0200 would be corrupted by the un-masked form; watch it.
    m.mem.memWrite(0x0200, 0x5A);
    m.mem.memWrite(0x0201, 0xA5);

    m.run(20000);

    assert(m.cpu->getProgramCounter() == 0x0306);      // fixup still correct
    assert(m.mem.memRead(0x0200) == 0x5A);             // and nothing else touched
    assert(m.mem.memRead(0x0201) == 0xA5);

    peer.stop();
    m.card->transportLink().stop();
    std::printf("  ok: stack fixup wraps inside page 1 (SP = $01)\n");
}

// ═════════════════════════════════════════════════════════════════════════
// 4. Behaviour with no peer
// ═════════════════════════════════════════════════════════════════════════
void testDeviceCountWithoutPeer()
{
    Machine m;
    m.startLink();

    m.mem.memWrite(0x0310, 0x03);
    m.mem.memWrite(0x0311, 0x00);        // unit 0 = the bus itself
    m.mem.memWrite(0x0312, 0x00);
    m.mem.memWrite(0x0313, 0x40);
    m.mem.memWrite(0x0314, 0x00);        // status code 0 = device count
    placeSmartPortCall(m, kSpStatus, 0x0310);

    m.run(20000);

    assert(m.cpu->getProgramCounter() == 0x0306);
    assert(m.cpu->getAccumulator() == 0x00);     // success...
    assert(m.mem.memRead(0x4000) == 0x00);       // ...with zero devices
    assert(m.cpu->getXRegister() == 8);
    // Answered locally: a bus-scan probe must never become a link call.
    //
    // This was a stopwatch (`assert(ms < 100)` around the run above), and the
    // stopwatch could not measure what its comment claimed: with no peer
    // attached, `SpOverSlipLink::transact` returns on `!transport_->isOpen()`
    // BEFORE the 250 ms wait, so the "network timeout per probe" failure mode
    // is unreachable in this test. All the bound could ever react to was host
    // speed — and it duly fired under a valgrind run of the suite, on a path
    // with nothing network about it. The link's own counters state the
    // intended property directly, and no slowdown can perturb them.
    const auto stats = m.card->transportLink().stats();
    assert(stats.calls == 0);
    assert(stats.timeouts == 0);
    assert(m.card->localCount() == 1);

    m.card->transportLink().stop();
    std::printf("  ok: device-count probe answered locally, no peer needed\n");
}

// ═════════════════════════════════════════════════════════════════════════
//  The built-in N: has to be FINDABLE, and it must not shadow anything
// ═════════════════════════════════════════════════════════════════════════
//
// A SmartPort chain is contiguous 1..N and what unit 0 answers is a COUNT,
// not a highest-unit number. Every standard chain walk stops at the first
// unit that answers "no device", so a device parked past the peer's last one
// is invisible to the very scan meant to find it — which is what a fixed unit
// 11 did: with no peer the guest probed unit 1, got nothing, and stopped.
void testBuiltInNetworkIsInsideTheChain()
{
    Machine m;
    m.card->setBuiltInNetwork(true);
    m.startLink();                       // listening, but nobody connects

    // Unit 0 / code 0 — the bus scan.
    m.mem.memWrite(0x0310, 0x03);
    m.mem.memWrite(0x0311, 0x00);
    m.mem.memWrite(0x0312, 0x00);
    m.mem.memWrite(0x0313, 0x40);
    m.mem.memWrite(0x0314, 0x00);
    placeSmartPortCall(m, kSpStatus, 0x0310);
    m.run(20000);

    assert(m.cpu->getAccumulator() == 0x00);
    // ONE device, not eleven: the count must be a count.
    assert(m.mem.memRead(0x4000) == 0x01);

    // And unit 1 must be the network device — the DIB names it, so a guest
    // that walks the chain finds it on the first probe.
    m.mem.memWrite(0x0310, 0x03);
    m.mem.memWrite(0x0311, 0x01);        // unit 1
    m.mem.memWrite(0x0312, 0x00);
    m.mem.memWrite(0x0313, 0x41);        // DIB buffer at $4100
    m.mem.memWrite(0x0314, 0x03);        // status code 3 = DIB
    placeSmartPortCall(m, kSpStatus, 0x0310);
    m.run(20000);

    assert(m.cpu->getAccumulator() == 0x00);
    char name[8] = {};
    for (int i = 0; i < 7; ++i) name[i] = static_cast<char>(m.mem.memRead(0x4105 + i));
    assert(std::string(name) == "NETWORK");
    // b7 is "block device" and this is a character device. 0xF8 claimed block
    // device, zero blocks and formattable all at once.
    assert((m.mem.memRead(0x4100) & 0x80) == 0);
    assert((m.mem.memRead(0x4100) & 0x10) != 0);   // b4 online

    // The GENERAL status call is not the network status. Answering the latter
    // put $00 in byte 0 whenever nothing was open, and byte 0 is the status
    // byte: b4 clear reads as "offline, no read, no write", so a chain walker
    // concluded the device was dead before ever opening anything.
    m.mem.memWrite(0x0310, 0x03);
    m.mem.memWrite(0x0311, 0x01);
    m.mem.memWrite(0x0312, 0x00);
    m.mem.memWrite(0x0313, 0x42);
    m.mem.memWrite(0x0314, 0x00);        // status code 0 = general status
    placeSmartPortCall(m, kSpStatus, 0x0310);
    m.run(20000);

    assert(m.cpu->getAccumulator() == 0x00);
    assert((m.mem.memRead(0x4200) & 0x10) != 0);   // online, even with nothing open

    // None of it involved the link: no peer, no calls, no timeouts.
    const auto stats = m.card->transportLink().stats();
    assert(stats.calls == 0);
    assert(stats.timeouts == 0);

    m.card->transportLink().stop();
    std::printf("  ok: built-in N: sits at unit 1 and answers a scan with no peer\n");
}

// With a peer attached, the built-in device must step out of the way of the
// peer's own chain rather than answer for one of its units.
void testBuiltInNetworkDoesNotShadowThePeer()
{
    Machine m;
    m.card->setBuiltInNetwork(true);
    m.startLink();
    FakePeer peer(m.port, standardHandler);
    assert(waitFor([&] { return m.card->link().deviceCount() == 2; }));

    // The fake peer publishes two devices, neither of them a network device,
    // so the built-in one belongs at unit 3 — past them, not on top of them.
    m.mem.memWrite(0x0310, 0x03);
    m.mem.memWrite(0x0311, 0x00);
    m.mem.memWrite(0x0312, 0x00);
    m.mem.memWrite(0x0313, 0x40);
    m.mem.memWrite(0x0314, 0x00);
    placeSmartPortCall(m, kSpStatus, 0x0310);
    m.run(20000);

    assert(m.cpu->getAccumulator() == 0x00);
    assert(m.mem.memRead(0x4000) == 0x03);   // the peer's 2 + ours

    // Unit 1 is the PEER's. Its DIB must come from the peer, not from us —
    // a network device answering a disk's block reads is ProDOS reporting an
    // I/O error on a perfectly good volume.
    const auto before = m.card->transportLink().stats().calls;
    m.mem.memWrite(0x0310, 0x03);
    m.mem.memWrite(0x0311, 0x01);
    m.mem.memWrite(0x0312, 0x00);
    m.mem.memWrite(0x0313, 0x41);
    m.mem.memWrite(0x0314, 0x03);
    placeSmartPortCall(m, kSpStatus, 0x0310);
    m.run(20000);

    // Forwarded: the call reached the link instead of being answered here.
    assert(m.card->transportLink().stats().calls > before);
    char name[8] = {};
    for (int i = 0; i < 7; ++i) name[i] = static_cast<char>(m.mem.memRead(0x4105 + i));
    assert(std::string(name) != "NETWORK");

    m.card->transportLink().stop();
    std::printf("  ok: built-in N: steps aside for the peer's own units\n");
}

void testForwardedCallWithoutPeerIsNoDevice()
{
    Machine m;
    m.startLink();

    m.mem.memWrite(0x0310, 0x03);
    m.mem.memWrite(0x0311, 0x01);        // a real unit — must be forwarded
    m.mem.memWrite(0x0312, 0x00);
    m.mem.memWrite(0x0313, 0x40);
    m.mem.memWrite(0x0314, 0x00);
    placeSmartPortCall(m, kSpStatus, 0x0310);

    m.run(20000);

    assert(m.cpu->getProgramCounter() == 0x0306);
    assert(m.cpu->getAccumulator() == kSpNoDevice);          // $28
    assert((m.cpu->getStatusRegister() & M6502::Status::C) != 0);  // carry = error

    m.card->transportLink().stop();
    std::printf("  ok: no peer → $28 (no device) with carry set\n");
}

// ═════════════════════════════════════════════════════════════════════════
// 5. The I/O page must never be used as a transfer buffer
// ═════════════════════════════════════════════════════════════════════════
void testIoPageBufferRefused()
{
    Machine m;
    m.startLink();
    FakePeer peer(m.port, standardHandler);
    assert(waitFor([&] { return m.card->link().deviceCount() == 2; }));

    // Point the buffer at $C050 — the TEXT/GRAPHICS soft switch. If the card
    // marshalled through Memory blindly, servicing this call would flip the
    // machine into graphics mode as a side effect of "writing memory".
    m.mem.memWrite(0x0310, 0x03);
    m.mem.memWrite(0x0311, 0x01);
    m.mem.memWrite(0x0312, 0x50);
    m.mem.memWrite(0x0313, 0xC0);
    m.mem.memWrite(0x0314, 0x00);
    m.mem.memWrite(0x0315, 0x00);
    m.mem.memWrite(0x0316, 0x00);
    placeSmartPortCall(m, kSpReadBlock, 0x0310);

    const bool textBefore = m.mem.getDisplayState().textMode;
    m.run(20000);

    assert(m.cpu->getProgramCounter() == 0x0306);
    assert(m.cpu->getAccumulator() == kSpIoError);            // refused, $27
    assert(m.mem.getDisplayState().textMode == textBefore);                 // and inert

    peer.stop();
    m.card->transportLink().stop();
    std::printf("  ok: a buffer inside $C0xx is refused, soft switches untouched\n");
}

void testControlListCannotWrapAddressSpace()
{
    Machine m;
    // CONTROL payload points at $FFFF. Reading its uint16 length used to read
    // the high byte from $0000, then payload+2 wrapped to $0001.
    m.mem.memWrite(0x0310, 0x03);
    m.mem.memWrite(0x0311, 0x01);
    m.mem.memWrite(0x0312, 0xFF);
    m.mem.memWrite(0x0313, 0xFF);
    m.mem.memWrite(0x0314, 0x01);
    placeSmartPortCall(m, kSpControl, 0x0310);

    m.run(20000);
    assert(m.cpu->getProgramCounter() == 0x0306);
    assert(m.cpu->getAccumulator() == kSpIoError);
    std::printf("  ok: CONTROL list at $FFFF cannot wrap into page zero\n");
}

// ═════════════════════════════════════════════════════════════════════════
// 6. Snapshot blob
// ═════════════════════════════════════════════════════════════════════════
void testSnapshotBlob()
{
    Machine m;
    std::vector<uint8_t> blob;
    m.card->appendSnapshotState(blob);
    assert(blob.size() >= 6);
    assert(blob[0] == 'F' && blob[1] == 'N' && blob[2] == 'E' && blob[3] == 'T');

    // Round trip is a no-op beyond resynchronising the link...
    m.card->loadSnapshotState(blob.data(), blob.size());

    // ...and a foreign blob (a different card was in this slot when the
    // snapshot was taken) must be ignored, not misparsed.
    const uint8_t foreign[6] = { 'X', 'X', 'X', 'X', 0x09, 0x01 };
    m.card->loadSnapshotState(foreign, sizeof(foreign));

    std::printf("  ok: snapshot blob is tagged and ignores foreign data\n");
}


// ═════════════════════════════════════════════════════════════════════════
// 7. Printer tap (phase 2)
// ═════════════════════════════════════════════════════════════════════════
void testPrinterTap()
{
    Machine m;
    m.startLink();
    FakePeer peer(m.port, standardHandler);
    assert(waitFor([&] { return m.card->link().deviceCount() == 2; }));

    // Unit 2 advertises itself with type $15 (MODEM) because the FujiNet
    // firmware mislabels its own printer — the name is what identifies it.
    assert(m.card->hasPrinterUnit());
    assert(m.card->bytesWritten() == 0);

    // A tiny print job in guest RAM, containing the two bytes SLIP has to
    // escape so the whole path is exercised.
    const uint8_t job[] = { 'H', 'i', 0x0D, 0xC0, 0xDB, 0x1B, 'n' };
    for (size_t i = 0; i < sizeof(job); ++i)
        m.mem.memWrite(static_cast<uint16_t>(0x3000 + i), job[i]);

    // SmartPort WRITE: count(4), unit, buffer ptr, byte count, address.
    m.mem.memWrite(0x0310, 0x04);
    m.mem.memWrite(0x0311, 0x02);                 // unit 2 = the printer
    m.mem.memWrite(0x0312, 0x00);                 // buffer → $3000
    m.mem.memWrite(0x0313, 0x30);
    m.mem.memWrite(0x0314, sizeof(job));          // byte count lo
    m.mem.memWrite(0x0315, 0x00);                 // byte count hi
    m.mem.memWrite(0x0316, 0x00);                 // address
    m.mem.memWrite(0x0317, 0x00);
    m.mem.memWrite(0x0318, 0x00);
    placeSmartPortCall(m, kSpWrite, 0x0310);
    m.run(20000);

    assert(m.cpu->getProgramCounter() == 0x0306);
    assert(m.cpu->getAccumulator() == 0x00);

    // The bytes reached POM2's spool, byte-exact, through the same
    // drainSpoolFrom contract PrinterCard uses.
    assert(m.card->bytesWritten() == sizeof(job));
    std::vector<uint8_t> drained;
    const size_t total = m.card->drainSpoolFrom(0, drained);
    assert(total == sizeof(job));
    assert(drained.size() == sizeof(job));
    assert(std::memcmp(drained.data(), job, sizeof(job)) == 0);

    // Incremental drain: from the end yields nothing, and the total is
    // unchanged — this is what stops pumpImageWriter reprinting every frame.
    std::vector<uint8_t> again;
    assert(m.card->drainSpoolFrom(total, again) == total);
    assert(again.empty());

    // A write to a NON-printer unit must not reach paper.
    m.mem.memWrite(0x0311, 0x01);                 // unit 1 = a block device
    placeSmartPortCall(m, kSpWrite, 0x0310);
    m.run(20000);
    assert(m.card->bytesWritten() == sizeof(job));   // unchanged

    peer.stop();
    m.card->transportLink().stop();
    std::printf("  ok: printer tap spools only the printer unit's writes\n");
}

} // namespace

int main()
{
    testRomSignature();
    testBootWithPeer();
    testBootWithNoPeerContinuesSlotScan();
    testSmartPortStatusCall();
    testSmartPortReadBlock();
    testStackWrapAtPageBoundary();
    testDeviceCountWithoutPeer();
    testBuiltInNetworkIsInsideTheChain();
    testBuiltInNetworkDoesNotShadowThePeer();
    testForwardedCallWithoutPeerIsNoDevice();
    testIoPageBufferRefused();
    testControlListCannotWrapAddressSpace();
    testSnapshotBlob();
    testPrinterTap();

    std::puts("fujinet_card: OK");
    return 0;
}

#endif // POM2_HAS_SOCKETS
