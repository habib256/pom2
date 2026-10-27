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
//   5. Snapshot / rewind: mode and the mid-command byte cursor round-trip,
//      the restored VBL re-phases to the beam, and a card restored from a
//      state where the guest had interrupts OFF stops interrupting.
//   6. MODE_INT_VBL lands on the beam's VBL edge (scanline 192), NTSC and
//      PAL, whatever the beam phase of the reset or restore.
//
// Note what test_vbl_pacing_follows_set_cycles pins on purpose: mode $08
// (MODE_INT_VBL with MOUSE_ON clear) DOES raise the VBL interrupt. That is
// AppleWin's OnMouseEvent verbatim — its `else byState &= STAT_INT_VBL`
// branch keeps the VBL bit rather than dropping it — and this port follows
// it deliberately.

#include "MouseCardAppleWin.h"
#include "M6502.h"
#include "Memory.h"
#include "ResourcePaths.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include "TestTempPath.h"

namespace {

std::string writeTempBlob(const std::vector<uint8_t>& bytes,
                          const std::string& nameSuffix)
{
    const std::string path = pom2test::tempPath("pom2_mouseaw_test_" + nameSuffix);
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
    assert(!card.loadRom(pom2test::tempPath("pom2_does_not_exist_slot_aw.bin")));
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

// MODE_INT_VBL pacing follows the profile-plumbed frame length
// (setVblCycles): default 17030 (262 x 65, NTSC); PAL profiles pass 20280
// (312 x 65). Regression: the period was a hard-wired NTSC constant, so on
// PAL profiles the VBL interrupt drifted against the 50 Hz frame.
void test_vbl_pacing_follows_set_cycles()
{
    std::vector<uint8_t> rom(0x800, 0x00);
    const auto slotPath = writeTempBlob(rom, "vbl_slot.bin");

    auto runCase = [&](bool usePal) {
        Memory mem;
        if (usePal) mem.setVideoStandard(VideoStandard::PAL);
        auto card = std::make_unique<MouseCardAppleWin>(4);
        assert(card->loadRom(slotPath));
        MouseCardAppleWin* raw = card.get();
        mem.slotBus().plug(4, std::move(card));
        raw->onReset();
        assert(raw->vblCycles() == 17030);          // NTSC default
        const int period = usePal ? 20280 : 17030;
        if (usePal) raw->setVblCycles(period);
        assert(raw->vblCycles() == period);

        const uint16_t devBase = 0xC0C0;            // slot 4
        primePiaForOutput(mem, devBase);
        // On PAL the card's default is 60 Hz on its own timer (TN Mouse #2);
        // the beam's edge needs TIMEDATA 50 Hz ($91) and an INIT ($50).
        if (usePal) {
            pulseCommand(mem, devBase, 0x91);
            pulseCommand(mem, devBase, 0x50);
            for (int k = 0; k < 2; ++k) {           // read INIT's two reply bytes
                mem.memWrite(devBase + 2, 0x00);    // PB4 1→0: read strobe
                mem.memWrite(devBase + 2, 0x10);
            }
        }
        // MOUSE_SET ($0n) with MODE_INT_VBL (bit 3) in the low nibble.
        pulseCommand(mem, devBase, 0x08);

        // The edge is line 192 of the beam's frame: one cycle short of it,
        // no VBL IRQ; crossing it raises the interrupt.
        mem.advanceCycles(1);                       // card phases itself
        const uint64_t edge = 192 * 65;
        mem.advanceCycles(static_cast<int>(edge - mem.getCycleCounter() - 1));
        assert(!raw->slotIrqAsserted());
        mem.advanceCycles(1);
        assert(raw->slotIrqAsserted());
    };
    runCase(/*usePal=*/false);
    runCase(/*usePal=*/true);

    // Bogus values are ignored (period must stay positive).
    MouseCardAppleWin guard(4);
    guard.setVblCycles(0);
    guard.setVblCycles(-5);
    assert(guard.vblCycles() == 17030);

    std::remove(slotPath.c_str());
}

// MODE_INT_VBL fires at the BEAM's VBL edge, not a frame after the card's
// reset. Bug hunt 2026-09-29: the pacer counted from onReset, so the first
// interrupt landed on scanlines 188 / 234 / 65 / 157 for four reset phases
// and every Ctrl-Reset moved the A2DeskTop cursor update to a new line.
// Resets the card at several beam phases (and restores a snapshot onto a
// clock moved to yet another phase) on NTSC and on PAL after TIMEDATA 50 Hz,
// and requires the first
// VBL interrupt on scanline 192 +-1 -- cross-checked against Memory's own
// VBLBAR at $C019.
void test_vbl_irq_locked_to_beam()
{
    std::vector<uint8_t> rom(0x800, 0x00);
    const auto slotPath = writeTempBlob(rom, "vbl_beam_slot.bin");
    const uint16_t devBase = 0xC0C0;     // slot 4

    auto firstIrqLine = [&](Memory& mem, MouseCardAppleWin* raw,
                            uint64_t frame) -> int {
        // 4-cycle steps: instruction-sized, far inside the +-1 line bound.
        for (uint64_t i = 0; i < 2 * frame; i += 4) {
            mem.advanceCycles(4);
            if (raw->slotIrqAsserted()) {
                const uint64_t c = mem.getCycleCounter();
                // Memory agrees the beam is in VBL ($C019 bit 7 low).
                assert((mem.memRead(0xC019) & 0x80) == 0);
                return static_cast<int>((c % frame) / 65);
            }
        }
        return -1;
    };

    for (const bool pal : { false, true }) {
        const uint64_t lines = pal ? 312 : 262;
        const uint64_t frame = lines * 65;
        for (const uint64_t phase : { uint64_t{0}, uint64_t{3000}, uint64_t{9000},
                                      uint64_t{12479}, uint64_t{12480},
                                      uint64_t{15000}, frame - 1 }) {
            Memory mem;
            mem.setIIEMode(true);
            if (pal) mem.setVideoStandard(VideoStandard::PAL);
            auto card = std::make_unique<MouseCardAppleWin>(4);
            assert(card->loadRom(slotPath));
            MouseCardAppleWin* raw = card.get();
            raw->setVblCycles(static_cast<int>(frame));
            mem.slotBus().plug(4, std::move(card));

            // Somewhere mid-session, then Ctrl-Reset at this beam phase.
            mem.setCycleCounter(frame * 57 + phase);
            mem.slotBus().reset();
            primePiaForOutput(mem, devBase);
            // A PAL program asks for 50 Hz (TN Mouse #2); left at the
            // default 60 Hz the card runs on its own timer, off the beam.
            if (pal) {
                pulseCommand(mem, devBase, 0x91);   // MOUSE_TIME: 50 Hz
                pulseCommand(mem, devBase, 0x50);   // MOUSE_INIT
                for (int k = 0; k < 2; ++k) {       // its two reply bytes
                    mem.memWrite(devBase + 2, 0x00);
                    mem.memWrite(devBase + 2, 0x10);
                }
            }
            pulseCommand(mem, devBase, 0x09);   // MOUSE_SET: ON | INT_VBL

            const int line = firstIrqLine(mem, raw, frame);
            if (line < 191 || line > 193) {
                std::fprintf(stderr,
                    "%s reset at phase %llu: first VBL IRQ on line %d, "
                    "want 192\n", pal ? "PAL" : "NTSC",
                    static_cast<unsigned long long>(phase), line);
            }
            assert(line >= 191 && line <= 193);

            // Service + read the interrupt, snapshot, move the clock to
            // another beam phase (a restore carries its own CPU counter),
            // restore: re-phased to the beam, no spurious interrupt out of
            // the clock jump.
            pulseCommand(mem, devBase, 0x20);   // MOUSE_SERV: drops the line
            pulseCommand(mem, devBase, 0x10);   // MOUSE_READ: clears status
            assert(!raw->slotIrqAsserted());
            std::vector<uint8_t> blob;
            raw->appendSnapshotState(blob);
            mem.setCycleCounter(frame * 1000 + (phase + 7777) % frame);
            raw->loadSnapshotState(blob.data(), blob.size());
            mem.advanceCycles(1);
            assert(!raw->slotIrqAsserted());
            const int line2 = firstIrqLine(mem, raw, frame);
            if (line2 < 191 || line2 > 193) {
                std::fprintf(stderr,
                    "%s restore: first VBL IRQ on line %d, want 192\n",
                    pal ? "PAL" : "NTSC", line2);
            }
            assert(line2 >= 191 && line2 <= 193);
        }
    }

    std::remove(slotPath.c_str());
}

// Under a 4x accelerator the beam keeps the crystal's time (Memory's video
// clock), so the card's on-beam VBL must land on line 192 of the BEAM's
// frame — not every 17030 CPU cycles, which is four times a frame.
void test_vbl_irq_on_beam_under_accelerator()
{
    std::vector<uint8_t> rom(0x800, 0x00);
    const auto slotPath = writeTempBlob(rom, "vbl_accel_slot.bin");
    const uint16_t devBase = 0xC0C0;
    const uint64_t frame = 262 * 65;

    Memory mem;
    mem.setIIEMode(true);
    auto card = std::make_unique<MouseCardAppleWin>(4);
    assert(card->loadRom(slotPath));
    MouseCardAppleWin* raw = card.get();
    raw->setVblCycles(static_cast<int>(frame));
    mem.slotBus().plug(4, std::move(card));
    mem.setCycleCounter(frame * 57 + 3000);
    mem.slotBus().reset();
    primePiaForOutput(mem, devBase);
    pulseCommand(mem, devBase, 0x09);            // MOUSE_SET: ON | INT_VBL

    // The machine speeds up (EmulationController::applyAcceleratorClock
    // re-anchors Memory first, then tells the cards).
    mem.setVideoClockRatio(0.25);
    raw->setStandardClock(1022727.0);
    raw->setCpuClock(4.0 * 1022727.0);

    for (int irq = 0; irq < 3; ++irq) {
        int line = -1;
        for (uint64_t i = 0; i < 8 * frame; i += 4) {
            mem.advanceCycles(4);
            if (raw->slotIrqAsserted()) {
                line = static_cast<int>((mem.videoNow() % frame) / 65);
                assert((mem.memRead(0xC019) & 0x80) == 0);
                break;
            }
        }
        if (line < 191 || line > 193)
            std::fprintf(stderr, "4x: VBL IRQ %d on beam line %d, want 192\n", irq, line);
        assert(line >= 191 && line <= 193);
        pulseCommand(mem, devBase, 0x20);        // MOUSE_SERV
        pulseCommand(mem, devBase, 0x10);        // MOUSE_READ
        assert(!raw->slotIrqAsserted());
    }
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
    memA.advanceCycles(1000);               // before the first VBL edge
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

    // The restored card's next MODE_INT_VBL is the BEAM's VBL edge (line
    // 192 of whatever frame memB's clock is in), not a count carried in
    // the blob.
    memB.setCycleCounter(17030 * 3 + 100);
    memB.advanceCycles(1);
    const uint64_t edgeB = 17030 * 3 + 192 * 65;
    memB.advanceCycles(static_cast<int>(edgeB - memB.getCycleCounter() - 1));
    assert(!b->slotIrqAsserted());
    memB.advanceCycles(1);
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
    memD.advanceCycles(1);                  // card phases to the beam
    memD.advanceCycles(17030);
    assert(d->slotIrqAsserted());           // interrupting the guest

    d->loadSnapshotState(quiet.data(), quiet.size());
    if (d->slotIrqAsserted()) {
        std::fprintf(stderr,
            "restored mouse card still asserting IRQ after rewind\n");
    }
    assert(!d->slotIrqAsserted());
    assert(d->debugSnapshot().byMode == 0);
    for (int i = 0; i < 5; ++i) memD.advanceCycles(17030);
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

// ── Firmware-level checks: the real AppleMouse II EPROM on a 6502 ─────────
// The Apple II Technical Notes (Mouse #2, #3, #7) describe the firmware
// entries; these run them through 341-0270-C, as a program would.
struct FirmwareRig {
    Memory mem;
    M6502  cpu{&mem};
    MouseCardAppleWin* card = nullptr;
    bool ok = false;
    FirmwareRig()
    {
        const auto rom  = pom2::findResource("roms/apple2p.rom");
        const auto slot = pom2::findResource("roms/mouse_341-0270-c.bin");
        if (rom.empty() || slot.empty()) return;
        mem.setCpu(&cpu);
        if (!mem.loadAppleIIRom(rom.c_str())) return;
        auto owned = std::make_unique<MouseCardAppleWin>(4);
        if (!owned->loadRom(slot)) return;
        card = owned.get();
        mem.slotBus().plug(4, std::move(owned));
        cpu.setCpuMode(M6502::CpuMode::NMOS);
        cpu.hardReset(); mem.slotBus().reset();
        ok = true;
    }
    // SEI; LDA #a; LDX #$C4; LDY #$40; JSR $C4(entry); JMP * — as TN Mouse
    // #7's caller does.
    void call(uint8_t off, uint8_t a)
    {
        const uint8_t e = mem.memRead(static_cast<uint16_t>(0xC400 + off));
        const uint8_t prog[] = { 0x78, 0xA9, a, 0xA2, 0xC4, 0xA0, 0x40,
                                 0x20, e, 0xC4, 0x4C, 0x0A, 0x03 };
        for (unsigned i = 0; i < sizeof prog; ++i)
            mem.memWrite(static_cast<uint16_t>(0x300 + i), prog[i]);
        cpu.setProgramCounter(0x300);
        for (int g = 0; g < 3'000'000 && cpu.getProgramCounter() != 0x30A; ++g) cpu.run(1);
        assert(cpu.getProgramCounter() == 0x30A && "a Mouse Card entry never returned");
    }
    void clamp(int axis, int lo, int hi)
    {
        mem.memWrite(0x478, static_cast<uint8_t>(lo));
        mem.memWrite(0x4F8, static_cast<uint8_t>(hi));
        mem.memWrite(0x578, static_cast<uint8_t>(lo >> 8));
        mem.memWrite(0x5F8, static_cast<uint8_t>(hi >> 8));
        call(0x17, static_cast<uint8_t>(axis));          // CLAMPMOUSE
    }
    void idle(long cycles)
    {
        const uint8_t loop[] = { 0x4C, 0x00, 0x03 };    // JMP $0300
        for (unsigned i = 0; i < sizeof loop; ++i)
            mem.memWrite(static_cast<uint16_t>(0x300 + i), loop[i]);
        cpu.setProgramCounter(0x300);
        for (long spent = 0; spent < cycles;) spent += cpu.run(64);
    }
};

// TN Mouse #7: GETCLAMP ($Cn1A), called eight times with $478 = $4E..$47,
// returns MaxYL, MaxXL, MaxYH, MaxXH, MinYL, MinXL, MinYH, MinXH in $578.
// The firmware asks the MCU with $F0 + a two-byte address; the HLE had no
// such command, took the address $4E for MOUSE_POS and moved the pointer.
// Expected bytes = what the LLE card returns with the real 68705.
void test_getclamp_reads_the_window_and_leaves_the_pointer()
{
    FirmwareRig r;
    if (!r.ok) { std::printf("  skip: GETCLAMP needs the ][+ and Mouse Card ROMs\n"); return; }
    r.call(0x19, 0);                                    // INITMOUSE
    r.clamp(0, 100, 500);
    r.clamp(1, 20, 300);
    // POSMOUSE takes the slot's own screen holes ($0478+n X lo, $04F8+n
    // Y lo, $0578+n X hi, $05F8+n Y hi); CLAMPMOUSE above takes the bare
    // $478/$4F8/$578/$5F8.
    r.mem.memWrite(0x478 + 4, 250); r.mem.memWrite(0x578 + 4, 0);  // X = 250
    r.mem.memWrite(0x4F8 + 4, 150); r.mem.memWrite(0x5F8 + 4, 0);  // Y = 150
    r.call(0x16, 0);                                    // POSMOUSE
    {
        const auto s0 = r.card->debugSnapshot();
        assert(s0.iX == 250 && s0.iY == 150 && "POSMOUSE placed the pointer");
    }
    const uint8_t expect[8] = { 0x00, 0x00, 0x64, 0x14, 0x01, 0x01, 0xF4, 0x2C };
    for (int k = 7; k >= 0; --k) {
        r.mem.memWrite(0x478, static_cast<uint8_t>(0x47 + k));
        r.mem.memWrite(0x4F8, 0x00);
        r.call(0x1A, 0x00);
        const uint8_t got = r.mem.memRead(0x578);
        if (got != expect[k])
            std::fprintf(stderr, "GETCLAMP $%02X: got $%02X, want $%02X\n", 0x47 + k, got, expect[k]);
        assert(got == expect[k]);
    }
    const auto s = r.card->debugSnapshot();
    assert(s.iX == 250 && s.iY == 150 && "GETCLAMP must not move the pointer");
}

// TN Mouse #2: TIMEDATA ($Cn1C), A bit 0 = 50 Hz, effective at the next
// INITMOUSE, and an INITMOUSE without one goes back to the default 60 Hz
// (timeDataHz() == 0), which is not the frame rate on a PAL machine.
void test_timedata_sets_the_vbl_rate()
{
    FirmwareRig r;
    if (!r.ok) { std::printf("  skip: TIMEDATA needs the ][+ and Mouse Card ROMs\n"); return; }
    r.card->setCpuClock(1022727.0);
    r.call(0x19, 0);
    assert(r.card->timeDataHz() == 0);
    r.call(0x1C, 0x01);                                 // TIMEDATA 50 Hz
    assert(r.card->timeDataHz() == 0 && "not before INITMOUSE");
    r.call(0x19, 0);
    assert(r.card->timeDataHz() == 50);

    // One second of VBL interrupts (SETMOUSE $08), each acknowledged by
    // SERVEMOUSE as a handler would.
    auto countIrqs = [&]() {
        r.call(0x12, 0x08);                             // SETMOUSE: VBL int only
        int n = 0;
        // One second on the machine's own counter: SERVEMOUSE's cycles count.
        const uint64_t end = r.mem.getCycleCounter() + 1'022'727;
        while (r.mem.getCycleCounter() < end) {
            r.idle(500);
            if (r.card->slotIrqAsserted()) { ++n; r.call(0x13, 0); }   // SERVEMOUSE
        }
        return n;
    };
    const int at50 = countIrqs();
    r.call(0x1C, 0x00);                                 // TIMEDATA 60 Hz
    r.call(0x19, 0);
    assert(r.card->timeDataHz() == 60);
    const int at60 = countIrqs();
    std::printf("  TIMEDATA: %d VBL interrupts/s at 50 Hz, %d at 60 Hz\n", at50, at60);
    assert(at50 >= 49 && at50 <= 51);
    assert(at60 >= 59 && at60 <= 61);
    r.call(0x19, 0);                                    // INIT without TIMEDATA
    assert(r.card->timeDataHz() == 0);

    // The default is 60 Hz on a PAL machine too: the card does not see the
    // 50 Hz frame, a program has to ask for it.
    r.mem.setVideoStandard(VideoStandard::PAL);         // the machine's beam…
    r.card->setVblCycles(20280);                        // …and the card's: 312 × 65
    r.call(0x19, 0);
    const int palDefault = countIrqs();
    r.call(0x1C, 0x01);                                 // TIMEDATA 50 Hz
    r.call(0x19, 0);
    const int palFifty = countIrqs();
    std::printf("  PAL: %d VBL interrupts/s by default, %d after TIMEDATA 50 Hz\n",
                palDefault, palFifty);
    assert(palDefault >= 59 && palDefault <= 61);
    // On the beam now: 1 022 727 / 20280 = 50.4 frames in the rig's second.
    assert(palFifty >= 49 && palFifty <= 52);

    // The card's own timer is the 68705's crystal: an accelerator switching
    // the CPU between 1 and 4 MHz (a Zip's slow windows, every few ms) must
    // not move it. Each clock change used to re-phase the edge, so a clock
    // that changed more often than once a period starved the interrupt.
    r.call(0x1C, 0x00);                                 // back to 60 Hz…
    r.call(0x19, 0);                                    // …free-running on PAL
    r.call(0x12, 0x08);
    double seconds = 0.0, hz = 1022727.0;
    int n = 0;
    long sinceSwitch = 0;
    while (seconds < 1.0) {
        const uint64_t before = r.mem.getCycleCounter();
        r.idle(500);
        const uint64_t spent = r.mem.getCycleCounter() - before;
        seconds += static_cast<double>(spent) / hz;
        sinceSwitch += static_cast<long>(spent);
        if (r.card->slotIrqAsserted()) { ++n; r.call(0x13, 0); }
        if (sinceSwitch >= 3000) {
            sinceSwitch = 0;
            hz = hz < 2e6 ? 4090908.0 : 1022727.0;
            r.card->setCpuClock(hz);
        }
    }
    std::printf("  PAL, accelerator toggling: %d VBL interrupts in one real second\n", n);
    assert(n >= 58 && n <= 63);   // the rig's ~61 at a steady clock, ±2
}

// TN Mouse #3: with the mouse off "any mouse motion is ignored" and the
// position is kept until it is turned back on — the pointer must not jump
// by the distance travelled meanwhile.
void test_mouse_off_ignores_motion()
{
    FirmwareRig r;
    if (!r.ok) { std::printf("  skip: mouse-off needs the ][+ and Mouse Card ROMs\n"); return; }
    r.call(0x19, 0);
    r.call(0x12, 0x01);                                 // SETMOUSE: on
    r.card->setHostMouse(0, 0, false);
    r.idle(200);
    r.card->setHostMouse(10, 0, false);
    r.idle(200);
    const int on = r.card->debugSnapshot().iX;
    assert(on > 0 && "motion is tracked with the mouse on");
    r.call(0x12, 0x00);                                 // SETMOUSE: off
    r.card->setHostMouse(60, 0, false);
    r.idle(200);
    assert(r.card->debugSnapshot().iX == on && "motion ignored with the mouse off");
    r.call(0x12, 0x01);                                 // back on
    r.idle(200);
    assert(r.card->debugSnapshot().iX == on && "no jump when it is turned back on");
}

// Two host updates before one poll, 200 units in all: the delta taken from
// the 8-bit counter at poll time wrapped past 128, and +200 moved the
// pointer 56 units BACKWARDS (bug hunt 2026-10-23).
void test_fast_motion_does_not_wrap()
{
    FirmwareRig r;
    if (!r.ok) { std::printf("  skip: fast motion needs the ][+ and Mouse Card ROMs\n"); return; }
    r.call(0x19, 0);
    r.call(0x12, 0x01);                                 // SETMOUSE: on
    r.card->setHostMouse(0, 0, false);
    r.idle(200);
    const int x0 = r.card->debugSnapshot().iX;
    r.card->setHostMouse(100, 0, false);                // two updates, each
    r.card->setHostMouse(200, 0, false);                // within ±127
    r.idle(200);
    assert(r.card->debugSnapshot().iX - x0 == 200 && "fast motion wrapped");
}

int main()
{
    test_missing_rom_refuses_to_load();
    test_size_mismatch_refuses_to_load();
    test_slot_rom_bank_select();
    test_command_handshake_reaches_oncommand();
    test_home_goes_to_clamp_origin();
    test_snapshot_round_trip_and_irq_rewind();
    test_vbl_pacing_follows_set_cycles();
    test_vbl_irq_locked_to_beam();
    test_vbl_irq_on_beam_under_accelerator();
    test_getclamp_reads_the_window_and_leaves_the_pointer();
    test_timedata_sets_the_vbl_rate();
    test_mouse_off_ignores_motion();
    test_fast_motion_does_not_wrap();

    std::printf("OK mouse_card_applewin_smoke\n");
    return 0;
}
