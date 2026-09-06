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

// Differential pin for the inline bus fast paths in Memory.h.
//
// Memory::memRead() and Memory::memWrite() decide their hot cases inline and
// fall through to memReadSlow() / memWriteSlow(), the original bodies. The
// fast path's contract is "byte-identical value AND identical side effects" —
// and the one regression it has had (2026-08-20) was a missing `addr < 0xD000`
// bound that sent language-card RAM reads into the //e internal-ROM shortcut;
// no hash in pom2_bench caught it because no bench workload maps LC RAM on a
// //e. So this test walks the whole 64 K under every paging state that feeds
// the fast-path conditions and compares, per address:
//
//   reads:  memRead(a) == memReadSlow(a), and the INTC8ROM latch must not move
//           on the slow call if the fast call already ran (a fast path that
//           skipped a side effect shows up as the slow path performing it
//           afterwards);
//   writes: memWrite(a, v) and memWriteSlow(a, v') must land in the same bank
//           (main / aux / neither).
//
// Addresses with read side effects of their own ($C000-$C0FF soft switches and
// device selects, $CFFF) are skipped: the differential would perturb the state
// it is measuring. They are not fast-path candidates anyway.

#include "DiskIICard.h"
#include "Memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

struct MemoryFastPathProbe {
    static uint8_t readSlow(Memory& m, uint16_t a)               { return m.memReadSlow(a); }
    static void    writeSlow(Memory& m, uint16_t a, uint8_t v)    { m.memWriteSlow(a, v); }
    static bool    intC8Rom(const Memory& m)                      { return m.intC8Rom; }
    static void    setIntC8Rom(Memory& m, bool v)                 { m.intC8Rom = v; }
    static uint8_t mainAt(const Memory& m, uint16_t a)            { return m.mem[a]; }
    static uint8_t auxAt(const Memory& m, uint16_t a)             { return m.aux[a]; }
    static void    seed(Memory& m, uint16_t a, uint8_t v)         { m.mem[a] = v; m.aux[a] = v; }
    static void    seedMain(Memory& m, uint16_t a, uint8_t v)     { m.mem[a] = v; }
    static void    seedAux(Memory& m, uint16_t a, uint8_t v)      { m.aux[a] = v; }
    static bool    lcReadRam(const Memory& m)                     { return m.lcReadRam; }
};

namespace {

int g_failures = 0;

void fail(const char* what, int state, unsigned addr, unsigned a, unsigned b)
{
    if (++g_failures <= 20)
        std::fprintf(stderr, "FAIL %s state=%d addr=$%04X fast=%02X slow=%02X\n",
                     what, state, addr, a, b);
}

std::string findRom()
{
    for (const char* p : { "roms/apple2e.rom", "../roms/apple2e.rom", "../../roms/apple2e.rom" }) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec)) return p;
    }
    return {};
}

// Paging state bits for the matrix.
enum : unsigned {
    S_INTCXROM = 1u << 0,
    S_SLOTC3   = 1u << 1,
    S_INTC8    = 1u << 2,
    S_LCRAM    = 1u << 3,   // language card reads RAM ($C083 ×2)
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
    // Soft-switch writes (//e MMU: writes to $C00x). The order matters only in
    // that INTC8ROM is a latch set by a $C3xx read, so it is forced last.
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

} // namespace

int main()
{
    const std::string rom = findRom();
    if (rom.empty()) {
        std::fprintf(stderr, "bus_fastpath: roms/apple2e.rom not found — skipped\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    for (int iie = 0; iie < 2; ++iie) {
        Memory m;
        m.setIIEMode(iie != 0);
        if (!m.loadAppleIIRom(rom.c_str())) {
            std::fprintf(stderr, "loadAppleIIRom failed\n");
            return 1;
        }
        // A real slot ROM in slot 6 so the "not internal ROM" cases read
        // something other than the floating bus (embedded boot PROM).
        m.slotBus().plug(6, std::make_unique<DiskIICard>());
        // Distinguishable contents in every bank so a wrong bank SHOWS: main,
        // aux and both language-card banks each get their own pattern. (The
        // first version of this test left them all zero and let the very bug
        // it was written for — LC RAM reads taking the internal-ROM shortcut
        // — pass, because both banks read back 00.)
        for (unsigned a = 0; a < 0xC000; ++a) {
            MemoryFastPathProbe::seedMain(m, static_cast<uint16_t>(a), static_cast<uint8_t>(0x11 + (a * 7)));
            MemoryFastPathProbe::seedAux(m,  static_cast<uint16_t>(a), static_cast<uint8_t>(0x99 + (a * 13)));
        }
        for (unsigned bank = 0; bank < 2; ++bank) {
            const uint16_t sw = bank ? 0xC08B : 0xC083;   // read RAM + write-enable
            (void)m.memRead(sw); (void)m.memRead(sw);
            for (unsigned a = 0xD000; a < 0x10000; ++a)
                MemoryFastPathProbe::writeSlow(m, static_cast<uint16_t>(a),
                                               static_cast<uint8_t>((bank ? 0x40 : 0xC0) + (a * 3)));
        }

        for (unsigned s = 0; s < S_COUNT; ++s) {
            if (!iie && (s & ~S_LCRAM)) continue;   // ][+: only the LC bit applies
            applyState(m, s);
            if (((s & S_LCRAM) != 0) != MemoryFastPathProbe::lcReadRam(m)) {
                std::fprintf(stderr, "LC state did not take (state %u)\n", s);
                return 1;
            }

            // ── reads ───────────────────────────────────────────────────
            for (unsigned a = 0; a < 0x10000; ++a) {
                if (a >= 0xC000 && a <= 0xC0FF) continue;
                if (a == 0xCFFF) continue;
                const bool l0 = MemoryFastPathProbe::intC8Rom(m);
                const uint8_t fv = m.memRead(static_cast<uint16_t>(a));
                const bool l1 = MemoryFastPathProbe::intC8Rom(m);
                const uint8_t sv = MemoryFastPathProbe::readSlow(m, static_cast<uint16_t>(a));
                const bool l2 = MemoryFastPathProbe::intC8Rom(m);
                if (fv != sv) fail("read value", static_cast<int>(s), a, fv, sv);
                if (l1 != l2) fail("read side effect (INTC8ROM)", static_cast<int>(s), a, l1, l2);
                (void)l0;
                // Restore the latch so each address sees the state under test.
                MemoryFastPathProbe::setIntC8Rom(m, (s & S_INTC8) != 0);
            }

            // ── writes (RAM only: $C000+ writes are soft switches / LC) ──
            for (unsigned a = 0; a < 0xC000; ++a) {
                const uint8_t v1 = static_cast<uint8_t>(0xA5 ^ (a & 0xFF));
                const uint8_t v2 = static_cast<uint8_t>(~v1);
                // Both banks start from a sentinel distinct from v1 and v2, so
                // "which bank holds the value" is unambiguous.
                MemoryFastPathProbe::seed(m, static_cast<uint16_t>(a), static_cast<uint8_t>(v1 ^ 0x55));
                m.memWrite(static_cast<uint16_t>(a), v1);
                const bool fMain = MemoryFastPathProbe::mainAt(m, static_cast<uint16_t>(a)) == v1;
                const bool fAux  = MemoryFastPathProbe::auxAt(m, static_cast<uint16_t>(a))  == v1;
                MemoryFastPathProbe::seed(m, static_cast<uint16_t>(a), static_cast<uint8_t>(v1 ^ 0x55));
                MemoryFastPathProbe::writeSlow(m, static_cast<uint16_t>(a), v2);
                const bool sMain = MemoryFastPathProbe::mainAt(m, static_cast<uint16_t>(a)) == v2;
                const bool sAux  = MemoryFastPathProbe::auxAt(m, static_cast<uint16_t>(a))  == v2;
                if (fMain != sMain || fAux != sAux)
                    fail("write bank", static_cast<int>(s), a,
                         (fMain ? 1u : 0u) | (fAux ? 2u : 0u),
                         (sMain ? 1u : 0u) | (sAux ? 2u : 0u));
            }
        }
    }

    if (g_failures) {
        std::fprintf(stderr, "bus_fastpath: %d mismatches\n", g_failures);
        return 1;
    }
    std::printf("bus_fastpath: fast paths match slow paths over every address and state\n");
    return 0;
}
