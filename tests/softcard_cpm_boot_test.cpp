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

// POM2 — Microsoft CP/M 2.2 end-to-end boot test (SoftCard Phase 3).
//
// Wires the full CP/M hardware stack headlessly — II+ ROM, Disk II in
// slot 6 with a Microsoft SoftCard CP/M boot disk, SoftCardZ80 in slot 4
// — jumps into the Disk II boot PROM at $C600 and runs both CPUs under
// the same DMA arbitration EmulationController uses, until the Z80-side
// CP/M prints its banner and the `A>` prompt on the 40-column text page.
//
// This is the Phase-3 acceptance gate: it proves the 6502 boot loader
// finds the card (write_cnxx probe), the Z80 executes CP/M out of the
// six translated windows, the BIOS talks to the Disk II through the
// $E000 I/O window, and control ping-pongs 6502↔Z80 until a live prompt.
//
// Two registered variants (both gated on user-provided media, ROM-test
// pattern — skip silently when files are absent):
//   softcard_cpm_boot       II+ profile, 40-col console:
//                           roms/apple2p.rom + disks_5.4/dsk/cpm22.dsk
//                           (canonical: "Softcard 16-sector disk
//                           (Microsoft 1980)" 44K v2.20 master)
//   softcard_cpm_boot_iie   argv[2]="iie": //e + IIe paging, for 56K/60K
//                           sysgens whose console uses the IIe 80-col
//                           firmware (even display columns land in AUX):
//                           roms/apple2e.rom + disks_5.4/dsk/cpm60k.dsk
//                           (validated against the MAME apple2ee +
//                           softcard oracle — byte-identical banner).
// argv[1] forces another image path for manual runs.

#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "SoftCardZ80.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

uint16_t textRowBase(int row)
{
    return uint16_t(0x0400 + (row % 8) * 0x80 + (row / 8) * 0x28);
}

// Assemble a display row as text. cols=40 reads the main text page
// only; cols=80 interleaves AUX (even display columns) with main (odd)
// — the IIe 80-column layout the 56K/60K CP/M console drivers use.
void readRow(Memory& mem, int row, int cols, char* out /*cols+1*/)
{
    const uint16_t base = textRowBase(row);
    const uint8_t* aux = mem.auxData();
    for (int c = 0; c < cols; ++c) {
        uint8_t raw;
        if (cols == 80)
            raw = (c & 1) ? mem.memRead(uint16_t(base + c / 2))
                          : aux[base + c / 2];
        else
            raw = mem.memRead(uint16_t(base + c));
        out[c] = char(raw & 0x7F);
    }
    out[cols] = 0;
}

bool rowContains(Memory& mem, int row, int cols, const char* pat)
{
    const int patLen = int(std::strlen(pat));
    char line[81];
    readRow(mem, row, cols, line);
    for (int start = 0; start + patLen <= cols; ++start)
        if (std::memcmp(line + start, pat, size_t(patLen)) == 0) return true;
    return false;
}

void dumpScreen(Memory& mem, int cols)
{
    for (int r = 0; r < 24; ++r) {
        char line[81];
        readRow(mem, r, cols, line);
        for (int c = 0; c < cols; ++c)
            if (!(line[c] >= 0x20 && line[c] < 0x7F)) line[c] = '.';
        std::printf("|%s|\n", line);
    }
}

} // namespace

int main(int argc, char** argv)
{
    // argv[2] == "iie": run as an Apple //e (apple2e.rom + IIe paging) —
    // needed by 56K/60K CP/M sysgens that use the IIe LC + lowercase.
    const bool iie = (argc > 2) && std::string(argv[2]) == "iie";
    const std::string romPath = iie
        ? findFirst({ "roms/apple2e.rom", "../roms/apple2e.rom",
                      "../../roms/apple2e.rom" })
        : findFirst({
        "roms/apple2p.rom", "../roms/apple2p.rom", "../../roms/apple2p.rom",
        "roms/apple2.rom", "../roms/apple2.rom", "../../roms/apple2.rom" });
    const std::string promPath = findFirst({
        "roms/disk2.rom", "../roms/disk2.rom", "../../roms/disk2.rom" });
    std::string dskPath = (argc > 1 && argv[1][0]) ? argv[1] : (iie
        ? findFirst({
            "disks_5.4/dsk/cpm60k.dsk", "../disks_5.4/dsk/cpm60k.dsk",
            "../../disks_5.4/dsk/cpm60k.dsk" })
        : findFirst({
            "disks_5.4/dsk/cpm22.dsk", "../disks_5.4/dsk/cpm22.dsk",
            "../../disks_5.4/dsk/cpm22.dsk" }));

    if (romPath.empty() || promPath.empty() || dskPath.empty()) {
        std::printf("softcard_cpm_boot%s: SKIP (needs roms/%s, "
                    "roms/disk2.rom, disks_5.4/dsk/%s)\n",
                    iie ? "[iie]" : "",
                    iie ? "apple2e.rom" : "apple2p.rom",
                    iie ? "cpm60k.dsk" : "cpm22.dsk");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    Memory mem;
    if (iie)
        mem.setIIEMode(true);
    if (!mem.loadAppleIIRom(romPath.c_str())) {
        std::fprintf(stderr, "loadAppleIIRom(%s) failed\n", romPath.c_str());
        return 1;
    }

    auto disk = std::make_unique<DiskIICard>(6);
    if (!disk->loadBootRom(promPath)) {
        std::fprintf(stderr, "Disk II PROM load failed\n");
        return 1;
    }
    if (!disk->insertDisk(dskPath)) {
        std::fprintf(stderr, "insertDisk(%s) failed: %s\n", dskPath.c_str(),
                     disk->getLastError().c_str());
        return 1;
    }
    mem.slotBus().plug(6, std::move(disk));

    M6502 cpu(&mem);
    mem.setCpu(&cpu);

    auto owned = std::make_unique<SoftCardZ80>();
    SoftCardZ80* card = owned.get();
    card->setMemory(&mem);
    card->setCpu(&cpu);
    mem.slotBus().plug(4, std::move(owned));

    cpu.hardReset();
    mem.slotBus().reset();
    cpu.setProgramCounter(0xC600);

    // Same arbitration shape as EmulationController::runCpuSlice, without
    // the frame pacing — a CP/M cold boot takes ~10 emulated seconds
    // (head recalibration + system track load); give it 60.
    constexpr int64_t kMaxCycles = 60LL * 1'022'727;
    constexpr int     kChunk     = 4096;
    int64_t total = 0;
    bool z80Ran = false;
    bool prompt = false;
    while (total < kMaxCycles) {
        int spent;
        if (card->dmaActive()) {
            z80Ran = true;
            spent = card->dmaRun(kChunk);
        } else {
            spent = cpu.run(kChunk);
        }
        total += (spent > 0 ? spent : kChunk);

        // Poll the text page once per ~frame for the CP/M prompt — in
        // both 40-col (II+ 44K CP/M) and IIe 80-col (56K/60K sysgens)
        // layouts.
        if ((total % 16384) < kChunk) {
            for (int r = 0; r < 24 && !prompt; ++r)
                prompt = rowContains(mem, r, 40, "A>")
                      || (iie && rowContains(mem, r, 80, "A>"));
            if (prompt) break;
        }
    }

    std::printf("softcard_cpm_boot: %s after %lld cycles (Z80 ran: %s)\n",
                prompt ? "A> prompt found" : "NO PROMPT",
                (long long)total, z80Ran ? "yes" : "no");
    dumpScreen(mem, iie ? 80 : 40);

    if (!z80Ran) {
        std::fprintf(stderr, "FAIL: the boot loader never granted the bus "
                             "to the Z80\n");
        return 1;
    }
    if (!prompt) {
        std::fprintf(stderr, "FAIL: no A> prompt within the cycle budget\n");
        return 1;
    }
    std::printf("softcard_cpm_boot: PASS\n");
    return 0;
}
