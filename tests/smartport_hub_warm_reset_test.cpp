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

// A //c+ Ctrl-Reset leaves the 3.5" mechanisms and the IWM alone.
//
// The soft reset used to call `SmartPortHub::reset()`, which reset every
// Sony drive (head to track 0, motor off — a drive has no RESET line on its
// connector, and MAME's `reset_w` touches no floppy state) and dropped the
// hub's devsel to 0 while the IWM, which a Ctrl-Reset does not reset, still
// drove 2. The IWM reports devsel on a change only, so the firmware's next
// internal-drive select ($CC80) was routed with devsel 0 and the internal
// drive stayed unreachable. `warmReset()` drops the MIG selection only and
// recomputes the routing from the IWM (bug hunt 2026-10-25).

#include "Disk35Image.h"
#include "IWMDevice.h"
#include "SmartPortHub.h"
#include "Sony35Drive.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
void check(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

// Strobe Sony register `reg` (CA0-CA2, SEL low): set the address, raise LSTRB.
void strobe(pom2::Sony35Drive& d, uint8_t reg)
{
    d.seekPhaseW(reg & 0x07, 0);
    d.seekPhaseW(static_cast<uint8_t>((reg & 0x07) | 0x08), 0);
    d.seekPhaseW(0x00, 0);
}

}  // namespace

int main()
{
    const fs::path p = fs::temp_directory_path() / "pom2_hub_warm_reset.po";
    {
        std::vector<char> b(pom2::Disk35Image::kBytesPerImage, 0x11);
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f.write(b.data(), static_cast<std::streamsize>(b.size()));
    }
    pom2::Disk35Image img;
    if (!img.loadFile(p.string())) { std::puts("FAIL: image"); return 1; }
    pom2::Sony35Drive di;
    di.setImage(&img);
    pom2::IWMDevice iwm;
    pom2::SmartPortHub hub;
    hub.attach(&iwm);
    hub.setSony35(&di, nullptr);
    uint64_t cyc = 1000;
    auto tick = [&] { cyc += 2; iwm.tick(cyc); };

    hub.setMigIntDrive(true);
    iwm.write(0x0B, 0); tick();          // SEL    → devsel 2
    iwm.write(0x09, 0); tick();          // ENABLE → the hub picks the internal drive
    check(hub.active35() == &di && iwm.getSony35() == &di, "setup: internal drive routed");

    strobe(di, 0x0);                     // DirNext
    for (int i = 0; i < 5; ++i) strobe(di, 0x1);   // StepOn ×5
    strobe(di, 0x2);                     // MotorOn
    check(di.track() == 5 && di.isMotorOn(), "setup: head on track 5, motor on");

    // Ctrl-Reset.
    hub.warmReset();
    check(di.track() == 5, "Ctrl-Reset moved the 3.5\" head");
    check(di.isMotorOn(), "Ctrl-Reset stopped the 3.5\" motor");
    check(!hub.active35Selected() && iwm.getSony35() == nullptr,
          "after Ctrl-Reset (intdrive dropped) the IWM still holds the internal Sony");

    // The firmware selects the internal drive again, the IWM still at devsel 2.
    hub.setMigIntDrive(true);
    check(hub.active35() == &di && iwm.getSony35() == &di,
          "after Ctrl-Reset the internal drive is unreachable until the next motor cycle");

    // Power-on is another matter: the cold path still resets the mechanisms.
    iwm.reset();
    hub.reset();
    check(di.track() == 0 && !di.isMotorOn(), "the cold reset no longer resets the drive");

    std::error_code ec;
    fs::remove(p, ec);
    if (g_failures) return 1;
    std::puts("OK: a //c+ Ctrl-Reset keeps the 3.5\" mechanisms and the IWM's routing");
    return 0;
}
