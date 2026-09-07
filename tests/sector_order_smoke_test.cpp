// A 5.25" image in EVERY sector order the loader accepts, with the real
// operating systems as the oracle — ProDOS 8 2.4.3 and the DOS 3.3 master.
//
// `prodos_save_smoke` pins the write pipeline on a `.po`. This pins the two
// things AROUND it that a `.po` never exercises: the DOS-3.3 ↔ ProDOS sector
// skew applied to READS and WRITES of a DOS-order image, and the vol-dir
// content sniff that overrides a misleading extension (`DiskImage.cpp`,
// "143 360-byte image"). A wrong skew on the write side is the failure that
// costs a user their disk: the boot reads through the same table and looks
// fine, and the sectors ProDOS writes land at the wrong file offsets.
//
// Four images carry the same ProDOS 2.4.3 volume: the tracked `.po`, its
// DOS-order twin (converted here through the CiderPress table), and each of
// those under the OTHER extension. ProDOS boots every one, DELETEs a file,
// SAVEs a program, and the image is written back. De-skewed to logical
// blocks, the four results must be byte-identical: the same ProDOS did the
// same work on the same volume, so the only way they can differ is a skew or
// sniff mistake in one of the paths.

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

constexpr std::size_t kImageBytes = 143360;

std::string findFirst(std::initializer_list<const char*> candidates)
{
    for (const char* c : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(c, ec)) return c;
    }
    return {};
}

std::vector<uint8_t> readAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

void writeAll(const fs::path& p, const std::vector<uint8_t>& d)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(d.data()),
              static_cast<std::streamsize>(d.size()));
}

// ProDOS block half (2*(block%8)+half) → DOS 3.3 logical sector. The
// CiderPress / apple2js table; sectors 0 and 15 are fixed points.
constexpr uint8_t kProDOSToDos[16] = { 0x0, 0xE, 0xD, 0xC, 0xB, 0xA, 0x9, 0x8,
                                       0x7, 0x6, 0x5, 0x4, 0x3, 0x2, 0x1, 0xF };

std::vector<uint8_t> reorder(const std::vector<uint8_t>& in, bool poToDsk)
{
    std::vector<uint8_t> out(in.size());
    for (unsigned blk = 0; blk < 280; ++blk)
        for (unsigned half = 0; half < 2; ++half) {
            const unsigned track = blk / 8;
            const unsigned ds    = kProDOSToDos[2 * (blk % 8) + half];
            const std::size_t po = blk * 512 + half * 256;
            const std::size_t dk = track * 4096 + ds * 256;
            if (poToDsk) std::memcpy(&out[dk], &in[po], 256);
            else         std::memcpy(&out[po], &in[dk], 256);
        }
    return out;
}

std::string scrapeTextPage(const uint8_t* ram)
{
    std::string out;
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

void runFor(M6502& cpu, long cycles)
{
    long n = 0;
    while (n < cycles) n += cpu.run(1024);
}

bool waitForText(M6502& cpu, const uint8_t* ram, const char* needle,
                 long maxCycles, long minCycles)
{
    long total = 0;
    while (total < maxCycles) {
        total += cpu.run(8192);
        if (total >= minCycles &&
            scrapeTextPage(ram).find(needle) != std::string::npos)
            return true;
    }
    return false;
}

// Boot `image` and wait for a BASIC prompt (DOS 3.3 master).
bool bootsToPrompt(const std::string& rom, const std::string& prom,
                   const fs::path& image, std::string& why)
{
    Memory mem;
    if (!mem.loadAppleIIRom(rom.c_str())) { why = "ROM"; return false; }
    auto card = std::make_unique<DiskIICard>();
    if (!card->loadBootRom(prom))          { why = "PROM"; return false; }
    if (!card->insertDisk(image.string())) { why = "insertDisk refused " + image.string(); return false; }
    mem.slotBus().plug(6, std::move(card));
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.slotBus().reset();
    cpu.hardReset();
    cpu.setProgramCounter(0xC600);
    if (!waitForText(cpu, mem.data(), "\n]", 60'000'000L, 5'000'000L)) {
        why = "no BASIC prompt:\n" + scrapeTextPage(mem.data());
        return false;
    }
    return true;
}

// Boot `image`, DELETE README, SAVE POMTEST, eject (write-back). Returns
// false with `why` on any visible failure.
bool bootAndSave(const std::string& rom, const std::string& prom,
                 const fs::path& image, std::string& why)
{
    Memory mem;
    if (!mem.loadAppleIIRom(rom.c_str())) { why = "ROM"; return false; }
    auto card = std::make_unique<DiskIICard>();
    if (!card->loadBootRom(prom))                 { why = "PROM"; return false; }
    if (!card->insertDisk(image.string()))        { why = "insertDisk refused " + image.string(); return false; }
    card->setWriteBackEnabled(true);
    DiskIICard* raw = card.get();
    mem.slotBus().plug(6, std::move(card));

    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.slotBus().reset();
    cpu.hardReset();
    cpu.setProgramCounter(0xC600);
    const uint8_t* ram = mem.data();

    if (!waitForText(cpu, ram, "BITSY  BYE", 180'000'000L, 10'000'000L)) {
        why = "did not boot to BITSY BYE:\n" + scrapeTextPage(ram);
        return false;
    }
    runFor(cpu, 30'000'000L);
    auto press = [&](char k, long wait) { mem.pasteRawKeys(&k, 1); runFor(cpu, wait); };
    press(0x0A, 5'000'000); press(0x0A, 5'000'000); press(0x0A, 5'000'000);
    press(0x0D, 90'000'000);
    if (scrapeTextPage(ram).find("\n]") == std::string::npos) {
        why = "no BASIC prompt:\n" + scrapeTextPage(ram);
        return false;
    }
    const char* prog = "DELETE README\rNEW\r10 PRINT \"HI\"\r20 PRINT \"BYE\"\r"
                       "SAVE POMTEST\rCAT\r";
    mem.pasteText(prog, std::strlen(prog));
    runFor(cpu, 120'000'000L);
    const std::string screen = scrapeTextPage(ram);
    if (screen.find("POMTEST") == std::string::npos ||
        screen.find("ERROR") != std::string::npos ||
        screen.find("DISK FULL") != std::string::npos) {
        why = "SAVE did not land:\n" + screen;
        return false;
    }
    if (!raw->hasUnsavedChanges()) { why = "SAVE touched nothing"; return false; }
    if (!raw->ejectDisk(0))        { why = "eject / write-back failed"; return false; }
    return true;
}

} // namespace

int main()
{
    const std::string rom  = findFirst({ "roms/apple2.rom", "../roms/apple2.rom",
                                         "../../roms/apple2.rom" });
    const std::string prom = findFirst({ "roms/disk2.rom", "../roms/disk2.rom",
                                         "../../roms/disk2.rom" });
    const std::string master = findFirst({
        "disks_5.4/dsk/ProDOS_2_4_3.po", "../disks_5.4/dsk/ProDOS_2_4_3.po",
        "../../disks_5.4/dsk/ProDOS_2_4_3.po" });
    if (rom.empty() || prom.empty() || master.empty()) {
        std::printf("sector_order_smoke SKIP: missing ROM or disk\n");
        return 77;
    }
    const std::vector<uint8_t> po = readAll(master);
    if (po.size() != kImageBytes) { std::printf("FAIL: master is not 143 360 bytes\n"); return 1; }
    const std::vector<uint8_t> dsk = reorder(po, /*poToDsk=*/true);
    if (reorder(dsk, false) != po) { std::printf("FAIL: skew table is not an involution\n"); return 1; }

    struct Case { const char* leaf; const std::vector<uint8_t>* bytes; bool dosOrder; };
    const Case cases[] = {
        { "pom2_pso_ref.po",          &po,  false },  // the tracked image
        { "pom2_pso_dosorder.dsk",    &dsk, true  },  // honest DOS-order twin
        { "pom2_pso_po_named_dsk.dsk", &po, false },  // sniff must override to ProDOS
        { "pom2_pso_dsk_named_po.po", &dsk, true  },  // sniff must override to DOS
    };
    std::vector<uint8_t> reference;
    for (const Case& c : cases) {
        const fs::path path = fs::temp_directory_path() / c.leaf;
        writeAll(path, *c.bytes);
        std::string why;
        if (!bootAndSave(rom, prom, path, why)) {
            std::printf("FAIL [%s]: %s\n", c.leaf, why.c_str());
            return 2;
        }
        std::vector<uint8_t> logical = readAll(path);
        if (logical.size() != kImageBytes) {
            std::printf("FAIL [%s]: write-back changed the file size\n", c.leaf);
            return 3;
        }
        if (c.dosOrder) logical = reorder(logical, /*poToDsk=*/false);
        if (logical == po) {
            std::printf("FAIL [%s]: the write-back left the volume unchanged\n", c.leaf);
            return 4;
        }
        if (reference.empty()) {
            reference = logical;
        } else if (logical != reference) {
            std::size_t firstDiff = 0;
            while (firstDiff < logical.size() && logical[firstDiff] == reference[firstDiff])
                ++firstDiff;
            std::printf("FAIL [%s]: logical volume differs from the .po result at "
                        "block %zu (offset %zu) — a skew or sniff mistake on this path\n",
                        c.leaf, firstDiff / 512, firstDiff);
            return 5;
        }
        std::printf("sector_order_smoke: %s boots, saves, and de-skews to the "
                    "same volume OK\n", c.leaf);
        std::error_code ec;
        fs::remove(path, ec);
    }

    // ── DOS 3.3 under both extensions, in both orders ────────────────────
    const std::string dosMaster = findFirst({
        "disks_5.4/dsk/dos33_master.dsk", "../disks_5.4/dsk/dos33_master.dsk",
        "../../disks_5.4/dsk/dos33_master.dsk" });
    if (dosMaster.empty()) {
        std::printf("sector_order_smoke: DOS 3.3 half SKIPPED (no dos33_master.dsk)\n");
    } else {
        const std::vector<uint8_t> dos = readAll(dosMaster);
        if (dos.size() != kImageBytes) { std::printf("FAIL: DOS master size\n"); return 6; }
        const std::vector<uint8_t> dosAsPo = reorder(dos, /*poToDsk=*/false); // DOS logical → ProDOS-order file
        struct DosCase { const char* leaf; const std::vector<uint8_t>* bytes; };
        const DosCase dosCases[] = {
            { "pom2_so_dos.dsk",           &dos },      // the master itself
            { "pom2_so_dos_named_po.po",   &dos },      // DOS order, misnamed
            { "pom2_so_dos_poorder.po",    &dosAsPo },  // honest ProDOS-order twin
            { "pom2_so_dos_poorder_dsk.dsk", &dosAsPo },// ProDOS order, misnamed
        };
        for (const DosCase& c : dosCases) {
            const fs::path path = fs::temp_directory_path() / c.leaf;
            writeAll(path, *c.bytes);
            std::string why;
            if (!bootsToPrompt(rom, prom, path, why)) {
                std::printf("FAIL [%s]: %s\n", c.leaf, why.c_str());
                return 7;
            }
            std::printf("sector_order_smoke: %s boots DOS 3.3 to the prompt OK\n", c.leaf);
            std::error_code ec;
            fs::remove(path, ec);
        }
    }
    std::printf("sector_order_smoke OK\n");
    return 0;
}
