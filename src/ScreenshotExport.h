// POM2 — SPDX-License-Identifier: GPL-3.0-or-later
#ifndef POM2_SCREENSHOT_EXPORT_H
#define POM2_SCREENSHOT_EXPORT_H

#include "NtscPostProcessor.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pom2 {

struct ScreenshotMetadata {
    std::string software;
    std::string machine;
    std::string monitor;
    std::string videoStandard;
    bool crtEnabled = false;
    NtscParams crt;
};

// Native, top-down 0xAABBGGRR framebuffer. Alpha is discarded, as in the
// historical PPM capture. PNG iTXt keyword "POM2" holds versioned JSON.
// No GL, machine access or I/O; encode only AFTER releasing machine locks.
bool encodeScreenshotPng(int width, int height, const std::vector<uint32_t>& pixels,
                         const ScreenshotMetadata& metadata,
                         std::vector<uint8_t>& png, std::string& error);

// Uses the per-user screenshots directory supplied by the frontend. Scans
// both legacy .ppm and .png names, then commits through the atomic writer.
bool saveScreenshotPng(const std::filesystem::path& directory,
                       int width, int height, const std::vector<uint32_t>& pixels,
                       const ScreenshotMetadata& metadata,
                       std::filesystem::path& savedPath, std::string& error);

} // namespace pom2
#endif
