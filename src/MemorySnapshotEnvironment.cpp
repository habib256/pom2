// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Memory.h"

#include "ByteIO.h"
#include "MemoryProfile.h"
#include "SlotBus.h"

// ENV v1, little-endian:
// version(1), configuration flags(1), PAL(1), reserved(1), aux banks(4),
// canonical $D000-$FFFF (permission, ROM byte) pairs, internal I/O ROM(4096),
// character ROM length(4) + bytes, profile firmware length(4) + bytes.
// Exact bytes avoid hash collisions and any dependency on host hash libraries.
// Like MEM, this records firmware identity; it never flashes firmware on load.
void Memory::appendSnapshotEnvironment(std::vector<uint8_t>& out) const
{
    using namespace pom2::byteio;
    putU8(out, 1);
    putU8(out, (iieMode ? 1 : 0) | (iicProfile_ ? 2 : 0) |
               (testMode ? 4 : 0) | (charRomDualBank_ ? 8 : 0) |
               (charRomLowercase_ ? 16 : 0));
    putU8(out, videoStandard() == VideoStandard::PAL ? 1 : 0);
    putU8(out, 0);
    putU32(out, ramWorksBanks_);
    for (uint32_t addr = 0xD000; addr <= 0xFFFF; ++addr) {
        const bool ram = ramWritable(static_cast<uint16_t>(addr));
        uint8_t firmware = ram ? 0 : mem[addr];
        if (!ram) {
            // TransWarp's live shadow is mapping state, not a ROM change.
            for (int slot = 1; slot <= 7; ++slot)
                if (const auto* card = slots.peripheral(slot))
                    if (card->peekDisplacedRom(static_cast<uint16_t>(addr), firmware))
                        break;
        }
        putU8(out, ram ? 1 : 0);
        // Flat test RAM must not become part of the firmware identity.
        putU8(out, firmware);
    }
    out.insert(out.end(), internalIORom.begin(), internalIORom.end());
    putU32(out, static_cast<uint32_t>(characterRom.size()));
    out.insert(out.end(), characterRom.begin(), characterRom.end());
    std::vector<uint8_t> profileFirmware;
    if (iicProfile_) iicProfile_->appendSnapshotFirmware(profileFirmware);
    putU32(out, static_cast<uint32_t>(profileFirmware.size()));
    out.insert(out.end(), profileFirmware.begin(), profileFirmware.end());
}
