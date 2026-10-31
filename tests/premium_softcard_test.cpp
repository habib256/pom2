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

// premium_softcard — the Microsoft Premium SoftCard //e (PremiumSoftCard.h).
//
// Bus part, no media, through the real //e bus (Memory::memRead/memWrite):
// the Z80's memory is the aux RAM by physical address; $C07E bit 2 inverts
// A15 for the 6502's aux accesses (reads and writes, the aux language card
// included) and nothing else; the status byte at RAMRD $2000 is the Z80's I
// register only while bit 6 is set, plain RAM otherwise; the call protocol —
// `LD I,$80` holds the Z80, a $41 write lets it go — and RESET.
//
// Boot part: Microsoft's CP/M 2.25 and 2.26 for the card
// (disks_5.4/dsk/premium_softcard_cpm225.dsk, _cpm226.po) on an enhanced //e:
// the banner and `A>` on the 80-column screen, DIR, then GBASIC — 24 KB
// loaded across the text page's Z80 address — computes 6 * 7.

#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "PremiumSoftCard.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int64_t kSecond = 1'022'727;

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

struct Rig {
    Memory mem;
    PremiumSoftCard* card = nullptr;
    Rig()
    {
        mem.setIIEMode(true);
        auto c = std::make_unique<PremiumSoftCard>();
        c->setMemory(&mem);
        card = c.get();
        mem.slotBus().plug(PremiumSoftCard::kAuxSlot, std::move(c));
    }
    void reg(uint8_t v) { mem.memWrite(0xC07E, v); }
};

void testFlip()
{
    Rig r;
    CHECK(r.mem.auxSlotPort() == r.card);
    r.mem.memWrite(0xC005, 0);                   // RAMWRT
    r.mem.memWrite(0xC003, 0);                   // RAMRD
    r.mem.memWrite(0x8000, 0x11);                // no flip: aux $8000
    CHECK(r.mem.auxPhysical(0x8000) == 0x11);
    r.reg(PremiumSoftCard::kFlipA15 | PremiumSoftCard::kRunOutOfReset);
    r.mem.memWrite(0x8000, 0xC3);                // flip: Z80 $0000
    r.mem.memWrite(0x6300, 0x5A);                // flip: Z80 $E300 (aux LC high)
    r.mem.memWrite(0x4000, 0x77);                // flip: Z80 $C000 (aux LC $D000 bank 1)
    CHECK(r.mem.auxPhysical(0x0000) == 0xC3);
    CHECK(r.mem.auxPhysical(0xE300) == 0x5A);
    CHECK(r.mem.auxPhysical(0xC000) == 0x77);
    CHECK(r.mem.auxPhysical(0x8000) == 0x11);
    CHECK(r.mem.memRead(0x8000) == 0xC3 && r.mem.memRead(0x0000 ^ 0x8000) == 0xC3);
    CHECK(r.mem.memRead(0x0000 | 0x8000) == 0xC3);
    // Main memory is untouched by the flip.
    r.mem.memWrite(0xC004, 0);
    r.mem.memWrite(0xC002, 0);
    r.mem.memWrite(0x8000, 0x22);
    CHECK(r.mem.memRead(0x8000) == 0x22 && r.mem.peekMainRam(0x8000) == 0x22);
    // The aux language card under ALTZP is flipped too: $E300 → phys $6300.
    r.mem.memWrite(0xC009, 0);
    r.mem.memRead(0xC083); r.mem.memRead(0xC083);
    r.mem.memWrite(0xE300, 0x99);
    CHECK(r.mem.auxPhysical(0x6300) == 0x99);
    r.mem.memWrite(0xC008, 0);
    // The text page: 6502 aux $0400 is the card's SRAM (the screen), the
    // Z80's $0400 is DRAM the 6502 reaches flipped, at $8400.
    r.mem.memWrite(0xC005, 0);
    r.mem.memWrite(0x8400, 0x4B);                // flipped: Z80 $0400
    CHECK(r.mem.auxPhysical(0x0400) == 0x4B);
    CHECK(r.mem.auxData()[0x0400] == 0x00);      // the screen did not change
    r.mem.memWrite(0xC004, 0);
    // Flip off: the plain //e routing again.
    r.reg(PremiumSoftCard::kRunOutOfReset);
    r.mem.memWrite(0xC003, 0);
    CHECK(r.mem.memRead(0x8000) == 0x11);
    r.mem.memWrite(0xC002, 0);
}

// Z80 at $0000 (aux): LD A,$80 / LD I,A / XOR A / LD I,A / LD A,$42 /
// LD ($0045),A / JR $ (spin). The LD I,$80 holds it; $41 releases it.
void testCall()
{
    Rig r;
    const uint8_t prog[] = { 0x3E, 0x80, 0xED, 0x47, 0xAF, 0xED, 0x47,
                             0x3E, 0x42, 0x32, 0x45, 0x00, 0x18, 0xFE };
    for (std::size_t i = 0; i < sizeof(prog); ++i) r.mem.auxPhysical(uint16_t(i)) = prog[i];
    r.mem.auxPhysical(0x2000) = 0x3C;
    r.reg(0x00);                                 // reset
    r.card->advanceCycles(100);
    CHECK(r.card->z80().getState().pc == 0);     // held in reset
    r.reg(0x01);                                 // out of reset, not running
    r.card->advanceCycles(100);
    CHECK(r.card->z80().getState().pc == 0);
    // Status at RAMRD $2000: RAM while bit 6 is clear.
    r.mem.memWrite(0xC003, 0);
    CHECK(r.mem.memRead(0x2000) == 0x3C);
    r.reg(0x41);                                 // go
    CHECK(r.mem.memRead(0x2000) == 0x00);        // I = 0
    r.card->advanceCycles(100);
    CHECK(r.card->callPending());
    CHECK(r.card->z80().getState().pc == 0x0004);   // held after LD I,A
    CHECK(r.mem.memRead(0x2000) == 0x80);           // the call, seen by the 6502
    r.reg(0x01);                                 // take the call: status off
    CHECK(r.mem.memRead(0x2000) == 0x3C);
    r.mem.memWrite(0xC002, 0);
    r.card->advanceCycles(100);
    CHECK(r.card->z80().getState().pc == 0x0004);   // still held
    r.reg(0x41);                                 // return from the call
    r.card->advanceCycles(100);
    CHECK(!r.card->callPending());
    CHECK(r.mem.auxPhysical(0x0045) == 0x42);
    CHECK(r.card->z80().getState().pc == 0x000C);

    // Snapshot round trip.
    std::vector<uint8_t> blob;
    r.card->appendSnapshotState(blob);
    PremiumSoftCard copy;
    copy.loadSnapshotState(blob.data(), blob.size());
    CHECK(copy.reg() == 0x41 && copy.z80().getState().pc == 0x000C);

    // RESET clears the register: Z80 in reset, no flip, no status.
    r.mem.slotBus().reset();
    CHECK(r.card->reg() == 0 && r.card->z80().getState().pc == 0);
    CHECK(r.mem.auxSlotPort() == r.card);
    r.mem.slotBus().unplug(PremiumSoftCard::kAuxSlot);
    CHECK(r.mem.auxSlotPort() == nullptr);
}

uint16_t textRowBase(int row) { return uint16_t(0x0400 + (row % 8) * 0x80 + (row / 8) * 0x28); }

std::vector<std::string> screen80(Memory& mem)
{
    std::vector<std::string> rows;
    const uint8_t* aux = mem.auxData();
    for (int r = 0; r < 24; ++r) {
        std::string s;
        for (int c = 0; c < 80; ++c) {
            const uint16_t a = uint16_t(textRowBase(r) + c / 2);
            uint8_t ch = uint8_t(((c & 1) ? mem.peekMainRam(a) : aux[a]) & 0x7F);
            if (ch < 0x20) ch = uint8_t(ch + 0x40);
            s.push_back(ch < 0x7F ? char(ch) : '.');
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        rows.push_back(s);
    }
    return rows;
}

bool onScreen80(Memory& mem, const std::string& text)
{
    for (const auto& r : screen80(mem)) if (r.find(text) != std::string::npos) return true;
    return false;
}

void dump80(Memory& mem)
{
    for (const auto& r : screen80(mem)) std::fprintf(stderr, "|%s\n", r.c_str());
}

std::string findFirst(std::initializer_list<const char*> c)
{
    for (const char* p : c) if (fs::is_regular_file(p)) return p;
    return {};
}

int testBoot(const char* disk)
{
    const std::string rom    = findFirst({ "roms/apple2e.rom" });
    const std::string prom   = findFirst({ "roms/disk2.rom" });
    const std::string master = findFirst({ disk });
    if (rom.empty() || prom.empty() || master.empty()) return 77;
    const fs::path tmp = fs::temp_directory_path() / (std::string("pom2_premium_softcard") + fs::path(master).extension().string());
    std::error_code ec;
    fs::copy_file(master, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) { std::fprintf(stderr, "FAIL: copy %s\n", master.c_str()); ++g_fail; return 0; }

    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(true);
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    auto d2 = std::make_unique<DiskIICard>(6);
    if (!mem.loadAppleIIRom(rom.c_str()) || !d2->loadBootRom(prom)
        || !d2->insertDisk(tmp.string())) {
        std::fprintf(stderr, "FAIL: machine setup\n");
        ++g_fail;
        return 0;
    }
    mem.slotBus().plug(6, std::move(d2));
    auto card = std::make_unique<PremiumSoftCard>();
    card->setMemory(&mem);
    PremiumSoftCard* psc = card.get();
    mem.slotBus().plug(PremiumSoftCard::kAuxSlot, std::move(card));
    cpu.hardReset();
    mem.slotBus().reset();
    auto run = [&](int64_t cycles) { for (int64_t t = 0; t < cycles;) t += cpu.run(4096); };

    run(15 * kSecond);
    const bool booted = onScreen80(mem, "Softcard //e CP/M") && onScreen80(mem, "A>");
    std::printf("  %s: %s\n", disk, booted ? "booted" : "did not boot");
    CHECK(booted);
    if (!booted) {
        std::fprintf(stderr, "reg %02X z80 pc %04X I %02X pending %d 6502 pc %04X\n",
                     psc->reg(), psc->z80().getState().pc, psc->z80().getState().i,
                     psc->callPending(), cpu.getProgramCounter());
        dump80(mem);
        return 0;
    }
    mem.pasteText("DIR\r");
    run(5 * kSecond);
    CHECK(onScreen80(mem, ": PIP      COM"));
    // GBASIC is 24 KB loaded into the TPA by the 6502's RWTS — through
    // Z80 $2000, where the status byte would answer if the card were
    // running — and runs a line of BASIC.
    mem.pasteText("GBASIC\r");
    run(10 * kSecond);
    mem.pasteText("PRINT 6*7\r");
    run(3 * kSecond);
    CHECK(onScreen80(mem, "BASIC-80 Rev. 5.27"));
    CHECK(onScreen80(mem, " 42"));
    if (g_fail || std::getenv("POM2_DUMP")) dump80(mem);
    return 0;
}

} // namespace

int main()
{
    testFlip();
    testCall();
    // 2.26 (the Apple II Documentation Project's image) is in ProDOS order
    // whatever its name says: only sectors 0 and 15 match 2.25's.
    const int boot = testBoot("disks_5.4/dsk/premium_softcard_cpm225.dsk");
    const int boot226 = testBoot("disks_5.4/dsk/premium_softcard_cpm226.po");
    std::error_code ec;
    for (const char* t : { "pom2_premium_softcard.dsk", "pom2_premium_softcard.po" })
        fs::remove(fs::temp_directory_path() / t, ec);
    if (g_fail) {
        std::fprintf(stderr, "premium_softcard: %d failure(s)\n", g_fail);
        return 1;
    }
    if (boot == 77 || boot226 == 77) {
        std::printf("premium_softcard: SKIP boot part — needs roms/apple2e.rom, "
                    "roms/disk2.rom, disks_5.4/dsk/premium_softcard_cpm225.dsk and "
                    "disks_5.4/dsk/premium_softcard_cpm226.po\n");
        return 77;
    }
    std::printf("premium_softcard: PASS\n");
    return 0;
}
