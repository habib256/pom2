// No-Slot Clock against a REAL ProDOS clock driver.
//
// `no_slot_clock` pins the DS1216E protocol against a hand-rolled key walk.
// This pins what a user actually runs: ProDOS 8 2.4.3 booted from the
// tracked .po in drive 1, the a2stuff `prodos-drivers` disk in drive 2, and
// `-/DRIVERS/NS.CLOCK.SYSTEM` typed at the BASIC.SYSTEM prompt. That driver
// (the 1991 SMT `NS.CLOCK.SYSTEM` lineage) probes the slot ROMs $C300-$C700
// through `$Cn00` / `$Cn04`, then `$C800` with INTCXROM on, validates three
// reads, patches ProDOS's clock vector and prints the date it read.
//
// Two facts this states, one of which is a limitation:
//   * //e: the chip under the internal ROM answers at $C300 — the first
//     place the driver looks — and the driver prints the injected date.
//   * II+, motherboard placement only: POM2's chip under the Monitor ROM
//     ($F800-$FFFF, the AppleWin placement). Neither this driver nor the
//     original SMT one ever probes there (both scan slot ROMs then $C800),
//     so the driver reports "Not Found". ProDOS 8 itself has NO built-in
//     NSC support at any version — only the ThunderClock driver is built
//     in. Documented in DEV.md § No-Slot Clock.
//   * II+, chip ALSO under the ROM of the card in slot 1 (`nsclock_slot`,
//     the fresh-install default): the driver's slot scan finds it at $C100
//     on its second probe and reads the date. That is how a real II+ was
//     fitted — the SmartWatch went into a peripheral card's ROM socket.
//
// SKIPs (77) when a ROM or either disk is missing. The drivers disk is not
// redistributed by this repository; fetch it with
//   gh release download v1.8 -R a2stuff/prodos-drivers -p prodos-drivers.po
// and place it at disks_5.4/dsk/prodos-drivers-a2stuff-v1.8.po.

#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "NoSlotClock.h"
#include "SlotPeripheral.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string findFirst(std::initializer_list<const char*> candidates)
{
    for (const char* c : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(c, ec)) return c;
    }
    return {};
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

// A card with a ROM socket — what the SmartWatch sits under on a II+. The
// bytes are irrelevant to the driver (it walks the key on the address
// lines and reads the clock on D0); a real card's firmware would do.
class RomCard : public SlotPeripheral
{
public:
    explicit RomCard(int slot) : slot_(slot) {}
    std::string_view name() const override { return "ROM card"; }
    int getSlot() const { return slot_; }
    uint8_t slotRomRead(uint8_t low8) override
    { return static_cast<uint8_t>(0xEA ^ low8); }
private:
    int slot_;
};

// The injected clock: 7 September 2026, 21:30. The driver prints m/d/yy.
std::tm fixedTime()
{
    std::tm t{};
    t.tm_year = 2026 - 1900;
    t.tm_mon  = 8;
    t.tm_mday = 7;
    t.tm_hour = 21;
    t.tm_min  = 30;
    t.tm_wday = 1;
    return t;
}

std::string upper(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Boot, reach BASIC.SYSTEM, run the driver, return the screen (upper-cased:
// a II+ has no lowercase and shows the driver's message in capitals).
std::string runDriver(const std::string& rom, bool iie, int nscSlot,
                      const std::string& bootPo, const std::string& driversPo,
                      std::string& why)
{
    const fs::path scratch =
        fs::temp_directory_path() / (iie ? "pom2_nsc_drv_iie.po" : "pom2_nsc_drv_iip.po");
    std::error_code ec;
    fs::copy_file(bootPo, scratch, fs::copy_options::overwrite_existing, ec);
    if (ec) { why = "cannot copy the boot disk"; return {}; }

    Memory mem;
    if (iie) mem.setIIEMode(true);
    if (!mem.loadAppleIIRom(rom.c_str())) { why = "ROM load failed"; return {}; }
    pom2::NoSlotClock nsc(&fixedTime);
    nsc.setSlot(nscSlot);
    mem.setNoSlotClock(&nsc);
    if (nscSlot >= 1) mem.slotBus().plug(nscSlot, std::make_unique<RomCard>(nscSlot));

    auto card = std::make_unique<DiskIICard>();
    const std::string prom = findFirst({ "roms/disk2.rom", "../roms/disk2.rom",
                                         "../../roms/disk2.rom" });
    if (!prom.empty() && !card->loadBootRom(prom)) { why = "PROM"; return {}; }
    if (!card->insertDisk(0, scratch.string())) { why = "insert D1"; return {}; }
    if (!card->insertDisk(1, driversPo))        { why = "insert D2"; return {}; }
    mem.slotBus().plug(6, std::move(card));

    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.slotBus().reset();
    cpu.hardReset();
    cpu.setProgramCounter(0xC600);
    const uint8_t* ram = mem.data();

    // Same navigation as prodos_save_smoke: BITSY BYE → BASIC.SYSTEM.
    if (!waitForText(cpu, ram, "BITSY  BYE", 180'000'000L, 10'000'000L)) {
        why = "BITSY BYE never appeared:\n" + scrapeTextPage(ram);
        return {};
    }
    runFor(cpu, 30'000'000L);
    auto press = [&](char k, long wait) { mem.pasteRawKeys(&k, 1); runFor(cpu, wait); };
    press(0x0A, 5'000'000); press(0x0A, 5'000'000); press(0x0A, 5'000'000);
    press(0x0D, 90'000'000);
    if (scrapeTextPage(ram).find("\n]") == std::string::npos) {
        why = "no BASIC prompt:\n" + scrapeTextPage(ram);
        return {};
    }
    const char* cmd = "HOME\r-/DRIVERS/NS.CLOCK.SYSTEM\r";
    mem.pasteText(cmd, std::strlen(cmd));
    runFor(cpu, 60'000'000L);
    return upper(scrapeTextPage(ram));
}

} // namespace

int main()
{
    const std::string romIIe = findFirst({ "roms/apple2e.rom", "../roms/apple2e.rom",
                                           "../../roms/apple2e.rom" });
    const std::string romIIp = findFirst({ "roms/apple2p.rom", "roms/apple2.rom",
                                           "../roms/apple2p.rom", "../roms/apple2.rom",
                                           "../../roms/apple2.rom" });
    const std::string bootPo = findFirst({
        "disks_5.4/dsk/ProDOS_2_4_3.po", "../disks_5.4/dsk/ProDOS_2_4_3.po",
        "../../disks_5.4/dsk/ProDOS_2_4_3.po" });
    const std::string driversPo = findFirst({
        "disks_5.4/dsk/prodos-drivers-a2stuff-v1.8.po",
        "../disks_5.4/dsk/prodos-drivers-a2stuff-v1.8.po",
        "../../disks_5.4/dsk/prodos-drivers-a2stuff-v1.8.po" });
    if (romIIe.empty() || romIIp.empty() || bootPo.empty() || driversPo.empty()) {
        std::printf("no_slot_clock_prodos_driver SKIP: missing ROM or disk "
                    "(see the header for where the drivers disk comes from)\n");
        return 77;
    }

    std::string why;
    const std::string iie = runDriver(romIIe, true, -1, bootPo, driversPo, why);
    if (iie.empty()) { std::printf("FAIL (//e): %s\n", why.c_str()); return 1; }
    if (iie.find("NO-SLOT CLOCK - 9/7/26") == std::string::npos) {
        std::printf("FAIL: on the //e the driver did not read the injected "
                    "date through the $C300 window.\nScreen:\n%s\n", iie.c_str());
        return 2;
    }
    std::printf("no_slot_clock_prodos_driver: //e — NS.CLOCK.SYSTEM found the clock "
                "and read 9/7/26 OK\n");

    const std::string iip = runDriver(romIIp, false, -1, bootPo, driversPo, why);
    if (iip.empty()) { std::printf("FAIL (II+): %s\n", why.c_str()); return 3; }
    if (iip.find("NO-SLOT CLOCK - NOT FOUND") == std::string::npos) {
        // Either the driver grew a Monitor-ROM probe or POM2 moved the II+
        // window: both are behaviour changes DEV.md must record.
        std::printf("UNEXPECTED: on the II+ the driver's answer changed — update "
                    "DEV.md § No-Slot Clock.\nScreen:\n%s\n", iip.c_str());
        return 4;
    }
    std::printf("no_slot_clock_prodos_driver: II+ (motherboard ROM only) — the "
                "driver does not probe the Monitor ROM, reports Not Found "
                "(documented) OK\n");

    const std::string iipSlot = runDriver(romIIp, false, 1, bootPo, driversPo, why);
    if (iipSlot.empty()) { std::printf("FAIL (II+ slot 1): %s\n", why.c_str()); return 5; }
    if (iipSlot.find("NO-SLOT CLOCK - 9/7/26") == std::string::npos) {
        std::printf("FAIL: on the II+ the driver did not find the chip under "
                    "slot 1's card ROM.\nScreen:\n%s\n", iipSlot.c_str());
        return 6;
    }
    std::printf("no_slot_clock_prodos_driver: II+ (chip under slot 1's card ROM) — "
                "found at $C100, read 9/7/26 OK\n");
    return 0;
}
