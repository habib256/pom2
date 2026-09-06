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

// Purplesoft on an Eve — the maker's demo screens, frozen.
//
// Boots `disks_5.4/chatmauve/purplesoft-revb-oct83-demos.dsk` on a //e with
// 128 K and a Le Chat Mauve EVE (ChatMauveRGB pipeline), types `RUN DEMO
// GR16K` and `RUN DEMO TEXTE` through the keyboard latch, and hashes every
// STABLE screen: the demo shows each mode for a `FOR T = 1 TO 3000` pause,
// so "Eve switch state + framebuffer hash unchanged for 2 s of machine
// time" lands exactly in those pauses (while drawing, `& PLOT` toggles
// LOCKCPREG continuously and the picture moves). The ordered list of
// (state, hash) pairs is the golden: DEMO GR16K walks table IX-1's five
// AN3-off modes (COL140, COL280A, COL280B, CP280, BW560), DEMO TEXTE the
// six text screens (plain 40, TXT16 colour ×2, plain 80, TXTGREEN 80).
//
// Everything on the timeline is cycle-driven (boot, paste, Applesoft RND),
// so the run is deterministic and the hashes are host-independent — the
// same integer-only ChatMauveRGB pipeline `display_golden_hash` freezes.
// A change in CPU/disk timing legitimately moves the timeline; regenerate
// with:
//   POM2_GOLDEN_RECORD=1 build/tests/test_purplesoft_eve_screens
// and paste the printed tables. SKIPs when the ROMs or the disk are absent.

#include "Apple2Display.h"
#include "DiskIICard.h"
#include "LeChatMauveCard.h"
#include "M6502.h"
#include "Memory.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

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

uint64_t fnv1a(const void* data, size_t n)
{
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628257ull; }
    return h;
}

struct Screen { std::string label; uint64_t hash; };

// One machine, one program, the ordered stable screens.
std::vector<Screen> runProgram(const std::string& rom, const std::string& boot,
                               const std::string& dsk, const char* program,
                               double runFor)
{
    Memory mem;
    mem.setIIEMode(true);
    if (!mem.loadAppleIIRom(rom.c_str())) { std::fprintf(stderr, "ROM load failed\n"); std::exit(1); }
    auto disk = std::make_unique<DiskIICard>();
    if (!disk->loadBootRom(boot) || !disk->insertDisk(dsk)) { std::fprintf(stderr, "disk setup failed\n"); std::exit(1); }
    mem.slotBus().plug(6, std::move(disk));

    auto eveCard = std::make_unique<LeChatMauveCard>(7, LeChatMauveCard::Variant::Eve);
    LeChatMauveCard* eve = eveCard.get();
    eve->setMemory(&mem);
    mem.slotBus().plug(7, std::move(eveCard));

    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());
    disp.setChatMauveCard(eve);
    disp.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);

    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    mem.slotBus().reset();

    constexpr int kSecond = 1'022'727;
    auto runSeconds = [&](double s) {
        const uint64_t target = mem.getCycleCounter() + static_cast<uint64_t>(s * kSecond);
        while (mem.getCycleCounter() < target) cpu.run(2048);
    };

    // Boot: DOS 3.3 → HELLO → BRUN PURPLESOFT → `& TEXT 1` → prompt.
    // Fixed 40 s of machine time — generous but cycle-exact, so the paste
    // lands at the same cycle on every host.
    runSeconds(40.0);
    mem.pasteText(std::string("RUN ") + program + "\r");

    struct State {
        uint8_t sw; bool an3off, col80, text, hires, page2;
        bool operator==(const State& o) const {
            return sw == o.sw && an3off == o.an3off && col80 == o.col80 &&
                   text == o.text && hires == o.hires && page2 == o.page2;
        }
    };
    auto snap = [&]() {
        const auto& st = mem.getDisplayState();
        return State{ eve->eveSwitches(), st.dhgr, st.eightyCol,
                      st.textMode, st.hiRes, st.page2 };
    };
    auto labelOf = [&](const State& st) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "eve=%02X an3=%d 80col=%d %s",
                      st.sw, st.an3off ? 0 : 1, st.col80 ? 1 : 0,
                      st.text ? "TEXT" : (st.hires ? "HIRES" : "LORES"));
        return std::string(buf);
    };

    std::vector<Screen> out;
    State    lastState = snap();
    uint64_t lastHash  = 0;
    int      stable    = 0;
    bool     recorded  = true;   // don't record the pre-RUN prompt screen
    const double end = 40.0 + runFor;
    for (double t = 40.0; t < end; t += 0.2) {
        runSeconds(0.2);
        const State st = snap();
        disp.render(mem);
        const uint64_t h = fnv1a(disp.pixels(),
            static_cast<size_t>(disp.width()) * disp.height() * sizeof(uint32_t));
        if (st == lastState && h == lastHash) {
            if (++stable == 10 && !recorded) {           // 2 s unchanged
                out.push_back({ labelOf(st), h });
                recorded = true;
            }
        } else {
            stable   = 0;
            recorded = false;
            lastState = st;
            lastHash  = h;
        }
    }
    return out;
}

struct Golden { const char* label; uint64_t hash; };

// Regenerate with POM2_GOLDEN_RECORD=1 (see file header).
const Golden kGr16k[] = {
    { "eve=01 an3=0 80col=1 HIRES", 0x3692b3fb2952c803ULL },
    { "eve=03 an3=0 80col=1 HIRES", 0x9970a4166c430203ULL },
    { "eve=05 an3=0 80col=1 HIRES", 0x4d9447938811fdc3ULL },
    { "eve=0F an3=0 80col=0 HIRES", 0xf7c22d3637a68c83ULL },
    { "eve=0D an3=0 80col=1 HIRES", 0xc182c9278abcdf83ULL },
    { "eve=00 an3=1 80col=0 TEXT", 0x746637af3427b55cULL },
};
const Golden kTexte[] = {
    { "eve=00 an3=1 80col=0 TEXT", 0x4029be5b3d86ff83ULL },
    { "eve=00 an3=1 80col=0 TEXT", 0x5470d09f71d5d15cULL },
    { "eve=10 an3=1 80col=0 TEXT", 0xbf5f45860b15d6c3ULL },
    { "eve=10 an3=1 80col=0 TEXT", 0xf9a128193ccfb803ULL },
    { "eve=00 an3=1 80col=1 TEXT", 0x2bf25c769f26715cULL },
    { "eve=20 an3=1 80col=1 TEXT", 0x8f7fde43dde94603ULL },
    { "eve=00 an3=1 80col=1 TEXT", 0xb46f0ef3a6a5275cULL },
};

int check(const char* name, const std::vector<Screen>& got,
          const Golden* want, size_t nWant, bool record)
{
    if (record) {
        std::printf("const Golden k%s[] = {\n", name);
        for (const auto& s : got)
            std::printf("    { \"%s\", 0x%016llxULL },\n", s.label.c_str(),
                        static_cast<unsigned long long>(s.hash));
        std::printf("};\n");
        return 0;
    }
    int bad = 0;
    if (got.size() != nWant) {
        std::fprintf(stderr, "%s: %zu stable screens, expected %zu\n",
                     name, got.size(), nWant);
        ++bad;
    }
    for (size_t i = 0; i < got.size() && i < nWant; ++i) {
        if (got[i].label != want[i].label || got[i].hash != want[i].hash) {
            std::fprintf(stderr,
                "%s[%zu]: got { \"%s\", 0x%016llx }, want { \"%s\", 0x%016llx }\n",
                name, i, got[i].label.c_str(),
                static_cast<unsigned long long>(got[i].hash),
                want[i].label, static_cast<unsigned long long>(want[i].hash));
            ++bad;
        }
    }
    if (!bad) std::printf("  %s: %zu screens match\n", name, got.size());
    return bad;
}

} // namespace

int main()
{
    const std::string rom  = findFirst({ "../roms/apple2e.rom", "roms/apple2e.rom", "../../roms/apple2e.rom" });
    const std::string boot = findFirst({ "../roms/disk2.rom", "roms/disk2.rom", "../../roms/disk2.rom" });
    const std::string dsk  = findFirst({
        "../disks_5.4/chatmauve/purplesoft-revb-oct83-demos.dsk",
        "disks_5.4/chatmauve/purplesoft-revb-oct83-demos.dsk",
        "../../disks_5.4/chatmauve/purplesoft-revb-oct83-demos.dsk" });
    if (rom.empty() || boot.empty() || dsk.empty()) {
        std::printf("purplesoft_eve_screens SKIP: missing apple2e.rom / disk2.rom / demos disk\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }
    const bool record = std::getenv("POM2_GOLDEN_RECORD") != nullptr;

    std::printf("Purplesoft Eve screens:\n");
    const auto gr = runProgram(rom, boot, dsk, "DEMO GR16K", 110.0);
    const auto tx = runProgram(rom, boot, dsk, "DEMO TEXTE", 50.0);
    int bad = 0;
    bad += check("Gr16k", gr, kGr16k, std::size(kGr16k), record);
    bad += check("Texte", tx, kTexte, std::size(kTexte), record);
    if (record) return 0;
    if (bad) { std::fprintf(stderr, "purplesoft_eve_screens: %d mismatch(es)\n", bad); return 1; }
    std::printf("purplesoft_eve_screens: OK\n");
    return 0;
}
