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

// videx_videoterm_boot — the real Videoterm firmware on the real Apple ][+
// ROM. ROM-gated: SKIPs (77) unless roms/apple2p.rom, a Videoterm firmware
// and its normal character set resolve (RomFetch downloads the card's dumps
// from MAME's a2vidtrm romset; none of them is tracked in the repository).
//
// The card is built by SlotCardFactory, the way the machine builds it, and
// plugged into slot 3 — the only slot the v2.4 firmware works in.
//
//   1. power on with no disk controller: Applesoft's `]` prompt, 40 columns
//   2. `PR#3`: the firmware initialises the 6845 (R1 = 80 columns, R6 = 24
//      rows, R9 = 8 → 9 rasters), sets AN0 ($C059), and Applesoft's prompt
//      lands in the CARD's VRAM; Apple2Display then publishes 720 × 216
//   3. `PRINT 6*7` answers 42 in VRAM
//   4. Ctrl-Reset: the ][+ RESET handler reads $C058 (Autostart $FA6F), AN0
//      drops, and the monitor shows the Apple's 40 columns again

#include "Apple2Display.h"
#include "M6502.h"
#include "Memory.h"
#include "ResourcePaths.h"
#include "SlotBus.h"
#include "SlotCardFactory.h"
#include "VidexVideotermCard.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0;
void expect(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

void runCycles(M6502& cpu, long n)
{
    long spent = 0;
    while (spent < n) spent += cpu.run(1000);
}

// Does the card's VRAM hold `text` (7-bit ASCII) anywhere, in any case of
// bit 7? The firmware stores plain ASCII; bit 7 would select the alternate
// set.
bool vramHas(const VidexVideotermCard& c, const char* text)
{
    const std::size_t n = std::strlen(text);
    for (std::size_t a = 0; a < VidexVideotermCard::kVramBytes; ++a) {
        bool ok = true;
        for (std::size_t i = 0; i < n && ok; ++i)
            ok = (c.vram()[(a + i) & 0x7FF] & 0x7F) == static_cast<uint8_t>(text[i]);
        if (ok) return true;
    }
    return false;
}

bool apple40Has(Memory& mem, char ch)
{
    for (uint16_t a = 0x400; a < 0x800; ++a)
        if ((mem.memRead(a) & 0x7F) == static_cast<uint8_t>(ch)) return true;
    return false;
}

void typeLine(Memory& mem, M6502& cpu, const char* line)
{
    mem.pasteText(std::string(line));
    runCycles(cpu, 600000);
}

} // namespace

int main()
{
    const std::string rom = pom2::findResource("roms/apple2p.rom");
    pom2::SlotCardFactory factory;
    auto made = factory.create({ "videoterm", 3, false, pom2::SystemProfile::AppleIIPlus });
    if (rom.empty() || !made) {
        std::printf("SKIP: videx_videoterm_boot needs roms/apple2p.rom and the "
                    "Videoterm dumps (%s)\n",
                    made.warning.empty() ? "no ][+ ROM" : made.warning.c_str());
        return 77;
    }
    std::printf("card: %s\n", made.status.c_str());

    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    expect(mem.loadAppleIIRom(rom.c_str()), "][+ ROM loads");
    auto* card = static_cast<VidexVideotermCard*>(made.card.get());
    mem.slotBus().plug(3, std::move(made.card));
    cpu.setCpuMode(M6502::CpuMode::NMOS);
    mem.resetSoftSwitches();
    mem.slotBus().reset();
    cpu.hardReset();
    Apple2Display disp;
    disp.setVidexCard(card);

    // 1. Applesoft, 40 columns.
    runCycles(cpu, 3'000'000);
    expect(apple40Has(mem, ']'), "power-on: Applesoft prompt on the 40-column page");
    expect(!mem.getDisplayState().an0, "power-on: AN0 off");
    disp.render(mem);
    expect(disp.width() == 280 && !disp.showingCardPicture(), "power-on: the Apple's picture");

    // 2. PR#3.
    typeLine(mem, cpu, "PR#3\r");
    expect(mem.getDisplayState().an0, "PR#3: the firmware set AN0 ($C059)");
    expect(card->crtc().horizDisplayed() == 80, "PR#3: 6845 R1 = 80 columns");
    expect(card->crtc().vertDisplayed() == 24, "PR#3: 6845 R6 = 24 rows");
    expect(card->crtc().maxRaster() == 8, "PR#3: 6845 R9 = 8 (9 rasters per row)");
    expect(vramHas(*card, "]"), "PR#3: the ] prompt is in the card's VRAM");
    disp.render(mem);
    expect(disp.showingCardPicture() && disp.width() == 720 && disp.height() == 216,
           "PR#3: the monitor shows the card's 720x216 picture");
    {
        // Some dot of the picture must be lit: the prompt, or the cursor.
        const uint32_t* p = disp.pixels();
        bool lit = false;
        for (int i = 0; i < 720 * 216 && !lit; ++i) lit = p[i] != 0xFF000000u;
        expect(lit, "PR#3: the card's picture is not blank");
    }

    // 3. BASIC through the card.
    typeLine(mem, cpu, "PRINT 6*7\r");
    expect(vramHas(*card, "PRINT 6*7"), "the typed line echoes into VRAM");
    expect(vramHas(*card, "42"), "PRINT 6*7 answers 42 in VRAM");

    // 4. Ctrl-Reset — EmulationController::softReset's sequence.
    mem.resetSoftSwitchesWarm();
    mem.slotBus().reset();
    cpu.softReset();
    runCycles(cpu, 1'000'000);
    expect(!mem.getDisplayState().an0, "Ctrl-Reset: the ROM's LDA $C058 dropped AN0");
    expect(card->vramBank() == 0, "Ctrl-Reset: the card's window is back on bank 0");
    disp.render(mem);
    expect(disp.width() == 280 && !disp.showingCardPicture(),
           "Ctrl-Reset: the monitor shows the Apple's 40 columns again");
    expect(apple40Has(mem, ']'), "Ctrl-Reset: Applesoft prompt on the 40-column page");

    if (failures) {
        std::printf("videx_videoterm_boot: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("videx_videoterm_boot: ok\n");
    return 0;
}
