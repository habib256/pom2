// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Memory.h"

#include "ByteIO.h"
#include "MemoryProfile.h"
#include "MemoryProfile_IIcClass.h"
#include "SessionIO.h"
#include <algorithm>
#include <memory>
#include "SlotBus.h"

// ENV v1, little-endian:
// version(1), configuration flags(1), PAL(1), reserved(1), aux banks(4),
// canonical $D000-$FFFF (permission, ROM byte) pairs, internal I/O ROM(4096),
// character ROM length(4) + bytes, profile firmware length(4) + bytes.
// Exact bytes avoid hash collisions and any dependency on host hash libraries.
// Ordinary snapshots compare this identity. Only session reconstruction
// explicitly installs it after validating an isolated candidate machine.
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


bool Memory::installSessionEnvironment(const std::vector<uint8_t>& bytes, std::string& error)
{
    pom2::sessionio::Reader r(bytes.data(), bytes.size());
    const auto version = r.u8(), flags = r.u8(), pal = r.u8(), reserved = r.u8();
    const auto banks = r.u32();
    constexpr size_t firmwareEnd = 8 + 0x3000 * 2 + 0x1000;
    auto invalid = [&] { error = "invalid session environment"; return false; };
    if (version != 1 || (flags & ~31) || pal > 1 || reserved || !banks || banks > 128 ||
        ((flags & 2) && (!(flags & 1) || banks != 1)) || !r.need(firmwareEnd - 8)) return invalid();
    for (size_t i = 8; i < 8 + 0x6000; i += 2)
        if (bytes[i] > 1 || (bytes[i] && bytes[i + 1])) return invalid();
    r.pos = firmwareEnd;
    std::vector<uint8_t> chars, profile;
    if (!r.blob(chars, 8192) || !r.blob(profile, 16385) || !r.done() ||
        (!chars.empty() && chars.size() != 2048 && chars.size() != 4096 && chars.size() != 8192) ||
        ((flags & 8) && chars.size() != 8192) ||
        ((flags & 2) ? (profile.size() != 1 && profile.size() != 16385) : !profile.empty()) ||
        (!profile.empty() && (profile[0] > 1 || profile.size() != (profile[0] ? 16385u : 1u)))) return invalid();
    std::unique_ptr<MemoryProfile> strategy;
    if (flags & 2) {
        std::array<uint8_t, 0x4000> firmware{};
        std::copy_n(bytes.data() + 8 + 0x6000, 0x1000, firmware.data());
        for (size_t i = 0; i < 0x3000; ++i) firmware[0x1000 + i] = bytes[9 + i * 2];
        strategy = std::make_unique<IIcClassProfile>(firmware.data(), firmware.size(),
            profile[0] ? profile.data() + 1 : nullptr, iwmDevice, smartPortHub, iwmAuthoritative);
        strategy->setExternalSmartPort(externalSmartPort_);
    }
    // All wire checks and firmware allocation precede the first mutation.
    clearWriteWatches(); clearReadWatches(); setAuxShadow(false, false, 0);
    iicProfile_ = std::move(strategy);
    setIIEMode(flags & 1); setTestMode(flags & 4); setRamWorksBanks(banks);
    for (uint32_t a = 0xD000; a <= 0xFFFF; ++a) {
        const size_t i = 8 + (a - 0xD000) * 2;
        writable[a] = bytes[i] != 0;
        mem[a] = bytes[i + 1];
    }
    std::copy_n(bytes.data() + 8 + 0x6000, 0x1000, internalIORom.begin());
    characterRom = std::move(chars); charRomDualBank_ = flags & 8; charRomLowercase_ = flags & 16;
    setVideoStandard(pal ? VideoStandard::PAL : VideoStandard::NTSC);
    refreshBusFlags();
    return true;
}
