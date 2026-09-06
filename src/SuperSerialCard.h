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

// SuperSerialCard — Apple Super Serial Card (slot 2 by convention).
// Models a 6551 ACIA register set wired to a TCP listener so a host
// terminal (telnet, screen, minicom-over-tty) can talk to the running
// Apple II/IIe as a serial peer.
//
// Soft-switch map (slot N at $C080+N*16; for slot 2 → $C0A0-$C0AF):
//
//   $C0nA-$C0nB   DIP-switch reads (we report a sane default config)
//   $C0nC-$C0nD   ACIA reset / status mirror
//   $C0nE         Reserved (reads $00)
//   $C0nF         Reserved (reads $00)
//   $C0n8         ACIA data register (read = pop RX byte; write = push TX)
//   $C0n9         ACIA status register
//                   bit 7 = IRQ                     (we hold low)
//                   bit 6 = DSR                     (1 when client connected)
//                   bit 5 = DCD                     (mirror of DSR)
//                   bit 4 = TDRE: TX register empty (always 1 — TCP buffers it)
//                   bit 3 = RDRF: RX register full  (1 when bytes are queued)
//                   bit 0..2 = framing/parity/overrun (always 0)
//
// Slot ROM ($Cs00-$CsFF, s=2 → $C200-$C2FF) advertises the SSC
// auto-detection signature ($Cn05 = $38, $Cn07 = $18, $Cn0B = $01,
// $Cn0C = $31), the Pascal 1.1 firmware-protocol entry table at $Cn0D-$Cn10
// (PINIT/PREAD/PWRITE/PSTATUS routine offsets), and a tiny PR#n / IN#n hook
// that hands character I/O off to the device-select range. Boot from $Cs00
// isn't supported — the SSC was rarely a boot device on real hardware.
//
// TCP bridge: a worker thread listens on 127.0.0.1:`port` (default 6502)
// and accepts at most one client at a time. Bytes flow through two
// 4 KB ring buffers under a mutex. Inbound telnet IAC negotiation is
// silently dropped and line endings normalised so a vanilla `telnet`
// binary connects cleanly without hand-shaking; outbound data is
// telnet-escaped per RFC 854 ($FF → IAC IAC, bare CR → CR NUL). Raw mode
// (`ssc_raw_mode`) bypasses all of that in both directions for 8-bit
// binary protocols.

#ifndef POM2_SUPER_SERIAL_CARD_H
#define POM2_SUPER_SERIAL_CARD_H

#include "SlotPeripheral.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace pom2 { class SuperSerialTransport; }

class SuperSerialCard : public SlotPeripheral
{
public:
    static constexpr int kDefaultSlot = 2;
    static constexpr uint16_t kDefaultPort = 6502;

    /// Construct with the slot number this card will be plugged into.
    /// The slot is baked into the slot ROM (PR#n / IN#n trampolines and
    /// the absolute device-select addresses inside the spin-on-TDRE
    /// output / spin-on-RDRF input routines), so changing it after
    /// construction would require rebuilding the ROM — pass the right
    /// slot up front.
    explicit SuperSerialCard(int slot = kDefaultSlot);
    ~SuperSerialCard() override;

    int getSlot() const { return slot; }

    /// True when the hand-assembled slot ROM did not fit its declared layout.
    bool romLayoutError() const { return romLayoutError_; }

    /// Start listening on 127.0.0.1:port. Returns false if the bind fails;
    /// the card stays plugged but `clientConnected()` will always be false.
    bool startListening(uint16_t port);
    /// Tear down the listener and any active connection. Safe to call from
    /// the UI thread; the worker is joined before returning.
    void stopListening();

    /// Replace the host transport. A card constructed without one creates the
    /// loopback TCP transport on first startListening(); a test injects a
    /// transport that opens no socket and starts no thread.
    void setTransport(std::unique_ptr<pom2::SuperSerialTransport> transport);

    /// Optional "telnet → keyboard" bridge. When set, every byte the
    /// SSC receives over TCP in telnet TEXT mode is *also* forwarded to
    /// this sink (raw mode suppresses the forwarding — binary protocol
    /// payloads must not be typed into the paste queue) (typically
    /// `Memory::queueKey`). That makes the SSC a self-testing console:
    /// the host can telnet in, type `PR#2 / IN#2` directly into BASIC
    /// (no chicken-and-egg of needing IN#N before the keyboard listens
    /// on slot 2), then drive the Apple II via the same telnet socket.
    /// Bytes still land in the ACIA RX queue too, so a real IN#2 path
    /// works once the host has activated it.
    void setKeyboardSink(std::function<void(uint8_t)> sink)
    {
        std::lock_guard<std::mutex> lk(bufferMtx);
        keyboardSink = std::move(sink);
    }

    bool isListening()      const { return listening; }
    bool clientConnected()  const { return connected;  }
    uint16_t getPort()      const { return port;       }
    uint64_t bytesRx()      const { return rxCount;    }
    uint64_t bytesTx()      const { return txCount;    }

    /// Raw-mode toggle: when true, skip telnet IAC ($FF) stripping AND
    /// line-ending normalisation on RX, so 8-bit binary protocols
    /// (XMODEM / Kermit / ADTPro) see every byte verbatim. Default
    /// false (telnet text mode, expected for keyboard / terminal use).
    /// The TCP listener is always raw at the socket level — this flag
    /// only gates POM2's RX-side filtering. Persisted as
    /// `ssc_raw_mode`.
    void setRawMode(bool raw) { rawMode_ = raw; }
    bool rawMode()   const    { return rawMode_; }

    /// Inject bytes as if they had just arrived on the TCP socket. The
    /// path matches the worker thread's: SR_OVERRUN on ring overflow,
    /// RX IRQ raise gated by `rxIrqEnable_`, echo-mode loopback into the
    /// TX queue. Test-only public entry point — production code uses the
    /// worker thread which calls the same method internally.
    void deliverRxBytes(const uint8_t* data, size_t n);

    // ── Transport hooks ──────────────────────────────────────────────────
    // The three calls a host transport makes into the card. They exchange
    // BYTES and connection edges only: the transport owns its socket and its
    // thread, the card owns the rings, the telnet parser and the pacing.
    //
    // Everything a transport needs is here, which is the test that the split
    // is in the right place — a transport that opens no socket at all (a
    // fake, driving the card directly) needs nothing else.

    /// Text-mode inbound filtering: strip telnet IAC negotiation, then
    /// normalise line endings. Returns the surviving byte count, in place.
    /// Raw mode skips this entirely — the point of raw mode is that nothing
    /// touches the stream.
    size_t processTransportTextRx(uint8_t* data, size_t n);

    /// Hand received bytes to the card: into the RX ring, and — in text mode
    /// only — to the keyboard sink. Raw mode deliberately does not reach the
    /// keyboard: a binary transfer is not typing.
    void deliverTransportBytes(const uint8_t* data, size_t n, bool textMode);

    /// Take whatever the guest has queued, honouring the emulated line rate.
    /// Appends to `out` (never clears it) because telnet escaping can EXPAND
    /// the stream — a literal $FF becomes IAC IAC — so a fixed-capacity
    /// buffer would have to be able to fail, and a partial escape sequence on
    /// the wire is a corrupt stream. Returns the number of guest bytes taken.
    size_t drainTransportTx(std::vector<uint8_t>& out);

    /// Connection edges. `resetTelnet()` must run on every new connection:
    /// the IAC parser and the CR state persist across recv() chunks, so a
    /// half-parsed sequence from a dropped client would corrupt the next one.
    void onTransportConnected();
    void onTransportDisconnected();

    /// Printer tap — the //c's real printer port IS this card (slot 1),
    /// so the host-side ImageWriter needs to see the TX byte stream the
    /// way it sees a PrinterCard/GrapplerCard spool. When the tap is on,
    /// every byte the ACIA accepts for transmit (i.e. past the DTR gate)
    /// is *also* appended to a host-visible spool with the exact
    /// `drainSpoolFrom` shape of `PrinterCard::drainSpoolFrom`, which is
    /// what `MainWindow::pumpImageWriter()` consumes. The TCP bridge is
    /// unaffected — tap and telnet can run at once (a serial printer and
    /// a terminal can't share a real port either, but the tap is a
    /// host-side wiretap, not a second DB-25). Persisted per slot as
    /// `ssc_printer_tap_slotN`; defaults ON for slot 1 (the printer-port
    /// convention) and OFF elsewhere.
    void setPrinterTap(bool on);
    bool printerTap() const;
    /// Append every spooled byte at index >= `from` to `out`; returns the
    /// spool's total size (the caller's next `from`). Same contract as
    /// `PrinterCard::drainSpoolFrom`, including the resync-on-clear rule:
    /// `from` past the end hands back the whole spool.
    size_t drainPrinterSpoolFrom(size_t from, std::vector<uint8_t>& out) const;
    size_t printerSpoolBytes() const;
    void   clearPrinterSpool();

    /// Telnet RX line-ending normaliser (drop NUL, CR LF → CR, LF → CR).
    /// Public + static so it is unit-testable in isolation; the RX worker
    /// calls it on every non-raw inbound chunk. Mutates `data` in place,
    /// returns the new length. RFC 854: a bare CR arrives as CR NUL.
    /// `prevCR` is the persistent saw-a-CR state — the CALLER owns it so a
    /// CR LF pair split across two recv() chunks still collapses (the RX
    /// worker passes telnetPrevCR_; a per-call local reset the state at
    /// every ≤256-byte chunk and a split \r|\n delivered CR CR — one
    /// spurious ENTER per straddled line ending).
    static size_t normalizeLineEndings(uint8_t* data, size_t n, bool& prevCR);

    /// Telnet IAC filter — a PERSISTENT state machine (member state) so an
    /// IAC sequence split across recv() chunks, and the variable-length
    /// IAC SB … IAC SE subnegotiation, are handled correctly. Mutates `data`
    /// in place, returns the kept length. Public for unit testing (the RX
    /// worker calls it on every non-raw inbound chunk). Call resetTelnet()
    /// at the start of each connection.
    size_t processTelnetRx(uint8_t* data, size_t n);
    void   resetTelnet()
    {
        telnetState_   = TelnetState::Text;
        telnetPrevCR_  = false;
        telnetCommand_ = 0;
        std::lock_guard<std::mutex> lk(bufferMtx);
        telnetReply_.clear();   // a new peer negotiates from scratch
    }

    /// Answer one WILL/WONT/DO/DONT (RFC 854 requires an answer to every
    /// option request — see the definition for what silence does to a stock
    /// telnet client). BINARY and SGA are accepted, everything else refused.
    /// The reply is queued RAW, ahead of guest data, by drainTransportTx.
    void answerTelnetOption(uint8_t command, uint8_t option);

    /// What the card has queued in answer to telnet negotiation, for tests.
    std::vector<uint8_t> pendingTelnetReply() const
    {
        std::lock_guard<std::mutex> lk(bufferMtx);
        return telnetReply_;
    }

    /// Telnet TX escaping (RFC 854): append `b` to `out`, doubling $FF
    /// (IAC IAC) and following a bare CR with NUL (the Apple II's newline
    /// is a lone CR, which telnet transmits as CR NUL). Applied by the TX
    /// drain only in telnet text mode — raw mode sends bytes verbatim.
    /// Public + static for unit testing.
    static void appendTelnetTxEscaped(std::vector<uint8_t>& out, uint8_t b);

    // Test/debug introspection — read-only reflection of the decoded
    // command/control register state.
    double  bytesPerSecond() const { return bytesPerSecond_; }
    bool    dtrAsserted()    const { return dtrAsserted_;    }
    bool    echoMode()       const { return echoMode_;       }
    bool    rxIrqEnabled()   const { return rxIrqEnable_;    }
    /// SW2-6: does the card's IRQ output reach the slot at all?
    bool    irqDipEnabled()  const { return (lastDip2 & DSW2_IRQ_ENABLE) != 0; }
    void    setIrqDipEnabled(bool on);
    uint8_t statusErrorBits()const { return statusErrors_;   }
    uint8_t irqState()       const { return irqState_;       }
    size_t  rxQueueDepth()   const;
    size_t  txQueueDepth()   const;
    /// Snapshot of "what the Apple II most recently received" (RX) and
    /// "most recently sent over TCP" (TX). Cheap circular ring of the last
    /// ~256 bytes; used by the status panel as a debug peephole.
    std::string recentTxText() const;     // Apple II → telnet
    std::string recentRxText() const;     // telnet  → Apple II

    // ─── SlotPeripheral overrides ────────────────────────────────────────
    std::string_view name() const override { return "Super Serial"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead(uint8_t low8) override;
    void    onReset() override;
    void    onUnplug() override;
    // CPU-thread hook: apply a worker-thread-pending IRQ-line change here so
    // assertIrq() (which mutates non-atomic SlotPeripheral state) is only
    // ever called from the CPU thread. See raiseIrqSource()/pushIrqLine().
    void    advanceCycles(int cycles) override;

    /// Snapshot/rewind: 'SSC1'-tagged blob with the guest-visible ACIA
    /// register state (command/control decode + sticky status errors).
    /// The TCP connection, ring buffers and printer spool are host-side
    /// and deliberately NOT serialized — a rewind cannot un-send bytes
    /// that already left over the socket. Without this the guest's
    /// restored firmware saw whatever baud/DTR/IRQ config the LIVE card
    /// had drifted to.
    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

private:
    int slot;
    std::array<uint8_t, 256> rom{};
    /// Set by buildRom() when a hand-assembled region overran its budget or
    /// stopped ending where the Pascal entry table / branch targets assume.
    bool                     romLayoutError_ = false;
    /// The host bridge. Null until startListening() creates the default TCP
    /// transport, or a test injects one. The card keeps `listening` as a
    /// mirror so the existing ACIA/status paths are unchanged.
    std::unique_ptr<pom2::SuperSerialTransport> transport_;
    std::atomic<bool> listening { false };
    std::atomic<bool> connected { false };
    uint16_t port = kDefaultPort;

    // Persistent telnet IAC parser state (worker thread only). Survives
    // recv() chunk boundaries so a split IAC / SB sequence parses correctly.
    enum class TelnetState { Text, Iac, Opt, Sb, SbIac };
    TelnetState telnetState_ = TelnetState::Text;
    /// CR-seen state for normalizeLineEndings, persistent across recv()
    /// chunks for the same reason telnetState_ is (see resetTelnet()).
    bool telnetPrevCR_ = false;
    /// The WILL/WONT/DO/DONT byte whose option byte has not arrived yet —
    /// persistent because a recv() chunk can end between the two.
    uint8_t telnetCommand_ = 0;
    /// Telnet PROTOCOL bytes owed to the peer (option answers). Kept apart
    /// from txBuf because guest data is IAC-escaped on the way out and these
    /// must not be; drained ahead of it, under `bufferMtx`.
    std::vector<uint8_t> telnetReply_;

    // Atomic so the UI/dtor thread can shutdown() these to wake the worker
    // out of accept()/recv() without a torn read, and so close() happens
    // exactly once (the worker is the sole closer of clientFd). See
    // stopListening()/closeClient().
    // 6551 status-register bit layout (MAME `mos6551.h:53-61`).
    static constexpr uint8_t SR_PARITY_ERROR  = 0x01;
    static constexpr uint8_t SR_FRAMING_ERROR = 0x02;
    static constexpr uint8_t SR_OVERRUN       = 0x04;
    static constexpr uint8_t SR_RDRF          = 0x08;
    static constexpr uint8_t SR_TDRE          = 0x10;
    static constexpr uint8_t SR_DCD           = 0x20;
    static constexpr uint8_t SR_DSR           = 0x40;
    /// SW2-6, the interrupt-enable DIP. On a real Super Serial Card this
    /// switch physically gates the 6551's IRQ output before it reaches the
    /// slot's IRQ pin, so with it OFF an interrupt-driven driver simply never
    /// fires however the ACIA's own command register is programmed — the two
    /// are independent, which is exactly the confusion the switch causes on
    /// real hardware. MAME `a2ssc.cpp:373`.
    ///
    /// POM2 reports the switch in DSW2 already; bit 5 is SW2-6 under the
    /// usual switch-1-is-bit-0 numbering, and the shipped default (0x60) has
    /// it ON, which is why nothing changes for existing configurations.
    static constexpr uint8_t DSW2_IRQ_ENABLE = 0x20;

    static constexpr uint8_t SR_IRQ           = 0x80;

    // 6551 internal IRQ-source mask (MAME `mos6551.h:71-77`). All four
    // sources are generated: RX IRQ, DCD/DSR change, and TDRE — the last one
    // the moment a byte is accepted into TDR, because pinning TDRE high
    // (the host buffers TX) means the transmitter is empty again immediately.
    static constexpr uint8_t IRQ_DCD  = 0x01;
    static constexpr uint8_t IRQ_DSR  = 0x02;
    static constexpr uint8_t IRQ_RDRF = 0x04;
    static constexpr uint8_t IRQ_TDRE = 0x08;

    // ACIA register state.
    uint8_t cmdReg     = 0x00;
    uint8_t ctlReg     = 0x00;
    // Optional latches the ROM may probe but our model doesn't care about.
    uint8_t lastDip1   = 0xA8;     // 19200 8N1, full duplex
    uint8_t lastDip2   = 0x60;     // CR + LF, no echo, SW2-6 interrupts ON

    // Decoded command-register state (mirrors MAME `mos6551.cpp::write_command`).
    // dtrAsserted_  := cmd bit 0 == 1 (real DTR pin pulled low = device ready)
    // rxIrqEnable_  := !cmd[1] && dtrAsserted_  — see MAME `mos6551.cpp:292`
    // echoMode_     := cmd bit 4 — see MAME `mos6551.cpp:309`
    bool dtrAsserted_ = false;
    bool rxIrqEnable_ = false;
    // txIrqEnable_ := kTxIrqEnableByCmd[cmd[3:2]] && dtrAsserted_ — MAME
    // `mos6551.cpp:293-307`. Only cmd[3:2] == 01 enables it. TDRE is pinned
    // high here (the host buffers TX), so this gates a transmit interrupt
    // raised the moment a byte is accepted into TDR, plus the immediate one a
    // command write that ARMS it produces.
    bool txIrqEnable_ = false;
    bool echoMode_    = false;
    // Raw mode: bypass telnet IAC strip + LF/CR normalisation on RX.
    // For 8-bit binary protocols (XMODEM / Kermit / ADTPro). Persisted
    // as `ssc_raw_mode`. Atomic so the TCP worker thread can read it
    // without holding bufferMtx.
    std::atomic<bool> rawMode_{false};

    // Decoded control-register state. Stored for completeness; only the
    // baud-rate index actually drives behaviour (TX drain pacing).
    uint8_t  wordLength_     = 8;
    bool     extraStop_      = false;
    uint8_t  baudIndex_      = 0;       // ctl[3:0]
    double   bytesPerSecond_ = 0.0;     // 0 → unconstrained (16x ext clk)

    // Persistent status flags (RDRF/TDRE/DCD/DSR computed dynamically at
    // read-time, but OVERRUN/FRAMING/PARITY are sticky — cleared only by
    // read of RDR per MAME `mos6551.cpp:234`).
    uint8_t statusErrors_ = 0;

    // IRQ state (MAME-style mask). The pin level pushed to the bus is
    // simply `irqState_ != 0`; edge debouncing is handled by the base
    // class's `assertIrq()` so we don't cache an extra bool here.
    // Atomic: the TCP worker raises IRQ sources while the CPU thread reads
    // status / clears them. (The CPU IRQ *line* is only ever driven from the
    // CPU thread — see raiseIrqSource/advanceCycles.)
    std::atomic<uint8_t> irqState_{0};
    // Set by the worker when it changes irqState_; the CPU thread's
    // advanceCycles() consumes it to drive assertIrq() on the CPU thread.
    std::atomic<bool> irqLineDirty_{false};

    // Connection-edge tracking for DCD/DSR IRQ generation. Both bits move
    // together in this model (SSC + telnet has no separate carrier-vs-DTR
    // signalling), but we keep two IRQ source bits to mirror MAME.
    bool prevConnected_ = false;

    // TX rate-limit accounting (worker thread). `lastDrainTime_` is the
    // last wall-clock at which we replenished `sendBudget_`. Both reset on
    // every control-reg write so a change of baud rate doesn't dump a
    // backlog at once.
    double sendBudget_ = 0.0;
    std::chrono::steady_clock::time_point lastDrainTime_;


    /// Apply a write to the command register: decode DTR/echo/RX-IRQ,
    /// clear pending RX IRQ when its enable bit goes off (MAME
    /// `mos6551.cpp:293-296`), force TX MARK when DTR de-asserts.
    /// Caller must hold `bufferMtx`.
    void applyCommandReg(uint8_t v);
    /// Apply a write to the control register: decode word length, stop
    /// bits, baud-rate divider, recompute `bytesPerSecond_`, reset the
    /// rate-limit accumulator. Caller must hold `bufferMtx`.
    void applyControlReg(uint8_t v);
    /// Programmed reset (write to status register, MAME
    /// `mos6551.cpp:264-270`): clear OVERRUN, clear DCD/DSR IRQ sources,
    /// then `write_command(cmd & ~0x1F)` (preserve parity bits 5-7).
    /// Caller must hold `bufferMtx`.
    void applyProgrammedReset();
    /// Called from the TCP worker when a client connects or disconnects.
    /// Mirrors MAME's "DCD/DSR pin change → status XOR → IRQ if !DTR"
    /// logic (`mos6551.cpp:443-461`) but driven by connect events rather
    /// than per-bit-clock polling.
    void onConnectionEdge(bool nowConnected);

    /// Is anything actually cabled to the port? DCD/DSR are active-low
    /// "device present" pins, and two different devices can hold them
    /// down: a telnet peer on the modem side, or an ImageWriter on the
    /// printer side. The tap counts because a printer is a DCE that is
    /// simply *there* — it has no carrier to acquire, so a firmware that
    /// waits for one (the //c's built-in printer port does, on every
    /// character) must not be told the line is dead.
    /// Caller must hold `bufferMtx` (reads `printerTap_`).
    bool deviceAttached() const { return connected || printerTap_; }

    /// Latch new IRQ sources and push the line if it transitioned.
    /// Caller must hold `bufferMtx`.
    void raiseIrqSource(uint8_t mask);
    /// Clear specific IRQ sources (e.g. status read clears all, RDR read
    /// clears IRQ_RDRF). Caller must hold `bufferMtx`.
    void clearIrqSource(uint8_t mask);
    /// Apply `irqState_ != 0` to the CPU pin if it differs from the
    /// previously asserted level. Caller must hold `bufferMtx`.
    void pushIrqLine();

    // TX (Apple II → TCP) and RX (TCP → Apple II) ring buffers.
    mutable std::mutex bufferMtx;
    std::deque<uint8_t> txBuf;     // bytes the CPU wrote, awaiting socket send
    std::deque<uint8_t> rxBuf;     // bytes from the socket, awaiting CPU read
    std::atomic<uint64_t> rxCount { 0 };
    std::atomic<uint64_t> txCount { 0 };
    // Recent-bytes peephole (latest ~256 bytes in each direction).
    std::deque<uint8_t> rxTail;
    std::deque<uint8_t> txTail;
    // Optional keyboard injection: each TCP RX byte is also handed to
    // this callback so a telnet session lands characters in BASIC's
    // keyboard latch even when IN#n hasn't been run yet.
    std::function<void(uint8_t)> keyboardSink;

    // Printer tap (see setPrinterTap). Guarded by bufferMtx like the TX
    // ring; the spool intentionally survives onReset() — it is host-side
    // paper trail, not machine state (same rule as PrinterCard's spool).
    bool printerTap_ = false;
    std::vector<uint8_t> printerSpool_;
    // Absolute offset of printerSpool_[0] in the ever-spooled byte stream —
    // lets the 1 MiB cap trim the consumed prefix without breaking the
    // drain cursor (see drainPrinterSpoolFrom).
    size_t printerSpoolBase_ = 0;
    // The cap drops half a megabyte out of the middle of a printout; warn
    // once so it is not silent, but only once (a guest that trips it will
    // trip it again immediately, and a log storm helps nobody).
    bool   printerSpoolTrimWarned_ = false;

    void buildRom();
};

#endif // POM2_SUPER_SERIAL_CARD_H
