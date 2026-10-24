// DOS 3.3 INIT smoke test — can POM2 format a disk?
//
// Bug hunt #7 found that with the shipped `roms/diskii_p6.rom` (the
// released emulator's default) `INIT HELLO,D2` answered I/O ERROR on every
// image: the revolution anchor a burst is reduced against was last set at
// motor-on, tens of revolutions back, and a non-WOZ track's period is its
// PADDED cell count, which every sync run written moves. The data field of
// sector 0 therefore landed ~42 nibbles before the address field it had
// just written, format verify failed, and RWTS retried track 0 forever. The
// legacy 32-cycle gate (no P6 ROM) formatted fine, which is why no boot or
// SAVE test saw it: nothing writes a whole track but INIT.
//
// Boots the DOS 3.3 master in drive 1, INITs a blank image in drive 2, then
// CATALOGs it and round-trips a SAVE / LOAD through the freshly formatted
// disk. Skips (77) without apple2.rom / disk2.rom / diskii_p6.rom / the
// master image.
//
// `woz` (STORAGE-012): the blank is a WOZ2 that carries track 0 only, every
// other TMAP entry $FF — how Applesauce images a half-used disk. The WOZ
// write path used to drop writes to an absent track without a word, so the
// format's verify failed on track 1 and the disk could never be INITed.

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

bool fileExists(const std::string& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

std::string findFirst(std::initializer_list<const char*> candidates) {
    for (const char* c : candidates) if (fileExists(c)) return c;
    return {};
}

bool copyFile(const std::string& src, const fs::path& dst) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << in.rdbuf();
    return static_cast<bool>(out);
}

// Apple II text page 1 layout: row Y at base $0400 + 0x80*(Y%8) + 0x28*(Y/8).
// We strip bit-7 and map control chars to ' ' so a substring search works.
std::string scrapeTextPage(const uint8_t* ram) {
    std::string out;
    out.reserve(24 * 41);
    for (int row = 0; row < 24; ++row) {
        const int base = 0x0400 + 0x80 * (row % 8) + 0x28 * (row / 8);
        for (int col = 0; col < 40; ++col) {
            const char c = static_cast<char>(ram[base + col] & 0x7F);
            out.push_back((c >= 0x20 && c < 0x7F) ? c : ' ');
        }
        out.push_back('\n');
    }
    return out;
}

void runFor(M6502& cpu, long long cycles) {
    long long n = 0;
    while (n < cycles) n += cpu.run(1024);
}

// Run until `needle` appears in the text page or `maxCycles` elapse.
bool waitForText(M6502& cpu, const uint8_t* ram, const char* needle,
                 long long maxCycles, long long minCycles = 0) {
    long long total = 0;
    while (total < maxCycles) {
        const int slice = cpu.run(8192);
        total += slice;
        if (total >= minCycles && scrapeTextPage(ram).find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    const bool woz = argc > 1 && std::strcmp(argv[1], "woz") == 0;
    const std::string romPath  = findFirst({
        "../roms/apple2.rom", "roms/apple2.rom", "../../roms/apple2.rom" });
    const std::string promPath = findFirst({
        "../roms/disk2.rom", "roms/disk2.rom", "../../roms/disk2.rom" });
    const std::string p6Path = findFirst({
        "../roms/diskii_p6.rom", "roms/diskii_p6.rom", "../../roms/diskii_p6.rom" });
    const std::string masterPath = findFirst({
        "../disks_5.4/dsk/dos33_master.dsk", "disks_5.4/dsk/dos33_master.dsk",
        "../../disks_5.4/dsk/dos33_master.dsk" });
    if (romPath.empty() || promPath.empty() || p6Path.empty() || masterPath.empty()) {
        std::printf("diskii_format_smoke SKIP: missing ROM or disk\n");
        return 77;
    }

    const fs::path scratchDir = fs::temp_directory_path() / "pom2_diskii_format";
    std::error_code ec;
    fs::create_directories(scratchDir, ec);
    const fs::path master = scratchDir / "master.dsk";
    const fs::path blank  = scratchDir / (woz ? "blank.woz" : "blank.dsk");
    if (!copyFile(masterPath, master)) { std::fprintf(stderr, "cannot copy master\n"); return 1; }
    if (woz) {
        // WOZ2: INFO v2 (4 µs cells), TMAP with only qt 0, TRKS header 0 at
        // block 3 for 13 blocks of 50 000 blank cells.
        std::vector<uint8_t> w = {'W','O','Z','2',0xFF,0x0A,0x0D,0x0A,0,0,0,0};
        auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) w.push_back(uint8_t(v >> (8 * i))); };
        auto u16 = [&](uint16_t v) { w.push_back(uint8_t(v)); w.push_back(uint8_t(v >> 8)); };
        w.insert(w.end(), {'I','N','F','O'}); u32(60);
        std::vector<uint8_t> info(60, 0);
        info[0] = 2; info[1] = 1; info[37] = 1; info[39] = 32; info[44] = 13;
        for (int i = 5; i < 37; ++i) info[size_t(i)] = ' ';
        w.insert(w.end(), info.begin(), info.end());
        w.insert(w.end(), {'T','M','A','P'}); u32(160);
        w.push_back(0); w.insert(w.end(), 159, 0xFF);
        w.insert(w.end(), {'T','R','K','S'}); u32(160 * 8 + 13 * 512);
        u16(3); u16(13); u32(50000);
        w.resize(1536, 0);
        w.resize(1536 + 13 * 512, 0);
        std::ofstream f(blank, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(w.data()), static_cast<std::streamsize>(w.size()));
        if (!f) { std::fprintf(stderr, "cannot write blank WOZ\n"); return 1; }
    } else {
        std::ofstream f(blank, std::ios::binary | std::ios::trunc);
        std::vector<char> zeros(143360, 0);
        f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
        if (!f) { std::fprintf(stderr, "cannot write blank image\n"); return 1; }
    }

    Memory mem;
    if (!mem.loadAppleIIRom(romPath.c_str())) { std::fprintf(stderr, "ROM load failed\n"); return 1; }
    auto card = std::make_unique<DiskIICard>();
    if (!card->loadBootRom(promPath))     { std::fprintf(stderr, "PROM load failed\n"); return 1; }
    if (!card->loadLssRom(p6Path))        { std::fprintf(stderr, "P6 load failed\n");   return 1; }
    if (!card->insertDisk(master.string()))   { std::fprintf(stderr, "insert D1 failed\n"); return 1; }
    if (!card->insertDisk(1, blank.string())) { std::fprintf(stderr, "insert D2 failed\n"); return 1; }
    card->setWriteBackEnabled(true);
    DiskIICard* cardRaw = card.get();
    mem.slotBus().plug(6, std::move(card));

    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.slotBus().reset();
    cpu.hardReset();
    cpu.setProgramCounter(0xC600);
    const uint8_t* ram = mem.data();

    if (!waitForText(cpu, ram, "\n]", 250'000'000, /*minCycles=*/10'000'000)) {
        std::fprintf(stderr, "FAIL: BASIC prompt never appeared\n%s\n",
                     scrapeTextPage(ram).c_str());
        return 2;
    }
    runFor(cpu, 40'000'000);

    // INIT the blank disk, with the program in memory becoming HELLO. The
    // arithmetic marker prints only once INIT has returned to the prompt
    // (the paste queue is drained by the keyboard reads), and its result is
    // a string that the typed line itself does not contain.
    {
        const char* prog =
            "NEW\r"
            "10  PRINT \"HI\"\r"
            "INIT HELLO,D2\r"
            "PRINT 1234+1\r";
        mem.pasteText(prog, std::strlen(prog));
    }
    if (!waitForText(cpu, ram, "1235", 4'000'000'000LL)) {
        std::fprintf(stderr, "FAIL: INIT never came back to the prompt\n%s\n",
                     scrapeTextPage(ram).c_str());
        return 3;
    }
    const std::string afterInit = scrapeTextPage(ram);
    if (afterInit.find("I/O ERROR") != std::string::npos) {
        std::fprintf(stderr,
            "FAIL: INIT answered I/O ERROR (the regression) after %llu "
            "write flushes, head at half-track %d\n%s\n",
            static_cast<unsigned long long>(cardRaw->getWriteFlushCount()),
            cardRaw->getHalfTrack(), afterInit.c_str());
        return 4;
    }
    // A one-track-only format stops near 1 000 flushes; a full 35-track
    // INIT runs past 30 000.
    if (cardRaw->getWriteFlushCount() < 30'000) {
        std::fprintf(stderr, "FAIL: INIT wrote only %llu flushes, not a whole disk\n",
                     static_cast<unsigned long long>(cardRaw->getWriteFlushCount()));
        return 5;
    }
    std::printf("INIT OK: %llu nibble flushes\n",
                static_cast<unsigned long long>(cardRaw->getWriteFlushCount()));

    {
        const char* prog = "CATALOG,D2\rPRINT 1234+2\r";
        mem.pasteText(prog, std::strlen(prog));
    }
    if (!waitForText(cpu, ram, "1236", 400'000'000)) {
        std::fprintf(stderr, "FAIL: CATALOG,D2 never returned\n%s\n", scrapeTextPage(ram).c_str());
        return 6;
    }
    const std::string afterCatalog = scrapeTextPage(ram);
    if (afterCatalog.find("I/O ERROR") != std::string::npos ||
        afterCatalog.find("HELLO") == std::string::npos) {
        std::fprintf(stderr, "FAIL: the formatted disk does not CATALOG a HELLO\n%s\n",
                     afterCatalog.c_str());
        return 7;
    }

    {
        const char* prog =
            "NEW\r"
            "10  PRINT \"ROUND\"+\"TRIP\"\r"
            "SAVE FOO,D2\r"
            "NEW\r"
            "LOAD FOO,D2\r"
            "LIST\r"
            "PRINT 1234+3\r";
        mem.pasteText(prog, std::strlen(prog));
    }
    if (!waitForText(cpu, ram, "1237", 400'000'000)) {
        std::fprintf(stderr, "FAIL: SAVE/LOAD never returned\n%s\n", scrapeTextPage(ram).c_str());
        return 8;
    }
    const std::string afterList = scrapeTextPage(ram);
    const size_t listAt = afterList.rfind("LIST");
    if (afterList.find("I/O ERROR") != std::string::npos || listAt == std::string::npos ||
        afterList.find("PRINT \"ROUND\"", listAt) == std::string::npos) {
        std::fprintf(stderr, "FAIL: SAVE/LOAD on the formatted disk did not round-trip\n%s\n",
                     afterList.c_str());
        return 9;
    }
    std::printf("diskii_format_smoke OK\n");
    return 0;
}
