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

// MC68705P3 decode + execute smoke test — pins the verbatim port of
// MAME's `src/devices/cpu/m6805/m6805.cpp` HMOS instruction set against
// MAME-equivalent invariants. The mouse-card firmware will be the real
// load test, but at this stage we drive synthetic firmware images to
// confirm decode, addressing modes, stack semantics, and interrupt
// handling all line up with MAME's documented behaviour.
//
// What is pinned:
//
//   * Reset vector fetched big-endian from $07FE/$07FF.
//   * SP starts at $7F (kSpMask), wraps in [$60, $7F].
//   * I-flag set on reset (firmware must CLI before IRQs are taken).
//   * LDA/STA/JMP/JSR/RTS round-trip through internal RAM.
//   * Push order on IRQ: PCH, PCL, X, A, CC (ascending addresses).
//   * RTI restores in reverse: CC, A, X, PCH, PCL.
//   * Cycle counts match MAME `s_hmos_cycles[]` for a hand-picked set
//     of frequently-used opcodes.
//   * Port latch / DDR write callback fires on every $00..$06 store.
//   * IRQ line, when asserted with I clear, vectors via $07FA/$07FB.

#include "M68705P3.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

// Build a 2 KB EPROM image. The image's offset 0 maps to chip address
// $0000 — MAME's `loadRomBytes` discards the lower $80 (which overlaps
// I/O regs and RAM). We arrange:
//
//   - Reset vector at file offset $7FE/$7FF (chip $07FE/$07FF).
//   - Code starting at chip $0080 (offset $80 in file).
struct RomImage
{
    std::vector<uint8_t> bytes{std::vector<uint8_t>(0x800, 0xFF)};

    void emit(uint16_t addr, std::initializer_list<uint8_t> bs) {
        for (uint8_t b : bs) bytes[addr++] = b;
    }
    void put(uint16_t addr, uint8_t b) { bytes[addr] = b; }

    // Set the reset vector to `entry`. Stored big-endian at $7FE/$7FF.
    void setReset(uint16_t entry) {
        bytes[0x7FE] = static_cast<uint8_t>((entry >> 8) & 0xFF);
        bytes[0x7FF] = static_cast<uint8_t>(entry & 0xFF);
    }
    void setIrq(uint16_t entry) {
        bytes[0x7FA] = static_cast<uint8_t>((entry >> 8) & 0xFF);
        bytes[0x7FB] = static_cast<uint8_t>(entry & 0xFF);
    }
};

void test_reset_vector()
{
    RomImage rom;
    // Code at $0080: HALT-equivalent infinite loop (BRA $0080 = $20 $FE).
    // ... but we just want the fetcher to land on the right byte.
    rom.emit(0x80, { 0x9D });        // NOP at chip $0080
    rom.setReset(0x0080);

    M68705P3 cpu;
    assert(cpu.loadRomBytes(rom.bytes.data(), rom.bytes.size()));
    cpu.reset();
    assert(cpu.getPC() == 0x0080);
    assert(cpu.getS() == 0x7F);
    // I-flag set on reset.
    assert((cpu.getCC() & 0x08) == 0x08);

    // Step the NOP — PC advances by 1, takes 2 cycles.
    const int n = cpu.step();
    assert(n == 2);
    assert(cpu.getPC() == 0x0081);
}

void test_lda_immediate_and_n_z_flags()
{
    RomImage rom;
    // LDA #$00         A6 00       (clears A, sets Z)
    // LDA #$80         A6 80       (loads $80, sets N)
    // LDA #$42         A6 42       (loads $42, clears N+Z)
    rom.emit(0x80, { 0xA6, 0x00, 0xA6, 0x80, 0xA6, 0x42 });
    rom.setReset(0x0080);

    M68705P3 cpu;
    cpu.loadRomBytes(rom.bytes.data(), rom.bytes.size());
    cpu.reset();

    assert(cpu.step() == 2);     // LDA #$00
    assert(cpu.getA() == 0x00);
    assert(cpu.getCC() & 0x02);  // Z set
    assert(!(cpu.getCC() & 0x04)); // N clear

    assert(cpu.step() == 2);     // LDA #$80
    assert(cpu.getA() == 0x80);
    assert(!(cpu.getCC() & 0x02)); // Z clear
    assert(cpu.getCC() & 0x04);    // N set

    assert(cpu.step() == 2);     // LDA #$42
    assert(cpu.getA() == 0x42);
    assert(!(cpu.getCC() & 0x02));
    assert(!(cpu.getCC() & 0x04));
}

void test_sta_direct_and_lda_direct()
{
    RomImage rom;
    // LDA #$5A         A6 5A
    // STA $30          B7 30
    // LDA #$00         A6 00       (clobber)
    // LDA $30          B6 30       (recover $5A)
    rom.emit(0x80, { 0xA6, 0x5A, 0xB7, 0x30, 0xA6, 0x00, 0xB6, 0x30 });
    rom.setReset(0x0080);

    M68705P3 cpu;
    cpu.loadRomBytes(rom.bytes.data(), rom.bytes.size());
    cpu.reset();
    cpu.step();     // LDA #$5A
    cpu.step();     // STA $30
    assert(cpu.peekRam(0x30) == 0x5A);
    cpu.step();     // LDA #$00
    cpu.step();     // LDA $30
    assert(cpu.getA() == 0x5A);
}

void test_jsr_rts_stack_round_trip()
{
    RomImage rom;
    // $0080: LDX #$AA   AE AA
    // $0082: JSR $00A0  BD A0
    // $0084: NOP        9D            ← return target
    // $0085: BRA $0085  20 FE         ← park
    //
    // $00A0: INX        5C
    // $00A1: RTS        81
    rom.emit(0x80, { 0xAE, 0xAA, 0xBD, 0xA0, 0x9D, 0x20, 0xFE });
    rom.emit(0xA0, { 0x5C, 0x81 });
    rom.setReset(0x0080);

    M68705P3 cpu;
    cpu.loadRomBytes(rom.bytes.data(), rom.bytes.size());
    cpu.reset();

    cpu.step();   // LDX #$AA — 2 cyc
    assert(cpu.getX() == 0xAA);
    assert(cpu.step() == 7);  // JSR direct = 7 cyc
    // After JSR, PC should be at $00A0; SP decremented twice.
    assert(cpu.getPC() == 0x00A0);
    assert(cpu.getS() == 0x7D);     // pushed PCL, PCH

    cpu.step();   // INX = 4 cyc (HMOS table says 4)
    assert(cpu.getX() == 0xAB);
    assert(cpu.step() == 6);    // RTS = 6 cyc
    assert(cpu.getPC() == 0x0084);
    assert(cpu.getS() == 0x7F);
    cpu.step();    // NOP
}

void test_branch_taken_and_not_taken()
{
    // BNE forward (cond=Z clear) vs. BEQ forward (cond=Z set). Cycles
    // for branches are 4 regardless of taken/not-taken (HMOS table).
    RomImage rom;
    // $0080: LDA #$00      A6 00     ; sets Z
    // $0082: BEQ $0086     27 02     ; taken, jumps +2 → $0086
    // $0084: NOP           9D         (skipped)
    // $0085: NOP           9D         (skipped)
    // $0086: LDA #$01      A6 01
    // $0088: BEQ $008C     27 02     ; not taken (Z clear after LDA #1)
    // $008A: NOP           9D
    // $008B: NOP           9D
    // $008C: NOP           9D
    rom.emit(0x80, { 0xA6, 0x00, 0x27, 0x02, 0x9D, 0x9D,
                     0xA6, 0x01, 0x27, 0x02, 0x9D, 0x9D, 0x9D });
    rom.setReset(0x0080);

    M68705P3 cpu;
    cpu.loadRomBytes(rom.bytes.data(), rom.bytes.size());
    cpu.reset();

    cpu.step();   // LDA #$00
    assert(cpu.step() == 4);    // BEQ taken
    assert(cpu.getPC() == 0x0086);
    cpu.step();   // LDA #$01
    assert(cpu.step() == 4);    // BEQ not taken
    assert(cpu.getPC() == 0x008A);
}

void test_cycle_counts()
{
    // Spot-check a few opcodes against MAME's s_hmos_cycles[]:
    //   $9D NOP  = 2
    //   $A6 LDA imm = 2
    //   $B6 LDA dir = 4
    //   $C6 LDA ext = 5
    //   $E6 LDA ix1 = 5
    //   $F6 LDA ix  = 4
    //   $BD JSR dir = 7
    //   $CD JSR ext = 8
    //   $81 RTS     = 6
    //   $80 RTI     = 9
    //   $AD BSR     = 8
    RomImage rom;
    rom.emit(0x80, {
        0x9D,                    // NOP             $0080  2c
        0xA6, 0x42,              // LDA #$42        $0081  2c
        0xB6, 0x30,              // LDA $30         $0083  4c
        0xC6, 0x00, 0x40,        // LDA $0040       $0085  5c
        0xAE, 0x10,              // LDX #$10        $0088  2c
        0xE6, 0x20,              // LDA $20,X       $008A  5c
        0xF6,                    // LDA ,X          $008C  4c
    });
    rom.setReset(0x0080);
    M68705P3 cpu;
    cpu.loadRomBytes(rom.bytes.data(), rom.bytes.size());
    cpu.reset();

    assert(cpu.step() == 2);  // NOP
    assert(cpu.step() == 2);  // LDA imm
    assert(cpu.step() == 4);  // LDA dir
    assert(cpu.step() == 5);  // LDA ext
    assert(cpu.step() == 2);  // LDX imm
    assert(cpu.step() == 5);  // LDA ix1
    assert(cpu.step() == 4);  // LDA ix
}

void test_irq_vector_and_push_order()
{
    // Force an IRQ and verify the vector + stack frame match MAME's
    // documented push order: PCH, PCL, X, A, CC (low-to-high).
    RomImage rom;
    // $0080: LDA #$11    A6 11
    // $0082: LDX #$22    AE 22
    // $0084: CLI         9A          ← I-flag clear, IRQ takes effect
    // $0085: BRA $0085   20 FE       ← park
    //
    // $0100: LDA #$99    A6 99       ← IRQ handler
    // $0102: BRA $0102   20 FE
    rom.emit(0x80, { 0xA6, 0x11, 0xAE, 0x22, 0x9A, 0x20, 0xFE });
    rom.emit(0x100, { 0xA6, 0x99, 0x20, 0xFE });
    rom.setReset(0x0080);
    rom.setIrq(0x0100);

    M68705P3 cpu;
    cpu.loadRomBytes(rom.bytes.data(), rom.bytes.size());
    cpu.reset();

    cpu.step(); // LDA #$11
    cpu.step(); // LDX #$22
    cpu.step(); // CLI

    // Now assert IRQ. The next step() should service it.
    cpu.setIrq(true);
    cpu.step(); // serviceInterrupt + first instruction of handler
    // After interrupt service, PC should be in the handler (post first
    // LDA #$99, since serviceInterrupt is called, then we execute one
    // instruction). Actually serviceInterrupt sets PC to vector then
    // step() reads opcode at $0100 and executes it.
    assert(cpu.getA() == 0x99);
    assert(cpu.getPC() == 0x0102);

    // Stack should hold: SP=$7F-5=$7A, with PCH/PCL/X/A/CC at $7B..$7F.
    // (Stack on real chip pushes from $7F down, decrementing.)
    // After 5 pushes from $7F, SP = $7A.
    assert(cpu.getS() == 0x7A);

    // Verify the actual stack contents. MAME's pushword pushes PCL
    // first then PCH (each pushbyte writes at S then decrements S), so
    // ascending addresses hold: $7F = PCL, $7E = PCH, $7D = X, $7C = A,
    // $7B = CC. RTI's pullword reverses this: pulls PCH first, then
    // PCL, reconstructing PC = (PCH << 8) | PCL.
    assert(cpu.peekRam(0x7F) == 0x85);              // PCL of $0085
    assert(cpu.peekRam(0x7E) == 0x00);              // PCH of $0085
    assert(cpu.peekRam(0x7D) == 0x22);              // X
    assert(cpu.peekRam(0x7C) == 0x11);              // A
    // CC pushed = pre-IRQ CC, with I cleared (CLI was just executed).
    assert((cpu.peekRam(0x7B) & 0x08) == 0x00);
    // CC inside ISR has I set.
    assert(cpu.getCC() & 0x08);
}

void test_port_latch_and_callback()
{
    RomImage rom;
    // $0080: LDA #$AA      A6 AA
    // $0082: STA $00       B7 00     ; write Port A latch
    // $0084: LDA #$0F      A6 0F
    // $0086: STA $04       B7 04     ; write Port A DDR (lower 4 = output)
    // $0088: BRA $0088     20 FE
    rom.emit(0x80, { 0xA6, 0xAA, 0xB7, 0x00, 0xA6, 0x0F, 0xB7, 0x04, 0x20, 0xFE });
    rom.setReset(0x0080);

    M68705P3 cpu;
    cpu.loadRomBytes(rom.bytes.data(), rom.bytes.size());
    cpu.reset();

    int callbacks = 0;
    int lastPort = -1;
    cpu.setPortWriteCallback([&](int p) {
        ++callbacks;
        lastPort = p;
    });

    cpu.step();  // LDA #$AA
    cpu.step();  // STA $00 — fires callback for port 0
    assert(callbacks == 1);
    assert(lastPort == 0);
    assert(cpu.getPortLatch(0) == 0xAA);

    cpu.step();  // LDA #$0F
    cpu.step();  // STA $04 — fires callback for port 0 (DDR)
    assert(callbacks == 2);
    assert(cpu.getPortDdr(0) == 0x0F);

    // Output bits 0..3 surface the latch's lower nibble; bits 4..7 are
    // input (DDR=0) so they float high. Pin observation: $AA & $0F |
    // $FF & $F0 = $0A | $F0 = $FA.
    assert(cpu.getPortPins(0) == 0xFA);
}

void test_port_input_read()
{
    RomImage rom;
    // DDR Port B = $00 (all input). Drive input pins externally with
    // setPortInput(1, 0x55), then have the firmware read $01 into A.
    //
    // $0080: LDA #$00     A6 00
    // $0082: STA $05      B7 05      ; Port B DDR = all input
    // $0084: LDA $01      B6 01      ; read Port B → A
    // $0086: STA $30      B7 30      ; store A to RAM
    // $0088: BRA $0088    20 FE
    rom.emit(0x80, { 0xA6, 0x00, 0xB7, 0x05, 0xB6, 0x01, 0xB7, 0x30, 0x20, 0xFE });
    rom.setReset(0x0080);

    M68705P3 cpu;
    cpu.loadRomBytes(rom.bytes.data(), rom.bytes.size());
    cpu.reset();
    cpu.setPortInput(1, 0x55);

    cpu.step(); // LDA #$00
    cpu.step(); // STA DDRB
    cpu.step(); // LDA Port B
    cpu.step(); // STA $30
    assert(cpu.peekRam(0x30) == 0x55);
}

void test_clr_is_write_only()
{
    // 6805 CLR on a memory operand is WRITE-ONLY (MAME `clr` uses ARGADDR,
    // not ARGBYTE). On a port address ($00/$01/$02) a spurious read would
    // fire the port-read callback (the mouse bridge's quadrature edge).
    RomImage rom;
    // $0080: CLR $01   (3F 01)   ; CLR Port B (direct)
    // $0082: BRA $0082 (20 FE)
    rom.emit(0x80, { 0x3F, 0x01, 0x20, 0xFE });
    rom.setReset(0x0080);

    M68705P3 cpu;
    cpu.loadRomBytes(rom.bytes.data(), rom.bytes.size());
    cpu.reset();

    int reads = 0;
    cpu.setPortReadCallback([&](int) { ++reads; return uint8_t(0x55); });

    cpu.step();   // CLR $01
    assert(reads == 0);                    // must NOT read the port
    assert(cpu.getPortLatch(1) == 0x00);   // latch cleared to 0
    std::printf("  ok: CLR memory is write-only (no spurious port read)\n");
}

void test_mul()
{
    // MUL ($42): X:A = X × A, clears H and C. Set H (via a half-carrying
    // ADD) and C (via SEC) first so we can prove MUL clears both.
    RomImage rom;
    // $0080: LDA #$08   A6 08
    // $0082: ADD #$08   AB 08    ; A=$10, H set (carry out of bit 3)
    // $0084: LDA #$20   A6 20    ; A=$20 (LDA leaves H untouched)
    // $0086: LDX #$10   AE 10
    // $0088: SEC        99       ; C set
    // $0089: MUL        42       ; $10 × $20 = $0200 → X=$02, A=$00
    // $008A: BRA $008A  20 FE
    rom.emit(0x80, { 0xA6, 0x08, 0xAB, 0x08, 0xA6, 0x20,
                     0xAE, 0x10, 0x99, 0x42, 0x20, 0xFE });
    rom.setReset(0x0080);

    M68705P3 cpu;
    cpu.loadRomBytes(rom.bytes.data(), rom.bytes.size());
    cpu.reset();

    cpu.step();   // LDA #$08
    cpu.step();   // ADD #$08
    assert(cpu.getCC() & 0x10);    // H set by the half-carry
    cpu.step();   // LDA #$20
    cpu.step();   // LDX #$10
    cpu.step();   // SEC
    assert(cpu.getCC() & 0x01);    // C set
    cpu.step();   // MUL
    assert(cpu.getX() == 0x02);            // high byte
    assert(cpu.getA() == 0x00);            // low byte
    assert((cpu.getCC() & 0x01) == 0);     // C cleared
    assert((cpu.getCC() & 0x10) == 0);     // H cleared
    std::printf("  ok: MUL ($42) X:A = X*A, clears H and C\n");
}

// ── Timer: the MOR decides the prescaler, not TCR ────────────────────────
//
// MAME `m68705.cpp:465-476` reads the Mask Option Register at $0784
// (`m68705p3_device::get_mask_options()` = `get_user_rom()[0x0784] & 0xf7`,
// `:765-768`). `MOR_TOPT` (0x40) set → TIMER_MOR: divisor from `MOR_PS`,
// source from `(MOR_CLS|MOR_TIE) >> 4`, and `tcr_w` then forces TCR_TIE and
// does NOT reprogram either. Only with TOPT clear does TCR own them
// (TIMER_PGM). `roms/mouse_341-0269.bin`, the Apple Mouse Card's MCU dump,
// carries MOR = $40 → ÷1, so the timer interrupt period is 256 MCU cycles,
// not the 32768 a hard-coded TIMER_PGM ÷128 produced.
//
// Measured the way the hardware defines it: MCU cycles between two entries
// through the $07F8 timer vector.
long measureTimerPeriod(uint8_t mor)
{
    RomImage rom;
    // $0080  A6 07     LDA #$07         ; TIM clear, TIR clear, PS = 7
    // $0082  B7 09     STA $09          ; TCR write — arms the timer IRQ
    // $0084  9A        CLI              ; unmask, then idle forever
    // $0085  20 FE     BRA $0085
    //
    // The SAME firmware in both configurations: under TIMER_MOR that PS = 7
    // is ignored (MAME's `tcr_w` only reprograms the divisor under
    // TIMER_PGM), under TIMER_PGM it selects ÷128.
    rom.emit(0x80, { 0xA6, 0x07, 0xB7, 0x09, 0x9A, 0x20, 0xFE });
    rom.setReset(0x0080);
    // Timer ISR at $0100: mark the entry on port C ($02, unused by this
    // test's wiring) then acknowledge by clearing TCR.TIR and RTI.
    //   $0100  A6 07   LDA #$07
    //   $0102  B7 02   STA $02          ; port-C latch write → callback
    //   $0104  B7 09   STA $09          ; TCR: TIR cleared, TIM clear, PS=7
    //   $0106  80      RTI
    rom.emit(0x0100, { 0xA6, 0x07, 0xB7, 0x02, 0xB7, 0x09, 0x80 });
    rom.bytes[0x7F8] = 0x01;
    rom.bytes[0x7F9] = 0x00;
    rom.put(0x784, mor);

    M68705P3 cpu;
    assert(cpu.loadRomBytes(rom.bytes.data(), rom.bytes.size()));
    cpu.reset();

    long cycles = 0;
    long stamps[3] = { -1, -1, -1 };
    int hits = 0;
    cpu.setPortWriteCallback([&](int port) {
        if (port == 2 && hits < 3) stamps[hits++] = cycles;
    });
    // Enough budget for two periods at ÷128 (2 × 32768) with slack.
    for (int i = 0; i < 100000 && hits < 3; ++i) cycles += cpu.run(1);
    assert(hits == 3 && "timer never vectored through $07F8");
    // stamps[0] is the first (post-reset TDR = $FF) shot; the period is
    // the gap between the two steady-state ones.
    return stamps[2] - stamps[1];
}

void test_timer_mor_divisor()
{
    // The shipped Apple Mouse MCU dump: MOR = $40 → TOPT set, PS = 0.
    const long morPeriod = measureTimerPeriod(0x40);
    // 256 TDR steps at ÷1, plus the ISR's own run to the acknowledging
    // STA $09 (the counter keeps running through it).
    assert(morPeriod >= 256 && morPeriod <= 275);
    std::printf("  ok: MOR $40 (TOPT, PS=0) → timer period %ld MCU cycles\n",
                morPeriod);

    // TOPT clear → TIMER_PGM, divisor from the firmware's TCR writes; this
    // firmware never writes a PS field, so the reset default ÷128 stands.
    const long pgmPeriod = measureTimerPeriod(0x00);
    assert(pgmPeriod >= 256L * 128 && pgmPeriod <= 256L * 128 + 32);
    std::printf("  ok: MOR $00 (TOPT clear) → TIMER_PGM, period %ld MCU cycles\n",
                pgmPeriod);

    // tcr_r: PSC (bit 3) reads back as 1 on a MOR part, 0 on a PGM one
    // (MAME `m68705.h:86`).
    for (uint8_t mor : { uint8_t(0x40), uint8_t(0x00) }) {
        RomImage rom;
        //   $0080  B6 09   LDA $09     ; read TCR
        //   $0082  20 FE   BRA $0082
        rom.emit(0x80, { 0xB6, 0x09, 0x20, 0xFE });
        rom.setReset(0x0080);
        rom.put(0x784, mor);
        M68705P3 cpu;
        assert(cpu.loadRomBytes(rom.bytes.data(), rom.bytes.size()));
        cpu.reset();
        assert(cpu.timerIsMorConfigured() == ((mor & 0x40) != 0));
        cpu.step();
        assert(((cpu.getA() & 0x08) != 0) == ((mor & 0x40) != 0));
    }
    std::printf("  ok: TCR read-back PSC bit follows the MOR option\n");
}

}  // namespace

int main()
{
    test_reset_vector();
    test_lda_immediate_and_n_z_flags();
    test_sta_direct_and_lda_direct();
    test_jsr_rts_stack_round_trip();
    test_branch_taken_and_not_taken();
    test_cycle_counts();
    test_irq_vector_and_push_order();
    test_port_latch_and_callback();
    test_port_input_read();
    test_clr_is_write_only();
    test_mul();
    test_timer_mor_divisor();

    std::printf("OK m68705_decode_smoke\n");
    return 0;
}
