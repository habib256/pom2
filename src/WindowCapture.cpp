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

#include "WindowCapture.h"

#ifndef __EMSCRIPTEN__

#include "AtomicFileReplace.h"
#include "Logger.h"
#include "ScreenshotExport.h"

#include "Pom2GL.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace pom2 {

void captureWindowIfRequested(int w, int h)
{
    static const std::string target = [] {
        const char* v = std::getenv("POM2_WINDOW_SHOT");
        return std::string(v ? v : "");
    }();
    if (target.empty() || w <= 0 || h <= 0) return;
    const std::filesystem::path request = target + ".request";
    std::error_code ec;
    if (!std::filesystem::exists(request, ec)) return;

    std::vector<uint32_t> pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    // OpenGL's rows run bottom-up; a picture's run top-down.
    std::vector<uint32_t> row(static_cast<std::size_t>(w));
    for (int y = 0; y < h / 2; ++y) {
        uint32_t* a = pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        uint32_t* b = pixels.data() + static_cast<std::size_t>(h - 1 - y) * static_cast<std::size_t>(w);
        std::memcpy(row.data(), a, row.size() * sizeof(uint32_t));
        std::memcpy(a, b, row.size() * sizeof(uint32_t));
        std::memcpy(b, row.data(), row.size() * sizeof(uint32_t));
    }
    std::vector<uint8_t> png;
    std::string error;
    ScreenshotMetadata metadata;
    metadata.software = "POM2 window capture";
    if (!encodeScreenshotPng(w, h, pixels, metadata, png, error))
        pom2::log().warn("WindowCapture", "encode failed: " + error);
    else if (!writeFileAtomic(target, png.data(), png.size(), ec))
        pom2::log().warn("WindowCapture", "write failed: " + ec.message());
    // Removed even on failure: a request that stays would retry every frame.
    std::filesystem::remove(request, ec);
}

} // namespace pom2

#else

namespace pom2 {
void captureWindowIfRequested(int, int) {}
} // namespace pom2

#endif
