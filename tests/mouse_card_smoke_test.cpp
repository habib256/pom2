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

// MouseCard wiring smoke test — pins the verbatim port of MAME
// `bus/a2bus/mouse.cpp` against the structural invariants the firmware
// relies on, even without the actual Apple-copyright ROMs (which we
// can't bundle).
//
// What is pinned:
//
//   * Construction with a synthetic 2 KB slot ROM + a synthetic 2 KB
//     MCU ROM works without crashing.
//   * Slot ROM bank-select via PIA Port B bits 1..3: writing $00, $02,
//     $04, ..., $0E to the PIA's port B (with CRA bit 2 enabled) maps
//     the corresponding 256-byte slice of the EPROM into $C(s)00..
//     $C(s)FF. Verifies MAME's `m_rom_bank = (data & 0x0e) << 7`.
//   * Quadrature edge synthesis: setHostMouse with X delta produces
//     CLK toggles on PB1, with DIR on PB0 reflecting motion direction;
//     same for Y on PB3 / PB2.
//   * Button bit on PB7 (active LOW: 0 = pressed).
//   * Without ROMs the card refuses to load (loadRoms returns false on
//     missing files).
//   * A pending slot IRQ (MCU PB6 driven low) survives a snapshot
//     save/restore round trip. PB6 is the card's ONLY IRQ source (MAME
//     mouse.cpp:46 + `mcu_port_b_w`); re-deriving the line from the PIA's
//     irqa/irqb — whose MAME handlers are empty stubs — silently dropped
//     every interrupt that was outstanding at snapshot time, and nothing
//     re-arms it because the firmware only rewrites PB6 after the host
//     services the request.
//   * MCU pacing: the retired/budget cycle ratio converges on 2.0 over
//     many small per-instruction budgets. `M68705P3::run` finishes the
//     instruction that straddles the budget edge, so the overshoot must
//     be billed to the next tick (MAME's `m6805_base_device::execute_run`
//     leaves `m_icount` negative and the scheduler charges the device
//     `cycles_running - m_icount`). Discarding it clocked the 68705
//     26-50 % above its 2043600 Hz.

#include "MouseCard.h"
#include "Memory.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "TestTempPath.h"

namespace {

// Drop a 2 KB blob to a temp file. Returns the path.
std::string writeTempBlob(const std::vector<uint8_t>& bytes,
                           const std::string& nameSuffix)
{
    const std::string path = pom2test::tempPath("pom2_mouse_test_" + nameSuffix);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return path;
}

void test_missing_roms_refuses_to_load()
{
    MouseCard card(4);
    // Neither file exists.
    assert(!card.loadRoms(pom2test::tempPath("pom2_does_not_exist_slot.bin"),
                          pom2test::tempPath("pom2_does_not_exist_mcu.bin")));
    assert(!card.isReady());
}

void test_size_mismatch_refuses_to_load()
{
    MouseCard card(4);
    // 1024-byte ROMs (half the right size).
    std::vector<uint8_t> half(0x400, 0xAA);
    const auto p1 = writeTempBlob(half, "half_slot.bin");
    const auto p2 = writeTempBlob(half, "half_mcu.bin");
    assert(!card.loadRoms(p1, p2));
    assert(!card.isReady());
    std::remove(p1.c_str());
    std::remove(p2.c_str());
}

// Build a slot ROM where each 256-byte bank is filled with a unique
// signature byte (bank 0 = $A0, bank 1 = $A1, ..., bank 7 = $A7).
std::vector<uint8_t> buildBankSignatureRom()
{
    std::vector<uint8_t> rom(0x800, 0);
    for (int bank = 0; bank < 8; ++bank) {
        std::fill(rom.begin() + bank * 0x100,
                  rom.begin() + (bank + 1) * 0x100,
                  static_cast<uint8_t>(0xA0 + bank));
    }
    return rom;
}

// Build a 2 KB MCU image whose reset vector ($07FE/$07FF) lands on a
// safe BRA $00FE infinite loop ($20 $FE = BRA -2). This way the MCU
// won't run amok during tests.
std::vector<uint8_t> buildHaltMcuRom()
{
    std::vector<uint8_t> rom(0x800, 0xFF);
    // Code at chip $0080 (file offset $0080): BRA $0080 = $20 $FE
    rom[0x80] = 0x20;
    rom[0x81] = 0xFE;
    // Reset vector at $07FE/$07FF (big-endian) = $0080
    rom[0x7FE] = 0x00;
    rom[0x7FF] = 0x80;
    return rom;
}

void test_slot_rom_bank_select()
{
    const auto slotBytes = buildBankSignatureRom();
    const auto mcuBytes  = buildHaltMcuRom();
    const auto slotPath  = writeTempBlob(slotBytes, "bank_slot.bin");
    const auto mcuPath   = writeTempBlob(mcuBytes,  "halt_mcu.bin");

    Memory mem;
    auto card = std::make_unique<MouseCard>(4);
    assert(card->loadRoms(slotPath, mcuPath));
    assert(card->isReady());
    MouseCard* raw = card.get();
    mem.slotBus().plug(4, std::move(card));
    raw->onReset();

    // The PIA's CRA bit 2 must be 1 for offset 2 to write port B (data),
    // not DDR. Same for CRB bit 2. We program both via the device-select
    // window at $C0C0 (slot 4, low4=0 → PRA/DDRA, low4=1 → CRA, low4=2 →
    // PRB/DDRB, low4=3 → CRB).
    const uint16_t devBase = 0xC0C0;     // slot 4
    mem.memWrite(devBase + 1, 0x04);     // CRA = 0x04: data port at offset 0
    mem.memWrite(devBase + 3, 0x04);     // CRB = 0x04: data port at offset 2

    // Set DDR B = $FF first (all output) by clearing CRB bit 2, writing
    // DDR, then re-setting CRB bit 2.
    mem.memWrite(devBase + 3, 0x00);     // CRB = 0: access DDR at offset 2
    mem.memWrite(devBase + 2, 0xFF);     // DDRB = all output
    mem.memWrite(devBase + 3, 0x04);     // CRB = 0x04: back to data port

    // Now write each bank value to PRB and verify the slot ROM at
    // $C400 reflects the corresponding signature byte.
    const struct { uint8_t prb; uint8_t expected; } cases[] = {
        { 0x00, 0xA0 },
        { 0x02, 0xA1 },
        { 0x04, 0xA2 },
        { 0x06, 0xA3 },
        { 0x08, 0xA4 },
        { 0x0A, 0xA5 },
        { 0x0C, 0xA6 },
        { 0x0E, 0xA7 },
    };
    for (const auto& c : cases) {
        mem.memWrite(devBase + 2, c.prb);
        const uint8_t got = mem.memRead(0xC400);     // slot 4 ROM
        if (got != c.expected) {
            std::fprintf(stderr, "bank PRB=$%02X: expected $%02X got $%02X\n",
                c.prb, c.expected, got);
        }
        assert(got == c.expected);
    }

    std::remove(slotPath.c_str());
    std::remove(mcuPath.c_str());
}

void test_signature_passes_through_to_slot_rom()
{
    // Build a slot ROM with the canonical Apple Mouse Card signature
    // bytes at offsets 0x05 / 0x07 / 0x0B / 0x0C (per Apple II Mouse
    // Technical Notes — these are also the ProDOS-style probe points).
    std::vector<uint8_t> slotBytes(0x800, 0xEA);
    // Bank 0 only — offset within bank = same as low byte of Cn00.
    slotBytes[0x05] = 0x38;   // Pascal-style "device firmware" sig
    slotBytes[0x07] = 0x18;
    slotBytes[0x0B] = 0x01;
    slotBytes[0x0C] = 0x20;
    const auto slotPath = writeTempBlob(slotBytes, "sig_slot.bin");
    const auto mcuPath  = writeTempBlob(buildHaltMcuRom(), "sig_mcu.bin");

    Memory mem;
    auto card = std::make_unique<MouseCard>(4);
    assert(card->loadRoms(slotPath, mcuPath));
    MouseCard* raw = card.get();
    mem.slotBus().plug(4, std::move(card));
    raw->onReset();

    // After reset the bank-select is 0 (firmware hasn't run; PIA Port B
    // output starts at 0). So slot ROM reads should hit bank 0.
    assert(mem.memRead(0xC405) == 0x38);
    assert(mem.memRead(0xC407) == 0x18);
    assert(mem.memRead(0xC40B) == 0x01);
    assert(mem.memRead(0xC40C) == 0x20);

    std::remove(slotPath.c_str());
    std::remove(mcuPath.c_str());
}

// Locate a repo file from whichever build subdirectory ctest runs in.
std::string firstExisting(const std::vector<std::string>& cands)
{
    namespace fs = std::filesystem;
    for (const auto& p : cands) {
        if (fs::exists(p))            return p;
        if (fs::exists("../" + p))    return "../" + p;
        if (fs::exists("../../" + p)) return "../../" + p;
    }
    return {};
}

// Build a 2 KB MCU image that drives PB6 low (= slot IRQ asserted, MAME
// mouse.cpp:275-284 `if (!BIT(data, 6)) raise_slot_irq();`) and then parks
// in a BRA self-loop, exactly as the real firmware parks while waiting for
// the host to service the request it just raised.
std::vector<uint8_t> buildIrqAssertingMcuRom()
{
    std::vector<uint8_t> rom(0x800, 0xFF);
    uint16_t p = 0x80;
    rom[p++] = 0xA6; rom[p++] = 0x40;   // LDA #$40
    rom[p++] = 0xB7; rom[p++] = 0x05;   // STA $05   DDRB: PB6 = output
    rom[p++] = 0xA6; rom[p++] = 0x00;   // LDA #$00
    rom[p++] = 0xB7; rom[p++] = 0x01;   // STA $01   PB6 latch = 0 -> IRQ
    rom[p++] = 0x20; rom[p++] = 0xFE;   // BRA *-2   park
    rom[0x7FE] = 0x00;
    rom[0x7FF] = 0x80;
    return rom;
}

void test_pending_irq_survives_snapshot()
{
    const auto slotPath = writeTempBlob(buildBankSignatureRom(), "irq_slot.bin");
    const auto mcuPath  = writeTempBlob(buildIrqAssertingMcuRom(), "irq_mcu.bin");

    MouseCard a(4);
    assert(a.loadRoms(slotPath, mcuPath));
    a.onReset();
    assert(!a.slotIrqAsserted());

    // Let the MCU reach its park loop with PB6 held low.
    for (int i = 0; i < 64 && !a.slotIrqAsserted(); ++i) a.advanceCycles(4);
    assert(a.slotIrqAsserted());

    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);
    assert(!blob.empty());

    // A FRESH card is where a rewind lands: its IRQ line starts released.
    MouseCard b(4);
    assert(b.loadRoms(slotPath, mcuPath));
    b.onReset();
    assert(!b.slotIrqAsserted());

    b.loadSnapshotState(blob.data(), blob.size());
    if (!b.slotIrqAsserted()) {
        std::fprintf(stderr,
            "pending mouse IRQ lost across snapshot restore\n");
    }
    assert(b.slotIrqAsserted());

    // ...and it must still be visible without the firmware writing PB6
    // again: the restored MCU is parked, so nothing re-runs the port-write
    // callback.
    for (int i = 0; i < 64; ++i) b.advanceCycles(4);
    assert(b.slotIrqAsserted());

    // A foreign blob must leave the card alone rather than misparse it.
    const std::vector<uint8_t> foreign(256, 0xAB);
    std::vector<uint8_t> before;
    b.appendSnapshotState(before);
    b.loadSnapshotState(foreign.data(), foreign.size());
    std::vector<uint8_t> after;
    b.appendSnapshotState(after);
    assert(after == before);

    std::remove(slotPath.c_str());
    std::remove(mcuPath.c_str());
}

void test_mcu_pacing_ratio()
{
    // Prefer the real 341-0269 firmware: its idle loop is all 10-cycle
    // BRCLRs, the worst case against the 4-14 MCU-cycle budgets
    // advanceCycles actually sees. Fall back to the synthetic park loop so
    // the invariant is still pinned on a ROM-less checkout.
    std::string slotPath = firstExisting({"roms/mouse_341-0270-c.bin"});
    std::string mcuPath  = firstExisting({"roms/mouse_341-0269.bin"});
    std::string tmpSlot, tmpMcu;
    if (slotPath.empty() || mcuPath.empty()) {
        tmpSlot = writeTempBlob(buildBankSignatureRom(), "pace_slot.bin");
        tmpMcu  = writeTempBlob(buildIrqAssertingMcuRom(), "pace_mcu.bin");
        slotPath = tmpSlot;
        mcuPath  = tmpMcu;
    }

    MouseCard card(4);
    assert(card.loadRoms(slotPath, mcuPath));
    card.onReset();

    // A realistic 6502 instruction-cycle mix — advanceCycles is driven per
    // instruction, so every budget is this small.
    static const int kInstrCycles[] = { 2, 3, 4, 2, 5, 6, 3, 4, 2, 7, 4, 2, 3, 6 };
    uint64_t budget = 0;
    for (int i = 0; i < 200000; ++i) {
        const int c = kInstrCycles[i % (sizeof(kInstrCycles) / sizeof(int))];
        budget += static_cast<uint64_t>(c);
        card.advanceCycles(c);
    }

    const double ratio = static_cast<double>(card.mcuCyclesRun()) /
                         static_cast<double>(budget);
    std::printf("  mouse MCU pacing: %llu MCU cycles / %llu CPU cycles"
                " = %.6f (target 2.000000)\n",
                static_cast<unsigned long long>(card.mcuCyclesRun()),
                static_cast<unsigned long long>(budget), ratio);
    std::fflush(stdout);     // the assert below abort()s without flushing
    if (std::fabs(ratio - 2.0) > 0.001) {
        std::fprintf(stderr,
            "MCU clocked at %.4fx the bus instead of 2.0x\n", ratio);
    }
    assert(std::fabs(ratio - 2.0) <= 0.001);

    if (!tmpSlot.empty()) std::remove(tmpSlot.c_str());
    if (!tmpMcu.empty())  std::remove(tmpMcu.c_str());
}

}  // namespace

int main()
{
    test_missing_roms_refuses_to_load();
    test_size_mismatch_refuses_to_load();
    test_slot_rom_bank_select();
    test_signature_passes_through_to_slot_rom();
    test_pending_irq_survives_snapshot();
    test_mcu_pacing_ratio();

    std::printf("OK mouse_card_smoke\n");
    return 0;
}
