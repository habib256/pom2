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

// Ultima V "Save Music Configuration" on a WOZ — diagnostic, not a pinned test.
//
// Boots a (copy of the) Ultima V Program disk on a //e with a Mockingboard
// in slot 4, drives the guest through a key script, and reports what the
// game did to the disk: write flushes, dirty tracks, the boot page, and the
// CPU's resting PC. On exit the disk is ejected so the write-back lands in
// the file and the modified track can be decoded offline.
//
//   u5_woz_save_probe <image.woz> [bootSecs] [key tokens...]
//
// Key tokens: esc cr down up left right space sleepN (N emulated seconds)
// or any literal text. Defaults reproduce the GUI sequence that froze.

#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "Mockingboard.h"
#include "ProbeOutDir.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

void runSeconds(M6502& cpu, Memory& mem, double secs, const char* tag)
{
    const uint64_t target = mem.getCycleCounter() +
                            static_cast<uint64_t>(secs * 1'022'727.0);
    uint16_t lastPc = 0; int samePc = 0;
    while (mem.getCycleCounter() < target) {
        cpu.run(8192);
        const uint16_t pc = cpu.getProgramCounter();
        if (pc == lastPc) ++samePc; else { samePc = 0; lastPc = pc; }
    }
    std::printf("[%-8s] t=%.1fs PC=$%04X%s\n", tag,
                mem.getCycleCounter() / 1'022'727.0, cpu.getProgramCounter(),
                samePc > 1000 ? "  <-- PC frozen" : "");
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s image.woz [bootSecs] [keys...]\n", argv[0]); return 1; }
    const std::string dsk = argv[1];
    const int bootSecs = (argc > 2) ? std::atoi(argv[2]) : 40;
    // POM2_U5_DRIVE2=<path> mounts a second image in drive 2. Ultima V's
    // "Save Music Configuration" targets whatever drive its $78 holds, and
    // on a two-drive setup that is drive 2.
    const char* drive2 = std::getenv("POM2_U5_DRIVE2");
    std::vector<std::string> keys;
    for (int i = 3; i < argc; ++i) keys.push_back(argv[i]);
    if (keys.empty())
        keys = {"space", "sleep6", "down", "down", "down", "down", "down", "cr",
                "sleep4", "esc", "sleep3", "down", "down", "cr", "sleep3",
                "right", "right", "right", "C", "cr", "sleep3", "down", "cr",
                "sleep12"};

    Memory mem;
    mem.setIIEMode(true);
    if (!mem.loadAppleIIRom("roms/apple2e.rom")) { std::fprintf(stderr, "ROM\n"); return 1; }

    auto disk = std::make_unique<DiskIICard>();
    if (!disk->loadBootRom("roms/disk2.rom") || !disk->insertDisk(dsk)) {
        std::fprintf(stderr, "Disk II setup failed\n"); return 1;
    }
    disk->loadLssRom("roms/diskii_p6.rom");
    if (drive2 && *drive2 && !disk->insertDisk(1, drive2))
        std::fprintf(stderr, "drive 2 insert failed\n");
    disk->setWriteBackEnabled(true);
    DiskIICard* card = disk.get();
    mem.slotBus().plug(6, std::move(disk));
    mem.slotBus().plug(4, std::make_unique<MockingboardCard>(4));

    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.slotBus().reset();
    cpu.hardReset();
    cpu.setProgramCounter(0xC600);

    std::printf("image WP=%d\n", card->isFileWriteProtected() ? 1 : 0);
    runSeconds(cpu, mem, bootSecs, "boot");

    for (const std::string& k : keys) {
        if (k.rfind("sleep", 0) == 0) { runSeconds(cpu, mem, std::atof(k.c_str() + 5), "sleep"); continue; }
        if (k.rfind("dump", 0) == 0) {
            // dumpXXXX: 128 bytes from hex address XXXX, as currently paged.
            const uint16_t a0 = static_cast<uint16_t>(std::strtol(k.c_str() + 4, nullptr, 16));
            for (int r = 0; r < 128; r += 16) {
                std::printf("    $%04X:", a0 + r);
                for (int c = 0; c < 16; ++c) std::printf(" %02X", mem.memRead(static_cast<uint16_t>(a0 + r + c)));
                std::printf("\n");
            }
            std::printf("    disk: motor=%d drive=%d qt=%d bitLss=%d fileWP=%d flushes=%llu dirty=%d\n",
                        card->isMotorOn() ? 1 : 0, card->getActiveDrive(),
                        card->getQuarterTrack(), card->usingBitLss() ? 1 : 0,
                        card->isFileWriteProtected() ? 1 : 0,
                        (unsigned long long)card->getWriteFlushCount(),
                        card->hasUnsavedChanges() ? 1 : 0);
            std::printf("    regs: A=%02X X=%02X Y=%02X SP=%02X P=%02X PC=%04X\n",
                        cpu.getAccumulator(), cpu.getXRegister(), cpu.getYRegister(),
                        cpu.getStackPointer(), cpu.getStatusRegister(), cpu.getProgramCounter());
            continue;
        }
        if (k.rfind("shot", 0) == 0) {
            // Dump both HGR pages raw (8 KB each) + which one is displayed;
            // a host-side script renders them. Lets the key script be
            // steered by what the game actually shows.
            // Under pom2test::probeOutDir(), not the cwd — which this probe
            // requires to be the repo root (CLI-3, bug hunt 2026-11-01 round
            // 3); a failed open is said, not swallowed.
            const std::string base = pom2test::probeOutDir() + "/" + k.substr(4);
            const uint8_t* ram = mem.data();
            for (int pg = 1; pg <= 2; ++pg) {
                const std::string path = base + ".hgr" + std::to_string(pg);
                FILE* f = std::fopen(path.c_str(), "wb");
                if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
                std::fwrite(ram + (pg == 1 ? 0x2000 : 0x4000), 1, 0x2000, f);
                std::fclose(f);
            }
            std::printf("    shot %s: page2=%d hires=%d text=%d\n", base.c_str(),
                        mem.getDisplayState().page2 ? 1 : 0,
                        mem.getDisplayState().hiRes ? 1 : 0,
                        mem.getDisplayState().textMode ? 1 : 0);
            continue;
        }
        std::string s = k;
        if      (k == "esc")   s = "\x1b";
        else if (k == "cr")    s = "\r";
        else if (k == "down")  s = "\n";
        else if (k == "up")    s = "\x0b";
        else if (k == "left")  s = "\x08";
        else if (k == "right") s = "\x15";
        else if (k == "space") s = " ";
        mem.pasteRawKeys(s.data(), s.size());
        const uint64_t flushesBefore = card->getWriteFlushCount();
        runSeconds(cpu, mem, 1.5, k.c_str());
        if (card->getWriteFlushCount() != flushesBefore)
            std::printf("    write flushes: %llu -> %llu, head qt=%d\n",
                        (unsigned long long)flushesBefore,
                        (unsigned long long)card->getWriteFlushCount(),
                        card->getQuarterTrack());
    }

    std::printf("\nfinal: PC=$%04X flushes=%llu dirty=%d qt=%d\n",
                cpu.getProgramCounter(),
                (unsigned long long)card->getWriteFlushCount(),
                card->hasUnsavedChanges() ? 1 : 0, card->getQuarterTrack());
    const uint8_t* ram = mem.data();
    std::printf("$0800 page:\n");
    for (int r = 0; r < 256; r += 16) {
        std::printf("  $%04X:", 0x800 + r);
        for (int c = 0; c < 16; ++c) std::printf(" %02X", ram[0x800 + r + c]);
        std::printf("\n");
    }
    card->ejectDisk();   // flushes the write-back into the file
    std::printf("ejected (write-back committed)\n");
    return 0;
}
