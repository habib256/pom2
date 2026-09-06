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

// Scc8530Device — port of MAME's `src/devices/machine/z80scc.{h,cpp}`
// (Zilog Z8530 SCC, Serial Communications Controller), pinned to MAME
// commit 588eeb33707f8d392701716c41b0420a48c41f28 (2026-08-29).
//
// The Z8530 is a two-channel USART with an on-chip baud-rate generator,
// a 3-byte receive FIFO, a one-byte transmit buffer and a six-source
// interrupt block. In the Apple world it is the serial/LocalTalk chip of
// the Macintosh, the IIgs and — the reason this file exists — the Apple II
// Workstation Card, whose firmware drives it at `$7500-$7503` in the
// card's own address space (see `docs/printer_plan_2.md` § 5.1).
//
// WHAT IS PORTED, AND WHAT IS NOT
// ───────────────────────────────
// Ported line-for-line, with the MAME function name in a comment above
// each member: the whole register file (WR0-WR15 / RR0-RR15), the two-step
// register-pointer access protocol, every WR0 command, the WR9 reset
// commands, the interrupt block (priority, IP bits in RR3, vector
// modification into RR2, the Z80 daisy-chain IUS logic used by the SCC
// for its *internal* chain), the receive FIFO with its error FIFO and
// lock-on-error behaviour, the single-slot transmit buffer, the
// CTS/DCD/SYNC pins with their External/Status latch, and the baud-rate
// generator including the Zero Count interrupt.
//
// Deliberately NOT ported, each for a stated reason:
//
//   * The Z-Bus (Z8030) accessors. This is the 8530, a Universal Bus
//     part; `zbus_r/w` are guarded in MAME by `SET_Z80X30` and would be
//     dead code here. `m_variant` is therefore constant-folded to
//     `TYPE_SCC8530` throughout, which also removes the ESCC/85C30
//     branches (extended read, 8-byte FIFOs, WR7'). Every place MAME
//     tests the variant, the fold is noted in a comment.
//
//   * MAME's `device_serial_interface` bit-shifting. POM2 models the
//     serial line at BYTE granularity: a byte written to the transmit
//     buffer occupies the shift register for exactly as long as its
//     frame would take on the wire (start + data + parity + stop bits at
//     the programmed rate, computed the way `update_serial` programs
//     diserial), and is handed to the host as one byte when that time
//     expires. Receive is the mirror image: `receiveByte` is the seam
//     where `rcv_complete` would call `receive_data`. Every register,
//     status bit and interrupt above that seam behaves as MAME's does;
//     what is lost is sub-byte visibility of the wire, which nothing on
//     an Apple II can observe. → `docs/lle_vs_hle.md`
//
//   * SDLC framing is NOT from MAME, because MAME does not model it:
//     `do_sccreg_wr4` logs "SDLC - not implemented" and every CRC reset
//     code in WR0 is a no-op there. LocalTalk runs on SDLC, so POM2 models
//     it from the Zilog SCC/ESCC user manual (UM010902) instead — a
//     **documented deviation from the MAME-is-the-oracle rule**, marked at
//     every site with `SDLC (datasheet, not MAME)`. What is modelled is the
//     part a byte-granular seam can carry: frame delimitation, the Tx
//     Underrun/EOM latch that closes a frame, Sync/Hunt, address search,
//     abort, End Of Frame with its residue code, and the CRC as a
//     *guarantee* rather than two bytes on a wire. What is not modelled is
//     what only exists between the bytes — bit stuffing, the flag patterns,
//     and the FM0/DPLL line coding — none of which any register can see.
//
// Time model: `tick()` counts the SCC's own PCLK, exactly like MAME's
// `owner()->clock()`, because every rate in the chip (BRG, Zero Count,
// the shift registers) is derived from it. Owners that live in the
// 1.023 MHz Apple II domain scale on the way in rather than making this
// file guess. Both internal clocks use an exact integer accumulator
// (`acc += ticks * rate`, then draw whole periods out of it) so a slow
// caller with a coarse `tick` granularity loses no edges and accumulates
// no drift.

#ifndef POM2_SCC8530_DEVICE_H
#define POM2_SCC8530_DEVICE_H

#include <cstdint>
#include <cstddef>
#include <functional>
#include <vector>

namespace pom2 {

class Scc8530Device
{
public:
    /// Channel index. MAME `z80scc.h` `CHANNEL_A` / `CHANNEL_B`.
    enum ChannelId : int { CHAN_A = 0, CHAN_B = 1 };

    /// PCLK of the Workstation Card / Macintosh SCC: 3.6864 MHz, the
    /// crystal that makes the standard bit rates come out exact.
    static constexpr uint32_t kDefaultPclk = 3686400;

    /// Interrupt line — MAME `out_int_callback`. `true` = /INT asserted.
    using IntLineCb = std::function<void(bool asserted)>;
    /// One transmitted byte has left the shift register (channel, data).
    /// This is POM2's byte-granular stand-in for MAME `out_txd_callback`.
    using ByteCb = std::function<void(int channel, uint8_t data)>;
    /// /RTS, /DTR and /W//REQ pins — MAME `out_rts_cb` / `out_dtr_cb` /
    /// `out_wreq_cb`. `state` is the pin level, active LOW as on the chip.
    using LineCb = std::function<void(int channel, bool state)>;

    Scc8530Device();

    // ─── Wiring ──────────────────────────────────────────────────────────

    /// PCLK in Hz. MAME passes this as the device clock.
    void setPclk(uint32_t hz);
    uint32_t pclk() const { return pclk_; }

    /// External clock on the channel's /RTxC pin, in Hz. MAME
    /// `configure_channels(rxa, txa, rxb, txb)` — POM2 keeps only the
    /// /RTxC leg, which is the one every rate in `get_brg_rate` and
    /// `get_rtxc_rate` actually reads.
    void setRtxc(int channel, uint32_t hz);

    void setIntCallback(IntLineCb cb) { intCb_ = std::move(cb); }
    void setTxCallback(ByteCb cb)     { txCb_ = std::move(cb); }
    void setRtsCallback(LineCb cb)    { rtsCb_ = std::move(cb); }
    void setDtrCallback(LineCb cb)    { dtrCb_ = std::move(cb); }
    void setWreqCallback(LineCb cb)   { wreqCb_ = std::move(cb); }

    /// Hardware reset (/RESET pin, or the WR9 "Force Hardware Reset"
    /// command). MAME `z80scc_device::device_reset_after_children`
    /// (z80scc.cpp:502) applied on top of both channel resets.
    void reset();

    // ─── Bus access ──────────────────────────────────────────────────────
    // Two pin orderings exist in the wild and MAME exposes both. Pick the
    // one your board wires; `offset` is the low two address bits.

    /// A1 = A//B, A0 = D//C — MAME `ab_dc_r` (z80scc.cpp:946).
    /// Offsets: 0 = B control, 1 = B data, 2 = A control, 3 = A data.
    /// This is the Apple II Workstation Card's wiring.
    uint8_t readAbDc(uint8_t offset);
    void    writeAbDc(uint8_t offset, uint8_t data);

    /// A1 = D//C, A0 = A//B — MAME `dc_ab_r` (z80scc.cpp:903).
    /// Offsets: 0 = B control, 1 = A control, 2 = B data, 3 = A data.
    uint8_t readDcAb(uint8_t offset);
    void    writeDcAb(uint8_t offset, uint8_t data);

    /// MAME `z80scc_channel::control_read` / `control_write`
    /// (z80scc.cpp:1717 / 2334) and `data_read` / `data_write`
    /// (z80scc.cpp:2362 / 2457).
    uint8_t controlRead(int channel);
    void    controlWrite(int channel, uint8_t data);
    uint8_t dataRead(int channel);
    void    dataWrite(int channel, uint8_t data);

    // ─── Modem-control pins (inputs) ─────────────────────────────────────
    // `state` is the pin level: LOW (false) is the ASSERTED state of these
    // active-low inputs, matching MAME's `cts_w(0)` = "CTS active".

    void ctsW(int channel, bool state);   ///< MAME `cts_w`  (z80scc.cpp:2604)
    void dcdW(int channel, bool state);   ///< MAME `dcd_w`  (z80scc.cpp:2641)
    void syncW(int channel, bool state);  ///< MAME `sync_w` (z80scc.cpp:2680)

    // ─── Serial seam ─────────────────────────────────────────────────────

    /// Deliver one received byte. This is where MAME's `rcv_complete`
    /// (z80scc.cpp:1303) calls `receive_data`; the byte is dropped, with
    /// the same complaint MAME's `rcv_callback` logs, when the receiver
    /// is disabled (WR3 D0).
    void receiveByte(int channel, uint8_t data);

    // ─── SDLC (datasheet, not MAME) ──────────────────────────────────────

    /// True while WR4 selects SDLC on this channel.
    bool sdlcMode(int channel) const;

    /// A complete transmitted frame, payload only. In SDLC the chip closes
    /// a frame on transmit underrun with the CRC enabled, which is what the
    /// driver arranges by clearing the Tx Underrun/EOM latch after loading
    /// the first byte. The CRC bytes and the flags are the wire's business
    /// and are not in this payload.
    using FrameCb = std::function<void(int channel, const std::vector<uint8_t>& frame)>;
    void setFrameCallback(FrameCb cb) { frameCb_ = std::move(cb); }

    /// Deliver one received SDLC frame, payload only. Clears Sync/Hunt the
    /// way an opening flag does, honours WR3's address search (the frame is
    /// ignored unless its first byte matches WR6 or is the $FF broadcast),
    /// and marks the last byte End Of Frame — a special receive condition,
    /// so it locks the receive FIFO until the driver issues Error Reset,
    /// exactly as the manual describes. Pass `crcError` to present a frame
    /// the chip received with a bad FCS.
    void receiveFrame(int channel, const uint8_t* data, std::size_t len,
                      bool crcError = false);

    /// Bytes buffered in the frame currently being transmitted. Non-zero
    /// only between the first byte of a frame and its close.
    std::size_t txFrameSize(int channel) const;

    /// SDLC (datasheet, not MAME): ceiling on ONE transmitted frame.
    ///
    /// The chip has no such limit — bytes leave the shift register onto the
    /// wire as they are loaded, and the frame ends when the transmitter
    /// underruns. POM2 has to BUFFER the frame instead, because `frameCb_`
    /// delivers it whole, so a guest that keeps the transmit buffer fed and
    /// never lets the underrun latch fire grows a std::vector without bound:
    /// a leak on the CPU thread with no error anywhere, and a frame past
    /// 65535 bytes would also truncate the 16-bit length the snapshot writes.
    /// The protocol this exists for is LocalTalk, whose LLAP frame is a
    /// 3-byte header plus at most 600 data bytes; 1024 is comfortably past
    /// anything legal. Reaching it ABORTS the frame — the same outcome as
    /// WR0's Send Abort, which is what a receiver would see anyway.
    static constexpr std::size_t kMaxTxFrameBytes = 1024;

    /// Advance the chip by `pclkCycles` PCLK ticks: the transmit shift
    /// registers and the baud-rate generator's Zero Count timer.
    void tick(uint64_t pclkCycles);

    // ─── Interrupts ──────────────────────────────────────────────────────

    /// Current /INT pin state. MAME `m_out_int_state`.
    bool intAsserted() const { return outIntState_; }

    /// Interrupt acknowledge. MAME `m1_r` → `z80daisy_irq_ack`
    /// (z80scc.cpp:589). Returns the vector from RR2, or -1 when WR9's
    /// NV bit asks for the CPU's own autovector, or when nothing is
    /// pending.
    int intAck();

    // ─── Inspection (tests, debug panels) ────────────────────────────────

    uint8_t peekWr(int channel, int reg) const;
    uint8_t peekRr(int channel, int reg) const;
    /// Programmed bit rates in bits/second, 0 when no clock is selected.
    uint32_t txRate(int channel) const;
    uint32_t rxRate(int channel) const;
    /// True while a byte occupies the transmit shift register.
    bool txBusy(int channel) const;
    /// Bytes currently in the 3-deep receive FIFO.
    int rxFifoCount(int channel) const;

    // ─── Snapshot (rewind) ───────────────────────────────────────────────
    // Every register, both FIFOs, the interrupt block and the three clock
    // accumulators. Not the callbacks and not PCLK/RTxC, which are wiring
    // the owner re-establishes. A card that carries this round-trips its
    // chip; one that does not lands the SCC on reset values and waits for
    // its firmware to reprogram it.

    void appendSnapshot(std::vector<uint8_t>& out) const;
    /// Returns false — and changes nothing — if the blob is foreign, short
    /// or from a newer version.
    bool restoreSnapshot(const uint8_t* data, std::size_t len);

private:
    // ─── MAME z80daisy.h interrupt-state bits ────────────────────────────
    static constexpr uint8_t kDaisyInt = 0x01; ///< Z80_DAISY_INT
    static constexpr uint8_t kDaisyIeo = 0x02; ///< Z80_DAISY_IEO (IUS)

    // ─── Interrupt sources and priorities (z80scc.h) ─────────────────────
    enum IntType : int {
        INT_TRANSMIT = 0,
        INT_EXTERNAL = 1,
        INT_RECEIVE  = 2,
        INT_SPECIAL  = 3,
    };
    enum IntPrio : int {
        INT_TRANSMIT_PRIO = 1,
        INT_EXTERNAL_PRIO = 0,
        INT_RECEIVE_PRIO  = 2,
        INT_SPECIAL_PRIO  = 0,
    };

    struct Channel
    {
        // Read registers — MAME `m_rr0` … `m_rr15`.
        uint8_t rr0 = 0, rr1 = 0, rr2 = 0, rr3 = 0, rr10 = 0;
        // Write registers — MAME `m_wr0` … `m_wr15`. WR9 is device-wide.
        uint8_t wr0 = 0, wr1 = 0, wr2 = 0, wr3 = 0, wr4 = 0, wr5 = 0;
        uint8_t wr10 = 0, wr11 = 0, wr12 = 0, wr13 = 0, wr14 = 0, wr15 = 0;

        // Receive FIFO — 3 deep on the NMOS 8530 (MAME z80scc.cpp:1049).
        uint8_t rxData[3] = {0, 0, 0};
        uint8_t rxError[3] = {0, 0, 0};
        /// SDLC (datasheet, not MAME): End Of Frame rides beside the slot
        /// rather than inside `rxError`, because MAME's `data_read` masks
        /// that byte down to the three async error bits and would drop it.
        bool rxEof[3] = {false, false, false};
        int rxFifoRp = 0, rxFifoWp = 0;
        static constexpr int kRxFifoSz = 3;

        // Transmit "FIFO" — one slot on the NMOS part (z80scc.cpp:1051),
        // whose fullness is carried by RR0 D2 (TBE) rather than pointers.
        uint8_t txData[1] = {0};
        int txFifoRp = 0, txFifoWp = 0;
        static constexpr int kTxFifoSz = 1;

        int rxFirst = 0;        ///< MAME `m_rx_first`
        int txIntDisarm = 0;    ///< MAME `m_tx_int_disarm`
        uint8_t extIntLatch = 0;   ///< MAME `m_extint_latch`
        uint8_t extIntStates = 0;  ///< MAME `m_extint_states`
        int dtr = 0, rts = 0;
        uint16_t syncPattern = 0;  ///< MAME `m_sync_pattern`
        unsigned brgRate = 0;      ///< MAME `m_brg_rate`
        unsigned delayedTxBrgChange = 0;

        uint32_t rtxc = 0;      ///< /RTxC input frequency, Hz
        unsigned rcvRate = 0;   ///< MAME diserial `set_rcv_rate`
        unsigned traRate = 0;   ///< MAME diserial `set_tra_rate`

        // Byte-granular transmit shift register (POM2's diserial stand-in).
        uint8_t txShiftData = 0;
        int txHalfBits = 0;     ///< half-bit periods left in the frame
        uint64_t txAcc = 0;     ///< PCLK accumulator for the half-bit clock

        // Baud-rate generator Zero Count timer — MAME `m_baudtimer`.
        unsigned brgTimerRate = 0;
        uint64_t brgAcc = 0;

        /// SDLC (datasheet, not MAME): the frame being assembled on
        /// transmit, payload only. Emptied when the frame closes or aborts.
        std::vector<uint8_t> txFrame;
    };

    // ─── Device-level state ──────────────────────────────────────────────
    Channel ch_[2];
    uint8_t wr9_ = 0;          ///< MAME `m_wr9` — one per device
    uint8_t wr0PtrBits_ = 0;   ///< MAME `m_wr0_ptrbits`
    uint8_t intState_[6] = {0};   ///< MAME `m_int_state`
    int     intSource_[6] = {0};  ///< MAME `m_int_source`
    bool    outIntState_ = false;
    uint32_t pclk_ = kDefaultPclk;

    IntLineCb intCb_;
    ByteCb    txCb_;
    LineCb    rtsCb_;
    LineCb    dtrCb_;
    LineCb    wreqCb_;
    FrameCb   frameCb_;

    // ─── Device-level helpers (z80scc_device::) ──────────────────────────
    int  daisyIrqState() const;                       ///< z80scc.cpp:557
    void checkInterrupts();                           ///< z80scc.cpp:651
    void resetInterrupts();                           ///< z80scc.cpp:666
    uint8_t modifyVector(uint8_t vec, int index, uint8_t src) const; ///< :679
    static int extIntPriority(int type);              ///< z80scc.cpp:714
    void triggerInterrupt(int index, int type);       ///< z80scc.cpp:733
    int  updateExtInt(int index);                     ///< z80scc.cpp:793

    // ─── Channel helpers (z80scc_channel::) ──────────────────────────────
    void channelReset(int index);                     ///< z80scc.cpp:1123
    uint8_t registerRead(int index, uint8_t reg);      ///< z80scc.cpp:1657
    void registerWrite(int index, uint8_t reg, uint8_t data); ///< z80scc.cpp:2288
    void receiveData(int index, uint8_t data);        ///< z80scc.cpp:2566
    void rxFifoRpStep(int index);                     ///< z80scc.cpp:2423
    void txFifoRpStep(int index);                     ///< z80scc.cpp:2440
    void checkDmaRequest(int index);                  ///< z80scc.cpp:3015
    void checkReceiveInterrupt(int index);            ///< z80scc.cpp:3038
    void updateSerial(int index);                     ///< z80scc.cpp:2858
    void updateBaudTimer(int index);                  ///< z80scc.cpp:2822
    void updateRts(int index);                        ///< z80scc.cpp:1352
    void setRts(int index, int state);                ///< z80scc.cpp:1346
    void setDtr(int index, int state);                ///< z80scc.cpp:2967
    void traComplete(int index);                      ///< z80scc.cpp:1218
    /// SDLC (datasheet, not MAME): close the frame in flight and hand it
    /// over. Called from the transmit underrun.
    void closeTxFrame(int index);
    unsigned brgRateOf(int index) const;              ///< z80scc.cpp:2789
    unsigned rtxcRateOf(int index) const;             ///< z80scc.cpp:2813
    int  clockMode(int index) const;                  ///< z80scc.cpp:1320
    int  rxWordLength(int index) const;               ///< z80scc.cpp:1396
    int  txWordLength(int index) const;               ///< z80scc.cpp:1415
    /// Frame length in HALF-bit periods, the way `update_serial` programs
    /// diserial's `set_data_frame(1, data, parity, stop)`. Half-bits
    /// because 1.5 stop bits is a legal SCC frame.
    int  frameHalfBits(int index) const;

    // Write-register handlers, one per MAME `do_sccreg_wrN`.
    void doWr0(int index, uint8_t data);
    void doWr1(int index, uint8_t data);
    void doWr2(int index, uint8_t data);
    void doWr3(int index, uint8_t data);
    void doWr4(int index, uint8_t data);
    void doWr5(int index, uint8_t data);
    void doWr9(int index, uint8_t data);
    void doWr14(int index, uint8_t data);
    void doWr15(int index, uint8_t data);

    // Read-register handlers.
    uint8_t doRr0(int index) const;
    uint8_t doRr2(int index);
    uint8_t doRr3(int index) const;
};

} // namespace pom2

#endif // POM2_SCC8530_DEVICE_H
