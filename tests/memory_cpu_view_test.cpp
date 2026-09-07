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

// Pin for Memory::peekCpuView / snapshotCpuView / peekCpuWriteTarget — the
// side-effect-free paged view the Debugger panel and the Memory Viewer read.
//
// The bug it exists for (hunt #4, #11 + #12): both panels snapshotted
// `Memory::data()`, the flat MAIN-bank mirror. On a //e executing out of aux,
// out of a RamWorks bank or out of language-card RAM, the disassembly showed
// bytes the CPU never fetches — so the instruction boundaries were wrong and
// every breakpoint set by clicking that listing landed mid-instruction. The
// Memory Viewer had the matching write bug: it read the mirror but wrote
// through memWrite(), so under 80STORE/RAMWRT an Undo pushed a main-RAM byte
// into aux.
//
// Three properties are asserted, over the same paging matrix bus_fastpath_test
// walks:
//
//   1. VALUE: peekCpuView(a) == memRead(a) for all of $0000-$BFFF and
//      $D000-$FFFF (the ranges the view claims to resolve), plus the //e
//      internal-I/O-ROM window when the paging state maps it there.
//   2. NO SIDE EFFECTS: a full snapshotCpuView() over all 64 KiB — soft
//      switches included — leaves the machine's serialized state, the
//      INTC8ROM latch and the $C800 expansion owner bit-identical.
//   3. WRITE TARGET: peekCpuWriteTarget(a) is the byte memWrite(a, …) would
//      replace, which under RAMWRT≠RAMRD is NOT peekCpuView(a).

#include "DiskIICard.h"
#include "Memory.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct MemoryFastPathProbe {
    static bool    intC8Rom(const Memory& m)                   { return m.intC8Rom; }
    static void    setIntC8Rom(Memory& m, bool v)              { m.intC8Rom = v; }
    static void    seedMain(Memory& m, uint16_t a, uint8_t v)  { m.mem[a] = v; }
    static void    seedAux(Memory& m, uint16_t a, uint8_t v)   { m.aux[a] = v; }
    static uint8_t mainAt(const Memory& m, uint16_t a)         { return m.mem[a]; }
    static uint8_t auxAt(const Memory& m, uint16_t a)          { return m.aux[a]; }
    static void    writeSlow(Memory& m, uint16_t a, uint8_t v) { m.memWriteSlow(a, v); }
    static bool    lcReadRam(const Memory& m)                  { return m.lcReadRam; }
};

namespace {

int g_failures = 0;

void fail(const char* what, unsigned state, unsigned addr,
          unsigned a, unsigned b)
{
    if (++g_failures <= 20)
        std::fprintf(stderr,
                     "FAIL %s state=%u addr=$%04X view=%02X bus=%02X\n",
                     what, state, addr, a, b);
}

std::string findRom()
{
    for (const char* p : { "roms/apple2e.rom", "../roms/apple2e.rom",
                           "../../roms/apple2e.rom" }) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec)) return p;
    }
    return {};
}

enum : unsigned {
    S_INTCXROM = 1u << 0,
    S_SLOTC3   = 1u << 1,
    S_INTC8    = 1u << 2,
    S_LCRAM    = 1u << 3,
    S_RAMRD    = 1u << 4,
    S_RAMWRT   = 1u << 5,
    S_ALTZP    = 1u << 6,
    S_80STORE  = 1u << 7,
    S_PAGE2    = 1u << 8,
    S_HIRES    = 1u << 9,
    S_COUNT    = 1u << 10,
};

void applyState(Memory& m, unsigned s)
{
    m.resetSoftSwitches();
    m.memWrite((s & S_INTCXROM) ? 0xC007 : 0xC006, 0);
    m.memWrite((s & S_SLOTC3)   ? 0xC00B : 0xC00A, 0);
    m.memWrite((s & S_RAMRD)    ? 0xC003 : 0xC002, 0);
    m.memWrite((s & S_RAMWRT)   ? 0xC005 : 0xC004, 0);
    m.memWrite((s & S_ALTZP)    ? 0xC009 : 0xC008, 0);
    m.memWrite((s & S_80STORE)  ? 0xC001 : 0xC000, 0);
    (void)m.memRead((s & S_PAGE2) ? 0xC055 : 0xC054);
    (void)m.memRead((s & S_HIRES) ? 0xC057 : 0xC056);
    if (s & S_LCRAM) { (void)m.memRead(0xC083); (void)m.memRead(0xC083); }
    else             { (void)m.memRead(0xC082); }
    MemoryFastPathProbe::setIntC8Rom(m, (s & S_INTC8) != 0);
}

// True when the //e paging state maps the motherboard internal I/O ROM at
// `addr` — the only part of $C100-$CFFF the view claims to resolve (a slot
// card's $Cn00 page can be MMIO, so it is deliberately left as the mirror).
bool mapsInternalIORom(unsigned s, unsigned addr)
{
    const bool c3     = (addr & 0xFF00u) == 0xC300u;
    const bool slotC3 = (s & S_SLOTC3) != 0;
    if (s & S_INTCXROM) return true;
    if (c3 && !slotC3)  return true;
    if ((s & S_INTC8) && addr >= 0xC800) return true;
    return false;
}

}  // namespace

int main()
{
    const std::string rom = findRom();
    if (rom.empty()) {
        std::fprintf(stderr,
                     "memory_cpu_view: roms/apple2e.rom not found — skipped\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    for (int iie = 0; iie < 2; ++iie) {
        Memory m;
        m.setIIEMode(iie != 0);
        if (!m.loadAppleIIRom(rom.c_str())) {
            std::fprintf(stderr, "loadAppleIIRom failed\n");
            return 1;
        }
        m.slotBus().plug(6, std::make_unique<DiskIICard>());

        // Distinct patterns per bank, so reading the WRONG bank shows.
        for (unsigned a = 0; a < 0xC000; ++a) {
            MemoryFastPathProbe::seedMain(m, static_cast<uint16_t>(a),
                                          static_cast<uint8_t>(0x11 + a * 7));
            MemoryFastPathProbe::seedAux(m, static_cast<uint16_t>(a),
                                         static_cast<uint8_t>(0x99 + a * 13));
        }
        for (unsigned bank = 0; bank < 2; ++bank) {
            const uint16_t sw = bank ? 0xC08B : 0xC083;
            (void)m.memRead(sw); (void)m.memRead(sw);
            for (unsigned a = 0xD000; a < 0x10000; ++a)
                MemoryFastPathProbe::writeSlow(
                    m, static_cast<uint16_t>(a),
                    static_cast<uint8_t>((bank ? 0x40 : 0xC0) + a * 3));
        }

        std::vector<uint8_t> view(0x10000, 0);

        for (unsigned s = 0; s < S_COUNT; ++s) {
            if (!iie && (s & ~S_LCRAM)) continue;   // ][+: only the LC bit
            applyState(m, s);

            // ── 1. value ────────────────────────────────────────────────
            for (unsigned a = 0; a < 0x10000; ++a) {
                const bool io = (a >= 0xC000 && a <= 0xC0FF);
                const bool slotWindow =
                    (a >= 0xC100 && a < 0xD000) &&
                    !(iie && mapsInternalIORom(s, a));
                if (io || slotWindow || a == 0xCFFF) continue;

                const uint8_t pv = m.peekCpuView(static_cast<uint16_t>(a));
                const uint8_t bv = m.memRead(static_cast<uint16_t>(a));
                if (pv != bv) fail("value", s, a, pv, bv);
                // memRead may have moved INTC8ROM; put the state back.
                MemoryFastPathProbe::setIntC8Rom(m, (s & S_INTC8) != 0);
            }

            // ── 2. no side effects, over the WHOLE 64 K ─────────────────
            applyState(m, s);
            std::vector<uint8_t> before;
            m.appendSnapshotState(before);
            const bool latchBefore = MemoryFastPathProbe::intC8Rom(m);
            const int  expBefore   = m.slotBus().getActiveExpansionSlot();

            m.snapshotCpuView(view.data());

            std::vector<uint8_t> after;
            m.appendSnapshotState(after);
            if (before != after) {
                std::fprintf(stderr,
                    "FAIL snapshotCpuView changed machine state (state=%u)\n", s);
                ++g_failures;
            }
            if (latchBefore != MemoryFastPathProbe::intC8Rom(m)) {
                std::fprintf(stderr,
                    "FAIL snapshotCpuView moved INTC8ROM (state=%u)\n", s);
                ++g_failures;
            }
            if (expBefore != m.slotBus().getActiveExpansionSlot()) {
                std::fprintf(stderr,
                    "FAIL snapshotCpuView claimed/released $C800 (state=%u)\n", s);
                ++g_failures;
            }
            // The bulk form must agree with the single-address one.
            for (unsigned a = 0; a < 0x10000; ++a) {
                const uint8_t pv = m.peekCpuView(static_cast<uint16_t>(a));
                if (view[a] != pv) fail("bulk", s, a, view[a], pv);
            }
        }

        if (!iie) continue;

        // ── 3. write target ────────────────────────────────────────────
        // RAMWRT on, RAMRD off: reads come from main, writes go to aux. A
        // view-derived undo record would capture the main byte here.
        applyState(m, S_RAMWRT);
        MemoryFastPathProbe::seedMain(m, 0x6000, 0x11);
        MemoryFastPathProbe::seedAux(m, 0x6000, 0x22);
        if (m.peekCpuView(0x6000) != 0x11) {
            std::fprintf(stderr, "FAIL read view is not main under RAMWRT\n");
            ++g_failures;
        }
        if (m.peekCpuWriteTarget(0x6000) != 0x22) {
            std::fprintf(stderr, "FAIL write target is not aux under RAMWRT\n");
            ++g_failures;
        }
        const uint8_t replaced = m.peekCpuWriteTarget(0x6000);
        m.memWrite(0x6000, 0x55);
        if (MemoryFastPathProbe::auxAt(m, 0x6000) != 0x55 ||
            MemoryFastPathProbe::mainAt(m, 0x6000) != 0x11) {
            std::fprintf(stderr, "FAIL memWrite did not land in aux\n");
            ++g_failures;
        }
        // Undo, the way the viewer replays it.
        m.memWrite(0x6000, replaced);
        if (MemoryFastPathProbe::auxAt(m, 0x6000) != 0x22) {
            std::fprintf(stderr, "FAIL undo did not restore the aux byte\n");
            ++g_failures;
        }

        // Language card, the #11 headline: LC RAM mapped in, and the flat
        // mirror still holds ROM.
        applyState(m, S_LCRAM);
        const uint8_t lcView   = m.peekCpuView(0xD000);
        const uint8_t lcBus    = m.memRead(0xD000);
        const uint8_t lcMirror = m.data()[0xD000];
        if (lcView != lcBus) {
            std::fprintf(stderr, "FAIL LC view %02X != bus %02X\n", lcView, lcBus);
            ++g_failures;
        }
        if (lcView == lcMirror) {
            std::fprintf(stderr,
                "FAIL LC view equals the flat mirror — test setup is degenerate\n");
            ++g_failures;
        }
    }

    if (g_failures) {
        std::fprintf(stderr, "memory_cpu_view: %d failures\n", g_failures);
        return 1;
    }
    std::printf("memory_cpu_view: ok\n");
    return 0;
}
