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

// Purplesoft on an Eve — the card maker's own demo disk, run headless.
//
// Boots `disks_5.4/chatmauve/purplesoft-revb-oct83-demos.dsk` (DOS 3.3 →
// HELLO → BRUN PURPLESOFT → `& TEXT 1` → prompt) on a //e with 128 K and a
// Le Chat Mauve EVE, types a RUN command through the keyboard latch, and
// while the program runs:
//   * logs every change of the Eve's switch byte / AN3 / 80COL with the
//     emulated time, so the `& GR n` / `& TEXT n` tables in PURPLESOFT* can
//     be read against what the card decodes (dhgrMode / hgrMode / textMode);
//   * writes a PPM of the rendered frame at each such change (after a short
//     settle) and at the end, into the directory given by
//     POM2_PROBE_OUT (default: the current directory).
//
// Usage: test_purplesoft_eve_probe [program]   (default "DEMO GR16K")
// Diagnostic, not a pinned test — its output is for eyes (convert the PPMs
// and look) and for choosing what to pin next.

#include "Apple2Display.h"
#include "DiskIICard.h"
#include "LeChatMauveCard.h"
#include "M6502.h"
#include "Memory.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace {

bool fileExists(const std::string& p)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

std::string findFirst(std::initializer_list<const char*> candidates)
{
    for (const char* c : candidates) if (fileExists(c)) return c;
    return {};
}

void writePpm(const std::string& path, Apple2Display& d)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    const int w = d.width(), h = d.height();
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    const uint32_t* px = d.pixels();
    for (int i = 0; i < w * h; ++i) {
        const uint32_t c = px[i];
        const unsigned char rgb[3] = { static_cast<unsigned char>(c & 0xFF),
                                       static_cast<unsigned char>((c >> 8) & 0xFF),
                                       static_cast<unsigned char>((c >> 16) & 0xFF) };
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

const char* dhgrName(LeChatMauveCard::DhgrMode m)
{
    using D = LeChatMauveCard::DhgrMode;
    switch (m) {
        case D::BW560: return "BW560"; case D::COL140: return "COL140";
        case D::Mixed: return "Mixed"; case D::Chunky160: return "160";
        case D::COL280A: return "COL280A"; case D::COL280B: return "COL280B";
        case D::CP280: return "CP280"; case D::Blank: return "Blank";
    }
    return "?";
}
const char* hgrName(LeChatMauveCard::HgrMode m)
{
    using H = LeChatMauveCard::HgrMode;
    switch (m) {
        case H::LcmColor: return "LCM"; case H::Mono: return "Mono"; case H::FgBg: return "FgBg";
        case H::Spec1: return "Spec1"; case H::Spec2: return "Spec2"; case H::Dash: return "Dash";
        case H::Cp280: return "CP280";
    }
    return "?";
}
const char* textName(LeChatMauveCard::TextMode m)
{
    using T = LeChatMauveCard::TextMode;
    switch (m) { case T::Plain: return "Plain"; case T::Color: return "Color"; case T::Green: return "Green"; }
    return "?";
}

// The 40-column text page as 24 lines of ASCII (main RAM only).
void dumpText(Memory& mem)
{
    const uint8_t* ram = mem.data();
    for (int row = 0; row < 24; ++row) {
        const uint16_t a = static_cast<uint16_t>(0x0400 + 0x80 * (row & 7) + 0x28 * (row >> 3));
        std::string line;
        for (int c = 0; c < 40; ++c) {
            const uint8_t ch = ram[a + c] & 0x7F;
            line += (ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : '.';
        }
        std::printf("  |%s|\n", line.c_str());
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);   // a killed run keeps its log
    const std::string program = argc > 1 ? argv[1] : "DEMO GR16K";
    const std::string diskArg = argc > 2 ? argv[2] : "";
    const char* outEnv = std::getenv("POM2_PROBE_OUT");
    const std::string outDir = outEnv ? outEnv : ".";

    const std::string rom  = findFirst({ "../roms/apple2e.rom", "roms/apple2e.rom", "../../roms/apple2e.rom" });
    const std::string boot = findFirst({ "../roms/disk2.rom", "roms/disk2.rom", "../../roms/disk2.rom" });
    const std::string dsk  = !diskArg.empty() ? diskArg : findFirst({
        "../disks_5.4/chatmauve/purplesoft-revb-oct83-demos.dsk",
        "disks_5.4/chatmauve/purplesoft-revb-oct83-demos.dsk",
        "../../disks_5.4/chatmauve/purplesoft-revb-oct83-demos.dsk" });
    if (rom.empty() || boot.empty() || dsk.empty()) {
        std::printf("purplesoft_eve_probe SKIP: missing apple2e.rom / disk2.rom / purplesoft demos disk\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    // Bisection knobs (diagnostic): POM2_PROBE_IIPLUS=1 boots the ][+ ROM in
    // II+ mode, POM2_PROBE_NMOS=1 keeps the NMOS core, POM2_PROBE_NOEVE=1
    // leaves the card out, POM2_PROBE_C600=1 jumps straight to the PROM.
    const bool knobIIPlus = std::getenv("POM2_PROBE_IIPLUS") != nullptr;
    const bool knobNmos   = std::getenv("POM2_PROBE_NMOS") != nullptr;
    const bool knobNoEve  = std::getenv("POM2_PROBE_NOEVE") != nullptr;
    const bool knobC600   = std::getenv("POM2_PROBE_C600") != nullptr;
    const std::string rom2 = knobIIPlus
        ? findFirst({ "../roms/apple2p.rom", "roms/apple2p.rom", "../roms/apple2.rom", "roms/apple2.rom" }) : rom;

    Memory mem;
    mem.setIIEMode(!knobIIPlus);
    if (!mem.loadAppleIIRom(rom2.c_str())) { std::fprintf(stderr, "loadAppleIIRom failed\n"); return 1; }
    auto disk = std::make_unique<DiskIICard>();
    if (!disk->loadBootRom(boot) || !disk->insertDisk(dsk)) { std::fprintf(stderr, "Disk II setup failed\n"); return 1; }
    DiskIICard* diskRaw = disk.get();
    mem.slotBus().plug(6, std::move(disk));

    auto eveCard = std::make_unique<LeChatMauveCard>(7, LeChatMauveCard::Variant::Eve);
    LeChatMauveCard* eve = eveCard.get();
    if (!knobNoEve) {
        eve->setMemory(&mem);
        mem.slotBus().plug(7, std::move(eveCard));
    }

    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());
    disp.setChatMauveCard(eve);
    disp.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);

    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    // The enhanced //e ROM is 65C02 code: on an NMOS core its reset path
    // trips over KIL/undefined opcodes and the machine limps (CLAUDE.md,
    // "//c-class CPU rules"). The DROL probe forgot this and hangs in DOS.
    cpu.setCpuMode(knobNmos ? M6502::CpuMode::NMOS : M6502::CpuMode::CMOS);
    cpu.hardReset();
    mem.slotBus().reset();
    if (knobC600) cpu.setProgramCounter(0xC600);

    constexpr int kSecond = 1'022'727;
    auto runSeconds = [&](double s) {
        const uint64_t target = mem.getCycleCounter() + static_cast<uint64_t>(s * kSecond);
        while (mem.getCycleCounter() < target) cpu.run(2048);
    };
    auto stamp = [&]() { return static_cast<double>(mem.getCycleCounter()) / kSecond; };

    // DOS 3.3 boot + HELLO (BRUN PURPLESOFT, & TEXT 1). Headless DOS 3.3
    // pays RWTS's 1-s motor-on wait on nearly every sector in this emulator
    // (dos33_save_smoke: "~30M cycles to reach the prompt"; the 58-sector
    // PURPLESOFT* costs another minute) — so wait long, and report every
    // 10 s where the machine is. The first `$C0Bx` access or a display-mode
    // change is Purplesoft coming up; give it a few more seconds after that.
    {
        double seenAt = -1;
        uint64_t waitHits = 0, polls = 0;
        while (stamp() < 40.0) {
            for (int k = 0; k < 1000; ++k) {           // 1 s in 1 ms slices
                runSeconds(0.001);
                const uint16_t pc = cpu.getProgramCounter();
                ++polls;
                if ((pc >= 0xBD9F && pc <= 0xBDAB) || pc == 0xBA02 || pc == 0xBA03 ||
                    (pc >= 0x3D9F && pc <= 0x3DAB) || pc == 0x3A02 || pc == 0x3A03)
                    ++waitHits;
            }
            const auto& st = mem.getDisplayState();
            if (seenAt < 0 && (eve->eveSwitches() != 0 || !st.textMode || st.eightyCol || st.dhgr))
                seenAt = stamp();
            if (static_cast<int>(stamp()) % 10 == 0)
                std::printf("t=%5.1fs PC=$%04X halftrack=%d motor=%d text=%d hires=%d 80col=%d an3off=%d eve=$%02X\n",
                            stamp(), cpu.getProgramCounter(), diskRaw->getHalfTrack(), diskRaw->isMotorOn() ? 1 : 0,
                            st.textMode, st.hiRes, st.eightyCol, st.dhgr, eve->eveSwitches());
            if (seenAt > 0 && stamp() > seenAt + 8.0) break;
        }
        std::printf("boot: %.0f%% of samples inside RWTS's motor-on wait loop\n",
                    polls ? 100.0 * waitHits / polls : 0.0);
    }
    // Diagnostic: RWTS's motor-on check + wait, as loaded in RAM, and how the
    // boot time was spent (POM2_PROBE_RWTS=1).
    if (std::getenv("POM2_PROBE_RWTS")) {
        const uint8_t* r = mem.data();
        std::printf("RWTS $BD40..$BDBF:");
        for (int a = 0xBD40; a < 0xBDC0; ++a) std::printf(" %02X", r[a]);
        std::printf("\nRWTS $BA00..$BA10:");
        for (int a = 0xBA00; a < 0xBA10; ++a) std::printf(" %02X", r[a]);
        std::printf("\n");
    }
    std::printf("t=%.1fs after boot: PC=$%04X text=%d hires=%d 80col=%d an3off=%d eve=$%02X\n",
                stamp(), cpu.getProgramCounter(),
                mem.getDisplayState().textMode, mem.getDisplayState().hiRes,
                mem.getDisplayState().eightyCol, mem.getDisplayState().dhgr, eve->eveSwitches());
    dumpText(mem);
    disp.render(mem);
    writePpm(outDir + "/purplesoft_00_prompt.ppm", disp);

    // "COL280" is not a program on the disk but an experiment typed in: draw
    // one horizontal line per named colour in COL280A and COL280B, then read
    // the (main, aux) bit pairs back — the bit order the manual's prose does
    // not give (plan § 6). Everything else is `RUN <program>`.
    std::string cmd;
    if (program == "COL280") {
        cmd = "NEW\r"
              "10 & GR 7\r"
              "20 & COLOR= 9: & PLOT 0,0 TO 100,0\r"
              "30 & COLOR= 12: & PLOT 0,10 TO 100,10\r"
              "40 & COLOR= 15: & PLOT 0,20 TO 100,20\r"
              "50 & COLOR= 0: & PLOT 0,30 TO 100,30\r"
              "55 & COLOR= 1: & PLOT 0,40 TO 100,40\r"
              "60 FOR T=1 TO 4000: NEXT\r"
              "70 & GR 8\r"
              "80 & COLOR= 7: & PLOT 0,0 TO 100,0\r"
              "90 & COLOR= 11: & PLOT 0,10 TO 100,10\r"
              "100 & COLOR= 13: & PLOT 0,20 TO 100,20\r"
              "110 & COLOR= 0: & PLOT 0,30 TO 100,30\r"
              "120 & COLOR= 15: & PLOT 0,40 TO 100,40\r"
              "130 FOR T=1 TO 4000: NEXT\r"
              "RUN\r";
    } else {
        cmd = "RUN " + program + "\r";
    }
    mem.pasteText(cmd);
    std::printf("typed: %s\n", cmd.c_str());
    auto dumpRows = [&](const char* label) {
        std::printf("   %s — rows with lit dots:", label);
        for (int y = 0; y < 192; ++y) {
            const uint16_t a = static_cast<uint16_t>(0x2000 + 0x400 * (y & 7) + 0x80 * ((y >> 3) & 7) + 0x28 * (y >> 6));
            bool lit = false;
            for (int c = 0; c < 40; ++c) if ((mem.data()[a + c] | mem.auxData()[a + c]) & 0x7F) lit = true;
            if (lit) {
                std::printf(" %d", y);
                for (int c = 0; c < 3; ++c) std::printf("(%02X,%02X)", mem.data()[a + c], mem.auxData()[a + c]);
            }
        }
        std::printf("\n   %s — (main, aux) at col 0..3 for rows 0/10/20/30/40:\n", label);
        for (int y : { 0, 10, 20, 30, 40 }) {
            const uint16_t a = static_cast<uint16_t>(0x2000 + 0x400 * (y & 7) + 0x80 * ((y >> 3) & 7) + 0x28 * (y >> 6));
            std::printf("     row %2d:", y);
            for (int c = 0; c < 4; ++c) std::printf(" (%02X,%02X)", mem.data()[a + c], mem.auxData()[a + c]);
            std::printf("\n");
        }
    };

    // Run the program, logging the card's state changes and grabbing a frame
    // a moment after each (the program draws for a while, then waits).
    struct State { uint8_t sw; bool an3off, col80, text, hires, page2; };
    auto snap = [&]() {
        const auto& st = mem.getDisplayState();
        return State{ eve->eveSwitches(), st.dhgr, st.eightyCol, st.textMode, st.hiRes, st.page2 };
    };
    State last = snap();
    int shot = 1;
    double pendingShotAt = -1;
    const double runFor = 150.0;
    const double endAt = stamp() + runFor;
    while (stamp() < endAt) {
        runSeconds(1.0 / 60.0);
        const State now = snap();
        if (now.sw != last.sw || now.an3off != last.an3off || now.col80 != last.col80 ||
            now.text != last.text || now.hires != last.hires) {
            std::printf("t=%6.2fs eve=$%02X [%s%s%s%s%s%s%s%s] AN3 %s 80COL %s %s %s → DHGR %s / HGR %s / TEXT %s  CPREG=$%02X\n",
                        stamp(), now.sw,
                        (now.sw & 0x01) ? "ENHR " : "", (now.sw & 0x02) ? "HR1 " : "",
                        (now.sw & 0x04) ? "HR2 " : "", (now.sw & 0x08) ? "HR3 " : "",
                        (now.sw & 0x10) ? "TXT16 " : "", (now.sw & 0x20) ? "GREEN " : "",
                        (now.sw & 0x40) ? "LOCKCP " : "", (now.sw & 0x80) ? "LOCKRES" : "",
                        now.an3off ? "off" : "on", now.col80 ? "on" : "off",
                        now.text ? "TEXT" : (now.hires ? "HIRES" : "LORES"),
                        now.page2 ? "PAGE2" : "PAGE1",
                        dhgrName(eve->dhgrMode()), hgrName(eve->hgrMode(!now.an3off)),
                        textName(eve->textMode(now.col80, !now.an3off)), eve->cpreg());
            last = now;
            pendingShotAt = stamp() + 2.5;   // let the program draw its screen
        }
        if (pendingShotAt > 0 && stamp() >= pendingShotAt) {
            pendingShotAt = -1;
            disp.render(mem);
            char name[64];
            std::snprintf(name, sizeof(name), "/purplesoft_%02d_t%05.1f.ppm", shot++, stamp());
            writePpm(outDir + name, disp);
            std::printf("   shot %s (%dx%d)\n", name + 1, disp.width(), disp.height());
            // What the program put in the two banks of the HGR page: the
            // most frequent AUX bytes where MAIN is lit (CP280's colour
            // attributes — the nibble order shows here), and overall.
            {
                int histLit[256] = {0}, histAll[256] = {0}, histMain[256] = {0};
                for (int a = 0x2000; a < 0x4000; ++a) {
                    const uint8_t m = mem.data()[a], x = mem.auxData()[a];
                    ++histAll[x]; ++histMain[m];
                    if (m & 0x7F) ++histLit[x];
                }
                auto top = [&](const int* h, const char* what) {
                    std::printf("     %s:", what);
                    for (int k = 0; k < 6; ++k) {
                        int best = -1;
                        for (int v = 0; v < 256; ++v) if (h[v] > 0 && (best < 0 || h[v] > h[best])) best = v;
                        if (best < 0) break;
                        std::printf(" $%02X×%d", best, h[best]);
                        const_cast<int*>(h)[best] = 0;
                    }
                    std::printf("\n");
                };
                top(histLit,  "aux where main lit");
                top(histAll,  "aux overall       ");
                top(histMain, "main overall      ");
                if (program == "COL280") dumpRows(dhgrName(eve->dhgrMode()));
            }
        }
    }
    disp.render(mem);
    writePpm(outDir + "/purplesoft_99_end.ppm", disp);
    std::printf("t=%.1fs end: PC=$%04X\n", stamp(), cpu.getProgramCounter());
    dumpText(mem);
    return 0;
}
