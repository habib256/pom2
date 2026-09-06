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

// Scc8530Device — see Scc8530Device.h for what is ported and what is not.
// Every function carries the MAME `z80scc.cpp` symbol and line it came
// from (commit 588eeb33707f8d392701716c41b0420a48c41f28).

#include "Scc8530Device.h"

#include "ByteIO.h"



namespace pom2 {

namespace {

// ─── Register bit definitions — MAME z80scc.cpp:144-361, verbatim ────────

enum : uint8_t {
    RR0_RX_CHAR_AVAILABLE = 0x01,
    RR0_ZC                = 0x02,
    RR0_TX_BUFFER_EMPTY   = 0x04,
    RR0_DCD               = 0x08,
    RR0_SYNC_HUNT         = 0x10,
    RR0_CTS               = 0x20,
    RR0_TX_UNDERRUN       = 0x40,
    RR0_BREAK_ABORT       = 0x80,
};

enum : uint8_t {
    RR1_ALL_SENT          = 0x01,
    RR1_RESIDUE_CODE_MASK = 0x0e,
    RR1_PARITY_ERROR      = 0x10,
    RR1_RX_OVERRUN_ERROR  = 0x20,
    RR1_CRC_FRAMING_ERROR = 0x40,
    RR1_END_OF_FRAME      = 0x80,
};

// Universal Bus WR0 commands for the 85X30 family.
enum : uint8_t {
    WR0_REGISTER_MASK         = 0x07,
    WR0_COMMAND_MASK          = 0x38,
    WR0_NULL                  = 0x00,
    WR0_POINT_HIGH            = 0x08,
    WR0_RESET_EXT_STATUS      = 0x10,
    WR0_SEND_ABORT            = 0x18,
    WR0_ENABLE_INT_NEXT_RX    = 0x20,
    WR0_RESET_TX_INT          = 0x28,
    WR0_ERROR_RESET           = 0x30,
    WR0_RESET_HIGHEST_IUS     = 0x38,
};

enum : uint8_t {
    WR1_EXT_INT_ENABLE      = 0x01,
    WR1_TX_INT_ENABLE       = 0x02,
    WR1_PARITY_IS_SPEC_COND = 0x04,
    WR1_RX_INT_MODE_MASK    = 0x18,
    WR1_RX_INT_DISABLE      = 0x00,
    WR1_RX_INT_FIRST        = 0x08,
    WR1_RX_INT_ALL          = 0x10,
    WR1_RX_INT_PARITY       = 0x18,
    WR1_WREQ_ON_RX_TX       = 0x20,
    WR1_WREQ_FUNCTION       = 0x40,
    WR1_WREQ_ENABLE         = 0x80,
};

enum : uint8_t {
    WR3_RX_ENABLE              = 0x01,
    WR3_SYNC_CHAR_LOAD_INHIBIT = 0x02,
    WR3_ADDRESS_SEARCH_MODE    = 0x04,
    WR3_RX_CRC_ENABLE          = 0x08,
    WR3_ENTER_HUNT_MODE        = 0x10,
    WR3_AUTO_ENABLES           = 0x20,
    WR3_RX_WORD_LENGTH_MASK    = 0xc0,
    WR3_RX_WORD_LENGTH_5       = 0x00,
    WR3_RX_WORD_LENGTH_7       = 0x40,
    WR3_RX_WORD_LENGTH_6       = 0x80,
    WR3_RX_WORD_LENGTH_8       = 0xc0,
};

enum : uint8_t {
    WR4_PARITY_ENABLE   = 0x01,
    WR4_PARITY_EVEN     = 0x02,
    WR4_STOP_BITS_MASK  = 0x0c,
    WR4_STOP_BITS_1     = 0x04,
    WR4_STOP_BITS_1_5   = 0x08,
    WR4_STOP_BITS_2     = 0x0c,
    WR4_SYNC_MODE_MASK  = 0x30,
    WR4_CLOCK_RATE_MASK = 0xc0,
    WR4_CLOCK_RATE_X1   = 0x00,
    WR4_CLOCK_RATE_X16  = 0x40,
    WR4_CLOCK_RATE_X32  = 0x80,
    WR4_CLOCK_RATE_X64  = 0xc0,
};

enum : uint8_t {
    WR5_TX_CRC_ENABLE       = 0x01,
    WR5_RTS                 = 0x02,
    WR5_CRC16               = 0x04,
    WR5_TX_ENABLE           = 0x08,
    WR5_SEND_BREAK          = 0x10,
    WR5_TX_WORD_LENGTH_MASK = 0x60,
    WR5_TX_WORD_LENGTH_5    = 0x00,
    WR5_TX_WORD_LENGTH_6    = 0x40,
    WR5_TX_WORD_LENGTH_7    = 0x20,
    WR5_TX_WORD_LENGTH_8    = 0x60,
    WR5_DTR                 = 0x80,
};

enum : uint8_t {
    WR9_CMD_MASK        = 0xc0,
    WR9_CMD_NORESET     = 0x00,
    WR9_CMD_CHNB_RESET  = 0x40,
    WR9_CMD_CHNA_RESET  = 0x80,
    WR9_CMD_HW_RESET    = 0xc0,
    WR9_BIT_VIS         = 0x01,
    WR9_BIT_NV          = 0x02,
    WR9_BIT_DLC         = 0x04,
    WR9_BIT_MIE         = 0x08,
    WR9_BIT_SHSL        = 0x10,
    WR9_BIT_IACK        = 0x20,
};

enum : uint8_t {
    WR11_RCVCLK_TYPE     = 0x80,
    WR11_RCVCLK_SRC_MASK = 0x60,
    WR11_RCVCLK_SRC_RTXC = 0x00,
    WR11_RCVCLK_SRC_TRXC = 0x20,
    WR11_RCVCLK_SRC_BR   = 0x40,
    WR11_RCVCLK_SRC_DPLL = 0x60,
    WR11_TRACLK_SRC_MASK = 0x18,
    WR11_TRACLK_SRC_RTXC = 0x00,
    WR11_TRACLK_SRC_TRXC = 0x08,
    WR11_TRACLK_SRC_BR   = 0x10,
    WR11_TRACLK_SRC_DPLL = 0x18,
};

enum : uint8_t {
    WR14_DPLL_CMD_MASK  = 0xe0,
    WR14_BRG_ENABLE     = 0x01,
    WR14_BRG_SOURCE     = 0x02,
    WR14_DTR_REQ_FUNC   = 0x04,
    WR14_AUTO_ECHO      = 0x08,
    WR14_LOCAL_LOOPBACK = 0x10,
};

enum : uint8_t {
    WR15_WR7PRIME   = 0x01,
    WR15_ZEROCOUNT  = 0x02,
    WR15_STATUS_FIFO= 0x04,
    WR15_DCD        = 0x08,
    WR15_SYNC       = 0x10,
    WR15_CTS        = 0x20,
    WR15_TX_EOM     = 0x40,
    WR15_BREAK_ABORT= 0x80,
};

// MAME register indices (z80scc.h). Only the names actually reachable on
// an NMOS 8530 after `scc_register_read`'s remap are spelled out here.
enum : uint8_t {
    REG_RR0_STATUS          = 0,
    REG_RR1_SPEC_RCV_COND   = 1,
    REG_RR2_INTERRUPT_VECT  = 2,
    REG_RR3_INTERUPPT_PEND  = 3,
    REG_RR8_RECEIVE_DATA    = 8,
    REG_RR10_MISC_STATUS    = 10,
    REG_RR12_LO_TIME_CONST  = 12,
    REG_RR13_HI_TIME_CONST  = 13,
    REG_RR14_WR7_OR_R10     = 14,
    REG_RR15_WR15_EXT_STAT  = 15,
};

/// MAME `bitswap<4>(src, 3, 0, 1, 2)` for the Status High vector layout
/// (z80scc.cpp:704). Bit 3 stays put; bits 2,1,0 are reordered 0,1,2.
inline uint8_t bitswap4_3012(uint8_t v)
{
    return static_cast<uint8_t>(((v >> 3) & 1) << 3 | ((v >> 0) & 1) << 2 |
                                ((v >> 1) & 1) << 1 | ((v >> 2) & 1) << 0);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  Construction / reset
// ─────────────────────────────────────────────────────────────────────────

Scc8530Device::Scc8530Device()
{
    // MAME `z80scc_channel::z80scc_channel` (z80scc.cpp:993) zeroes every
    // register; the Channel member initialisers already did that. What is
    // left is the power-on reset the machine performs on top.
    reset();
}

void Scc8530Device::setPclk(uint32_t hz)
{
    pclk_ = hz ? hz : 1;
    updateSerial(CHAN_A);
    updateSerial(CHAN_B);
    updateBaudTimer(CHAN_A);
    updateBaudTimer(CHAN_B);
}

void Scc8530Device::setRtxc(int channel, uint32_t hz)
{
    ch_[channel & 1].rtxc = hz;
    updateSerial(channel & 1);
    updateBaudTimer(channel & 1);
}

/// MAME `z80scc_channel::device_reset` (z80scc.cpp:1123). Channel reset:
/// the values the SCC user manual gives for a Channel Reset command, which
/// is also what a hardware reset applies before the device-level overrides.
void Scc8530Device::channelReset(int index)
{
    Channel& c = ch_[index];

    // "Reset RS232 emulation": receive/transmit shift registers.
    c.txHalfBits = 0;
    c.txAcc = 0;

    // empty fifos
    c.rxFifoWp = c.rxFifoRp = 0;
    c.txFifoWp = c.txFifoRp = 0;
    for (bool& e : c.rxEof) e = false;
    c.txFrame.clear();       // SDLC (datasheet, not MAME)

    c.wr0  = 0x00;
    c.wr1 &= 0x24;
    c.wr3 &= 0x01;
    c.wr4  = 0x04;
    c.wr5  = 0x00;
    // MAME sets WR7' = 0x20 for 85C30/ESCC only — not this part.
    wr9_ &= 0xdf;
    c.wr10 &= 0x60;
    c.wr11 &= 0xff;
    c.wr14 &= 0xc3;
    c.wr14 |= 0x20;
    c.wr15  = 0xf8;
    c.rr0  &= 0xfc;
    c.rr0  |= 0x44;
    c.rr1  &= 0x07;
    c.rr1  |= 0x06;            // required reset value
    c.rr1  |= RR1_ALL_SENT;    // "don't care" in the manual, drivers hang without it
    c.rr3   = 0x00;
    c.rr10 &= 0x40;

    // reset external lines
    setRts(index, c.rts = (c.wr5 & WR5_RTS) ? 0 : 1);
    setDtr(index, c.dtr = (c.wr14 & WR14_DTR_REQ_FUNC) ? 0 : ((c.wr5 & WR5_DTR) ? 0 : 1));

    // reset interrupts
    if (index == CHAN_A)
        resetInterrupts();

    c.extIntLatch = 0;
    c.extIntStates = c.rr0;
    c.brgTimerRate = 0;   // MAME: m_baudtimer->adjust(attotime::never)
    c.brgAcc = 0;
}

/// MAME `z80scc_device::device_reset_after_children` (z80scc.cpp:502).
/// device_t resets children first, so both channel resets run before these
/// hardware-only overrides land.
void Scc8530Device::reset()
{
    channelReset(CHAN_A);
    channelReset(CHAN_B);

    wr9_ &= 0x3c;
    wr9_ |= 0xc0;
    ch_[CHAN_A].wr10 = 0x00;
    ch_[CHAN_B].wr10 = 0x00;
    ch_[CHAN_A].wr11 = 0x08;
    ch_[CHAN_B].wr11 = 0x08;
    ch_[CHAN_A].wr14 &= 0xf0;
    ch_[CHAN_A].wr14 |= 0x30;
    ch_[CHAN_B].wr14 &= 0xf0;
    ch_[CHAN_B].wr14 |= 0x30;

    wr0PtrBits_ = 0;

    updateSerial(CHAN_A);
    updateSerial(CHAN_B);
}

// ─────────────────────────────────────────────────────────────────────────
//  Interrupts
// ─────────────────────────────────────────────────────────────────────────

/// MAME `z80scc_device::z80daisy_irq_state` (z80scc.cpp:557).
int Scc8530Device::daisyIrqState() const
{
    int state = 0;
    for (uint8_t elem : intState_) {
        // if we're servicing a request, don't indicate more interrupts
        if (elem & kDaisyIeo) { state |= kDaisyIeo; break; }
        state |= elem;
    }
    // Last chance to keep the control of the interrupt line
    state |= (wr9_ & WR9_BIT_DLC) ? kDaisyIeo : 0;
    return state;
}

/// MAME `z80scc_device::check_interrupts` (z80scc.cpp:651).
void Scc8530Device::checkInterrupts()
{
    bool state = (daisyIrqState() & kDaisyInt) != 0;
    if (outIntState_ != state) {
        outIntState_ = state;
        if (intCb_) intCb_(state);
    }
}

/// MAME `z80scc_device::reset_interrupts` (z80scc.cpp:666).
void Scc8530Device::resetInterrupts()
{
    for (uint8_t& elem : intState_) elem = 0;
    checkInterrupts();
}

/// MAME `z80scc_device::modify_vector` (z80scc.cpp:679).
uint8_t Scc8530Device::modifyVector(uint8_t vec, int index, uint8_t src) const
{
    src &= 3;
    src |= (index == CHAN_A ? 0x04 : 0x00);

    if (wr9_ & WR9_BIT_SHSL) {      // affect V4-V6
        src = bitswap4_3012(src);
        vec &= 0x8f;
        vec |= static_cast<uint8_t>(src << 4);
    } else {                        // affect V1-V3
        vec &= 0xf1;
        vec |= static_cast<uint8_t>(src << 1);
    }
    return vec;
}

/// MAME `z80scc_device::get_extint_priority` (z80scc.cpp:714).
int Scc8530Device::extIntPriority(int type)
{
    switch (type) {
    case INT_RECEIVE:  return INT_RECEIVE_PRIO;
    case INT_EXTERNAL: return INT_EXTERNAL_PRIO;
    case INT_TRANSMIT: return INT_TRANSMIT_PRIO;
    case INT_SPECIAL:  return INT_SPECIAL_PRIO;
    default:           return -1;   // MAME logs "Bad interrupt source"
    }
}

/// MAME `z80scc_device::trigger_interrupt` (z80scc.cpp:733).
void Scc8530Device::triggerInterrupt(int index, int type)
{
    uint8_t vector = ch_[CHAN_A].rr2;

    // The Master Interrupt Enable (MIE) bit, WR9 D3, gates everything.
    if (!(wr9_ & WR9_BIT_MIE))
        return;

    int source = type;
    int prioLevel = extIntPriority(type);
    if (source < INT_TRANSMIT || source > INT_SPECIAL || prioLevel < 0 || prioLevel > 2)
        return;

    if (wr9_ & WR9_BIT_VIS)
        vector = modifyVector(vector, index, static_cast<uint8_t>(source));

    ch_[CHAN_B].rr2 = vector;

    // Interrupt source priority order: A Rx, A Tx, A Ext, B Rx, B Tx, B Ext
    int priority = prioLevel + (index == CHAN_A ? 0 : 3);

    intState_[priority] |= kDaisyInt;
    intSource_[priority] = source;

    // Prio levels are aligned with the bit order of RR3, so this works.
    ch_[CHAN_A].rr3 |= static_cast<uint8_t>(1 << (prioLevel + ((index == CHAN_A) ? 3 : 0)));

    checkInterrupts();
}

/// MAME `z80scc_device::update_extint` (z80scc.cpp:793).
int Scc8530Device::updateExtInt(int index)
{
    int ret = 1;    // assume there are more external/status interrupts to serve
    const uint8_t rr0  = ch_[index].rr0;
    const uint8_t wr15 = ch_[index].wr15;
    const uint8_t lrr0 = ch_[index].extIntStates;

    if (((lrr0 & wr15 & 0xf8) ^ (rr0 & wr15 & 0xf8)) == 0) {
        intState_[INT_EXTERNAL_PRIO + (index == CHAN_A ? 0 : 3)] = 0;
        ch_[CHAN_A].rr3 &= static_cast<uint8_t>(
            ~(1 << (INT_EXTERNAL_PRIO + ((index == CHAN_A) ? 3 : 0))));
        ret = 0;
    }
    return ret;
}

/// MAME `z80scc_device::z80daisy_irq_ack` (z80scc.cpp:589), reached
/// through `m1_r` (z80scc.cpp:826).
int Scc8530Device::intAck()
{
    int ret = -1;   // "use the CPU's default vector"
    for (uint8_t& elem : intState_) {
        if (elem & kDaisyInt) {
            elem = kDaisyIeo;   // set IUS
            checkInterrupts();
            if (wr9_ & WR9_BIT_NV)
                break;          // autovector requested
            ret = ch_[CHAN_B].rr2;
            break;
        }
    }
    return ret;
}

// ─────────────────────────────────────────────────────────────────────────
//  Bus access
// ─────────────────────────────────────────────────────────────────────────

/// MAME `z80scc_device::ab_dc_r` (z80scc.cpp:946).
uint8_t Scc8530Device::readAbDc(uint8_t offset)
{
    const int ab = (offset >> 1) & 1;
    const int dc = offset & 1;
    return dc ? dataRead(ab ? CHAN_A : CHAN_B) : controlRead(ab ? CHAN_A : CHAN_B);
}

/// MAME `z80scc_device::ab_dc_w` (z80scc.cpp:967).
void Scc8530Device::writeAbDc(uint8_t offset, uint8_t data)
{
    const int ab = (offset >> 1) & 1;
    const int dc = offset & 1;
    if (dc) dataWrite(ab ? CHAN_A : CHAN_B, data);
    else    controlWrite(ab ? CHAN_A : CHAN_B, data);
}

/// MAME `z80scc_device::dc_ab_r` (z80scc.cpp:903).
uint8_t Scc8530Device::readDcAb(uint8_t offset)
{
    const int ab = offset & 1;
    const int dc = (offset >> 1) & 1;
    return dc ? dataRead(ab ? CHAN_A : CHAN_B) : controlRead(ab ? CHAN_A : CHAN_B);
}

/// MAME `z80scc_device::dc_ab_w` (z80scc.cpp:923).
void Scc8530Device::writeDcAb(uint8_t offset, uint8_t data)
{
    const int ab = offset & 1;
    const int dc = (offset >> 1) & 1;
    if (dc) dataWrite(ab ? CHAN_A : CHAN_B, data);
    else    controlWrite(ab ? CHAN_A : CHAN_B, data);
}

/// MAME `z80scc_channel::control_read` (z80scc.cpp:1717).
uint8_t Scc8530Device::controlRead(int channel)
{
    const int index = channel & 1;
    int reg = wr0PtrBits_;
    const int regmask = (WR0_REGISTER_MASK | (wr0PtrBits_ & WR0_POINT_HIGH));

    wr0PtrBits_ = 0;
    reg &= regmask;

    if (reg != 0)
        ch_[index].wr0 &= static_cast<uint8_t>(~regmask);   // mask out register index

    return registerRead(index, static_cast<uint8_t>(reg));
}

/// MAME `z80scc_channel::control_write` (z80scc.cpp:2334).
void Scc8530Device::controlWrite(int channel, uint8_t data)
{
    const int index = channel & 1;
    uint8_t reg = wr0PtrBits_;
    const uint8_t regmask =
        static_cast<uint8_t>(WR0_REGISTER_MASK | (wr0PtrBits_ & WR0_POINT_HIGH));

    wr0PtrBits_ = 0;    // "Point High" is only valid for one access
    reg &= regmask;

    if (reg != 0)
        ch_[index].wr0 &= static_cast<uint8_t>(~regmask);

    registerWrite(index, reg, data);
}

/// MAME `z80scc_channel::scc_register_read` (z80scc.cpp:1657).
///
/// The variant fold: this is an NMOS part with no Status FIFO, so MAME's
/// first branch (`BIT(m_wr15, 2) == 0 || SET_NMOS`) is always taken and
/// the remap below is unconditional. That makes RR4-RR7, RR9 and RR11
/// unreachable — they are images of RR0/RR1/RR2/RR3, RR13 and RR15 — which
/// is why no handler exists for them here.
uint8_t Scc8530Device::registerRead(int index, uint8_t reg)
{
    Channel& c = ch_[index];

    if (reg > 3 && reg < 8) reg &= 0x03;
    else if (reg == 9)  reg = 13;
    else if (reg == 11) reg = 15;

    switch (reg) {
    case REG_RR0_STATUS:         return doRr0(index);
    case REG_RR1_SPEC_RCV_COND:  return c.rr1;
    case REG_RR2_INTERRUPT_VECT: return doRr2(index);
    case REG_RR3_INTERUPPT_PEND: return doRr3(index);
    case REG_RR8_RECEIVE_DATA:   return dataRead(index);
    case REG_RR10_MISC_STATUS:   return c.rr10;   // MAME: "feature not implemented"
    case REG_RR12_LO_TIME_CONST: return c.wr12;
    case REG_RR13_HI_TIME_CONST: return c.wr13;
    case REG_RR14_WR7_OR_R10:    return c.rr10;   // NMOS: image of RR10
    case REG_RR15_WR15_EXT_STAT: return static_cast<uint8_t>(c.wr15 & 0xfa);
    default:                     return 0;
    }
}

/// MAME `z80scc_channel::do_sccreg_rr0` (z80scc.cpp:1434).
uint8_t Scc8530Device::doRr0(int index) const
{
    const Channel& c = ch_[index];
    uint8_t rr0 = c.rr0;

    if (c.extIntLatch == 1) {
        // clear enabled bits, saving 2 unrelated bits, then set the
        // enabled ones from the latched states
        const uint8_t keep = static_cast<uint8_t>(~c.wr15) | WR15_WR7PRIME | WR15_STATUS_FIFO;
        rr0 = static_cast<uint8_t>((rr0 & keep) | (c.extIntStates & ~keep));
    }
    return rr0;
}

/// MAME `z80scc_channel::do_sccreg_rr2` (z80scc.cpp:1465).
uint8_t Scc8530Device::doRr2(int index)
{
    // Assume the unmodified value in polled mode.
    ch_[index].rr2 = ch_[CHAN_A].wr2;

    // Channel B always returns the modified vector, VIS or not.
    if (index == CHAN_B) {
        int i = 0;
        for (uint8_t& elem : intState_) {
            if (elem & kDaisyInt) {
                ch_[index].rr2 = modifyVector(ch_[index].rr2,
                                              i < 3 ? CHAN_A : CHAN_B,
                                              static_cast<uint8_t>(intSource_[i] & 3));
                // MAME acks here only for ESCC/CMOS parts with WR9 IACK
                // set; the NMOS 8530 does not.
                break;
            }
            i++;
        }
    }
    return ch_[index].rr2;
}

/// MAME `z80scc_channel::do_sccreg_rr3` (z80scc.cpp:1508). RR3 exists only
/// in channel A; channel B reads back all zeroes.
uint8_t Scc8530Device::doRr3(int index) const
{
    return (index == CHAN_A) ? static_cast<uint8_t>(ch_[CHAN_A].rr3 & 0x3f) : 0;
}

// ─────────────────────────────────────────────────────────────────────────
//  Write registers
// ─────────────────────────────────────────────────────────────────────────

/// MAME `z80scc_channel::scc_register_write` (z80scc.cpp:2288).
void Scc8530Device::registerWrite(int index, uint8_t reg, uint8_t data)
{
    Channel& c = ch_[index];
    switch (reg) {
    case 0:  doWr0(index, data); break;
    case 1:  doWr1(index, data); checkInterrupts(); break;
    case 2:  doWr2(index, data); break;
    case 3:  doWr3(index, data); break;
    case 4:  doWr4(index, data); break;
    case 5:  doWr5(index, data); break;
    case 6:  // do_sccreg_wr6 — transmit sync / SDLC address
        c.syncPattern = static_cast<uint16_t>((c.syncPattern & 0xff00) | data);
        break;
    case 7:  // do_sccreg_wr7 — receive sync / SDLC flag. WR7' is 85C30/ESCC
             // only, so the NMOS part always lands in the sync-pattern leg.
        c.syncPattern = static_cast<uint16_t>((data << 8) | (c.syncPattern & 0xff));
        break;
    case 8:  dataWrite(index, data); break;
    case 9:  doWr9(index, data); break;
    case 10: c.wr10 = data; break;   // MAME: "not implemented" beyond latching
    case 11: c.wr11 = data; updateSerial(index); break;
    case 12: c.wr12 = data; updateSerial(index); break;
    case 13: c.wr13 = data; updateSerial(index); break;
    case 14: doWr14(index, data); break;
    case 15: doWr15(index, data); break;
    default: break;
    }
}

/// MAME `z80scc_channel::do_sccreg_wr0` (z80scc.cpp:1741).
void Scc8530Device::doWr0(int index, uint8_t data)
{
    Channel& c = ch_[index];
    c.wr0 = data;

    wr0PtrBits_ = static_cast<uint8_t>(data & WR0_REGISTER_MASK);

    switch (data & WR0_COMMAND_MASK) {
    case WR0_POINT_HIGH:
        // Adds eight to the register pointer so WR8-WR15 can be reached.
        wr0PtrBits_ |= 8;
        break;

    case WR0_RESET_EXT_STATUS:
        // Release the latch if no other external/status source is active.
        c.extIntLatch = static_cast<uint8_t>(updateExtInt(index));
        if (c.extIntLatch == 0)
            checkInterrupts();
        break;

    case WR0_RESET_HIGHEST_IUS:
        for (uint8_t& elem : intState_) {
            if (elem & kDaisyIeo) {
                elem = 0;
                checkInterrupts();
                break;
            }
        }
        // re-assert the interrupt if the condition is still present
        checkReceiveInterrupt(index);
        break;

    case WR0_ERROR_RESET:
        // Reset the error state in the FIFO and unlock it: unlock == step
        // to the next slot.
        // SDLC (datasheet, not MAME): End Of Frame is a special receive
        // condition, so this is also what clears RR1 D7 and releases the
        // FIFO lock the frame's last byte took.
        c.rr1 &= static_cast<uint8_t>(~(RR1_END_OF_FRAME | RR1_RESIDUE_CODE_MASK));
        c.rr1 |= 0x06;      // the reset residue value (z80scc.cpp:1152)
        if (c.rxFifoWp != c.rxFifoRp) {
            c.rxEof[c.rxFifoRp] = false;
            rxFifoRpStep(index);
        }
        break;

    case WR0_SEND_ABORT:
        // SDLC (datasheet, not MAME): flush the transmitter and send 8-13
        // '1' bits. The frame in flight is destroyed, not delivered.
        c.txFrame.clear();
        c.txHalfBits = 0;
        c.rr0 |= RR0_TX_UNDERRUN;
        c.rr1 |= RR1_ALL_SENT;
        break;

    case WR0_NULL:
        break;

    case WR0_ENABLE_INT_NEXT_RX:
        c.rxFirst = 1;
        break;

    case WR0_RESET_TX_INT:
        c.txIntDisarm = 1;
        intState_[INT_TRANSMIT_PRIO + (index == CHAN_A ? 0 : 3)] = 0;
        ch_[CHAN_A].rr3 &= static_cast<uint8_t>(
            ~(1 << (INT_TRANSMIT_PRIO + ((index == CHAN_A) ? 3 : 0))));
        checkInterrupts();
        break;

    default:
        break;
    }

    // The CRC initialisation codes in D7/D6 are all "not implemented" in
    // MAME (z80scc.cpp:1840-1856). Only the last one has an effect POM2 can
    // carry at a byte seam, and it is the one that matters:
    //
    // SDLC (datasheet, not MAME) — "Reset Tx Underrun/EOM Latch". The driver
    // issues it right after loading the first byte of a frame, which arms
    // the underrun at the END of that frame to mean End Of Message. Without
    // it the transmitter would just idle and no frame would ever close.
    if ((data & 0xC0) == 0xC0) {
        c.rr0 &= static_cast<uint8_t>(~RR0_TX_UNDERRUN);
        if (sdlcMode(index) && c.txFrame.empty())
            c.rr1 &= static_cast<uint8_t>(~RR1_ALL_SENT);
    }

    wr0PtrBits_ &= static_cast<uint8_t>(~WR0_REGISTER_MASK);
    wr0PtrBits_ |= static_cast<uint8_t>(c.wr0 & WR0_REGISTER_MASK);
}

/// MAME `z80scc_channel::do_sccreg_wr1` (z80scc.cpp:1867).
void Scc8530Device::doWr1(int index, uint8_t data)
{
    ch_[index].wr1 = data;
    checkDmaRequest(index);
    checkInterrupts();
}

/// MAME `z80scc_channel::do_sccreg_wr2` (z80scc.cpp:1910). One vector
/// register per device, written from either channel.
void Scc8530Device::doWr2(int /*index*/, uint8_t data)
{
    ch_[CHAN_A].wr2 = data;
    ch_[CHAN_A].rr2 = data;
    ch_[CHAN_B].rr2 = data;
    checkInterrupts();
}

/// MAME `z80scc_channel::do_sccreg_wr3` (z80scc.cpp:1923).
void Scc8530Device::doWr3(int index, uint8_t data)
{
    Channel& c = ch_[index];
    c.wr3 = data;

    if ((c.wr3 & WR3_ENTER_HUNT_MODE) || ((c.wr3 & WR3_RX_ENABLE) == 0))
        c.rr0 |= RR0_SYNC_HUNT;

    updateSerial(index);
    // MAME also calls receive_register_reset() here; the byte-granular
    // receiver has no partial state to throw away.
}

/// MAME `z80scc_channel::do_sccreg_wr4` (z80scc.cpp:1951).
void Scc8530Device::doWr4(int index, uint8_t data)
{
    Channel& c = ch_[index];
    if (data == c.wr4)
        return;     // MAME suppresses the Tx re-init on an identical write
    c.wr4 = data;
    updateSerial(index);
    // safe_transmit_register_reset() — MAME logs when the shifter is busy
    c.txHalfBits = 0;
}

/// MAME `z80scc_channel::do_sccreg_wr5` (z80scc.cpp:1973).
void Scc8530Device::doWr5(int index, uint8_t data)
{
    Channel& c = ch_[index];
    if (data == c.wr5)
        return;
    c.wr5 = data;
    updateSerial(index);
    c.txHalfBits = 0;       // safe_transmit_register_reset()
    updateRts(index);       // also updates DTR
    checkDmaRequest(index);
}

/// MAME `z80scc_channel::do_sccreg_wr9` (z80scc.cpp:2015).
void Scc8530Device::doWr9(int /*index*/, uint8_t data)
{
    wr9_ = data;    // one WR9, written from either channel

    switch (data & WR9_CMD_MASK) {
    case WR9_CMD_NORESET:
        break;
    case WR9_CMD_CHNB_RESET:
        channelReset(CHAN_B);
        break;
    case WR9_CMD_CHNA_RESET:
        channelReset(CHAN_A);
        break;
    case WR9_CMD_HW_RESET:
        // "Identical to a hardware reset, except that the Shift Right /
        // Shift Left bit is not changed and the MIE, Status High/Status
        // Low and DLC bits take the programmed values." The Shift bits are
        // Z80X30-only, so only the second clause applies here.
        reset();
        wr9_ &= static_cast<uint8_t>(~(WR9_BIT_MIE | WR9_BIT_SHSL | WR9_BIT_DLC));
        wr9_ |= static_cast<uint8_t>(data & (WR9_BIT_MIE | WR9_BIT_SHSL | WR9_BIT_DLC));
        break;
    default:
        break;
    }
}

/// MAME `z80scc_channel::do_sccreg_wr14` (z80scc.cpp:2177). The DPLL
/// commands in D7-D5 are all logged as "not implemented" by MAME; the BRG
/// enable in D0 is the half that has behaviour.
void Scc8530Device::doWr14(int index, uint8_t data)
{
    Channel& c = ch_[index];
    bool brgChange = false;

    if (!(c.wr14 & WR14_BRG_ENABLE) && (data & WR14_BRG_ENABLE)) {
        brgChange = true;
    } else if ((c.wr14 & WR14_BRG_ENABLE) && !(data & WR14_BRG_ENABLE)) {
        c.brgTimerRate = 0;     // stop the baud-rate generator
        c.brgAcc = 0;
    }

    c.wr14 = data;
    updateSerial(index);
    if (brgChange)
        updateBaudTimer(index);
}

/// MAME `z80scc_channel::do_sccreg_wr15` (z80scc.cpp:2267).
void Scc8530Device::doWr15(int index, uint8_t data)
{
    Channel& c = ch_[index];
    const uint8_t old = c.wr15;
    c.wr15 = data;
    if ((old & WR15_ZEROCOUNT) != (c.wr15 & WR15_ZEROCOUNT))
        updateBaudTimer(index);
}

// ─────────────────────────────────────────────────────────────────────────
//  Data registers
// ─────────────────────────────────────────────────────────────────────────

/// MAME `z80scc_channel::data_read` (z80scc.cpp:2362).
uint8_t Scc8530Device::dataRead(int channel)
{
    const int index = channel & 1;
    Channel& c = ch_[index];
    uint8_t data = 0;

    if (c.rxFifoWp != c.rxFifoRp) {
        data = c.rxData[c.rxFifoRp];

        c.rr1 = static_cast<uint8_t>(
            (c.rr1 & ~(RR1_CRC_FRAMING_ERROR | RR1_RX_OVERRUN_ERROR | RR1_PARITY_ERROR)) |
            c.rxError[c.rxFifoRp]);

        // SDLC (datasheet, not MAME): the frame's last byte carries End Of
        // Frame and its residue code. It rides beside the slot because the
        // mask above — MAME's — would drop it.
        bool eof = false;
        if (c.rxEof[c.rxFifoRp]) {
            eof = true;
            c.rr1 |= RR1_END_OF_FRAME;
            // Whole bytes only at this seam, so the residue is the "no
            // residue, 8-bit character" code.
            c.rr1 &= static_cast<uint8_t>(~RR1_RESIDUE_CODE_MASK);
            c.rr1 |= 0x06;
        }

        // A special condition locks the FIFO to preserve the status; the
        // Error Reset command in WR0 is what unlocks it. End Of Frame is one
        // of them (SDLC, datasheet).
        if (eof || (c.rr1 & (RR1_CRC_FRAMING_ERROR | RR1_RX_OVERRUN_ERROR |
                     ((c.wr1 & WR1_PARITY_IS_SPEC_COND) ? RR1_PARITY_ERROR : 0)))) {
            triggerInterrupt(index, INT_SPECIAL);
        } else {
            rxFifoRpStep(index);

            if (c.rxFifoWp == c.rxFifoRp) {
                intState_[INT_RECEIVE_PRIO + (index == CHAN_A ? 0 : 3)] = 0;
                ch_[CHAN_A].rr3 &= static_cast<uint8_t>(
                    ~(1 << (INT_RECEIVE_PRIO + ((index == CHAN_A) ? 3 : 0))));
                checkInterrupts();
            }
        }
        checkDmaRequest(index);
    }
    // else: MAME logs "Attempt to read out character from empty FIFO"
    return data;
}

/// MAME `z80scc_channel::data_write` (z80scc.cpp:2457).
void Scc8530Device::dataWrite(int channel, uint8_t data)
{
    const int index = channel & 1;
    Channel& c = ch_[index];

    const bool full = !(c.rr0 & RR0_TX_BUFFER_EMPTY) &&
                      ((c.txFifoWp + 1 == c.txFifoRp) ||
                       ((c.txFifoWp + 1 == Channel::kTxFifoSz) && (c.txFifoRp == 0)));

    if (!full) {
        c.txData[c.txFifoWp++] = data;
        if (c.txFifoWp >= Channel::kTxFifoSz)
            c.txFifoWp = 0;

        // One FIFO slot on the NMOS part, so writing one byte fills it.
        c.rr0 &= static_cast<uint8_t>(~RR0_TX_BUFFER_EMPTY);
        c.txIntDisarm = 1;
        intState_[INT_TRANSMIT_PRIO + (index == CHAN_A ? 0 : 3)] = 0;
        ch_[CHAN_A].rr3 &= static_cast<uint8_t>(
            ~(1 << (INT_TRANSMIT_PRIO + ((index == CHAN_A) ? 3 : 0))));
        checkInterrupts();
    }

    checkDmaRequest(index);

    // A byte has just been loaded, so a preceding "Reset Tx Int Pending"
    // no longer applies.
    c.txIntDisarm = 0;

    if (c.wr5 & WR5_TX_ENABLE) {
        if (c.txHalfBits == 0) {    // is_transmit_register_empty()
            c.txShiftData = c.txData[c.txFifoRp];
            c.txHalfBits = frameHalfBits(index);
            txFifoRpStep(index);
            c.rr1 &= static_cast<uint8_t>(~RR1_ALL_SENT);
            c.rr0 |= RR0_TX_BUFFER_EMPTY;   // a slot is free again
        }
    }

    if (c.wr1 & WR1_TX_INT_ENABLE) {
        if (c.rr0 & RR0_TX_BUFFER_EMPTY)
            triggerInterrupt(index, INT_TRANSMIT);
    }
}

/// MAME `z80scc_channel::m_rx_fifo_rp_step` (z80scc.cpp:2423).
void Scc8530Device::rxFifoRpStep(int index)
{
    Channel& c = ch_[index];
    c.rxFifoRp++;
    if (c.rxFifoRp >= Channel::kRxFifoSz)
        c.rxFifoRp = 0;
    if (c.rxFifoRp == c.rxFifoWp)
        c.rr0 &= static_cast<uint8_t>(~RR0_RX_CHAR_AVAILABLE);
}

/// MAME `z80scc_channel::m_tx_fifo_rp_step` (z80scc.cpp:2440).
void Scc8530Device::txFifoRpStep(int index)
{
    Channel& c = ch_[index];
    c.txFifoRp++;
    if (c.txFifoRp >= Channel::kTxFifoSz)
        c.txFifoRp = 0;
}

/// MAME `z80scc_channel::receive_data` (z80scc.cpp:2566).
void Scc8530Device::receiveData(int index, uint8_t data)
{
    Channel& c = ch_[index];

    if (c.rxFifoWp + 1 == c.rxFifoRp ||
        ((c.rxFifoWp + 1 == Channel::kRxFifoSz) && (c.rxFifoRp == 0))) {
        // Overrun: store the character but do not step the FIFO.
        c.rxError[c.rxFifoWp] |= RR1_RX_OVERRUN_ERROR;
        c.rxData[c.rxFifoWp] = data;
    } else {
        c.rxError[c.rxFifoWp] &= static_cast<uint8_t>(~RR1_RX_OVERRUN_ERROR);
        c.rxData[c.rxFifoWp] = data;
        c.rxEof[c.rxFifoWp] = false;
        c.rxFifoWp++;
        if (c.rxFifoWp >= Channel::kRxFifoSz)
            c.rxFifoWp = 0;
    }

    c.rr0 |= RR0_RX_CHAR_AVAILABLE;
    checkDmaRequest(index);
    checkReceiveInterrupt(index);
}

/// MAME `z80scc_channel::rcv_callback` + `rcv_complete`
/// (z80scc.cpp:1286 / 1303): the receiver only shifts while WR3 D0 is set.
void Scc8530Device::receiveByte(int channel, uint8_t data)
{
    const int index = channel & 1;
    if (!(ch_[index].wr3 & WR3_RX_ENABLE))
        return;     // MAME logs "Received data bit but receiver is disabled"
    receiveData(index, data);
}

/// MAME `z80scc_channel::check_receive_interrupt` (z80scc.cpp:3038).
void Scc8530Device::checkReceiveInterrupt(int index)
{
    Channel& c = ch_[index];
    if (!(c.rr0 & RR0_RX_CHAR_AVAILABLE))
        return;

    switch (c.wr1 & WR1_RX_INT_MODE_MASK) {
    case WR1_RX_INT_FIRST:
        if (c.rxFirst) {
            triggerInterrupt(index, INT_RECEIVE);
            c.rxFirst = 0;
        }
        break;
    case WR1_RX_INT_ALL:
        triggerInterrupt(index, INT_RECEIVE);
        break;
    default:
        break;
    }
}

/// MAME `z80scc_channel::check_dma_request` (z80scc.cpp:3015).
void Scc8530Device::checkDmaRequest(int index)
{
    Channel& c = ch_[index];

    if (c.wr14 & WR14_DTR_REQ_FUNC)
        setDtr(index, (c.rr0 & RR0_TX_BUFFER_EMPTY) ? 0 : 1);

    if ((c.wr1 & WR1_WREQ_ENABLE) && (c.wr1 & WR1_WREQ_FUNCTION)) {
        if (!wreqCb_) return;
        if (c.wr1 & WR1_WREQ_ON_RX_TX)
            wreqCb_(index, (c.rr0 & RR0_RX_CHAR_AVAILABLE) ? false : true);
        else
            wreqCb_(index, ((c.rr0 & RR0_TX_BUFFER_EMPTY) && (c.wr5 & WR5_TX_ENABLE))
                               ? false : true);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Modem-control pins
// ─────────────────────────────────────────────────────────────────────────

/// MAME `z80scc_channel::cts_w` (z80scc.cpp:2604).
void Scc8530Device::ctsW(int channel, bool state)
{
    const int index = channel & 1;
    Channel& c = ch_[index];

    if ((c.rr0 & RR0_CTS) != (state ? 0 : RR0_CTS)) {   // change detection
        if (!state && (c.wr3 & WR3_AUTO_ENABLES))
            c.wr5 |= WR5_TX_ENABLE;

        if (state) c.rr0 &= static_cast<uint8_t>(~RR0_CTS);
        else       c.rr0 |= RR0_CTS;

        if (c.extIntLatch == 0 && (c.wr1 & WR1_EXT_INT_ENABLE) && (c.wr15 & WR15_CTS)) {
            triggerInterrupt(index, INT_EXTERNAL);
            c.extIntLatch = 1;
            c.extIntStates = c.rr0;
        }
    }
}

/// MAME `z80scc_channel::dcd_w` (z80scc.cpp:2641).
void Scc8530Device::dcdW(int channel, bool state)
{
    const int index = channel & 1;
    Channel& c = ch_[index];

    if ((c.rr0 & RR0_DCD) != (state ? 0 : RR0_DCD)) {
        if (!state && (c.wr3 & WR3_AUTO_ENABLES))
            c.wr3 |= WR3_RX_ENABLE;

        if (state) c.rr0 &= static_cast<uint8_t>(~RR0_DCD);
        else       c.rr0 |= RR0_DCD;

        if (c.extIntLatch == 0 && (c.wr1 & WR1_EXT_INT_ENABLE) && (c.wr15 & WR15_DCD)) {
            c.extIntLatch = 1;
            c.extIntStates = c.rr0;
            triggerInterrupt(index, INT_EXTERNAL);
        }
    }
}

/// MAME `z80scc_channel::sync_w` (z80scc.cpp:2680). With the crystal
/// oscillator selected in WR11 D7 the pin does not exist; MAME calls
/// `fatalerror` there, POM2 ignores the edge.
void Scc8530Device::syncW(int channel, bool state)
{
    const int index = channel & 1;
    Channel& c = ch_[index];

    if (c.wr11 & WR11_RCVCLK_TYPE)
        return;

    if ((c.rr0 & RR0_SYNC_HUNT) != (state ? 0 : RR0_SYNC_HUNT)) {
        if (state) c.rr0 &= static_cast<uint8_t>(~RR0_SYNC_HUNT);
        else       c.rr0 |= RR0_SYNC_HUNT;

        if (c.extIntLatch == 0 && (c.wr1 & WR1_EXT_INT_ENABLE) && (c.wr15 & WR15_SYNC)) {
            c.extIntLatch = 1;
            c.extIntStates = c.rr0;
            triggerInterrupt(index, INT_EXTERNAL);
        }
    }
}

/// MAME `z80scc_channel::set_rts` (z80scc.cpp:1346).
void Scc8530Device::setRts(int index, int state)
{
    if (rtsCb_) rtsCb_(index, state != 0);
}

/// MAME `z80scc_channel::set_dtr` (z80scc.cpp:2967).
void Scc8530Device::setDtr(int index, int state)
{
    ch_[index].dtr = state;
    if (dtrCb_) dtrCb_(index, state != 0);
}

/// MAME `z80scc_channel::update_rts` (z80scc.cpp:1352).
void Scc8530Device::updateRts(int index)
{
    Channel& c = ch_[index];

    if (c.wr5 & WR5_RTS) {
        c.rts = 1;
        setRts(index, !c.rts);
    } else {
        c.rts = 0;
        if (!(c.wr3 & WR3_AUTO_ENABLES) || (c.rr1 & RR1_ALL_SENT))
            setRts(index, !c.rts);
    }

    if (!(c.wr14 & WR14_DTR_REQ_FUNC))
        setDtr(index, (c.wr5 & WR5_DTR) ? 0 : 1);
}

// ─────────────────────────────────────────────────────────────────────────
//  Clocks and framing
// ─────────────────────────────────────────────────────────────────────────

/// MAME `z80scc_channel::get_clock_mode` (z80scc.cpp:1320).
int Scc8530Device::clockMode(int index) const
{
    switch (ch_[index].wr4 & WR4_CLOCK_RATE_MASK) {
    case WR4_CLOCK_RATE_X1:  return 1;
    case WR4_CLOCK_RATE_X16: return 16;
    case WR4_CLOCK_RATE_X32: return 32;
    case WR4_CLOCK_RATE_X64: return 64;
    default:                 return 1;
    }
}

/// MAME `z80scc_channel::get_rx_word_length` (z80scc.cpp:1396).
int Scc8530Device::rxWordLength(int index) const
{
    switch (ch_[index].wr3 & WR3_RX_WORD_LENGTH_MASK) {
    case WR3_RX_WORD_LENGTH_5: return 5;
    case WR3_RX_WORD_LENGTH_6: return 6;
    case WR3_RX_WORD_LENGTH_7: return 7;
    case WR3_RX_WORD_LENGTH_8: return 8;
    default:                   return 5;
    }
}

/// MAME `z80scc_channel::get_tx_word_length` (z80scc.cpp:1415).
int Scc8530Device::txWordLength(int index) const
{
    switch (ch_[index].wr5 & WR5_TX_WORD_LENGTH_MASK) {
    case WR5_TX_WORD_LENGTH_5: return 5;
    case WR5_TX_WORD_LENGTH_6: return 6;
    case WR5_TX_WORD_LENGTH_7: return 7;
    case WR5_TX_WORD_LENGTH_8: return 8;
    default:                   return 5;
    }
}

/// The frame `update_serial` hands diserial, measured in half-bit periods:
/// `set_data_frame(1 start, rxWordLength, parity, stopBits)`
/// (z80scc.cpp:2882). Half-bits because WR4 can ask for 1.5 stop bits.
/// Note MAME passes the *receive* word length for both directions, and
/// keeps the start bit even in the sync modes where WR4 D3-D2 = 00 — this
/// port reproduces both, rather than second-guessing the oracle.
int Scc8530Device::frameHalfBits(int index) const
{
    const Channel& c = ch_[index];

    // SDLC (datasheet, not MAME): a character on an SDLC line is exactly its
    // data bits — no start bit, no stop bit, no parity. MAME's
    // `update_serial` always passes a start bit to diserial, which is
    // harmless there because MAME does not implement SDLC at all; here it
    // would make every byte 9/8 too long and put the transmit timing out by
    // 12 %, which a 230.4 kbit/s LocalTalk driver would notice.
    if (sdlcMode(index))
        return 2 * rxWordLength(index);

    int halfBits = 2 * (1 + rxWordLength(index) + ((c.wr4 & WR4_PARITY_ENABLE) ? 1 : 0));

    switch (c.wr4 & WR4_STOP_BITS_MASK) {
    case WR4_STOP_BITS_1:   halfBits += 2; break;
    case WR4_STOP_BITS_1_5: halfBits += 3; break;
    case WR4_STOP_BITS_2:   halfBits += 4; break;
    default:                break;  // sync modes: no stop bit
    }
    return halfBits;
}

/// MAME `z80scc_channel::get_brg_rate` (z80scc.cpp:2789).
unsigned Scc8530Device::brgRateOf(int index) const
{
    const Channel& c = ch_[index];
    const unsigned brgConst = 2u + static_cast<unsigned>((c.wr13 << 8) | c.wr12);
    const unsigned src = (c.wr14 & WR14_BRG_SOURCE) ? pclk_ : c.rtxc;
    const unsigned rate = src / (brgConst == 0 ? 1 : brgConst);
    return rate / (2u * static_cast<unsigned>(clockMode(index)));
}

/// MAME `z80scc_channel::get_rtxc_rate` (z80scc.cpp:2813).
unsigned Scc8530Device::rtxcRateOf(int index) const
{
    return ch_[index].rtxc / static_cast<unsigned>(clockMode(index));
}

/// MAME `z80scc_channel::update_baudtimer` (z80scc.cpp:2822). The timer
/// only runs when the BRG is enabled AND WR15's Zero Count interrupt is
/// armed; its period is the *undivided* BRG output.
void Scc8530Device::updateBaudTimer(int index)
{
    Channel& c = ch_[index];

    if (!(c.wr14 & WR14_BRG_ENABLE) || !(c.wr15 & WR15_ZEROCOUNT)) {
        c.brgTimerRate = 0;
        c.brgAcc = 0;
        return;
    }

    const unsigned brgConst = 2u + static_cast<unsigned>((c.wr13 << 8) | c.wr12);
    const unsigned src = (c.wr14 & WR14_BRG_SOURCE) ? pclk_ : c.rtxc;
    c.brgTimerRate = src / (brgConst == 0 ? 1 : brgConst);
}

/// MAME `z80scc_channel::update_serial` (z80scc.cpp:2858).
void Scc8530Device::updateSerial(int index)
{
    Channel& c = ch_[index];

    c.brgRate = (c.wr14 & WR14_BRG_ENABLE) ? brgRateOf(index) : 0;

    switch (c.wr11 & WR11_RCVCLK_SRC_MASK) {
    case WR11_RCVCLK_SRC_RTXC: c.rcvRate = rtxcRateOf(index); break;
    case WR11_RCVCLK_SRC_BR:   c.rcvRate = c.brgRate;         break;
    default:                   c.rcvRate = 0;                 break;  // TRxC / DPLL
    }

    switch (c.wr11 & WR11_TRACLK_SRC_MASK) {
    case WR11_TRACLK_SRC_RTXC:
        c.traRate = rtxcRateOf(index);
        break;
    case WR11_TRACLK_SRC_BR:
        if (c.txHalfBits == 0) {
            c.traRate = c.brgRate;
        } else {
            // Delayed baud-rate change: the SCC applies it at the end of
            // the character in flight (z80scc.cpp:1221).
            c.delayedTxBrgChange = 1;
        }
        break;
    default:
        c.traRate = 0;
        break;
    }

    updateBaudTimer(index);
}

/// MAME `z80scc_channel::tra_complete` (z80scc.cpp:1218): the shift
/// register has finished the character on the wire.
void Scc8530Device::traComplete(int index)
{
    Channel& c = ch_[index];

    if (c.delayedTxBrgChange == 1) {
        c.delayedTxBrgChange = 0;
        c.traRate = c.brgRate;
    }

    const uint8_t sent = c.txShiftData;
    const bool sdlc = sdlcMode(index);

    if ((c.wr5 & WR5_TX_ENABLE) && !(c.wr5 & WR5_SEND_BREAK)) {
        // SDLC (datasheet, not MAME): in a framed mode the byte belongs to
        // the frame, not to the host — `frameCb_` delivers it when the frame
        // closes. In async the byte goes out as MAME sends it.
        if (sdlc) {
            if (c.txFrame.size() >= kMaxTxFrameBytes) {
                // SDLC (datasheet, not MAME): past any frame this seam can
                // deliver. Real silicon would keep streaming — it buffers
                // nothing — but POM2 must not grow a vector for as long as a
                // driver keeps feeding it, so the frame is ABORTED exactly the
                // way WR0's Send Abort aborts one: destroyed rather than
                // delivered, with the Tx Underrun/EOM latch set so the next
                // status read shows the transmitter is no longer inside a
                // frame. See kMaxTxFrameBytes for why 1024.
                c.txFrame.clear();
                c.rr0 |= RR0_TX_UNDERRUN;
            } else {
                c.txFrame.push_back(sent);
            }
        }
        else if (txCb_) txCb_(index, sent);

        if (c.wr14 & WR14_LOCAL_LOOPBACK)
            receiveByte(index, sent);

        if ((c.rr0 & RR0_TX_BUFFER_EMPTY) == 0 || c.txFifoRp != c.txFifoWp) {
            // Reload the shift register from the transmit buffer.
            c.txShiftData = c.txData[c.txFifoRp];
            c.txHalfBits = frameHalfBits(index);
            txFifoRpStep(index);
            c.rr0 |= RR0_TX_BUFFER_EMPTY;
        } else {
            c.rr1 |= RR1_ALL_SENT;
            // SDLC (datasheet, not MAME): the transmitter has underrun. If
            // the Tx Underrun/EOM latch is still clear the driver armed it
            // for exactly this moment — that is End Of Message, so the CRC
            // goes out and the frame closes.
            if (sdlc && !(c.rr0 & RR0_TX_UNDERRUN)) {
                closeTxFrame(index);
                c.rr0 |= RR0_TX_UNDERRUN;
                if (c.extIntLatch == 0 && (c.wr1 & WR1_EXT_INT_ENABLE) &&
                    (c.wr15 & WR15_TX_EOM)) {
                    c.extIntLatch = 1;
                    c.extIntStates = c.rr0;
                    triggerInterrupt(index, INT_EXTERNAL);
                }
            }
            // With the RTS bit reset, /RTS goes high once the transmitter
            // has emptied.
            if (!c.rts)
                setRts(index, 1);
        }

        checkDmaRequest(index);

        if ((c.wr1 & WR1_TX_INT_ENABLE) && c.txIntDisarm == 0) {
            if (c.rr0 & RR0_TX_BUFFER_EMPTY)
                triggerInterrupt(index, INT_TRANSMIT);
        }
        c.txIntDisarm = 0;
    }
}

void Scc8530Device::tick(uint64_t pclkCycles)
{
    if (pclkCycles == 0) return;

    for (int index = 0; index < 2; ++index) {
        Channel& c = ch_[index];

        // Transmit shift register, clocked in half-bit periods so a 1.5
        // stop-bit frame lands on an exact boundary.
        if (c.txHalfBits > 0 && c.traRate > 0) {
            c.txAcc += pclkCycles * static_cast<uint64_t>(c.traRate) * 2u;
            while (c.txAcc >= pclk_ && c.txHalfBits > 0) {
                c.txAcc -= pclk_;
                if (--c.txHalfBits == 0)
                    traComplete(index);
            }
            if (c.txHalfBits == 0)
                c.txAcc = 0;
        }

        // MAME `TIMER_CALLBACK_MEMBER(z80scc_channel::brg_tick)`
        // (z80scc.cpp:1171): WR15's Zero Count is implied by the timer
        // running at all.
        //
        // The BRG can be programmed faster than the caller's tick
        // granularity — a time constant of 0 makes it PCLK/2 — so the
        // elapsed periods are counted and the interrupt raised ONCE rather
        // than looped. The Zero Count interrupt is a level (an IP bit in
        // RR3), not a counter, so N raises and one raise are
        // indistinguishable to the guest, and the loop cannot run away.
        if (c.brgTimerRate > 0) {
            c.brgAcc += pclkCycles * static_cast<uint64_t>(c.brgTimerRate);
            if (c.brgAcc >= pclk_) {
                c.brgAcc %= pclk_;
                triggerInterrupt(index, INT_EXTERNAL);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  SDLC (datasheet, not MAME) — Zilog SCC/ESCC user manual UM010902
// ─────────────────────────────────────────────────────────────────────────

/// WR4 D5-D4 = 10 with the stop-bit field zero: SDLC/HDLC.
bool Scc8530Device::sdlcMode(int channel) const
{
    const Channel& c = ch_[channel & 1];
    return (c.wr4 & WR4_STOP_BITS_MASK) == 0 &&
           (c.wr4 & WR4_SYNC_MODE_MASK) == 0x20;
}

std::size_t Scc8530Device::txFrameSize(int channel) const
{
    return ch_[channel & 1].txFrame.size();
}

/// The transmitter underran with the Tx Underrun/EOM latch clear, which in
/// SDLC is End Of Message: the CRC goes out and the closing flag follows.
/// At a byte seam the CRC is a guarantee rather than two bytes, so what the
/// host receives is the payload.
void Scc8530Device::closeTxFrame(int index)
{
    Channel& c = ch_[index];
    if (c.txFrame.empty()) return;
    if (frameCb_) frameCb_(index, c.txFrame);
    c.txFrame.clear();
}

void Scc8530Device::receiveFrame(int channel, const uint8_t* data,
                                 std::size_t len, bool crcError)
{
    const int index = channel & 1;
    Channel& c = ch_[index];

    if (!(c.wr3 & WR3_RX_ENABLE)) return;   // receiver off: no flag is seen
    if (!sdlcMode(index)) return;           // and no frames outside SDLC
    if (!data || len == 0) return;

    // Address search (WR3 D2): in SDLC the receiver only opens for frames
    // addressed to WR6 or to the $FF broadcast, and stays in hunt otherwise.
    if ((c.wr3 & WR3_ADDRESS_SEARCH_MODE) &&
        data[0] != static_cast<uint8_t>(c.syncPattern & 0xFF) && data[0] != 0xFF)
        return;

    // The opening flag clears Sync/Hunt, and once cleared in SDLC it stays
    // cleared until the receiver is disabled or Enter Hunt is commanded.
    if (c.rr0 & RR0_SYNC_HUNT) {
        c.rr0 &= static_cast<uint8_t>(~RR0_SYNC_HUNT);
        if (c.extIntLatch == 0 && (c.wr1 & WR1_EXT_INT_ENABLE) &&
            (c.wr15 & WR15_SYNC)) {
            c.extIntLatch = 1;
            c.extIntStates = c.rr0;
            triggerInterrupt(index, INT_EXTERNAL);
        }
    }

    for (std::size_t i = 0; i < len; ++i) {
        const int slot = c.rxFifoWp;
        receiveData(index, data[i]);
        if (i + 1 == len) {
            // The last byte carries End Of Frame, and the FCS verdict with
            // it. `receiveData` may have refused to step the pointer on an
            // overrun, in which case the flag still belongs to that slot.
            c.rxEof[slot] = true;
            if (crcError)
                c.rxError[slot] |= RR1_CRC_FRAMING_ERROR;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Inspection
// ─────────────────────────────────────────────────────────────────────────

uint8_t Scc8530Device::peekWr(int channel, int reg) const
{
    const Channel& c = ch_[channel & 1];
    switch (reg) {
    case 0:  return c.wr0;
    case 1:  return c.wr1;
    case 2:  return ch_[CHAN_A].wr2;
    case 3:  return c.wr3;
    case 4:  return c.wr4;
    case 5:  return c.wr5;
    case 6:  return static_cast<uint8_t>(c.syncPattern & 0xff);
    case 7:  return static_cast<uint8_t>(c.syncPattern >> 8);
    case 9:  return wr9_;
    case 10: return c.wr10;
    case 11: return c.wr11;
    case 12: return c.wr12;
    case 13: return c.wr13;
    case 14: return c.wr14;
    case 15: return c.wr15;
    default: return 0;
    }
}

uint8_t Scc8530Device::peekRr(int channel, int reg) const
{
    const Channel& c = ch_[channel & 1];
    switch (reg) {
    case 0:  return doRr0(channel & 1);
    case 1:  return c.rr1;
    case 2:  return c.rr2;
    case 3:  return doRr3(channel & 1);
    case 10: return c.rr10;
    case 12: return c.wr12;
    case 13: return c.wr13;
    case 15: return static_cast<uint8_t>(c.wr15 & 0xfa);
    default: return 0;
    }
}

uint32_t Scc8530Device::txRate(int channel) const { return ch_[channel & 1].traRate; }
uint32_t Scc8530Device::rxRate(int channel) const { return ch_[channel & 1].rcvRate; }
bool Scc8530Device::txBusy(int channel) const { return ch_[channel & 1].txHalfBits > 0; }

int Scc8530Device::rxFifoCount(int channel) const
{
    const Channel& c = ch_[channel & 1];
    int n = c.rxFifoWp - c.rxFifoRp;
    if (n < 0) n += Channel::kRxFifoSz;
    return n;
}

// ─────────────────────────────────────────────────────────────────────────
//  Snapshot
// ─────────────────────────────────────────────────────────────────────────

namespace {
constexpr uint32_t kSnapMagic   = 0x53434331u;  // "SCC1"
constexpr uint8_t  kSnapVersion = 2;
} // namespace

void Scc8530Device::appendSnapshot(std::vector<uint8_t>& out) const
{
    byteio::putU32(out, kSnapMagic);
    byteio::putU8 (out, kSnapVersion);

    byteio::putU8 (out, wr9_);
    byteio::putU8 (out, wr0PtrBits_);
    byteio::putU8 (out, outIntState_ ? 1 : 0);
    for (uint8_t v : intState_)  byteio::putU8(out, v);
    for (int v : intSource_)     byteio::putU8(out, static_cast<uint8_t>(v));

    for (const Channel& c : ch_) {
        byteio::putU8(out, c.rr0);  byteio::putU8(out, c.rr1);
        byteio::putU8(out, c.rr2);  byteio::putU8(out, c.rr3);
        byteio::putU8(out, c.rr10);
        byteio::putU8(out, c.wr0);  byteio::putU8(out, c.wr1);
        byteio::putU8(out, c.wr2);  byteio::putU8(out, c.wr3);
        byteio::putU8(out, c.wr4);  byteio::putU8(out, c.wr5);
        byteio::putU8(out, c.wr10); byteio::putU8(out, c.wr11);
        byteio::putU8(out, c.wr12); byteio::putU8(out, c.wr13);
        byteio::putU8(out, c.wr14); byteio::putU8(out, c.wr15);

        for (uint8_t v : c.rxData)  byteio::putU8(out, v);
        for (uint8_t v : c.rxError) byteio::putU8(out, v);
        for (bool e : c.rxEof)      byteio::putU8(out, e ? 1 : 0);
        byteio::putU8(out, static_cast<uint8_t>(c.rxFifoRp));
        byteio::putU8(out, static_cast<uint8_t>(c.rxFifoWp));
        byteio::putU8(out, c.txData[0]);
        byteio::putU8(out, static_cast<uint8_t>(c.txFifoRp));
        byteio::putU8(out, static_cast<uint8_t>(c.txFifoWp));

        byteio::putU8(out, static_cast<uint8_t>(c.rxFirst));
        byteio::putU8(out, static_cast<uint8_t>(c.txIntDisarm));
        byteio::putU8(out, c.extIntLatch);
        byteio::putU8(out, c.extIntStates);
        byteio::putU8(out, static_cast<uint8_t>(c.dtr));
        byteio::putU8(out, static_cast<uint8_t>(c.rts));
        byteio::putU16(out, c.syncPattern);

        byteio::putU32(out, c.brgRate);
        byteio::putU32(out, c.delayedTxBrgChange);
        byteio::putU32(out, c.rcvRate);
        byteio::putU32(out, c.traRate);
        byteio::putU8 (out, c.txShiftData);
        byteio::putU16(out, static_cast<uint16_t>(c.txHalfBits));
        byteio::putU64(out, c.txAcc);
        byteio::putU32(out, c.brgTimerRate);
        byteio::putU64(out, c.brgAcc);
        byteio::putU16(out, static_cast<uint16_t>(c.txFrame.size()));
        for (uint8_t v : c.txFrame) byteio::putU8(out, v);
    }
}

bool Scc8530Device::restoreSnapshot(const uint8_t* data, std::size_t len)
{
    if (!data) return false;
    byteio::Reader r(data, len);
    if (!r.has(5)) return false;
    if (r.u32() != kSnapMagic) return false;
    if (r.u8()  != kSnapVersion) return false;

    // 3 device bytes + 6 int states + 6 int sources, then per channel:
    // 17 registers + 6 rx bytes + 2 rx pointers + 3 tx + 6 flags + 2 sync
    // + 16 rate/accumulator bytes + 1 + 2 + 8 + 4 + 8.
    // The per-channel fixed part; each channel is then followed by a
    // 16-bit SDLC frame length and that many bytes, checked as they come.
    constexpr std::size_t kPerChannel =
        17 + 6 + 3 + 2 + 3 + 6 + 2 + 16 + 1 + 2 + 8 + 4 + 8 + 2;
    if (!r.has(3 + 6 + 6 + 2 * kPerChannel)) return false;

    wr9_        = r.u8();
    wr0PtrBits_ = r.u8();
    const bool intState = r.u8() != 0;
    for (uint8_t& v : intState_)  v = r.u8();
    for (int& v : intSource_)     v = r.u8();

    for (Channel& c : ch_) {
        c.rr0 = r.u8(); c.rr1 = r.u8(); c.rr2 = r.u8(); c.rr3 = r.u8();
        c.rr10 = r.u8();
        c.wr0 = r.u8(); c.wr1 = r.u8(); c.wr2 = r.u8(); c.wr3 = r.u8();
        c.wr4 = r.u8(); c.wr5 = r.u8(); c.wr10 = r.u8(); c.wr11 = r.u8();
        c.wr12 = r.u8(); c.wr13 = r.u8(); c.wr14 = r.u8(); c.wr15 = r.u8();

        for (uint8_t& v : c.rxData)  v = r.u8();
        for (uint8_t& v : c.rxError) v = r.u8();
        for (bool& e : c.rxEof)      e = r.u8() != 0;
        c.rxFifoRp = r.u8() % Channel::kRxFifoSz;
        c.rxFifoWp = r.u8() % Channel::kRxFifoSz;
        c.txData[0] = r.u8();
        c.txFifoRp = r.u8() % Channel::kTxFifoSz;
        c.txFifoWp = r.u8() % Channel::kTxFifoSz;

        c.rxFirst      = r.u8();
        c.txIntDisarm  = r.u8();
        c.extIntLatch  = r.u8();
        c.extIntStates = r.u8();
        c.dtr = r.u8();
        c.rts = r.u8();
        c.syncPattern = r.u16();

        c.brgRate            = r.u32();
        c.delayedTxBrgChange = r.u32();
        c.rcvRate            = r.u32();
        c.traRate            = r.u32();
        c.txShiftData        = r.u8();
        c.txHalfBits         = r.u16();
        c.txAcc              = r.u64();
        c.brgTimerRate       = r.u32();
        c.brgAcc             = r.u64();
        const std::size_t frameLen = r.u16();
        if (!r.has(frameLen)) return false;
        // A snapshot is a file: a hand-edited length must not restore a frame
        // past the ceiling the transmit path enforces (kMaxTxFrameBytes).
        c.txFrame.assign(data + r.pos,
                         data + r.pos +
                             (frameLen < kMaxTxFrameBytes ? frameLen
                                                          : kMaxTxFrameBytes));
        r.pos += frameLen;
    }

    // Republish the interrupt line: the owner's /IRQ has to agree with the
    // restored state, and it only ever hears about edges.
    outIntState_ = intState;
    if (intCb_) intCb_(outIntState_);
    return true;
}

} // namespace pom2
