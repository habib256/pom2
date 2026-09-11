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

// SmartPort-protocol dispatch ($Cn0D) + real-Liron identity test.
//
// Since 2026-07-12 the SmartPortCard implements the real SmartPort call
// convention at $Cn0D (= ProDOS entry $Cn0A + 3, matching the real Liron's
// $CnFF=$0A): `JSR $Cn0D / DFB cmd / DW paramList`, error code in A with
// carry set on failure, execution resuming past the 3 inline bytes. The
// 6502 handler lives in the card's $C800 bank and drives the C++ engine
// through device-select registers (see SmartPortCard::buildC800).
//
// When roms/liron.rom (the BMOW/Yellowstone dump of the real controller
// firmware) is present, the slot page is re-based on the real per-slot
// page — authentic identity bytes — with the HLE entries overlaid; the
// dispatch itself must behave identically with or without the dump, so
// every protocol assertion below runs in BOTH configurations.

#include "M6502.h"
#include "Memory.h"
#include "ResourcePaths.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int      kSlot       = 5;
constexpr size_t   kBlockBytes = 512;
constexpr size_t   kBlocks     = 1600;      // 800 K 3.5"
constexpr uint16_t kCallSite   = 0x0300;
constexpr uint16_t kPlist      = 0x0380;
constexpr uint16_t kBuf        = 0x2000;

std::string writeSyntheticPo() {
    const auto p = fs::temp_directory_path() / "pom2_liron_dispatch.po";
    std::vector<uint8_t> img(kBlocks * kBlockBytes);
    for (size_t b = 0; b < kBlocks; ++b)
        std::memset(img.data() + b * kBlockBytes,
                    static_cast<uint8_t>(b), kBlockBytes);
    std::ofstream f(p, std::ios::binary);
    assert(f && "open temp .po");
    f.write(reinterpret_cast<const char*>(img.data()),
            static_cast<std::streamsize>(img.size()));
    return p.string();
}

struct CallResult {
    uint8_t a;
    bool    carry;
    uint8_t sentinel;   // $10 after the post-call LDA/STA ran (RA advanced)
};

// JSR $Cn0D / DFB cmd / DW plist / LDA #$77 / STA $10 / park.
CallResult spCall(M6502& cpu, Memory& mem, uint8_t cmd)
{
    mem.memWrite(0x0010, 0x00);
    uint16_t p = kCallSite;
    auto w = [&](uint8_t v) { mem.memWrite(p++, v); };
    w(0x20); w(0x0D); w(static_cast<uint8_t>(0xC0 + kSlot));  // JSR $C50D
    w(cmd);
    w(kPlist & 0xFF); w(kPlist >> 8);
    // Post-call code — captures A/carry before anything else can touch it.
    w(0x08);                                  // PHP
    w(0x85); w(0x11);                         // STA $11 (A = error code)
    w(0x68); w(0x85); w(0x12);                // PLA / STA $12 (status flags)
    w(0xA9); w(0x77); w(0x85); w(0x10);       // LDA #$77 / STA $10 (sentinel)
    const uint16_t park = p;
    w(0x4C); w(park & 0xFF); w(park >> 8);    // JMP park

    // Seed A/P so a "did nothing" bug can't fake success.
    cpu.setAccumulator(0xEE);
    cpu.setProgramCounter(kCallSite);
    cpu.run(200000);

    CallResult r;
    r.a        = mem.memRead(0x11);
    r.carry    = (mem.memRead(0x12) & 0x01) != 0;
    r.sentinel = mem.memRead(0x10);
    return r;
}

void setPlist(Memory& mem, std::initializer_list<uint8_t> bytes)
{
    uint16_t p = kPlist;
    for (uint8_t b : bytes) mem.memWrite(p++, b);
}

}  // namespace

int main()
{
    const std::string po       = writeSyntheticPo();
    const std::string lironRom = pom2::findResource("roms/liron.rom");

    for (int pass = 0; pass < 2; ++pass) {
        const bool useLiron = pass == 1;
        if (useLiron && lironRom.empty()) {
            std::printf("liron_smartport_dispatch: roms/liron.rom absent — "
                        "identity pass skipped\n");
            break;
        }

        Memory mem;                          // II+ default → slot ROM served
        M6502 cpu(&mem);
        mem.setCpu(&cpu);
        cpu.hardReset();

        auto unit = std::make_unique<pom2::SmartPort35Unit>();
        assert(unit->loadImage(po));
        unit->setWriteBackEnabled(true);
        pom2::SmartPort35Unit* uraw = unit.get();

        auto card = std::make_unique<pom2::SmartPortCard>(kSlot);
        // Eight by default since 2026-09-11; this test's geometry is the
        // two-unit card (unit 2 empty → $28, STATUS unit 0 counts 2).
        assert(card->unitCount() == pom2::SmartPortCard::kDefaultUnits);
        card->setUnitCount(2);
        if (useLiron) assert(card->loadLironRom(lironRom));
        card->setUnit(0, std::move(unit));   // SmartPort unit 1
        // bay 1 left EMPTY (no unit object)  → SmartPort unit 2 = $28
        mem.slotBus().plug(kSlot, std::move(card));

        // ── Identity bytes (real dump only) ─────────────────────────────
        const uint16_t cn = 0xC000 + kSlot * 0x100;
        assert(mem.memRead(cn + 0x01) == 0x20);
        assert(mem.memRead(cn + 0x03) == 0x00);
        assert(mem.memRead(cn + 0x05) == 0x03);
        if (useLiron) {
            assert(mem.memRead(cn + 0x07) == 0x00 && "SmartPort class byte");
            assert(mem.memRead(cn + 0xFB) == 0x00 && "SmartPort ID type");
            assert(mem.memRead(cn + 0xFE) == 0xBF && "real capability byte");
            assert(mem.memRead(cn + 0xFF) == 0x0A && "ProDOS entry $Cn0A");
        }

        // ── STATUS unit 0, code 0: controller status ─────────────────────
        setPlist(mem, {3, 0, kBuf & 0xFF, kBuf >> 8, 0x00});
        auto r = spCall(cpu, mem, 0x00);
        assert(r.sentinel == 0x77 && "RA must advance past the 3 inline bytes");
        assert(!r.carry && r.a == 0x00);
        assert(mem.memRead(kBuf) == 2 && "device count");

        // ── STATUS unit 1, code 0: general status + 3-byte block count ──
        setPlist(mem, {3, 1, kBuf & 0xFF, kBuf >> 8, 0x00});
        r = spCall(cpu, mem, 0x00);
        assert(!r.carry && r.a == 0x00);
        assert(mem.memRead(kBuf) == 0xF8 && "block+write+read+online+format");
        const uint32_t blocks = mem.memRead(kBuf + 1)
                              | mem.memRead(kBuf + 2) << 8
                              | mem.memRead(kBuf + 3) << 16;
        assert(blocks == kBlocks);

        // ── STATUS unit 1, code 3: DIB ───────────────────────────────────
        setPlist(mem, {3, 1, kBuf & 0xFF, kBuf >> 8, 0x03});
        r = spCall(cpu, mem, 0x00);
        assert(!r.carry && r.a == 0x00);
        assert(mem.memRead(kBuf + 4) == 14 && "ID string length");
        assert(mem.memRead(kBuf + 5) == 'P' && mem.memRead(kBuf + 8) == '2');
        assert(mem.memRead(kBuf + 21) == 0x01 && "device type: 3.5 disk");

        // ── READ unit 1, block 7 ─────────────────────────────────────────
        setPlist(mem, {3, 1, kBuf & 0xFF, kBuf >> 8, 7, 0, 0});
        r = spCall(cpu, mem, 0x01);
        assert(!r.carry && r.a == 0x00);
        for (int i = 0; i < 512; ++i)
            assert(mem.memRead(kBuf + i) == 0x07);

        // ── WRITE unit 1, block 12 ───────────────────────────────────────
        for (int i = 0; i < 512; ++i) mem.memWrite(0x2800 + i, 0x5A);
        setPlist(mem, {3, 1, 0x00, 0x28, 12, 0, 0});
        r = spCall(cpu, mem, 0x02);
        assert(!r.carry && r.a == 0x00);
        uint8_t blk[kBlockBytes];
        assert(uraw->readBlock(12, blk));
        for (int i = 0; i < 512; ++i) assert(blk[i] == 0x5A);
        assert(mem.memRead(0x2800) == 0x5A && "caller buffer intact");

        // ── Errors: empty bay ($28), bad block ($2D), bad pcount ($04),
        //    bad command ($01), FORMAT ok, CONTROL code 0 ok ─────────────
        setPlist(mem, {3, 2, kBuf & 0xFF, kBuf >> 8, 0, 0, 0});
        r = spCall(cpu, mem, 0x01);
        assert(r.carry && r.a == 0x28);

        setPlist(mem, {3, 1, kBuf & 0xFF, kBuf >> 8, 0x40, 0x06, 0});  // 1600
        r = spCall(cpu, mem, 0x01);
        assert(r.carry && r.a == 0x2D);

        setPlist(mem, {5, 1, kBuf & 0xFF, kBuf >> 8, 0, 0, 0});
        r = spCall(cpu, mem, 0x01);
        assert(r.carry && r.a == 0x04);

        setPlist(mem, {1, 1, 0, 0, 0});
        r = spCall(cpu, mem, 0x42);          // extended READ — unimplemented
        assert(r.carry && r.a == 0x01);

        setPlist(mem, {3, 1, 0, 0, 0});
        r = spCall(cpu, mem, 0x03);          // FORMAT (write-back on)
        assert(!r.carry && r.a == 0x00);

        setPlist(mem, {3, 1, kBuf & 0xFF, kBuf >> 8, 0x00});
        r = spCall(cpu, mem, 0x04);          // CONTROL code 0
        assert(!r.carry && r.a == 0x00);

        // ── ZP $42-$45 preserved across the dispatch ─────────────────────
        mem.memWrite(0x42, 0xDE); mem.memWrite(0x43, 0xAD);
        mem.memWrite(0x44, 0xBE); mem.memWrite(0x45, 0xEF);
        setPlist(mem, {3, 1, kBuf & 0xFF, kBuf >> 8, 0x00});
        r = spCall(cpu, mem, 0x00);
        assert(!r.carry);
        assert(mem.memRead(0x42) == 0xDE && mem.memRead(0x43) == 0xAD &&
               mem.memRead(0x44) == 0xBE && mem.memRead(0x45) == 0xEF);

        // ── //e with INTC8ROM latched ────────────────────────────────────
        // The dispatch stub jumps into the card's $C800 bank. On a //e with
        // SLOTC3ROM off, any read in $C300-$C3FF latches INTC8ROM and points
        // $C800-$CFFF at the MOTHERBOARD ROM — and the 80-column firmware
        // reads $C3xx all the time, so this is the normal state of a running
        // machine, not a corner. Without the `BIT $CFFF` that now precedes
        // the JMP, $CE00 fetched internal ROM bytes and the SmartPort call
        // ran whatever they happened to decode to. Same call as the very
        // first STATUS above; the only difference is the MMU state.
        mem.setIIEMode(true);
        (void)mem.memRead(0xC300);           // latch INTC8ROM
        setPlist(mem, {3, 1, kBuf & 0xFF, kBuf >> 8, 0x00});
        mem.memWrite(kBuf, 0x00);            // no stale payload to pass on
        r = spCall(cpu, mem, 0x00);
        assert(r.sentinel == 0x77 && "RA must still advance past the 3 bytes");
        assert(!r.carry && r.a == 0x00 && "dispatch survives INTC8ROM");
        assert(mem.memRead(kBuf) == 0xF8 && "real STATUS payload, not ROM");
        mem.setIIEMode(false);

        std::printf("liron_smartport_dispatch: %s pass OK\n",
                    useLiron ? "real-ROM" : "synthetic");
    }

    std::printf("liron_smartport_dispatch: all assertions passed\n");
    return 0;
}
