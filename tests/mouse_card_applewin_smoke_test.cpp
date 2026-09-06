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

// MouseCardAppleWin smoke test — pins the AppleWin HLE port (the
// non-MAME variant that synthesises the MCU side instead of running the
// 341-0269 mask ROM). Checked here:
//
//   1. Construction with a synthetic 2 KB slot ROM works; loadRom rejects
//      missing files and wrong sizes.
//   2. Slot ROM bank-select via PIA Port B bits 8..10 — matches
//      AppleWin SetSlotRom: `offset = (m_by6821B << 7) & 0x0700`.
//   3. The BIT5 (PB5) write-strobe handshake from On6821_B reaches
//      OnCommand: pulse a MOUSE_HOME byte through the PIA and observe
//      the slot IRQ stays low + the card is alive afterwards (no state
//      corruption). Also pulse MOUSE_INIT and verify the firmware ends
//      up reading 0xFF back on Port A (the canned reply for MOUSE_INIT).
//   4. MOUSE_HOME lands on the clamp window's upper-left corner, which is
//      what Apple's HOMEMOUSE entry promises — not the hard (0,0) AppleWin
//      uses (the two agree only at the power-on 0..1023 window).
//   5. Snapshot / rewind: mode, the mid-command byte cursor and the VBL
//      pacer round-trip, and a card restored from a state where the guest
//      had interrupts OFF stops interrupting.
//
// Note what test_vbl_pacing_follows_set_cycles pins on purpose: mode $08
// (MODE_INT_VBL with MOUSE_ON clear) DOES raise the VBL interrupt. That is
// AppleWin's OnMouseEvent verbatim — its `else byState &= STAT_INT_VBL`
// branch keeps the VBL bit rather than dropping it — and this port follows
// it deliberately.

#include "MouseCardAppleWin.h"
#include "Memory.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::string writeTempBlob(const std::vector<uint8_t>& bytes,
                          const std::string& nameSuffix)
{
    const std::string path = "/tmp/pom2_mouseaw_test_" + nameSuffix;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return path;
}

std::vector<uint8_t> buildBankSignatureRom()
{
    std::vector<uint8_t> rom(0x800, 0);
    for (int bank = 0; bank < 8; ++bank) {
        std::fill(rom.begin() + bank * 0x100,
                  rom.begin() + (bank + 1) * 0x100,
                  static_cast<uint8_t>(0xC0 + bank));
    }
    return rom;
}

void test_missing_rom_refuses_to_load()
{
    MouseCardAppleWin card(4);
    assert(!card.loadRom("/tmp/pom2_does_not_exist_slot_aw.bin"));
    assert(!card.isReady());
}

void test_size_mismatch_refuses_to_load()
{
    MouseCardAppleWin card(4);
    std::vector<uint8_t> half(0x400, 0xAA);
    const auto p = writeTempBlob(half, "half_slot.bin");
    assert(!card.loadRom(p));
    assert(!card.isReady());
    std::remove(p.c_str());
}

// Helper: program the PIA to "all data + DDR=0xFF" on both ports via the
// $C0(8+s)X device-select window. The PIA register select is `addr & 3`.
void primePiaForOutput(Memory& mem, uint16_t devBase)
{
    // CRA = 0 → access DDR at offset 0; DDRA = 0xFF; CRA = 0x04 → data.
    mem.memWrite(devBase + 1, 0x00);
    mem.memWrite(devBase + 0, 0xFF);
    mem.memWrite(devBase + 1, 0x04);
    // Same for B.
    mem.memWrite(devBase + 3, 0x00);
    mem.memWrite(devBase + 2, 0xFF);
    mem.memWrite(devBase + 3, 0x04);
}

void test_slot_rom_bank_select()
{
    const auto slotBytes = buildBankSignatureRom();
    const auto slotPath  = writeTempBlob(slotBytes, "bank_slot.bin");

    Memory mem;
    auto card = std::make_unique<MouseCardAppleWin>(4);
    assert(card->loadRom(slotPath));
    assert(card->isReady());
    MouseCardAppleWin* raw = card.get();
    mem.slotBus().plug(4, std::move(card));
    raw->onReset();

    const uint16_t devBase = 0xC0C0;     // slot 4
    primePiaForOutput(mem, devBase);

    // AppleWin On6821_B only reacts when bits in 0x3E change. After reset
    // by6821B = 0x40, so the first PRB write must include enough deltas
    // to clear the prior state. We toggle bank bits 0x0E (PB1..PB3) plus
    // PB5 / PB4 set to "idle" (= high) so the strobe edges don't fire.
    const struct { uint8_t prb; uint8_t expected; } cases[] = {
        { 0x30 | 0x00, 0xC0 },     // bank 0
        { 0x30 | 0x02, 0xC1 },     // bank 1
        { 0x30 | 0x04, 0xC2 },     // bank 2
        { 0x30 | 0x06, 0xC3 },     // bank 3
        { 0x30 | 0x08, 0xC4 },     // bank 4
        { 0x30 | 0x0A, 0xC5 },     // bank 5
        { 0x30 | 0x0C, 0xC6 },     // bank 6
        { 0x30 | 0x0E, 0xC7 },     // bank 7
    };
    for (const auto& c : cases) {
        mem.memWrite(devBase + 2, c.prb);
        const uint8_t got = mem.memRead(0xC400);
        if (got != c.expected) {
            std::fprintf(stderr,
                "bank PRB=$%02X: expected $%02X got $%02X\n",
                c.prb, c.expected, got);
        }
        assert(got == c.expected);
    }

    std::remove(slotPath.c_str());
}

// Drive one command byte through the BIT5 write-strobe handshake:
// "byte on PRA, then PB5 1→0 commits the byte". After the trailing
// edge, On6821_B feeds the byte into OnCommand.
void pulseCommand(Memory& mem, uint16_t devBase, uint8_t cmdByte)
{
    // PRA = command byte.
    mem.memWrite(devBase + 0, cmdByte);
    // PB5 high (with PB4=1 too so the read-strobe doesn't false-fire).
    mem.memWrite(devBase + 2, 0x30);
    // PB5 low — trailing edge consumes the byte.
    mem.memWrite(devBase + 2, 0x10);
}

void test_command_handshake_reaches_oncommand()
{
    // A blank slot ROM is fine — we're poking the HLE side directly.
    std::vector<uint8_t> rom(0x800, 0x00);
    const auto slotPath = writeTempBlob(rom, "blank_slot.bin");

    Memory mem;
    auto card = std::make_unique<MouseCardAppleWin>(4);
    assert(card->loadRom(slotPath));
    MouseCardAppleWin* raw = card.get();
    mem.slotBus().plug(4, std::move(card));
    raw->onReset();

    const uint16_t devBase = 0xC0C0;     // slot 4
    primePiaForOutput(mem, devBase);

    // ── MOUSE_HOME ($70): single-byte command. After it runs the card's
    //    internal position is (0,0); no IRQ should be asserted (mode off).
    pulseCommand(mem, devBase, 0x70);
    assert(!raw->slotIrqAsserted());

    // ── MOUSE_INIT ($50): three-byte command — OnCommand fires after the
    //    first byte and pre-loads byBuff[1] = 0xFF, which gets pushed
    //    onto Port A via pia.setPortAInput(byBuff[1]). Read it back.
    //    Real firmware would flip DDRA → 0 (input) before reading so the
    //    PIA returns the *input* latch, not the output latch. Do the
    //    same here.
    pulseCommand(mem, devBase, 0x50);
    mem.memWrite(devBase + 1, 0x00);     // CRA bit 2 = 0 → DDR access
    mem.memWrite(devBase + 0, 0x00);     // DDRA = all input
    mem.memWrite(devBase + 1, 0x04);     // back to data port
    const uint8_t pra = mem.memRead(devBase + 0);
    if (pra != 0xFF) {
        std::fprintf(stderr,
            "MOUSE_INIT: expected PRA=$FF after OnCommand, got $%02X\n", pra);
    }
    assert(pra == 0xFF);

    std::remove(slotPath.c_str());
}

// MODE_INT_VBL pacing follows the profile-plumbed cycles-per-frame
// (setVblCycles): default 17045 (NTSC 60 Hz); PAL profiles pass 20313
// (50 Hz). Regression: the period was a hard-wired NTSC constant, so on
// PAL profiles the VBL interrupt drifted against the 50 Hz frame.
void test_vbl_pacing_follows_set_cycles()
{
    std::vector<uint8_t> rom(0x800, 0x00);
    const auto slotPath = writeTempBlob(rom, "vbl_slot.bin");

    auto runCase = [&](bool usePal) {
        Memory mem;
        auto card = std::make_unique<MouseCardAppleWin>(4);
        assert(card->loadRom(slotPath));
        MouseCardAppleWin* raw = card.get();
        mem.slotBus().plug(4, std::move(card));
        raw->onReset();
        assert(raw->vblCycles() == 17045);          // NTSC default
        const int period = usePal ? 20313 : 17045;
        if (usePal) raw->setVblCycles(period);
        assert(raw->vblCycles() == period);

        const uint16_t devBase = 0xC0C0;            // slot 4
        primePiaForOutput(mem, devBase);
        // MOUSE_SET ($0n) with MODE_INT_VBL (bit 3) in the low nibble.
        pulseCommand(mem, devBase, 0x08);

        // One cycle short of a frame → no VBL IRQ yet.
        raw->advanceCycles(period - 1);
        assert(!raw->slotIrqAsserted());
        // Crossing the frame boundary raises the VBL interrupt.
        raw->advanceCycles(1);
        assert(raw->slotIrqAsserted());
    };
    runCase(/*usePal=*/false);
    runCase(/*usePal=*/true);

    // Bogus values are ignored (period must stay positive).
    MouseCardAppleWin guard(4);
    guard.setVblCycles(0);
    guard.setVblCycles(-5);
    assert(guard.vblCycles() == 17045);

    std::remove(slotPath.c_str());
}

// MOUSE_HOME ($70) homes to the UPPER-LEFT CORNER OF THE CLAMPING WINDOW,
// not to (0,0) — "sets the mouse position to the upper-left corner of the
// clamping window" is Apple's own wording for the HOMEMOUSE firmware entry
// this command backs. AppleWin hard-codes SetPositionAbs(0,0), which agrees
// only while the window is still the power-on 0..1023; a program that clamps
// to X 100..500 / Y 200..600 and then homes used to land at (0,0), outside
// its own window.
void test_home_goes_to_clamp_origin()
{
    std::vector<uint8_t> rom(0x800, 0x00);
    const auto slotPath = writeTempBlob(rom, "home_slot.bin");

    Memory mem;
    auto card = std::make_unique<MouseCardAppleWin>(4);
    assert(card->loadRom(slotPath));
    MouseCardAppleWin* raw = card.get();
    mem.slotBus().plug(4, std::move(card));
    raw->onReset();

    const uint16_t devBase = 0xC0C0;     // slot 4
    primePiaForOutput(mem, devBase);

    // MOUSE_CLAMP is a 5-byte command: cmd, then
    //   min = (byBuff[3] << 8) | byBuff[1],  max = (byBuff[4] << 8) | byBuff[2]
    // LSB of the command byte selects the axis (0 = X, 1 = Y).
    auto clamp = [&](uint8_t cmd, int lo, int hi) {
        pulseCommand(mem, devBase, cmd);
        pulseCommand(mem, devBase, static_cast<uint8_t>(lo & 0xFF));
        pulseCommand(mem, devBase, static_cast<uint8_t>(hi & 0xFF));
        pulseCommand(mem, devBase, static_cast<uint8_t>((lo >> 8) & 0xFF));
        pulseCommand(mem, devBase, static_cast<uint8_t>((hi >> 8) & 0xFF));
    };
    clamp(0x60, 100, 500);      // X window
    clamp(0x61, 200, 600);      // Y window
    {
        const auto s = raw->debugSnapshot();
        assert(s.iMinX == 100 && s.iMaxX == 500);
        assert(s.iMinY == 200 && s.iMaxY == 600);
    }

    // Park the cursor somewhere inside the window (MOUSE_POS, 5 bytes:
    // X lo/hi then Y lo/hi) so homing has something to undo.
    pulseCommand(mem, devBase, 0x40);
    pulseCommand(mem, devBase, 300 & 0xFF);
    pulseCommand(mem, devBase, (300 >> 8) & 0xFF);
    pulseCommand(mem, devBase, 400 & 0xFF);
    pulseCommand(mem, devBase, (400 >> 8) & 0xFF);
    {
        const auto s = raw->debugSnapshot();
        assert(s.iX == 300 && s.iY == 400);
    }

    pulseCommand(mem, devBase, 0x70);           // MOUSE_HOME
    const auto s = raw->debugSnapshot();
    if (s.iX != 100 || s.iY != 200) {
        std::fprintf(stderr,
            "MOUSE_HOME: expected the clamp origin (100,200), got (%d,%d)\n",
            s.iX, s.iY);
    }
    assert(s.iX == 100 && s.iY == 200);

    std::remove(slotPath.c_str());
}

// Snapshot / rewind. This card serialized NOTHING, and MachineSnapshot
// skips a card whose blob comes back empty — so no SLOTn section was ever
// written and a rewind left the LIVE card in place. The visible damage: a
// rewind past the guest's MOUSE_SET left byMode holding MODE_INT_VBL, so
// the card kept raising the slot IRQ at frame rate into a machine whose
// vector no longer pointed at a mouse handler, with only a MOUSE_SERV that
// would never arrive to release the line.
void test_snapshot_round_trip_and_irq_rewind()
{
    std::vector<uint8_t> rom(0x800, 0x00);
    const auto slotPath = writeTempBlob(rom, "snap_slot.bin");
    const uint16_t devBase = 0xC0C0;     // slot 4

    // Plug a card into `mem` and return the raw pointer (mem owns it).
    auto plug = [&](Memory& mem) {
        auto card = std::make_unique<MouseCardAppleWin>(4);
        assert(card->loadRom(slotPath));
        MouseCardAppleWin* raw = card.get();
        mem.slotBus().plug(4, std::move(card));
        raw->onReset();
        primePiaForOutput(mem, devBase);
        return raw;
    };

    // ── A: the recorded machine. Interrupts on, mid-MOUSE_CLAMP, and a
    //    partly-elapsed VBL period.
    Memory memA;
    MouseCardAppleWin* a = plug(memA);
    pulseCommand(memA, devBase, 0x09);      // MOUSE_SET: MOUSE_ON | INT_VBL
    pulseCommand(memA, devBase, 0x60);      // MOUSE_CLAMP: 5-byte command...
    pulseCommand(memA, devBase, 0x11);      // ...byte 2
    pulseCommand(memA, devBase, 0x22);      // ...byte 3 (cursor parked at 3)
    a->advanceCycles(1000);                 // VBL pacer partly elapsed
    assert(!a->slotIrqAsserted());
    {
        const auto s = a->debugSnapshot();
        assert(s.byMode == 0x09);
        assert(s.dataLen == 5 && s.buffPos == 3 && s.lastCmd == 0x60);
    }

    std::vector<uint8_t> blob;
    a->appendSnapshotState(blob);
    assert(!blob.empty());                  // else MachineSnapshot skips us

    // ── B: a fresh card is where a rewind lands. Restore must reproduce
    //    mode, the mid-command cursor and the VBL pacer exactly.
    Memory memB;
    MouseCardAppleWin* b = plug(memB);
    b->loadSnapshotState(blob.data(), blob.size());
    {
        const auto s = b->debugSnapshot();
        assert(s.byMode == 0x09);
        assert(s.dataLen == 5 && s.buffPos == 3 && s.lastCmd == 0x60);
    }
    std::vector<uint8_t> blob2;
    b->appendSnapshotState(blob2);
    assert(blob2 == blob);

    // The restored pacer still owes 17045 - 1000 cycles before the next
    // MODE_INT_VBL, not a full frame.
    b->advanceCycles(17045 - 1000 - 1);
    assert(!b->slotIrqAsserted());
    b->advanceCycles(1);
    assert(b->slotIrqAsserted());

    // ── C/D: rewind to before the guest enabled interrupts. The restored
    //    card must go quiet — this is the case that used to keep firing at
    //    60 Hz into a machine with no handler.
    Memory memC;
    MouseCardAppleWin* c = plug(memC);
    std::vector<uint8_t> quiet;
    c->appendSnapshotState(quiet);          // interrupts off, nothing pending

    Memory memD;
    MouseCardAppleWin* d = plug(memD);
    pulseCommand(memD, devBase, 0x09);
    d->advanceCycles(17045);
    assert(d->slotIrqAsserted());           // interrupting the guest

    d->loadSnapshotState(quiet.data(), quiet.size());
    if (d->slotIrqAsserted()) {
        std::fprintf(stderr,
            "restored mouse card still asserting IRQ after rewind\n");
    }
    assert(!d->slotIrqAsserted());
    assert(d->debugSnapshot().byMode == 0);
    for (int i = 0; i < 5; ++i) d->advanceCycles(17045);
    assert(!d->slotIrqAsserted());          // and stays quiet

    // ── A foreign blob must be ignored, not misparsed.
    Memory memE;
    MouseCardAppleWin* e = plug(memE);
    std::vector<uint8_t> before;
    e->appendSnapshotState(before);
    const std::vector<uint8_t> foreign(256, 0xAB);
    e->loadSnapshotState(foreign.data(), foreign.size());
    std::vector<uint8_t> after;
    e->appendSnapshotState(after);
    assert(after == before);

    std::remove(slotPath.c_str());
}

}  // namespace

int main()
{
    test_missing_rom_refuses_to_load();
    test_size_mismatch_refuses_to_load();
    test_slot_rom_bank_select();
    test_command_handshake_reaches_oncommand();
    test_home_goes_to_clamp_origin();
    test_snapshot_round_trip_and_irq_rewind();
    test_vbl_pacing_follows_set_cycles();

    std::printf("OK mouse_card_applewin_smoke\n");
    return 0;
}
