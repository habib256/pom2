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

// Via6522 — minimal 65C22 VIA model, used by every POM2 card that fronts
// AY-3-8910/8913 PSGs (Mockingboard, Phasor, Echo+). Originally lived as
// a private nested struct inside MockingboardCard; extracted 2026-05-27
// so PhasorCard can share the same VIA model verbatim without
// duplicating the timer / IFR / Port-A/B logic.
//
// Scope: T1 (both modes, INCLUDING the ACR.7 PB7 square-wave output),
// T2 (one-shot phase-2), IFR/IER, Port A/B output latches + DDR, port-A
// input latching (ACR.0), CA1 input edge (setCa1NegativeEdge — Sound II
// SSI263 A/!R wiring) and the MAME `CLR_PA_INT()` rule (any reg-1/ORA
// access clears IFR.CA1 + IFR.CA2-unless-independent; reg $F/ORANH is
// side-effect-free).
//
// NOT modelled (matching the original MB scope) — the complete list:
//
//   * SR shift register (reg $A is a plain byte store).
//   * CA2 / CB1 / CB2: no input pins, no output/handshake/pulse modes,
//     no IFR.CB1 (0x10) / IFR.CB2 (0x08) / IFR.SR (0x04) sources. Only
//     CA1 exists, and only as an edge injector.
//   * **Port-B input latching (ACR.1)**, because it latches on a CB1
//     active edge and this model has no CB1. Port-A latching (ACR.0) IS
//     modelled — CA1 exists. MAME `6522via.cpp:645-651` / `:1181-1184`.
//   * PB6 pulse counting for T2 (ACR.5 is honoured as "stop counting
//     phase-2", but no POM2 card wires PB6, so it never ticks).
//   * Port-B external inputs: `readPortB()` pulls DDR=0 pins high. No
//     POM2 card drives port B from outside (the AY read strobe is on
//     port A, which does have `setPortAInput`).
//
// Header-only: every method is `inline`, no `Via6522.cpp` to link.
// Cards include this header, instantiate via `std::make_unique<Via6522>`,
// and reach the public methods directly.
//
// Verbatim port of MAME `machine/6522via.cpp` for the subset above. The
// detailed per-method MAME line references live next to each switch
// case below.

#ifndef POM2_VIA6522_H
#define POM2_VIA6522_H

#include "ByteIO.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pom2 {

struct Via6522
{
    // ─── Register layout (WDC W65C22 datasheet) ──────────────────────────
    enum : uint8_t {
        VIA_ORB    = 0x0,  // Output Reg B / Input Reg B
        VIA_ORA    = 0x1,  // Output Reg A (with handshake)
        VIA_DDRB   = 0x2,
        VIA_DDRA   = 0x3,
        VIA_T1CL   = 0x4,  // Timer 1 counter low
        VIA_T1CH   = 0x5,  // Timer 1 counter high
        VIA_T1LL   = 0x6,  // Timer 1 latch low
        VIA_T1LH   = 0x7,  // Timer 1 latch high
        VIA_T2CL   = 0x8,
        VIA_T2CH   = 0x9,
        VIA_SR     = 0xA,
        VIA_ACR    = 0xB,
        VIA_PCR    = 0xC,
        VIA_IFR    = 0xD,
        VIA_IER    = 0xE,
        VIA_ORANH  = 0xF,  // Output Reg A no handshake
    };

    // IFR / IER bit positions.
    static constexpr uint8_t IFR_CA2 = 0x01;
    static constexpr uint8_t IFR_CA1 = 0x02;
    static constexpr uint8_t IFR_T2  = 0x20;
    static constexpr uint8_t IFR_T1  = 0x40;
    static constexpr uint8_t IFR_ANY = 0x80;   // computed on read

    // ─── State ───────────────────────────────────────────────────────────
    uint8_t portAOut = 0x00;
    /// External level driven onto port A's pins. Real 6522 port-A reads
    /// return `(out & ddr) | (pin & ~ddr)`; POM2 had no pin model, so an
    /// AY READ strobe had nowhere to land (see Ay3_8910::busOut).
    uint8_t portAIn  = 0xFF;
    uint8_t portBOut = 0x00;
    uint8_t ddrA = 0x00;
    uint8_t ddrB = 0x00;
    uint8_t acr  = 0x00;
    uint8_t pcr  = 0x00;
    uint8_t sr   = 0x00;
    // Timer 1 latches and counter. `t1Counter` is signed-extended to 32
    // bits so we can detect the underflow transition (16-bit counter goes
    // 0 → -1, i.e. the pulse fires when we count *through* zero).
    uint16_t t1Latch = 0xFFFF;
    int32_t  t1Counter = 0xFFFF;
    bool     t1FireArmed = false;  // post-reset T1 doesn't fire until SW loads T1CH
    // Timer 2. One-shot on phase-2 (ACR.bit5 PB6 pulse-counting NOT
    // modelled — no POM2 card drives PB6 externally). Unlocks Ultima IV
    // Echo+ speech driver, FrenchTouch sample demos.
    uint8_t  t2ll       = 0xFF;
    uint16_t t2Latch    = 0xFFFF;  // {t2lh:t2ll}, set by T2CH write
    int32_t  t2Counter  = 0xFFFF;
    bool     t2Active   = false;   // armed → fires one IRQ on underflow
    // IFR / IER store only per-source bits (0..6); bit 7 computed on read.
    uint8_t ifr = 0x00;
    uint8_t ier = 0x00;
    /// T1 square-wave state on PB7 (ACR bit 7). MAME `m_t1_pb7`
    /// (`6522via.cpp:279` device_start = 0, `:347` device_reset = 1).
    /// Toggled by every continuous-mode underflow, forced to 1 by a
    /// one-shot underflow, cleared by a T1CH write, and OR'd into every
    /// port-B read IGNORING DDRB (`T1_SET_PB7(c) = (c & 0x80)`,
    /// `6522via.cpp:85` + `:605-630`).
    bool    t1Pb7 = true;
    /// Port-A input latch (ACR bit 0). Loaded from the composed port-A
    /// value on an active CA1 edge and returned by ORA/ORANH while
    /// IFR.CA1 is still set — MAME `6522via.cpp:662-700` + `:1107-1110`.
    uint8_t latchA = 0xFF;

    inline void reset()
    {
        portAOut = portBOut = 0;
        portAIn  = 0xFF;
        latchA   = 0xFF;
        t1Pb7    = true;     // MAME device_reset (`6522via.cpp:347`)
        ddrA = ddrB = 0;
        acr  = pcr = sr = 0;
        t1Latch = 0xFFFF;
        t1Counter = 0xFFFF;
        t1FireArmed = false;
        t2ll       = 0xFF;
        t2Latch    = 0xFFFF;
        t2Counter  = 0xFFFF;
        t2Active   = false;
        ifr = 0;
        ier = 0;
    }

    // ─── Snapshot (rewind) ───────────────────────────────────────────────
    // Fixed 24-byte layout of the full register/timer state. Lazily-synced
    // counters are captured as-is (the next syncToCpuCycle re-advances them,
    // exactly as it would have on the live machine).
    // NOT carried: `t1Pb7` and `latchA`. Both are re-derived within one T1
    // period / one CA1 edge of a restore, and no POM2 card wires PB7 or a
    // latched port A to anything, so widening the blob (and every card's
    // versioned reader) would buy nothing. `loadSnapshot` leaves whatever
    // the live object held; `reset()` puts them back to 1 / $FF.
    /// v1 layout (pre-2026-07-30): everything except `portAIn`.
    static constexpr std::size_t kSnapshotBytesV1 = 24;
    /// Current layout — v1 plus the port-A input pin latch.
    static constexpr std::size_t kSnapshotBytes = 25;
    inline void appendSnapshot(std::vector<uint8_t>& o) const
    {
        byteio::putU8(o, portAOut); byteio::putU8(o, portBOut);
        byteio::putU8(o, ddrA);     byteio::putU8(o, ddrB);
        byteio::putU8(o, acr);      byteio::putU8(o, pcr); byteio::putU8(o, sr);
        byteio::putU16(o, t1Latch); byteio::putU32(o, static_cast<uint32_t>(t1Counter));
        byteio::putU8(o, t1FireArmed ? 1 : 0);
        byteio::putU8(o, t2ll);     byteio::putU16(o, t2Latch);
        byteio::putU32(o, static_cast<uint32_t>(t2Counter));
        byteio::putU8(o, t2Active ? 1 : 0);
        byteio::putU8(o, ifr);      byteio::putU8(o, ier);
        byteio::putU8(o, portAIn);
    }
    /// `size` selects the layout: kSnapshotBytesV1 for a pre-2026-07-30
    /// blob (portAIn absent → left at its reset value), kSnapshotBytes for
    /// the current one. Caller ensures at least `size` bytes are readable.
    inline void loadSnapshot(const uint8_t* d,
                             std::size_t size = kSnapshotBytes)
    {
        byteio::Reader r(d, size);
        portAOut = r.u8(); portBOut = r.u8(); ddrA = r.u8(); ddrB = r.u8();
        acr = r.u8(); pcr = r.u8(); sr = r.u8();
        t1Latch = r.u16(); t1Counter = static_cast<int32_t>(r.u32());
        t1FireArmed = r.u8() != 0;
        t2ll = r.u8(); t2Latch = r.u16(); t2Counter = static_cast<int32_t>(r.u32());
        t2Active = r.u8() != 0;
        ifr = r.u8(); ier = r.u8();
        if (size >= kSnapshotBytes) portAIn = r.u8();
    }

    // Composed Port reads: input pins (DDR=0) pulled high (Mockingboard /
    // Phasor have no inputs wired), output pins reflect the latch.
    inline uint8_t readPortB() const
    {
        const uint8_t input = 0xFF;
        uint8_t pb = static_cast<uint8_t>((portBOut & ddrB) | (input & ~ddrB));
        // ACR bit 7 (T1_SET_PB7): PB7 stops being a port bit and becomes
        // the T1 output. MAME overwrites it in `input_pb`, `output_pb` AND
        // `read_pb` (`6522via.cpp:605-630`) without consulting DDRB.
        if (acr & 0x80) pb = static_cast<uint8_t>((pb & 0x7F) | (t1Pb7 ? 0x80 : 0x00));
        return pb;
    }
    inline uint8_t readPortA() const
    {
        // Input pins come from `portAIn` (default 0xFF = pulled high, the
        // old hard-coded behaviour). Cards that drive port A externally —
        // the Mockingboard/Phasor AY READ strobe — latch the chip's bus
        // value there.
        return (portAOut & ddrA) | (portAIn & ~ddrA);
    }
    /// Latch an external level onto port A's input pins.
    inline void setPortAInput(uint8_t v) { portAIn = v; }

    /// What a guest read of a counter byte would return, with no side
    /// effect — `written + 1 - elapsed`, the line `read()` implements and
    /// `via_t1_rearm_chain` pins. The diagnostic panels re-implemented this
    /// as the RAW counter and were one too high on all four bytes, which is
    /// the one number somebody debugging a raster problem would trust.
    inline uint8_t counterReadback(bool t2, bool high) const
    {
        const int32_t rb = (t2 ? t2Counter : t1Counter) - 1;
        return static_cast<uint8_t>((high ? (rb >> 8) : rb) & 0xFF);
    }

    /// What a bus read of ORA/ORANH sees. With ACR bit 0 set the port is
    /// LATCHED: the value frozen by the last active CA1 edge stands until
    /// IFR.CA1 is acknowledged. MAME `6522via.cpp:662-671` (ORA) and
    /// `:690-700` (ORANH), both `(PA_LATCH_ENABLE(m_acr) != 0) && ((m_ifr
    /// & INT_CA1) != 0) ? m_latch_a : input_pa()`.
    inline uint8_t latchedPortA() const
    {
        if ((acr & 0x01) && (ifr & IFR_CA1)) return latchA;
        return readPortA();
    }

    inline bool irqOut() const { return (ifr & ier & 0x7F) != 0; }

    /// MAME 6522via.cpp:99 — `CLR_PA_INT()` = `clear_int(INT_CA1 |
    /// ((!CA2_IND_IRQ(m_pcr)) ? INT_CA2 : 0))`, executed on EVERY read or
    /// write of register 1 (ORA, the handshake port). `CA2_IND_IRQ(c)` =
    /// `((c & 0x0a) == 0x02)` (6522via.cpp:51) — when CA2 is configured as
    /// an "independent interrupt" input, an ORA access leaves IFR.CA2 set.
    /// IFR.CA1 is always cleared. Register $F (ORANH, "no handshake") has
    /// no such side effect (6522via.cpp:690-700 read, :888-896 write).
    /// Needed since setCa1NegativeEdge() can latch IFR.CA1 (Sound II
    /// SSI263 wiring) — a driver acking via the standard ORA access would
    /// otherwise see a stuck IRQ.
    inline void clearPaInt()
    {
        uint8_t mask = IFR_CA1;
        if ((pcr & 0x0A) != 0x02) mask |= IFR_CA2;   // CA2 not independent
        ifr &= ~mask;
    }

    inline uint8_t computedIfr() const
    {
        return static_cast<uint8_t>(
            (ifr & 0x7F) | (irqOut() ? IFR_ANY : 0));
    }

    // T1 mode bits live in ACR bits 7..6.
    inline bool t1Continuous() const { return (acr & 0x40) != 0; }

    // 6522 read — `reg` in 0..15. Some reads have side effects
    // (clearing IFR.T1 on T1CL/T1CH read).
    inline uint8_t read(uint8_t reg)
    {
        switch (reg & 0x0F) {
        case VIA_ORB:    return readPortB();
        case VIA_ORA: {
            // Reading ORA clears IFR.CA1 (+ CA2 unless independent) —
            // MAME 6522via.cpp:662-688 (CLR_PA_INT() at :676). CA2
            // pulse/handshake *output* modes stay unmodelled.
            const uint8_t v = latchedPortA();
            clearPaInt();
            return v;
        }
        case VIA_DDRB:   return ddrB;
        case VIA_DDRA:   return ddrA;
        // Counter read-back: while a timer is ARMED the live counter
        // carries the +2 IRQ pre-bias (see T1CH/T2CH write cases), which
        // must NOT be visible to the guest — MAME's get_counter1_value()
        // returns `remaining - IFR_DELAY` while active (6522via.cpp:
        // ~518-530), i.e. our `counter - 2`. After underflow (one-shot
        // disarmed) MAME switches to the free-running 0xFFFF-elapsed
        // epoch, which the raw wrapped counter already equals (fire at
        // counter == -1 → 0xFFFF). A bare read of the biased counter
        // returned N+2 right after a T1CH write — absolute-value
        // detectors (write $12xx, read back expecting $12) saw $13.
        // T1 counter read-back bias is -1, NOT MAME's -IFR_DELAY (-2 on
        // our +2-biased counter): the visible line is `written + 1 -
        // elapsed` — the 6522 loads the counter on the phi2 AFTER the
        // T1C-H write, so a read e cycles later shows N-(e-1). Measured,
        // not assumed: French Touch's TRIBU (DIX anthology, GPLv3
        // sources in disks_5.4/demo/unreeeal_superhero_3_tribute/)
        // phase-locks five chained T1 IRQs per PAL frame by reading the
        // free-running counter mid-handler (`ADC $C404 … STA $C405`,
        // 21 cycles read→write) and re-arming with `TIMERn + readback`,
        // every TIMERn carrying "DELAY = 24 ; value FOR APPLEWIN / REAL
        // APPLE II". Per-link period = TIMERn + 21 + 1 + bias-line: the
        // -1 bias holds the chain at exactly 20280 cycles/frame (drift
        // 0, as released on hardware); with MAME's -2 every link lost
        // one cycle — POM2 measured -5 cycles/frame on the real disk,
        // every raster edge crawling left one character cell every ~3
        // frames. The continuous auto-reload period stays latch+2
        // (`via_t1_continuous_period`, MAD EFFECT); the same -1 applies
        // disarmed (one-shot fired, counter free-running) so the line
        // is continuous through the underflow. Pinned by
        // `via_t1_rearm_chain`.
        case VIA_T1CL: {
            ifr &= ~IFR_T1;
            const int32_t rb = t1Counter - 1;
            return static_cast<uint8_t>(rb & 0xFF);
        }
        case VIA_T1CH: {
            const int32_t rb = t1Counter - 1;
            return static_cast<uint8_t>((rb >> 8) & 0xFF);
        }
        case VIA_T1LL:   return static_cast<uint8_t>(t1Latch & 0xFF);
        case VIA_T1LH:   return static_cast<uint8_t>((t1Latch >> 8) & 0xFF);
        case VIA_T2CL: {
            // T2CL read clears IFR.T2 (MAME `6522via.cpp:590-594`).
            //
            // Read-back bias -1, NOT MAME's -2: the same hardware rule the
            // T1 cases above carry (commit c58528a) — the visible counter
            // line is `written + 1 - elapsed`, because the counter loads on
            // the phi2 AFTER the CxH write. T2 is the identical load/
            // decrement silicon (W65C22 datasheet), so MAME's extra cycle is
            // the same one-low divergence that made DIX rasters crawl on
            // T1; a beam-racer re-arming from a T2 read-back would drift
            // the same way. Unconditional, like T1, so the line stays
            // continuous through the underflow (armed counter carries the
            // +2 pre-bias; after the one-shot fires, the +0x10000 free-run
            // continues the same line). Pinned by `via_t2_timing`.
            ifr &= ~IFR_T2;
            const int32_t rb = t2Counter - 1;
            return static_cast<uint8_t>(rb & 0xFF);
        }
        case VIA_T2CH: {
            const int32_t rb = t2Counter - 1;
            return static_cast<uint8_t>((rb >> 8) & 0xFF);
        }
        case VIA_SR:     return sr;
        case VIA_ACR:    return acr;
        case VIA_PCR:    return pcr;
        case VIA_IFR:    return computedIfr();
        case VIA_IER:    return static_cast<uint8_t>(ier | 0x80);
        case VIA_ORANH:  return latchedPortA();
        default:         return 0xFF;
        }
    }

    // 6522 write. Returns a bit-pattern of which "events" happened so
    // the caller can react: bit 0 = Port B output changed, bit 1 = Port
    // A output changed.
    inline uint8_t write(uint8_t reg, uint8_t v)
    {
        uint8_t events = 0;
        switch (reg & 0x0F) {
        case VIA_ORB: {
            const uint8_t prev = portBOut;
            portBOut = v;
            if ((prev & ddrB) != (v & ddrB)) events |= 0x01;
            break;
        }
        case VIA_ORA: {
            const uint8_t prev = portAOut;
            portAOut = v;
            // Writing ORA clears IFR.CA1 (+ CA2 unless independent) —
            // MAME 6522via.cpp:866-886 (CLR_PA_INT() at :875).
            clearPaInt();
            if ((prev & ddrA) != (v & ddrA)) events |= 0x02;
            break;
        }
        // A DDR write moves PINS, not just the latch — the event has to be
        // reported on the composed pin value (`readPortB`/`readPortA`), which
        // is also what the consumer is handed. Comparing `latch & ddr` missed
        // every bit whose latch is 0: the pin goes from pulled-up 1 to driven
        // 0, a real edge, with both sides of the comparison reading 0. On the
        // Mockingboard and the Phasor that pin can be PB2, the AY's /RESET —
        // so a driver that drives only BC1+BDIR (DDRB = $03, /RESET left to
        // the board's pull-up) and later takes PB2 over as an output asserted
        // a reset the AY never saw, and kept the previous program's registers.
        case VIA_DDRB: {
            const uint8_t prev = readPortB();
            ddrB = v;
            if (readPortB() != prev) events |= 0x01;
            break;
        }
        case VIA_DDRA: {
            const uint8_t prev = readPortA();
            ddrA = v;
            if (readPortA() != prev) events |= 0x02;
            break;
        }
        case VIA_T1CL:
        case VIA_T1LL:
            t1Latch = (t1Latch & 0xFF00) | v;
            break;
        case VIA_T1CH:
            // Latch high, transfer latches into counter, start timer,
            // clear IFR.T1.
            t1Latch = (t1Latch & 0x00FF) | (static_cast<uint16_t>(v) << 8);
            // MAME schedules the FIRST T1 shot at `TIMER1_VALUE + IFR_DELAY`
            // (`6522via.cpp:927-943`, adjust at :941; IFR_DELAY = 3 at :102)
            // — same constant the continuous reload uses (t1_tick,
            // `6522via.cpp:536-543`). POM2's `advance()` fires when the
            // counter crosses < 0, i.e. one tick AFTER it reaches 0, so a
            // raw `t1Counter = N` would fire at N+1 — two cycles EARLY.
            // Pre-bias by (IFR_DELAY - 1) = 2 so the first shot lands at
            // exactly N+3, matching hardware. The RECURRING period below is
            // a different number — latch+2, not latch+3 — because IFR_DELAY
            // is a one-off underflow→IFR latency and does not repeat; see
            // the free-run comment in `advance()`. Same pre-bias protocol as
            // the T2CH case — see the DIX beam-racing rationale there.
            // Counter readback DELTAS are unaffected (pinned by
            // mockingboard_4am_detect).
            t1Counter = static_cast<int32_t>(t1Latch) + 2;
            t1FireArmed = true;
            ifr &= ~IFR_T1;
            // MAME `6522via.cpp:934` — a T1CH write drives PB7 low, which
            // is what makes the ACR.7 square wave start from a known phase.
            t1Pb7 = false;
            break;
        case VIA_T1LH:
            // Latch high only: NO counter transfer, NO restart — but the
            // T1 interrupt flag IS cleared. MAME `6522via.cpp` VIA_T1LH
            // (~:920-924): `m_t1lh = data; clear_int(INT_T1);` — the old
            // comment here claimed the opposite while citing the same
            // case. AppleWin's SY6522 clears it too.
            t1Latch = (t1Latch & 0x00FF) | (static_cast<uint16_t>(v) << 8);
            ifr &= ~IFR_T1;
            break;
        case VIA_T2CL:
            // Store the low latch only — no effect on the running counter
            // (MAME `6522via.cpp:764-766`).
            t2ll    = v;
            t2Latch = static_cast<uint16_t>((t2Latch & 0xFF00) | v);
            break;
        case VIA_T2CH:
            // Latch high, transfer {t2lh:t2ll} into counter, clear IFR.T2,
            // arm T2 (MAME `6522via.cpp:946-960`).
            t2Latch    = static_cast<uint16_t>((static_cast<uint16_t>(v) << 8) | t2ll);
            // MAME schedules the underflow IRQ at `TIMER2_VALUE + IFR_DELAY`
            // (`:959`), with IFR_DELAY = 3 (latch-copy + IRQ-latch settle —
            // same constant T1 uses above). POM2's `advance()` fires when the
            // counter crosses < 0, i.e. one tick AFTER it reaches 0, so a raw
            // `t2Counter = N` would fire at N+1 — two cycles EARLY. Pre-bias by
            // (IFR_DELAY - 1) = 2 so the one-shot fires at exactly N+3, matching
            // real hardware. This is what lets a Timer-2-synced beam-racer
            // (French Touch DIX: `T2 = 7512 - latency`) land its effect on the
            // intended scanline instead of two bytes early.
            t2Counter  = static_cast<int32_t>(t2Latch) + 2;
            ifr       &= ~IFR_T2;
            t2Active   = true;
            break;
        case VIA_SR:    sr  = v; break;
        case VIA_ACR: {
            // MAME `6522via.cpp` VIA_ACR write: switching T1 into
            // continuous mode RE-ARMS the timer from the current counter
            // (`m_t1_active = 1; m_t1->adjust(counter1 + IFR_DELAY)`).
            // POM2 only stored the byte, so a T1 that had fired one-shot
            // and disarmed stayed dead after the guest flipped to
            // continuous — no more IRQs until the next T1CH write. Same
            // +2 pre-bias protocol as the T1CH/T2CH cases: the fire then
            // lands at counter1 + IFR_DELAY(3).
            //
            // Only a STOPPED T1 is re-armed. For a RUNNING one the round trip
            // through MAME's uint16 `get_counter1_value()` (`counter -
            // IFR_DELAY`, then `+ IFR_DELAY` again) is the identity everywhere
            // EXCEPT the two cycles before an underflow, where `0`/`1` wrap to
            // `0xFFFE`/`0xFFFF` and shove the pending interrupt 65536 cycles
            // (64 ms) into the future — a dropped music tick or a lost raster
            // frame, once per ~period/2 ACR writes (bug hunt #8). Leaving a
            // live counter alone is also what the silicon does: an ACR write
            // does not reload T1 (W65C22 datasheet), and AppleWin's SY6522
            // stores the byte and nothing else. MAME shares the wrap; hardware
            // does not.
            acr = v;
            if (t1Continuous() && !t1FireArmed) {
                t1Counter   = (t1Counter & 0xFFFF) + 2;
                t1FireArmed = true;
            }
            break;
        }
        case VIA_PCR:   pcr = v; break;
        case VIA_IFR:
            // Writing 1s to IFR clears those bits (only 0..6 user-clearable).
            ifr &= ~(v & 0x7F);
            break;
        case VIA_IER:
            // Bit 7 selects set vs clear; bits 0..6 are the mask.
            if (v & 0x80) ier |= (v & 0x7F);
            else          ier &= ~(v & 0x7F);
            break;
        case VIA_ORANH: {
            const uint8_t prev = portAOut;
            portAOut = v;
            if ((prev & ddrA) != (v & ddrA)) events |= 0x02;
            break;
        }
        default: break;
        }
        return events;
    }

    /// Strobe an external CA1 input edge — used by Mockingboard "Sound II"
    /// when the on-board SSI263's A/!R signal toggles. PCR bit 0 selects
    /// the active edge: 0 = negative-going, 1 = positive-going. We model
    /// the canonical Mockingboard wiring (SSI263 A/!R inverted into CA1
    /// → 0→1 of A/!R = negative edge on CA1), so `setCa1NegativeEdge()`
    /// sets IFR.CA1 only when PCR.0 == 0. If the host CPU has IER.CA1
    /// (= IFR_CA1 bit) enabled, `irqOut()` will then go high.
    /// AppleWin parity: `if ((GetPCR(m_device) & 1) == 0) UpdateIFR(0, IxR_SSI263)`.
    inline void setCa1NegativeEdge()
    {
        if ((pcr & 0x01) == 0) { latchPortAOnCa1(); ifr |= IFR_CA1; }
    }

    /// Symmetric helper for cards that need the opposite polarity.
    inline void setCa1PositiveEdge()
    {
        if ((pcr & 0x01) != 0) { latchPortAOnCa1(); ifr |= IFR_CA1; }
    }

    /// MAME `6522via.cpp:1107-1110` (`ca1_w`): an active CA1 transition
    /// freezes port A into the input latch when ACR bit 0 is set. Done
    /// BEFORE IFR.CA1 is raised so `latchedPortA()` cannot return a
    /// stale latch on the very same edge.
    inline void latchPortAOnCa1()
    {
        if (acr & 0x01) latchA = readPortA();
    }

    // Advance T1 (and T2 in one-shot phase-2 mode) by `cycles` 1.0227 MHz
    // ticks. Sets IFR.T1/T2 on underflow and (T1 continuous mode) reloads
    // from the latch automatically.
    inline bool advance(int cycles)
    {
        if (cycles <= 0) return false;
        bool fired = false;
        t1Counter -= cycles;
        while (t1Counter < 0) {
            if (t1FireArmed) {
                ifr |= IFR_T1;
                fired = true;
                if (!t1Continuous()) {
                    t1FireArmed = false;
                }
            }
            if (!t1Continuous()) {
                // MAME `t1_tick` one-shot branch (`6522via.cpp:544-549`):
                // PB7 goes high and the timer stops.
                t1Pb7 = true;
            }
            if (t1Continuous()) {
                // Period is latch + 2 — the 6522 free-run contract. Was +3,
                // which is MAME's `TIMER1_VALUE + IFR_DELAY`; but IFR_DELAY
                // models the one-off underflow→IFR latency, NOT part of the
                // recurring period. Folding it into the period stretched
                // every frame by one cycle, so anything using T1 continuous
                // as a frame clock DRIFTED one cycle per frame — invisible
                // to a music tick, fatal to a beam-raced effect.
                //
                // French Touch "MAD EFFECT" states the contract while
                // computing its own latch (`Sources/main.a`, shipped in
                // disks_5.4/demo/madef/):
                //   ; PAL delay = 65*(192+70+50) = 20280
                //   ; -2 (6522 takes 2 cycles to generate INT)
                //   ; = 20278 = $4F36
                // i.e. period == latch + 2. With +3 its 192-line drawing
                // loop slid a cycle per frame until whole scanlines of the
                // picture fell into VBL and were dropped. Pinned by
                // `via_t1_continuous_period`.
                //
                // Collapse the reload
                // arithmetically: a degenerate tiny latch (e.g. 0 → period 2)
                // under a large `cycles` (clamped to ~2.1e9 on a sync desync)
                // would otherwise spin this loop hundreds of millions of
                // times. IFR_T1 has already latched (idempotent above), so
                // jump t1Counter forward in one int64 step to ≥ 0. MAME
                // instead guards its reschedule with `if (TIMER1_VALUE > 0)`
                // (`6522via.cpp` t1_tick); no equivalent guard is needed
                // here because `period` is never 0 and the int64 step below
                // terminates for any latch value.
                const int64_t period  = static_cast<int64_t>(t1Latch) + 2;
                const int64_t deficit = -static_cast<int64_t>(t1Counter);   // > 0
                // CEILING, not floor+1. When the slice ends exactly ON an
                // underflow (`deficit % period == 0`) floor+1 counted one
                // period too many: it swallowed a whole period (the next T1
                // interrupt landed one period late) and flipped the PB7
                // square wave's phase, so `advance(n)` stopped agreeing with
                // n x `advance(1)` — the invariant the lazy sync and the
                // batched disk-turbo path both depend on (bug hunt #8).
                const int64_t periods = (deficit + period - 1) / period;
                t1Counter = static_cast<int32_t>(
                    static_cast<int64_t>(t1Counter) + periods * period);
                // MAME toggles PB7 once per underflow, but only
                // `if (TIMER1_VALUE > 0)` (`6522via.cpp:539-541`) — a zero
                // latch leaves the wave frozen. The collapse above swallows
                // `periods` underflows in one step, so apply their parity
                // rather than a single flip.
                if (t1Latch != 0 && (periods & 1)) t1Pb7 = !t1Pb7;
            } else {
                t1Counter += 0x10000;
            }
        }
        // T2 advance — phase-2 mode only.
        if ((acr & 0x20) == 0) {
            t2Counter -= cycles;
            while (t2Counter < 0) {
                if (t2Active) {
                    ifr |= IFR_T2;
                    fired = true;
                    t2Active = false;
                }
                t2Counter += 0x10000;
            }
        }
        return fired;
    }
};

} // namespace pom2

#endif // POM2_VIA6522_H
