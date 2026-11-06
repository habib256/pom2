// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WozCompat — what a WOZ image says about the machine it was made for, and
// the sentence to show when that is not the machine it was put in.
//
// A WOZ carries, since version 2, the list of machines its software is known
// to run on and the RAM it needs. Nothing read them: a //e-only disk put in a
// ][+ just failed, somewhere in its own loader, with whatever that loader
// does on the wrong machine — a crash, a hang, a garbled screen — and the
// natural conclusion was that the emulator had a bug. The disk had said so
// all along.
//
// It is a HINT and is treated as one. The field records where the software is
// KNOWN to work, by whoever imaged the disk; a machine missing from it may
// run the disk perfectly. So this never refuses a mount and never changes
// what the machine does: it says one line, in the drive's panel.
//
// [B] WOZ 2.1 reference (applesaucefdc.com/woz/reference2), INFO chunk:
//     +40 uint16 "Compatible Hardware — Bit field with a 1 indicating known
//     compatibility. Multiple compatibility flags are possible. A 0 value
//     represents that the compatible hardware list is unknown." and
//     +42 uint16 "Required RAM — Minimum RAM size needed for this software.
//     This value is in K (1024 bytes). If the minimum size is unknown, this
//     value should be set to 0." Both are INFO version 2 fields.
//
// Pure: no image, no profile, no UI. `woz_compat` tests it.

#ifndef POM2_WOZ_COMPAT_H
#define POM2_WOZ_COMPAT_H

#include <cstdint>
#include <string>

namespace pom2 {
namespace woz {

/// The reference's bit values, verbatim.
enum Hardware : std::uint16_t {
    kAppleII          = 0x0001,
    kAppleIIPlus      = 0x0002,
    kAppleIIe         = 0x0004,   ///< unenhanced
    kAppleIIc         = 0x0008,
    kAppleIIeEnhanced = 0x0010,
    kAppleIIgs        = 0x0020,
    kAppleIIcPlus     = 0x0040,
    kAppleIII         = 0x0080,
    kAppleIIIPlus     = 0x0100,
    kKnownHardware    = 0x01FF,
};

/// "Apple //e Enhanced, Apple //c" — the machines `bits` names, in the
/// reference's order. Empty for 0. Bits the reference does not define are
/// ignored rather than guessed at.
inline std::string hardwareList(std::uint16_t bits)
{
    static const struct { std::uint16_t bit; const char* name; } kNames[] = {
        { kAppleII, "Apple ][" },           { kAppleIIPlus, "Apple ][ Plus" },
        { kAppleIIe, "Apple //e" },         { kAppleIIc, "Apple //c" },
        { kAppleIIeEnhanced, "Apple //e Enhanced" }, { kAppleIIgs, "Apple IIgs" },
        { kAppleIIcPlus, "Apple //c Plus" }, { kAppleIII, "Apple ///" },
        { kAppleIIIPlus, "Apple /// Plus" },
    };
    std::string out;
    for (const auto& n : kNames) {
        if (!(bits & n.bit)) continue;
        if (!out.empty()) out += ", ";
        out += n.name;
    }
    return out;
}

/// The line to show, or empty when there is nothing to say: the image names
/// no machine (0 = unknown), or names this one, and asks for no more RAM
/// than the machine has.
///
/// `machineBit`: the ONE bit of the machine the disk is in. `machineName`:
/// how the user knows it (the profile's display name). `machineRamK`: its
/// RAM in K, 0 to skip the RAM check.
inline std::string machineWarning(std::uint16_t declared, std::uint16_t requiredRamK,
                                  std::uint16_t machineBit, const std::string& machineName,
                                  unsigned machineRamK)
{
    std::string out;
    const std::uint16_t known = static_cast<std::uint16_t>(declared & kKnownHardware);
    if (known != 0 && (known & machineBit) == 0)
        out = "This disk is marked for " + hardwareList(known) + "; this machine is " +
              machineName + ".";
    if (requiredRamK != 0 && machineRamK != 0 && requiredRamK > machineRamK) {
        if (!out.empty()) out += " ";
        out += "It asks for " + std::to_string(requiredRamK) + "K of RAM; this machine has " +
               std::to_string(machineRamK) + "K.";
    }
    return out;
}

}  // namespace woz
}  // namespace pom2

#endif  // POM2_WOZ_COMPAT_H
