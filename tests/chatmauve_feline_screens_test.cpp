// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

// Le Chat Mauve FÉLINE — Arlequin and Extasie, the card's own showcase
// titles, run headless and their stable screens frozen (DISPLAY-011).
//
// Same method as `purplesoft_eve_screens` (the Eve's goldens): a //e with
// 128 K, a Disk II and a Le Chat Mauve Féline in slot 7 (ChatMauveRGB
// pipeline), booted from power-on with nothing typed; every screen whose
// framebuffer hash stays unchanged for 2 s of machine time is recorded, in
// order. The timeline is cycle-driven, so the hashes are host-independent.
//   * Arlequin — `disks_5.4/chatmauve/chatmauve-demo-side2-arlequin.po`.
//   * Extasie — `disks_5.4/gist/Extasie disk1.dsk`, with the AppleWin-HLE
//     mouse in slot 4 (Extasie calls its firmware there with no scan).
//
// A golden is only worth pinning if the picture is RIGHT, so recording
// writes each stable screen as a PPM for eyes as well as printing the table:
//   POM2_GOLDEN_RECORD=1 POM2_GOLDEN_DUMP=<dir> build/tests/test_chatmauve_feline_screens
// A change in CPU/disk timing legitimately moves the timeline; regenerate
// then. SKIPs when the ROMs or the disks are absent.

#include "Apple2Display.h"
#include "DiskIICard.h"
#include "LeChatMauveCard.h"
#include "M6502.h"
#include "Memory.h"
#include "MouseCardAppleWin.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string findFirst(std::initializer_list<const char*> rel)
{
    std::error_code ec;
    for (const char* r : rel)
        for (const std::string pre : { "", "../", "../../" })
            if (std::filesystem::is_regular_file(pre + r, ec)) return pre + r;
    return {};
}

uint64_t fnv1a(const void* data, size_t n)
{
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

void dumpPpm(const Apple2Display& d, const std::string& path)
{
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << d.width() << " " << d.height() << "\n255\n";
    const uint32_t* px = d.pixels();
    for (int i = 0; i < d.width() * d.height(); ++i) {
        const uint32_t v = px[i];   // 0xAABBGGRR
        const char rgb[3] = { char(v & 0xFF), char((v >> 8) & 0xFF), char((v >> 16) & 0xFF) };
        f.write(rgb, 3);
    }
}

struct Screen { uint64_t hash; double at; };

// `keys` are typed at `keysAt` seconds; screens are recorded from
// `recordFrom` on (the boot's own screens — the `]` prompt and the like —
// say nothing about the card and would only make the golden fragile).
std::vector<Screen> runTitle(const char* name, const std::string& rom,
                             const std::string& boot, const std::string& dsk,
                             const std::string& mouseRom, double runFor,
                             double recordFrom, const char* keys = nullptr,
                             double keysAt = 0.0)
{
    Memory mem;
    mem.setIIEMode(true);
    if (!mem.loadAppleIIRom(rom.c_str())) { std::fprintf(stderr, "ROM load failed\n"); std::exit(1); }
    auto disk = std::make_unique<DiskIICard>();
    if (!disk->loadBootRom(boot) || !disk->insertDisk(dsk)) {
        std::fprintf(stderr, "disk setup failed: %s\n", dsk.c_str()); std::exit(1);
    }
    mem.slotBus().plug(6, std::move(disk));
    auto chat = std::make_unique<LeChatMauveCard>(7, LeChatMauveCard::Variant::Feline);
    LeChatMauveCard* feline = chat.get();
    feline->setMemory(&mem);
    mem.slotBus().plug(7, std::move(chat));
    if (!mouseRom.empty()) {
        auto mouse = std::make_unique<MouseCardAppleWin>(4);
        if (!mouse->loadRom(mouseRom)) { std::fprintf(stderr, "mouse ROM load failed\n"); std::exit(1); }
        mem.slotBus().plug(4, std::move(mouse));
    }

    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());
    disp.setChatMauveCard(feline);
    disp.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);

    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    mem.slotBus().reset();

    constexpr int kSecond = 1'022'727;
    const char* dumpDir = std::getenv("POM2_GOLDEN_DUMP");
    std::vector<Screen> out;
    uint64_t lastHash = 0;
    int stable = 0;
    bool recorded = true;
    for (double t = 0.0; t < runFor; t += 0.2) {
        if (keys && t >= keysAt) { mem.pasteRawKeys(keys, std::strlen(keys)); keys = nullptr; }
        const uint64_t target = mem.getCycleCounter() + static_cast<uint64_t>(0.2 * kSecond);
        while (mem.getCycleCounter() < target) cpu.run(2048);
        disp.render(mem);
        const uint64_t h = fnv1a(disp.pixels(),
            static_cast<size_t>(disp.width()) * disp.height() * sizeof(uint32_t));
        if (h == lastHash) {
            if (++stable == 10 && !recorded && t >= recordFrom) {   // 2 s unchanged
                out.push_back({ h, t });
                recorded = true;
                if (dumpDir) {
                    char path[512];
                    std::snprintf(path, sizeof path, "%s/%s_%02zu_%04.1fs.ppm",
                                  dumpDir, name, out.size(), t);
                    dumpPpm(disp, path);
                }
            }
        } else {
            stable = 0;
            recorded = false;
            lastHash = h;
        }
    }
    return out;
}

int check(const char* name, const std::vector<Screen>& got,
          const uint64_t* want, size_t nWant, bool record)
{
    if (record) {
        std::printf("const uint64_t k%s[] = {\n", name);
        for (const auto& s : got)
            std::printf("    0x%016llxULL,   // stable at %.1f s\n",
                        static_cast<unsigned long long>(s.hash), s.at);
        std::printf("};\n");
        return 0;
    }
    int bad = 0;
    if (got.size() != nWant) {
        std::fprintf(stderr, "%s: %zu stable screens, expected %zu\n", name, got.size(), nWant);
        ++bad;
    }
    for (size_t i = 0; i < got.size() && i < nWant; ++i)
        if (got[i].hash != want[i]) {
            std::fprintf(stderr, "%s[%zu]: got 0x%016llx, want 0x%016llx\n", name, i,
                         static_cast<unsigned long long>(got[i].hash),
                         static_cast<unsigned long long>(want[i]));
            ++bad;
        }
    if (!bad) std::printf("  %s: %zu screens match\n", name, got.size());
    return bad;
}

// Regenerate with POM2_GOLDEN_RECORD=1 (see the file header) — after
// looking at the dumped screens.
// Arlequin: main menu, the DEMONSTRATION banner and the interpreter's
// boot page (seen twice), a blank between, the cat (DHGR on the Féline),
// then the demo menu in the card's coloured 40 columns.
const uint64_t kArlequin[] = {
    0x0e27885a3c6c8106ULL,   // stable at 18.2 s — MENU PRINCIPAL ARLEQUIN
    0x329c153b66f7d383ULL,   // stable at 25.4 s
    0x2cd432b05c7b199bULL,   // stable at 30.4 s — ARLEQUIN 1.1, INTERPRETEUR GRAPHIQUE
    0x329c153b66f7d383ULL,   // stable at 32.8 s
    0xb7074120c452a383ULL,   // stable at 36.2 s — blank
    0x3d60f4fe56411913ULL,   // stable at 41.2 s — MIAOUU!!!
    0x7c74026b8b358603ULL,   // stable at 62.6 s — MENU DE DEMO
};
// Extasie: the title page, mixed DHGR.
const uint64_t kExtasie[] = {
    0xb774819af186c7cbULL,   // stable at 17.4 s
};

} // namespace

int main()
{
    const std::string rom   = findFirst({ "roms/apple2e.rom" });
    const std::string boot  = findFirst({ "roms/disk2.rom" });
    const std::string mrom  = findFirst({ "roms/mouse_341-0270-c.bin" });
    const std::string arleq = findFirst({ "disks_5.4/chatmauve/chatmauve-demo-side2-arlequin.po" });
    const std::string extas = findFirst({ "disks_5.4/gist/Extasie disk1.dsk" });
    if (rom.empty() || boot.empty() || mrom.empty() || arleq.empty() || extas.empty()) {
        std::printf("chatmauve_feline_screens SKIP: missing ROMs or disks\n");
        return 77;
    }
    const bool record = std::getenv("POM2_GOLDEN_RECORD") != nullptr;
    std::printf("Le Chat Mauve Féline screens:\n");
    int bad = 0;
    // Arlequin: its main menu, then DEMONSTRATION (two cursor-downs, RETURN —
    // raw keys: pasteText would fold the Ctrl-J arrows into RETURNs).
    bad += check("Arlequin", runTitle("arlequin", rom, boot, arleq, {}, 66.0, 15.0,
                                      "\x0A\x0A\r", 22.0),
                 kArlequin, std::size(kArlequin), record);
    // Extasie: its title screen (mixed DHGR, the Féline's own mode).
    bad += check("Extasie", runTitle("extasie", rom, boot, extas, mrom, 20.0, 15.0),
                 kExtasie, std::size(kExtasie), record);
    if (record) return 0;
    if (bad) { std::fprintf(stderr, "chatmauve_feline_screens: %d mismatch(es)\n", bad); return 1; }
    std::printf("chatmauve_feline_screens: OK\n");
    return 0;
}
