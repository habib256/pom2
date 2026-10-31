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

// Input I/O smoke test — pins the observable bus behaviour of the keyboard,
// the paddles and the push-buttons THROUGH memRead/memWrite, deliberately
// without reaching into Memory's internals.
//
// This is the safety net the Memory god-object split (TODO P2: extract
// Keyboard + PaddleInputs) has to pass unchanged. The split moves this state
// out of Memory into its own classes; if the extraction is behaviour-
// preserving, every assertion here still holds against the same
// memRead/memWrite calls a 6502 driver makes. Written BEFORE the split, on
// purpose — a net woven after the fall catches nothing.
//
// References for the numbers, all already cited in Memory.cpp:
//   * $C000 keyboard latch: low 7 = key, bit 7 = strobe/ready.
//   * $C010 clears the strobe and drains one queued byte.
//   * $C061-$C063 push-buttons / Open-Apple / Solid-Apple in bit 7.
//   * $C064-$C067 paddle RC discharge: bit 7 high while
//     `cycleCounter - paddleLatchCycle < paddleValue * 11`.
//   * $C070 (mirrored $C070-$C07F) re-arms the paddle timer.

#include "Memory.h"
#include "PaddleMouse.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr uint16_t kKbd    = 0xC000;
constexpr uint16_t kStrobe = 0xC010;
constexpr uint16_t kBtn0   = 0xC061;
constexpr uint16_t kBtn1   = 0xC062;
constexpr uint16_t kBtn2   = 0xC063;
constexpr uint16_t kPdl0   = 0xC064;
constexpr uint16_t kPdlRst = 0xC070;

bool ready(Memory& mem)     { return (mem.memRead(kKbd) & 0x80) != 0; }
uint8_t keyByte(Memory& mem){ return mem.memRead(kKbd) & 0x7F; }

// ── 1. The keyboard latch: queue, read, strobe-clear ──────────────────────
void testKeyboardLatch()
{
    Memory mem;

    // Nothing queued: strobe low, and clearing it is harmless.
    assert(!ready(mem));
    (void)mem.memRead(kStrobe);
    assert(!ready(mem));

    // One key: strobe high, low 7 bits carry it, and the byte STAYS latched
    // across repeated $C000 reads (the strobe is level, not a one-shot read).
    mem.queueKey('A');
    assert(ready(mem));
    assert(keyByte(mem) == 'A');
    assert(keyByte(mem) == 'A' && "reading $C000 must not consume the key");
    assert(ready(mem)   && "reading $C000 must not clear the strobe");

    // $C010 clears the strobe. With nothing else queued the key is gone.
    (void)mem.memRead(kStrobe);
    assert(!ready(mem) && "a $C010 access must drop the strobe");

    // A live keystroke is NEWEST-WINS, not FIFO: two queueKey calls with no
    // paste in flight behave like the hardware latch — the second overwrites
    // the first unread byte, exactly as fast typing does on real silicon.
    mem.queueKey('X');
    mem.queueKey('Y');
    assert(ready(mem) && keyByte(mem) == 'Y' &&
           "queueKey with no paste in flight must be newest-wins");
    (void)mem.memRead(kStrobe);
    assert(!ready(mem) && "no queue behind a live key — the strobe stays clear");

    // A host PASTE, by contrast, is a FIFO drained one byte per strobe clear.
    const char burst[] = "AB";
    const size_t n = mem.pasteRawKeys(burst, 2);
    assert(n == 2);
    assert(ready(mem) && keyByte(mem) == 'A');
    (void)mem.memRead(kStrobe);
    assert(ready(mem) && keyByte(mem) == 'B' &&
           "the paste FIFO must promote the next byte on the strobe clear");
    (void)mem.memRead(kStrobe);
    assert(!ready(mem));

    std::printf("[ OK ] keyboard latch: newest-wins key, FIFO paste, strobe-drain\n");
}

// ── 2. Push-buttons and the Apple modifier keys ───────────────────────────
void testButtons()
{
    Memory mem;

    // All buttons up: bit 7 low on $C061-$C063.
    assert((mem.memRead(kBtn0) & 0x80) == 0);
    assert((mem.memRead(kBtn1) & 0x80) == 0);
    assert((mem.memRead(kBtn2) & 0x80) == 0);

    // Paddle buttons 0..2 raise bit 7 of $C061..$C063 respectively.
    mem.setPaddleButton(0, true);
    assert((mem.memRead(kBtn0) & 0x80) != 0);
    assert((mem.memRead(kBtn1) & 0x80) == 0 && "button 0 must not bleed to $C062");
    mem.setPaddleButton(0, false);
    assert((mem.memRead(kBtn0) & 0x80) == 0);

    // Open-Apple is wired to PB0 ($C061), Solid-Apple to PB1 ($C062) — the
    // two sources are OR'd with the paddle buttons on the same wire.
    mem.setOpenAppleKey(true);
    assert((mem.memRead(kBtn0) & 0x80) != 0);
    assert((mem.memRead(kBtn1) & 0x80) == 0);
    mem.setOpenAppleKey(false);
    mem.setSolidAppleKey(true);
    assert((mem.memRead(kBtn1) & 0x80) != 0);
    assert((mem.memRead(kBtn0) & 0x80) == 0);
    mem.setSolidAppleKey(false);
    assert((mem.memRead(kBtn1) & 0x80) == 0);

    std::printf("[ OK ] push-buttons + Open/Solid-Apple on PB0/PB1\n");
}

// ── 2b. The //e Shift-key mod on PB2 (INPUT-001) ──────────────────────────
// MAME `apple2e.cpp` c000_r $C063: with `kbd_shift_mod` the bit is the
// Shift key, active LOW (the wire grounds PB2 while Shift is held), and the
// game port's button 2 is out of the circuit. Without the mod, Shift is not
// on $C063 at all. The old dead code OR'd Shift in HIGH.
void testShiftKeyMod()
{
    Memory mem;
    mem.setIIEMode(true);
    mem.setShiftKey(true);
    assert((mem.memRead(kBtn2) & 0x80) == 0 && "no mod: Shift is not on PB2");
    mem.setShiftKey(false);

    mem.setShiftKeyMod(true);
    assert((mem.memRead(kBtn2) & 0x80) != 0 && "mod: Shift UP reads $80");
    mem.setShiftKey(true);
    assert((mem.memRead(kBtn2) & 0x80) == 0 && "mod: Shift DOWN grounds PB2");
    mem.setShiftKey(false);
    mem.setPaddleButton(2, true);
    assert((mem.memRead(kBtn2) & 0x80) != 0 && "mod: button 2 is cut off, Shift up");
    mem.setPaddleButton(2, false);

    // A ][+ has no such option here (MAME wires it through another switch).
    mem.setIIEMode(false);
    assert((mem.memRead(kBtn2) & 0x80) == 0 && "][+: PB2 is the game port's");
    std::printf("[ OK ] //e Shift-key mod: Shift on PB2, active low\n");
}

// ── 2b'. The ][ / ][+ Shift-key mods: MAME apple2.cpp's three wirings ─────
// c060_r cases 1-3 and 6-7: SW2 = Shift; PADDL2 = Shift + PADDL3 = Ctrl;
// SW0 = Shift + SW1 = Ctrl. Every wire active LOW (a2kbd.h "0 = pressed"),
// read only on a ][ / ][+ — the //e keeps its own setting.
void testIiShiftWirings()
{
    using W = pom2::PaddleInputs::IiShiftWiring;
    Memory mem;                                         // ][+ mode
    auto up = [&](uint16_t a) { return (mem.memRead(a) & 0x80) != 0; };

    mem.setShiftKey(true);
    assert(!up(kBtn2) && "not fitted: Shift is nowhere");
    mem.setShiftKey(false);

    mem.setIiShiftWiring(W::Sw2);
    assert(up(kBtn2));
    mem.setShiftKey(true);
    assert(!up(kBtn2) && "SW2: Shift down grounds PB2");
    mem.setShiftKey(false);

    mem.setIiShiftWiring(W::Paddl23);
    assert(up(0xC066) && up(0xC067) && "PADDL2/3: both keys up read $80");
    mem.setShiftKey(true);
    assert(!up(0xC066) && up(0xC067));
    mem.setShiftKey(false);
    mem.setControlKey(true);
    assert(up(0xC066) && !up(0xC067));
    mem.setControlKey(false);

    mem.setIiShiftWiring(W::Sw01);
    assert(up(kBtn0) && up(kBtn1));
    mem.setShiftKey(true);
    assert(!up(kBtn0) && up(kBtn1));
    mem.setShiftKey(false);
    mem.setControlKey(true);
    assert(up(kBtn0) && !up(kBtn1));
    mem.setControlKey(false);

    // A //e ignores every ][+ wiring: its PB0/PB1 are the Apple keys.
    mem.setIIEMode(true);
    assert(!up(kBtn0) && !up(kBtn1));
    std::printf("[ OK ] ][+ Shift-key mods: SW2, PADDL2/3, SW0/1, active low\n");
}

// ── 2b''. Nothing on the game port ─────────────────────────────────────
// MAME apple2.cpp c060_r: a ][+'s PB0/PB1 "poll high if not connected,
// unlike IIe" (their pull-downs are in the controller), and on both machines
// an empty port's paddles never time out (`!is_device_connected()`). The
// setting defaults to connected, so nothing changes until it is turned off.
void testEmptyGamePort()
{
    auto up = [](Memory& m, uint16_t a) { return (m.memRead(a) & 0x80) != 0; };
    Memory plus;                                        // ][+
    assert(!up(plus, kBtn0) && !up(plus, kBtn1) && "connected by default: released");
    plus.setGamePortConnected(false);
    assert(up(plus, kBtn0) && up(plus, kBtn1) && "][+ empty port: PB0/PB1 float high");
    assert(!up(plus, kBtn2) && "PB2 is not pulled up");
    plus.memRead(kPdlRst);
    for (int i = 0; i < 20000; ++i) plus.memRead(0xC000);   // far past any count
    assert(up(plus, kPdl0) && up(plus, 0xC067) && "empty port: paddles never time out");
    plus.setPaddleButton(0, false);
    plus.setGamePortConnected(true);
    assert(!up(plus, kBtn0));

    Memory iie;
    iie.setIIEMode(true);
    iie.setGamePortConnected(false);
    assert(!up(iie, kBtn0) && !up(iie, kBtn1) && "//e: pull-downs on the board");
    iie.setOpenAppleKey(true);
    assert(up(iie, kBtn0) && "the Apple keys still work");
    iie.setOpenAppleKey(false);
    assert(up(iie, kPdl0) && "//e empty port: paddle reads 255");
    std::printf("[ OK ] empty game port: ][+ PB0/PB1 high, paddles 255\n");
}

// ── 2c. The host mouse as paddles (INPUT-003, PaddleMouse.h) ──────────────
void testPaddleMouse()
{
    using pom2::paddlemouse::axisToPaddle;
    // Edge to edge over a 100..500 px screen.
    assert(axisToPaddle(100, 100, 500, 1.0) == 0);
    assert(axisToPaddle(300, 100, 500, 1.0) == 128);
    assert(axisToPaddle(500, 100, 500, 1.0) == 255);
    // The edges are the knob's stops: off-screen holds, never wraps.
    assert(axisToPaddle(-50, 100, 500, 1.0) == 0);
    assert(axisToPaddle(900, 100, 500, 1.0) == 255);
    // Range 0.5: the middle half sweeps it all, twice as fast.
    assert(axisToPaddle(200, 100, 500, 0.5) == 0);
    assert(axisToPaddle(400, 100, 500, 0.5) == 255);
    assert(axisToPaddle(250, 100, 500, 0.5) == 64);
    // A range below the floor is clamped; a degenerate screen is centred.
    assert(axisToPaddle(300, 100, 500, 0.0) == 128);
    assert(axisToPaddle(300, 100, 100, 1.0) == 128);
    std::printf("[ OK ] mouse → paddles: absolute, clamped at the screen edges\n");
}

// ── 3. Paddle RC discharge timing and the $C070 re-arm ────────────────────
void testPaddleTiming()
{
    Memory mem;

    // Re-arm the timer at the current cycle, then set a mid value.
    mem.setPaddle(0, 100);
    (void)mem.memRead(kPdlRst);          // paddleLatchCycle = now

    // Immediately after the reset, bit 7 is high (timer still charging).
    assert((mem.memRead(kPdl0) & 0x80) != 0 &&
           "a freshly reset paddle must read high");

    // Just before the threshold (100 * 11 = 1100 cycles) it stays high…
    mem.advanceCycles(1000);
    assert((mem.memRead(kPdl0) & 0x80) != 0);

    // …and past it, bit 7 falls.
    mem.advanceCycles(200);              // now 1200 > 1100
    assert((mem.memRead(kPdl0) & 0x80) == 0 &&
           "the paddle must discharge once elapsed exceeds value * 11");

    // $C070 re-arms it: high again from the new latch point.
    (void)mem.memRead(kPdlRst);
    assert((mem.memRead(kPdl0) & 0x80) != 0 &&
           "$C070 must restart the discharge timer");

    // A larger value discharges later — the reading is monotone in value.
    // (Let the running count expire first: see the lockout below.)
    mem.advanceCycles(1200);
    mem.setPaddle(0, 255);
    (void)mem.memRead(kPdlRst);
    mem.advanceCycles(1200);             // past a 100-paddle, not a 255 one
    assert((mem.memRead(kPdl0) & 0x80) != 0 &&
           "a 255 paddle must still read high at 1200 cycles");

    std::printf("[ OK ] paddle RC discharge timing + $C070 re-arm\n");
}

// ── 4. The 558 one-shots are not retriggered while they run ──────────────
// MAME apple2e.cpp:2053-2056: each axis reloads on $C070 only if its count
// has run out. PDL(0) then PDL(1) at once: the second strobe lands while
// paddle 1 is still counting from the first, so paddle 1 times out against
// the FIRST strobe — the hardware's "PDL(1) after PDL(0) reads low".
void testPaddleNoRetrigger()
{
    Memory mem;
    mem.advanceCycles(5000);
    mem.setPaddle(0, 10);                // 110 cycles
    mem.setPaddle(1, 200);               // 2200 cycles
    (void)mem.memRead(kPdlRst);          // both triggered at T
    mem.advanceCycles(150);              // paddle 0 done, paddle 1 running
    (void)mem.memRead(kPdlRst);          // re-triggers paddle 0 only
    assert((mem.memRead(kPdl0) & 0x80) != 0 &&
           "an expired axis restarts on the strobe");
    mem.advanceCycles(2200 - 150 + 10);  // T + 2210
    assert((mem.memRead(kPdl0 + 1) & 0x80) == 0 &&
           "paddle 1 times out against the first strobe, not the second");

    // A snapshot round-trip carries every axis's timer.
    mem.setPaddle(2, 255);
    (void)mem.memRead(kPdlRst);          // all four expired: all restart
    mem.advanceCycles(100);
    (void)mem.memRead(kPdlRst);          // a no-op: everything still runs
    std::vector<uint8_t> blob;
    mem.appendSnapshotState(blob);
    Memory back;
    back.setPaddle(2, 0);
    back.setCycleCounter(mem.getCycleCounter());
    assert(back.loadSnapshotState(blob.data(), blob.size()));
    back.advanceCycles(2700);            // 2800 after the restart
    assert((back.memRead(kPdl0 + 2) & 0x80) != 0 &&
           "the armed count (255) travels with the snapshot");
    back.advanceCycles(100);
    assert((back.memRead(kPdl0 + 2) & 0x80) == 0);
    std::printf("[ OK ] 558 one-shots are not retriggered while counting\n");
}

}  // namespace

int main()
{
    testKeyboardLatch();
    testButtons();
    testShiftKeyMod();
    testIiShiftWirings();
    testEmptyGamePort();
    testPaddleMouse();
    testPaddleTiming();
    testPaddleNoRetrigger();
    std::printf("input_io_smoke_test: OK\n");
    return 0;
}
