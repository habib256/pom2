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

// An UNFORMATTED diskette — the one a format has to create rather than
// overwrite.
//
// A zero-filled .dsk is not one: every loader nibblizes on insert, so
// 143 360 bytes of zeros mount as a perfectly formatted disk whose 560
// sectors happen to hold zeros, and RWTS reads every one of them. That is
// why `diskii_format_smoke` proves only that POM2 can RE-format. A diskette
// out of its wrapper has no address fields at all, and the question this
// test answers is whether POM2 models their ABSENCE — and their creation.
//
// Three claims, in the order a user meets them:
//   1. a blank surface is unreadable — CATALOG,D2 answers I/O ERROR,
//      because there is no address field for RWTS to find. It must ANSWER,
//      not hang: a drive that never returns is a worse lie than a wrong
//      answer, and it is what the bug below did;
//   2. INIT formats it anyway: the write path lays the address and data
//      fields down on virgin surface (DiskImage::writeFlux), not over an
//      existing layout;
//   3. the result is a real disk — CATALOG lists the greeting program and
//      a SAVE round-trips through it.
//
// Run twice, once per READ GATE. POM2 ships `roms/diskii_p6.rom`, so a real
// machine always takes the bit-level LSS path — and that one has served
// read-amplifier noise over flux-less surface since 2026-09-07
// (`advanceNoise`, plus the weak-zone train in `getNextTransition`). A
// harness that loads only the boot PROM gets the LEGACY 32-cycle nibble
// gate, which had no such rule: an erased slot is `$00`, no legal GCR byte
// is, so `LDA $C08C,X / BPL -3` waited for a bit 7 that never came, RWTS
// never reached its retry counter, and a blank disk FROZE the guest instead
// of answering I/O ERROR. Reported by a2filecmd against
// `bench/mini33_format`, which loads the boot PROM and no P6 (2026-09-16).
//
// Skips (77) without apple2.rom / disk2.rom / diskii_p6.rom / the DOS 3.3
// master image.

#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string findFirst(std::initializer_list<const char*> candidates) {
    std::error_code ec;
    for (const char* c : candidates)
        if (fs::is_regular_file(c, ec)) return c;
    return {};
}

// Apple II text page 1: row Y at $0400 + 0x80*(Y%8) + 0x28*(Y/8).
std::string scrapeTextPage(const uint8_t* ram) {
    std::string out;
    for (int row = 0; row < 24; ++row) {
        const int base = 0x0400 + 0x80 * (row % 8) + 0x28 * (row / 8);
        for (int col = 0; col < 40; ++col) {
            uint8_t c = ram[base + col] & 0x7F;
            out += (c < 0x20) ? ' ' : static_cast<char>(c);
        }
        out += '\n';
    }
    return out;
}

void runCycles(M6502& cpu, long cycles) {
    for (long n = 0; n < cycles;) n += cpu.run(1024);
}

struct Machine {
    Memory                      mem;
    std::unique_ptr<M6502>      cpu;
    DiskIICard*                 card = nullptr;
};

// Type `text`, then run until `want` shows up on screen or the budget is
// spent. Returns whether it appeared.
bool typeUntil(Machine& m, const char* text, const char* want, int slices) {
    m.mem.pasteRawKeys(text, std::strlen(text));
    for (int i = 0; i < slices; ++i) {
        runCycles(*m.cpu, 1000000);
        if (scrapeTextPage(m.mem.data()).find(want) != std::string::npos)
            return true;
    }
    return false;
}

int fail(Machine& m, const char* gate, const char* what) {
    std::fprintf(stderr, "FAIL [%s gate]: %s\n%s\n", gate, what,
                 scrapeTextPage(m.mem.data()).c_str());
    return 1;
}

// One full pass. `p6Path` empty = the legacy 32-cycle nibble gate.
int runScenario(const std::string& romPath, const std::string& promPath,
                const std::string& p6Path, const fs::path& master,
                const fs::path& fresh, const char* gate)
{
    {
        // A fresh backing file each pass — the previous one has a DOS on it.
        std::error_code ec;
        fs::remove(fresh, ec);
        std::ofstream f(fresh, std::ios::binary | std::ios::trunc);
        const std::vector<char> zeros(DiskImage::kBytesPerImage, 0);
        f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
        if (!f) { std::fprintf(stderr, "cannot write the blank image\n"); return 1; }
    }

    Machine m;
    m.mem.setIIEMode(false);
    if (!m.mem.loadAppleIIRom(romPath.c_str())) { std::fprintf(stderr, "ROM load failed\n"); return 1; }
    auto card = std::make_unique<DiskIICard>();
    if (!card->loadBootRom(promPath)) { std::fprintf(stderr, "PROM load failed\n"); return 1; }
    // The only difference between the two passes.
    if (!p6Path.empty() && !card->loadLssRom(p6Path)) {
        std::fprintf(stderr, "P6 load failed\n");
        return 1;
    }
    if (!card->insertDisk(0, master.string())) { std::fprintf(stderr, "insert D1 failed\n"); return 1; }
    if (!card->insertBlankDisk(1, fresh.string())) {
        std::fprintf(stderr, "FAIL [%s gate]: the drive refused an unformatted diskette\n", gate);
        return 1;
    }
    if (!card->driveImage(1).isSurfaceBlank()) {
        std::fprintf(stderr, "FAIL [%s gate]: the mounted surface is not blank\n", gate);
        return 1;
    }
    card->setWriteBackEnabled(true);
    m.card = card.get();
    m.mem.slotBus().plug(6, std::move(card));

    m.cpu = std::make_unique<M6502>(&m.mem);
    m.mem.setCpu(m.cpu.get());
    m.cpu->setCpuMode(M6502::CpuMode::NMOS);
    m.mem.clearRam();
    m.mem.resetSoftSwitches();
    m.mem.slotBus().reset();
    m.cpu->hardReset();
    m.cpu->setProgramCounter(0xC600);

    bool booted = false;
    for (int i = 0; i < 400 && !booted; ++i) {
        runCycles(*m.cpu, 1000000);
        booted = scrapeTextPage(m.mem.data()).find("DOS VERSION 3.3") != std::string::npos;
    }
    if (!booted) return fail(m, gate, "the DOS 3.3 master never booted");

    // 1. No address fields — nothing to read, and RWTS must SAY SO. The
    //    budget is deliberately generous: the bug this pins did not answer
    //    late, it never answered at all.
    if (!typeUntil(m, "CATALOG,D2\r", "I/O ERROR", 400))
        return fail(m, gate,
                    "an unformatted diskette did not answer I/O ERROR "
                    "(the drive hung on a bit 7 that never came)");

    // 2. Format it. Every address and data field here is written onto virgin
    //    surface through writeFlux — there is no previous layout to overwrite.
    m.mem.pasteRawKeys("INIT HELLO,D2\r", 14);
    for (int i = 0; i < 1500; ++i) runCycles(*m.cpu, 1000000);
    {
        const std::string s = scrapeTextPage(m.mem.data());
        const auto initAt = s.find("INIT HELLO,D2");
        if (initAt == std::string::npos) return fail(m, gate, "INIT never echoed");
        if (s.find("I/O ERROR", initAt) != std::string::npos)
            return fail(m, gate, "INIT could not format an unformatted diskette");
    }
    if (m.card->driveImage(1).isSurfaceBlank())
        return fail(m, gate, "INIT reported success and wrote nothing");

    // 3. It is a real disk now.
    if (!typeUntil(m, "CATALOG,D2\r", "HELLO", 400))
        return fail(m, gate, "the freshly formatted disk does not CATALOG its greeting");
    if (!typeUntil(m, "SAVE POM2,D2\r", "]", 300))
        return fail(m, gate, "SAVE onto the freshly formatted disk never returned");
    if (!typeUntil(m, "CATALOG,D2\r", "POM2", 400))
        return fail(m, gate, "the saved program is not in the catalog");

    // And the format reaches the file: write-back decodes the tracks the
    // guest laid down into an ordinary image.
    if (!m.card->flushPendingWrites())
        return fail(m, gate, "write-back of the formatted disk failed");
    {
        std::ifstream f(fresh, std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                          std::istreambuf_iterator<char>());
        if (bytes.size() != static_cast<size_t>(DiskImage::kBytesPerImage)) {
            std::fprintf(stderr, "FAIL [%s gate]: the written image is %zu bytes\n",
                         gate, bytes.size());
            return 1;
        }
        // Track 17 sector 0 is the VTOC: DOS 3.3 writes its own catalog
        // track number, first free track and volume there.
        const uint8_t* vtoc = bytes.data() + (17 * 16 * 256);
        if (vtoc[0x01] != 17 || vtoc[0x06] != 254 || vtoc[0x27] != 122 ||
            vtoc[0x34] != 35 || vtoc[0x35] != 16) {
            std::fprintf(stderr,
                "FAIL [%s gate]: the saved image has no DOS 3.3 VTOC "
                "(catalog=%u volume=%u pairs=%u tracks=%u sectors=%u)\n",
                gate, vtoc[0x01], vtoc[0x06], vtoc[0x27], vtoc[0x34], vtoc[0x35]);
            return 1;
        }
    }
    std::printf("  %s gate: blank surface answered I/O ERROR, INIT created the "
                "address fields, the result is a DOS 3.3 disk\n", gate);
    return 0;
}

}  // namespace

int main()
{
    const std::string romPath = findFirst({
        "../roms/apple2.rom", "roms/apple2.rom", "../../roms/apple2.rom" });
    const std::string promPath = findFirst({
        "../roms/disk2.rom", "roms/disk2.rom", "../../roms/disk2.rom" });
    const std::string p6Path = findFirst({
        "../roms/diskii_p6.rom", "roms/diskii_p6.rom", "../../roms/diskii_p6.rom" });
    const std::string masterPath = findFirst({
        "../disks_5.4/dsk/dos33_master.dsk", "disks_5.4/dsk/dos33_master.dsk",
        "../../disks_5.4/dsk/dos33_master.dsk" });
    if (romPath.empty() || promPath.empty() || p6Path.empty() || masterPath.empty()) {
        std::printf("diskii_unformatted_disk SKIP: missing ROM or disk\n");
        return 77;
    }

    std::error_code ec;
    const fs::path scratch = fs::temp_directory_path() / "pom2_diskii_unformatted";
    fs::create_directories(scratch, ec);
    const fs::path master = scratch / "master.dsk";
    const fs::path fresh  = scratch / "fresh.dsk";
    fs::copy_file(masterPath, master, fs::copy_options::overwrite_existing, ec);
    if (ec) { std::fprintf(stderr, "cannot copy the master: %s\n", ec.message().c_str()); return 1; }

    // The bit-level LSS first (what a real POM2 runs), then the legacy
    // 32-cycle gate (what a harness without roms/diskii_p6.rom runs).
    if (runScenario(romPath, promPath, p6Path, master, fresh, "LSS")    != 0) return 1;
    if (runScenario(romPath, promPath, {},     master, fresh, "legacy") != 0) return 1;

    // ── The file the UI and the CLI both go through ──────────────────
    // DiskImage::createBlankFile makes the BACKING file; it must never land
    // on one that already exists. "New blank disk" overwriting somebody's
    // only copy of a game has no undo, so the refusal is the feature.
    {
        const fs::path made = scratch / "made.dsk";
        fs::remove(made, ec);
        std::string err;
        if (!DiskImage::createBlankFile(made.string(), err)) {
            std::fprintf(stderr, "FAIL: createBlankFile: %s\n", err.c_str());
            return 1;
        }
        if (fs::file_size(made, ec) != static_cast<uintmax_t>(DiskImage::kBytesPerImage)) {
            std::fprintf(stderr, "FAIL: the created image is the wrong size\n");
            return 1;
        }
        // Put something recognisable in it, ask again, and check both that it
        // is refused and that the bytes survived.
        {
            std::ofstream f(made, std::ios::binary | std::ios::trunc);
            f << "NOT A BLANK DISK";
        }
        if (DiskImage::createBlankFile(made.string(), err)) {
            std::fprintf(stderr, "FAIL: createBlankFile overwrote an existing file\n");
            return 1;
        }
        std::ifstream back(made, std::ios::binary);
        std::string kept((std::istreambuf_iterator<char>(back)),
                          std::istreambuf_iterator<char>());
        if (kept != "NOT A BLANK DISK") {
            std::fprintf(stderr, "FAIL: the refused create damaged the file\n");
            return 1;
        }
    }

    // ── The two-phase shape mountBlankDiskII uses ────────────────────
    // prepareDisk (unlocked) → eraseSurface → install. The erase belongs
    // between the phases, and a medium that refuses it must be caught there
    // rather than installed as a formatted disk.
    {
        const fs::path virgin = scratch / "virgin.dsk";
        fs::remove(virgin, ec);
        std::string err;
        if (!DiskImage::createBlankFile(virgin.string(), err)) {
            std::fprintf(stderr, "FAIL: createBlankFile (virgin): %s\n", err.c_str());
            return 1;
        }
        DiskImage prepared;
        if (!DiskIICard::prepareDisk(virgin.string(), true, prepared, err)) {
            std::fprintf(stderr, "FAIL: prepareDisk: %s\n", err.c_str());
            return 1;
        }
        if (prepared.isSurfaceBlank()) {
            std::fprintf(stderr,
                "FAIL: a freshly prepared image is already blank — then the "
                "scenarios above prove nothing\n");
            return 1;
        }
        prepared.eraseSurface();
        if (!prepared.isSurfaceBlank()) {
            std::fprintf(stderr, "FAIL: eraseSurface left flux on the surface\n");
            return 1;
        }
        // The notch inhibits the erase current, as it does every other write.
        DiskImage locked;
        if (!DiskIICard::prepareDisk(virgin.string(), true, locked, err)) {
            std::fprintf(stderr, "FAIL: prepareDisk (locked): %s\n", err.c_str());
            return 1;
        }
        locked.setHostWriteProtected(true);
        locked.eraseSurface();
        if (locked.isSurfaceBlank()) {
            std::fprintf(stderr, "FAIL: a write-protected medium was erased anyway\n");
            return 1;
        }
    }

    std::printf("diskii_unformatted_disk OK: both read gates, and the "
                "blank-disk file helper refuses to overwrite\n");
    return 0;
}
