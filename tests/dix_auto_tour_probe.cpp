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

// dix_auto_tour_probe — DIX (disks_3.5/DIX.po) played through in its own
// automatic mode on the fresh-install machine, no window. Diagnostic, not a
// pinned test (VALIDATION-002 evidence, 2026-10-23).
//
// The machine is the fresh-install one (CLAUDE.md § Fresh-install
// defaults): //e Enhanced PAL, Grappler+ in 1, Mockingboard A/C in 2, the
// AppleWin-HLE mouse in 4, the SmartPort card in 5 holding DIX, Le Chat
// Mauve (Féline) in 7. The Disk II in 6 is left out: nothing is in it.
//
// At the menu it presses LEFT (choice 0 wraps to 16, "ALL DEMOS IN
// AUTOMATIC MODE") then RETURN. In that mode every part runs on its own
// timer and the loader chains them (`loader.a` AUTOMODE), so a part that
// hangs never hands over. Parts are told apart by the first block each
// SmartPort read asks for, read from $46/$47 at the $C50A entry the loader
// calls (`loader.a` READ_FILE; start blocks from its LOAD_* routines).
//
// Per part it prints the span, how many seconds the video RAM changed
// (raster parts such as Mad Effect change soft switches over static RAM, so
// "still" is not "frozen"), and whether the PC was ever found on a $00 —
// the runaway signature. With --png it writes three frames per part, as
// POM2's software display paints them, under probeOutDir().
//
// Usage: dix_auto_tour_probe [--seconds 2100] [--png] [--out DIR]
//                            [--disk path.po]

#include "Apple2Display.h"
#include "CpuClock.h"
#include "LeChatMauveCard.h"
#include "M6502.h"
#include "Memory.h"
#include "MemoryWatchSink.h"
#include "Mockingboard.h"
#include "ProbeOutDir.h"
#include "ResourcePaths.h"
#include "SlotBus.h"
#include "SlotCardFactory.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

struct Part { int block; const char* name; };
// First block of each LOAD_* routine in DIX's loader.a.
const Part kParts[] = {
    {10, "menu"}, {100, "Unlimited Bobs"}, {153, "Scroll Scroll Scroll"},
    {205, "Crazy Cycles"}, {336, "A Question of Frames"}, {530, "Plasmagoria"},
    {560, "Raster Bars"}, {635, "Crazy Cycles 2"}, {836, "One More Thing"},
    {945, "Mad Effect"}, {1018, "Mad Effect 2"},
    {1069, "Unreeeal Superhero 3 Tribute"}, {1097, "Mad Effect 3"},
    {1132, "Digidream"}, {1156, "Oldskool Fort et Vert"},
    {1173, "Digidream SID Voice Edition"}};
constexpr int kPartCount = sizeof kParts / sizeof kParts[0];

int partOf(int block)
{
    int k = -1;
    for (int i = 0; i < kPartCount; ++i)
        if (block >= kParts[i].block) k = i;
    return k;
}

// Notes the part each SmartPort block read belongs to; a change of part is
// a hand-over.
struct SmartPortEntry : pom2::MemoryWatchSink {
    Memory* mem = nullptr;
    const long long* cycles = nullptr;
    int current = -1, pending = -1;
    long long since = 0;
    int reads = 0;
    void noteAccess(uint16_t, uint8_t, bool write) override
    {
        if (write) return;
        ++reads;
        const int p = partOf(mem->peekCpuView(0x46) | mem->peekCpuView(0x47) << 8);
        if (p >= 0 && p != current) { current = pending = p; since = *cycles; }
    }
};

}  // namespace

int main(int argc, char** argv)
{
    double seconds = 2100;   // one full cycle (~28 min) and the start of the next
    bool png = false;
    std::string outArg, diskArg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--png") png = true;
        else if (a == "--out" && i + 1 < argc) outArg = argv[++i];
        else if (a == "--disk" && i + 1 < argc) diskArg = argv[++i];
    }
    const std::string rom = pom2::findResource("roms/apple2e.rom");
    const std::string disk = diskArg.empty() ? pom2::findResource("disks_3.5/DIX.po") : diskArg;
    if (rom.empty() || disk.empty()) { std::printf("SKIP: missing apple2e.rom or DIX.po\n"); return 77; }
    const double hz = POM2_TIMING_PAL.cpuClockHz;

    Memory mem;
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    mem.setVideoStandard(VideoStandard::PAL);
    if (!mem.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/false)) return 1;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);

    pom2::SlotCardFactory factory;
    for (const auto& [slot, key] : {std::pair<int, const char*>{1, "grappler"}, {4, "mouseaw"}}) {
        pom2::SlotCardFactory::Request rq;
        rq.key = key; rq.slot = slot; rq.cpuIsCmos = true;
        auto r = factory.create(rq);
        if (!r) { std::printf("slot %d: '%s' not created (%s)\n", slot, key, r.warning.c_str()); continue; }
        r.card->setCpuClock(hz);
        r.card->setStandardClock(hz);
        mem.slotBus().plug(slot, std::move(r.card));
    }
    auto mb = std::make_unique<MockingboardCard>(2, MockingboardCard::Variant::AC);
    mb->setCpu(&cpu);
    mb->setCpuClock(hz);
    mb->setStandardClock(hz);
    mem.slotBus().plug(2, std::move(mb));
    auto sp = std::make_unique<pom2::SmartPortCard>(5);
    auto unit = std::make_unique<pom2::SmartPort35Unit>();
    if (!unit->loadImage(disk)) { std::printf("FAIL: %s\n", unit->lastError().c_str()); return 1; }
    sp->setUnit(0, std::move(unit));
    mem.slotBus().plug(5, std::move(sp));
    auto lcm = std::make_unique<LeChatMauveCard>(7, LeChatMauveCard::Variant::Feline);
    lcm->setMemory(&mem);
    lcm->setCpuClock(hz);
    LeChatMauveCard* chatMauve = lcm.get();
    mem.slotBus().plug(7, std::move(lcm));

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    mem.slotBus().reset();

    long long cycles = 0;
    SmartPortEntry entry;
    entry.mem = &mem;
    entry.cycles = &cycles;
    mem.setWatchSink(&entry);
    mem.setReadWatch(0xC50A, true);   // opcode fetch at the block entry

    const std::string out = png ? pom2test::probeOutDir(outArg) : std::string();
    Apple2Display display;
    display.setAuxMemory(mem.auxData());
    display.setChatMauveCard(chatMauve);
    auto shot = [&](const std::string& name) {
        if (!png) return;
        display.render(mem);
        const std::string path = out + "/" + name + ".png";
        stbi_write_png(path.c_str(), display.width(), display.height(), 4,
                       display.pixels(), display.width() * 4);
    };
    auto run = [&](double s) {
        const long long end = cycles + static_cast<long long>(s * hz);
        while (cycles < end) cycles += cpu.run(4096);
    };
    auto videoHash = [&] {
        uint64_t h = 1469598103934665603ULL;
        const uint8_t* aux = mem.auxData();
        for (int a = 0x400; a < 0x6000; ++a) {
            h ^= mem.peekMainRam(static_cast<uint16_t>(a)); h *= 1099511628211ULL;
            h ^= aux[a];                                     h *= 1099511628211ULL;
        }
        return h;
    };

    run(25);   // the menu is up at ~17 s
    shot("dix_menu");
    mem.pasteRawKeys("\x08", 1);   // LEFT: 0 wraps to 16 = automatic mode
    run(1);
    std::printf("menu choice $DFFF = %02X at %.1f s\n", mem.peekCpuView(0xDFFF), cycles / hz);
    mem.pasteRawKeys("\r", 1);

    int part = -1, index = 0, moving = 0, still = 0, onBrk = 0, shots = 0, failures = 0;
    long long partStart = cycles;
    uint64_t previous = videoHash();
    auto report = [&] {
        if (part < 0) return;
        std::printf("  %-30s %7.1f .. %7.1f s (%5.1f s)  video moving %3d s / still %3d s  on $00: %d\n",
                    kParts[part].name, partStart / hz, cycles / hz, (cycles - partStart) / hz,
                    moving, still, onBrk);
        if (onBrk) ++failures;
    };
    while (cycles < seconds * hz) {
        run(1);
        if (entry.pending >= 0) {
            report();
            part = entry.pending; entry.pending = -1; partStart = entry.since;
            moving = still = onBrk = shots = 0; ++index;
        }
        const uint64_t h = videoHash();
        (h != previous ? moving : still)++;
        previous = h;
        if (mem.peekCpuView(cpu.getProgramCounter()) == 0x00) ++onBrk;
        const double t = (cycles - partStart) / hz;
        if (part > 0 && ((shots == 0 && t >= 10) || (shots == 1 && t >= 45) || (shots == 2 && t >= 100))) {
            char name[64];
            std::snprintf(name, sizeof name, "dix_%02d_b%04d_%03ds", index, kParts[part].block, static_cast<int>(t));
            shot(name);
            ++shots;
        }
    }
    report();
    std::printf("end %.1f s, %d SmartPort reads, PC=$%04X%s%s\n", cycles / hz, entry.reads,
                cpu.getProgramCounter(), png ? ", frames in " : "", out.c_str());
    return failures ? 2 : 0;
}
