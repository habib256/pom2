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

// MC6821 — see header. Verbatim port of MAME's `6821pia.cpp`. Read
// inline against the MAME source line-by-line: the C++ structure is
// identical (constructor → reset → register read/write → CA/CB line
// inputs), only the device_t / devcb plumbing has been replaced with
// std::function callbacks.

#include "MC6821.h"

namespace {
constexpr uint8_t PIA_IRQ1 = 0x80;
constexpr uint8_t PIA_IRQ2 = 0x40;
}  // namespace

MC6821::MC6821()
{
    reset();
    in_a = 0xFF;
    in_b = 0;
}

void MC6821::reset()
{
    // MAME device_reset(): port A defaults to internal pull-ups (out_a
    // pushed as 0xFF below), port B starts three-state (we just leave
    // it 0 — Mouse Card firmware always programs DDR before reading).
    out_a = 0;
    out_ca2 = false;
    ddr_a = 0;
    ctl_a = 0;
    irq_a1 = false;
    irq_a2 = false;
    irq_a_state = false;
    out_b = 0;
    out_cb2 = false;
    ddr_b = 0;
    ctl_b = 0;
    irq_b1 = false;
    irq_b2 = false;
    irq_b_state = false;

    if (irq_a_cb)   irq_a_cb(false);
    if (irq_b_cb)   irq_b_cb(false);
    if (out_a_cb)   out_a_cb(0xFF);     // internal pull-ups
    if (out_ca2_cb) out_ca2_cb(true);
}

// ─── Interrupt aggregation (MAME update_interrupts) ─────────────────────

void MC6821::updateInterrupts()
{
    bool new_a = (irq_a1 && irq1_enabled(ctl_a)) || (irq_a2 && irq2_enabled(ctl_a));
    if (new_a != irq_a_state) {
        irq_a_state = new_a;
        if (irq_a_cb) irq_a_cb(new_a);
    }
    bool new_b = (irq_b1 && irq1_enabled(ctl_b)) || (irq_b2 && irq2_enabled(ctl_b));
    if (new_b != irq_b_state) {
        irq_b_state = new_b;
        if (irq_b_cb) irq_b_cb(new_b);
    }
}

// ─── Port read helpers (MAME get_in_a_value / get_in_b_value) ───────────

uint8_t MC6821::getInAValue() const
{
    // For Port A, when in output mode, external devices can drive pins
    // too. We omit MAME's `m_a_input_overrides_output_mask` because the
    // mouse card doesn't use it.
    return static_cast<uint8_t>((~ddr_a & in_a) | (ddr_a & out_a));
}

uint8_t MC6821::getInBValue() const
{
    if (ddr_b == 0xFF) return out_b;     // all output
    return static_cast<uint8_t>((out_b & ddr_b) | (in_b & ~ddr_b));
}

uint8_t MC6821::getOutAValue() const
{
    if (ddr_a == 0xFF) return out_a;
    return static_cast<uint8_t>((out_a & ddr_a) | (getInAValue() & ~ddr_a));
}

uint8_t MC6821::getOutBValue() const
{
    return static_cast<uint8_t>(out_b & ddr_b);
}

// ─── Bus reads (MAME read / port_X_r / control_X_r) ─────────────────────

uint8_t MC6821::read(uint8_t offset)
{
    switch (offset & 0x03) {
    case 0x00: {
        if (output_selected(ctl_a)) {
            uint8_t ret = getInAValue();
            // IRQ flags implicitly cleared by reading port A.
            irq_a1 = false;
            irq_a2 = false;
            updateInterrupts();
            // CA2 read-strobe: when CA2 is an output in pulse/strobe mode,
            // reading port A pulses it low (and back high if strobe-E-reset
            // is selected). MAME `6821pia.cpp:403-412` (`port_a_r`):
            //
            //   if (c2_output(m_ctl_a) && c2_strobe_mode(m_ctl_a)) {
            //       if (m_out_ca2) set_out_ca2(false);
            //       if (strobe_e_reset(m_ctl_a)) set_out_ca2(true);
            //   }
            //
            // `set_out_ca2` has NO change guard of its own (unlike
            // `set_out_cb2`, `:362-385`) — it fires the handler every call —
            // so the `if (m_out_ca2)` at the call site is what keeps an
            // already-low CA2 from re-notifying the wire on every port-A
            // read. POM2 called it unconditionally.
            if (!c2_set_mode(ctl_a) && c2_output(ctl_a)) {
                if (out_ca2) setOutCa2(false);
                if (c2_set(ctl_a)) setOutCa2(true);
            }
            return ret;
        }
        return ddr_a;
    }
    case 0x01: {
        uint8_t ret = ctl_a;
        if (irq_a1) ret |= PIA_IRQ1;
        if (irq_a2 && c2_input(ctl_a)) ret |= PIA_IRQ2;
        return ret;
    }
    case 0x02: {
        if (output_selected(ctl_b)) {
            uint8_t ret = getInBValue();
            // MAME `6821pia.cpp:444-451` (`port_b_r`), and the comment
            // there says it explicitly: "This read will implicitly clear
            // the IRQ B1 flag. If CB2 is in write-strobe mode with CB1
            // restore, and a CB1 active transition set the flag, clearing
            // it will cause CB2 to go high again. Note that this is
            // different from what happens with port A." POM2 dropped the
            // restore, so a CB1-reset strobe stayed low forever after the
            // first CB1 edge.
            if (irq_b1 && !c2_set_mode(ctl_b) && !c2_set(ctl_b))
                setOutCb2(true);
            irq_b1 = false;
            irq_b2 = false;
            updateInterrupts();
            return ret;
        }
        return ddr_b;
    }
    case 0x03: {
        uint8_t ret = ctl_b;
        if (irq_b1) ret |= PIA_IRQ1;
        if (irq_b2 && c2_input(ctl_b)) ret |= PIA_IRQ2;
        return ret;
    }
    }
    return 0;
}

// ─── Bus writes (MAME write / port_X_w / control_X_w) ───────────────────

void MC6821::write(uint8_t offset, uint8_t data)
{
    switch (offset & 0x03) {
    case 0x00:
        if (output_selected(ctl_a)) {
            // port A write
            out_a = data;
            sendOutA();
        } else {
            // DDR A write
            if (ddr_a != data) {
                ddr_a = data;
                sendOutA();
            }
        }
        break;
    case 0x01: {
        // CRA write — bits 6/7 are read-only.
        data &= 0x3F;
        bool ca2_was_output = c2_output(ctl_a);
        ctl_a = data;
        if (c2_output(ctl_a)) {
            if (c2_set_mode(ctl_a)) {
                bool set = c2_set(ctl_a);
                if (!ca2_was_output || out_ca2 != set) setOutCa2(set);
            } else {
                if (!ca2_was_output || !out_ca2) setOutCa2(true);
            }
        } else if (ca2_was_output) {
            // CA2 reverted to input — pulled high.
            if (out_ca2_cb) out_ca2_cb(true);
        }
        updateInterrupts();
        break;
    }
    case 0x02:
        if (output_selected(ctl_b)) {
            out_b = data;
            sendOutB();
            // CB2 in write strobe mode: pulse low on every port-B write.
            // MAME `6821pia.cpp:689-706` (`port_b_w`) gates on
            // `c2_strobe_mode(m_ctl_b)` ALONE — no `c2_output` test, unlike
            // the A-side read strobe at `:403`. That asymmetry is real: the
            // B side drives the strobe off the write itself. POM2's extra
            // `c2_output(ctl_b)` suppressed it whenever CRB bit 5 was clear.
            if (!c2_set_mode(ctl_b)) {
                setOutCb2(false);
                // Strobe-E reset: bit 3 of CRB selects whether the strobe
                // self-clears at end of cycle.
                if (c2_set(ctl_b)) setOutCb2(true);
            }
        } else {
            if (ddr_b != data) {
                ddr_b = data;
                sendOutB();
            }
        }
        break;
    case 0x03: {
        // CRB write.
        data &= 0x3F;
        ctl_b = data;
        // MAME `6821pia.cpp:788-802` (`control_b_w`) computes `temp` and
        // calls `set_out_cb2(temp)` UNCONDITIONALLY — the same missing
        // `c2_output` asymmetry as `port_b_w` above. `set_out_cb2` is the
        // one with the change guard, so a no-op write stays a no-op.
        setOutCb2(c2_set_mode(ctl_b) ? c2_set(ctl_b) : true);
        updateInterrupts();
        break;
    }
    }
}

// ─── Output dispatch ────────────────────────────────────────────────────

void MC6821::sendOutA()
{
    if (out_a_cb) out_a_cb(getOutAValue());
}
void MC6821::sendOutB()
{
    if (out_b_cb) out_b_cb(getOutBValue());
}

void MC6821::setOutCa2(bool level)
{
    out_ca2 = level;
    if (out_ca2_cb) out_ca2_cb(level);
}
void MC6821::setOutCb2(bool level)
{
    if (level == out_cb2) return;
    out_cb2 = level;
    if (out_cb2_cb) out_cb2_cb(level);
}

// ─── External pin drivers ───────────────────────────────────────────────

void MC6821::setPortAInput(uint8_t v)
{
    in_a = v;
}

void MC6821::setPortBInput(uint8_t v)
{
    in_b = v;
}

void MC6821::setCA1(bool state)
{
    if ((in_ca1 != state) &&
        ((state && c1_low_to_high(ctl_a)) ||
         (!state && c1_high_to_low(ctl_a))))
    {
        irq_a1 = true;
        updateInterrupts();
        // CA2 in read-strobe mode with C1 reset: pulse high on CA1 edge.
        if (c2_output(ctl_a) && !c2_set_mode(ctl_a) && !c2_set(ctl_a) && !out_ca2) {
            setOutCa2(true);
        }
    }
    in_ca1 = state;
}

void MC6821::setCA2(bool state)
{
    if (c2_input(ctl_a) && (in_ca2 != state) &&
        ((state && c2_low_to_high(ctl_a)) ||
         (!state && c2_high_to_low(ctl_a))))
    {
        irq_a2 = true;
        updateInterrupts();
    }
    in_ca2 = state;
}

void MC6821::setCB1(bool state)
{
    if ((in_cb1 != state) &&
        ((state && c1_low_to_high(ctl_b)) ||
         (!state && c1_high_to_low(ctl_b))))
    {
        irq_b1 = true;
        updateInterrupts();
    }
    in_cb1 = state;
}

void MC6821::setCB2(bool state)
{
    if (c2_input(ctl_b) && (in_cb2 != state) &&
        ((state && c2_low_to_high(ctl_b)) ||
         (!state && c2_high_to_low(ctl_b))))
    {
        irq_b2 = true;
        updateInterrupts();
    }
    in_cb2 = state;
}

uint8_t MC6821::getPortAOutput() const { return getOutAValue(); }
uint8_t MC6821::getPortBOutput() const { return getOutBValue(); }

// ── Snapshot ──────────────────────────────────────────────────────────────
// Guest-visible register pairs + the edge latches. Callbacks are wiring
// (re-installed by the owning card), not state.

void MC6821::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.push_back(in_a);   out.push_back(in_b);
    out.push_back(in_ca1 ? 1 : 0);  out.push_back(in_ca2 ? 1 : 0);
    out.push_back(in_cb1 ? 1 : 0);  out.push_back(in_cb2 ? 1 : 0);
    out.push_back(out_a);  out.push_back(out_b);
    out.push_back(out_ca2 ? 1 : 0); out.push_back(out_cb2 ? 1 : 0);
    out.push_back(ddr_a);  out.push_back(ddr_b);
    out.push_back(ctl_a);  out.push_back(ctl_b);
    out.push_back(irq_a1 ? 1 : 0);  out.push_back(irq_a2 ? 1 : 0);
    out.push_back(irq_b1 ? 1 : 0);  out.push_back(irq_b2 ? 1 : 0);
    out.push_back(irq_a_state ? 1 : 0);
    out.push_back(irq_b_state ? 1 : 0);
    out.push_back(0); out.push_back(0);   // reserved, keeps the size fixed
}

size_t MC6821::loadSnapshotState(const uint8_t* data, size_t len)
{
    if (data == nullptr || len < kSnapshotBytes) return 0;
    size_t p = 0;
    in_a   = data[p++]; in_b   = data[p++];
    in_ca1 = data[p++] != 0; in_ca2 = data[p++] != 0;
    in_cb1 = data[p++] != 0; in_cb2 = data[p++] != 0;
    out_a  = data[p++]; out_b  = data[p++];
    out_ca2 = data[p++] != 0; out_cb2 = data[p++] != 0;
    ddr_a  = data[p++]; ddr_b  = data[p++];
    ctl_a  = data[p++]; ctl_b  = data[p++];
    irq_a1 = data[p++] != 0; irq_a2 = data[p++] != 0;
    irq_b1 = data[p++] != 0; irq_b2 = data[p++] != 0;
    irq_a_state = data[p++] != 0;
    irq_b_state = data[p++] != 0;
    p += 2;                                  // reserved
    return p;
}
