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

// Pinned smoke test for the //c-class on-board SmartPort boot path.
//
// Real //c/+/c+ hardware masks ALL slot ROM behind a forced INTCXROM, so a
// plugged ProDOS block card ($Cn00 firmware) is invisible and cannot boot.
// The one exception POM2 carves out: the built-in SmartPort at slot 5, whose
// $C500 firmware is punched through the mask so ProDOS / bootFromSlot(5) see
// a bootable block device. The IWM/Sony GCR 3.5" boot path the real firmware
// uses is unmodelled — POM2 substitutes a host-serviced block stub (the same
// SmartPortCard the //e profile uses). See project_iic_smartport_boot.
//
// This pins:
//   1. SmartPortCard::exposesIicOnboardRom() — false when empty, true once a
//      unit holds media (so the //c autostart never boots an empty card).
//   2. Memory punches the $C500-$C5FF hole on //c-class ONLY when the slot-5
//      card exposes its ROM: the SmartPort signature + driver-entry bytes are
//      visible with media, and the internal ROM returns once media is ejected.
//   3. The block-transfer protocol ($C0D0-$C0D4 device-select, never masked)
//      streams block 0 of the mounted image through Memory end-to-end.

#include "M6502.h"
#include "Memory.h"
#include "MemoryWatchSink.h"
#include "SmartPortCard.h"
#include "SmartPort35Unit.h"
#include "SmartPortHdvUnit.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string firstExisting(const std::vector<std::string>& candidates)
{
    for (const auto& p : candidates) {
        if (fs::exists(p)) return p;
        if (fs::exists("../" + p))    return "../" + p;
        if (fs::exists("../../" + p)) return "../../" + p;
    }
    return {};
}

// Synthesize a "looks-ProDOS" 800K .po image: block 2 carries the volume-key
// header bytes Disk35Image::loadFile sniffs; block 0 is a uniform fill so the
// read-protocol check is trivial. Mirrors smartport_35_smoke_test.cpp.
std::string makeRaw800k(uint8_t fillKey)
{
    const fs::path p = fs::temp_directory_path() /
        ("pom2_iic_sp_" + std::to_string(fillKey) + ".po");
    std::vector<uint8_t> buf(819200, fillKey);
    buf[0x400 + 0] = 0x00;
    buf[0x400 + 1] = 0x00;
    buf[0x400 + 4] = 0xF5;            // storage_type=F, name_length=5
    buf[0x400 + 5] = 'P'; buf[0x400 + 6] = 'O'; buf[0x400 + 7] = 'M';
    buf[0x400 + 8] = '2'; buf[0x400 + 9] = '5';
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
    return p.string();
}

} // namespace

static void testExposesRomOnlyWithMedia()
{
    pom2::SmartPortCard card(5);
    assert(!card.exposesIicOnboardRom());        // empty → masked

    const std::string img = makeRaw800k(0xE5);
    card.setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
    std::string err;
    assert(card.mountBay(0, img, err));          // media in → exposed
    assert(card.exposesIicOnboardRom());

    card.ejectBay(0);                            // ejected → masked again
    assert(!card.exposesIicOnboardRom());
    fs::remove(img);
    std::printf("  ok: exposesIicOnboardRom() tracks media presence\n");
}

static void testMemoryHolePunch()
{
    const std::string rom = firstExisting({
        "roms/apple2c-32Kv0.rom", "roms/apple2cp.rom",
    });
    if (rom.empty()) {
        std::printf("  SKIP: no 32 KB //c-class ROM present\n");
        return;
    }

    Memory mem;
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    assert(mem.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/true));

    auto card = std::make_unique<pom2::SmartPortCard>(5);
    pom2::SmartPortCard* raw = card.get();
    mem.slotBus().plug(5, std::move(card));

    // No media yet → $C500 hole stays closed (internal ROM). The internal
    // //c SmartPort firmware also starts with a signature, so compare against
    // the *card* bytes specifically: the host stub begins with JMP ($4C) and
    // carries driver-entry $50 at $C5FF — the real firmware does not.
    const uint8_t intC500 = mem.memRead(0xC500);
    const uint8_t intC5FF = mem.memRead(0xC5FF);
    assert(!(intC500 == 0x4C && intC5FF == 0x50));   // hole closed

    // Arm + mount media → hole opens, card firmware visible. (The hole is
    // gated on the SmartPort being "armed" by an explicit boot — see
    // Memory::setIicSmartPortArmed; bootFromSlot sets it at runtime.)
    const std::string img = makeRaw800k(0x33);
    std::string err;
    raw->setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
    assert(raw->mountBay(0, img, err));

    // Still closed until armed, even with media present.
    assert(!(mem.memRead(0xC500) == 0x4C && mem.memRead(0xC5FF) == 0x50));
    mem.setIicSmartPortArmed(true);

    assert(mem.memRead(0xC500) == 0x4C);   // JMP $C520 (boot trampoline)
    assert(mem.memRead(0xC501) == 0x20);   // ProDOS signature
    assert(mem.memRead(0xC503) == 0x00);
    assert(mem.memRead(0xC505) == 0x03);
    assert(mem.memRead(0xC507) == 0x01);   // ProDOS block-device class
    assert(mem.memRead(0xC5FF) == 0x50);   // driver entry $C550

    // Eject → hole closes again.
    raw->ejectBay(0);
    assert(!(mem.memRead(0xC500) == 0x4C && mem.memRead(0xC5FF) == 0x50));

    fs::remove(img);
    std::printf("  ok: Memory punches $C500 hole only when slot-5 card has media\n");
}

static void testBlockReadThroughMemory()
{
    const std::string rom = firstExisting({
        "roms/apple2c-32Kv0.rom", "roms/apple2cp.rom",
    });
    if (rom.empty()) {
        std::printf("  SKIP: no 32 KB //c-class ROM present\n");
        return;
    }

    Memory mem;
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    assert(mem.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/true));

    auto card = std::make_unique<pom2::SmartPortCard>(5);
    pom2::SmartPortCard* raw = card.get();
    mem.slotBus().plug(5, std::move(card));

    const std::string img = makeRaw800k(0xC7);
    std::string err;
    raw->setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
    assert(raw->mountBay(0, img, err));

    // Device-select I/O ($C0D0-$C0DF = slot 5) is never masked on //c-class:
    // select unit 0, point at block 0, stream 512 bytes back through Memory.
    mem.memWrite(0xC0D0, 0x00);   // unit select 0
    mem.memWrite(0xC0D1, 0x00);   // block LO
    mem.memWrite(0xC0D2, 0x00);   // block HI
    for (int i = 0; i < 512; ++i) {
        const uint8_t b = mem.memRead(0xC0D3);
        assert(b == 0xC7);        // block 0 is a uniform fill
    }
    const uint8_t status = mem.memRead(0xC0D4);
    assert((status & 0x01) == 0); // no I/O error
    assert((status & 0x80) == 0); // media present

    fs::remove(img);
    std::printf("  ok: block 0 streams through Memory $C0D3 (slot-5 device-select)\n");
}

// The UI's //c HDV route (routeMountHdv) does NOT go through mountBay: it
// swaps in a SmartPortHdvUnit and adopts a prepared image, the MediaMount
// two-phase way. That combination had never met the $C500 stub until
// SCOSWAMP.HDV booted a //c (2026-08-30) and hung with block 0 never read.
static void testHdvUnitBlockReadThroughMemory()
{
    const std::string rom = firstExisting({
        "roms/apple2c-32Kv0.rom", "roms/apple2cp.rom",
    });
    if (rom.empty()) {
        std::printf("  SKIP: no 32 KB //c-class ROM present\n");
        return;
    }

    Memory mem;
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    assert(mem.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/true));

    auto card = std::make_unique<pom2::SmartPortCard>(5);
    pom2::SmartPortCard* raw = card.get();
    mem.slotBus().plug(5, std::move(card));

    // A minimal HDV: 16 uniform blocks of 0x5A.
    const fs::path img = fs::temp_directory_path() / "pom2_iic_sp_hdv.hdv";
    {
        std::ofstream f(img, std::ios::binary);
        std::vector<char> blk(512, 0x5A);
        for (int i = 0; i < 16; ++i) f.write(blk.data(), blk.size());
    }

    raw->setUnit(0, std::make_unique<pom2::SmartPortHdvUnit>());
    pom2::Block512Backing::PreparedImage prep;
    std::string err;
    assert(pom2::Block512Backing::readImageFile(img.string(), prep, err));
    assert(raw->unit(0)->adoptImage(std::move(prep)));

    mem.setIicSmartPortArmed(true);
    assert(mem.memRead(0xC500) == 0x4C);   // hole open: unit isLoaded()

    mem.memWrite(0xC0D0, 0x00);   // unit select 0
    mem.memWrite(0xC0D1, 0x00);   // block LO
    mem.memWrite(0xC0D2, 0x00);   // block HI
    for (int i = 0; i < 512; ++i) {
        const uint8_t b = mem.memRead(0xC0D3);
        assert(b == 0x5A);
    }
    const uint8_t status = mem.memRead(0xC0D4);
    assert((status & 0x01) == 0); // no I/O error
    assert((status & 0x80) == 0); // media present

    fs::remove(img);
    std::printf("  ok: HDV unit streams block 0 through $C0D3 like the 3.5\n");
}

// Execute the ACTUAL boot: arm, force PC to $C500 like bootFromSlot does,
// and let the 6502 run the stub. Traces the first divergence instead of
// poking registers by hand — this is the experiment the register test
// above cannot do.
static void testHdvBootExecution()
{
    const std::string rom = firstExisting({
        "roms/apple2c-32Kv0.rom", "roms/apple2cp.rom",
    });
    if (rom.empty()) {
        std::printf("  SKIP: no 32 KB //c-class ROM present\n");
        return;
    }

    Memory mem;
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    assert(mem.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/true));

    auto card = std::make_unique<pom2::SmartPortCard>(5);
    pom2::SmartPortCard* raw = card.get();
    mem.slotBus().plug(5, std::move(card));

    const fs::path img = fs::temp_directory_path() / "pom2_iic_boot_hdv.hdv";
    {
        std::ofstream f(img, std::ios::binary);
        std::vector<char> blk(512, 0x00);
        blk[0] = 0x01; blk[1] = 0x60;   // block 0: "1 block loader" + RTS
        f.write(blk.data(), blk.size());
        std::vector<char> rest(512 * 15, 0x00);
        f.write(rest.data(), rest.size());
    }

    raw->setUnit(0, std::make_unique<pom2::SmartPortHdvUnit>());
    pom2::Block512Backing::PreparedImage prep;
    std::string err;
    assert(pom2::Block512Backing::readImageFile(img.string(), prep, err));
    assert(raw->unit(0)->adoptImage(std::move(prep)));

    mem.setIicSmartPortArmed(true);
    M6502 cpu(&mem);
    cpu.hardReset();
    cpu.setProgramCounter(0xC500);

    // Trace des premieres instructions : ou le stub deraille-t-il ?
    struct Tracer : M6502DebugHook {
        std::vector<uint16_t> pcs;
        bool onInstruction(uint16_t pc) override {
            if (pcs.size() < 400) pcs.push_back(pc);
            return false;
        }
    } tracer;
    cpu.setDebugHook(&tracer);

    // 200k cycles is far more than block 0 needs (the stub loop is ~5k).
    int n = 0;
    while (n < 200000) n += cpu.run(1024);
    cpu.setDebugHook(nullptr);
    std::printf("  trace:");
    uint16_t last = 0; int reps = 0;
    for (uint16_t pc : tracer.pcs) {
        if (pc == last) { reps++; continue; }
        if (reps > 1) std::printf("(x%d)", reps);
        std::printf(" %04X", pc);
        last = pc; reps = 1;
    }
    std::printf("\n");

    const uint8_t b0 = mem.memRead(0x0800);
    const uint8_t b1 = mem.memRead(0x0801);
    std::printf("  $0800 = %02X %02X (attendu 01 60)\n", b0, b1);
    assert(b0 == 0x01 && b1 == 0x60);   // block 0 landed and loader ran

    fs::remove(img);
    std::printf("  ok: the 6502 boots block 0 off the HDV unit via $C500\n");
}

// Boot the REAL game HDV headless and trap the first executions inside
// $C800-$CFFF: who enters the expansion window, and from where?
static void testRealHdvBootTrace()
{
    const char* hdv = std::getenv("POM2_TRACE_HDV");
    if (!hdv) { std::printf("  SKIP: POM2_TRACE_HDV non pose\n"); return; }
    const std::string rom = firstExisting({
        "roms/apple2c-32Kv0.rom", "roms/apple2cp.rom",
    });
    if (rom.empty()) { std::printf("  SKIP: pas de ROM //c\n"); return; }

    Memory mem;
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    assert(mem.loadAppleIIRom(rom.c_str(), true));
    auto card = std::make_unique<pom2::SmartPortCard>(5);
    pom2::SmartPortCard* raw = card.get();
    mem.slotBus().plug(5, std::move(card));

    // La face du port compte : ProDOS //c sonde $C507 (classe SmartPort) —
    // l'overlay Liron la fournit, comme au runtime.
    if (!raw->loadLironRom("roms/liron.rom"))
        raw->loadLironRom("../roms/liron.rom");
    raw->setUnit(0, std::make_unique<pom2::SmartPortHdvUnit>());
    pom2::Block512Backing::PreparedImage prep;
    std::string err;
    assert(pom2::Block512Backing::readImageFile(hdv, prep, err));
    assert(raw->unit(0)->adoptImage(std::move(prep)));

    mem.setIicSmartPortArmed(true);
    M6502 cpu(&mem);
    cpu.hardReset();
    cpu.setProgramCounter(0xC500);

    struct Trap : M6502DebugHook {
        Memory* mem;
        M6502*  cpu = nullptr;
        uint16_t ring[256] = {0};
        int      pos = 0;
        int      hits = 0;
        long     count = 0;
        bool onInstruction(uint16_t pc) override {
            ring[pos & 255] = pc; pos++;
            count++;
            // premiere entree dans C800-CFFF DEPUIS l'exterieur
            if (pc >= 0xC800 && pc <= 0xCFFE) {
                const uint16_t prev = ring[(pos - 2) & 255];
                const bool fromStub = (prev >= 0xC500 && prev <= 0xC5FF);
                if (false) {
                    hits++;
                    std::printf("  [entree %d @%ld] %04X -> %04X owner=%d "
                                "intC8=%d ; via:", hits, count, prev, pc,
                                mem->slotBus().getActiveExpansionSlot(),
                                0);
                    for (int i = 8; i >= 2; --i)
                        std::printf(" %04X", ring[(pos - i) & 255]);
                    std::printf("\n");
                }
            }
            return false;
        }
    } trap;
    trap.mem = &mem;
    trap.cpu = &cpu;
    cpu.setDebugHook(&trap);

    int n = 0;
    while (n < 60000000) n += cpu.run(4096);
    cpu.setDebugHook(nullptr);
    std::printf("  fin: %ld instr, $2000=%02X, $4000=%02X, derniers PC:",
                trap.count, mem.memRead(0x2000), mem.memRead(0x4000));
    for (int i = 24; i >= 1; --i)
        std::printf(" %04X", trap.ring[(trap.pos - i) & 255]);
    std::printf("\n");
}

// L'oracle //e : meme HDV, meme carte, meme boot force a $C500 — seule la
// ROM change. Si P8 meurt ici aussi, le probleme est la carte face a
// ProDOS ; s'il boote, la divergence est le chemin //c.
static void testRealHdvBootIIe()
{
    const char* hdv = std::getenv("POM2_TRACE_HDV");
    if (!hdv) { std::printf("  SKIP: POM2_TRACE_HDV non pose\n"); return; }
    const std::string rom = firstExisting({
        "roms/apple2e.rom", "../roms/apple2e.rom",
    });
    if (rom.empty()) { std::printf("  SKIP: pas de ROM //e\n"); return; }

    Memory mem;
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    assert(mem.loadAppleIIRom(rom.c_str(), true));

    auto card = std::make_unique<pom2::SmartPortCard>(5);
    pom2::SmartPortCard* raw = card.get();
    mem.slotBus().plug(5, std::move(card));
    if (!raw->loadLironRom("roms/liron.rom"))
        raw->loadLironRom("../roms/liron.rom");

    raw->setUnit(0, std::make_unique<pom2::SmartPortHdvUnit>());
    pom2::Block512Backing::PreparedImage prep;
    std::string err;
    assert(pom2::Block512Backing::readImageFile(hdv, prep, err));
    assert(raw->unit(0)->adoptImage(std::move(prep)));

    M6502 cpu(&mem);
    cpu.hardReset();
    cpu.setProgramCounter(0xC500);

    struct Peek : M6502DebugHook {
        Memory* mem; M6502* cpu; int hits = 0;
        bool onInstruction(uint16_t pc) override {
            if (pc == 0xE1C2 && hits < 3) {
                hits++;
                const uint8_t y = cpu->getYRegister();
                std::printf("  [//e dispatch #%d] Y=%02X $D801+Y=%02X "
                            "$FECF=%02X  $D910/30/50/70/90:"
                            " %02X %02X %02X %02X %02X\n",
                            hits, y, mem->memRead(0xD801 + y),
                            mem->memRead(0xFECF),
                            mem->memRead(0xD910), mem->memRead(0xD930),
                            mem->memRead(0xD950), mem->memRead(0xD970),
                            mem->memRead(0xD990));
            }
            return false;
        }
    } peek;
    peek.mem = &mem; peek.cpu = &cpu;
    cpu.setDebugHook(&peek);
    int n = 0;
    while (n < 60000000) n += cpu.run(4096);
    cpu.setDebugHook(nullptr);

    // La page texte 40 colonnes, brute : le jeu affiche sa banniere ou P8
    // son RESTART SYSTEM — l'un des deux dira qui a gagne.
    std::printf("  //e: $2000=%02X $4000=%02X  ecran:",
                mem.memRead(0x2000), mem.memRead(0x4000));
    for (uint16_t a = 0x05B2; a <= 0x05C6; ++a) {
        const uint8_t c = mem.memRead(a) & 0x7F;
        std::printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
    }
    std::printf("|");
    for (uint16_t a = 0x0480; a <= 0x04A8; ++a) {
        const uint8_t c = mem.memRead(a) & 0x7F;
        std::printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
    }
    std::printf("\n");
}

// The $C800 window the //c punch opens is closed again by "the first $C0xx
// access foreign to the slot's device-select" — that is what tells the
// internal firmware (which hammers the soft switches constantly) apart from
// the stub (which never touches them). Only the READ path implemented it: a
// write to $C000-$C07F returned from memWriteSlow's soft-switch branch before
// ever reaching the close. Firmware WRITES its soft switches at least as
// often as it reads them (STA $C00D, STA $C051…), so the window stayed open
// across them and the next JMP $CExx from the Monitor executed the card's
// bank instead of the internal ROM — exactly the failure the guard exists to
// prevent.
static void testSoftSwitchWriteClosesTheC800Window()
{
    const std::string rom = firstExisting({
        "roms/apple2c-32Kv0.rom", "roms/apple2cp.rom",
    });
    if (rom.empty()) {
        std::printf("  SKIP: no 32 KB //c-class ROM present\n");
        return;
    }

    Memory mem;
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    assert(mem.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/true));

    auto card = std::make_unique<pom2::SmartPortCard>(5);
    pom2::SmartPortCard* raw = card.get();
    mem.slotBus().plug(5, std::move(card));

    const std::string img = makeRaw800k(0x77);
    std::string err;
    raw->setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
    assert(raw->mountBay(0, img, err));
    mem.setIicSmartPortArmed(true);

    const uint8_t internalC800 = mem.memRead(0xC800);   // window shut
    const uint8_t cardC800     = raw->expansionRomRead(0);
    assert(internalC800 != cardC800 &&
           "the two banks are indistinguishable — this test cannot discriminate");

    // A fetch in the stub's $C5xx page opens the window (and marks slot 5 as
    // the expansion owner), exactly as entering the driver does.
    auto openWindow = [&] {
        (void)mem.memRead(0xC500);
        assert(mem.memRead(0xC800) == cardC800 && "the window did not open");
    };

    // The read path — the half that already worked, kept as the control.
    openWindow();
    (void)mem.memRead(0xC051);                       // TEXT on, by read
    assert(mem.memRead(0xC800) == internalC800);

    // THE case: the same switch, written.
    openWindow();
    mem.memWrite(0xC051, 0x00);                      // TEXT on, by write
    assert(mem.memRead(0xC800) == internalC800 &&
           "a soft-switch WRITE left the //c $C800 card window open");

    // …and a write to slot 5's own device-select ($C0D0-$C0DF) must NOT close
    // it: that is the card talking to its own registers, not foreign traffic.
    openWindow();
    mem.memWrite(0xC0D0, 0x00);                      // unit select 0
    assert(mem.memRead(0xC800) == cardC800 &&
           "the slot's own device-select closed its expansion window");

    // A foreign slot's device-select does close it, on the write path too.
    mem.memWrite(0xC0C0, 0x00);                      // slot 4, empty
    assert(mem.memRead(0xC800) == internalC800);

    fs::remove(img);
    std::printf("  ok: a soft-switch write closes the //c $C800 card window\n");
}

int main()
{
    std::printf("\n[//c on-board SmartPort smoke]\n");
    testSoftSwitchWriteClosesTheC800Window();
    testExposesRomOnlyWithMedia();
    testHdvUnitBlockReadThroughMemory();
    testHdvBootExecution();
    testRealHdvBootTrace();
    testRealHdvBootIIe();
    testMemoryHolePunch();
    testBlockReadThroughMemory();
    std::printf("[//c on-board SmartPort smoke] ALL PASS\n");
    return 0;
}
