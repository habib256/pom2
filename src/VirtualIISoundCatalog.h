// POM2 Apple II Emulator — GPL-3.0-or-later
#pragma once

#include <array>

namespace pom2 {

struct VirtualIISoundAsset {
    const char* path;
    bool loop;
    bool required;
};

// Order matches FloppySoundDevice's Virtual II sample slots.
inline constexpr std::array<VirtualIISoundAsset, 8> kVirtualIISoundAssets{{
    {"lecteur/Disk Rotation.wav",   true,  true},
    {"lecteur/Boot.wav",            false, false},
    {"lecteur/Move arm.aiff",       false, true},
    {"lecteur/Disk Insertion.aiff", false, true},
    {"lecteur/Disk Removal.aiff",   false, true},
    {"lecteur/I-O Error.wav",       false, false},
    {"interface/PopOn2.aiff",       false, false},
    {"interface/PopOff2.aiff",      false, false},
}};

inline constexpr const char* kVirtualIIPrinterSound =
    "imprimante/Matrix Printer.wav";

} // namespace pom2
