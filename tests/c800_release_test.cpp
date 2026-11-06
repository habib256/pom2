// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which accesses make a card let go of $C800-$CFFF, card by card.
//
// The Apple II convention is one address, $CFFF, and no word about RESET:
// [B] Apple II Reference Manual (1979) p. 85, Fig. 8 — a latch with
//     S = I/O SELECT and R = $CFFF. Fig. 9 on the same page lets a card
//     decode less ("$CFXX Decoding"), and cards did, each its own way:
//
//         card                 lets go on        RESET clears   source
//         (the convention)     $CFFF             no             Fig. 8
//         Super Serial Card    $CF00-$CFFF       yes            its manual p. 45
//         Grappler (1981)      $CFF0-$CFFF       no             its schematic
//         Grappler+            $CFF0-$CFFF       no             its schematic
//         Liron                $CFFE-$CFFF       yes            board schematic
//         CFFA 2.0             $CFEF, $CFFF      no             its CPLD source
//
// The overrides carry the citations (SlotPeripheral.h has the table). This
// test pins the table, runs two of the cards through the bus, and — when the
// firmware dumps are present — checks the ROMs agree: every card leaves the
// bytes it cannot serve unused.

#include "CffaCard.h"
#include "GrapplerCard.h"
#include "GrapplerClassicCard.h"
#include "LironCard.h"
#include "SlotBus.h"
#include "SlotPeripheral.h"
#include "SuperSerialCard.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool ok, const std::string& what)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

struct PlainRomCard final : SlotPeripheral {
    std::string_view name() const override { return "plain"; }
    uint8_t deviceSelectRead(uint8_t) override { return 0xFF; }
    void    deviceSelectWrite(uint8_t, uint8_t) override {}
    uint8_t slotRomRead(uint8_t) override { return 0x11; }
    uint8_t expansionRomRead(uint16_t) override { return 0x22; }
    bool    takesC800() const override { return true; }
};

// Every one of the 2048 offsets, against the set the schematic gives.
void expectDecode(const SlotPeripheral& card, const char* label,
                  const std::function<bool(uint16_t)>& releases, bool onReset)
{
    int wrong = 0;
    uint16_t first = 0;
    for (uint16_t off = 0; off < 0x800; ++off) {
        if (card.releasesC800At(off) != releases(off)) {
            if (!wrong) first = off;
            ++wrong;
        }
    }
    char msg[160];
    std::snprintf(msg, sizeof msg, "%s: the release decode matches its schematic "
                  "(%d of 2048 addresses wrong, first $%04X)", label, wrong,
                  wrong ? 0xC800 + first : 0);
    expect(wrong == 0, msg);
    expect(card.releasesC800OnReset() == onReset,
           std::string(label) + (onReset ? ": RESET clears the latch"
                                         : ": RESET leaves the latch alone"));
}

std::vector<uint8_t> readRom(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

}  // namespace

int main()
{
    std::printf("== $C800 release decode, card by card ==\n");

    PlainRomCard        plain;
    SuperSerialCard     ssc(2);
    GrapplerClassicCard grappler(1);
    GrapplerCard        grapplerPlus(1);
    pom2::LironCard     liron(5);
    pom2::CffaCard      cffa(7);

    expectDecode(plain, "the convention",
                 [](uint16_t o) { return o == 0x7FF; }, false);
    expectDecode(ssc, "Super Serial Card",
                 [](uint16_t o) { return o >= 0x700; }, true);
    expectDecode(grappler, "Grappler (1981)",
                 [](uint16_t o) { return o >= 0x7F0; }, false);
    expectDecode(grapplerPlus, "Grappler+",
                 [](uint16_t o) { return o >= 0x7F0; }, false);
    expectDecode(liron, "Liron",
                 [](uint16_t o) { return o >= 0x7FE; }, true);
    expectDecode(cffa, "CFFA 2.0",
                 [](uint16_t o) { return o == 0x7FF || o == 0x7EF; }, false);

    // Through the bus: the hooks are what SlotBus asks.
    std::printf("\n-- on the bus --\n");
    {
        SlotBus bus;
        bus.plug(5, std::make_unique<pom2::LironCard>(5));
        bus.plug(7, std::make_unique<pom2::CffaCard>(7));

        auto claim = [&](uint16_t page) {
            (void)bus.expansionRomRead(0xCFFF);
            (void)bus.slotRomRead(page);
        };
        claim(0xC700);
        expect(bus.getActiveExpansionSlot() == 7, "CFFA takes the window from $C7xx");
        (void)bus.expansionRomRead(0xCFEE);
        (void)bus.expansionRomRead(0xCFDF);
        (void)bus.expansionRomRead(0xCFFE);
        expect(bus.getActiveExpansionSlot() == 7,
               "CFFA keeps it at $CFEE, $CFDF and $CFFE (its routine runs to $CFFE)");
        (void)bus.expansionRomRead(0xCFEF);
        expect(bus.getActiveExpansionSlot() == -1, "CFFA lets go at $CFEF");
        claim(0xC700);
        bus.expansionRomWrite(0xCFEF, 0x00);
        expect(bus.getActiveExpansionSlot() == -1, "…on a write as well");
        claim(0xC700);
        bus.reset(false);
        expect(bus.getActiveExpansionSlot() == 7, "CFFA keeps the window across RESET");

        claim(0xC500);
        expect(bus.getActiveExpansionSlot() == 5, "the Liron takes the window from $C5xx");
        (void)bus.expansionRomRead(0xCFFD);
        (void)bus.expansionRomRead(0xCFEF);
        expect(bus.getActiveExpansionSlot() == 5,
               "the Liron keeps it at $CFFD — and at the CFFA's $CFEF");
        (void)bus.expansionRomRead(0xCFFE);
        expect(bus.getActiveExpansionSlot() == -1, "the Liron lets go at $CFFE");
        claim(0xC500);
        bus.reset(false);
        expect(bus.getActiveExpansionSlot() == -1, "RESET clears the Liron's latch");
        bus.reset(true);
    }

    // The firmware never uses a byte its own decode takes away. Present only
    // where the dumps are (tools/fetch_test_media.py).
    std::printf("\n-- the ROMs agree --\n");
    {
        const auto gp = readRom("roms/grappler_plus.bin");
        if (gp.size() == 4096) {
            bool filler = true;
            for (size_t bank = 0; bank < 2; ++bank)
                for (size_t i = 0; i < 16; ++i) {
                    static const uint8_t pat[4] = {0x00, 0xFF, 0xFF, 0x00};
                    if (gp[bank * 0x800 + 0x7F0 + i] != pat[i & 3]) filler = false;
                }
            expect(filler, "Grappler+: $CFF0-$CFFF is filler in both banks");
        } else std::printf("  [SKIP] roms/grappler_plus.bin not present\n");

        const auto g1 = readRom("roms/grappler_eps-1.bin");
        if (g1.size() == 2048) {
            // "(C) SEC 08/16/81", high bit set: text, not code.
            const std::string want = "(C) SEC 08/16/81";
            bool text = true;
            for (size_t i = 0; i < 16; ++i)
                if (g1[0x7F0 + i] != (static_cast<uint8_t>(want[i]) | 0x80)) text = false;
            expect(text, "Grappler: $CFF0-$CFFF is the copyright string");
        } else std::printf("  [SKIP] roms/grappler_eps-1.bin not present\n");

        const auto lr = readRom("roms/liron.rom");
        if (lr.size() == 4096)
            expect(lr[0x800 + 0x7FE] == 0xFF && lr[0x800 + 0x7FF] == 0xFF &&
                   lr[0x800 + 0x7FD] != 0xFF,
                   "Liron: $CFFE and $CFFF are unused, $CFFD is not");
        else std::printf("  [SKIP] roms/liron.rom not present\n");

        for (const char* path : {"roms/cffa20ee02.bin", "roms/cffa20eec02.bin"}) {
            const auto cf = readRom(path);
            if (cf.size() == 4096)
                expect(cf[0x800 + 0x7EF] == 0xFF && cf[0x800 + 0x7F0] != 0xFF &&
                       cf[0x800 + 0x7FE] == 0x60,
                       std::string(path) + ": $CFEF is a hole, code runs "
                       "$CFF0-$CFFE and ends on RTS");
            else std::printf("  [SKIP] %s not present\n", path);
        }
    }

    if (failures) { std::printf("\nc800_release: %d failure(s)\n", failures); return 1; }
    std::printf("\nOK c800_release\n");
    return 0;
}
