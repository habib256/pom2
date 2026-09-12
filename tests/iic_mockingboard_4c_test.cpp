// The Mockingboard 4c on a //c's internal expansion connector — 2026-09-12.
//
// A //c has no slots, but it does have an internal expansion connector, and
// the Mockingboard 4c (ReactiveMicro's //c build of the card) sits on it and
// answers at $C400-$C4FF — the slot-4 addresses — while the machine's own
// mouse is the IOU, not a card. POM2 modelled "no slots" as "no cards at
// all", so this whole class of //c software had nowhere to run.
//
// DIGIDREAM is that software, and it ships in this repo's corpus: its boot
// source is versioned "SPECIAL IIc/MB4C", its //c branch wakes the card with
// two writes ($C403/$C404), and its detection then READS the 6522's T1
// counter twice at $Cx04 from $C7 down to $C1 looking for a -8 delta. So the
// window has to carry writes AND reads. Without a card it writes $CB/$CF —
// "KO" — to the top-left of the text page and spins for ever.
//
// The control run (no card) must reach that KO: otherwise this test would
// pass on a disk that never looked for a Mockingboard at all.
//
// Needs roms/apple2c-32Kv0.rom and disks_5.4/demo/digidream/DD.dsk.

#include "DiskIICard.h"
#include "IWMDevice.h"
#include "M6502.h"
#include "Memory.h"
#include "Mockingboard.h"
#include "ResourcePaths.h"
#include "SlotBus.h"
#include "TestTempPath.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
void fail(const std::string& what) { std::printf("FAIL: %s\n", what.c_str()); ++g_failures; }

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

struct Outcome {
    bool     ko          = false;   // the demo's "no Mockingboard" marker
    uint32_t ayWrites    = 0;
    uint8_t  mixer       = 0;       // AY R7, which any player writes early
    std::string screen;
};

Outcome boot(const std::string& rom, const fs::path& dsk, bool withCard)
{
    Outcome o;
    Memory mem;
    M6502  cpu(&mem);
    pom2::IWMDevice iwm;
    mem.setCpu(&cpu);
    mem.setIWM(&iwm);
    mem.setIWMAuthoritative(true);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    // A //c-class ROM is what puts Memory into //c mode (forced INTCXROM and
    // all): `loadAppleIIRom` probes the dump, exactly as MAME does.
    if (!mem.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/true)) {
        fail("cannot load the //c ROM");
        return o;
    }

    auto d2 = std::make_unique<DiskIICard>(6);
    const std::string bootRom = pom2::findResource("roms/disk2.rom");
    const std::string lssRom  = pom2::findResource("roms/diskii_p6.rom");
    if (!bootRom.empty()) d2->loadBootRom(bootRom);
    if (!lssRom.empty())  d2->loadLssRom(lssRom);
    if (!d2->insertDisk(0, dsk.string())) { fail("cannot insert DD.dsk"); return o; }
    d2->setIWM(&iwm);
    mem.slotBus().plug(6, std::move(d2));

    // Parked on virtual slot 3 deliberately: the card answers at $C400
    // wherever POM2 holds it, and slot 4 is the machine's IOU mouse.
    MockingboardCard* mb = nullptr;
    if (withCard) {
        auto card = std::make_unique<MockingboardCard>(3);
        mb = card.get();
        card->setCpu(&cpu);
        mem.slotBus().plug(3, std::move(card));
    }

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    constexpr long kBudget = 80'000'000;          // ~80 emulated seconds
    for (long total = 0; total < kBudget; ) {
        total += cpu.run(4096);
        if (mem.peekMainRam(0x0400) == 0xCB && mem.peekMainRam(0x0401) == 0xCF) {
            o.ko = true;
            break;
        }
        if (mb && mb->getAyWriteCount(0) > 0 && total > 40'000'000) break;
    }
    if (mb) {
        o.ayWrites = mb->getAyWriteCount(0);
        o.mixer    = mb->getAyRegister(0, 7);
    }
    o.screen = scrapeTextPage(mem.data());
    return o;
}

}  // namespace

// ── The window, without a disk: writes AND reads, on a bare //c ─────────
//
// This is the feature itself. DIGIDREAM's own sequence: wake the card with
// STA $C403 / STA $C402, then read $C404 twice and look at the delta of the
// 6522's T1 counter — which is why the window cannot be write-only.
void testTheWindowCarriesBothDirections(const std::string& rom)
{
    Memory mem;
    M6502  cpu(&mem);
    mem.setCpu(&cpu);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    if (!mem.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/true)) {
        fail("cannot load the //c ROM");
        return;
    }
    // Parked on slot 3: the card answers at $C400 wherever POM2 holds it,
    // and slot 4 on this machine is the IOU mouse.
    auto card = std::make_unique<MockingboardCard>(3);
    MockingboardCard* mb = card.get();
    card->setCpu(&cpu);
    mem.slotBus().plug(3, std::move(card));

    mem.memWrite(0xC403, 0xFF);                     // VIA1 DDRA
    mem.memWrite(0xC402, 0x07);                     // VIA1 DDRB
    if (mb->peekViaRegister(0, 3) != 0xFF || mb->peekViaRegister(0, 2) != 0x07)
        fail("a write into $C400-$C40F did not reach the card's VIA");

    mem.memWrite(0xC405, 0x20);                     // T1CH: start the timer
    const uint8_t t1a = mem.memRead(0xC404);
    mem.advanceCycles(8);
    const uint8_t t1b = mem.memRead(0xC404);
    const int delta = static_cast<int8_t>(static_cast<uint8_t>(t1b - t1a));
    if (delta >= 0)
        fail("two reads of $C404 did not show the T1 counter running (delta " +
             std::to_string(delta) + ") — the demo's detection reads this");

    // A whole AY register store through the window.
    mem.memWrite(0xC401, 0x07); mem.memWrite(0xC400, 0x07);
    mem.memWrite(0xC400, 0x04);
    mem.memWrite(0xC401, 0x38); mem.memWrite(0xC400, 0x06);
    mem.memWrite(0xC400, 0x04);
    if (mb->getAyRegister(0, 7) != 0x38 || mb->getAyWriteCount(0) == 0)
        fail("an AY register store through $C400/$C401 never reached the chip");

    // And the machine's own mouse keeps slot 4: the card claims the PAGE,
    // not the slot.
    if (mem.slotBus().peripheral(4) != nullptr)
        fail("this bare-machine fixture should have nothing in slot 4");
    if (g_failures == 0)
        std::printf("  ok: $C400-$C4FF carries writes and reads to the card "
                    "(T1 delta %d)\n", delta);
}

int main()
{
    const std::string rom = pom2::findResource("roms/apple2c-32Kv0.rom");
    const std::string dsk = pom2::findResource("disks_5.4/demo/digidream/DD.dsk");
    if (rom.empty() || dsk.empty()) {
        std::printf("SKIP iic_mockingboard_4c: need roms/apple2c-32Kv0.rom and "
                    "disks_5.4/demo/digidream/DD.dsk\n");
        return 77;
    }
    testTheWindowCarriesBothDirections(rom);

    // A COPY: the tracked disk is user media, and a demo may write to it.
    const fs::path scratch = fs::path(pom2test::tempPath("pom2_mb4c")).parent_path()
                             / "pom2_mb4c";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);
    const fs::path copy = scratch / "DD.dsk";
    fs::copy_file(dsk, copy, fs::copy_options::overwrite_existing, ec);
    if (ec) { fail("cannot copy DD.dsk: " + ec.message()); return 1; }

    // ── Control: no card. The demo must say so. ──────────────────────────
    {
        const Outcome o = boot(rom, copy, /*withCard=*/false);
        if (!o.ko) {
            fail("without a Mockingboard 4c, DIGIDREAM did not reach its "
                 "\"KO\" marker — this test would not be measuring anything");
            std::printf("--- text page ---\n%s---\n", o.screen.c_str());
        } else {
            std::printf("  ok: no card -> the demo's own \"KO\" marker\n");
        }
    }

    // ── With the card on the internal connector ──────────────────────────
    {
        const Outcome o = boot(rom, copy, /*withCard=*/true);
        std::printf("  with the 4c: KO=%d, AY writes=%u, R7=$%02X\n",
                    o.ko ? 1 : 0, o.ayWrites, o.mixer);
        if (o.ko)
            fail("DIGIDREAM still reports no Mockingboard with a 4c plugged — "
                 "its detection reads $C404, so the $C400 window must carry "
                 "reads as well as writes");
        // The AY writes are NOT asserted here: DIGIDREAM detects at boot and
        // then loads its parts from the floppy for a good while before the
        // player starts, so whether any store lands inside this budget is a
        // property of the disk, not of the window. The window is pinned at
        // unit level above; what the demo proves is the DETECTION, which is
        // what "KO" is about.
        if (!o.ko)
            std::printf("  ok: DIGIDREAM finds its Mockingboard 4c\n");
        if (g_failures) std::printf("--- text page ---\n%s---\n", o.screen.c_str());
    }

    fs::remove_all(scratch, ec);
    if (g_failures) return 1;
    std::puts("iic_mockingboard_4c OK");
    return 0;
}
