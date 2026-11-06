// POM2 — SPDX-License-Identifier: GPL-3.0-or-later
// Optional local-media probe, no third-party ROM/disk bundled. Usage in
// docs/hardware/sirius-joyport.md. This deliberately runs the game's driver.
#include "Memory.h"
#include "M6502.h"
#include "DiskIICard.h"
#include "SlotBus.h"
#include <cassert>
#include <cstdio>
#include <memory>

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::fprintf(stderr, "Usage: %s apple2.rom disk2.rom wavy-navy-4am.dsk\n", argv[0]);
        return 77;
    }
    using S = pom2::SiriusJoyport;
    for (int jack = 0; jack < 2; ++jack) {
        Memory m; m.setIIEMode(false);
        if (!m.loadAppleIIRom(argv[1])) return 1;
        auto disk = std::make_unique<DiskIICard>();
        if (!disk->loadBootRom(argv[2]) || !disk->insertDisk(argv[3])) return 1;
        m.slotBus().plug(6, std::move(disk));
        m.setJoyportMode(jack == 0 ? S::Left : S::Right);
        M6502 cpu(&m); cpu.setCpuMode(M6502::CpuMode::NMOS);
        cpu.hardReset(); m.slotBus().reset();
        int before = 0, left = 0, right = 0;
        for (int frame = 0; frame < 1562; ++frame) {
            cpu.run(17030);
            if (frame == 600) m.queueKey(' ');
            if (frame == 900) m.queueKey('Z'); // game's JOYPORT option
            if (frame == 1200) m.queueKey('S');
            if (frame == 1500) m.setJoyportContacts(jack, S::Fire);
            if (frame == 1530) m.setJoyportContacts(jack, 0);
            if (frame == 1540) {
                assert(m.peekMainRam(0x4752) == 2); // selected Joyport driver
                before = m.peekMainRam(0x4706); // boat position, from disassembly
                m.setJoyportContacts(jack, S::Leftward);
            }
            if (frame == 1550) {
                left = m.peekMainRam(0x4706);
                assert(m.peekMainRam(0x470B) == 0x10); // left target
                m.setJoyportContacts(jack, S::Rightward);
            }
            if (frame == 1560) {
                right = m.peekMainRam(0x4706);
                assert(m.peekMainRam(0x470B) == 0xE1); // right target
            }
        }
        assert(left < before && right > left);
        std::printf("Wavy Navy socket %d: boat %d -> %d -> %d; Joyport driver selected\n",
                    jack + 1, before, left, right);
    }
}
