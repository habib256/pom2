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

// Extasie mouse probe — diagnostic, no add_test.
//
// The user reports the mouse dead in Extasie (`disks_5.4/gist/Extasie
// disk1.dsk`, ProDOS, Chat Mauve mixed-mode program). This boots it on a
// //e with 128 K, a Le Chat Mauve (Féline) and an AppleWin-HLE mouse card,
// and answers with data:
//   * what the program shows (text-page dumps + PPM frames into
//     POM2_PROBE_OUT);
//   * whether it ever TOUCHES the mouse card — a write-watch sink over
//     $C080-$C0FF logs every device-select write with its slot;
//   * whether injected host-mouse motion (setHostMouse ramps + clicks)
//     changes anything on screen.
//
// Env knobs: POM2_PROBE_MOUSE_SLOT (default 4; the //e mouse's home),
// POM2_PROBE_KEYS (characters typed at 5 s intervals once the program is
// up, e.g. "1" to pick a menu entry; '\n' allowed via ','), POM2_PROBE_RUNFOR.

#include "Apple2Display.h"
#include "DiskIICard.h"
#include "LeChatMauveCard.h"
#include "M6502.h"
#include "Memory.h"
#include "MemoryWatchSink.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
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
    std::fprintf(f, "P6\n%d %d\n255\n", d.width(), d.height());
    const uint32_t* px = d.pixels();
    for (int i = 0; i < d.width() * d.height(); ++i) {
        const unsigned char rgb[3] = { static_cast<unsigned char>(px[i] & 0xFF),
                                       static_cast<unsigned char>((px[i] >> 8) & 0xFF),
                                       static_cast<unsigned char>((px[i] >> 16) & 0xFF) };
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

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

// Every device-select WRITE, counted per address (reads are not delivered
// by the watch machinery — writes are enough to see which slot's card the
// program initialises).
struct DevselLog : pom2::MemoryWatchSink {
    std::map<uint16_t, int> writes;
    void noteAccess(uint16_t addr, uint8_t, bool write) override {
        if (write) ++writes[addr];
    }
    void report() {
        std::printf("devsel writes ($C080-$C0FF):");
        if (writes.empty()) std::printf(" none");
        for (auto& [a, n] : writes) std::printf(" $%04X×%d", a, n);
        std::printf("\n");
        writes.clear();
    }
};

} // namespace

int main()
{
    const int mouseSlot = std::getenv("POM2_PROBE_MOUSE_SLOT")
        ? std::atoi(std::getenv("POM2_PROBE_MOUSE_SLOT")) : 4;
    const double runFor = std::getenv("POM2_PROBE_RUNFOR")
        ? std::atof(std::getenv("POM2_PROBE_RUNFOR")) : 90.0;
    const char* keys = std::getenv("POM2_PROBE_KEYS");
    const char* outEnv = std::getenv("POM2_PROBE_OUT");
    const std::string outDir = outEnv ? outEnv : ".";

    const std::string rom  = findFirst({ "../roms/apple2e.rom", "roms/apple2e.rom" });
    const std::string boot = findFirst({ "../roms/disk2.rom", "roms/disk2.rom" });
    const std::string mrom = findFirst({ "../roms/mouse_341-0270-c.bin", "roms/mouse_341-0270-c.bin" });
    const std::string dsk  = std::getenv("POM2_PROBE_DISK")
        ? std::string(std::getenv("POM2_PROBE_DISK"))
        : findFirst({
        "../disks_5.4/gist/Extasie disk1.dsk", "disks_5.4/gist/Extasie disk1.dsk" });
    if (rom.empty() || boot.empty() || mrom.empty() || dsk.empty()) {
        std::printf("extasie_mouse_probe SKIP: missing rom/prom/mouse rom/disk\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    Memory mem;
    mem.setIIEMode(true);
    if (!mem.loadAppleIIRom(rom.c_str())) return 1;
    auto disk = std::make_unique<DiskIICard>();
    if (!disk->loadBootRom(boot) || !disk->insertDisk(dsk)) return 1;
    mem.slotBus().plug(6, std::move(disk));

    LeChatMauveCard::Variant cmVariant = LeChatMauveCard::Variant::Feline;
    if (const char* v = std::getenv("POM2_PROBE_CM_VARIANT"))
        LeChatMauveCard::parseVariant(v, cmVariant);
    auto chat = std::make_unique<LeChatMauveCard>(7, cmVariant);
    LeChatMauveCard* chatRaw = chat.get();
    std::printf("chat mauve variant: %s\n", LeChatMauveCard::variantKey(cmVariant));
    mem.slotBus().plug(7, std::move(chat));

    // POM2_PROBE_MOUSE_LLE=1 plugs the MAME-LLE MouseCard (the user's card
    // key "mouse") instead of the AppleWin HLE — same firmware EPROM, real
    // M68705 MCU. Injection goes through the same setHostMouse plumbing.
    MouseCardAppleWin* mouseRaw = nullptr;
    MouseCard*         mouseLle = nullptr;
    if (std::getenv("POM2_PROBE_MOUSE_LLE")) {
        const std::string mcu = findFirst({ "../roms/mouse_341-0269.bin", "roms/mouse_341-0269.bin" });
        if (mcu.empty()) { std::printf("SKIP: no MCU rom\n"); return 0; }
        auto lle = std::make_unique<MouseCard>(mouseSlot);
        if (!lle->loadRoms(mrom, mcu)) { std::printf("LLE mouse ROM load failed\n"); return 1; }
        mouseLle = lle.get();
        mem.slotBus().plug(mouseSlot, std::move(lle));
        std::printf("mouse card (MAME LLE) in slot %d\n", mouseSlot);
    } else {
        auto mouse = std::make_unique<MouseCardAppleWin>(mouseSlot);
        if (!mouse->loadRom(mrom)) { std::printf("mouse ROM load failed\n"); return 1; }
        mouseRaw = mouse.get();
        mem.slotBus().plug(mouseSlot, std::move(mouse));
        std::printf("mouse card (AppleWin HLE) in slot %d\n", mouseSlot);
    }
    auto feedMouse = [&](uint8_t rx, uint8_t ry, bool btn) {
        if (mouseRaw) mouseRaw->setHostMouse(rx, ry, btn);
        else          mouseLle->setHostMouse(rx, ry, btn);
    };

    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());
    disp.setChatMauveCard(chatRaw);
    disp.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);

    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    mem.slotBus().reset();

    // What the 6502 sees in the mouse slot's firmware page — the signature
    // and the assembly entry-point table Extasie reads ($C412+). Dumped
    // before boot and again from snap(), so an EPROM bank mismatch between
    // the LLE and HLE variants shows immediately.
    auto dumpFirmwarePage = [&](const char* when) {
        std::printf("fw $C%X00 page (%s):", mouseSlot, when);
        for (int off : { 0x00, 0x05, 0x07, 0x0B, 0x0C })
            std::printf(" [%02X]=%02X", off,
                        mem.memRead(static_cast<uint16_t>(0xC000 + mouseSlot * 256 + off)));
        std::printf(" | 12..1F:");
        for (int off = 0x12; off <= 0x1F; ++off)
            std::printf(" %02X",
                        mem.memRead(static_cast<uint16_t>(0xC000 + mouseSlot * 256 + off)));
        std::printf("\n");
    };
    dumpFirmwarePage("power-on");

    DevselLog log;
    mem.setWatchSink(&log);
    for (uint32_t a = 0xC080; a <= 0xC0FF; ++a)
        mem.setWriteWatch(static_cast<uint16_t>(a), true);

    constexpr int kSecond = 1'022'727;
    auto runSeconds = [&](double s) {
        const uint64_t target = mem.getCycleCounter() + static_cast<uint64_t>(s * kSecond);
        while (mem.getCycleCounter() < target) cpu.run(2048);
    };
    auto stamp = [&]() { return static_cast<double>(mem.getCycleCounter()) / kSecond; };

    int shot = 0;
    const bool dumpRam = std::getenv("POM2_PROBE_DUMPRAM") != nullptr;
    auto snap = [&](const char* tag) {
        disp.render(mem);
        char name[512];
        std::snprintf(name, sizeof(name), "%s/extasie_%02d_%s.ppm", outDir.c_str(), shot++, tag);
        writePpm(name, disp);
        if (dumpRam) {
            // Both hi-res pages, main and aux — to tell "the unpacker put
            // garbage in RAM" apart from "the renderer mangled good RAM".
            char rn[512];
            std::snprintf(rn, sizeof(rn), "%s.ram", name);
            if (FILE* f = std::fopen(rn, "wb")) {
                std::fwrite(mem.data() + 0x2000, 1, 0x4000, f);
                std::fwrite(mem.auxData() + 0x2000, 1, 0x4000, f);
                std::fclose(f);
            }
            std::printf("      page2=%d 80store=%d\n",
                        mem.getDisplayState().page2, mem.getDisplayState().eightyStore);
        }
        std::printf("t=%6.1fs shot %s (%dx%d) text=%d hires=%d 80col=%d an3off=%d mode=%d PC=$%04X\n",
                    stamp(), name + outDir.size() + 1, disp.width(), disp.height(),
                    mem.getDisplayState().textMode, mem.getDisplayState().hiRes,
                    mem.getDisplayState().eightyCol, mem.getDisplayState().dhgr,
                    static_cast<int>(chatRaw->currentMode()), cpu.getProgramCounter());
    };

    // ProDOS boot + STARTUP. Snapshot every 10 s so the flow is visible.
    for (int i = 0; i < 5; ++i) { runSeconds(10.0); snap("boot"); }
    dumpText(mem);
    log.report();
    dumpFirmwarePage("after boot");

    // Optional keys (menu picks), 5 s apart.
    if (keys) {
        for (const char* k = keys; *k; ++k) {
            char c = *k;
            if (c == ',') c = '\r';
            else if (c == '_') c = 0x0A;   // down arrow
            else if (c == '^') c = 0x0B;   // up arrow
            else if (c == '<') c = 0x08;   // left
            else if (c == '>') c = 0x15;   // right
            else if (c == '~') c = 0x1B;   // esc
            else if (c == '.') {           // no key — just wait
                runSeconds(5.0);
                continue;
            }
            mem.queueKey(static_cast<uint8_t>(c));
            std::printf("t=%6.1fs key '%c'\n", stamp(), c);
            runSeconds(5.0);
            snap("key");
        }
        dumpText(mem);
        log.report();
        dumpFirmwarePage("after keys");
    }

    // Mouse injection: ramp the raw counters (the closed-loop MainWindow
    // drives) and click. If the program reads the mouse at all, the card's
    // internal position moves and — if a cursor exists — the screen changes.
    uint8_t rx = 0, ry = 0;
    for (int burst = 0; burst < 4; ++burst) {
        for (int i = 0; i < 60; ++i) {
            rx = static_cast<uint8_t>(rx + 2);
            ry = static_cast<uint8_t>(ry + 1);
            feedMouse(rx, ry, false);
            runSeconds(0.02);
        }
        feedMouse(rx, ry, true);     // press
        runSeconds(0.3);
        feedMouse(rx, ry, false);    // release
        runSeconds(1.0);
        if (mouseRaw) {
            const auto st = mouseRaw->debugSnapshot();
            std::printf("t=%6.1fs after burst %d: firmware pos=(%d,%d) clamp=[%d..%d]x[%d..%d] mode=$%02X lastCmd=$%02X\n",
                        stamp(), burst, st.iX, st.iY, st.iMinX, st.iMaxX, st.iMinY, st.iMaxY,
                        st.byMode, st.lastCmd);
        } else {
            std::printf("t=%6.1fs after burst %d (LLE card — no HLE snapshot)\n", stamp(), burst);
        }
        snap("mouse");
    }
    dumpText(mem);
    log.report();

    // Tail: one shot every 10 s (slideshows advance on their own).
    double left = runFor > 60 ? runFor - 60 : 5.0;
    while (left > 0) {
        const double step = left > 10 ? 10 : left;
        runSeconds(step);
        left -= step;
        snap("end");
    }
    dumpText(mem);
    log.report();
    return 0;
}
