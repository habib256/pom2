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

// videx_ultraterm — the Videx UltraTerm (VidexUltratermCard.h).
//
// Bus part, synthetic ROMs: $CnXX serves the slot's own copy, $C800-$CBFF
// the upper firmware, $CC00 VRAM or firmware by MCP bit 7, the 512-byte
// blocks picked by A2-A3 in Videoterm mode and the 256-byte pages by the MCP,
// reset (MCP 0, control 2 kept), the snapshot, and the picture's geometry
// and attributes.
//
// Firmware part: the real UltraTerm firmware (MAME a2ulttrm) in slot 3 of a
// ][+: PR#3 turns the card's picture on (MCP bit 6), and what BASIC prints
// is in VRAM where the CRTC shows it.

#include "M6502.h"
#include "Memory.h"
#include "VidexUltratermCard.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int64_t kSecond = 1'022'727;

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

std::vector<uint8_t> slurp(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

void testBus()
{
    VidexUltratermCard c(3);
    std::vector<uint8_t> fw(4096), ch(4096, 0);
    for (std::size_t i = 0; i < fw.size(); ++i) fw[i] = uint8_t(i >> 8);
    CHECK(!c.loadRoms(std::vector<uint8_t>(2048), ch));
    CHECK(c.loadRoms(fw, ch));

    CHECK(c.slotRomRead(0x10) == 0x03);            // slot 3's copy
    CHECK(c.expansionRomRead(0x000) == 0x08);       // $C800 = firmware $800
    CHECK(c.expansionRomRead(0x3FF) == 0x0B);

    // Videoterm mode: $C0n4 selects block 1; $CC00 is VRAM, mirrored per 512.
    c.deviceSelectRead(0x4);
    c.expansionRomWrite(0x400, 0xAB);
    CHECK(c.vram()[512] == 0xAB);
    CHECK(c.expansionRomRead(0x600) == 0xAB);       // $CE00 mirrors $CC00
    c.deviceSelectWrite(0xC, 0);                     // a write moves it too: block 3
    CHECK(c.windowBase() == 1536);

    // MCP bit 7: firmware over $CC00 for reads; writes still go to VRAM.
    c.deviceSelectWrite(2, VidexUltratermCard::kMemSel);
    CHECK(c.expansionRomRead(0x400) == 0x0C);
    c.expansionRomWrite(0x400, 0x5A);
    CHECK(c.vram()[0] == 0x5A);                      // MCP 0x80: Videoterm mode, block 0

    // 256-byte pages: MCP bit 4 + page 9; A2-A3 no longer move anything.
    c.deviceSelectWrite(2, VidexUltratermCard::kPages256 | 9);
    c.deviceSelectRead(0xC);
    CHECK(c.windowBase() == 9 * 256);
    c.expansionRomWrite(0x5FF, 0x77);                // $CDFF → page offset $FF
    CHECK(c.vram()[9 * 256 + 0xFF] == 0x77);
    CHECK(c.displayMask() == 0x0FFF);
    CHECK(c.deviceSelectRead(2) == (VidexUltratermCard::kPages256 | 9));
    CHECK(c.deviceSelectRead(6) == (VidexUltratermCard::kPages256 | 9));   // A0-A1 decode

    c.deviceSelectWrite(3, 0x21);
    CHECK(c.ownsScreen(true, false) == false);
    c.deviceSelectWrite(2, VidexUltratermCard::kVidSel);
    CHECK(c.ownsScreen(false, false));
    c.onReset();
    CHECK(c.control1() == 0 && c.control2() == 0x21 && !c.ownsScreen(true, true));

    // Snapshot.
    c.deviceSelectWrite(2, VidexUltratermCard::kPages256 | 2);
    std::vector<uint8_t> blob;
    c.appendSnapshotState(blob);
    VidexUltratermCard d(3);
    d.loadSnapshotState(blob.data(), blob.size());
    CHECK(d.control1() == (VidexUltratermCard::kPages256 | 2) && d.vram()[9 * 256 + 0xFF] == 0x77);
}

// Geometry and attributes: 160 × 24 cells of 9 × 1 rasters, one glyph whose
// raster 0 is $80 (the leftmost dot): a highlight char vs a lowlight one,
// and inverse.
void testPicture()
{
    VidexUltratermCard c(3);
    std::vector<uint8_t> fw(4096, 0), ch(4096, 0);
    ch[0x800 + 'A' * 16] = 0x80;
    c.loadRoms(fw, ch);
    auto reg = [&](int r, uint8_t v) { c.deviceSelectWrite(0, uint8_t(r)); c.deviceSelectWrite(1, v); };
    reg(1, 160); reg(6, 24); reg(9, 0); reg(10, 0x20);   // cursor off
    reg(12, 0); reg(13, 0);
    CHECK(c.pictureWidth() == 1440 && c.pictureHeight() == 24);
    c.deviceSelectWrite(2, VidexUltratermCard::kPages256);   // page 0 at $CC00
    c.expansionRomWrite(0x400, 'A');                          // lowlight
    c.expansionRomWrite(0x401, 'A' | 0x80);                   // bit 7: highlight
    c.deviceSelectWrite(3, 0x10);
    std::vector<uint32_t> pic(1440 * 24);
    const uint32_t lit = 0xFF00FF00u, dark = 0xFF000000u;
    c.paintPicture(pic.data(), lit, dark, 0, 1'022'727.0);
    CHECK(pic[0] == 0xFF00BF00u);                  // 3/4 of the lit green
    CHECK(pic[1] == dark);
    CHECK(pic[9] == lit);
    c.deviceSelectWrite(3, 0x02);                  // inverse for bit-7-clear chars
    c.paintPicture(pic.data(), lit, dark, 0, 1'022'727.0);
    CHECK(pic[0] == dark && pic[1] == 0xFF00BF00u);
}

uint16_t textRowBase(int row) { return uint16_t(0x0400 + (row % 8) * 0x80 + (row / 8) * 0x28); }

std::string cardText(const VidexUltratermCard& c)
{
    std::string out;
    const auto& crtc = c.crtc();
    uint16_t addr = crtc.startAddress();
    for (int r = 0; r < crtc.vertDisplayed(); ++r) {
        std::string line;
        for (int i = 0; i < crtc.horizDisplayed(); ++i) {
            const char ch = char(c.vram()[(addr + i) & c.displayMask()] & 0x7F);
            line.push_back(ch >= 0x20 && ch < 0x7F ? ch : ' ');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line + "\n";
        addr = uint16_t(addr + crtc.horizDisplayed());
    }
    return out;
}

std::string findFirst(std::initializer_list<const char*> c)
{
    for (const char* p : c) if (fs::is_regular_file(p)) return p;
    return {};
}

// 0 = ran, 77 = media missing.
int testFirmware()
{
    const std::string rom = findFirst({ "roms/apple2p.rom" });
    const std::string fw  = findFirst({ VidexUltratermCard::kRomPath });
    const std::string ch  = findFirst({ VidexUltratermCard::kCharPath });
    if (rom.empty() || fw.empty() || ch.empty()) return 77;

    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    auto card = std::make_unique<VidexUltratermCard>(3);
    if (!mem.loadAppleIIRom(rom.c_str()) || !card->loadRoms(slurp(fw), slurp(ch))) {
        std::fprintf(stderr, "FAIL: machine setup\n");
        ++g_fail;
        return 0;
    }
    VidexUltratermCard* ut = card.get();
    mem.slotBus().plug(3, std::move(card));
    cpu.hardReset();
    mem.slotBus().reset();
    auto run = [&](int64_t cycles) { for (int64_t t = 0; t < cycles;) t += cpu.run(4096); };
    run(3 * kSecond);                         // no disk: the Autostart ends at BASIC
    mem.pasteText("PR#3\r");
    run(2 * kSecond);
    mem.pasteText("PRINT \"ULTRA\" + \"TERM\"\r");
    run(2 * kSecond);
    const std::string text = cardText(*ut);
    CHECK(ut->ownsScreen(true, false));
    CHECK(ut->crtc().horizDisplayed() == 80);
    CHECK(text.find("ULTRATERM") != std::string::npos);
    CHECK(ut->pictureWidth() == 720);
    if (const char* ppm = std::getenv("POM2_ULTRATERM_PPM")) {   // eyeball the glyphs
        const int w = ut->pictureWidth(), h = ut->pictureHeight();
        std::vector<uint32_t> pic(std::size_t(w) * std::size_t(h));
        ut->paintPicture(pic.data(), 0xFFFFFFFFu, 0xFF000000u, 0, 1'022'727.0);
        std::ofstream o(ppm, std::ios::binary);
        o << "P6\n" << w << " " << h << "\n255\n";
        for (uint32_t px : pic) { const char rgb[3] = { char(px), char(px >> 8), char(px >> 16) }; o.write(rgb, 3); }
    }
    if (g_fail || std::getenv("POM2_DUMP")) {
        std::fprintf(stderr, "MCP %02X C2 %02X R1 %d R6 %d R9 %d\n%s", ut->control1(),
                     ut->control2(), ut->crtc().horizDisplayed(), ut->crtc().vertDisplayed(),
                     ut->crtc().maxRaster(), text.c_str());
        for (int r = 0; r < 24; ++r) {
            std::string s;
            for (int c = 0; c < 40; ++c) {
                const char x = char(mem.memRead(uint16_t(textRowBase(r) + c)) & 0x7F);
                s.push_back(x >= 0x20 && x < 0x7F ? x : '.');
            }
            std::fprintf(stderr, "|%s|\n", s.c_str());
        }
    }
    return 0;
}

} // namespace

int main()
{
    testBus();
    testPicture();
    const int fw = testFirmware();
    if (g_fail) {
        std::fprintf(stderr, "videx_ultraterm: %d failure(s)\n", g_fail);
        return 1;
    }
    if (fw == 77) {
        std::printf("videx_ultraterm: SKIP firmware part — needs roms/apple2p.rom, "
                    "roms/videx_ultraterm_frm_b537.bin, roms/videx_ultraterm_chs_7859.bin\n");
        return 77;
    }
    std::printf("videx_ultraterm: PASS\n");
    return 0;
}
