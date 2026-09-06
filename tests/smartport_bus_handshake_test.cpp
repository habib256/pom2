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

// The SmartPort BUS protocol, end to end: a Liron card boots a 3.5" disk.
//
// `liron_boot35` pins the card with its bus responder OFF: the firmware finds
// nothing on the port and reports ProDOS $28, which is what a Liron with no
// intelligent device does. This file pins the other configuration — responder
// ON — and every step below was read out of the firmware and is easy to break
// silently.
//
// The protocol, extracted from `roms/liron.rom` with POM2's own
// `disassemble6502` (the //c's bank-1 firmware is the same code byte for
// byte, so these addresses serve both):
//
//   $C800  probe    PH1 + LSTRB high, SEL high, motor on, then poll the
//                   status register 50× for SENSE (bit 7). Timeout → $28.
//   $C87D  send     bytes to $C0nD, each with bit 7 set, waiting on the
//                   write handshake at $C0nC between them. Seven data bytes
//                   per group, preceded by a byte carrying their gathered
//                   high bits ($41).
//   $C92C  drain    poll $C0nC until bit 6 (underrun) CLEARS.
//   $C943  ack      poll the status register until SENSE goes LOW.
//   $C960  receive  re-assert PH1 + LSTRB, read $C0nD, wait for SENSE HIGH,
//                   then hunt for $C3 and take SEVEN header bytes into $0051
//                   down to $004B, each masked with $7F. $004C is the odd-byte
//                   count, $004B the count of seven-byte groups.
//   $C52B  bulk     the slot page's own reader: a marker byte then seven data
//                   bytes, repeated $004B times. The marker carries the seven
//                   bytes' bit 7s, most significant first — the ROM's tables
//                   at $CA27/$CA37/$CA47/$CA57 hold nothing but $80/$00 masks
//                   keyed on its bits, which is the same statement in silicon.
//   $C5B8  tail     two checksum bytes then $C8. The checksum is a running
//                   XOR of the header WIRE bytes, the decoded group bytes and
//                   the decoded odd bytes, sent 4-and-4: the receiver recovers
//                   it as ((chk2 << 1) | 1) & chk1.
//
// POM2 implements all of that (`LironCard::busBuildReply`), and this test
// pins the water mark it reaches: the enumeration's command $05 decodes, the
// status reply is accepted, and the scan counts the device and stops looping
// (`$4D` non-zero exits it — `BEQ $CE19` is the loop).
//
// Where it stops: the firmware does not go on to the boot's block read. The
// likely gap is the CONTENT of the status reply — a device descriptor the
// enumeration stores and the boot then consults — not the wire, which is now
// verified numerically: the running checksum POM2 computes for a header-only
// reply ($81) is the byte the firmware's own $40 holds at its terminator
// check. `POM2_TRACE_SMARTPORT_BUS=1` prints every byte in both directions.

#include "LironCard.h"
#include "M6502.h"
#include "Memory.h"
#include "ResourcePaths.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

int main()
{
    const std::string rom  = pom2::findResource("roms/apple2e.rom");
    const std::string disk =
        pom2::findResource("disks_3.5/A2DeskTop-1.5-en_800k.2mg");
    if (rom.empty() || disk.empty()) {
        std::printf("SKIP smartport_bus_handshake: need roms/apple2e.rom and "
                    "an 800K image\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    constexpr int kSlot = 5;
    Memory mem;
    M6502  cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(true);
    mem.clearRam();
    mem.resetSoftSwitches();
    if (!mem.loadAppleIIRom(rom.c_str())) {
        std::printf("SKIP smartport_bus_handshake: cannot load %s\n",
                    rom.c_str());
        return 77;   // ctest SKIP_RETURN_CODE
    }

    auto card = std::make_unique<pom2::LironCard>(kSlot);
    pom2::LironCard* liron = card.get();
    if (!liron->romLoaded()) {
        std::printf("SKIP smartport_bus_handshake: %s\n",
                    liron->lastError().c_str());
        return 77;   // ctest SKIP_RETURN_CODE
    }
    liron->setBusResponderEnabled(true);
    std::string err;
    if (!liron->mountBay(0, disk, err)) {
        std::printf("SKIP smartport_bus_handshake: %s\n", err.c_str());
        return 77;   // ctest SKIP_RETURN_CODE
    }
    mem.slotBus().plug(kSlot, std::move(card));

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    cpu.setProgramCounter(static_cast<uint16_t>(0xC000 + kSlot * 256));
    bool prodosSeen = false;
    for (long total = 0; total < 40'000'000; ) {
        total += cpu.run(4096);
        if (!prodosSeen && (total % (1 << 20)) < 4096) {
            for (int i = 0; i < 0x400 && !prodosSeen; ++i) {
                // "ProDOS" in Apple high-ASCII, anywhere on text page 1.
                static const uint8_t kProDOS[6] = {0xD0,0xF2,0xEF,0xC4,0xCF,0xD3};
                bool hit = true;
                for (int k = 0; k < 6 && hit; ++k)
                    hit = mem.memRead(static_cast<uint16_t>(0x400 + i + k)) == kProDOS[k];
                if (hit) prodosSeen = true;
            }
        }
    }

    const auto p = liron->busProgress();
    int failures = 0;
    auto fail = [&](const char* msg) { std::printf("FAIL: %s\n", msg); ++failures; };

    // The presence poll: fifty status reads at $C813 waiting for SENSE.
    if (!p.probeAnswered)
        fail("the firmware's presence poll never saw SENSE — the card is not "
             "recognising PH1 + LSTRB + enable as \"the bus\"");
    // The INIT scan, then the boot's block reads — every one a full
    // command/reply round trip through $C800 and $C960.
    if (!p.commandTaken)   fail("no command packet arrived");
    if (!p.packetParsed)   fail("the command bytes did not decode as a packet");
    if (p.transactions < 3)
        fail("fewer than three transactions: the two INITs and the first "
             "READ are the minimum for a boot to have started");
    if (p.blocksRead == 0)
        fail("no block was read over the bus — the enumeration was accepted "
             "but the boot's READ never came or was refused (its unit is the "
             "packet's DESTINATION, not contents[1])");
    if (!p.replyDelivered) fail("the last reply was never read to its end");
    // And the machine actually got somewhere with the bytes: ProDOS's
    // banner on the text page (A2DeskTop boots ProDOS 8 first).
    std::string screen;
    static const int rowBase[24] = {
        0x400,0x480,0x500,0x580,0x600,0x680,0x700,0x780,
        0x428,0x4A8,0x528,0x5A8,0x628,0x6A8,0x728,0x7A8,
        0x450,0x4D0,0x550,0x5D0,0x650,0x6D0,0x750,0x7D0 };
    for (int r = 0; r < 24; ++r) {
        for (int c = 0; c < 40; ++c) {
            const uint8_t ch = mem.memRead(static_cast<uint16_t>(rowBase[r] + c)) & 0x7F;
            screen += (ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : ' ';
        }
        screen += '\n';
    }
    if (!prodosSeen && screen.find("ProDOS") == std::string::npos)
        fail("ProDOS never reached the text page: blocks were served, but "
             "not the right bytes (payload order, or the odd byte before "
             "the groups)");

    if (failures) {
        std::printf("progress: probe=%d command=%d parsed=%d delivered=%d "
                    "bytes=%zu body=%zu cmd=$%02X transactions=%d read=%d\n",
                    p.probeAnswered ? 1 : 0, p.commandTaken ? 1 : 0,
                    p.packetParsed ? 1 : 0, p.replyDelivered ? 1 : 0,
                    p.commandBytes, p.bodyBytes, p.commandByte,
                    p.transactions, p.blocksRead);
        std::printf("%s", screen.c_str());
        return 1;
    }
    std::printf("smartport_bus_handshake: OK — probe answered, %d transactions, "
                "%d blocks read over the bus, ProDOS 8 on the text page\n",
                p.transactions, p.blocksRead);
    return 0;
}
