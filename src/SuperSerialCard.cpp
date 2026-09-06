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

#include "SuperSerialCard.h"
#include "SlotRomAsm.h"

#include "SuperSerialTransport.h"
#include "Pom2Build.h"
#include "Logger.h"
#include "M6502.h"

#include <cerrno>
#include <cstring>
#if POM2_HAS_SOCKETS
// POSIX socket stack — used for the telnet bridge listener. Under
// Emscripten there is no BSD-socket API in the browser, so the
// listener / worker thread is compiled out and startListening()
// becomes a logged no-op. The rest of the SSC (6551 ACIA registers,
// slot ROM, Pascal 1.1 block) is fully functional in WASM; only the
// host-side TCP plumbing is dropped.
// Host socket stack for both families — POSIX and Winsock. SocketUtil.h
// (included above) is built on it and carries the accept/SIGPIPE idioms.
#endif

namespace {

constexpr size_t kBufCap   = 4096;     // bounded ring buffer (TX and RX)
constexpr size_t kTailCap  = 256;      // recent-bytes peephole

// MAME `mos6551.cpp:46` — internal baud-rate divider table indexed by
// control bits 0-3. Index 0 is the "16x external clock" mode (effectively
// unconstrained for POM2). Otherwise baud = xtal / (divider * 16) with
// the SSC's 1.8432 MHz crystal — the resulting standard rates run from
// 50 baud (index 1) to 19200 (index 15).
constexpr int kInternalDivider[16] = {
    1, 2304, 1536, 1048, 856, 768, 384, 192, 96, 64, 48, 32, 24, 16, 12, 6
};
constexpr double kSscXtalHz = 1843200.0;

// MAME `mos6551.cpp:49-55`: per-cmd[2:3] table of {tx_irq, rts_active, brk}.
// Only `tx_irq` is consulted in POM2 (TDRE is pinned high so the IRQ never
// fires anyway, but we still mirror the state so cmdReg readback matches a
// real driver's expectation).
constexpr bool kTxIrqEnableByCmd[4] = { false, true, false, false };

double baudIndexToBytesPerSec(uint8_t idx)
{
    if (idx == 0) return 0.0;     // 16x ext clk — treat as unconstrained
    const int divider = kInternalDivider[idx & 0xF];
    if (divider <= 0) return 0.0;
    const double baud = kSscXtalHz / (divider * 16.0);
    // 8-N-1 framing: 1 start + 8 data + 1 stop = 10 bit-times per byte.
    // (We don't refine by extraStop/wordlength since this is the most
    // common configuration and the error is ≤10% — negligible vs the
    // wall-clock jitter of the worker's 2 ms idle.)
    return baud / 10.0;
}

// Telnet IAC = $FF. We swallow IAC + 2 bytes (the most common 3-byte
// commands: WILL/WONT/DO/DONT) so a stock `telnet` client's option
// negotiation doesn't leak garbage bytes into the Apple II keyboard.
// IAC IAC ($FF $FF) is a literal escaped $FF — pass one $FF through.
// swallowTelnetIac replaced by the persistent member state machine
// SuperSerialCard::processTelnetRx (see header) — a stateless per-call
// function can neither span a recv() chunk boundary nor handle the
// variable-length IAC SB … IAC SE subnegotiation. Definition follows the
// anonymous namespace.

// normalizeLineEndings moved to a public static member (see header) so it
// can be unit-tested directly; definition follows the anonymous namespace.

}  // namespace

// Line-ending normalisation for telnet-sourced RX: a stock telnet client
// sends CR LF on ENTER and bare LF on some line-mode tools, but the Apple
// II expects CR alone. Applying once here on RX symmetrises the rxBuf and
// keyboard-sink consumers.
// Transformations:
//   * drop NUL ($00) FIRST — per RFC 854 a telnet client transmits a bare
//     carriage return as the two-byte sequence CR NUL. The NUL must be
//     dropped before it can touch `prevCR`; otherwise the CR-state is lost
//     and a following LF (CR NUL LF) is wrongly emitted as a second CR.
//   * collapse CR LF → CR (strip the LF after a CR).
//   * map bare LF → CR.
// Buffer mutated in place, returns new length.
size_t SuperSerialCard::normalizeLineEndings(uint8_t* data, size_t n,
                                             bool& prevCR)
{
    size_t w = 0;
    for (size_t r = 0; r < n; ++r) {
        uint8_t c = data[r];
        if (c == 0) continue;                       // drop NUL before prevCR
        if (c == '\n' && prevCR) { prevCR = false; continue; }
        prevCR = (c == '\r');
        if (c == '\n') c = '\r';
        data[w++] = c;
    }
    return w;
}

// Telnet TX escaping (RFC 854). Two transformations, applied byte-by-byte
// as the worker drains txBuf in telnet text mode (raw mode sends verbatim —
// same flag that gates the RX-side IAC/CR filters):
//   * $FF → IAC IAC ($FF $FF). A literal data $FF would otherwise start an
//     IAC command sequence at the peer and corrupt the stream.
//   * bare CR → CR NUL. RFC 854 requires a carriage return that is not part
//     of CR LF to be transmitted as the two-byte sequence CR NUL; the Apple
//     II's newline is a bare CR, so every CR it emits gets the NUL.
void SuperSerialCard::appendTelnetTxEscaped(std::vector<uint8_t>& out, uint8_t b)
{
    out.push_back(b);
    if (b == 0xFF)      out.push_back(0xFF);    // IAC IAC
    else if (b == '\r') out.push_back(0x00);    // CR NUL
}

// RFC 854 §"Telnet Options": a party that receives a request to enable an
// option it does not want MUST refuse it, and refusing means answering — WONT
// to a WILL, DONT to a DO. Silence is not a legal answer and it is not an
// inert one either: a stock `telnet` client sends its opening WILL/DO burst,
// waits for the replies, and with none arriving it settles into LINE MODE and
// local echo. The user then types a whole line before the Apple II sees any of
// it, and sees every character twice.
//
// The two options POM2 does want are the ones that make the link an 8-bit
// character-at-a-time pipe, which is what a serial port is:
//   * BINARY (0)   — RFC 856, no 7-bit or NVT translation.
//   * SGA    (3)   — RFC 858, suppress go-ahead; a client that gets WILL SGA
//                    leaves line mode.
// Everything else is refused. `swallowed` is what the caller already did and
// stays done; only the ANSWER is new.
void SuperSerialCard::answerTelnetOption(uint8_t command, uint8_t option)
{
    constexpr uint8_t kIac  = 0xFF;
    constexpr uint8_t kWill = 0xFB, kWont = 0xFC, kDo = 0xFD, kDont = 0xFE;
    constexpr uint8_t kOptBinary = 0x00, kOptSga = 0x03;

    const bool accept = (option == kOptBinary || option == kOptSga);
    uint8_t answer;
    switch (command) {
    // "You may" / "You must not": we answer about OUR OWN side.
    case kDo:   answer = accept ? kWill : kWont; break;
    case kDont: answer = kWont; break;
    // "I will" / "I won't": we answer about THEIR side.
    case kWill: answer = accept ? kDo : kDont;   break;
    case kWont: answer = kDont; break;
    default: return;
    }

    // Never answer a REFUSAL we already agree with — DONT/WONT are the state
    // both ends are already in, and echoing them starts an option loop that
    // RFC 854 §"option negotiation" specifically warns about.
    if ((command == kDont && answer == kWont) ||
        (command == kWont && answer == kDont))
        return;

    std::lock_guard<std::mutex> lk(bufferMtx);
    // Straight into the raw reply queue, not txBuf: these bytes are telnet
    // PROTOCOL and must reach the wire unescaped, whereas everything in txBuf
    // is guest data and gets IAC-doubled on the way out.
    constexpr size_t kReplyCap = 256;      // a negotiation burst, not a stream
    if (telnetReply_.size() + 3 > kReplyCap) return;
    telnetReply_.push_back(kIac);
    telnetReply_.push_back(answer);
    telnetReply_.push_back(option);
}

size_t SuperSerialCard::processTelnetRx(uint8_t* data, size_t n)
{
    size_t w = 0;
    for (size_t r = 0; r < n; ++r) {
        const uint8_t b = data[r];
        switch (telnetState_) {
        case TelnetState::Text:
            if (b == 0xFF) telnetState_ = TelnetState::Iac;   // start of command
            else           data[w++] = b;                     // data byte
            break;
        case TelnetState::Iac:
            if (b == 0xFF) {                                  // IAC IAC → literal 0xFF
                data[w++] = 0xFF;
                telnetState_ = TelnetState::Text;
            } else if (b >= 0xFB && b <= 0xFE) {              // WILL/WONT/DO/DONT
                // Persistent, like telnetState_: a chunk boundary can fall
                // between the command and its option byte.
                telnetCommand_ = b;
                telnetState_ = TelnetState::Opt;              // one option byte follows
            } else if (b == 0xFA) {                           // SB — subnegotiation
                telnetState_ = TelnetState::Sb;
            } else {                                          // 2-byte command (GA, NOP, …)
                telnetState_ = TelnetState::Text;
            }
            break;
        case TelnetState::Opt:
            // Swallow the option byte — and ANSWER it. See answerTelnetOption.
            answerTelnetOption(telnetCommand_, b);
            telnetCommand_ = 0;
            telnetState_ = TelnetState::Text;
            break;
        case TelnetState::Sb:
            if (b == 0xFF) telnetState_ = TelnetState::SbIac; // maybe IAC SE
            // else: subnegotiation payload — dropped
            break;
        case TelnetState::SbIac:
            if (b == 0xF0)      telnetState_ = TelnetState::Text; // IAC SE → end SB
            else                telnetState_ = TelnetState::Sb;   // IAC IAC / nested cmd → stay in SB
            break;
        }
    }
    return w;
}

SuperSerialCard::SuperSerialCard(int slotNum)
    : slot(slotNum)
{
    buildRom();
}

SuperSerialCard::~SuperSerialCard()
{
    stopListening();
}

bool SuperSerialCard::startListening(uint16_t newPort)
{
    // The card does not know how to MAKE a transport — that would be a device
    // reaching into the runtime for a socket and a thread, which the
    // configure-time layer guard rejects. Whoever plugs the card gives it one
    // (MainWindow, pom2_headless), and a card with none simply has nothing on
    // the far end: the ACIA still works, PR#n output still fills the TX
    // counter, there is just no peer.
    if (!transport_) {
        port = newPort;
        return false;
    }
    const bool ok = transport_->start(newPort);
    port = transport_->port();
    listening = transport_->isListening();
    return ok;
}

void SuperSerialCard::stopListening()
{
    if (!transport_) { listening = false; return; }
    transport_->stop();
    listening = transport_->isListening();
    connected = false;
}

void SuperSerialCard::setTransport(std::unique_ptr<pom2::SuperSerialTransport> t)
{
    // Replacing a live transport must not leave its thread running.
    if (transport_) transport_->stop();
    transport_ = std::move(t);
    listening = transport_ && transport_->isListening();
}

// ── Transport hooks ─────────────────────────────────────────────────────
//
// Moved verbatim out of runWorker(): the filtering, the delivery fan-out and
// the paced drain are card behaviour, and they stayed inside the worker only
// because the worker was a card member. Every lock they take is the card's.

size_t SuperSerialCard::processTransportTextRx(uint8_t* data, size_t n)
{
    n = processTelnetRx(data, n);
    return normalizeLineEndings(data, n, telnetPrevCR_);
}

void SuperSerialCard::deliverTransportBytes(const uint8_t* data, size_t n,
                                            bool textMode)
{
    if (n == 0) return;
    deliverRxBytes(data, n);
    if (!textMode) return;

    // Copy the sink under the lock and call it outside: it is host-supplied
    // (the paste / keyboard path) and must not run with bufferMtx held.
    std::function<void(uint8_t)> sink;
    {
        std::lock_guard<std::mutex> lk(bufferMtx);
        sink = keyboardSink;
    }
    if (!sink) return;
    for (size_t i = 0; i < n; ++i) sink(data[i]);
}

size_t SuperSerialCard::drainTransportTx(std::vector<uint8_t>& out)
{
    const bool raw = rawMode_.load(std::memory_order_relaxed);
    size_t taken = 0;

    std::lock_guard<std::mutex> lk(bufferMtx);

    // Telnet option answers first, VERBATIM — they are protocol, not guest
    // data, so they must not be IAC-escaped, and they must not wait behind a
    // rate-limited TX ring either: a peer still negotiating has nothing to
    // send us until it has our answer.
    if (!telnetReply_.empty() && !raw) {
        out.insert(out.end(), telnetReply_.begin(), telnetReply_.end());
        telnetReply_.clear();
    }

    const auto now = std::chrono::steady_clock::now();
    if (bytesPerSecond_ > 0.0) {
        // Credit accrues with wall time and is capped at one buffer, so a
        // long pause cannot burst the whole ring onto the wire at once — the
        // emulated line has a speed and the far end should see it.
        const double dt =
            std::chrono::duration<double>(now - lastDrainTime_).count();
        sendBudget_ += dt * bytesPerSecond_;
        if (sendBudget_ > static_cast<double>(kBufCap))
            sendBudget_ = static_cast<double>(kBufCap);
        const size_t take = static_cast<size_t>(sendBudget_);
        while (taken < take && !txBuf.empty()) {
            const uint8_t b = txBuf.front();
            txBuf.pop_front();
            if (raw) out.push_back(b);
            else     appendTelnetTxEscaped(out, b);
            ++taken;
        }
        sendBudget_ -= static_cast<double>(taken);
    } else if (!txBuf.empty()) {
        // Unthrottled: the guest asked for no rate limit.
        out.reserve(out.size() + txBuf.size());
        while (!txBuf.empty()) {
            const uint8_t b = txBuf.front();
            txBuf.pop_front();
            if (raw) out.push_back(b);
            else     appendTelnetTxEscaped(out, b);
            ++taken;
        }
    }
    lastDrainTime_ = now;
    return taken;
}

void SuperSerialCard::onTransportConnected()
{
    resetTelnet();   // fresh IAC + CR state per connection
    connected = true;
    onConnectionEdge(true);
}

void SuperSerialCard::onTransportDisconnected()
{
    connected = false;
    onConnectionEdge(false);
}

void SuperSerialCard::deliverRxBytes(const uint8_t* data, size_t n)
{
    if (n == 0) return;
    bool armRxIrq = false;
    bool echoLoopback = false;
    {
        std::lock_guard<std::mutex> lk(bufferMtx);
        for (size_t i = 0; i < n; ++i) {
            // MAME `mos6551.cpp:542-543`: SR_RDRF still set on the next
            // byte's arrival → set SR_OVERRUN. We model the same on ring
            // overflow — a host driver that hasn't drained rxBuf loses
            // the oldest unread byte and gets OVERRUN flagged on its
            // next status read. Critical for Kermit-CRC / XMODEM-CRC
            // retransmit logic.
            if (rxBuf.size() >= kBufCap) {
                rxBuf.pop_front();
                statusErrors_ |= SR_OVERRUN;
            }
            rxBuf.push_back(data[i]);
            rxTail.push_back(data[i]);
            if (rxTail.size() > kTailCap) rxTail.pop_front();
        }
        armRxIrq = rxIrqEnable_;
        echoLoopback = echoMode_;
        // MAME `mos6551.cpp:584-594`: REM=1 routes the RX line to TX
        // (unless OVERRUN is set, in which case the line idles high).
        // POM2 doesn't have bit-time accuracy, so the byte-level
        // equivalent is to push each received byte straight into txBuf.
        if (echoLoopback && !(statusErrors_ & SR_OVERRUN)) {
            for (size_t i = 0; i < n; ++i) {
                if (txBuf.size() >= kBufCap) txBuf.pop_front();
                txBuf.push_back(data[i]);
                txTail.push_back(data[i]);
                if (txTail.size() > kTailCap) txTail.pop_front();
                ++txCount;
            }
        }
        if (armRxIrq) raiseIrqSource(IRQ_RDRF);
    }
    rxCount += n;
}

size_t SuperSerialCard::rxQueueDepth() const
{
    std::lock_guard<std::mutex> lk(bufferMtx);
    return rxBuf.size();
}

size_t SuperSerialCard::txQueueDepth() const
{
    std::lock_guard<std::mutex> lk(bufferMtx);
    return txBuf.size();
}

void SuperSerialCard::onReset()
{
    std::lock_guard<std::mutex> lk(bufferMtx);
    txBuf.clear();
    rxBuf.clear();
    statusErrors_ = 0;
    sendBudget_   = 0.0;
    lastDrainTime_ = std::chrono::steady_clock::now();
    // Full hardware reset (Ctrl-Reset). MAME `mos6551.cpp:117-134`:
    //   write_command(0); write_control(0); m_irq_state = 0;
    // applyCommandReg(0) will lower DTR (→ disable both IRQs), force MARK,
    // and clear any pending IRQ source.
    applyCommandReg(0);
    applyControlReg(0);
    irqState_ = 0;
    pushIrqLine();
}

void SuperSerialCard::onUnplug()
{
    // Drop this slot's contribution to the wire-OR IRQ so the
    // aggregator doesn't keep the bit stuck once the card vanishes.
    irqState_ = 0;
    pushIrqLine();
}

void SuperSerialCard::applyCommandReg(uint8_t v)
{
    // MAME `mos6551.cpp:286-323`. `m_dtr` in MAME tracks the inverted pin
    // (output_dtr(!(cmd&1))), so dtrAsserted_ here = (cmd & 1) — the
    // "device ready, please interrupt me" state.
    cmdReg = v;
    const bool prevDtr = dtrAsserted_;
    dtrAsserted_ = (v & 0x01) != 0;
    rxIrqEnable_ = !((v >> 1) & 1) && dtrAsserted_;
    const int txCtl = (v >> 2) & 0x3;
    const bool txIrqEnable = kTxIrqEnableByCmd[txCtl] && dtrAsserted_;
    txIrqEnable_ = txIrqEnable;
    echoMode_ = (v & 0x10) != 0;

    // Pending-IRQ cleanup when an enable bit goes off, MAME
    // `mos6551.cpp:293-307`.
    if (!rxIrqEnable_ && (irqState_ & IRQ_RDRF)) {
        irqState_ &= ~uint8_t{IRQ_RDRF};
    }
    if (!txIrqEnable && (irqState_ & IRQ_TDRE)) {
        irqState_ &= ~uint8_t{IRQ_TDRE};
    }
    // ...and the other way round. TDRE is PINNED HIGH in this model (the host
    // side buffers TX, so the transmitter is always empty), and on a 6551 the
    // transmit interrupt is level-driven off that bit: arming it while the
    // transmitter is already empty raises the interrupt immediately (MAME
    // `mos6551.cpp:293-307`, `update_irq` re-evaluates on every command
    // write). That immediate one is what a driver written as "enable the TX
    // interrupt, then WFI until the ISR feeds the next byte" depends on —
    // without it there is no first interrupt and it never starts.
    if (txIrqEnable && !(irqState_ & IRQ_TDRE)) {
        irqState_ |= uint8_t{IRQ_TDRE};
    }

    // DTR transitions: MAME `mos6551.cpp:317-321` — when DTR is
    // de-asserted (m_dtr=1, i.e. cmd bit 0 == 0 here), force TX MARK +
    // output_txd(1). For POM2 that means dropping the pending TX buffer
    // and the rate-limit budget — the line is held high, no bytes go
    // out. Going the other way (de-assert → assert) doesn't auto-resume:
    // the driver writes new bytes to TDR which we'll then send.
    if (prevDtr && !dtrAsserted_) {
        txBuf.clear();
        sendBudget_ = 0.0;
    }
    pushIrqLine();
}

void SuperSerialCard::applyControlReg(uint8_t v)
{
    // MAME `mos6551.cpp:271-285`. We don't model rx/tx clock direction
    // (bit 4) since POM2 has no external clock source; word length and
    // extra-stop are stored but not enforced on TX (a TX bit-clip would
    // break the 8-bit-clean policy we already documented in the previous
    // bit-7-strip removal); only the baud-rate divider drives behaviour.
    ctlReg = v;
    baudIndex_       = v & 0x0F;
    wordLength_      = static_cast<uint8_t>(8 - ((v >> 5) & 0x3));
    extraStop_       = (v & 0x80) != 0;
    bytesPerSecond_  = baudIndexToBytesPerSec(baudIndex_);
    sendBudget_      = 0.0;
    lastDrainTime_   = std::chrono::steady_clock::now();
}

void SuperSerialCard::applyProgrammedReset()
{
    // MAME `mos6551.cpp:264-270`. Programmed reset clears OVERRUN +
    // DCD/DSR IRQ sources, then write_command(cmd & ~0x1F) — preserves
    // parity bits 5-7. The previous POM2 code wiped cmdReg entirely,
    // leaving any parity config in undefined state.
    statusErrors_ &= ~uint8_t{SR_OVERRUN};
    irqState_ &= ~uint8_t{IRQ_DCD | IRQ_DSR};
    applyCommandReg(cmdReg & ~uint8_t{0x1F});
}

void SuperSerialCard::onConnectionEdge(bool nowConnected)
{
    std::lock_guard<std::mutex> lk(bufferMtx);
    if (prevConnected_ == nowConnected) return;
    prevConnected_ = nowConnected;
    // MAME `mos6551.cpp:443-461`: a DCD or DSR pin change toggles the
    // matching status bit and raises IRQ_DCD/IRQ_DSR — but only when DTR
    // is asserted. ProTERM, MODEM.MGR and similar carrier-aware drivers
    // arm DTR before watching for IRQ-driven connect/disconnect notices;
    // a plain `IN#2 / PR#2` listing run leaves DTR low and won't be
    // woken by us.
    if (dtrAsserted_) {
        raiseIrqSource(IRQ_DCD | IRQ_DSR);
    }
}

void SuperSerialCard::raiseIrqSource(uint8_t mask)
{
    // Worker-thread path (RX arrival, connection edge). Update the source
    // bits atomically but DON'T touch the CPU IRQ line here — assertIrq()
    // mutates non-atomic SlotPeripheral state and must run on the CPU
    // thread. advanceCycles() (CPU thread) applies it within a frame.
    irqState_ |= mask;
    irqLineDirty_.store(true, std::memory_order_release);
}

void SuperSerialCard::clearIrqSource(uint8_t mask)
{
    // CPU-thread path (guest read of status/data clears the source) — apply
    // the line immediately so the clear is responsive.
    irqState_ &= ~mask;
    pushIrqLine();
}

void SuperSerialCard::advanceCycles(int /*cycles*/)
{
    // Drive the CPU IRQ line from a worker-thread-pending change. Runs on the
    // CPU thread (Memory::advanceCycles → SlotBus fan-out), so assertIrq() is
    // CPU-thread-only across the whole card.
    if (irqLineDirty_.exchange(false, std::memory_order_acquire)) {
        pushIrqLine();
    }
}

void SuperSerialCard::pushIrqLine()
{
    // Edge debounce + slot routing live in SlotPeripheral::assertIrq; this
    // method maps the mask down to a boolean line level — and applies SW2-6,
    // which on real hardware sits between the 6551's IRQ output and the
    // slot's IRQ pin. With it OFF the ACIA still raises its internal sources
    // and still shows them in the status register's bit 7; what does not
    // happen is the CPU interrupt. Modelling the source but not the gate made
    // a card configured for polling behave like one configured for
    // interrupts. MAME `a2ssc.cpp:373`.
    assertIrq(irqState_ != 0 && irqDipEnabled());
}

void SuperSerialCard::setIrqDipEnabled(bool on)
{
    if (on) lastDip2 |=  DSW2_IRQ_ENABLE;
    else    lastDip2 &= ~DSW2_IRQ_ENABLE;
    // Flipping the switch takes effect on the line immediately, exactly as
    // moving it on a powered card does — the 6551's own state is untouched.
    pushIrqLine();
}

uint8_t SuperSerialCard::slotRomRead(uint8_t low8)
{
    return rom[low8];
}

uint8_t SuperSerialCard::deviceSelectRead(uint8_t low4)
{
    // Address decode (MAME `a2ssc.cpp:339-353`):
    //   bit 3 set ($C0n8-$C0nF) → ACIA, with **A0-A1 only** decoded
    //     so $C0nC-$C0nF mirror $C0n8-$C0nB.
    //   bit 3 clear ($C0n0-$C0n7) → 74LS259 DIP-switch reads, with bits
    //     1/0 selecting which DSW to AND-mask: bit 1 clear ⇒ AND DSW1,
    //     bit 0 clear ⇒ AND DSW2 (so $C0n0 returns DSW1 & DSW2,
    //     $C0n1 returns DSW1, $C0n2 returns DSW2, $C0n3 returns 0xFF).
    if (low4 & 0x08) {
        const uint8_t reg = low4 & 0x03;
        switch (reg) {
            case 0x0: {  // RDR (data register)
                std::lock_guard<std::mutex> lk(bufferMtx);
                uint8_t b = 0;
                if (!rxBuf.empty()) {
                    b = rxBuf.front();
                    rxBuf.pop_front();
                }
                // MAME `mos6551.cpp:231-236`: read of RDR clears
                // PARITY_ERROR / FRAMING_ERROR / OVERRUN / RDRF. RDRF
                // here is computed from `rxBuf.empty()` at read time so
                // it clears for free; the sticky error flags need an
                // explicit reset. Also drop IRQ_RDRF so a polled driver
                // that *doesn't* read the status register (rare but
                // legal — the SSC spec lets you arrive directly at RDR
                // once you know a byte is there) still releases the
                // line. Mirror of MAME drop-on-RDR via `update_irq`.
                statusErrors_ &= ~uint8_t{SR_PARITY_ERROR |
                                          SR_FRAMING_ERROR |
                                          SR_OVERRUN};
                clearIrqSource(IRQ_RDRF);
                // Re-arm while bytes remain queued. Real hardware has a
                // ONE-byte RDR, so MAME's mos6551 raises IRQ_RDRF for
                // every byte the receiver assembles (mos6551.cpp:668-672)
                // and "queue" cannot desynchronise from "IRQ". POM2 holds
                // a 4 KB host-side ring and used to raise the IRQ once per
                // delivered TCP chunk, so an interrupt-driven guest driver
                // (ProTERM / MODEM.MGR / GS-OS class) got ONE interrupt,
                // read one byte, and never woke again — the rest of the
                // chunk sat unread until the next chunk arrived. Every
                // re-arm consumes a byte, so this cannot storm.
                if (!rxBuf.empty() && rxIrqEnable_) raiseIrqSource(IRQ_RDRF);
                return b;
            }
            case 0x1: {  // status register
                std::lock_guard<std::mutex> lk(bufferMtx);
                uint8_t s = SR_TDRE;                   // TCP buffers TX
                s |= statusErrors_;
                if (!rxBuf.empty()) s |= SR_RDRF;
                // DCD/DSR are ACTIVE-LOW pins: the status BIT is set when
                // the line is INACTIVE (no carrier / not ready). MAME
                // mos6551.cpp:37-39 inits `m_dsr(1), m_dcd(1)` and
                // device_reset sets both status bits; AppleWin is explicit
                // ("DSR is active low (see SY6551 datasheet)",
                // SerialComms.cpp:864 — it returns ST_DSR|ST_DCD when
                // nothing is attached). POM2 had the sense inverted, so a
                // carrier-aware guest saw "online" with an idle listener
                // and "NO CARRIER" the instant a client connected.
                //
                // "Nothing is attached" is the operative phrase: an
                // ImageWriter cabled to the port IS a DCE sitting there
                // with its lines up, and a printer has no carrier to
                // lose. The //c has no physical slots, so its built-in
                // printer port (slot 1) is the only route to the
                // ImageWriter — and its firmware gates every character on
                // this exact status read, spinning until DCD reads
                // active. Reporting "no carrier" at an armed printer tap
                // therefore hangs the guest on `PR#1` and no byte ever
                // reaches the spool. deviceAttached() is what the pins
                // answer to: a telnet peer OR a tapped printer.
                if (!deviceAttached()) s |= (SR_DCD | SR_DSR);
                if (irqState_ != 0) s |= SR_IRQ;
                // MAME `mos6551.cpp:237-250`: status read clears
                // `m_irq_state` and re-evaluates the line. Without this,
                // every IRQ-driven driver (ProTERM, MODEM.MGR, GS/OS
                // SerialPort) spins forever waiting for the ACK.
                clearIrqSource(0xFF);
                return s;
            }
            case 0x2: return cmdReg;
            case 0x3: return ctlReg;
        }
    }
    // DIP-switch readback ($C0n0-$C0n7).
    uint8_t result = 0xFF;
    if (!(low4 & 0x02)) result &= lastDip1;
    if (!(low4 & 0x01)) result &= lastDip2;
    return result;
}

void SuperSerialCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    // Only $C0n8-$C0nF carries the ACIA registers (A0-A1 mirror); writes
    // to $C0n0-$C0n7 hit the 74LS259 latch which we don't model.
    if (!(low4 & 0x08)) return;
    const uint8_t reg = low4 & 0x03;
    std::lock_guard<std::mutex> lk(bufferMtx);
    switch (reg) {
        case 0x0: {  // TDR (data register) — push to TX queue.
            // Real 6551 transmits all 8 bits up to `m_wordlength`.
            // Previously POM2 stripped bit 7 unconditionally, which
            // broke any 8-bit-clean transfer (XMODEM, YMODEM, ZMODEM,
            // Kermit-binary, BinSCII, ADTPro upload). The bit-7 strip
            // is a *terminal* policy, not a UART one — let the host
            // terminal decide via telnet binary mode if it wants 7-bit.
            //
            // DTR de-asserted (cmd bit 0 == 0) parks the transmitter at
            // MARK in MAME (`mos6551.cpp:317-321`) — drop the byte on
            // the floor rather than queue it for a future re-assert.
            if (!dtrAsserted_) break;
            if (txBuf.size() >= kBufCap) txBuf.pop_front();
            txBuf.push_back(v);
            txTail.push_back(v);
            if (txTail.size() > kTailCap) txTail.pop_front();
            ++txCount;
            // The byte is accepted and the transmitter is empty again the
            // instant it is — the host side buffers TX, which is why SR_TDRE
            // is pinned high in the status read. On a 6551 that transition is
            // exactly what raises the transmit interrupt (MAME
            // `mos6551.cpp:668-672` does the same for the receiver). It used
            // to be computed and thrown away, so a driver programmed with
            // command $05 (DTR on, RX IRQ on, TX IRQ on) wrote one byte,
            // slept waiting for the ISR to ask for the next, and never woke.
            if (txIrqEnable_) raiseIrqSource(IRQ_TDRE);
            // Printer tap: mirror the accepted byte into the host-visible
            // spool the ImageWriter drains (see setPrinterTap in the header).
            // The spool is capped: the drain cursor speaks absolute offsets
            // (printerSpoolBase_ + index), so trimming the consumed prefix
            // never desynchronises the consumer. Uncapped, a runaway guest
            // print loop grew this vector without bound (the tx ring above
            // is capped; this one wasn't).
            if (printerTap_) {
                printerSpool_.push_back(v);
                constexpr size_t kSpoolCap = 1u << 20;
                if (printerSpool_.size() > kSpoolCap) {
                    const size_t drop = kSpoolCap / 2;
                    printerSpool_.erase(
                        printerSpool_.begin(),
                        printerSpool_.begin() + static_cast<std::ptrdiff_t>(drop));
                    printerSpoolBase_ += drop;
                    // Half a megabyte just fell out of the middle of a
                    // printout. That has to leave a mark somewhere: it is
                    // silent data loss otherwise, and the paper only shows
                    // a job that stops mid-sentence. Logged once per
                    // session — a guest that trips this once will trip it
                    // repeatedly, and a log storm helps nobody.
                    if (!printerSpoolTrimWarned_) {
                        printerSpoolTrimWarned_ = true;
                        pom2::log().warn("SSC",
                            "printer spool hit its 1 MiB cap — dropping the "
                            "oldest " + std::to_string(drop / 1024) +
                            " KiB. The ImageWriter is falling behind the "
                            "guest; the printout will have a gap.");
                    }
                }
            }
            break;
        }
        case 0x1: applyProgrammedReset(); break;
        case 0x2: applyCommandReg(v);     break;
        case 0x3: applyControlReg(v);     break;
    }
}

void SuperSerialCard::setPrinterTap(bool on)
{
    std::lock_guard<std::mutex> lk(bufferMtx);
    printerTap_ = on;
}

bool SuperSerialCard::printerTap() const
{
    std::lock_guard<std::mutex> lk(bufferMtx);
    return printerTap_;
}

size_t SuperSerialCard::drainPrinterSpoolFrom(size_t from,
                                              std::vector<uint8_t>& out) const
{
    std::lock_guard<std::mutex> lk(bufferMtx);
    // Offsets are absolute (bytes ever spooled), so the cap's front-trim
    // keeps the count monotonic. Same resync rule as
    // PrinterCard::drainSpoolFrom: `from` past the end means the spool was
    // cleared behind the caller's back — hand back everything so the
    // consumer resynchronises instead of going deaf. `from` below the base
    // means the cap already trimmed bytes the caller never drained (host
    // fell > 1 MiB behind): resume at the oldest byte still held.
    const size_t total = printerSpoolBase_ + printerSpool_.size();
    size_t start = (from > total) ? printerSpoolBase_
                                  : std::max(from, printerSpoolBase_);
    start -= printerSpoolBase_;
    out.insert(out.end(),
               printerSpool_.begin() + static_cast<std::ptrdiff_t>(start),
               printerSpool_.end());
    return total;
}

size_t SuperSerialCard::printerSpoolBytes() const
{
    std::lock_guard<std::mutex> lk(bufferMtx);
    return printerSpoolBase_ + printerSpool_.size();
}

void SuperSerialCard::clearPrinterSpool()
{
    std::lock_guard<std::mutex> lk(bufferMtx);
    printerSpool_.clear();
    printerSpoolBase_ = 0;
}

std::string SuperSerialCard::recentTxText() const
{
    std::lock_guard<std::mutex> lk(bufferMtx);
    std::string out;
    out.reserve(txTail.size());
    for (uint8_t b : txTail) {
        const uint8_t c = b & 0x7F;
        out.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) :
                      (c == '\r' || c == '\n') ? '\n' : '.');
    }
    return out;
}

std::string SuperSerialCard::recentRxText() const
{
    std::lock_guard<std::mutex> lk(bufferMtx);
    std::string out;
    out.reserve(rxTail.size());
    for (uint8_t b : rxTail) {
        out.push_back((b >= 0x20 && b < 0x7F) ? static_cast<char>(b) :
                      (b == '\r' || b == '\n') ? '\n' : '.');
    }
    return out;
}

void SuperSerialCard::buildRom()
{
    rom.fill(0xEA);     // NOP padding

    const uint8_t devLo      = static_cast<uint8_t>(0x80 + slot * 16);
    const uint8_t statusReg  = static_cast<uint8_t>(devLo + 0x9);
    const uint8_t dataReg    = static_cast<uint8_t>(devLo + 0x8);
    const uint8_t cmdRegAddr = static_cast<uint8_t>(devLo + 0xA);

    pom2::SlotRomAsm a(rom, slot, "SuperSerialCard");

    // Apple-II PR#n / IN#n auto-config protocol. The autodetect bytes sit at
    // addresses the platform mandates ($Cn05, $Cn07, $Cn0B, $Cn0C), so
    // execution is routed AROUND them with a JMP rather than through them.
    //   $Cn05 = $38   (sig 1)
    //   $Cn07 = $18   (sig 2)
    //   $Cn0B = $01   (firmware revision)
    //   $Cn0C = $31   (device class: serial-port aka "Communications")
    a.region("prEntry", 0x00, 0x05).jmp("prBind");

    // IN#n entry at $Cn08. Apple BASIC calls $Cn00 for both PR# and IN# but
    // distinguishes by zero-page contents; many ROMs publish IN#n at $Cn00+8.
    a.region("inEntry", 0x08, 0x0B).jmp("inBind");

    a.region("pascalSig", 0x05, 0x08).poke(0x05, 0x38).poke(0x07, 0x18);

    // ── Pascal 1.1 firmware protocol entry block ─────────────────────────
    // Apple II Pascal recognises a Pascal-1.1 card by the ID bytes above,
    // then dispatches through four single-byte entry OFFSETS at $Cn0D-$Cn10
    // (each the LOW byte of a routine in this page; the interpreter forms
    // $Cn00|offset). Layout + calling convention verified against the real
    // SSC ROM disassembly (6502disassembly.com/a2-rom/SSC):
    //   PINIT   X = error (0 = OK)
    //   PREAD   returns char in A, high bit cleared
    //   PWRITE  char to send in A
    //   PSTATUS A=0 → "ready for output?", A=1 → "input available?";
    //           carry SET = ready, X = 0.
    // Without this block POM2's SSC published the ID bytes but no entry
    // table, so a Pascal program detected the card then jumped into NOP fill.
    //
    // These four bytes are why the page is assembled rather than typed. They
    // are addresses of routines defined LOWER DOWN this function, and each
    // used to be a literal repeated at both ends; a routine that moved left
    // the interpreter dispatching into the middle of an instruction, with
    // nothing to notice it.
    a.region("pascalId", 0x0B, 0x0D)
     .poke(0x0B, 0x01)      // generic Pascal 1.1 signature
     .poke(0x0C, 0x31);     // device signature (comms, class 3)

    a.region("pascalTable", 0x0D, 0x11)
     .byteOf("pinit").byteOf("pread").byteOf("pwrite").byteOf("pstatus");

    // PR#n bind — initialise the ACIA, then patch CSWL/CSWH to point at the
    // output routine and RTS so the BASIC interpreter resumes.
    //
    // The ACIA init (cmd=$0B: DTR asserted, RX IRQ off, RTS low) mirrors what
    // the real SSC firmware does on first entry — it programs the 6551 from
    // the DIP switches before any I/O (the BASICINIT path in the real ROM
    // disassembly). Without it a plain `PR#n : PRINT` writes the TDR with DTR
    // de-asserted and the transmitter (correctly, per MAME
    // `mos6551.cpp:317-321`) drops every byte on the floor — only Pascal,
    // whose PINIT does the same $0B write, could ever transmit.
    a.region("prBind", 0x20, 0x40)
     .emit({ 0xA9, 0x0B,             // LDA #$0B    (DTR on, RX IRQ off)
             0x8D, cmdRegAddr, 0xC0, // STA $C0nA   (command register)
             0xA9 }).byteOf("cout")  // LDA #<cout
     .emit({ 0x85, 0x36,             // STA $36   (CSWL)
             0xA9, a.pageHi(),       // LDA #>cout
             0x85, 0x37,             // STA $37   (CSWH)
             0x60 });                // RTS

    // IN#n bind — same ACIA init, then patch KSWL/KSWH (input vector).
    a.region("inBind", 0x40, 0x50)
     .emit({ 0xA9, 0x0B,
             0x8D, cmdRegAddr, 0xC0,
             0xA9 }).byteOf("cin")   // LDA #<cin
     .emit({ 0x85, 0x38,             // STA $38   (KSWL)
             0xA9, a.pageHi(),
             0x85, 0x39,             // STA $39   (KSWH)
             0x60 });

    // PINIT — assert DTR + RTS-low/TX-IRQ-off (cmd=$0B) so the port can
    // transmit, then return success (X=0).
    a.region("pinit", 0x50, 0x60)
     .emit({ 0xA9, 0x0B,             // LDA #$0B
             0x8D, cmdRegAddr, 0xC0, // STA $C0nA   (command register)
             0xA2, 0x00,             // LDX #$00    (no error)
             0x60 });                // RTS

    // PREAD — spin until RDRF, return the byte in A with the high bit
    // cleared (Pascal wants 7-bit ASCII), X=0.
    a.region("pread", 0x60, 0x70)
     .label("preadWait")
     .emit({ 0xAD, statusReg, 0xC0,  // LDA $C0n9
             0x29, 0x08 })           // AND #$08  (RDRF)
     .branch(0xF0, "preadWait")      // BEQ preadWait
     .emit({ 0xAD, dataReg, 0xC0,    // LDA $C0n8
             0x29, 0x7F,             // AND #$7F  (strip high bit)
             0xA2, 0x00,             // LDX #$00
             0x60 });                // RTS

    // PWRITE — spin until TDRE, send the char in A, X=0.
    a.region("pwrite", 0x70, 0x80)
     .emit({ 0x48 })                 // PHA
     .label("pwriteWait")
     .emit({ 0xAD, statusReg, 0xC0,  // LDA $C0n9
             0x29, 0x10 })           // AND #$10  (TDRE)
     .branch(0xF0, "pwriteWait")     // BEQ pwriteWait
     .emit({ 0x68,                   // PLA
             0x8D, dataReg, 0xC0,    // STA $C0n8
             0xA2, 0x00,             // LDX #$00
             0x60 });                // RTS

    // PSTATUS — A=0 → output-ready (TDRE), A=1 → input-avail (RDRF).
    // LSR moves the request code into carry; CMP #$01 maps "masked bit set"
    // back into the carry flag (ready). X=0.
    a.region("pstatus", 0x80, 0xB0)
     .emit({ 0x4A,                   // LSR A       (C=0 output, C=1 input)
             0xAD, statusReg, 0xC0 })// LDA $C0n9
     .branch(0xB0, "psInput")        // BCS psInput
     .emit({ 0x29, 0x10 })           // AND #$10    (TDRE)
     .jmp("psTest")
     .label("psInput").emit({ 0x29, 0x08 })   // AND #$08  (RDRF)
     .label("psTest").emit({ 0xC9, 0x01,      // CMP #$01  (A!=0 → C = ready)
                             0xA2, 0x00,      // LDX #$00
                             0x60 });         // RTS

    // Output routine — Apple monitor convention: the byte to write is in A
    // (high bit set per the OUT vector spec). PHA once, spin on TDRE (the
    // branch goes back to the LDA *after* the PHA so we don't push a stack
    // frame per iteration), then PLA + write.
    a.region("cout", 0xB0, 0xE0)
     .emit({ 0x48 })                 // PHA
     .label("coutWait")
     .emit({ 0xAD, statusReg, 0xC0,  // LDA $C0n9
             0x29, 0x10 })           // AND #$10  (TDRE)
     .branch(0xF0, "coutWait")       // BEQ coutWait
     .emit({ 0x68,                   // PLA
             0x8D, dataReg, 0xC0,    // STA $C0n8
             0x60 });                // RTS

    // Input routine — spin until RDRF, return the byte in A with bit 7 set
    // (Apple keyboard convention).
    a.region("cin", 0xE0, pom2::kSlotRomBytes)
     .label("cinWait")
     .emit({ 0xAD, statusReg, 0xC0,  // LDA $C0n9
             0x29, 0x08 })           // AND #$08  (RDRF)
     .branch(0xF0, "cinWait")        // BEQ cinWait
     .emit({ 0xAD, dataReg, 0xC0,    // LDA $C0n8
             0x09, 0x80,             // ORA #$80  (Apple keys are high-bit-set)
             0x60 });                // RTS

    romLayoutError_ = !a.finish();
}

// ── Snapshot / rewind ─────────────────────────────────────────────────────
// Only the ACIA's guest-visible register state travels. The TCP socket,
// the RX/TX rings and the printer spool are host-side: a rewind cannot
// un-send bytes that already left the socket, and re-delivering buffered
// input would duplicate it. Pre-fix, none of this was serialized at all,
// so a restored machine kept the live card's baud/DTR/IRQ configuration.

namespace {
constexpr uint8_t kSscSnapMagic[4] = { 'S', 'S', 'C', '1' };
}

void SuperSerialCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    std::lock_guard<std::mutex> lk(bufferMtx);
    out.insert(out.end(), kSscSnapMagic, kSscSnapMagic + 4);
    out.push_back(cmdReg);
    out.push_back(ctlReg);
    out.push_back(statusErrors_);
    out.push_back(dtrAsserted_ ? 1 : 0);
    out.push_back(rxIrqEnable_ ? 1 : 0);
    out.push_back(echoMode_    ? 1 : 0);
    out.push_back(wordLength_);
    out.push_back(extraStop_ ? 1 : 0);
    out.push_back(baudIndex_);
    out.push_back(irqState_.load());
}

void SuperSerialCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (data == nullptr || len < 14 ||
        std::memcmp(data, kSscSnapMagic, 4) != 0)
        return;   // foreign blob — a different card sat in this slot
    std::lock_guard<std::mutex> lk(bufferMtx);
    size_t p = 4;
    cmdReg        = data[p++];
    ctlReg        = data[p++];
    // A snapshot is a FILE, so every restored field is untrusted input.
    // `statusErrors_` is the STICKY-ERROR half of the status register and
    // nothing else — the other bits (RDRF, TDRE, DCD, DSR, IRQ) are computed
    // at read time and then OR'd with this byte, so a blob carrying $FF here
    // pinned DCD and DSR high for the rest of the session: a carrier-aware
    // driver reads "NO CARRIER" with a live telnet peer attached, forever,
    // and no register write can clear it (only a read of RDR clears these,
    // and it clears only the three bits below). Keep the three that are
    // actually sticky. MAME `mos6551.cpp:234`.
    statusErrors_ = static_cast<uint8_t>(
        data[p++] & (SR_PARITY_ERROR | SR_FRAMING_ERROR | SR_OVERRUN));
    dtrAsserted_  = data[p++] != 0;
    rxIrqEnable_  = data[p++] != 0;
    echoMode_     = data[p++] != 0;
    wordLength_   = data[p++];
    extraStop_    = data[p++] != 0;
    baudIndex_    = static_cast<uint8_t>(data[p++] & 0x0F);
    // Same class of guard: the IRQ mask has four defined sources, and a bit
    // outside them can never be cleared by `clearIrqSource` (the read paths
    // clear named bits, and `irqState_ != 0` is what drives SR_IRQ and the
    // slot's IRQ line) — a stuck spurious bit holds the CPU interrupt down.
    irqState_.store(static_cast<uint8_t>(
        data[p++] & (IRQ_DCD | IRQ_DSR | IRQ_RDRF | IRQ_TDRE)));
    irqLineDirty_.store(true);   // CPU thread re-drives the line

    // DERIVED from the restored cmdReg, like rxIrqEnable_ is serialised —
    // without it the transmit-interrupt gate keeps the LIVE session's value.
    txIrqEnable_ = kTxIrqEnableByCmd[(cmdReg >> 2) & 0x3] && dtrAsserted_;

    // `bytesPerSecond_` is DERIVED from baudIndex_, not serialized — the same
    // reason wordLength_/extraStop_ are restored explicitly above. Without
    // this it keeps whatever rate the LIVE session was last programmed to, so
    // a 300-baud snapshot restored into a 19 200-baud session kept draining
    // the TX ring 64x too fast (and vice versa). Only applyControlReg used to
    // compute it, and a snapshot load is not a register write. Reset the
    // pacing budget + drain clock with it, exactly as applyControlReg does:
    // a stale `lastDrainTime_` would credit the restored rate for all the
    // wall-clock time that elapsed before the load and dump a burst.
    bytesPerSecond_ = baudIndexToBytesPerSec(baudIndex_);
    sendBudget_     = 0.0;
    lastDrainTime_  = std::chrono::steady_clock::now();
}
