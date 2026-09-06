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

// Super Serial Card ACIA smoke test — pins the 6551-shaped command /
// control / status / data behaviour against MAME `mos6551.cpp` as the
// source of truth. The TCP bridge / worker thread are out of scope here
// (no socket spinup); we drive the card via deviceSelectRead/Write the
// way the 6502 would, and inject "bytes from the wire" via the
// `deliverRxBytes` test-only entry point.
//
// What this gates (one assertion per item from TODO §1):
//
//   * Echo mode (REM, cmd bit 4) — MAME `mos6551.cpp:309, 584-594`.
//     Setting REM=1 with DTR asserted should loop each received byte
//     back into the TX queue. OVERRUN suppresses echo (real silicon
//     idles MARK).
//   * read_rdr clears errors + RDRF — MAME `mos6551.cpp:231-236`.
//     A sticky SR_OVERRUN bit must clear on the next $C0n8 read. RDRF
//     follows rxBuf.empty() so it clears as the queue drains.
//   * DTR side-effects — MAME `mos6551.cpp:290-292, 317-321`. cmd bit 0
//     == 0 (DTR de-asserted) disables RX/TX IRQ, drops pending TX, and
//     blocks new TDR writes from queueing.
//   * DCD/DSR transitions raise IRQ — MAME `mos6551.cpp:443-461`.
//     onConnectionEdge raises IRQ_DCD|IRQ_DSR only when DTR is asserted;
//     plain status read confirms DCD+DSR bits track the connection
//     state.
//   * Overrun tracking — MAME `mos6551.cpp:542-543`. Filling rxBuf past
//     its bound sets SR_OVERRUN; the next status read sees it; read_rdr
//     clears it.
//   * Control reg baud — MAME `mos6551.cpp:271-285` + the SSC 1.8432 MHz
//     xtal. Writing index 14 ($0E) into ctl[3:0] should yield 9600 baud
//     = 960 bytes/sec; index 0 is "16x ext clk", treated as
//     unconstrained.
//   * Programmed reset (write to $C0n9) preserves parity bits 5-7 in
//     cmdReg — MAME `mos6551.cpp:264-270`.

#include "SuperSerialCard.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

// Slot $C0nX addresses for slot 2 (the SSC's conventional slot).
constexpr uint8_t kRdrAddr     = 0x8;   // $C0A8
constexpr uint8_t kStatusAddr  = 0x9;   // $C0A9
constexpr uint8_t kCommandAddr = 0xA;   // $C0AA
constexpr uint8_t kControlAddr = 0xB;   // $C0AB

// 6551 SR_* bits (mirror SuperSerialCard.h private constants).
constexpr uint8_t SR_PARITY_ERROR  = 0x01;
constexpr uint8_t SR_FRAMING_ERROR = 0x02;
constexpr uint8_t SR_OVERRUN       = 0x04;
constexpr uint8_t SR_RDRF          = 0x08;
constexpr uint8_t SR_TDRE          = 0x10;
constexpr uint8_t SR_DCD           = 0x20;
constexpr uint8_t SR_DSR           = 0x40;
constexpr uint8_t SR_IRQ           = 0x80;

void testDtrAndCommandDecode()
{
    SuperSerialCard ssc(2);

    // After construction the command register is zero → DTR=0 (de-asserted),
    // RX IRQ disabled (gated by !dtrAsserted_), echo off.
    assert(!ssc.dtrAsserted());
    assert(!ssc.rxIrqEnabled());
    assert(!ssc.echoMode());

    // Set DTR=1, RX IRQ enable bit 1=0, echo bit 4=0 → cmd=$01.
    ssc.deviceSelectWrite(kCommandAddr, 0x01);
    assert(ssc.dtrAsserted());
    assert(ssc.rxIrqEnabled());
    assert(!ssc.echoMode());

    // Set echo (bit 4) → cmd=$11.
    ssc.deviceSelectWrite(kCommandAddr, 0x11);
    assert(ssc.echoMode());

    // Mask the RX IRQ (bit 1=1) → cmd=$13. rxIrqEnable_ should go off.
    ssc.deviceSelectWrite(kCommandAddr, 0x13);
    assert(!ssc.rxIrqEnabled());
    assert(ssc.dtrAsserted());
    assert(ssc.echoMode());

    // De-assert DTR → all IRQs disabled.
    ssc.deviceSelectWrite(kCommandAddr, 0x10);
    assert(!ssc.dtrAsserted());
    assert(!ssc.rxIrqEnabled());

    std::printf("  ok: command decode (DTR / RX IRQ / echo)\n");
}

void testTdrWhileDtrDeasserted()
{
    SuperSerialCard ssc(2);
    // DTR de-asserted (cmd bit 0 = 0): writes to TDR should be dropped.
    ssc.deviceSelectWrite(kCommandAddr, 0x00);
    ssc.deviceSelectWrite(kRdrAddr, 'A');
    ssc.deviceSelectWrite(kRdrAddr, 'B');
    assert(ssc.txQueueDepth() == 0);

    // Assert DTR → TDR writes now queue.
    ssc.deviceSelectWrite(kCommandAddr, 0x01);
    ssc.deviceSelectWrite(kRdrAddr, 'X');
    assert(ssc.txQueueDepth() == 1);

    // De-assert DTR mid-stream → MAME forces MARK, POM2 drops the queue.
    ssc.deviceSelectWrite(kCommandAddr, 0x00);
    assert(ssc.txQueueDepth() == 0);

    std::printf("  ok: DTR side-effects on TDR\n");
}

void testEchoLoopback()
{
    SuperSerialCard ssc(2);
    // Echo on (cmd $11), DTR asserted, RX IRQ enable bit clear.
    ssc.deviceSelectWrite(kCommandAddr, 0x11);
    const uint8_t payload[] = { 'H', 'i', '!' };
    ssc.deliverRxBytes(payload, sizeof(payload));
    // rxBuf and txBuf both hold the 3-byte payload.
    assert(ssc.rxQueueDepth() == 3);
    assert(ssc.txQueueDepth() == 3);

    // Drain rxBuf via $C0n8 — bytes come out in FIFO order.
    assert(ssc.deviceSelectRead(kRdrAddr) == 'H');
    assert(ssc.deviceSelectRead(kRdrAddr) == 'i');
    assert(ssc.deviceSelectRead(kRdrAddr) == '!');
    assert(ssc.rxQueueDepth() == 0);

    // Echo off → next batch lands only in rxBuf.
    ssc.deviceSelectWrite(kCommandAddr, 0x01);  // DTR on, echo off
    const uint8_t solo[] = { 'Z' };
    ssc.deliverRxBytes(solo, 1);
    assert(ssc.rxQueueDepth() == 1);
    assert(ssc.txQueueDepth() == 3);            // unchanged from previous run

    std::printf("  ok: echo mode loopback\n");
}

void testOverrunAndRdrClear()
{
    SuperSerialCard ssc(2);
    ssc.deviceSelectWrite(kCommandAddr, 0x01);  // DTR on, RX IRQ enabled

    // Push more than the 4 K ring can hold so the oldest get evicted and
    // SR_OVERRUN latches.
    std::vector<uint8_t> blast(5000, 0xAA);
    ssc.deliverRxBytes(blast.data(), blast.size());
    assert(ssc.rxQueueDepth() == 4096);
    assert((ssc.statusErrorBits() & SR_OVERRUN) != 0);

    // Status read shows OVERRUN, RDRF, DCD/DSR (no client → off), TDRE.
    // It also clears the IRQ source mask (MAME `mos6551.cpp:244-248`).
    const uint8_t st = ssc.deviceSelectRead(kStatusAddr);
    assert(st & SR_TDRE);
    assert(st & SR_RDRF);
    assert(st & SR_OVERRUN);
    // ACTIVE-LOW pins: bits SET means "line inactive" (no carrier / not
    // ready) — MAME mos6551 device_reset sets both, AppleWin returns
    // ST_DSR|ST_DCD when nothing is attached. POM2 had this inverted
    // until 2026-07-29.
    assert((st & (SR_DCD | SR_DSR)) == (SR_DCD | SR_DSR));   // no client
    // No SR_IRQ flagged here — status read just consumed it and the
    // returned byte snapshots the *current* status (MAME returns the
    // pre-clear status, but POM2's order-of-evaluation is symmetric
    // since irqState_ != 0 was true at the time the byte was computed).
    assert(ssc.irqState() == 0);                // cleared by status read

    // RDR read clears SR_OVERRUN per MAME `mos6551.cpp:231-236`.
    ssc.deviceSelectRead(kRdrAddr);
    assert((ssc.statusErrorBits() & SR_OVERRUN) == 0);

    std::printf("  ok: overrun set on overflow, cleared on RDR read\n");
}

void testProgrammedResetPreservesParity()
{
    SuperSerialCard ssc(2);
    // Set parity = even, echo = 1, RX IRQ enable = 1, DTR = 1 → cmd $7B.
    ssc.deviceSelectWrite(kCommandAddr, 0x7B);
    assert(ssc.echoMode());
    assert(ssc.dtrAsserted());

    // Programmed reset (any write to $C0n9). MAME clears the low 5 bits
    // (DTR/IRQ-en/tx-ctl/echo) but preserves parity bits 5-7.
    ssc.deviceSelectWrite(kStatusAddr, 0x00);
    // DTR de-asserted (bit 0 cleared), echo cleared (bit 4 cleared).
    assert(!ssc.dtrAsserted());
    assert(!ssc.echoMode());
    // The cmd register value should retain bits 5-7 (parity = 011) but
    // have its low 5 bits zeroed.
    // 0x7B = 0111 1011 → reset to 0110 0000 = 0x60.
    // We read the value back via the device select read.
    const uint8_t cmd = ssc.deviceSelectRead(kCommandAddr);
    assert(cmd == 0x60);

    std::printf("  ok: programmed reset preserves parity bits\n");
}

void testControlRegBaud()
{
    SuperSerialCard ssc(2);
    // Index 0 = 16x external clock → unconstrained in POM2.
    ssc.deviceSelectWrite(kControlAddr, 0x00);
    assert(ssc.bytesPerSecond() == 0.0);

    // Index 14 = 9600 baud. 1.8432 MHz / (12 * 16) = 9600 / 10 = 960 bps.
    ssc.deviceSelectWrite(kControlAddr, 0x0E);
    assert(std::abs(ssc.bytesPerSecond() - 960.0) < 1e-6);

    // Index 15 = 19200 baud = 1920 bps.
    ssc.deviceSelectWrite(kControlAddr, 0x0F);
    assert(std::abs(ssc.bytesPerSecond() - 1920.0) < 1e-6);

    // Index 6 = 300 baud = 30 bps.
    ssc.deviceSelectWrite(kControlAddr, 0x06);
    assert(std::abs(ssc.bytesPerSecond() - 30.0) < 1e-6);

    // Read-back of control reg returns the byte we wrote.
    assert(ssc.deviceSelectRead(kControlAddr) == 0x06);

    std::printf("  ok: control reg baud-rate decode\n");
}

void testRxIrqGatedByCommand()
{
    SuperSerialCard ssc(2);

    // Case 1: cmd $01 — DTR on, RX IRQ enable bit 1 = 0. deliverRxBytes
    // should raise IRQ_RDRF.
    ssc.deviceSelectWrite(kCommandAddr, 0x01);
    const uint8_t b1[] = { 'a' };
    ssc.deliverRxBytes(b1, 1);
    assert(ssc.irqState() & 0x04);              // IRQ_RDRF

    // Status read clears all sources.
    ssc.deviceSelectRead(kStatusAddr);
    assert(ssc.irqState() == 0);

    // Case 2: cmd $03 — DTR on, RX IRQ disabled (bit 1 = 1). No IRQ.
    ssc.deviceSelectWrite(kCommandAddr, 0x03);
    ssc.deliverRxBytes(b1, 1);
    assert((ssc.irqState() & 0x04) == 0);

    // Case 3: cmd $00 — DTR off, RX IRQ also off via DTR gate.
    ssc.deviceSelectWrite(kCommandAddr, 0x00);
    ssc.deliverRxBytes(b1, 1);
    assert((ssc.irqState() & 0x04) == 0);

    std::printf("  ok: RX IRQ gated by cmd bit 1 + DTR\n");
}

void testCommandRegWriteClearsPendingRxIrq()
{
    // MAME `mos6551.cpp:293-296`: when the new command value disables the
    // RX IRQ enable, any pending IRQ_RDRF source is cleared and the line
    // re-evaluated.
    SuperSerialCard ssc(2);
    ssc.deviceSelectWrite(kCommandAddr, 0x01);  // DTR on, RX IRQ on
    const uint8_t b[] = { 'q' };
    ssc.deliverRxBytes(b, 1);
    assert(ssc.irqState() & 0x04);

    // Disable RX IRQ (bit 1 = 1) — pending RDRF source should go away.
    ssc.deviceSelectWrite(kCommandAddr, 0x03);
    assert((ssc.irqState() & 0x04) == 0);

    std::printf("  ok: cmd write clears pending IRQ when enable goes off\n");
}

void testTelnetLineEndingNormalisation()
{
    // SuperSerialCard::normalizeLineEndings — telnet RX line-ending fixup.
    // RFC 854: a bare carriage return is transmitted as CR NUL, ENTER as
    // CR LF. The Apple II expects CR alone. Regression pinned here: the
    // NUL in CR NUL LF used to reset prevCR before being dropped, leaking
    // a spurious second CR (CR NUL LF → CR CR instead of CR).
    auto norm = [](std::vector<uint8_t> in) {
        bool prevCR = false;
        const size_t m =
            SuperSerialCard::normalizeLineEndings(in.data(), in.size(), prevCR);
        in.resize(m);
        return in;
    };
    assert((norm({0x0D, 0x00})       == std::vector<uint8_t>{0x0D}));        // CR NUL → CR
    assert((norm({0x0D, 0x00, 0x0A}) == std::vector<uint8_t>{0x0D}));        // CR NUL LF → CR (the bug)
    assert((norm({0x0D, 0x0A})       == std::vector<uint8_t>{0x0D}));        // CR LF → CR
    assert((norm({0x0A})             == std::vector<uint8_t>{0x0D}));        // bare LF → CR
    assert(norm({0x00}).empty());                                           // lone NUL dropped
    assert((norm({'H','i',0x0D,0x00,0x0A,'!'})
                == std::vector<uint8_t>{'H','i',0x0D,'!'}));                 // embedded, passthrough

    // Regression: CR LF split across two recv() chunks. The caller-owned
    // prevCR state must span the seam — a per-call local turned the LF at
    // the head of chunk 2 into a SECOND CR (one spurious ENTER roughly
    // every 128 pasted lines through the 256-byte scratch reads).
    {
        bool prevCR = false;
        std::vector<uint8_t> c1{'A', 0x0D};
        std::vector<uint8_t> c2{0x0A, 'B'};
        c1.resize(SuperSerialCard::normalizeLineEndings(c1.data(), c1.size(), prevCR));
        c2.resize(SuperSerialCard::normalizeLineEndings(c2.data(), c2.size(), prevCR));
        assert((c1 == std::vector<uint8_t>{'A', 0x0D}));
        assert((c2 == std::vector<uint8_t>{'B'}));   // LF swallowed across the seam
    }
    // Same seam with the RFC 854 CR NUL LF spelling: CR ends chunk 1,
    // NUL LF opens chunk 2.
    {
        bool prevCR = false;
        std::vector<uint8_t> c1{0x0D};
        std::vector<uint8_t> c2{0x00, 0x0A};
        c1.resize(SuperSerialCard::normalizeLineEndings(c1.data(), c1.size(), prevCR));
        c2.resize(SuperSerialCard::normalizeLineEndings(c2.data(), c2.size(), prevCR));
        assert((c1 == std::vector<uint8_t>{0x0D}));
        assert(c2.empty());
    }
    std::printf("  ok: telnet CR/NUL/LF normalisation\n");
}

void testTelnetIacFsm()
{
    // processTelnetRx — persistent IAC state machine. Pins: variable-length
    // IAC SB … IAC SE subnegotiation fully swallowed (R6-#1, the NAWS leak),
    // 3-byte WILL/WONT/DO/DONT swallowed, IAC IAC → literal 0xFF, and an IAC
    // sequence SPLIT across recv() chunks parsed correctly (R6-#2).
    SuperSerialCard ssc(2);
    auto filter = [&](std::vector<uint8_t> in) {
        const size_t m = ssc.processTelnetRx(in.data(), in.size());
        in.resize(m);
        return in;
    };
    ssc.resetTelnet();
    assert((filter({0xFF,0xFA,0x1F,0x00,0x50,0x00,0x18,0xFF,0xF0,'A'})   // IAC SB NAWS … IAC SE
                == std::vector<uint8_t>{'A'}));
    ssc.resetTelnet();
    assert((filter({0xFF,0xFB,0x01,'X'}) == std::vector<uint8_t>{'X'}));  // WILL ECHO (3-byte)
    ssc.resetTelnet();
    assert((filter({'A',0xFF,0xFF,'B'}) == std::vector<uint8_t>{'A',0xFF,'B'})); // IAC IAC → 0xFF
    // Split across two chunks — the trailing IAC must be remembered.
    ssc.resetTelnet();
    auto c1 = filter({'A', 0xFF});           // 'A', then a dangling IAC
    auto c2 = filter({0xFB, 0x01, 'B'});     // …WILL ECHO completes, then 'B'
    c1.insert(c1.end(), c2.begin(), c2.end());
    assert((c1 == std::vector<uint8_t>{'A','B'}));
    std::printf("  ok: telnet IAC FSM (SB / WILL / IAC-IAC / split chunk)\n");
}

void testTelnetTxEscaping()
{
    // appendTelnetTxEscaped — RFC 854 conformance on the OUTBOUND leg
    // (applied by the TX drain in telnet text mode; raw mode bypasses):
    //   * data $FF doubles to IAC IAC, otherwise the peer parses it as the
    //     start of a command sequence;
    //   * a bare CR (the Apple II newline) transmits as CR NUL.
    auto esc = [](std::vector<uint8_t> in) {
        std::vector<uint8_t> out;
        for (uint8_t b : in) SuperSerialCard::appendTelnetTxEscaped(out, b);
        return out;
    };
    assert((esc({0xFF})           == std::vector<uint8_t>{0xFF, 0xFF}));
    assert((esc({0xFF, 0xFF})     == std::vector<uint8_t>{0xFF, 0xFF, 0xFF, 0xFF}));
    assert((esc({0x0D})           == std::vector<uint8_t>{0x0D, 0x00}));
    assert((esc({'H', 'i', 0x0D}) == std::vector<uint8_t>{'H', 'i', 0x0D, 0x00}));
    assert((esc({0x0A})           == std::vector<uint8_t>{0x0A}));      // LF untouched
    assert((esc({'A', 'Z', 0x00}) == std::vector<uint8_t>{'A', 'Z', 0x00}));
    std::printf("  ok: telnet TX escaping (IAC IAC / CR NUL)\n");
}

void testStatusReadDcdDsr()
{
    SuperSerialCard ssc(2);
    // No client connected → DCD + DSR bits SET (active-low pins idle high).
    const uint8_t s1 = ssc.deviceSelectRead(kStatusAddr);
    assert((s1 & (SR_DCD | SR_DSR)) == (SR_DCD | SR_DSR));
    // TDRE always set in POM2 (TCP buffers TX).
    assert(s1 & SR_TDRE);

    std::printf("  ok: status DCD/DSR mirror connection state\n");
}

}  // namespace

void testRawModeFlag()
{
    // Raw-mode (`ssc_raw_mode`, 3f42efc) gates the inbound telnet pipeline:
    // when ON the TCP worker skips BOTH processTelnetRx (IAC strip) AND
    // normalizeLineEndings (CR/NUL/LF fixup) — SuperSerialCard.cpp:256-261.
    // Those two transforms are pinned by testTelnetIacFsm +
    // testTelnetLineEndingNormalisation; here we pin the FLAG contract. The
    // default MUST be off so stock telnet clients get line-ending
    // normalisation without opting in — defaulting it on would silently
    // corrupt CR handling for every connection.
    SuperSerialCard ssc(2);
    assert(!ssc.rawMode());            // default: telnet processing ON
    ssc.setRawMode(true);
    assert(ssc.rawMode());
    ssc.setRawMode(false);
    assert(!ssc.rawMode());
    std::printf("  ok: raw-mode flag default-off + toggle\n");
}

void testPascalIdBlock()
{
    // Apple II Pascal 1.1 firmware protocol. Pascal recognises the card by the
    // ID bytes, then dispatches through the four entry OFFSETS at $Cn0D-$Cn10
    // (low byte of each routine in the $Cn page). Layout matches the real SSC
    // ROM (6502disassembly.com/a2-rom/SSC). Before this the SSC published the
    // ID bytes but no entry table → Pascal jumped into NOP fill.
    SuperSerialCard ssc(2);

    // The page is hand-assembled and every region declares where it ends
    // (SlotRomAsm.h). The entry table below is exactly the reason it has to:
    // $Cn0D-$Cn10 are the LOW BYTES of four routines, so a routine that
    // outgrew its region and pushed its neighbour down would leave Pascal
    // dispatching into the middle of an instruction — the SmartPort failure,
    // with a Pascal interpreter instead of ProDOS on the receiving end.
    assert(!ssc.romLayoutError());

    assert(ssc.slotRomRead(0x05) == 0x38);     // Pascal 1.1 sig 1
    assert(ssc.slotRomRead(0x07) == 0x18);     // Pascal 1.1 sig 2
    assert(ssc.slotRomRead(0x0B) == 0x01);     // generic Pascal 1.1 signature
    assert(ssc.slotRomRead(0x0C) == 0x31);     // device signature (comms, class 3)

    const uint8_t pinit  = ssc.slotRomRead(0x0D);
    const uint8_t pread  = ssc.slotRomRead(0x0E);
    const uint8_t pwrite = ssc.slotRomRead(0x0F);
    const uint8_t pstat  = ssc.slotRomRead(0x10);
    assert(pinit == 0x50 && pread == 0x60 && pwrite == 0x70 && pstat == 0x80);

    // Each offset must point at real code, not NOP ($EA) fill.
    assert(ssc.slotRomRead(pinit)  == 0xA9);   // PINIT   : LDA #$0B
    assert(ssc.slotRomRead(pread)  == 0xAD);   // PREAD   : LDA $C0n9
    assert(ssc.slotRomRead(pwrite) == 0x48);   // PWRITE  : PHA
    assert(ssc.slotRomRead(pstat)  == 0x4A);   // PSTATUS : LSR A

    // PSTATUS is the one routine with branch targets computed BY HAND against
    // its own internal offsets — `BCS $Cn8B` and `JMP $Cn8D`. Nothing else in
    // the ROM would notice if those two labels drifted, so they are checked
    // where they land: the input mask at $Cn8B and the shared CMP at $Cn8D.
    assert(ssc.slotRomRead(0x84) == 0xB0 && ssc.slotRomRead(0x85) == 0x05);
    assert(ssc.slotRomRead(0x88) == 0x4C && ssc.slotRomRead(0x89) == 0x8D);
    assert(ssc.slotRomRead(0x8B) == 0x29 && ssc.slotRomRead(0x8C) == 0x08);
    assert(ssc.slotRomRead(0x8D) == 0xC9 && ssc.slotRomRead(0x8E) == 0x01);

    // The PR#n / IN#n binds publish $CnB0 and $CnE0 as the character-in/out
    // vectors. Same question, same answer: both must be code, not fill.
    assert(ssc.slotRomRead(0xB0) == 0x48);     // output : PHA
    assert(ssc.slotRomRead(0xE0) == 0xAD);     // input  : LDA $C0n9

    std::printf("  ok: Pascal 1.1 ID block + entry table\n");
}

void testPrinterTapSpool()
{
    // Printer tap (//c printer port → host ImageWriter). Contract mirrors
    // PrinterCard::drainSpoolFrom: bytes accepted for transmit (past the
    // DTR gate) land in a host-visible spool; drain-from returns the total
    // size; a `from` past the end (spool cleared behind the caller) hands
    // back everything so the consumer resynchronises.
    SuperSerialCard ssc(1);
    assert(!ssc.printerTap());                 // default off at card level
    ssc.setPrinterTap(true);

    // DTR de-asserted: the transmitter drops the byte, so the tap must
    // not see it either (the tap sits on the accepted-TX stream, it is
    // not a bus wiretap).
    ssc.deviceSelectWrite(kRdrAddr, 'X');
    assert(ssc.printerSpoolBytes() == 0);

    ssc.deviceSelectWrite(kCommandAddr, 0x0B); // DTR on (firmware init value)
    const char* msg = "HELLO";
    for (const char* p = msg; *p; ++p)
        ssc.deviceSelectWrite(kRdrAddr, static_cast<uint8_t>(*p));
    assert(ssc.printerSpoolBytes() == 5);

    std::vector<uint8_t> got;
    size_t total = ssc.drainPrinterSpoolFrom(0, got);
    assert(total == 5);
    assert((got == std::vector<uint8_t>{'H','E','L','L','O'}));

    // Incremental drain from the previous total.
    ssc.deviceSelectWrite(kRdrAddr, '!');
    got.clear();
    total = ssc.drainPrinterSpoolFrom(total, got);
    assert(total == 6);
    assert((got == std::vector<uint8_t>{'!'}));

    // Clear + stale `from` → resync from the top.
    ssc.clearPrinterSpool();
    ssc.deviceSelectWrite(kRdrAddr, 'A');
    got.clear();
    total = ssc.drainPrinterSpoolFrom(6, got);
    assert(total == 1 && got.size() == 1 && got[0] == 'A');

    // Tap off → bytes still transmit (txQueue) but stop spooling.
    ssc.setPrinterTap(false);
    ssc.deviceSelectWrite(kRdrAddr, 'B');
    assert(ssc.printerSpoolBytes() == 1);

    std::printf("  ok: printer tap spool (DTR gate / drain-from / resync)\n");
}

void testRomPrInEntriesInitAcia()
{
    // The PR#n ($Cn20) and IN#n ($Cn40) firmware entries must program the
    // ACIA command register (cmd=$0B: DTR on, RX IRQ off) before hooking
    // CSW/KSW — the real SSC ROM initialises the 6551 from its DIP
    // switches on first entry (6502disassembly.com/a2-rom/SSC). Without
    // it, `PR#n : PRINT` writes the TDR with DTR de-asserted and every
    // byte is dropped (MAME `mos6551.cpp:317-321`).
    SuperSerialCard ssc(2);
    const uint8_t cmdRegAddr = 0x80 + 2 * 16 + 0xA;   // $C0AA low byte
    for (uint8_t base : { uint8_t{0x20}, uint8_t{0x40} }) {
        assert(ssc.slotRomRead(base + 0) == 0xA9);        // LDA #$0B
        assert(ssc.slotRomRead(base + 1) == 0x0B);
        assert(ssc.slotRomRead(base + 2) == 0x8D);        // STA $C0nA
        assert(ssc.slotRomRead(base + 3) == cmdRegAddr);
        assert(ssc.slotRomRead(base + 4) == 0xC0);
    }
    // The IN#n entry must not overrun the Pascal PINIT routine at $Cn50.
    assert(ssc.slotRomRead(0x4E) == 0xEA);                // still NOP fill
    assert(ssc.slotRomRead(0x50) == 0xA9);                // PINIT intact
    std::printf("  ok: PR#/IN# ROM entries init the ACIA (cmd=$0B)\n");
}

// ── Bug-hunt pins (2026-09-06) ───────────────────────────────────────────

// The transmit interrupt must actually fire.
//
// `txIrqEnable` was computed from cmd[3:2] and then thrown away with a
// `(void)` — the comment said TDRE is pinned high so the interrupt "never
// fires anyway", which has it exactly backwards: TDRE pinned high means the
// transmitter is empty the instant a byte is accepted, so the interrupt is
// due immediately. A driver programmed with command $05 (DTR on, RX IRQ on,
// TX IRQ on) writes one byte, sleeps for the ISR to ask for the next, and
// never wakes. MAME `mos6551.cpp:293-307`.
void testTxIrq()
{
    SuperSerialCard ssc(2);

    // cmd $01: DTR on, cmd[3:2] == 00 → TX IRQ NOT enabled.
    ssc.deviceSelectWrite(kCommandAddr, 0x01);
    (void)ssc.deviceSelectRead(kStatusAddr);        // clear whatever is pending
    ssc.deviceSelectWrite(kRdrAddr, 'A');
    assert((ssc.irqState() & 0x08) == 0);           // IRQ_TDRE

    // cmd $05: DTR on, cmd[3:2] == 01 → TX IRQ enabled. Arming it while the
    // transmitter is already empty raises the interrupt at once, which is the
    // first one a "enable, then wait for the ISR" driver depends on.
    ssc.deviceSelectWrite(kCommandAddr, 0x05);
    assert(ssc.irqState() & 0x08);
    assert(ssc.deviceSelectRead(kStatusAddr) & SR_IRQ);
    assert(ssc.irqState() == 0);                    // status read acknowledges

    // ...and every accepted byte raises it again.
    ssc.deviceSelectWrite(kRdrAddr, 'B');
    assert(ssc.irqState() & 0x08);

    // Disabling it clears the pending source (MAME `mos6551.cpp:293-307`).
    ssc.deviceSelectWrite(kCommandAddr, 0x01);
    assert((ssc.irqState() & 0x08) == 0);

    // DTR gates it, like every other source.
    ssc.deviceSelectWrite(kCommandAddr, 0x04);      // TX IRQ bits, DTR off
    ssc.deviceSelectWrite(kRdrAddr, 'C');
    assert((ssc.irqState() & 0x08) == 0);

    std::printf("  ok: TX IRQ raised on TDR accept and on arming\n");
}

// Telnet option requests must be ANSWERED, not merely swallowed.
//
// RFC 854 §"Telnet Options": a party that will not enable an option must
// refuse it — WONT to a WILL, DONT to a DO. Silence is not legal and not
// inert: a stock `telnet` client sends its opening burst, waits, gets
// nothing, and settles into LINE MODE with local echo, so the Apple II sees
// a whole line at a time and the user sees every character twice.
void testTelnetOptionNegotiation()
{
    SuperSerialCard ssc(2);
    auto feed = [&](std::vector<uint8_t> in) {
        (void)ssc.processTelnetRx(in.data(), in.size());
        return ssc.pendingTelnetReply();
    };

    // BINARY (0) and SGA (3) are the two that make this an 8-bit,
    // character-at-a-time pipe — accept both, in either direction.
    ssc.resetTelnet();
    assert((feed({0xFF, 0xFD, 0x00}) ==                  // DO BINARY
            std::vector<uint8_t>{0xFF, 0xFB, 0x00}));    // → WILL BINARY
    ssc.resetTelnet();
    assert((feed({0xFF, 0xFB, 0x03}) ==                  // WILL SGA
            std::vector<uint8_t>{0xFF, 0xFD, 0x03}));    // → DO SGA

    // Anything else is refused, and refusing is still an answer.
    ssc.resetTelnet();
    assert((feed({0xFF, 0xFD, 0x18}) ==                  // DO TERMINAL-TYPE
            std::vector<uint8_t>{0xFF, 0xFC, 0x18}));    // → WONT
    ssc.resetTelnet();
    assert((feed({0xFF, 0xFB, 0x01}) ==                  // WILL ECHO
            std::vector<uint8_t>{0xFF, 0xFE, 0x01}));    // → DONT

    // A refusal we already agree with gets NO answer — echoing DONT at a
    // DONT is the option loop RFC 854 warns about.
    ssc.resetTelnet();
    assert(feed({0xFF, 0xFE, 0x18}).empty());            // DONT TERMINAL-TYPE
    ssc.resetTelnet();
    assert(feed({0xFF, 0xFC, 0x01}).empty());            // WONT ECHO

    // Split across chunks: the command byte is remembered.
    ssc.resetTelnet();
    (void)feed({'A', 0xFF});
    assert((feed({0xFD, 0x03}) ==                        // …DO SGA completes
            std::vector<uint8_t>{0xFF, 0xFB, 0x03}));    // → WILL SGA

    // The answer leaves through the TX drain VERBATIM — it is protocol, not
    // guest data, so the IAC must NOT be doubled the way appendTelnetTxEscaped
    // doubles a data $FF.
    std::vector<uint8_t> out;
    (void)ssc.drainTransportTx(out);
    assert((out == std::vector<uint8_t>{0xFF, 0xFB, 0x03}));
    assert(ssc.pendingTelnetReply().empty());            // queue drained once

    std::printf("  ok: telnet option negotiation answered (RFC 854)\n");
}

// A snapshot is a FILE. `statusErrors_` is the STICKY-ERROR half of the
// status register and nothing else: the other bits are computed at read time
// and OR'd with it, so a blob carrying $FF pinned DCD and DSR high for the
// session — a carrier-aware driver reads "NO CARRIER" with a live peer
// attached and no register write can clear it.
void testSnapshotStatusErrorsAreMasked()
{
    SuperSerialCard ssc(2);
    std::vector<uint8_t> blob;
    ssc.appendSnapshotState(blob);
    assert(blob.size() >= 14);

    // Byte 6 is statusErrors_ (magic 4 + cmdReg + ctlReg), byte 13 irqState_.
    blob[6]  = 0xFF;
    blob[13] = 0xFF;
    ssc.loadSnapshotState(blob.data(), blob.size());

    assert(ssc.statusErrorBits() ==
           (SR_PARITY_ERROR | SR_FRAMING_ERROR | SR_OVERRUN));
    assert((ssc.statusErrorBits() & (SR_DCD | SR_DSR | SR_RDRF | SR_TDRE)) == 0);
    assert(ssc.irqState() == 0x0F);   // the four defined sources, nothing else

    std::printf("  ok: snapshot restore masks statusErrors_ / irqState_\n");
}

int main()
{
    testDtrAndCommandDecode();
    testTdrWhileDtrDeasserted();
    testEchoLoopback();
    testOverrunAndRdrClear();
    testProgrammedResetPreservesParity();
    testControlRegBaud();
    testRxIrqGatedByCommand();
    testCommandRegWriteClearsPendingRxIrq();
    testTelnetLineEndingNormalisation();
    testTelnetIacFsm();
    testTelnetTxEscaping();
    testStatusReadDcdDsr();
    testRawModeFlag();
    testPascalIdBlock();
    testPrinterTapSpool();
    testRomPrInEntriesInitAcia();
    testTxIrq();
    testTelnetOptionNegotiation();
    testSnapshotStatusErrorsAreMasked();
    std::printf("OK ssc_acia_smoke\n");
    return 0;
}
