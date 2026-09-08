// Probe (not a ctest): does A2 File Cmd talk to the //c's PRINTER port?
//
// A2 File Cmd 0.6.6 ships a VDrive driver that scans slots 1..7 for the
// first serial card's Pascal signature and installs on it. On a //c the
// first is slot 1 — the PRINTER port, whose bytes POM2 forwards to the
// ImageWriter (printer tap on, PR#1 lands on the platen with no setup) —
// and every ProDOS ON_LINE / block read the driver answers pushes a VDrive
// envelope down that port. Hypothesis for "the crackling comes from the
// printer sound" (2026-09-09): the ImageWriter is printing the driver's
// packets. Boot A2 File Cmd on the //c ROM with an SSC in slot 1 and one in
// slot 2, both transport-less, and count the bytes each one transmitted.
//
//   make iic_a2fc_printer_port_probe && ./tests/iic_a2fc_printer_port_probe

#include "DiskIICard.h"
#include "IWMDevice.h"
#include "M6502.h"
#include "Memory.h"
#include "ResourcePaths.h"
#include "SlotBus.h"
#include "SuperSerialCard.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

int main()
{
    const std::string rom  = pom2::findResource("roms/apple2c-32Kv0.rom");
    const std::string disk = pom2::findResource("disks_5.4/A2FILECMD.po");
    if (rom.empty() || disk.empty()) { std::puts("need roms/apple2c-32Kv0.rom and disks_5.4/A2FILECMD.po"); return 1; }
    Memory mem;
    M6502  cpu(&mem);
    pom2::IWMDevice iwm;
    mem.setCpu(&cpu);
    mem.setIWM(&iwm);
    mem.setIWMAuthoritative(true);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    if (!mem.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/true)) { std::puts("ROM load failed"); return 1; }

    auto s1 = std::make_unique<SuperSerialCard>(1);
    auto s2 = std::make_unique<SuperSerialCard>(2);
    SuperSerialCard* ssc1 = s1.get(); SuperSerialCard* ssc2 = s2.get();
    mem.slotBus().plug(1, std::move(s1));
    mem.slotBus().plug(2, std::move(s2));

    auto d2 = std::make_unique<DiskIICard>(6);
    const std::string bootRom = pom2::findResource("roms/disk2.rom");
    const std::string lssRom  = pom2::findResource("roms/diskii_p6.rom");
    if (!bootRom.empty()) d2->loadBootRom(bootRom);
    if (!lssRom.empty())  d2->loadLssRom(lssRom);
    const std::filesystem::path scratch = std::filesystem::temp_directory_path() / "pom2_a2fc_probe.po";
    std::error_code ec;
    std::filesystem::copy_file(disk, scratch, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec || !d2->insertDisk(0, scratch.string())) { std::puts("cannot insert A2FILECMD.po"); return 1; }
    d2->setIWM(&iwm);
    mem.slotBus().plug(6, std::move(d2));

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    uint64_t last1 = 0, last2 = 0;
    for (long total = 0; total < 60'000'000; ) {
        total += cpu.run(4096);
        if ((total % (1 << 21)) < 4096) {
            const uint64_t t1 = ssc1->bytesTx(), t2 = ssc2->bytesTx();
            if (t1 != last1 || t2 != last2)
                std::printf("  at %5.1f s: slot 1 (printer port) tx=%llu  slot 2 (modem port) tx=%llu\n",
                            total / 1022727.0, (unsigned long long)t1, (unsigned long long)t2);
            last1 = t1; last2 = t2;
        }
    }
    std::printf("final: slot 1 tx=%llu, slot 2 tx=%llu; slot 1 cmd=$%02X ctl=$%02X\n",
                (unsigned long long)ssc1->bytesTx(), (unsigned long long)ssc2->bytesTx(),
                ssc1->deviceSelectRead(0x2), ssc1->deviceSelectRead(0x3));
    std::filesystem::remove(scratch, ec);
    return 0;
}
