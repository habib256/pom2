// POM2 Apple II Emulator — GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

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

// Shared search order for startup and the resource-status panel.
inline std::vector<std::string> virtualIISoundDirectories(const std::string& configured)
{
    std::vector<std::string> dirs{configured, "roms/virtual_ii_sons",
        "../roms/virtual_ii_sons", "../../roms/virtual_ii_sons"};
#ifdef __APPLE__
    dirs.emplace_back("/Applications/Virtual ][.app/Contents/Resources");
    if (const char* home = std::getenv("HOME"); home && *home)
        dirs.emplace_back((std::filesystem::path(home) /
            "Applications/Virtual ][.app/Contents/Resources").string());
#endif
    return dirs;
}

inline std::vector<std::string> virtualIISoundFiles(const std::string& relative)
{
    std::vector<std::string> paths{relative};
    auto wav = std::filesystem::path(relative);
    if (wav.extension() == ".aiff") {
        wav.replace_extension(".wav");
        paths.push_back(wav.string());
    }
    // Virtual ][ stores its original recordings flat in Contents/Resources.
    auto flat = std::filesystem::path(relative).filename();
    paths.push_back(flat.string());
    if (flat.extension() == ".aiff") {
        flat.replace_extension(".wav");
        paths.push_back(flat.string());
    } else if (flat.extension() == ".wav") {
        flat.replace_extension(".raw");
        paths.push_back(flat.string());
    }
    return paths;
}

} // namespace pom2
