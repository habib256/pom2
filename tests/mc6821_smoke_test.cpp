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

// MC6821 PIA smoke test — pins the verbatim port of MAME's
// `src/devices/machine/6821pia.cpp` against MAME-equivalent invariants.
//
// What is pinned:
//
//   * Reset: all registers zeroed; CRA/CRB selects DDR access at offset
//     0/2 (bit 2 = 0); IRQ-A and IRQ-B output lines clear.
//   * DDR / port latch round-trip: write DDRA, write port A, read back
//     port A — composite read = (latch & DDR) | (input & ~DDR).
//   * Bits 6/7 of CRA/CRB are read-only on write.
//   * CRA bit 2 toggles between DDR and Data Port at offset 0; same for
//     CRB at offset 2.
//   * CA1 edge-triggered IRQ: low→high edge with CRA bit 1 set + bit 0
//     set raises IRQ-A; reading port A clears it.
//   * Output port-write callback fires on every Port A / DDR A write.
//   * IRQ output callback fires on transitions only, not redundantly.
//
// Source-of-truth: MAME `pia6821_device::read/write`, `ca1_w`, etc.

#include "MC6821.h"

#include <cassert>
#include <cstdio>

namespace {

void test_reset_state()
{
    MC6821 pia;
    pia.reset();
    assert(pia.getDdrA() == 0);
    assert(pia.getDdrB() == 0);
    assert(pia.getCRA() == 0);
    assert(pia.getCRB() == 0);
    assert(pia.irqA() == false);
    assert(pia.irqB() == false);
}

void test_ddr_data_select()
{
    MC6821 pia;
    pia.reset();
    // After reset, CRA bit 2 = 0 → offset 0 reads DDRA.
    assert(pia.read(0) == 0x00);

    // Write DDRA = $FF (all output).
    pia.write(0, 0xFF);
    assert(pia.getDdrA() == 0xFF);
    assert(pia.read(0) == 0xFF);     // still DDR access

    // Flip CRA bit 2: writing CRA with bit 2 set makes offset 0 access
    // the data port.
    pia.write(1, 0x04);
    assert(pia.getCRA() == 0x04);
    assert(pia.read(0) == 0x00);     // out_a still 0
}

void test_port_a_round_trip()
{
    MC6821 pia;
    pia.reset();
    pia.write(0, 0xF0);     // DDRA: upper nibble output, lower input
    pia.write(1, 0x04);     // CRA bit 2 → access port at offset 0
    pia.setPortAInput(0x0A); // external pulls input pins
    pia.write(0, 0xC5);     // out_a = $C5; pin output bits = $C0

    // Read port A: composite = (latch & DDR) | (input & ~DDR)
    //                        = ($C5 & $F0) | ($0A & $0F)
    //                        = $C0 | $0A = $CA
    assert(pia.read(0) == 0xCA);
}

void test_port_a_write_callback()
{
    MC6821 pia;
    pia.reset();
    int writes = 0;
    uint8_t lastVal = 0;
    pia.setPortAWriteCallback([&](uint8_t v) {
        ++writes;
        lastVal = v;
    });

    // Reset itself fires one write (internal pull-ups → $FF).
    const int afterReset = writes;
    pia.write(0, 0xFF);     // DDRA write — fires callback
    assert(writes == afterReset + 1);
    pia.write(1, 0x04);     // CRA write — does NOT fire (no port change)
    assert(writes == afterReset + 1);
    pia.write(0, 0xA5);     // port A write — fires
    assert(writes == afterReset + 2);
    assert(lastVal == 0xA5);
}

void test_ca1_edge_irq()
{
    MC6821 pia;
    pia.reset();
    int irq_transitions = 0;
    int last_state = -1;
    pia.setIrqACallback([&](bool s) {
        ++irq_transitions;
        last_state = s ? 1 : 0;
    });

    // CRA = 0x07: bit 0 = CA1 IRQ enable, bit 1 = low→high active edge,
    //             bit 2 = data-port select.
    pia.write(1, 0x07);

    // CA1 starts at true (set in constructor). Drop and raise.
    pia.setCA1(false);            // high→low transition (no IRQ since
                                  // we're in low→high mode)
    assert(pia.irqA() == false);

    pia.setCA1(true);             // low→high transition → IRQ
    assert(pia.irqA() == true);
    assert(irq_transitions == 1);
    assert(last_state == 1);

    // Verify the IRQ flag also surfaces in CRA bit 7 on read.
    assert((pia.read(1) & 0x80) == 0x80);

    // Reading port A clears IRQ.
    pia.write(0, 0x00);     // DDRA = all input
    pia.write(1, 0x05);     // re-set CRA (the previous CRA write enabled
                            // data-port; we need that for read(0) to
                            // exercise the IRQ-clearing path).

    pia.setCA1(false);
    pia.setCA1(true);             // re-arm IRQ
    assert(pia.irqA() == true);

    (void)pia.read(0);           // read port A → clear IRQ
    assert(pia.irqA() == false);
}

void test_cra_bits_67_readonly()
{
    MC6821 pia;
    pia.reset();
    // Try to set bits 6 and 7 via write — should be masked out.
    pia.write(1, 0xFF);
    assert((pia.getCRA() & 0xC0) == 0);
    // The lower bits should still be honoured.
    assert((pia.getCRA() & 0x3F) == 0x3F);
}

void test_port_b_three_state()
{
    MC6821 pia;
    pia.reset();
    pia.write(2, 0x00);     // DDRB = all input
    pia.write(3, 0x04);     // CRB bit 2 → data-port at offset 2
    pia.setPortBInput(0xA5);
    // With DDRB=0, all bits are inputs → read mirrors `in_b`.
    assert(pia.read(2) == 0xA5);

    pia.write(3, 0x00);     // CRB bit 2 = 0 → DDR access
    pia.write(2, 0xF0);     // DDRB upper output, lower input
    pia.write(3, 0x04);
    pia.write(2, 0x55);     // out_b = $55; pin = ($55 & $F0) = $50
    // Read port B: (out_b & DDR) | (in_b & ~DDR) = $50 | ($A5 & $0F) = $55
    assert(pia.read(2) == 0x55);
}

// ── The three B-side / CA2 divergences from MAME `6821pia.cpp` ─────────
//
// 1. `port_b_r` (`:439-460`) restores CB2 when a port-B read clears an
//    IRQ B1 raised by a CB1 edge, in write-strobe + CB1-reset mode. Its
//    own comment: "Note that this is different from what happens with
//    port A." POM2 had no restore at all.
// 2. `port_b_w` (`:689-706`) gates the CB2 write strobe on
//    `c2_strobe_mode(m_ctl_b)` ALONE — no `c2_output` test, unlike the
//    A-side read strobe at `:403`. Same for `control_b_w` (`:788-802`),
//    which calls `set_out_cb2` unconditionally.
// 3. `port_a_r` (`:403-412`) guards the strobe-low with `if (m_out_ca2)`,
//    because `set_out_ca2` (`:339-355`) has no change guard of its own —
//    unlike `set_out_cb2` (`:362-385`), which does.
void test_cb2_strobe_and_ca2_change_guard()
{
    // CRB = $04 (data port) | $00 (CB2 write-strobe, CB1-reset). Bit 5
    // (c2_output) deliberately CLEAR — upstream strobes anyway.
    {
        MC6821 pia;
        pia.reset();
        int edges = 0;
        bool level = true;
        pia.setCB2WriteCallback([&](bool v) { ++edges; level = v; });
        pia.write(3, 0x04);
        const int afterCtl = edges;
        assert(level && "control_b_w must park CB2 high in strobe mode");
        pia.write(2, 0x5A);                 // port-B write → strobe low
        assert(edges > afterCtl && !level &&
               "port-B write must strobe CB2 low with CRB bit 5 clear");

        // CB1 edge sets IRQ B1; the port-B read that clears it restores
        // CB2 high (CB1-reset mode = CRB bit 3 clear).
        pia.write(3, 0x05);                 // + CB1 IRQ enable, still strobe
        pia.setCB1(true);
        pia.setCB1(false);                  // active transition (high→low)
        pia.write(2, 0x5A);                 // strobe low again
        assert(!level);
        (void)pia.read(2);                  // port-B read → CB1 restore
        assert(level && "port-B read must restore CB2 on the CB1-reset rule");
    }

    // CA2 read-strobe: the second and later port-A reads must NOT re-notify
    // the wire while CA2 is already low (strobe-E reset OFF = CRA bit 3
    // clear, so it stays low until a CA1 edge).
    {
        MC6821 pia;
        pia.reset();
        int edges = 0;
        pia.setCA2WriteCallback([&](bool) { ++edges; });
        pia.write(1, 0x04 | 0x20);          // data port + CA2 output, strobe
        const int armed = edges;            // control_a_w parked it high
        (void)pia.read(0);                  // first read: high → low
        const int afterFirst = edges;
        assert(afterFirst == armed + 1 && "first port-A read strobes CA2 low");
        (void)pia.read(0);
        (void)pia.read(0);
        assert(edges == afterFirst &&
               "an already-low CA2 must not re-notify on every port-A read");
    }
}

}  // namespace

int main()
{
    test_reset_state();
    test_ddr_data_select();
    test_port_a_round_trip();
    test_port_a_write_callback();
    test_ca1_edge_irq();
    test_cra_bits_67_readonly();
    test_port_b_three_state();
    test_cb2_strobe_and_ca2_change_guard();

    std::printf("OK mc6821_smoke\n");
    return 0;
}
